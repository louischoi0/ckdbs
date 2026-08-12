#include "kds/server/mount_recovery.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/stream.hpp"

// Recovery at mount (docs/workplan-wal-recovery.md RV1/RV2) - the seam that
// turned `wal::RecoverCore` from a function only tests called into the thing
// a mount runs.
//
// `wal_recovery_test.cpp` already pins the driver's own behaviour: the
// refusal without an undo phase, the phase called exactly when owed, the
// ordering of the high-water repair. **This file pins what the seam adds**,
// and each of these would pass every driver test while being wrong:
//
//   - the anchor's two fields reaching analysis. A seam that read the
//     anchor and dropped `durable_lsn` would recover happily from a stream
//     that lost the records its anchor depends on - `analysis.hpp` calls
//     that recovery's quietest failure mode.
//   - the undo phase being installed *always*, not on request. The driver
//     refuses a stream with losers and no phase; a mount that passed null
//     would be choosing that refusal over recovering, on every crash.
//   - the two caller obligations coming back as numbers - the page floor
//     the extent allocator is seeded from, and the transaction ceiling the
//     superblock owes - because a report that dropped either would leave the
//     hazard RC04 exists to close wide open, silently.

namespace kds::server {
namespace {

constexpr std::uint64_t kSegmentSize = 16 * 1024;
constexpr PageId kPage = kFirstUserPageId;

std::vector<std::byte> Bytes(std::size_t n, unsigned char fill) {
    return std::vector<std::byte>(n, static_cast<std::byte>(fill));
}

class MountRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto device = wal::MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(device.ok()) << device.status().message();
        device_ = std::move(device.value());
        undo_log_.emplace(store_);
    }

    // PAGE_INIT + one heap insert under `txn_id`, then `terminal` unless it
    // is kPad - which stands for "append nothing", leaving the transaction a
    // loser. The same shape `wal_recovery_test.cpp` seeds with, so a
    // difference between the two files is a difference in the seam.
    void WriteStream(std::uint64_t txn_id, wal::RecordType terminal) {
        auto s = wal::WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok()) << s.status().message();

        std::vector<std::byte> init(wal::kPageInitPayloadSize, std::byte{0});
        const wal::PageInitPayload fields{
            /*min_key=*/1, static_cast<std::uint8_t>(PageType::kHeap), {0, 0, 0}};
        ASSERT_TRUE(wal::EncodePageInit(init, fields).ok());
        auto init_lsn = s.value()->Append({wal::RecordType::kPageInit, txn_id, kPage}, init);
        ASSERT_TRUE(init_lsn.ok()) << init_lsn.status().message();

        const auto payload = Bytes(24, 0xC1);
        std::vector<std::byte> buf(wal::kHeapWriteFixedSize + payload.size(), std::byte{0});
        const wal::HeapWritePayload hw{txn_id, /*undo_ptr=*/0, /*slot=*/0,
                                       static_cast<std::uint16_t>(payload.size())};
        auto n = wal::EncodeHeapWrite(buf, hw, payload);
        ASSERT_TRUE(n.ok()) << n.status().message();
        auto insert_lsn = s.value()->Append({wal::RecordType::kHeapInsert, txn_id, kPage},
                                            std::span(buf).first(n.value()));
        ASSERT_TRUE(insert_lsn.ok()) << insert_lsn.status().message();
        insert_lsn_ = insert_lsn.value();

        if (terminal != wal::RecordType::kPad) {
            ASSERT_TRUE(s.value()->Append({terminal, txn_id, kInvalidPageId}).ok());
        }
        ASSERT_TRUE(s.value()->Sync().ok());
    }

    // A stream that exists and holds nothing - a database opened and never
    // written to, which is the mount recovery must not charge for.
    void WriteEmptyStream() {
        auto s = wal::WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok()) << s.status().message();
        ASSERT_TRUE(s.value()->Sync().ok());
    }

    StatusOr<MountRecovery> Recover(const WalAnchorFields& anchor) {
        return RecoverCoreAtMount(/*core_id=*/0, anchor, *device_, store_, *undo_log_,
                                  /*wal=*/nullptr, /*log=*/nullptr);
    }

    std::unique_ptr<wal::MemoryLogDevice> device_;
    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<txn::UndoLog> undo_log_;
    wal::Lsn insert_lsn_ = 0;
};

// ---- The anchor's fields reach analysis -----------------------------------

TEST_F(MountRecoveryTest, AZeroedAnchorScansFromTheHeadOfTheStream) {
    // Every database this engine has written before RC08 has a zeroed slot,
    // so this is not an edge case - it is the only case today.
    WriteStream(/*txn_id=*/7, wal::RecordType::kTxnCommit);

    auto r = Recover(WalAnchorFields{});
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_FALSE(r.value().empty());
    EXPECT_EQ(r.value().records, 3u);  // PAGE_INIT, insert, commit
    EXPECT_EQ(r.value().winners, 1u);
    EXPECT_EQ(r.value().losers, 0u);
    EXPECT_GT(r.value().redo_applied, 0u);

    auto page = store_.Get(kPage);
    ASSERT_TRUE(page.ok()) << page.status().message();
    heap::PageView view(page.value());
    EXPECT_EQ(view.slot_count(), 1u);
}

TEST_F(MountRecoveryTest, TheAnchorsRedoStartNarrowsTheScan) {
    // Proves `redo_start_lsn` travels: starting at the insert's own LSN, the
    // PAGE_INIT below it is never scanned. A seam that dropped the field
    // would read 3 records here and pass every other test in this file.
    //
    // The full recovery runs first, because that is what an anchor *means*:
    // a published redo start says the pages below it were flushed, so the
    // narrowed scan finds its page already on the store. Starting a scan
    // above a PAGE_INIT whose page was never written is not a narrower
    // recovery, it is a broken one - and redo says so, `page id not found`.
    WriteStream(/*txn_id=*/7, wal::RecordType::kTxnCommit);
    auto full = Recover(WalAnchorFields{});
    ASSERT_TRUE(full.ok()) << full.status().message();
    ASSERT_EQ(full.value().records, 3u);

    WalAnchorFields anchor{};
    anchor.redo_start_lsn = insert_lsn_;
    auto r = Recover(anchor);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().records, 2u) << "the scan did not start at the anchor";
    EXPECT_EQ(r.value().redo_skipped_by_lsn, 1u) << "the insert alone, its PAGE_INIT unscanned";
}

TEST_F(MountRecoveryTest, AnAnchorPastTheDurableEndRefusesTheMount) {
    // Proves `durable_lsn` travels, and that a refusal is propagated rather
    // than logged and mounted anyway. `analysis.hpp`: a log that lost the
    // records its anchor depends on scans to zero records, byte-identical to
    // a clean shutdown - so dropping this field turns recovery's quietest
    // failure into a silent empty replay.
    WriteStream(/*txn_id=*/7, wal::RecordType::kTxnCommit);

    WalAnchorFields anchor{};
    anchor.durable_lsn = 1u << 30;
    auto r = Recover(anchor);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kCorruption) << r.status().message();
    EXPECT_EQ(store_.page_count(), 0u) << "a refused mount must not have written";
}

// ---- The undo phase is installed, always ---------------------------------

TEST_F(MountRecoveryTest, ALoserRecoversInsteadOfRefusingTheMount) {
    // The seam's whole reason for existing. `RecoverCore` with a null phase
    // refuses this stream (wal_recovery_test.cpp) - correctly, because
    // publishing a loser's writes is worse than not recovering. A mount must
    // therefore never pass null, and this is that assertion.
    WriteStream(/*txn_id=*/7, wal::RecordType::kPad);

    auto r = Recover(WalAnchorFields{});
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().losers, 1u);
    EXPECT_EQ(r.value().transactions_rolled_back, 1u);
    // No compensation: this loser's insert predates RV10's undo record in
    // the seeded stream, so its chain head is 0 and all it is owed is the
    // TXN_ABORT that stops the next recovery calling it a loser
    // (recovery_undo.cpp). What a real chain does to a real row is
    // recovery_undo_test.cpp's ten tests, not this seam's.
    EXPECT_EQ(r.value().compensations, 0u);
}

TEST_F(MountRecoveryTest, ADurableAbortOwesUndoNothing) {
    WriteStream(/*txn_id=*/7, wal::RecordType::kTxnAbort);

    auto r = Recover(WalAnchorFields{});
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().aborted, 1u);
    EXPECT_EQ(r.value().losers, 0u);
    EXPECT_EQ(r.value().transactions_rolled_back, 0u);
}

// ---- The caller's two obligations come back as numbers -------------------

TEST_F(MountRecoveryTest, ThePageFloorAndTrxCeilingAreReported) {
    WriteStream(/*txn_id=*/9000, wal::RecordType::kTxnCommit);

    auto r = Recover(WalAnchorFields{});
    ASSERT_TRUE(r.ok()) << r.status().message();

    // RC04's obligation 1: the extent allocator's search starts here, so an
    // extent cannot cover a page redo just wrote.
    EXPECT_TRUE(r.value().page_floor_raised);
    EXPECT_EQ(r.value().page_floor, kPage + 1);

    // And the ceiling the superblock owes - reported, not applied, because a
    // peer's superblock is a copy it may not write (M5).
    EXPECT_EQ(r.value().next_trx_id, 9001u);
    SuperBlock sb = SuperBlock::CreateFresh(/*now_unix_seconds=*/1);
    ASSERT_TRUE(sb.SetNextTrxId(r.value().next_trx_id).ok());
    EXPECT_EQ(sb.next_trx_id(), 9001u);
}

TEST_F(MountRecoveryTest, AnUnwrittenStreamCostsNothingAndSaysSo) {
    WriteEmptyStream();

    auto r = Recover(WalAnchorFields{});
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_TRUE(r.value().empty());
    EXPECT_EQ(r.value().records, 0u);
    EXPECT_EQ(r.value().redo_applied, 0u);
    EXPECT_FALSE(r.value().page_floor_raised)
        << "a stream naming no page must not move the allocation floor";
    EXPECT_EQ(r.value().next_trx_id, 0u);
    EXPECT_EQ(store_.page_count(), 0u);
}

TEST_F(MountRecoveryTest, RecoveringTwiceIsANoOp) {
    // A crash during a mount re-runs the whole thing, so the seam has to be
    // as idempotent as the driver under it.
    WriteStream(/*txn_id=*/7, wal::RecordType::kTxnCommit);

    ASSERT_TRUE(Recover(WalAnchorFields{}).ok());
    auto page = store_.Get(kPage);
    ASSERT_TRUE(page.ok());
    const std::vector<std::byte> after_first(page.value().begin(), page.value().end());

    auto second = Recover(WalAnchorFields{});
    ASSERT_TRUE(second.ok()) << second.status().message();
    EXPECT_EQ(second.value().redo_applied, 0u);
    EXPECT_EQ(second.value().redo_skipped_by_lsn, 2u);

    auto again = store_.Get(kPage);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(std::vector<std::byte>(again.value().begin(), again.value().end()), after_first);
}

}  // namespace
}  // namespace kds::server
