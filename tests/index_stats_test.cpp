#include "kds/stats/index_stats.hpp"

#include <gtest/gtest.h>

// `sys.indexes`' usage store (`stats/index_stats.hpp`, DF-S3): what a record
// adds, what a read sums, and its bound.

namespace kds::stats {
namespace {

IndexUse Use(std::uint64_t probes, std::uint64_t ranges, std::uint64_t scanned,
             std::uint64_t resolved, std::int64_t at_us) {
    IndexUse use;
    use.probes = probes;
    use.ranges = ranges;
    use.entries_scanned = scanned;
    use.rows_resolved = resolved;
    use.last_used_us = at_us;
    return use;
}

TEST(IndexStatsTest, AnIndexsCountsAreTheSumOfEveryCoresAndItsLastUseTheLatest) {
    IndexStats stats;
    stats.Record(0, 100, Use(1, 0, 4, 3, 5'000));
    stats.Record(0, 100, Use(2, 0, 6, 6, 9'000));
    stats.Record(1, 100, Use(0, 1, 50, 40, 7'000));  // another core, an older use
    stats.Record(1, 200, Use(0, 3, 1, 1, 2'000));

    const auto uses = stats.Snapshot();
    ASSERT_EQ(uses.size(), 2u);
    const IndexUse& a = uses.at(100);
    EXPECT_EQ(a.probes, 3u);
    EXPECT_EQ(a.ranges, 1u);
    EXPECT_EQ(a.entries_scanned, 60u);
    EXPECT_EQ(a.rows_resolved, 49u);
    EXPECT_EQ(a.last_used_us, 9'000);
    EXPECT_EQ(uses.at(200).ranges, 3u);
    EXPECT_EQ(stats.Dropped(), 0u);
}

TEST(IndexStatsTest, TheStoreIsItsOwnersAndANewOneStartsEmpty) {
    // DA-R6's reason: a second instance in one process shares nothing.
    IndexStats first;
    first.Record(0, 100, Use(1, 0, 1, 1, 1));
    IndexStats second;
    EXPECT_TRUE(second.Snapshot().empty());
    EXPECT_EQ(first.Snapshot().size(), 1u);
}

TEST(IndexStatsTest, AnIndexPastAPartsBoundIsDroppedAndCountedAndTheOthersStand) {
    IndexStats stats;
    for (std::size_t i = 0; i < IndexStats::kIndexesPerCore; ++i) {
        stats.Record(2, static_cast<catalog::Oid>(1000 + i), Use(1, 0, 1, 1, 1));
    }
    EXPECT_EQ(stats.Dropped(), 0u);
    stats.Record(2, 999'999, Use(1, 0, 1, 1, 1));
    stats.Record(2, 999'999, Use(1, 0, 1, 1, 1));
    EXPECT_EQ(stats.Dropped(), 2u);
    // An index the part already holds is still counted, and another core's
    // part has its own bound.
    stats.Record(2, 1000, Use(1, 0, 1, 1, 1));
    stats.Record(3, 999'999, Use(1, 0, 1, 1, 1));
    const auto uses = stats.Snapshot();
    EXPECT_EQ(uses.size(), IndexStats::kIndexesPerCore + 1);
    EXPECT_EQ(uses.at(1000).probes, 2u);
    EXPECT_EQ(uses.at(999'999).probes, 1u);
    EXPECT_EQ(stats.Dropped(), 2u);
}

}  // namespace
}  // namespace kds::stats
