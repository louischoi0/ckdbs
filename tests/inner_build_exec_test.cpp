#include "inner_build_fixture.hpp"

// JB3/JB4 (docs/workplan-join-inner-build.md) — the lazy build and the
// probe at the executor's walked-join site. The done-conditions, each
// pinned here: replies byte-identical to the un-built walk's
// (hand-computed vectors, which the probe now answers for every outer row
// after the first); the map holding every inner row passing the
// non-correlated residual; the examined class dropping from k·N to
// N-plus-matches. The fixture and its walk-order facts live in
// inner_build_fixture.hpp.

namespace kds::exec {
namespace {

class InnerBuildExecTest : public InnerBuildFixture {};

TEST_F(InnerBuildExecTest, TheWalkedJoinAnswersIdenticallyBuildsOnceAndProbes) {
    // JB3's done-condition 1: hand-computed, walk order — per outer row
    // (au 1, 2, 3), the inner relation in insertion order, full residual
    // applied. The pre-build executor's answer verbatim, which the probe
    // (JB4) now produces for every outer row after the first.
    // JB3's done-condition 2, count form: no non-correlated residual, so
    // every visible tr row enters the map — including (au_id=9), which
    // matches no outer row ever — and three outer rows build once: 5,
    // not 15.
    // JB4's done-condition, the examined class, on the same execution
    // that produced the pinned reply: alice's walk builds (5 examined),
    // bob probes his bucket (2), carol probes a missing bucket (0,
    // conclusive) — 7, where the per-row walk examined 15. N-plus-
    // matches, not k·N.
    ExecStats stats;
    const std::vector<std::string> rows =
        Run("SELECT au.name, tr.qty FROM au JOIN tr ON tr.au_id = au.id", &stats);
    EXPECT_EQ(rows, (std::vector<std::string>{"alice|10", "alice|30", "bob|20", "bob|50"}));
    const StepStats total = stats.Total();
    EXPECT_EQ(total.inner_builds, 1u);
    EXPECT_EQ(total.build_rows, 5u);
    ASSERT_GE(stats.steps.size(), 2u);
    EXPECT_EQ(stats.steps[1].rows_examined, 7u);
    EXPECT_EQ(stats.steps[1].build_probes, 2u) << "bob and carol; alice's walk was the build";
}

TEST_F(InnerBuildExecTest, ANestedAnnotatedStepBuildsUnderALiveOuterBuild) {
    // The `building_` save/restore, proven rather than asserted: two
    // annotated steps, the deeper one arming its build while the outer
    // one's is live. The outer map still buckets all five tr rows —
    // including the ones walked after the nested build ran — and each
    // step publishes exactly once.
    ExecStats stats;
    const std::vector<std::string> rows =
        Run("SELECT au.name, tr.qty, ln.amt FROM au JOIN tr ON tr.au_id = au.id "
            "JOIN ln ON ln.tr_qty = tr.qty",
            &stats);
    EXPECT_EQ(rows, (std::vector<std::string>{"alice|10|100", "alice|10|101", "alice|30|300",
                                              "bob|50|500"}));
    EXPECT_EQ(stats.Total().inner_builds, 2u);
    ASSERT_GE(stats.steps.size(), 3u);
    EXPECT_EQ(stats.steps[1].build_rows, 5u) << "the outer build bucketed every tr row";
    EXPECT_EQ(stats.steps[2].build_rows, 4u) << "the nested build bucketed every ln row";
}

TEST_F(InnerBuildExecTest, TheMapHoldsExactlyTheRowsPassingTheNonCorrelatedResidual) {
    // A non-correlated conjunct joins the step: `tr.qty <= 30` buckets
    // qty 10, 20, 30 and 5 — the last matching no outer key, bucketed
    // under its own value regardless — and excludes qty 50 outright.
    // Emission still applies the full residual: bob's qty 50 row is gone
    // from the reply too, by the same conjunct that kept it out of the map.
    ExecStats stats;
    const std::vector<std::string> rows = Run(
        "SELECT au.name, tr.qty FROM au JOIN tr ON tr.au_id = au.id WHERE tr.qty <= 30", &stats);
    EXPECT_EQ(rows, (std::vector<std::string>{"alice|10", "alice|30", "bob|20"}));
    const StepStats total = stats.Total();
    EXPECT_EQ(total.inner_builds, 1u);
    EXPECT_EQ(total.build_rows, 4u);
}

TEST_F(InnerBuildExecTest, AStopMidProbeEndsTheStatementCleanly) {
    // The probe's own stop branch — the LIMIT interaction. The sink stops
    // after three rows, which lands between bob's two bucket entries:
    // the reply is the walk's prefix, and nothing re-probes after a stop
    // (there is no resumable cursor to duplicate from).
    ExecStats stats;
    const std::vector<std::string> rows =
        Run("SELECT au.name, tr.qty FROM au JOIN tr ON tr.au_id = au.id", &stats,
            kDefaultJoinBuildMaxRows, /*stop_after=*/3);
    EXPECT_EQ(rows, (std::vector<std::string>{"alice|10", "alice|30", "bob|20"}));
}

TEST_F(InnerBuildExecTest, AStoppedFirstWalkPublishesNoMap) {
    // The sink stops the statement on its first row, mid-way through the
    // first inner walk: the rows after the stop were never bucketed, so
    // the partial map must not publish — WalkAndRecord's completed-walk
    // rule, applied to the build.
    ExecStats stats;
    const std::vector<std::string> rows =
        Run("SELECT au.name, tr.qty FROM au JOIN tr ON tr.au_id = au.id", &stats,
            kDefaultJoinBuildMaxRows, /*stop_after=*/1);
    EXPECT_EQ(rows, (std::vector<std::string>{"alice|10"}));
    EXPECT_EQ(stats.Total().inner_builds, 0u);
}

TEST_F(InnerBuildExecTest, AnExistsSubChainBuildsNothingUntilJB6) {
    // The stopping class is gated out wholesale: a prefix map that served
    // a miss as an absence is the one state the plan forbids, and the
    // gate at the dispatch is the enforcement. The reply is the plain
    // stopping walk's.
    ExecStats stats;
    const std::vector<std::string> rows = Run(
        "SELECT au.name FROM au WHERE EXISTS (SELECT tr.id FROM tr WHERE tr.au_id = au.id)",
        &stats);
    EXPECT_EQ(rows, (std::vector<std::string>{"alice", "bob"}));
    const StepStats total = stats.Total();
    EXPECT_EQ(total.inner_builds, 0u);
    EXPECT_EQ(total.build_rows, 0u);
}

}  // namespace
}  // namespace kds::exec
