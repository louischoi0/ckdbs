#include "kds/stats/statement_stats.hpp"

#include <string>

#include <gtest/gtest.h>

// `sys.statement_stats`' store (`stats/statement_stats.hpp`, BO-S5): its key,
// its merge across cores, and its bound.

namespace kds::stats {
namespace {

class StatementStatsTest : public ::testing::Test {
protected:
    void SetUp() override { StatementStats::ResetForTest(); }
    void TearDown() override { StatementStats::ResetForTest(); }
};

TEST_F(StatementStatsTest, AKindIsAStatementVerbOrOther) {
    EXPECT_EQ(StatementKindOf("  select * from t"), "SELECT");
    EXPECT_EQ(StatementKindOf("Purge from t where id = 1"), "PURGE");
    EXPECT_EQ(StatementKindOf("FLIBBERTIGIBBET EVERYTHING"), "OTHER");
    EXPECT_EQ(StatementKindOf(""), "OTHER");
    // The token `DispatchInner` routes on, split at whitespace: a verb glued
    // to punctuation routes nowhere, so it is no verb.
    EXPECT_EQ(StatementKindOf("SHOW(1)"), "OTHER");
    EXPECT_EQ(StatementKindOf("COMMIT;"), "OTHER");
    EXPECT_EQ(StatementKindOf("commit"), "COMMIT");
}

TEST_F(StatementStatsTest, OneShapeOnTwoCoresIsOneRowMerged) {
    StatementStats::Record(0, 42, "SELECT v FROM t WHERE id = 1", false, 10, 1'000);
    StatementStats::Record(1, 42, "SELECT v FROM t WHERE id = 2", true, 30, 500);
    StatementStats::Record(0, 42, "SELECT v FROM t WHERE id = 3", false, 20, 2'000);

    const std::vector<StatementShape> shapes = StatementStats::Snapshot();
    ASSERT_EQ(shapes.size(), 1u);
    const StatementShape& s = shapes[0];
    EXPECT_EQ(s.fetch_id, 42u);
    EXPECT_EQ(s.kind, "SELECT");
    EXPECT_EQ(s.calls, 3u);
    EXPECT_EQ(s.refused, 1u);
    EXPECT_EQ(s.total_us, 60u);
    EXPECT_EQ(s.max_us, 30u);
    // The earliest seen anywhere, and its statement.
    EXPECT_EQ(s.first_seen_us, 500);
    EXPECT_EQ(s.shape_text, "SELECT v FROM t WHERE id = 2");
    EXPECT_EQ(s.last_seen_us, 2'000);
}

TEST_F(StatementStatsTest, KindsWithNoFingerprintAreOneRowEachAtFetchIdZero) {
    StatementStats::Record(0, 0, "DELETE FROM t WHERE id = 1", false,
                           1, 1);
    StatementStats::Record(0, 0, "DELETE FROM u WHERE id = 9", false,
                           1, 2);
    StatementStats::Record(0, 0, "CREATE TABLE x (id int64)", false,
                           1, 3);
    const std::vector<StatementShape> shapes = StatementStats::Snapshot();
    ASSERT_EQ(shapes.size(), 2u);
    EXPECT_EQ(shapes[0].kind, "CREATE");
    EXPECT_EQ(shapes[1].kind, "DELETE");
    EXPECT_EQ(shapes[1].calls, 2u);
}

TEST_F(StatementStatsTest, AShapePastTheBoundIsDroppedAndCountedAndKnownShapesStillCount) {
    for (std::uint64_t id = 1; id <= StatementStats::kShapesPerCore; ++id) {
        StatementStats::Record(2, id, "SELECT", false, 1, 1);
    }
    StatementStats::Record(2, StatementStats::kShapesPerCore + 1, "SELECT", false, 1, 1);
    EXPECT_EQ(StatementStats::Dropped(), 1u);
    StatementStats::Record(2, 1, "SELECT", false, 1, 1);
    const std::vector<StatementShape> shapes = StatementStats::Snapshot();
    ASSERT_EQ(shapes.size(), StatementStats::kShapesPerCore);
    EXPECT_EQ(shapes.front().calls, 2u) << "a known shape is still counted at the bound";
}

TEST_F(StatementStatsTest, TheShapeTextIsCutAtItsBound) {
    const std::string longer(StatementStats::kShapeTextBytes + 40, 'x');
    StatementStats::Record(0, 7, longer, false, 1, 1);
    EXPECT_EQ(StatementStats::Snapshot().front().shape_text.size(),
              StatementStats::kShapeTextBytes);
}

TEST_F(StatementStatsTest, TheShapeTextIsNeverCutInsideAUtf8Sequence) {
    // 255 ASCII bytes, then a two-byte sequence straddling the 256-byte cut.
    const std::string line = "SHOW " + std::string(250, 'x') + "\xC3\xA9";
    StatementStats::Record(0, 0, line, false, 1, 1);
    EXPECT_EQ(StatementStats::Snapshot().front().shape_text, line.substr(0, 255));
}

}  // namespace
}  // namespace kds::stats
