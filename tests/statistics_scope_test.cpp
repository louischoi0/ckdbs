// The debug half of "statistics queue no statement" (`docs/rules/rules.md`
// §3a; `base/statistics_scope.hpp`, DL-S5): inside a statistics record a
// blocking latch acquisition and a page fetch abort, naming the record; the
// try form and everything outside a record are untouched. In a release
// build the scope is nothing and these cells say so.

#include "kds/base/statistics_scope.hpp"

#include <gtest/gtest.h>

#include "kds/base/latch.hpp"
#include "kds/server/superblock.hpp"
#include "kds/stats/statement_stats.hpp"
#include "kds/storage/in_memory_page_store.hpp"

namespace kds {
namespace {

TEST(StatisticsScopeTest, ATryInsideARecordAndABlockingLatchOutsideOneAreBothFine) {
    Latch latch(LatchKind::kStatsRing);
    {
        const StatisticsRecordScope recording("a test's record");
        const LatchGuard tried(&latch, std::try_to_lock);
        EXPECT_TRUE(tried.owns());
    }
    const LatchGuard waited(&latch);  // no record open: the form that may wait
    SUCCEED();
}

TEST(StatisticsScopeTest, AScopeClosesAndNestedOnesRestoreTheOuter) {
    Latch latch(LatchKind::kStatsRing);
    {
        const StatisticsRecordScope outer("outer");
        { const StatisticsRecordScope inner("inner"); }
        // Still inside `outer`: a try is all it may do.
        const LatchGuard tried(&latch, std::try_to_lock);
        EXPECT_TRUE(tried.owns());
    }
    const LatchGuard waited(&latch);
    SUCCEED();
}

// A real record's entry point opens a scope and stays inside the rule: it
// runs to its end with the guard armed.
TEST(StatisticsScopeTest, ARealRecordRunsUnderItsScope) {
    stats::StatementStats::ResetForTest();
    stats::StatementStats::Record(0, 1, "SELECT 1", false, 1, 1);
    EXPECT_EQ(stats::StatementStats::Snapshot().size(), 1u);
    stats::StatementStats::ResetForTest();
}

#ifndef NDEBUG

using StatisticsScopeDeathTest = ::testing::Test;

TEST(StatisticsScopeDeathTest, ABlockingLatchInsideARecordAbortsNamingTheRecord) {
    Latch latch(LatchKind::kStatsRing);
    EXPECT_DEATH(
        {
            const StatisticsRecordScope recording("a test's record");
            const LatchGuard waited(&latch);  // uncontended, and still an abort
        },
        "a blocking latch acquisition inside a statistics record \\(a test's record\\)");
}

// The form is what is refused, not the latch: an unarmed structure's null
// latch at `cores = 1` is the call site that waits above one core, and most
// of the suite runs at `cores = 1`.
TEST(StatisticsScopeDeathTest, ABlockingGuardOverAnUnarmedLatchInsideARecordAborts) {
    EXPECT_DEATH(
        {
            const StatisticsRecordScope recording("a test's record");
            const LatchGuard waited(nullptr);
        },
        "a blocking latch acquisition inside a statistics record \\(a test's record\\)");
}

TEST(StatisticsScopeDeathTest, APageFetchInsideARecordAbortsNamingTheRecord) {
    storage::InMemoryPageStore store{server::kFirstUserPageId};
    EXPECT_DEATH(
        {
            const StatisticsRecordScope recording("a test's record");
            (void)store.GetForRead(server::kFirstUserPageId);
        },
        "a page fetch inside a statistics record \\(a test's record\\)");
}

TEST(StatisticsScopeDeathTest, APageCreationInsideARecordAbortsNamingTheRecord) {
    storage::InMemoryPageStore store{server::kFirstUserPageId};
    EXPECT_DEATH(
        {
            const StatisticsRecordScope recording("a test's record");
            (void)store.CreateNew();
        },
        "a page creation inside a statistics record \\(a test's record\\)");
}

#else

// A release build: the scope is nothing, so what would abort in a debug
// build runs through. The rule is proved by the debug suite, not here.
TEST(StatisticsScopeTest, InAReleaseBuildTheScopeIsNothing) {
    Latch latch(LatchKind::kStatsRing);
    const StatisticsRecordScope recording("a test's record");
    const LatchGuard waited(&latch);
    SUCCEED();
}

#endif

}  // namespace
}  // namespace kds
