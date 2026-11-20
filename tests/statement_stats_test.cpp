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
    // Every verb the dispatcher routes is a kind of its own.
    EXPECT_EQ(StatementKindOf("USE ledger"), "USE");
    EXPECT_EQ(StatementKindOf("reindex index t_v"), "REINDEX");
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

TEST_F(StatementStatsTest, OneShapeUnderTwoBindingsIsTwoRowsEachMergedAcrossCores) {
    // DI-S1 (`statement_binding.hpp`): the key holds what the statement
    // bound, so the cores' parts merge a binding's rows and never two
    // bindings'.
    StatementBinding east;
    east.Declare(40, 7);
    StatementBinding west;
    west.Declare(41, 8);
    for (std::uint32_t core = 0; core < 2; ++core) {
        StatementStats::Record(core, 77, "SELECT v FROM t WHERE id = 1", false, 10, 100, {}, {},
                               {}, east);
        StatementStats::Record(core, 77, "SELECT v FROM t WHERE id = 1", core == 1, 30, 200, {},
                               {}, {}, west);
    }
    StatementStats::Record(0, 77, "SELECT v FROM t WHERE id = 1", true, 5, 300);  // bound nothing

    const std::vector<StatementShape> shapes = StatementStats::Snapshot();
    ASSERT_EQ(shapes.size(), 3u);
    // Ordered within the shape: the unbound row, then by namespace.
    EXPECT_TRUE(shapes[0].binding.none());
    EXPECT_EQ(shapes[0].calls, 1u);
    EXPECT_TRUE(shapes[1].binding == east);
    EXPECT_EQ(shapes[1].calls, 2u);
    EXPECT_EQ(shapes[1].refused, 0u);
    EXPECT_EQ(shapes[1].total_us, 20u);
    EXPECT_TRUE(shapes[2].binding == west);
    EXPECT_EQ(shapes[2].calls, 2u);
    EXPECT_EQ(shapes[2].refused, 1u);
    EXPECT_EQ(shapes[2].total_us, 60u);
}

TEST_F(StatementStatsTest, AStatementOverTheBindingsCapIsOneRowThatIsNoBindings) {
    StatementBinding over;
    for (std::uint64_t oid = 1; oid <= StatementBinding::kMaxRelations + 1; ++oid) {
        over.Declare(oid, 3);
    }
    StatementBinding other;
    for (std::uint64_t oid = 50; oid <= 50 + StatementBinding::kMaxRelations; ++oid) {
        other.Declare(oid, 4);
    }
    StatementStats::Record(0, 9, "SELECT 1", false, 1, 1, {}, {}, {}, over);
    StatementStats::Record(0, 9, "SELECT 1", false, 1, 1, {}, {}, {}, other);
    StatementStats::Record(0, 9, "SELECT 1", false, 1, 1);

    const std::vector<StatementShape> shapes = StatementStats::Snapshot();
    ASSERT_EQ(shapes.size(), 2u);
    EXPECT_TRUE(shapes[0].binding.none());
    EXPECT_EQ(shapes[0].calls, 1u);
    EXPECT_TRUE(shapes[1].binding.over);
    EXPECT_EQ(shapes[1].binding.n, 0u);
    EXPECT_EQ(shapes[1].calls, 2u);
}

TEST_F(StatementStatsTest, ASweepForGoneRelationsRunsOnceASchemaVersion) {
    StatementBinding gone;
    gone.Declare(40, 1);
    StatementBinding kept;
    kept.Declare(41, 1);
    StatementBinding both;
    both.Declare(41, 1);
    both.Declare(40, 1);
    StatementStats::Record(0, 1, "SELECT 1", false, 1, 1, {}, {}, {}, gone);
    StatementStats::Record(0, 2, "SELECT 1", false, 1, 1, {}, {}, {}, kept);
    StatementStats::Record(0, 3, "SELECT 1", false, 1, 1, {}, {}, {}, both);
    StatementStats::Record(0, 4, "SELECT 1", false, 1, 1);
    StatementStats::Record(1, 5, "SELECT 1", false, 1, 1, {}, {}, {}, gone);

    std::vector<std::uint64_t> asked;
    const auto is_gone = [&asked](std::uint64_t oid) {
        asked.push_back(oid);
        return oid == 40;
    };
    // Core 0's part alone, every row naming the gone relation, each
    // relation asked about once.
    EXPECT_EQ(StatementStats::ReclaimGone(0, 12, is_gone), 2u);
    EXPECT_EQ(asked, (std::vector<std::uint64_t>{40, 41}));
    EXPECT_EQ(StatementStats::Reclaimed(), 2u);
    std::vector<std::uint64_t> left;
    for (const StatementShape& s : StatementStats::Snapshot()) left.push_back(s.fetch_id);
    EXPECT_EQ(left, (std::vector<std::uint64_t>{2, 4, 5}));

    // The same version again asks nothing; another version sweeps again.
    asked.clear();
    EXPECT_EQ(StatementStats::ReclaimGone(0, 12, is_gone), 0u);
    EXPECT_TRUE(asked.empty());
    EXPECT_EQ(StatementStats::ReclaimGone(0, 13, is_gone), 0u);
    EXPECT_EQ(asked, (std::vector<std::uint64_t>{41}));
    EXPECT_EQ(StatementStats::ReclaimGone(1, 13, is_gone), 1u);
    EXPECT_EQ(StatementStats::Reclaimed(), 3u);
}

TEST_F(StatementStatsTest, KindsWithNoFingerprintAreOneRowEachAtFetchIdZero) {
    StatementStats::Record(0, 0, "PURGE FROM t WHERE id = 1", false,
                           1, 1);
    StatementStats::Record(0, 0, "PURGE FROM u WHERE id = 9", false,
                           1, 2);
    StatementStats::Record(0, 0, "CREATE TABLE x (id int64)", false,
                           1, 3);
    const std::vector<StatementShape> shapes = StatementStats::Snapshot();
    ASSERT_EQ(shapes.size(), 2u);
    EXPECT_EQ(shapes[0].kind, "CREATE");
    EXPECT_EQ(shapes[1].kind, "PURGE");
    EXPECT_EQ(shapes[1].calls, 2u);
}

// ---- DF-S1: what a shape costs ----------------------------------------------

TEST_F(StatementStatsTest, ACallsCostIsSummedWhereItWasCountedAndLeftOutWhereItWasNot) {
    StatementCost full;
    full.rows = 3;
    full.executed = true;
    full.rows_examined = 40;
    full.pages = 5;
    full.compiled = true;
    full.compile_ns = 7'000;
    sched::SessionCostSample delta;
    delta.on_cpu_ns = 9'000;
    delta.class_ns[static_cast<std::size_t>(WaitClass::kCommit)] = 2'000;
    delta.class_ns[static_cast<std::size_t>(WaitClass::kUserIo)] = 1'000;
    delta.page_reads = 2;
    delta.log_bytes = 100;
    delta.log_records = 2;
    full.timed = delta;
    StatementCost conflict;  // counted nothing but its refusal's cause
    conflict.conflict = true;

    // Two cores run the one shape: one row, every sum added (DF-R4).
    StatementStats::Record(0, 42, "UPDATE t SET v = 1 WHERE id = 1", false, 10, 1, {}, {}, full);
    StatementStats::Record(1, 42, "UPDATE t SET v = 1 WHERE id = 2", false, 10, 2, {}, {}, full);
    StatementStats::Record(1, 42, "UPDATE t SET v = 1 WHERE id = 3", true, 10, 3, {}, {},
                           conflict);

    const std::vector<StatementShape> shapes = StatementStats::Snapshot();
    ASSERT_EQ(shapes.size(), 1u);
    const StatementShape& s = shapes[0];
    EXPECT_EQ(s.calls, 3u);
    EXPECT_EQ(s.refused, 1u);
    EXPECT_EQ(s.conflicts, 1u);
    EXPECT_EQ(s.rows, 6u);
    // The call that counted none is in no sum and no count beside it (DF-R7).
    EXPECT_EQ(s.executed_calls, 2u);
    EXPECT_EQ(s.rows_examined, 80u);
    EXPECT_EQ(s.pages, 10u);
    EXPECT_EQ(s.compiled_calls, 2u);
    EXPECT_EQ(s.compile_ns, 14'000u);
    EXPECT_EQ(s.timed_calls, 2u);
    EXPECT_EQ(s.timed.on_cpu_ns, 18'000u);
    EXPECT_EQ(s.timed.class_ns[static_cast<std::size_t>(WaitClass::kCommit)], 4'000u);
    EXPECT_EQ(s.timed.class_ns[static_cast<std::size_t>(WaitClass::kUserIo)], 2'000u);
    EXPECT_EQ(s.timed.class_ns[static_cast<std::size_t>(WaitClass::kApplication)], 0u);
    EXPECT_EQ(s.timed.page_reads, 4u);
    EXPECT_EQ(s.timed.log_bytes, 200u);
    EXPECT_EQ(s.timed.log_records, 4u);
}

TEST_F(StatementStatsTest, AShapeWithNoCountedCallHasNoCountBesideItsSums) {
    StatementStats::Record(0, 7, "INSERT INTO t VALUES (1)", false, 1, 1);
    const StatementShape s = StatementStats::Snapshot().front();
    EXPECT_EQ(s.executed_calls, 0u);
    EXPECT_EQ(s.compiled_calls, 0u);
    EXPECT_EQ(s.timed_calls, 0u);
}

TEST_F(StatementStatsTest, TheLatencyBucketsAreTheWaitHistogramsAndSumToTheCalls) {
    // Bucket k holds [2^k, 2^(k+1)) us; 0 and 1 us fall in bucket 0 and the
    // last bucket takes everything above (`sched::WaitBucketOf`).
    const std::uint64_t elapsed_us[] = {0, 1, 2, 3, 4, 1'000, 1'023, 1'024, ~std::uint64_t{0} / 1000};
    for (std::uint64_t us : elapsed_us) {
        StatementStats::Record(0, 9, "SELECT v FROM t", false, us, 1);
    }
    StatementStats::Record(1, 9, "SELECT v FROM t", false, 5, 1);  // another core's part

    const StatementShape s = StatementStats::Snapshot().front();
    EXPECT_EQ(s.latency[0], 2u);
    EXPECT_EQ(s.latency[1], 2u);
    EXPECT_EQ(s.latency[2], 2u) << "4 us, and the other core's 5 us";
    EXPECT_EQ(s.latency[9], 2u) << "1000 and 1023 us";
    EXPECT_EQ(s.latency[10], 1u);
    EXPECT_EQ(s.latency[sched::kWaitHistogramBuckets - 1], 1u);
    std::uint64_t sum = 0;
    for (std::uint64_t n : s.latency) sum += n;
    EXPECT_EQ(sum, s.calls);
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
