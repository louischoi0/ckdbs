#include "kds/stats/index_stats.hpp"

#include <gtest/gtest.h>

// `sys.indexes`' usage store (`stats/index_stats.hpp`, DF-S3): what a record
// adds, what a read sums, and its bound.

namespace kds::stats {
namespace {

// One run on `core`: `probes` equality walks, `ranges` range walks.
void OneRun(IndexStats& stats, std::uint32_t core, catalog::Oid oid, std::uint64_t probes,
         std::uint64_t ranges, std::uint64_t scanned, std::uint64_t resolved,
         std::uint64_t at_ns) {
    IndexUse* counts = stats.CountsOf(core, oid);
    ASSERT_NE(counts, nullptr);
    if (probes != 0) counts->Add(/*range=*/false, probes, scanned, resolved, at_ns);
    if (ranges != 0) counts->Add(/*range=*/true, ranges, probes != 0 ? 0 : scanned,
                                 probes != 0 ? 0 : resolved, at_ns);
}

TEST(IndexStatsTest, AnIndexsCountsAreTheSumOfEveryCoresAndItsLastUseTheLatest) {
    IndexStats stats;
    OneRun(stats,0, 100, 1, 0, 4, 3, 5'000);
    OneRun(stats,0, 100, 2, 0, 6, 6, 9'000);
    OneRun(stats,1, 100, 0, 1, 50, 40, 7'000);  // another core, an older use
    OneRun(stats,1, 200, 0, 3, 1, 1, 2'000);

    const auto uses = stats.Snapshot();
    ASSERT_EQ(uses.size(), 2u);
    const IndexUse& a = uses.at(100);
    EXPECT_EQ(a.probes, 3u);
    EXPECT_EQ(a.ranges, 1u);
    EXPECT_EQ(a.entries_scanned, 60u);
    EXPECT_EQ(a.rows_resolved, 49u);
    EXPECT_EQ(a.last_used_ns, 9'000u);
    EXPECT_EQ(uses.at(200).ranges, 3u);
    EXPECT_EQ(stats.Dropped(), 0u);
}

TEST(IndexStatsTest, ACoresCountsOfAnIndexAreOneObjectForTheStoresLife) {
    // The dispatcher keeps the pointer and adds through it with no latch:
    // it must be the object a later lookup and a reader see, however many
    // indexes the part takes after it.
    IndexStats stats;
    IndexUse* const first = stats.CountsOf(3, 100);
    ASSERT_NE(first, nullptr);
    for (catalog::Oid oid = 1000; oid < 3000; ++oid) ASSERT_NE(stats.CountsOf(3, oid), nullptr);
    EXPECT_EQ(stats.CountsOf(3, 100), first) << "a rehash moved an index's counts";
    first->Add(/*range=*/false, 2, 5, 4, 0);
    EXPECT_EQ(stats.Snapshot().at(100).probes, 2u);
    // A run with no clock leaves the last use as it was: never.
    EXPECT_EQ(stats.Snapshot().at(100).last_used_ns, 0u);
    // Another core's counts of the same index are its own.
    EXPECT_NE(stats.CountsOf(4, 100), first);
}

TEST(IndexStatsTest, TheStoreIsItsOwnersAndANewOneStartsEmpty) {
    // DA-R6's reason: a second instance in one process shares nothing.
    IndexStats first;
    OneRun(first, 0, 100, 1, 0, 1, 1, 1);
    IndexStats second;
    EXPECT_TRUE(second.Snapshot().empty());
    EXPECT_EQ(first.Snapshot().size(), 1u);
}

TEST(IndexStatsTest, AnIndexPastAPartsBoundIsDroppedAndCountedAndTheOthersStand) {
    IndexStats stats;
    for (std::size_t i = 0; i < IndexStats::kIndexesPerCore; ++i) {
        OneRun(stats,2, static_cast<catalog::Oid>(1000 + i), 1, 0, 1, 1, 1);
    }
    EXPECT_EQ(stats.Dropped(), 0u);
    EXPECT_EQ(stats.CountsOf(2, 999'999), nullptr);
    EXPECT_EQ(stats.CountsOf(2, 999'999), nullptr);
    EXPECT_EQ(stats.Dropped(), 2u);
    // An index the part already holds is still counted, and another core's
    // part has its own bound.
    OneRun(stats,2, 1000, 1, 0, 1, 1, 1);
    OneRun(stats,3, 999'999, 1, 0, 1, 1, 1);
    const auto uses = stats.Snapshot();
    EXPECT_EQ(uses.size(), IndexStats::kIndexesPerCore + 1);
    EXPECT_EQ(uses.at(1000).probes, 2u);
    EXPECT_EQ(uses.at(999'999).probes, 1u);
    EXPECT_EQ(stats.Dropped(), 2u);
}

}  // namespace
}  // namespace kds::stats
