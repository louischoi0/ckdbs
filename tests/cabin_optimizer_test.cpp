#include "kds/stats/cabin_optimizer.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

// The decision core (physical-optimizer.md §II.4, workplan PHY02).
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
    // Both projections of the one state, exactly as `Snapshot()` fills
    // them - a fixture that set only the linear pair would hand the core
    // a snapshot no collector can produce.
    fp.frequency_log2 = Log2OfQ16(fp.frequency_q8);
    fp.pages_log2 = Log2OfQ16(fp.pages_q8);
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
    config.half_life_ns = 1000;
    // T_amort pinned at 1 half-life: these tests pin the *rules* at the
    // model's neutral unit, where B/C arithmetic reads off the snapshot
    // directly. The shipped defaults (window 64, cooldown 128 - overnight
    // survival) get their own test below at the shipped numbers.
    config.amort_windows = kFixOne;
    config.cooldown_half_lives = 2;  // cooldown = 2 x 1000 ns
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

    // Traffic dies entirely: zero frequency zeroes the benefit whatever
    // the frozen baseline says - a dead Cabin still dies - so DECAYING,
    // no action yet, pages stay, the state is recoverable.
    const auto cold = [&](std::uint64_t v, sched::MonoTimeNs epoch) {
        return Snap(v, epoch, {Fp(7, 0, 0, kRel, kCol, 42)}, {Quality(42, 1, 0)});
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

TEST(CabinOptimizerTest, AFrozenBaselineKeepsAServedCabinAlive) {
    CabinOptimizer opt(TestConfig());
    for (int i = 1; i <= 3; ++i) opt.Decide(HotSnapshot(i));
    opt.NoteCreated(kRel, kCol, 42, 5);

    // The Cabin now serves the shape, so the live mean collapses to the
    // probe cost (2 pages = p_cabin, live benefit 0). The frozen pre-Cabin
    // baseline keeps B = freq x 38 high: no decay, no drop, no churn.
    for (std::uint64_t v = 4; v < 24; ++v) {
        const ActionSet actions = opt.Decide(
            Snap(v, 0, {Fp(7, 10, 20, kRel, kCol, 42)}, {Quality(42, 10, 0)}));
        ASSERT_TRUE(actions.empty()) << "acted on its own cheapness at snapshot " << v;
    }
    EXPECT_EQ(opt.pages_committed(), 5u);

    // A dead shape still dies: zero frequency zeroes the frozen-priced
    // benefit too, and the cooldown runs to a DROP.
    EXPECT_TRUE(
        opt.Decide(Snap(24, 1000, {Fp(7, 0, 0, kRel, kCol, 42)}, {Quality(42, 1, 0)}))
            .empty());
    ActionSet dropped =
        opt.Decide(Snap(25, 4000, {Fp(7, 0, 0, kRel, kCol, 42)}, {Quality(42, 1, 0)}));
    ASSERT_EQ(dropped.size(), 1u);
    EXPECT_EQ(dropped[0].action, CabinAction::kDrop);
    EXPECT_EQ(dropped[0].reason, ActionReason::kSustainedDecay);
}

TEST(CabinOptimizerTest, TheDecisionLogCarriesTheDigestAndWraps) {
    CabinOptimizerConfig config = TestConfig();
    config.decision_log_capacity = 2;
    CabinOptimizer opt(config);

    // Three logged decisions through the quality path: CREATE (v3), HEAL
    // (v4), DROP (v5). Capacity 2 keeps the newest two, oldest first.
    for (int i = 1; i <= 3; ++i) opt.Decide(HotSnapshot(i, /*epoch=*/i * 10));
    opt.NoteCreated(kRel, kCol, 42, 5);
    const auto failing = [&](std::uint64_t v) {
        return Snap(v, v * 10, {Fp(7, 10, 400, kRel, kCol, 42)}, {Quality(42, 10, 2)});
    };
    opt.Decide(failing(4));
    opt.Decide(failing(5));

    const std::vector<DecisionRecord> log = opt.DecisionLog();
    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0].snapshot_version, 4u);
    EXPECT_EQ(log[0].decay_epoch, 40u);
    EXPECT_EQ(log[0].item.action, CabinAction::kHeal);
    EXPECT_EQ(log[1].snapshot_version, 5u);
    EXPECT_EQ(log[1].item.action, CabinAction::kDrop);
    EXPECT_EQ(log[1].item.reason, ActionReason::kQualityCollapse);
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

// The swap is the one rule that emits against a key other than the one
// being decided, so it is the one rule that can collide with the pass's own
// iteration. `TheBudgetInvariantHoldsAndSwapEvictsTheWeakest` above cannot
// see it: its victim sorts *before* the candidate and is already past. With
// the order reversed the victim was reached again while still carrying the
// DROP just emitted for it, and the DECAYING arm fired a second time - a
// `sustained-decay` DROP for the same cabin, whose cooldown had "elapsed"
// only because the swap left `decaying_since` at 0.
TEST(CabinOptimizerTest, ASwapVictimSortingAfterTheCandidateIsDroppedOnce) {
    CabinOptimizer opt(TestConfig());
    constexpr std::uint64_t kVictimRel = 5000;  // sorts AFTER the candidate
    constexpr std::uint64_t kCandRel = 4000;
    constexpr sched::MonoTimeNs kEpoch = 100'000;  // >> the 2000ns cooldown

    // The victim: hot enough to build (B = 4 x 8 = 32 > 3 x 10), and weak
    // enough afterwards to be the swap's choice.
    for (std::uint64_t v = 1; v <= 3; ++v) {
        opt.Decide(Snap(v, kEpoch, {Fp(7, 4, 40, kVictimRel, kCol)}));
    }
    opt.NoteCreated(kVictimRel, kCol, /*cabin_id=*/42, /*pages=*/5);
    ASSERT_EQ(opt.pages_committed(), 5u);

    ActionSet swap;
    for (std::uint64_t v = 4; swap.empty() && v <= 8; ++v) {
        swap = opt.Decide(Snap(v, kEpoch,
                               {Fp(7, 3, 30, kVictimRel, kCol, 42),
                                Fp(9, 20, 2000, kCandRel, kCol)},
                               {Quality(42, 10, 0)}));
    }

    ASSERT_EQ(swap.size(), 2u) << "the victim was decided twice in one pass";
    EXPECT_EQ(swap[0].action, CabinAction::kDrop);
    EXPECT_EQ(swap[0].reason, ActionReason::kBudgetSwap);
    EXPECT_EQ(swap[0].cabin_id, 42u);
    EXPECT_EQ(swap[1].action, CabinAction::kCreate);
    EXPECT_EQ(swap[1].rel_oid, kCandRel);

    // And the log says the same thing: one decision per cabin per pass, or
    // PO8's "recorded with the inputs that produced it" records a decision
    // no input produced.
    int drops_of_42 = 0;
    for (const DecisionRecord& record : opt.DecisionLog()) {
        if (record.item.action == CabinAction::kDrop && record.item.cabin_id == 42) {
            ++drops_of_42;
        }
    }
    EXPECT_EQ(drops_of_42, 1);
}

// The cooldown at a *shipped* half-life. Every other case in this file runs
// at nanosecond half-lives, where the 16.16 arithmetic has room; the
// default is 600 seconds, and lifting 6e11 nanoseconds into 16.16 fills
// half of a u64 before the amort_windows multiply even starts. The product
// saturated, and the 1200-second cooldown came out as 4.29 seconds - the
// hysteresis the θ gap buys, undone by an overflow nothing reported.
TEST(CabinOptimizerTest, TheShippedDefaultsSurviveAMarketOvernight) {
    // The operator-ratified requirement (2026-08-10, from the
    // business-days scenario): a Cabin must survive a market's
    // close-to-open gap - ~17.5 h, ~105 half-lives at the default 600 s -
    // and a *weekend* must still drop it. The shipped cooldown of 128
    // half-lives is 76,800 s = 21 h 20 m, so the night is covered before
    // the log2(B/C) margin is even counted, and 48 h of silence is
    // comfortably past it.
    //
    // The cooldown is its own parameter since 2026-08-10 and this test is
    // what stops it drifting below the requirement: 128 half-lives is not
    // a tuning preference, it is "longer than one night" with margin.
    CabinOptimizerConfig config;  // the shipped defaults, deliberately
    constexpr sched::MonoTimeNs kSecond = 1'000'000'000ULL;
    ASSERT_EQ(config.half_life_ns, 600 * kSecond);
    ASSERT_EQ(config.amort_windows, 64 * kFixOne);
    ASSERT_EQ(config.cooldown_half_lives, 128u);
    CabinOptimizer opt(config);

    for (std::uint64_t v = 1; v <= 3; ++v) opt.Decide(HotSnapshot(v, /*epoch=*/kSecond));
    opt.NoteCreated(kRel, kCol, 42, 5);

    const auto cold = [&](std::uint64_t v, sched::MonoTimeNs epoch) {
        return Snap(v, epoch, {Fp(7, 0, 0, kRel, kCol, 42)}, {Quality(42, 1, 0)});
    };
    // Traffic dies at the close: DECAYING from here, no action.
    ASSERT_TRUE(opt.Decide(cold(4, kSecond)).empty());

    // The whole overnight passes in silence - 17.5 h after the close. A
    // DROP here is the nightly rebuild loop the default exists to end.
    EXPECT_TRUE(opt.Decide(cold(5, (1 + 63'000) * kSecond)).empty())
        << "dropped during the overnight the default was sized to survive";

    // The morning rebound: the same traffic that earned the CREATE finds
    // the entry DECAYING and recovers it - no rebuild, no action.
    ASSERT_TRUE(opt.Decide(HotSnapshot(6, (1 + 63'060) * kSecond)).empty());
    // And it decays again when traffic dies again.
    ASSERT_TRUE(opt.Decide(cold(7, (1 + 63'120) * kSecond)).empty());

    // A weekend of silence is past the cooldown: the DROP the model still
    // owes - two silent days of staleness is what DROP is for.
    const ActionSet dropped = opt.Decide(cold(8, (1 + 63'120 + 172'800) * kSecond));
    ASSERT_EQ(dropped.size(), 1u);
    EXPECT_EQ(dropped[0].action, CabinAction::kDrop);
    EXPECT_EQ(dropped[0].reason, ActionReason::kSustainedDecay);
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

// ---- What the log-domain score buys (2026-08-10) -------------------------

namespace {

// Runs one Cabin at `freq` executions x `pages` pages until it is ACTIVE,
// then goes silent and returns how many half-lives it took to leave
// ACTIVE for DECAYING. One shape, one relation, so nothing interferes.
std::uint64_t HalfLivesUntilDecaying(std::uint32_t freq, std::uint32_t pages,
                                     std::uint64_t amort = 1) {
    CabinOptimizerConfig config = TestConfig();
    config.amort_windows = amort * kFixOne;
    CabinOptimizer opt(config);
    const sched::MonoTimeNs hl = config.half_life_ns;

    std::uint64_t version = 0;
    const auto hot = [&](sched::MonoTimeNs epoch) {
        return Snap(++version, epoch, {Fp(7, freq, pages)});
    };
    for (int i = 0; i < 4; ++i) (void)opt.Decide(hot(hl));
    opt.NoteCreated(kRel, kCol, /*cabin_id=*/42, /*pages=*/1);

    // Silence: the snapshot keeps reporting the same *state*, decayed by
    // the epoch, which is what a real collector does for a shape nobody
    // executes. Frequency and pages decay together, the mean does not.
    for (std::uint64_t n = 1; n <= 200; ++n) {
        SnapshotFingerprint fp = Fp(7, freq, pages);
        // Decay both halves by n half-lives, linear and log alike.
        fp.frequency_q8 = n >= 24 ? 0 : fp.frequency_q8 >> n;
        fp.pages_q8 = n >= 24 ? 0 : fp.pages_q8 >> n;
        fp.frequency_log2 -= static_cast<Log2Q16>(n) << 16;
        fp.pages_log2 -= static_cast<Log2Q16>(n) << 16;
        (void)opt.Decide(Snap(++version, hl + n * hl, {fp}, {Quality(42, 0, 0)}));
        for (const ManagedEntryView& entry : opt.ManagedEntries()) {
            if (std::string(entry.state) == "DECAYING") return n;
        }
    }
    return 0;  // never decayed
}

}  // namespace

TEST(CabinOptimizerTest, TheOnsetIsNotCappedByTheScoreWidth) {
    // What the log domain actually removes, stated exactly. The DECAYING
    // onset should arrive at log2(B/C) half-lives of silence, which grows
    // with the frequency, the saved-pages factor **and** T_amort. Q24.8
    // caps it instead at log2(frequency x 256) - the score's own bit
    // width - because past that every read is 0 and the comparison goes
    // vacuous. The cap sits about 1.3 half-lives above the onset at the
    // shipped T_amort = 64, so it is a cliff one doubling away rather
    // than a live defect; at 512 it bites, which is where this pins it.
    //
    // freq = 4 scales to 1024, so the linear score reads zero from the
    // 11th half-life; the honest onset at T_amort = 512 is ~11.7, i.e.
    // the 12th. One half-life apart, and that single step is the whole
    // difference between a verdict and a running-out-of-digits - pinned
    // exactly, because a looser bound passes against the old code.
    const std::uint64_t onset = HalfLivesUntilDecaying(/*freq=*/4, /*pages=*/40, /*amort=*/512);
    ASSERT_GT(onset, 0u) << "the Cabin never decayed";
    EXPECT_GE(onset, 12u)
        << "the onset stopped at the score's bit width (" << onset
        << " half-lives), so a wider amortization window bought nothing";

    // And it still tracks the evidence: 1024x the frequency is 10 more
    // half-lives of silence before the same verdict.
    const std::uint64_t bigger =
        HalfLivesUntilDecaying(/*freq=*/4096, /*pages=*/40960, /*amort=*/512);
    ASSERT_GT(bigger, 0u);
    EXPECT_NEAR(static_cast<double>(bigger) - static_cast<double>(onset), 10.0, 1.5);
}

TEST(CabinOptimizerTest, TheSwapEvictsTheColdestEvenWhenBothHaveUnderflowed) {
    // Victim selection used to read the linear benefit, so two incumbents
    // decayed past Q24.8's floor tied at zero and the swap took whichever
    // sorted first by key - an arbitrary Cabin, not the coldest. Ordering
    // in the log domain restores the intent.
    // The warm incumbent sorts **first** by key, deliberately: a scan that
    // ties at zero keeps whichever it met first, so this arrangement is
    // the one where the linear comparison evicts the wrong Cabin and the
    // log comparison does not. Reversed, both implementations agree and
    // the test proves nothing.
    constexpr std::uint64_t kWarmRel = 4000;
    constexpr std::uint64_t kColdRel = 4002;
    constexpr std::uint64_t kNewRel = 4004;
    constexpr sched::MonoTimeNs kEpoch = 100'000;

    CabinOptimizerConfig config = TestConfig();
    config.page_budget = 2;        // room for exactly the two incumbents
    config.amort_windows = 4096 * kFixOne;  // see below
    CabinOptimizer opt(config);

    constexpr std::uint64_t kWarmCabin = 92;
    constexpr std::uint64_t kColdCabin = 91;

    // A wide amortization window is what makes this state reachable at
    // all: an incumbent stays ACTIVE while its *linear* score reads zero
    // only when the cost floor it is measured against is itself tiny.
    // That is the same corner the onset cap lives in, approached from the
    // other side.
    //
    // Two incumbents, differing in what they were worth when built: the
    // warm one saves 8 pages a scan, the cold one 4. They are **created
    // by the controller**, not injected, because the frozen baseline the
    // comparison rests on is written by the CREATE decision - an injected
    // NoteCreated leaves it zero and both entries collapse together, for
    // an entirely different reason than the one under test.
    for (std::uint64_t v = 1; v <= 4; ++v) {
        (void)opt.Decide(Snap(v, 0, {Fp(7, 10, 80, kWarmRel, kCol),
                                     Fp(8, 10, 40, kColdRel, kCol)}));
    }
    opt.NoteCreated(kWarmRel, kCol, kWarmCabin, /*pages=*/1);
    opt.NoteCreated(kColdRel, kCol, kColdCabin, /*pages=*/1);
    ASSERT_EQ(opt.pages_committed(), 2u);

    // Now both idle past the linear floor - `frequency_q8` zero is exactly
    // what the collector reports there - at a frequency (2^-9 executions)
    // that still clears each one's own drop threshold. Their benefits
    // differ by the frozen saving, tenfold, and the linear form cannot
    // see it: both read 0.
    const auto faded = [&](std::uint64_t pattern, std::uint64_t rel, std::uint64_t cabin,
                           Log2Q16 mean_log) {
        SnapshotFingerprint fp = Fp(pattern, 10, 400, rel, kCol, cabin);
        fp.frequency_q8 = 0;
        fp.pages_q8 = 0;
        fp.frequency_log2 = -(Log2Q16{1} << 16);  // scaled 0.5: real 2^-9
        fp.pages_log2 = fp.frequency_log2 + mean_log;  // the mean is decay-invariant
        return fp;
    };
    for (std::uint64_t v = 5; v <= 12; ++v) {
        const ActionSet actions = opt.Decide(
            Snap(v, kEpoch,
                 {faded(7, kWarmRel, kWarmCabin, /*mean_log=*/Log2Q16{3} << 16),
                  faded(8, kColdRel, kColdCabin, /*mean_log=*/Log2Q16{2} << 16),
                  Fp(9, 400, 16000, kNewRel, kCol)},
                 {Quality(kWarmCabin, 0, 0), Quality(kColdCabin, 0, 0)}));
        for (const ActionItem& a : actions) {
            if (a.action != CabinAction::kDrop) continue;
            EXPECT_EQ(a.cabin_id, kColdCabin)
                << "the swap evicted the warmer incumbent (cabin " << a.cabin_id
                << "), which is what a tie at zero broken by key order does";
            EXPECT_EQ(a.reason, ActionReason::kBudgetSwap);
            return;
        }
    }
    FAIL() << "the budget swap never fired";
}

TEST(CabinOptimizerTest, TheCooldownIsSettableBelowTheAmortizationWindow) {
    // The capability decoupling adds (2026-08-10): the DECAYING dwell can
    // now be *shorter* than the amortization window, which `2 x T_amort`
    // made unrepresentable. The case for it is a workload with no quiet
    // period, where waiting a night to prove death buys nothing.
    //
    // Note what this does and does not fix. It retires a dead Cabin
    // promptly - but only because this configuration asserts that silence
    // is immediately meaningful. Where a real quiet period exists, the
    // same setting retires *live* Cabins, which is the nightly rebuild
    // loop returning; the floor is a property of the workload and not
    // something the engine can check, so it is documented rather than
    // enforced.
    CabinOptimizerConfig config;
    config.page_budget = 6;
    config.half_life_ns = 1000;
    config.amort_windows = 64 * kFixOne;   // a long belief about lifetime
    config.cooldown_half_lives = 4;        // and little patience: 4000 ns
    CabinOptimizer opt(config);

    // The wide window lowers the cost floor 64x, so this shape clears the
    // bar in the same three snapshots.
    for (std::uint64_t v = 1; v <= 3; ++v) opt.Decide(HotSnapshot(v, /*epoch=*/1000));
    opt.NoteCreated(kRel, kCol, 42, 5);

    const auto cold = [&](std::uint64_t v, sched::MonoTimeNs epoch) {
        return Snap(v, epoch, {Fp(7, 0, 0, kRel, kCol, 42)}, {Quality(42, 1, 0)});
    };
    ASSERT_TRUE(opt.Decide(cold(4, 1000)).empty());  // DECAYING

    // Inside the 4000 ns dwell: still held.
    EXPECT_TRUE(opt.Decide(cold(5, 3000)).empty());

    // Past it - and well short of the 128,000 ns the old expression would
    // have imposed at this window.
    const ActionSet dropped = opt.Decide(cold(6, 6000));
    ASSERT_EQ(dropped.size(), 1u);
    EXPECT_EQ(dropped[0].action, CabinAction::kDrop);
    EXPECT_EQ(dropped[0].reason, ActionReason::kSustainedDecay);
}

TEST(CabinOptimizerTest, NoteExtendedMovesThePageAccounting) {
    // PHY06's completion edge: an EXTEND grows the sets, so the budget's
    // accounting must move with it - and it is a *total*, so a repeated
    // report is idempotent rather than compounding.
    CabinOptimizer opt(TestConfig());
    for (std::uint64_t v = 1; v <= 3; ++v) (void)opt.Decide(HotSnapshot(v));
    opt.NoteCreated(kRel, kCol, /*cabin_id=*/42, /*pages=*/3);
    EXPECT_EQ(opt.pages_committed(), 3u);

    opt.NoteExtended(/*cabin_id=*/42, /*pages=*/5);
    EXPECT_EQ(opt.pages_committed(), 5u);
    opt.NoteExtended(/*cabin_id=*/42, /*pages=*/5);
    EXPECT_EQ(opt.pages_committed(), 5u) << "a repeated total compounded";

    // An unknown id is NoteDropped's no-op: the entry left the accounting
    // with its pages, and there is nothing to update.
    opt.NoteExtended(/*cabin_id=*/999, /*pages=*/50);
    EXPECT_EQ(opt.pages_committed(), 5u);
}

TEST(CabinOptimizerTest, ManagedEntriesExposeStateAndLastScores) {
    // PHY06's view: the projection carries the key, the lifecycle state,
    // and the *last* Decide pass's B/C - stamped every pass, so a quiet
    // entry still shows the numbers keeping it quiet.
    CabinOptimizer opt(TestConfig());
    (void)opt.Decide(HotSnapshot(1));

    std::vector<ManagedEntryView> entries = opt.ManagedEntries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].rel_oid, kRel);
    EXPECT_EQ(entries[0].col_pos, kCol);
    EXPECT_STREQ(entries[0].state, "CANDIDATE");
    EXPECT_EQ(entries[0].cabin_id, 0u);
    EXPECT_EQ(entries[0].confirm_streak, 1u);
    EXPECT_EQ(entries[0].benefit, 380 * kFixOne);
    EXPECT_EQ(entries[0].cost, 40 * kFixOne);

    for (std::uint64_t v = 2; v <= 3; ++v) (void)opt.Decide(HotSnapshot(v));
    opt.NoteCreated(kRel, kCol, /*cabin_id=*/42, /*pages=*/3);
    entries = opt.ManagedEntries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_STREQ(entries[0].state, "ACTIVE");
    EXPECT_EQ(entries[0].cabin_id, 42u);
    EXPECT_EQ(entries[0].pages, 3u);
}

}  // namespace
}  // namespace kds::stats
