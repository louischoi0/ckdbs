#include "inner_build_fixture.hpp"

// JB3/JB4/JB6 (docs/workplan-join-inner-build.md) — the lazy build, the
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

TEST_F(InnerBuildExecTest, AnExistsSubChainVisitsEachInnerRowAtMostOnce) {
    // JB6's done-condition, hand-computed on the fixture's walk order.
    // alice(1): nothing bucketed yet, so the walk runs from the head and
    // its sink stops on tr row 1 (au_id=1) - one row examined, one
    // bucketed, the mark at 1. bob(2): bucket 2 is empty and the map is a
    // prefix, so a miss is not an absence - the walk **resumes at the
    // mark** and stops on row 2. carol(3): bucket 3 empty again, the walk
    // resumes at 2 and runs out of relation without a match, which both
    // completes the map and proves the absence.
    //
    // Five inner rows, five examined - each visited once across three
    // outer rows, where the plain stopping walk examined 1 + 2 + 5 = 8.
    ExecStats stats;
    const std::vector<std::string> rows = Run(
        "SELECT au.name FROM au WHERE EXISTS (SELECT tr.id FROM tr WHERE tr.au_id = au.id)",
        &stats);
    EXPECT_EQ(rows, (std::vector<std::string>{"alice", "bob"}));
    const StepStats total = stats.Total();
    EXPECT_EQ(total.build_rows, 5u) << "every inner row bucketed, across three partial walks";
    EXPECT_EQ(total.inner_builds, 1u) << "the map completed when the last walk reached the end";
    EXPECT_EQ(total.build_probes, 2u) << "bob and carol; alice had no map to probe";
    ASSERT_GE(stats.steps.size(), 2u);
    EXPECT_EQ(stats.steps[1].rows_examined, 5u) << "N, not the walk's 8";
}

TEST_F(InnerBuildExecTest, ANotExistsProvesAbsenceOnlyFromACompletedMap) {
    // The other half of the same rule, and the one a partial map must
    // never answer: carol is emitted because a walk ran out of relation,
    // not because a bucket was empty. alice and bob are excluded by hits
    // exactly as EXISTS includes them.
    ExecStats stats;
    const std::vector<std::string> rows = Run(
        "SELECT au.name FROM au WHERE NOT EXISTS (SELECT tr.id FROM tr WHERE tr.au_id = au.id)",
        &stats);
    EXPECT_EQ(rows, (std::vector<std::string>{"carol"}));
    EXPECT_EQ(stats.Total().inner_builds, 1u);
    ASSERT_GE(stats.steps.size(), 2u);
    EXPECT_EQ(stats.steps[1].rows_examined, 5u);
}

TEST_F(InnerBuildExecTest, AnOuterKeyAlreadyInThePrefixIsAnsweredWithoutWalking) {
    // The prefix's payoff, on the one fixture shape with a repeated outer
    // key: ln's tr_qty is 10, 10, 30, 50 against tr's walk order
    // 10, 20, 30, 5, 50.
    //
    // ln#1 (10) walks one row and stops on the match. **ln#2 (10) walks
    // nothing at all** - bucket 10 holds tr row 1, re-checked and emitted,
    // and the sink stops there. ln#3 (30) misses and resumes at the mark
    // for rows 2 and 3; ln#4 (50) misses and resumes for rows 4 and 5.
    // Five inner rows bucketed, six examined - the five plus ln#2's one
    // re-check - where the plain walk examined 1 + 1 + 3 + 5 = 10.
    ExecStats stats;
    const std::vector<std::string> rows = Run(
        "SELECT ln.amt FROM ln WHERE EXISTS (SELECT tr.id FROM tr WHERE tr.qty = ln.tr_qty)",
        &stats);
    EXPECT_EQ(rows, (std::vector<std::string>{"100", "101", "300", "500"}));
    const StepStats total = stats.Total();
    EXPECT_EQ(total.build_rows, 5u);
    EXPECT_EQ(total.build_probes, 3u) << "ln#2, #3 and #4; ln#1 had no map to probe";
    EXPECT_EQ(total.inner_builds, 0u)
        << "the last walk stopped on the final row, so the map never reached the end";
    ASSERT_GE(stats.steps.size(), 2u);
    EXPECT_EQ(stats.steps[1].rows_examined, 6u);
}

TEST_F(InnerBuildExecTest, ACappedPrefixKeepsServingAndWalksFromTheFrozenMark) {
    // Spec §6's capped form, which is where the stopping class parts
    // company with the join's: a capped join build declines for the
    // statement, a capped prefix **freezes**. Cap 1 against five inner
    // rows: tr row 1 buckets, bob's resumed walk trips the cap on row 2
    // and the mark stops there for good, and carol's walk starts from
    // that same frozen mark rather than from the head. The reply is the
    // uncapped one, which is the only thing the cap may never change.
    ExecStats stats;
    const std::vector<std::string> rows = Run(
        "SELECT au.name FROM au WHERE EXISTS (SELECT tr.id FROM tr WHERE tr.au_id = au.id)",
        &stats, /*cap=*/1);
    EXPECT_EQ(rows, (std::vector<std::string>{"alice", "bob"}));
    const StepStats total = stats.Total();
    EXPECT_EQ(total.build_rows, 1u) << "one row bucketed, then the map is frozen";
    EXPECT_EQ(total.inner_builds, 0u)
        << "a frozen map never covers the relation, however far the walks run";
}

}  // namespace
}  // namespace kds::exec
