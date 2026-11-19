#include "kds/stats/txn_stats.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

// `sys.meta`'s six `txn` counts and `sys.txn_histogram`'s store
// (`stats/txn_stats.hpp`, DF-Q13 (a)): what a count is filed under and what a
// read sees.

namespace kds::stats {
namespace {

class TxnStatsTest : public ::testing::Test {
protected:
    void SetUp() override { TxnStats::ResetForTest(); }
    void TearDown() override { TxnStats::ResetForTest(); }
};

TEST_F(TxnStatsTest, AnEndIsCountedUnderItsCoreAndItsCauseAlone) {
    TxnStats::Count(1, TxnEnd::kCommit);
    TxnStats::Count(1, TxnEnd::kCommit);
    TxnStats::Count(1, TxnEnd::kWaitAbort);
    TxnStats::Count(0, TxnEnd::kCeilingRollback);

    const auto core1 = TxnStats::CountsOf(1);
    EXPECT_EQ(core1[static_cast<std::size_t>(TxnEnd::kCommit)], 2u);
    EXPECT_EQ(core1[static_cast<std::size_t>(TxnEnd::kWaitAbort)], 1u);
    EXPECT_EQ(core1[static_cast<std::size_t>(TxnEnd::kAbort)], 0u);
    EXPECT_EQ(core1[static_cast<std::size_t>(TxnEnd::kCeilingRollback)], 0u);
    const auto core0 = TxnStats::CountsOf(0);
    EXPECT_EQ(core0[static_cast<std::size_t>(TxnEnd::kCeilingRollback)], 1u);
    EXPECT_EQ(core0[static_cast<std::size_t>(TxnEnd::kCommit)], 0u);
}

TEST_F(TxnStatsTest, EveryEndHasAFieldNameOfItsOwn) {
    // `sys.meta` splits a section at spaces and a field at `=`: a name with
    // either, or two ends sharing one, would lose a count.
    std::vector<std::string> names;
    for (std::size_t i = 0; i < kTxnEndCount; ++i) {
        const std::string name = TxnEndFieldName(static_cast<TxnEnd>(i));
        EXPECT_EQ(name.find_first_of(" ="), std::string::npos) << name;
        EXPECT_NE(name, "unknown");
        for (const std::string& seen : names) EXPECT_NE(seen, name);
        names.push_back(name);
    }
    EXPECT_EQ(names.size(), 6u);
}

TEST_F(TxnStatsTest, ALifeFallsInItsLog2BucketOfMicrosecondsByAccessMode) {
    TxnStats::RecordLife(2, /*read_only=*/false, 0);              // bucket 0
    TxnStats::RecordLife(2, /*read_only=*/false, 1'999);          // 1 us: bucket 0
    TxnStats::RecordLife(2, /*read_only=*/false, 5'000);          // 5 us: [4, 8)
    TxnStats::RecordLife(2, /*read_only=*/true, 5'000);           // apart
    TxnStats::RecordLife(2, /*read_only=*/true, 1'000'000'000);   // 1 s: [2^19, 2^20) us
    TxnStats::RecordLife(0, /*read_only=*/false, ~std::uint64_t{0});  // the open last bucket

    const std::vector<TxnLifeBucket> rows = TxnStats::SnapshotLives();
    ASSERT_EQ(rows.size(), 5u);
    // Ordered by (core, read-write first, bucket).
    EXPECT_EQ(rows[0].core, 0u);
    EXPECT_FALSE(rows[0].read_only);
    EXPECT_EQ(rows[0].bucket, sched::kWaitHistogramBuckets - 1);
    EXPECT_EQ(rows[1].core, 2u);
    EXPECT_FALSE(rows[1].read_only);
    EXPECT_EQ(rows[1].bucket, 0u);
    EXPECT_EQ(rows[1].count, 2u);
    EXPECT_EQ(rows[2].bucket, 2u);
    EXPECT_EQ(rows[2].count, 1u);
    EXPECT_TRUE(rows[3].read_only);
    EXPECT_EQ(rows[3].bucket, 2u);
    EXPECT_TRUE(rows[4].read_only);
    EXPECT_EQ(rows[4].bucket, 19u);
}

}  // namespace
}  // namespace kds::stats
