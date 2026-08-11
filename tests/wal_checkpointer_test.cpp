#include "kds/wal/checkpointer.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/page_mgr/checkpoint_target.hpp"
#include "kds/storage/page_mgr/page_mgr.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/record.hpp"

// A checkpoint's product is one number - the redo start - and one promise:
// that number is never published over pages that are not on disk (wal.md
// sections 11 and 8-3).

namespace kds::wal {
namespace {

constexpr std::uint64_t kSegmentSize = 1024 * 1024;

class ScriptedTxns final : public ActiveTransactions {
public:
    std::vector<CheckpointActiveTxn> Snapshot() const override { return live_; }
    // Ids alone, with a kNoUndoPtr head each - these tests are about the
    // checkpoint's ordering and its tables' shape, not about undo chains.
    void SetLive(const std::vector<std::uint64_t>& ids) {
        live_.clear();
        live_.reserve(ids.size());
        for (std::uint64_t id : ids) live_.push_back({id, 0});
    }
    void SetLive(std::vector<CheckpointActiveTxn> live) { live_ = std::move(live); }

private:
    std::vector<CheckpointActiveTxn> live_;
};

// A checkpoint target that records what it was asked to flush and can
// refuse - the scripted half of the crash matrix.
class ScriptedTarget final : public CheckpointTarget {
public:
    std::vector<CheckpointDirtyPage> DirtyTable() const override { return dirty_; }

    Status FlushPages(std::span<const PageId> page_ids) override {
        if (fail_next_) {
            fail_next_ = false;
            return Status::IoError("injected: flush failed");
        }
        ++batches_;
        for (PageId id : page_ids) {
            flushed_.push_back(id);
            std::erase_if(dirty_, [id](const CheckpointDirtyPage& p) { return p.page_id == id; });
        }
        return Status::OK();
    }

    void SetDirty(std::vector<CheckpointDirtyPage> dirty) { dirty_ = std::move(dirty); }
    void FailNextFlush() noexcept { fail_next_ = true; }
    const std::vector<PageId>& flushed() const noexcept { return flushed_; }
    std::uint64_t batches() const noexcept { return batches_; }

private:
    std::vector<CheckpointDirtyPage> dirty_;
    std::vector<PageId> flushed_;
    std::uint64_t batches_ = 0;
    bool fail_next_ = false;
};

class CheckpointerTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto device = MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(device.ok()) << device.status().message();
        device_ = std::move(device.value());

        auto wal = WalManager::Open(device_.get(), clock_, 3);
        ASSERT_TRUE(wal.ok()) << wal.status().message();
        wal_ = std::move(wal.value());
    }

    std::unique_ptr<Checkpointer> MakeCheckpointer(CheckpointerConfig config = {}) {
        return std::make_unique<Checkpointer>(*wal_, target_, txns_, anchor_, config);
    }

    // Every record on the device, so a test can assert on what the log
    // actually says rather than on what the checkpointer claims.
    std::vector<DecodedRecord> DurableRecords() {
        body_.assign(kSegmentSize - kSegmentHeaderSize, std::byte{0});
        EXPECT_TRUE(device_->ReadAt(0, kSegmentHeaderSize, body_).ok());
        RecordReader reader(body_, kSegmentHeaderSize);
        std::vector<DecodedRecord> records;
        while (std::optional<DecodedRecord> record = reader.Next()) {
            if (record->type() == RecordType::kPad) {
                break;
            }
            records.push_back(*record);
        }
        return records;
    }

    sched::ManualClock clock_;
    std::unique_ptr<MemoryLogDevice> device_;
    std::unique_ptr<WalManager> wal_;
    ScriptedTxns txns_;
    ScriptedTarget target_;
    InMemoryCheckpointAnchor anchor_;
    std::vector<std::byte> body_;
};

// ---- The two tables ------------------------------------------------------

TEST_F(CheckpointerTest, BeginCarriesTheTablesAnalysisStartsFrom) {
    txns_.SetLive({11, 22, 33});
    target_.SetDirty({{7, 400}, {9, 500}});
    auto checkpointer = MakeCheckpointer();

    ASSERT_TRUE(checkpointer->RunToCompletion().ok());

    auto records = DurableRecords();
    ASSERT_FALSE(records.empty());
    EXPECT_EQ(records.front().type(), RecordType::kCheckpointBegin);
    auto begin = DecodeCheckpointBegin(records.front().payload);
    ASSERT_TRUE(begin.ok()) << begin.status().message();
    ASSERT_EQ(begin.value().active_txns.size(), 3u);
    EXPECT_EQ(begin.value().active_txns[0].txn_id, 11u);
    EXPECT_EQ(begin.value().active_txns[1].txn_id, 22u);
    EXPECT_EQ(begin.value().active_txns[2].txn_id, 33u);
    ASSERT_EQ(begin.value().dirty_pages.size(), 2u);
    EXPECT_EQ(begin.value().dirty_pages[0].page_id, 7u);
    EXPECT_EQ(begin.value().dirty_pages[0].rec_lsn, 400u);
}

TEST_F(CheckpointerTest, AnEmptyCheckpointStillMovesTheRedoStartForward) {
    auto checkpointer = MakeCheckpointer();
    ASSERT_TRUE(checkpointer->RunToCompletion().ok());

    // Nothing dirty means nothing before this checkpoint has to be
    // replayed - the whole point of running one on an idle core.
    EXPECT_EQ(checkpointer->redo_start_lsn(), checkpointer->last_checkpoint_lsn());
    EXPECT_EQ(anchor_.anchor().redo_start_lsn, checkpointer->redo_start_lsn());
    EXPECT_EQ(anchor_.anchor().core_id, 3u);
}

// ---- Redo start ----------------------------------------------------------

TEST_F(CheckpointerTest, TheRedoStartIsTheOldestPageStillNeedingReplay) {
    target_.SetDirty({{7, 900}, {9, 400}, {11, 650}});
    auto checkpointer = MakeCheckpointer();
    ASSERT_TRUE(checkpointer->RunToCompletion().ok());

    EXPECT_EQ(checkpointer->redo_start_lsn(), 400u);

    auto records = DurableRecords();
    ASSERT_EQ(records.back().type(), RecordType::kCheckpointEnd);
    auto end = DecodeCheckpointEnd(records.back().payload);
    ASSERT_TRUE(end.ok()) << end.status().message();
    // Logged as well as anchored, so a stream is self-describing without
    // a superblock (wal.md section 11-3).
    EXPECT_EQ(end.value().redo_start_lsn, 400u);
}

TEST_F(CheckpointerTest, PagesWithNothingToReplayDoNotPinTheRedoStart) {
    // recLSN 0 = dirty but never logged (a created page). Treating that as
    // an LSN would pin the redo start to the head of the log forever and
    // quietly destroy the RTO bound the checkpoint exists to provide.
    target_.SetDirty({{7, 0}, {9, 0}});
    auto checkpointer = MakeCheckpointer();
    ASSERT_TRUE(checkpointer->RunToCompletion().ok());

    EXPECT_EQ(checkpointer->redo_start_lsn(), checkpointer->last_checkpoint_lsn());
    EXPECT_NE(checkpointer->redo_start_lsn(), 0u);
    // They are still flushed - unreplayable is not the same as unneeded.
    EXPECT_EQ(target_.flushed().size(), 2u);
}

TEST_F(CheckpointerTest, ANeverLoggedPageDoesNotHideAnOlderOne) {
    target_.SetDirty({{7, 0}, {9, 250}});
    auto checkpointer = MakeCheckpointer();
    ASSERT_TRUE(checkpointer->RunToCompletion().ok());
    EXPECT_EQ(checkpointer->redo_start_lsn(), 250u);
}

// ---- Checkpoint honesty (wal.md 8-3) -------------------------------------

TEST_F(CheckpointerTest, EndIsNotWrittenWhilePagesAreStillUnflushed) {
    target_.SetDirty({{1, 100}, {2, 200}, {3, 300}});
    CheckpointerConfig config;
    config.pages_per_step = 1;
    auto checkpointer = MakeCheckpointer(config);

    ASSERT_TRUE(checkpointer->Start().ok());
    auto done = checkpointer->Step();
    ASSERT_TRUE(done.ok());
    EXPECT_FALSE(done.value());

    // An anchor published now would point past pages that are not on
    // disk, and recovery would never look before it.
    EXPECT_EQ(checkpointer->Complete().code(), StatusCode::kOutOfRange);
    EXPECT_EQ(anchor_.publishes(), 0u);
    for (const auto& record : DurableRecords()) {
        EXPECT_NE(record.type(), RecordType::kCheckpointEnd);
    }
}

TEST_F(CheckpointerTest, TheAnchorIsPublishedOnlyAfterEndIsDurable) {
    target_.SetDirty({{1, 100}});
    auto checkpointer = MakeCheckpointer();
    ASSERT_TRUE(checkpointer->RunToCompletion().ok());

    EXPECT_EQ(anchor_.publishes(), 1u);
    auto records = DurableRecords();
    ASSERT_FALSE(records.empty());
    EXPECT_EQ(records.back().type(), RecordType::kCheckpointEnd);
    // Durable, not merely appended: the anchor names a record a crash
    // must not be able to erase.
    EXPECT_TRUE(wal_->IsDurable(records.back().header.lsn));
    EXPECT_EQ(anchor_.anchor().durable_lsn, wal_->durable_lsn());
}

TEST_F(CheckpointerTest, AFailedFlushLeavesTheCheckpointRunningAndRetryable) {
    target_.SetDirty({{1, 100}, {2, 200}});
    auto checkpointer = MakeCheckpointer();

    ASSERT_TRUE(checkpointer->Start().ok());
    target_.FailNextFlush();
    EXPECT_EQ(checkpointer->Step().status().code(), StatusCode::kIoError);
    EXPECT_TRUE(checkpointer->in_progress());
    EXPECT_EQ(checkpointer->Complete().code(), StatusCode::kOutOfRange);

    // The cursor did not move, so the retry covers the same pages.
    auto done = checkpointer->Step();
    ASSERT_TRUE(done.ok());
    EXPECT_TRUE(done.value());
    ASSERT_TRUE(checkpointer->Complete().ok());
    EXPECT_EQ(target_.flushed().size(), 2u);
}

TEST_F(CheckpointerTest, CheckpointsDoNotNest) {
    auto checkpointer = MakeCheckpointer();
    ASSERT_TRUE(checkpointer->Start().ok());
    // A second one would publish an anchor built from the first's
    // half-flushed snapshot.
    EXPECT_EQ(checkpointer->Start().code(), StatusCode::kAlreadyExists);
}

TEST_F(CheckpointerTest, StepAndCompleteRefuseWhenNothingIsRunning) {
    auto checkpointer = MakeCheckpointer();
    EXPECT_EQ(checkpointer->Step().status().code(), StatusCode::kOutOfRange);
    EXPECT_EQ(checkpointer->Complete().code(), StatusCode::kOutOfRange);
}

// ---- Pacing --------------------------------------------------------------

TEST_F(CheckpointerTest, FlushingIsPacedAcrossSteps) {
    std::vector<CheckpointDirtyPage> dirty;
    for (PageId id = 1; id <= 10; ++id) {
        dirty.push_back({id, 100 + id});
    }
    target_.SetDirty(dirty);

    CheckpointerConfig config;
    config.pages_per_step = 3;
    auto checkpointer = MakeCheckpointer(config);
    ASSERT_TRUE(checkpointer->Start().ok());

    // A checkpoint spreads across reactor iterations instead of holding
    // one for its whole duration (wal.md section 11-2).
    int steps = 0;
    while (true) {
        auto done = checkpointer->Step();
        ASSERT_TRUE(done.ok());
        ++steps;
        if (done.value()) {
            break;
        }
        ASSERT_LT(steps, 10);
    }
    EXPECT_EQ(steps, 4);  // 3 + 3 + 3 + 1
    EXPECT_EQ(target_.batches(), 4u);
    ASSERT_TRUE(checkpointer->Complete().ok());
    EXPECT_EQ(checkpointer->stats().pages_flushed, 10u);
}

TEST_F(CheckpointerTest, PagesDirtiedDuringTheCheckpointAreNotItsProblem) {
    target_.SetDirty({{1, 100}});
    CheckpointerConfig config;
    config.pages_per_step = 1;
    auto checkpointer = MakeCheckpointer(config);

    ASSERT_TRUE(checkpointer->Start().ok());
    // Fuzzy: work keeps happening. This page is not in the snapshot, so
    // the checkpoint neither waits for it nor claims it - and its recLSN
    // is above the BEGIN LSN, so the published redo start still covers it.
    target_.SetDirty({{1, 100}, {2, 999999}});

    ASSERT_TRUE(checkpointer->Step().ok());
    ASSERT_TRUE(checkpointer->Complete().ok());
    EXPECT_EQ(target_.flushed(), (std::vector<PageId>{1}));
    EXPECT_LT(checkpointer->redo_start_lsn(), 999999u);
}

// ---- Over a real buffer pool ---------------------------------------------

TEST_F(CheckpointerTest, OverARealPoolTheFlushedPagesGoOutUnderTheWalGate) {
    storage::InMemoryPageStore backing;
    storage::BufferPool pool(backing, 8);
    pool.SetWalDurability(wal_.get());
    storage::BufferPoolCheckpointTarget target(pool);

    // Two pages, each mutated by a real record, in the order the engine
    // does it: append the record, then mutate the page under its LSN.
    for (PageId page_id : {PageId{1}, PageId{2}}) {
        auto frame = pool.AllocNew(page_id);
        ASSERT_TRUE(frame.ok()) << frame.status().message();
        storage::FormatPage(frame.value()->bytes(), PageType::kHeap);
        auto lsn = wal_->Append({RecordType::kHeapInsert, 5, page_id, 0});
        ASSERT_TRUE(lsn.ok()) << lsn.status().message();
        frame.value()->MarkDirty(lsn.value());
        pool.Unpin(*frame.value());
    }
    ASSERT_EQ(pool.DirtyTable().size(), 2u);

    Checkpointer checkpointer(*wal_, target, txns_, anchor_);
    ASSERT_TRUE(checkpointer.RunToCompletion().ok());

    // Every page written back, and the log durable past all of them -
    // WAL-before-data held for the whole checkpoint.
    EXPECT_TRUE(pool.DirtyTable().empty());
    EXPECT_EQ(pool.stats().flushes, 2u);
    EXPECT_TRUE(wal_->IsDurable(checkpointer.redo_start_lsn()));
    EXPECT_GT(checkpointer.last_checkpoint_lsn(), checkpointer.redo_start_lsn());
}

TEST_F(CheckpointerTest, ASecondCheckpointOverACleanPoolAnchorsItself) {
    storage::InMemoryPageStore backing;
    storage::BufferPool pool(backing, 8);
    pool.SetWalDurability(wal_.get());
    storage::BufferPoolCheckpointTarget target(pool);
    Checkpointer checkpointer(*wal_, target, txns_, anchor_);

    auto frame = pool.AllocNew(1);
    ASSERT_TRUE(frame.ok());
    storage::FormatPage(frame.value()->bytes(), PageType::kHeap);
    auto lsn = wal_->Append({RecordType::kHeapInsert, 5, 1, 0});
    ASSERT_TRUE(lsn.ok());
    frame.value()->MarkDirty(lsn.value());
    pool.Unpin(*frame.value());

    ASSERT_TRUE(checkpointer.RunToCompletion().ok());
    const Lsn first_redo_start = checkpointer.redo_start_lsn();
    EXPECT_EQ(first_redo_start, lsn.value());

    // Nothing dirty now, so the second checkpoint's redo start is itself:
    // recovery after it replays nothing that came before it. That advance
    // is the bounded-recovery guarantee (wal.md section 1).
    ASSERT_TRUE(checkpointer.RunToCompletion().ok());
    EXPECT_GT(checkpointer.redo_start_lsn(), first_redo_start);
    EXPECT_EQ(checkpointer.redo_start_lsn(), checkpointer.last_checkpoint_lsn());
    EXPECT_EQ(anchor_.publishes(), 2u);
}

}  // namespace
}  // namespace kds::wal
