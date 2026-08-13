#include "kds/wal/recovery.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <memory>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kds/server/superblock.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/stream.hpp"

// The recovery driver (docs/workplan-wal-recovery.md §2's unowned entry
// point, RV1/RV2).
//
// The property that carries this file is the **refusal**: a stream with a
// loser and no undo phase must not recover. Redo restores uncommitted
// writes on purpose, and txn.md §8's gap makes a surviving uncommitted row
// read as committed - so a driver that replayed and stopped would publish
// every loser's writes, which is strictly worse than not recovering. The
// test asserts the refusal happens **before redo has written anything**,
// because a refusal after the fact has already done the damage it refuses.

namespace kds::wal {
namespace {

constexpr std::uint64_t kSegmentSize = 16 * 1024;
constexpr PageId kPage = server::kFirstUserPageId;

std::vector<std::byte> Bytes(std::size_t n, unsigned char fill) {
    return std::vector<std::byte>(n, static_cast<std::byte>(fill));
}

// An undo phase that only records that it was asked. RC05 supplies the real
// one; what the driver owes is to call it exactly when there are losers.
class RecordingUndo final : public UndoPhase {
public:
    Status RollBack(storage::PageStore&, const AnalysisResult& analysis) override {
        ++calls;
        losers_seen = analysis.losers;
        return Status::OK();
    }
    int calls = 0;
    std::uint64_t losers_seen = 0;
};

class FailingUndo final : public UndoPhase {
public:
    Status RollBack(storage::PageStore&, const AnalysisResult&) override {
        return Status::Corruption("undo said no");
    }
};

class RecoveryTest : public ::testing::Test {
protected:
    std::unique_ptr<MemoryLogDevice> device_ =
        std::move(MemoryLogDevice::Create(kSegmentSize).value());
    storage::InMemoryPageStore store_{server::kFirstUserPageId};

    // PAGE_INIT + one insert under `txn_id`, then `terminal` if it is not
    // kPad (used as "append nothing", so the transaction stays a loser).
    void WriteStream(std::uint64_t txn_id, RecordType terminal) {
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok()) << s.status().message();

        std::vector<std::byte> init(kPageInitPayloadSize, std::byte{0});
        const PageInitPayload fields{/*min_key=*/1, static_cast<std::uint8_t>(PageType::kHeap),
                                     {0, 0, 0}, /*reserved2=*/0, /*owner_oid=*/0};
        ASSERT_TRUE(EncodePageInit(init, fields).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kPageInit, txn_id, kPage}, init).ok());

        const auto payload = Bytes(24, 0xC1);
        std::vector<std::byte> buf(kHeapWriteFixedSize + payload.size(), std::byte{0});
        const HeapWritePayload hw{txn_id, /*undo_ptr=*/0, /*slot=*/0,
                                  static_cast<std::uint16_t>(payload.size())};
        auto n = EncodeHeapWrite(buf, hw, payload);
        ASSERT_TRUE(n.ok()) << n.status().message();
        ASSERT_TRUE(s.value()
                        ->Append({RecordType::kHeapInsert, txn_id, kPage},
                                 std::span(buf).first(n.value()))
                        .ok());

        if (terminal != RecordType::kPad) {
            ASSERT_TRUE(s.value()->Append({terminal, txn_id, kInvalidPageId}).ok());
        }
        ASSERT_TRUE(s.value()->Sync().ok());
    }
};

// ---- The refusal ---------------------------------------------------------

TEST_F(RecoveryTest, ALoserWithNoUndoPhaseRefusesTheMountBeforeRedoWrites) {
    WriteStream(/*txn_id=*/7, RecordType::kPad);  // no terminal record

    auto r = RecoverCore((*device_), 0, store_, AnalysisStart{}, /*undo=*/nullptr);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kUnsupported) << r.status().message();

    // The point of the test: nothing was written. A refusal taken after redo
    // would have left the loser's row on the page, which txn.md §8's gap
    // then reads as committed.
    EXPECT_EQ(store_.page_count(), 0u) << "redo ran before the refusal";
}

TEST_F(RecoveryTest, AWinnerRecoversWithNoUndoPhaseAtAll) {
    WriteStream(/*txn_id=*/7, RecordType::kTxnCommit);

    auto r = RecoverCore((*device_), 0, store_, AnalysisStart{}, /*undo=*/nullptr);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().analysis.winners, 1u);
    EXPECT_EQ(r.value().analysis.losers, 0u);
    EXPECT_FALSE(r.value().undo_ran) << "nothing to undo, so nothing should have been called";
    EXPECT_EQ(r.value().redo.pages_created, 1u);

    auto page = store_.Get(kPage);
    ASSERT_TRUE(page.ok());
    heap::PageView view(page.value().bytes());
    EXPECT_EQ(view.slot_count(), 1u);
}

TEST_F(RecoveryTest, ADurableAbortIsNotALoserAndNeedsNoUndoPhase) {
    // txn.md §6 emits compensations as ordinary logged mutations before
    // TXN_ABORT, so redo has already replayed them. Undo owes it nothing,
    // and the driver must not refuse for it.
    WriteStream(/*txn_id=*/7, RecordType::kTxnAbort);

    auto r = RecoverCore((*device_), 0, store_, AnalysisStart{}, /*undo=*/nullptr);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().analysis.aborted, 1u);
    EXPECT_EQ(r.value().analysis.losers, 0u);
    EXPECT_FALSE(r.value().undo_ran);
}

// ---- The phase is called exactly when it is owed -------------------------

TEST_F(RecoveryTest, AnInstalledUndoPhaseIsCalledForALoser) {
    WriteStream(/*txn_id=*/7, RecordType::kPad);
    RecordingUndo undo;

    auto r = RecoverCore((*device_), 0, store_, AnalysisStart{}, &undo);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(undo.calls, 1);
    EXPECT_EQ(undo.losers_seen, 1u);
    EXPECT_TRUE(r.value().undo_ran);
    // And redo ran first: undo rolls back from crash-time state.
    EXPECT_EQ(r.value().redo.pages_created, 1u);
}

TEST_F(RecoveryTest, AnInstalledUndoPhaseIsNotCalledWithoutLosers) {
    WriteStream(/*txn_id=*/7, RecordType::kTxnCommit);
    RecordingUndo undo;

    auto r = RecoverCore((*device_), 0, store_, AnalysisStart{}, &undo);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(undo.calls, 0) << "a winner-only stream owes undo nothing";
    EXPECT_FALSE(r.value().undo_ran);
}

TEST_F(RecoveryTest, AFailingUndoPhaseRefusesTheMount) {
    WriteStream(/*txn_id=*/7, RecordType::kPad);
    FailingUndo undo;

    auto r = RecoverCore((*device_), 0, store_, AnalysisStart{}, &undo);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kCorruption) << r.status().message();
}

// ---- Ordering and the report ---------------------------------------------

TEST_F(RecoveryTest, TheHighWaterFloorIsRaisedBeforeUndoCouldWrite) {
    // RV4's rule is "before the store serves an allocation", and undo
    // writes - so an undo phase that allocates must already be above the
    // floor when it runs.
    class AllocatingUndo final : public UndoPhase {
    public:
        Status RollBack(storage::PageStore& store, const AnalysisResult&) override {
            auto created = store.CreateNew();
            if (!created.ok()) return created.status();
            first_id = created.value().first;
            return Status::OK();
        }
        PageId first_id = kInvalidPageId;
    };

    WriteStream(/*txn_id=*/7, RecordType::kPad);
    AllocatingUndo undo;

    auto r = RecoverCore((*device_), 0, store_, AnalysisStart{}, &undo);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_GT(undo.first_id, r.value().analysis.max_page_id)
        << "undo allocated a page the log names";
    EXPECT_EQ(r.value().high_water.page_floor, kPage + 1);
}

TEST_F(RecoveryTest, TheReportCarriesTheTrxCeilingItsCallerOwes) {
    WriteStream(/*txn_id=*/9000, RecordType::kTxnCommit);

    auto r = RecoverCore((*device_), 0, store_, AnalysisStart{}, /*undo=*/nullptr);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().high_water.next_trx_id, 9001u);

    // Applied by the caller, because the superblock is server/'s.
    server::SuperBlock sb = server::SuperBlock::CreateFresh(/*now_unix_seconds=*/1);
    ASSERT_TRUE(sb.SetNextTrxId(r.value().high_water.next_trx_id).ok());
    EXPECT_EQ(sb.next_trx_id(), 9001u);
}

TEST_F(RecoveryTest, RunningTwiceIsANoOp) {
    // A crash during recovery re-runs it, so the whole driver has to be as
    // idempotent as redo is.
    WriteStream(/*txn_id=*/7, RecordType::kTxnCommit);

    auto first = RecoverCore((*device_), 0, store_, AnalysisStart{}, /*undo=*/nullptr);
    ASSERT_TRUE(first.ok()) << first.status().message();
    auto page = store_.Get(kPage);
    ASSERT_TRUE(page.ok());
    const std::vector<std::byte> after_first(page.value().bytes().begin(), page.value().bytes().end());

    auto second = RecoverCore((*device_), 0, store_, AnalysisStart{}, /*undo=*/nullptr);
    ASSERT_TRUE(second.ok()) << second.status().message();
    EXPECT_EQ(second.value().redo.applied, 0u);
    EXPECT_EQ(second.value().redo.skipped_by_lsn, 2u);

    auto again = store_.Get(kPage);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(std::vector<std::byte>(again.value().bytes().begin(), again.value().bytes().end()), after_first);
}

// ---- An anchor that outruns its stream still refuses ---------------------

TEST_F(RecoveryTest, AnAnchorPastTheDurableEndRefusesTheMount) {
    WriteStream(/*txn_id=*/7, RecordType::kTxnCommit);

    auto r = RecoverCore((*device_), 0, store_,
                         AnalysisStart{/*redo_start_lsn=*/0, /*anchor_durable_lsn=*/1u << 30},
                         /*undo=*/nullptr);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kCorruption) << r.status().message();
    EXPECT_EQ(store_.page_count(), 0u) << "a failed analysis must not have written";
}

}  // namespace
}  // namespace kds::wal
