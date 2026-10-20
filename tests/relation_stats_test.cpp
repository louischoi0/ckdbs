#include "kds/stats/relation_stats.hpp"

#include <gtest/gtest.h>

// `sys.relation_stats`' activity store (`stats/relation_stats.hpp`, BO-S6):
// its sum across cores and its bound.

namespace kds::stats {
namespace {

class RelationStatsTest : public ::testing::Test {
protected:
    void SetUp() override { RelationStats::ResetForTest(); }
    void TearDown() override { RelationStats::ResetForTest(); }
};

TEST_F(RelationStatsTest, OneRelationOnTwoCoresIsSummedByKind) {
    RelationStats::Record(0, 200, RelationWrite::kInsert, 5);
    RelationStats::Record(1, 200, RelationWrite::kInsert, 2);
    RelationStats::Record(1, 200, RelationWrite::kUpdate, 3);
    RelationStats::Record(0, 200, RelationWrite::kDelete, 1);
    RelationStats::Record(3, 200, RelationWrite::kPurge, 1);
    RelationStats::Record(0, 201, RelationWrite::kInsert, 9);

    const auto snapshot = RelationStats::Snapshot();
    ASSERT_EQ(snapshot.size(), 2u);
    const RelationActivity& a = snapshot.at(200);
    EXPECT_EQ(a.inserts, 7u);
    EXPECT_EQ(a.updates, 3u);
    EXPECT_EQ(a.deletes, 1u);
    EXPECT_EQ(a.purged, 1u);
    EXPECT_EQ(snapshot.at(201).inserts, 9u);
}

TEST_F(RelationStatsTest, AWriteOfNoRowsTakesNoEntry) {
    RelationStats::Record(0, 200, RelationWrite::kUpdate, 0);
    EXPECT_TRUE(RelationStats::Snapshot().empty());
}

TEST_F(RelationStatsTest, ARelationPastTheBoundIsDroppedAndCountedAndKnownOnesStillCount) {
    for (catalog::Oid oid = 1; oid <= RelationStats::kRelationsPerCore; ++oid) {
        RelationStats::Record(2, oid, RelationWrite::kInsert, 1);
    }
    RelationStats::Record(2, RelationStats::kRelationsPerCore + 1, RelationWrite::kInsert, 1);
    EXPECT_EQ(RelationStats::Dropped(), 1u);
    RelationStats::Record(2, 1, RelationWrite::kInsert, 1);
    const auto snapshot = RelationStats::Snapshot();
    EXPECT_EQ(snapshot.size(), RelationStats::kRelationsPerCore);
    EXPECT_EQ(snapshot.at(1).inserts, 2u) << "a known relation is still counted at the bound";
    // Another core's part has a bound of its own.
    RelationStats::Record(3, RelationStats::kRelationsPerCore + 1, RelationWrite::kInsert, 1);
    EXPECT_EQ(RelationStats::Dropped(), 1u);
}

}  // namespace
}  // namespace kds::stats
