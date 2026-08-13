#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/page_mgr/page_mgr.hpp"
#include "kds/wal/durability.hpp"

// page.md section 18-4: a scripted WalDurability proves Flush() waits for
// the log, and that MarkClean is unreachable outside the flush path. The
// second half of that is enforced at compile time - MarkClean is private,
// so a test that calls it does not build - which leaves this file to prove
// the first half, plus the orderings that make it worth having.

namespace kds::storage {
namespace {

// A log that only moves when the test says so, and records every question
// asked of it. Nothing here does I/O: the point is to script the answers.
class ScriptedWal final : public wal::WalDurability {
public:
    wal::Lsn durable_lsn() const noexcept override { return durable_; }

    Status EnsureDurable(wal::Lsn lsn) override {
        waited_for_.push_back(lsn);
        if (fail_) {
            return Status::IoError("injected: log sync failed");
        }
        durable_ = std::max(durable_, lsn + 1);  // strictly past the record
        return Status::OK();
    }

    void SetDurable(wal::Lsn lsn) noexcept { durable_ = lsn; }
    void FailEveryWait(bool fail) noexcept { fail_ = fail; }
    const std::vector<wal::Lsn>& waited_for() const noexcept { return waited_for_; }

private:
    wal::Lsn durable_ = 0;
    bool fail_ = false;
    std::vector<wal::Lsn> waited_for_;
};

// A store that counts durability barriers and can refuse one, so the tests
// can see how many a flush costs and what a failed one leaves behind.
class CountingStore final : public PageStore {
public:
    explicit CountingStore(InMemoryPageStore& inner) : inner_(inner) {}

    StatusOr<std::span<std::byte, kPageSize>> CreateAtUnpinned(PageId page_id) override {
        return inner_.CreateAtUnpinned(page_id);
    }
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewUnpinned() override {
        return inner_.CreateNewUnpinned();
    }
    StatusOr<std::span<std::byte, kPageSize>> GetUnpinned(PageId page_id) override {
        return inner_.GetUnpinned(page_id);
    }
    Status Sync() override {
        ++syncs_;
        if (fail_next_) {
            fail_next_ = false;
            return Status::IoError("injected: write-out failed");
        }
        durable_ = true;
        return inner_.Sync();
    }

    std::uint64_t syncs() const noexcept { return syncs_; }
    bool ever_durable() const noexcept { return durable_; }
    void FailNextSync() noexcept { fail_next_ = true; }

private:
    InMemoryPageStore& inner_;
    std::uint64_t syncs_ = 0;
    bool fail_next_ = false;
    bool durable_ = false;
};

class WalGateTest : public ::testing::Test {
protected:
    WalGateTest() : store_(backing_), pool_(store_, 8) { pool_.SetWalDurability(&wal_); }

    // A formatted heap page dirtied by the record at `record_lsn`.
    Frame* DirtyPage(PageId page_id, wal::Lsn record_lsn) {
        auto frame = pool_.AllocNew(page_id);
        EXPECT_TRUE(frame.ok()) << frame.status().message();
        if (!frame.ok()) {
            return nullptr;
        }
        FormatPage(frame.value()->bytes(), PageType::kHeap);
        frame.value()->MarkDirty(record_lsn);
        return frame.value();
    }

    InMemoryPageStore backing_;
    CountingStore store_;
    BufferPool pool_;
    ScriptedWal wal_;
};

// ---- The gate ------------------------------------------------------------

TEST_F(WalGateTest, FlushWaitsForTheLogBeforeWritingAPage) {
    Frame* frame = DirtyPage(1, 5000);
    ASSERT_NE(frame, nullptr);

    // The log is behind the page: the write must not happen until it
    // catches up, and the wait is what makes it happen.
    wal_.SetDurable(4000);
    ASSERT_TRUE(pool_.Flush(*frame).ok());

    ASSERT_EQ(wal_.waited_for().size(), 1u);
    EXPECT_EQ(wal_.waited_for().front(), 5000u);
    EXPECT_TRUE(wal_.IsDurable(5000));
    EXPECT_TRUE(store_.ever_durable());
    EXPECT_FALSE(frame->is_dirty());
    EXPECT_EQ(pool_.stats().wal_waits, 1u);
}

TEST_F(WalGateTest, AFlushBehindADurableLogNeverWaits) {
    Frame* frame = DirtyPage(1, 5000);
    ASSERT_NE(frame, nullptr);

    // The common case: the log ran ahead long ago, so the gate costs
    // nothing on the flush path.
    wal_.SetDurable(9000);
    ASSERT_TRUE(pool_.Flush(*frame).ok());
    EXPECT_TRUE(wal_.waited_for().empty());
    EXPECT_EQ(pool_.stats().wal_waits, 0u);
}

TEST_F(WalGateTest, ThePageIsNotWrittenIfTheLogWaitFails) {
    Frame* frame = DirtyPage(1, 5000);
    ASSERT_NE(frame, nullptr);

    wal_.SetDurable(0);
    wal_.FailEveryWait(true);
    EXPECT_EQ(pool_.Flush(*frame).code(), StatusCode::kIoError);
    // The whole contract in one assertion: log first, or no write at all.
    EXPECT_EQ(store_.syncs(), 0u);
    EXPECT_TRUE(frame->is_dirty());
    EXPECT_EQ(pool_.stats().flushes, 0u);

    wal_.FailEveryWait(false);
    ASSERT_TRUE(pool_.Flush(*frame).ok());
    EXPECT_FALSE(frame->is_dirty());
}

TEST_F(WalGateTest, AFailedWriteOutLeavesTheFrameDirty) {
    Frame* frame = DirtyPage(1, 5000);
    ASSERT_NE(frame, nullptr);
    wal_.SetDurable(9000);

    store_.FailNextSync();
    EXPECT_EQ(pool_.Flush(*frame).code(), StatusCode::kIoError);
    // Dirty is the only safe state for a page whose write-out failed: the
    // next flush must try again.
    EXPECT_TRUE(frame->is_dirty());
    EXPECT_EQ(pool_.stats().flushes, 0u);
}

TEST_F(WalGateTest, APoolWithNoSeamRefusesToFlushALoggedPage) {
    BufferPool ungated(store_, 4);  // no SetWalDurability
    auto frame = ungated.AllocNew(1);
    ASSERT_TRUE(frame.ok());
    FormatPage(frame.value()->bytes(), PageType::kHeap);
    frame.value()->MarkDirty(5000);

    // A gate that disappears when unconfigured is not a gate.
    EXPECT_EQ(ungated.Flush(*frame.value()).code(), StatusCode::kInvalidArgument);
    EXPECT_TRUE(frame.value()->is_dirty());
}

TEST_F(WalGateTest, ANeverLoggedPageFlushesWithoutASeam) {
    BufferPool ungated(store_, 4);
    auto frame = ungated.AllocNew(1);
    ASSERT_TRUE(frame.ok());
    // page_lsn 0: nothing in any log describes this page, so WAL-before-
    // data has nothing to order against.
    ASSERT_TRUE(ungated.Flush(*frame.value()).ok());
    EXPECT_FALSE(frame.value()->is_dirty());
}

TEST_F(WalGateTest, FlushingACleanFrameDoesNothing) {
    Frame* frame = DirtyPage(1, 5000);
    ASSERT_NE(frame, nullptr);
    wal_.SetDurable(9000);
    ASSERT_TRUE(pool_.Flush(*frame).ok());

    const std::uint64_t syncs = store_.syncs();
    ASSERT_TRUE(pool_.Flush(*frame).ok());
    EXPECT_EQ(store_.syncs(), syncs);
    EXPECT_EQ(pool_.stats().flushes, 1u);
}

// ---- Checksum ordering ---------------------------------------------------

TEST_F(WalGateTest, TheChecksumIsStampedInsideFlushAndCoversWhatLands) {
    Frame* frame = DirtyPage(1, 5000);
    ASSERT_NE(frame, nullptr);
    wal_.SetDurable(9000);
    // Not stamped yet: the page has been mutated but not written out.
    EXPECT_EQ(GetStoredChecksum(frame->bytes()), 0u);

    ASSERT_TRUE(pool_.Flush(*frame).ok());
    // Stamped last, so it covers the page_lsn the mutation wrote too.
    EXPECT_NE(GetStoredChecksum(frame->bytes()), 0u);
    EXPECT_TRUE(VerifyPageChecksum(frame->bytes()).ok());
    EXPECT_EQ(GetPageLsn(frame->bytes()), 5000u);
}

TEST_F(WalGateTest, AnUnformattedPageIsWrittenWithoutAFakeChecksum) {
    auto frame = pool_.AllocNew(1);
    ASSERT_TRUE(frame.ok());  // created, never formatted, dirty by creation

    ASSERT_TRUE(pool_.Flush(*frame.value()).ok());
    // Stamping one would make a page whose type byte says "unformatted"
    // look verified.
    EXPECT_EQ(GetStoredChecksum(frame.value()->bytes()), 0u);
    EXPECT_EQ(pool_.stats().unformatted_flushes, 1u);
}

// ---- Dirty tracking ------------------------------------------------------

TEST_F(WalGateTest, MarkDirtyStampsThePageAndMirrorsIt) {
    auto frame = pool_.AllocNew(1);
    ASSERT_TRUE(frame.ok());
    FormatPage(frame.value()->bytes(), PageType::kHeap);

    frame.value()->MarkDirty(100);
    EXPECT_EQ(frame.value()->page_lsn(), 100u);
    EXPECT_EQ(GetPageLsn(frame.value()->bytes()), 100u);  // mirror cannot drift
    EXPECT_EQ(frame.value()->rec_lsn(), 100u);

    // recLSN is where the frame *became* dirty; later records move
    // page_lsn only, or the checkpoint's redo start would slide forward
    // past changes it still has to replay.
    frame.value()->MarkDirty(200);
    EXPECT_EQ(frame.value()->page_lsn(), 200u);
    EXPECT_EQ(frame.value()->rec_lsn(), 100u);
}

TEST_F(WalGateTest, ACreatedPageIsDirtyWithNothingToReplay) {
    auto frame = pool_.AllocNew(1);
    ASSERT_TRUE(frame.ok());
    // AllocNew dirties the frame as bookkeeping, before any record
    // describes it. recLSN 0 here means "nothing to replay", not "replay
    // from the head of the log" - a checkpoint that min()ed this in would
    // drag its redo start back to the start of the stream.
    EXPECT_TRUE(frame.value()->is_dirty());
    EXPECT_EQ(frame.value()->rec_lsn(), 0u);
    ASSERT_EQ(pool_.DirtyTable().size(), 1u);
    EXPECT_EQ(pool_.DirtyTable().front().rec_lsn, 0u);

    // The first logged mutation is the one recovery would start from,
    // even though the frame was already dirty.
    FormatPage(frame.value()->bytes(), PageType::kHeap);
    frame.value()->MarkDirty(700);
    EXPECT_EQ(frame.value()->rec_lsn(), 700u);
}

TEST_F(WalGateTest, RecLsnRestartsAfterAFlush) {
    Frame* frame = DirtyPage(1, 100);
    ASSERT_NE(frame, nullptr);
    wal_.SetDurable(9000);
    ASSERT_TRUE(pool_.Flush(*frame).ok());

    frame->MarkDirty(300);
    EXPECT_EQ(frame->rec_lsn(), 300u);  // clean, then dirty again: new recovery start
    EXPECT_EQ(frame->page_lsn(), 300u);
}

TEST_F(WalGateTest, TheDirtyTableIsWhatCheckpointBeginRecords) {
    ASSERT_NE(DirtyPage(30, 300), nullptr);
    ASSERT_NE(DirtyPage(10, 100), nullptr);
    ASSERT_NE(DirtyPage(20, 200), nullptr);
    wal_.SetDurable(9000);

    auto table = pool_.DirtyTable();
    ASSERT_EQ(table.size(), 3u);
    // Page-id sorted, so the record is a deterministic function of pool
    // state; the redo start is min(recLSN) over it.
    EXPECT_EQ(table[0].page_id, 10u);
    EXPECT_EQ(table[0].rec_lsn, 100u);
    EXPECT_EQ(table[1].page_id, 20u);
    EXPECT_EQ(table[2].rec_lsn, 300u);

    auto flushed = pool_.Lookup(20);
    ASSERT_TRUE(flushed.ok());
    ASSERT_TRUE(pool_.Flush(*flushed.value()).ok());
    pool_.Unpin(*flushed.value());

    // A flushed page leaves the table: it no longer needs replaying.
    table = pool_.DirtyTable();
    ASSERT_EQ(table.size(), 2u);
    EXPECT_EQ(table[0].page_id, 10u);
    EXPECT_EQ(table[1].page_id, 30u);
}

// ---- Batched flush -------------------------------------------------------

TEST_F(WalGateTest, FlushAllPaysOneLogWaitAndOneBarrier) {
    ASSERT_NE(DirtyPage(3, 300), nullptr);
    ASSERT_NE(DirtyPage(1, 100), nullptr);
    ASSERT_NE(DirtyPage(2, 200), nullptr);
    wal_.SetDurable(0);

    ASSERT_TRUE(pool_.FlushAll().ok());

    // One wait, on the highest page_lsn in the batch - the log is a
    // watermark, so that one clears the other two.
    ASSERT_EQ(wal_.waited_for().size(), 1u);
    EXPECT_EQ(wal_.waited_for().front(), 300u);
    EXPECT_EQ(store_.syncs(), 1u);
    EXPECT_EQ(pool_.stats().flushes, 3u);
    EXPECT_EQ(pool_.stats().flush_barriers, 1u);
    EXPECT_TRUE(pool_.DirtyTable().empty());
}

TEST_F(WalGateTest, FlushAllWritesNothingIfTheLogWaitFails) {
    ASSERT_NE(DirtyPage(1, 100), nullptr);
    ASSERT_NE(DirtyPage(2, 200), nullptr);
    wal_.SetDurable(0);
    wal_.FailEveryWait(true);

    EXPECT_EQ(pool_.FlushAll().code(), StatusCode::kIoError);
    EXPECT_EQ(store_.syncs(), 0u);
    EXPECT_EQ(pool_.DirtyTable().size(), 2u);  // every frame still dirty
}

TEST_F(WalGateTest, FlushAllOnACleanPoolDoesNoIo) {
    ASSERT_TRUE(pool_.FlushAll().ok());
    EXPECT_EQ(store_.syncs(), 0u);
    EXPECT_EQ(pool_.stats().flush_barriers, 0u);
}

}  // namespace
}  // namespace kds::storage
