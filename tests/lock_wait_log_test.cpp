#include "kds/stats/lock_wait_log.hpp"

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"

// `sys.lock_waits`' store (`stats/lock_wait_log.hpp`, BO-S3): its ring
// bound, its counts, and the row a wait writes when it never ends.

namespace kds::stats {
namespace {

class LockWaitLogTest : public ::testing::Test {
protected:
    void SetUp() override { LockWaitLog::ResetForTest(); }
    void TearDown() override { LockWaitLog::ResetForTest(); }
};

LockWaitRecord RowOn(std::uint32_t core, std::uint64_t key) {
    LockWaitRecord row;
    row.core = core;
    row.unit = txn::LockUnit::kTuple;
    row.key = key;
    return row;
}

TEST_F(LockWaitLogTest, AFullRingOverwritesItsOldestRowAndCountsIt) {
    for (std::uint64_t k = 1; k <= LockWaitLog::kRingCapacity + 2; ++k) {
        LockWaitLog::Record(RowOn(3, k));
    }
    const std::vector<LockWaitRecord> rows = LockWaitLog::Snapshot();
    ASSERT_EQ(rows.size(), LockWaitLog::kRingCapacity);
    // Oldest first, and the oldest is the third written: two were given up.
    EXPECT_EQ(rows.front().key, 3u);
    EXPECT_EQ(rows.front().seq, 3u);
    EXPECT_EQ(rows.back().key, LockWaitLog::kRingCapacity + 2);
    const LockWaitLog::Totals totals = LockWaitLog::Read();
    EXPECT_EQ(totals.recorded, LockWaitLog::kRingCapacity + 2);
    EXPECT_EQ(totals.overwritten, 2u);
    EXPECT_EQ(totals.dropped, 0u);
}

TEST_F(LockWaitLogTest, EachCoreKeepsItsOwnPartAndItsOwnSequence) {
    LockWaitLog::Record(RowOn(1, 10));
    LockWaitLog::Record(RowOn(0, 20));
    LockWaitLog::Record(RowOn(1, 11));
    const std::vector<LockWaitRecord> rows = LockWaitLog::Snapshot();
    ASSERT_EQ(rows.size(), 3u);
    // By core, then oldest first; each core counts from 1.
    EXPECT_EQ(rows[0].core, 0u);
    EXPECT_EQ(rows[0].seq, 1u);
    EXPECT_EQ(rows[1].key, 10u);
    EXPECT_EQ(rows[1].seq, 1u);
    EXPECT_EQ(rows[2].key, 11u);
    EXPECT_EQ(rows[2].seq, 2u);
}

TEST_F(LockWaitLogTest, AWaitThatNeverEndsIsWrittenCancelledWithItsTime) {
    sched::ManualClock clock(1'000'000);
    {
        PendingLockWait wait(&clock, RowOn(0, 5));
        clock.Advance(3'000'000);
        EXPECT_TRUE(LockWaitLog::Snapshot().empty()) << "written before it ended";
    }  // a coroutine destroyed at its park
    const std::vector<LockWaitRecord> rows = LockWaitLog::Snapshot();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].outcome, LockWaitOutcome::kCancelled);
    EXPECT_EQ(rows[0].waited_us, 3000u);
}

TEST_F(LockWaitLogTest, AWaitEndedOnceIsWrittenOnce) {
    sched::ManualClock clock;
    {
        PendingLockWait wait(&clock, RowOn(0, 5));
        wait.End(LockWaitOutcome::kGranted);
    }
    const std::vector<LockWaitRecord> rows = LockWaitLog::Snapshot();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].outcome, LockWaitOutcome::kGranted);
}

}  // namespace
}  // namespace kds::stats
