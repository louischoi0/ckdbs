#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/stats/cabin_optimizer.hpp"
#include "kds/stats/optimizer_signals.hpp"

// PHY07 - the deterministic replay harness (PO10).
//
// Seed-driven statistics streams are replayed through the *real* pipeline:
// generator events feed PHY01's `OptimizerSignals` under a ManualClock,
// each period snapshots and runs PHY02's `Decide`, and a simulated Execute
// applies the completion edges. The resulting action traces are compared
// against a golden file checked in beside the parser corpus - and the
// stability of that file across runs and platforms is itself the
// acceptance: everything on this path is integer arithmetic (splitmix64,
// the Q24.8 decay, the 16.16 core), so a platform where the trace moves
// has broken fixed-point determinism somewhere.
//
// Four scenarios, §II's drift vocabulary:
//   rising-star        a shape's frequency ramps -> CREATE, exactly once
//   fading-star        a built Cabin's traffic dies -> DECAYING -> DROP
//   stationary-noise   jitter inside the θ gap -> zero actions, ever
//   quality-collapse   hint failures spike -> HEAL, then DROP (PO7)
//
// Every scenario also carries a *foreign* Cabin (id 999) with miserable
// quality, and the jurisdiction oracle asserts it is never acted on.
//
// Regeneration: KDS_TRACE_REGEN=1 writes <traces>.regen with the actual
// traces; inspect the diff and replace the golden file deliberately - the
// parser corpus's arrangement.

namespace kds::stats {
namespace {

constexpr sched::MonoTimeNs kHalfLife = 1'000'000ULL;  // 1 ms: compact epochs
constexpr std::uint64_t kForeignCabin = 999;

// Portable seeded PRNG: splitmix64, chosen over <random> because the
// standard's *distributions* are implementation-defined and this file's
// whole point is byte-identical traces on every platform.
struct SplitMix64 {
    std::uint64_t state;
    std::uint64_t Next() {
        state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    // [lo, hi], via modulo: biased in general and fine here - the jitter
    // only has to be deterministic, not uniform.
    std::uint64_t Range(std::uint64_t lo, std::uint64_t hi) {
        return lo + Next() % (hi - lo + 1);
    }
};

// One period's synthetic traffic for the one interesting shape.
struct Period {
    std::uint32_t executions = 0;
    std::uint64_t pages_per_execution = 0;
    std::uint32_t cabin_lookups = 0;
    std::uint32_t hint_failures = 0;
};

struct Scenario {
    const char* name;
    std::uint64_t seed;
    std::uint64_t pattern_id;
    std::uint64_t rel_oid;
    std::vector<Period> periods;
};

// The generators. Each is a pure function of the seed, and the numbers
// are chosen against TestConfig()'s thresholds: mean 40 pages puts
// θ_drop×C ≈ 20 and θ_create×C ≈ 120, so a decayed frequency of ~2 sits
// mid-gap and ~10 clears the create bar.
Scenario RisingStar(std::uint64_t seed) {
    Scenario s{"rising-star", seed, 71, 4101, {}};
    SplitMix64 rng{seed};
    for (int step = 0; step < 12; ++step) {
        Period p;
        p.executions = static_cast<std::uint32_t>(step);  // the ramp
        p.pages_per_execution = rng.Range(38, 42);
        s.periods.push_back(p);
    }
    return s;
}

Scenario FadingStar(std::uint64_t seed) {
    Scenario s{"fading-star", seed, 72, 4102, {}};
    SplitMix64 rng{seed};
    for (int step = 0; step < 8; ++step) {  // hot: builds the Cabin
        s.periods.push_back(Period{10, rng.Range(38, 42), 0, 0});
    }
    for (int step = 0; step < 10; ++step) {  // dead: decays through cooldown
        s.periods.push_back(Period{0, 0, 0, 0});
    }
    return s;
}

Scenario StationaryNoise(std::uint64_t seed) {
    Scenario s{"stationary-noise", seed, 73, 4103, {}};
    SplitMix64 rng{seed};
    for (int step = 0; step < 60; ++step) {
        // One execution per period at a jittering page cost: the decayed
        // frequency settles near 2, so B wanders ~56..96 - inside the gap
        // by construction, which is what "stationary" means to the rules.
        s.periods.push_back(Period{1, rng.Range(30, 50), 0, 0});
    }
    return s;
}

Scenario ServedCheaply(std::uint64_t seed) {
    // The frozen-baseline case (PHY03): after the CREATE, the Cabin serves
    // the shape and the *observed* page cost collapses to the probe cost -
    // because the Cabin is working. A controller pricing the live cheapness
    // would drop its own success and churn; the frozen pre-Cabin baseline
    // keeps it ACTIVE, and this scenario's oracle is "exactly one action,
    // ever".
    Scenario s{"served-cheaply", seed, 75, 4105, {}};
    SplitMix64 rng{seed};
    for (int step = 0; step < 8; ++step) {  // hot and expensive: builds
        s.periods.push_back(Period{10, rng.Range(38, 42), 0, 0});
    }
    for (int step = 0; step < 12; ++step) {  // hot and cheap: served
        s.periods.push_back(Period{10, 2, 10, 0});
    }
    return s;
}

Scenario QualityCollapse(std::uint64_t seed) {
    Scenario s{"quality-collapse", seed, 74, 4104, {}};
    SplitMix64 rng{seed};
    for (int step = 0; step < 8; ++step) {  // hot and clean: builds
        s.periods.push_back(Period{10, rng.Range(38, 42), 10, 0});
    }
    for (int step = 0; step < 6; ++step) {  // the bulk update's aftermath
        s.periods.push_back(Period{10, rng.Range(38, 42), 10, 3});
    }
    return s;
}

// Runs one scenario through the real collector and core, applying the
// completion edges a real Execute would. Returns the rendered trace and
// checks the scenario-independent oracles as it goes.
std::vector<std::string> Replay(const Scenario& scenario) {
    sched::ManualClock clock(1);
    OptimizerSignals signals(&clock, kHalfLife);
    CabinOptimizerConfig config;
    config.half_life_ns = kHalfLife;
    config.page_budget = 64;
    // T_amort and the cooldown pinned at the values the golden traces
    // were recorded under - the drift scenarios' decay/drop timings are
    // part of what they pin, so both must be stated rather than
    // inherited from a default that may move. The shipped sizing has its
    // own test (cabin_optimizer_test's overnight case).
    config.amort_windows = kFixOne;
    config.cooldown_half_lives = 2;
    CabinOptimizer optimizer(config);

    std::vector<std::string> trace;
    std::uint64_t next_cabin_id = 100;
    std::uint64_t owned_cabin = 0;  // the scenario shape's Cabin, once built

    for (std::size_t step = 0; step < scenario.periods.size(); ++step) {
        const Period& period = scenario.periods[step];

        for (std::uint32_t i = 0; i < period.executions; ++i) {
            signals.NoteExecution(
                scenario.pattern_id, period.pages_per_execution,
                CandidateRef{scenario.rel_oid, /*col_pos=*/1, owned_cabin});
        }
        for (std::uint32_t i = 0; i < period.cabin_lookups && owned_cabin != 0; ++i) {
            signals.NoteCabinLookup(owned_cabin, /*served=*/true);
            signals.NoteCabinHint(owned_cabin, /*ok=*/i >= period.hint_failures);
        }
        // The foreign Cabin: a hot shape someone else's Cabin serves, with
        // terrible quality. The jurisdiction oracle below is what keeps
        // this from ever mattering.
        signals.NoteExecution(scenario.pattern_id + 1000, 500,
                              CandidateRef{scenario.rel_oid + 1000, 1, kForeignCabin});
        signals.NoteCabinLookup(kForeignCabin, /*served=*/false);
        signals.NoteCabinHint(kForeignCabin, /*ok=*/false);

        const OptimizerSnapshot snapshot = signals.Snapshot();
        const ActionSet actions = optimizer.Decide(snapshot);

        for (const ActionItem& action : actions) {
            // Oracle: jurisdiction. No action ever targets a Cabin this
            // controller did not create (Bound Cabins cannot even appear -
            // the snapshot is fed by Observational counting alone).
            EXPECT_NE(action.cabin_id, kForeignCabin)
                << scenario.name << " acted on the foreign cabin at step " << step;

            std::ostringstream line;
            line << scenario.name << " step=" << step << " "
                 << CabinActionName(action.action) << " reason="
                 << ActionReasonName(action.reason) << " rel=" << action.rel_oid
                 << " col=" << action.col_pos << " cabin=" << action.cabin_id
                 << " benefit=" << action.benefit << " cost=" << action.cost;
            trace.push_back(line.str());

            // The simulated Execute: deterministic ids and page counts.
            if (action.action == CabinAction::kCreate) {
                owned_cabin = next_cabin_id++;
                optimizer.NoteCreated(action.rel_oid, action.col_pos, owned_cabin,
                                      /*pages=*/4);
            } else if (action.action == CabinAction::kDrop) {
                optimizer.NoteDropped(action.cabin_id);
                if (action.cabin_id == owned_cabin) owned_cabin = 0;
            }
        }

        // Oracle: the budget invariant, after every executed ActionSet.
        EXPECT_LE(optimizer.pages_committed(), config.page_budget)
            << scenario.name << " step " << step;

        clock.Advance(kHalfLife);
    }
    return trace;
}

std::vector<Scenario> Scenarios() {
    return {RisingStar(0xA1), FadingStar(0xB2), StationaryNoise(0xC3),
            QualityCollapse(0xD4), ServedCheaply(0xE5)};
}

// ---- The scenario-specific oracles ---------------------------------------

TEST(CabinOptimizerReplayTest, RisingStarCreatesExactlyOnce) {
    const std::vector<std::string> trace = Replay(RisingStar(0xA1));
    int creates = 0;
    for (const std::string& line : trace) {
        if (line.find(" CREATE ") != std::string::npos) ++creates;
    }
    EXPECT_EQ(creates, 1) << "a ramp is sustained evidence exactly once";
}

TEST(CabinOptimizerReplayTest, FadingStarDecaysThroughTheCooldownToADrop) {
    const std::vector<std::string> trace = Replay(FadingStar(0xB2));
    ASSERT_FALSE(trace.empty());
    EXPECT_NE(trace.front().find(" CREATE "), std::string::npos) << trace.front();
    EXPECT_NE(trace.back().find(" DROP reason=sustained-decay"), std::string::npos)
        << trace.back();
}

TEST(CabinOptimizerReplayTest, StationaryNoiseNeverOscillates) {
    // The anti-thrash claim in its purest form: sixty jittering periods
    // inside the θ gap and the controller does nothing at all.
    EXPECT_TRUE(Replay(StationaryNoise(0xC3)).empty());
}

TEST(CabinOptimizerReplayTest, QualityCollapseHealsThenDrops) {
    const std::vector<std::string> trace = Replay(QualityCollapse(0xD4));
    std::size_t heal = std::string::npos, drop = std::string::npos;
    for (std::size_t i = 0; i < trace.size(); ++i) {
        if (trace[i].find(" HEAL ") != std::string::npos && heal == std::string::npos) {
            heal = i;
        }
        if (trace[i].find(" DROP reason=quality-collapse") != std::string::npos) drop = i;
    }
    ASSERT_NE(heal, std::string::npos) << "no HEAL was attempted";
    ASSERT_NE(drop, std::string::npos) << "quality collapse never dropped";
    EXPECT_LT(heal, drop) << "PO7: repair is attempted before discard, never after";
}

TEST(CabinOptimizerReplayTest, AServedCabinIsNotDroppedForItsOwnCheapness) {
    const std::vector<std::string> trace = Replay(ServedCheaply(0xE5));
    ASSERT_EQ(trace.size(), 1u)
        << "the frozen baseline failed: the controller acted on the cheapness its own "
           "Cabin caused";
    EXPECT_NE(trace[0].find(" CREATE "), std::string::npos) << trace[0];
}

// ---- The golden traces ----------------------------------------------------

TEST(CabinOptimizerReplayTest, TracesMatchTheGoldenFile) {
    std::vector<std::string> all;
    for (const Scenario& scenario : Scenarios()) {
        for (std::string& line : Replay(scenario)) all.push_back(std::move(line));
    }
    ASSERT_FALSE(all.empty()) << "no scenario drove a decision; the harness proves nothing";

    if (std::getenv("KDS_TRACE_REGEN") != nullptr) {
        std::ofstream out(std::string(KDS_CABIN_TRACES) + ".regen");
        for (const std::string& line : all) out << line << '\n';
        GTEST_SKIP() << "regenerated " << KDS_CABIN_TRACES << ".regen";
    }

    std::ifstream in(KDS_CABIN_TRACES);
    ASSERT_TRUE(in.is_open()) << "cannot open golden traces: " << KDS_CABIN_TRACES
                              << " (run once with KDS_TRACE_REGEN=1 and check the file in)";
    std::vector<std::string> golden;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) golden.push_back(line);
    }

    ASSERT_EQ(all.size(), golden.size()) << "trace count moved; regen and inspect the diff";
    for (std::size_t i = 0; i < all.size(); ++i) {
        EXPECT_EQ(all[i], golden[i]) << KDS_CABIN_TRACES << ":" << (i + 1);
    }
}

// The determinism claim the golden file rests on, proven directly: two
// full runs of every scenario, byte-identical.
TEST(CabinOptimizerReplayTest, ReplayIsDeterministic) {
    for (const Scenario& scenario : Scenarios()) {
        EXPECT_EQ(Replay(scenario), Replay(scenario)) << scenario.name;
    }
}

}  // namespace
}  // namespace kds::stats
