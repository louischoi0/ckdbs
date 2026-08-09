#include "kds/stats/cabin_optimizer.hpp"

#include <vector>

#include <gtest/gtest.h>

// The decision core (feat-physical-optimizer.md §II.4, workplan PHY02).
// Four properties carry the acceptance: golden ActionSets for scripted
// snapshot sequences, hysteresis (no oscillation inside the θ gap), the
// budget invariant (Σ pages ≤ budget after every executed ActionSet), and
// arithmetic determinism (identical streams ⇒ bit-identical traces).

namespace kds::stats {
namespace {

constexpr std::uint64_t kRel = 4001;
constexpr std::uint16_t kCol = 1;

SnapshotFingerprint Fp(std::uint64_t pattern, std::uint32_t freq_points,
                       std::uint32_t page_points, std::uint64_t rel = kRel,
                       std::uint16_t col = kCol, std::uint64_t cabin_id = 0) {
    SnapshotFingerprint fp;
    fp.pattern_id = pattern;
    fp.frequency_q8 = freq_points * kDecayScoreScale;
    fp.pages_q8 = page_points * kDecayScoreScale;
    fp.candidate = CandidateRef{rel, col, cabin_id};
    return fp;
}

SnapshotCabin Quality(std::uint64_t cabin_id, std::uint32_t lookups, std::uint32_t failures,
                      std::uint32_t coverage = 0) {
    SnapshotCabin c;
    c.cabin_id = cabin_id;
    c.lookups_q8 = lookups * kDecayScoreScale;
    c.hint_failures_q8 = failures * kDecayScoreScale;
    c.coverage_misses_q8 = coverage * kDecayScoreScale;
    return c;
}

OptimizerSnapshot Snap(std::uint64_t version, sched::MonoTimeNs epoch,
                       std::vector<SnapshotFingerprint> fps,
                       std::vector<SnapshotCabin> cabins = {}) {
    OptimizerSnapshot s;
    s.version = version;
    s.decay_epoch = epoch;
    s.fingerprints = std::move(fps);
    s.cabins = std::move(cabins);
    return s;
}

CabinOptimizerConfig TestConfig() {
    CabinOptimizerConfig config;
    config.page_budget = 6;
    config.half_life_ns = 1000;  // cooldown = 2 x amort x half-life = 2000
    return config;
}

// The scripted hot shape: 10 executions x 40 pages mean. B = 10 x 38 = 380,
// C = P_rel / 1 window = 40, so B > theta_create x C (120) - a CREATE after
// exactly N_confirm snapshots.
OptimizerSnapshot HotSnapshot(std::uint64_t version, sched::MonoTimeNs epoch = 0) {
    return Snap(version, epoch, {Fp(/*pattern=*/7, /*freq=*/10, /*pages=*/400)});
}

TEST(CabinOptimizerTest, GoldenCreateAfterTheConfirmStreak) {
    CabinOptimizer opt(TestConfig());

    EXPECT_TRUE(opt.Decide(HotSnapshot(1)).empty());
    EXPECT_TRUE(opt.Decide(HotSnapshot(2)).empty());

    const ActionSet third = opt.Decide(HotSnapshot(3));
    ASSERT_EQ(third.size(), 1u);
    EXPECT_EQ(third[0].action, CabinAction::kCreate);
    EXPECT_EQ(third[0].reason, ActionReason::kSustainedBenefit);
    EXPECT_EQ(third[0].rel_oid, kRel);
    EXPECT_EQ(third[0].col_pos, kCol);
    EXPECT_EQ(third[0].cabin_id, 0u);
    EXPECT_EQ(third[0].benefit, 380 * kFixOne);
    EXPECT_EQ(third[0].cost, 40 * kFixOne);

    // BUILDING: the estimate (P_rel / divisor = 40/8) is committed, and no
    // further decision fires while the build is Execute's.
    EXPECT_EQ(opt.pages_committed(), 5u);
    EXPECT_TRUE(opt.Decide(HotSnapshot(4)).empty());
}

TEST(CabinOptimizerTest, AStreakBrokenMidwayStartsOver) {
    CabinOptimizer opt(TestConfig());
    EXPECT_TRUE(opt.Decide(HotSnapshot(1)).empty());
    EXPECT_TRUE(opt.Decide(HotSnapshot(2)).empty());
    // One cold snapshot resets the confirm streak (N_confirm is
    // *consecutive*), so two more hot ones do not create - three do.
    EXPECT_TRUE(opt.Decide(Snap(3, 0, {Fp(7, 1, 4)})).empty());
    EXPECT_TRUE(opt.Decide(HotSnapshot(4)).empty());
    EXPECT_TRUE(opt.Decide(HotSnapshot(5)).empty());
    EXPECT_EQ(opt.Decide(HotSnapshot(6)).size(), 1u);
}

TEST(CabinOptimizerTest, HysteresisHoldsInsideTheThetaGap) {
    CabinOptimizer opt(TestConfig());
    for (int i = 1; i <= 3; ++i) opt.Decide(HotSnapshot(i));
    opt.NoteCreated(kRel, kCol, /*cabin_id=*/42, /*pages=*/5);

    // A stationary noisy workload: the mean jitters between 30 and 50
    // pages, so B swings 280..480 while theta_drop x C stays ~15..25 and
    // quality stays clean - every reading is inside the wide gap
    // theta_drop << 1 << theta_create is built for. Zero actions, ever.
    for (int i = 0; i < 100; ++i) {
        const bool high = (i % 2) == 0;
        const ActionSet actions = opt.Decide(
            Snap(4 + i, 0, {Fp(7, 10, high ? 500 : 300, kRel, kCol, /*cabin_id=*/42)},
                 {Quality(42, /*lookups=*/10, /*failures=*/0)}));
        ASSERT_TRUE(actions.empty()) << "oscillated at snapshot " << i;
    }
}

TEST(CabinOptimizerTest, TheBudgetInvariantHoldsAndSwapEvictsTheWeakest) {
    CabinOptimizer opt(TestConfig());  // budget 6, estimates of 5 each

    // A becomes ACTIVE at 5 pages.
    for (int i = 1; i <= 3; ++i) opt.Decide(HotSnapshot(i));
    opt.NoteCreated(kRel, kCol, 42, 5);
    ASSERT_EQ(opt.pages_committed(), 5u);

    // B, weaker than the swap bar (theta_swap x B_A = 760): confirmed
    // three times and still refused - the candidate waits, the budget
    // holds.
    const auto weaker = [&](std::uint64_t v) {
        return Snap(v, 0,
                    {Fp(7, 10, 400, kRel, kCol, 42), Fp(8, 10, 300, kRel + 1, kCol)},
                    {Quality(42, 10, 0)});
    };
    for (std::uint64_t v = 4; v <= 7; ++v) {
        for (const ActionItem& action : opt.Decide(weaker(v))) {
            EXPECT_NE(action.action, CabinAction::kCreate) << "an over-budget CREATE fired";
        }
        EXPECT_LE(opt.pages_committed(), 6u);
    }

    // C, past the swap bar (B_C = 20 x 98 = 1960 > 2 x 380): the weakest
    // ACTIVE is dropped *first in the set*, then the CREATE - so executing
    // the set in order lands back under budget.
    const auto stronger = [&](std::uint64_t v) {
        return Snap(v, 0,
                    {Fp(7, 10, 400, kRel, kCol, 42), Fp(9, 20, 2000, kRel + 2, kCol)},
                    {Quality(42, 10, 0)});
    };
    ActionSet swap;
    for (std::uint64_t v = 8; swap.empty() && v <= 10; ++v) swap = opt.Decide(stronger(v));
    ASSERT_EQ(swap.size(), 2u);
    EXPECT_EQ(swap[0].action, CabinAction::kDrop);
    EXPECT_EQ(swap[0].reason, ActionReason::kBudgetSwap);
    EXPECT_EQ(swap[0].cabin_id, 42u);
    EXPECT_EQ(swap[1].action, CabinAction::kCreate);
    EXPECT_EQ(swap[1].rel_oid, kRel + 2);

    opt.NoteDropped(42);
    opt.NoteCreated(kRel + 2, kCol, 43, 5);
    EXPECT_LE(opt.pages_committed(), 6u);
}

TEST(CabinOptimizerTest, QualityHealsOnceThenCollapses) {
    CabinOptimizer opt(TestConfig());
    for (int i = 1; i <= 3; ++i) opt.Decide(HotSnapshot(i));
    opt.NoteCreated(kRel, kCol, 42, 5);

    // 20% hint failures: above theta_heal while B stays worth it -> HEAL.
    const auto failing = [&](std::uint64_t v) {
        return Snap(v, 0, {Fp(7, 10, 400, kRel, kCol, 42)}, {Quality(42, 10, 2)});
    };
    ActionSet first = opt.Decide(failing(4));
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].action, CabinAction::kHeal);
    EXPECT_EQ(first[0].reason, ActionReason::kQualityHeal);

    // Still failing after the heal: PO7 says discard-and-reobserve, never
    // repair at any cost.
    ActionSet second = opt.Decide(failing(5));
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].action, CabinAction::kDrop);
    EXPECT_EQ(second[0].reason, ActionReason::kQualityCollapse);
}

TEST(CabinOptimizerTest, RecoveredQualityReArmsTheHeal) {
    CabinOptimizer opt(TestConfig());
    for (int i = 1; i <= 3; ++i) opt.Decide(HotSnapshot(i));
    opt.NoteCreated(kRel, kCol, 42, 5);

    ASSERT_EQ(opt.Decide(Snap(4, 0, {Fp(7, 10, 400, kRel, kCol, 42)}, {Quality(42, 10, 2)}))
                  .size(),
              1u);  // HEAL
    // Quality recovers: no action, and the heal re-arms.
    ASSERT_TRUE(
        opt.Decide(Snap(5, 0, {Fp(7, 10, 400, kRel, kCol, 42)}, {Quality(42, 10, 0)}))
            .empty());
    // A later failure heals again rather than collapsing.
    ActionSet again =
        opt.Decide(Snap(6, 0, {Fp(7, 10, 400, kRel, kCol, 42)}, {Quality(42, 10, 2)}));
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again[0].action, CabinAction::kHeal);
}

TEST(CabinOptimizerTest, DecayRunsTheCooldownAndRecoveryCancelsIt) {
    CabinOptimizer opt(TestConfig());
    for (int i = 1; i <= 3; ++i) opt.Decide(HotSnapshot(i));
    opt.NoteCreated(kRel, kCol, 42, 5);

    // B collapses to 1 x 1 = 1 against theta_drop x C = 1.5: DECAYING, no
    // action yet - pages stay, the state is recoverable.
    const auto cold = [&](std::uint64_t v, sched::MonoTimeNs epoch) {
        return Snap(v, epoch, {Fp(7, 1, 3, kRel, kCol, 42)}, {Quality(42, 1, 0)});
    };
    EXPECT_TRUE(opt.Decide(cold(4, 1000)).empty());

    // Recovery on rebound: back to ACTIVE without ever dropping.
    EXPECT_TRUE(opt.Decide(HotSnapshot(5)).empty());

    // Decay again and sit through the whole cooldown (2 half-lives): DROP.
    EXPECT_TRUE(opt.Decide(cold(6, 5000)).empty());
    EXPECT_TRUE(opt.Decide(cold(7, 6000)).empty());
    ActionSet dropped = opt.Decide(cold(8, 7000));
    ASSERT_EQ(dropped.size(), 1u);
    EXPECT_EQ(dropped[0].action, CabinAction::kDrop);
    EXPECT_EQ(dropped[0].reason, ActionReason::kSustainedDecay);
    EXPECT_EQ(dropped[0].cabin_id, 42u);
}

TEST(CabinOptimizerTest, JurisdictionIgnoresUnownedCabins) {
    CabinOptimizer opt(TestConfig());

    // A shape an existing, foreign Cabin already serves: however hot, it
    // is not a candidate, and the foreign Cabin's quality is not this
    // controller's to act on (PO1).
    for (std::uint64_t v = 1; v <= 10; ++v) {
        const ActionSet actions = opt.Decide(
            Snap(v, 0, {Fp(7, 50, 5000, kRel, kCol, /*cabin_id=*/99)},
                 {Quality(99, 50, 25)}));
        ASSERT_TRUE(actions.empty()) << "acted on an unowned Cabin at snapshot " << v;
    }
    EXPECT_EQ(opt.pages_committed(), 0u);
}

TEST(CabinOptimizerTest, IdenticalStreamsProduceBitIdenticalTraces) {
    const auto run = [] {
        CabinOptimizer opt(TestConfig());
        std::vector<ActionItem> trace;
        for (std::uint64_t v = 1; v <= 40; ++v) {
            OptimizerSnapshot s =
                v < 20 ? HotSnapshot(v, v * 100)
                       : Snap(v, v * 100, {Fp(7, 1, 3, kRel, kCol, 42)}, {Quality(42, 1, 0)});
            for (const ActionItem& a : opt.Decide(s)) {
                trace.push_back(a);
                if (a.action == CabinAction::kCreate) opt.NoteCreated(kRel, kCol, 42, 5);
                if (a.action == CabinAction::kDrop) opt.NoteDropped(a.cabin_id);
            }
        }
        return trace;
    };

    const std::vector<ActionItem> a = run();
    const std::vector<ActionItem> b = run();
    ASSERT_EQ(a.size(), b.size());
    ASSERT_FALSE(a.empty()) << "the stream drove no decisions; determinism proved nothing";
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].action, b[i].action) << i;
        EXPECT_EQ(a[i].reason, b[i].reason) << i;
        EXPECT_EQ(a[i].cabin_id, b[i].cabin_id) << i;
        EXPECT_EQ(a[i].rel_oid, b[i].rel_oid) << i;
        EXPECT_EQ(a[i].col_pos, b[i].col_pos) << i;
        EXPECT_EQ(a[i].benefit, b[i].benefit) << i;
        EXPECT_EQ(a[i].cost, b[i].cost) << i;
    }
}

}  // namespace
}  // namespace kds::stats
