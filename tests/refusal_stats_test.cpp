#include "kds/stats/refusal_stats.hpp"

#include <vector>

#include <gtest/gtest.h>

// `sys.refusal_stats`' store (`stats/refusal_stats.hpp`, DF-Q3 (a)): its key,
// its bound and what a read sees.

namespace kds::stats {
namespace {

class RefusalStatsTest : public ::testing::Test {
protected:
    void SetUp() override { RefusalStats::ResetForTest(); }
    void TearDown() override { RefusalStats::ResetForTest(); }
};

TEST_F(RefusalStatsTest, ARefusalIsCountedUnderItsCoreAndCode) {
    RefusalStats::Record(1, 10, 1'000);
    RefusalStats::Record(1, 10, 3'000);
    RefusalStats::Record(0, 10, 2'000);
    RefusalStats::Record(1, 2, 500);

    const std::vector<RefusalCount> rows = RefusalStats::Snapshot();
    ASSERT_EQ(rows.size(), 3u);
    // Ordered by (core, code); a code nothing refused has no row.
    EXPECT_EQ(rows[0].core, 0u);
    EXPECT_EQ(rows[0].code, 10u);
    EXPECT_EQ(rows[0].count, 1u);
    EXPECT_EQ(rows[1].core, 1u);
    EXPECT_EQ(rows[1].code, 2u);
    EXPECT_EQ(rows[2].core, 1u);
    EXPECT_EQ(rows[2].code, 10u);
    EXPECT_EQ(rows[2].count, 2u);
    EXPECT_EQ(rows[2].last_seen_us, 3'000);
}

TEST_F(RefusalStatsTest, ACodePastTheBoundIsNotRecorded) {
    // The store is bounded by the registry, never by what a caller hands it.
    RefusalStats::Record(0, static_cast<std::uint16_t>(RefusalStats::kRefusalCodes), 1);
    RefusalStats::Record(0, 0xFFFF, 1);
    EXPECT_TRUE(RefusalStats::Snapshot().empty());
    RefusalStats::Record(0, static_cast<std::uint16_t>(RefusalStats::kRefusalCodes - 1), 1);
    EXPECT_EQ(RefusalStats::Snapshot().size(), 1u);
}

}  // namespace
}  // namespace kds::stats
