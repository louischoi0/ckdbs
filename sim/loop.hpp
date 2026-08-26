#pragma once

// sim/loop.hpp — the crash–restart–verify loop (bench/workplan-teststrategy
// SIM04), the harness's centerpiece. One iteration:
//
//   build an instance on crashable in-memory devices
//   -> run a seeded workload, verifying every read against the oracle inline
//   -> end it the mode's way (clean shutdown / SYNC-then-crash / crash at a
//      seed-chosen op)
//   -> reboot over the surviving image
//   -> CheckInstance() integrity sweep
//   -> reconcile every table against the oracle
//
// Three modes, three contracts:
//
//   kClean      everything the oracle accepted must be present and equal,
//               and integrity must be clean. No excuses.
//   kSyncCrash  crash immediately after a SYNC: the synced snapshot must
//               be wholly present (that is what SYNC promises **today**);
//               anything acknowledged after it may be present or absent
//               but never wrong.
//   kCrash      crash anywhere: no read after restart may return a row the
//               oracle never accepted (no fabrication), and integrity of
//               the durable image must be clean. The "every acknowledged
//               row whose commit record is durable survives" assertion is
//               written here and **[GATED: recovery]** — nothing reads the
//               WAL back yet (docs/spec/txn.md section 8), so the loop *counts*
//               the rows recovery owes (`gated_missing_rows`) instead of
//               failing on them. The gate flips the day recovery lands:
//               set kRecoveryImplemented below to true and the counter
//               becomes an assertion. A table created after the last SYNC
//               is a separate counter again (`unlogged_ddl_lost_tables`):
//               CREATE TABLE is *unlogged by design*, so those losses are
//               not recovery's debt and stay expected even after the gate
//               flips.
//
// **Faults, orthogonal to the three modes** (SIM05). With
// `SimConfig::faults` set, a seeded schedule (sim/faults.hpp) arms device
// errors *during* the workload, and the contract widens rather than
// loosens:
//
//   - a statement may answer an error, and then it is not applied to the
//     oracle; a statement that answers *rows* is held to the same
//     agreement as ever. Wrong answers stay wrong under injection.
//   - an errored write's outcome is unknown, not absent: the row may be
//     there and may equally be gone. What identifies it is the **id** —
//     an errored UPDATE or DELETE makes that id unchecked, and an errored
//     INSERT makes the relation one that may hold rows the engine never
//     named an id for (sim/oracle.hpp).
//   - a `CREATE TABLE` that catches an injection is **retried once**,
//     because a relation that dies at op 0 turns every later op naming it
//     into an absorbed error and the iteration then verifies nothing while
//     reporting green. An iteration whose oracle ends up empty fails.
//   - once the schedule is exhausted the injections are disarmed and every
//     relation is scanned again — the **quiescence probe**. An instance
//     that still answers an error with no fault left to blame has not
//     survived the fault run, it has merely stopped working, and nothing
//     else in the loop would tell those apart.
//
// **The advisory features are toggled per iteration** (SIM06), and the
// invariant this exists to hammer is invariant 8's, generalized: toggling
// Waystone, the Cabin store or the access statistics may never change a
// result. The oracle does not know they exist, so every iteration already
// tests it; `RunTogglePairing` tests the stronger form — two instances
// differing *only* in the three switches, run the same op stream, and are
// compared statement for statement by `SameOutcome`, whose three tiers say
// exactly what an *answer* is and what is merely a reply reporting the
// advisory state on purpose. That is `waystone_contract_test.cpp`'s
// five-way comparison generalized to a generated workload.
//
// Every random choice forks off the iteration's seed by label, so a
// failing (seed, iteration, mode, profile) quadruple replays exactly.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sim/faults.hpp"
#include "sim/instance.hpp"
#include "sim/oracle.hpp"
#include "sim/workload.hpp"

namespace kds::sim {

// The recovery gate (see above). **Flipped on 2026-08-12**, the day recovery
// began running at mount (docs/workplan-wal-recovery.md RV1, RC10's first
// half): `SimInstance::Boot` now runs analysis / redo / high-water / undo
// over the surviving log before the first statement, so kCrash's full
// durability assertion is armed rather than counted.
//
// What arming it means, exactly: every row the oracle accepted whose commit
// record reached the device must be present and equal after the crash, and
// every loser's row must be gone. What it does **not** cover is stated where
// it is true — a table created after the last SYNC is still lost, because
// CREATE TABLE is unlogged by design (RV3), and `unlogged_ddl_lost_tables`
// stays a counter rather than becoming an assertion.
inline constexpr bool kRecoveryImplemented = true;

// The three advisory switches, off/on per instance (SIM06).
struct FeatureToggles {
    bool waystone = false;          // trail recording and replay
    bool cabins = false;            // the value-observed store
    bool access_statistics = true;  // sys.access_stats, the server default

    std::string Describe() const;
};

enum class SimMode : std::uint8_t {
    kClean = 0,
    kSyncCrash = 1,
    kCrash = 2,
};

const char* SimModeName(SimMode mode);
// The reverse, for a command line and a case file: both used to
// open-code it, in two places that could drift apart.
std::optional<SimMode> ParseSimMode(std::string_view name);

struct SimConfig {
    std::uint64_t seed = 0;
    std::size_t ops = 2000;
    SimMode mode = SimMode::kClean;
    Profile profile = Profile::kUniform;
    std::size_t iterations = 1;

    // The recovery gate, overridable per run so a test can prove the gated
    // assertion *fires* when hand-fed the flag — a gate that cannot fail is
    // not a gate. Production default is the engine's actual state.
    bool assert_recovery = kRecoveryImplemented;

    // The fault schedule (SIM05). kNone is the default and costs one
    // branch per op; `fault_rate` is faults per 1000 ops.
    FaultProfile faults = FaultProfile::kNone;
    std::uint32_t fault_rate = 20;

    // The advisory features (SIM06). Unset draws them per iteration from
    // the seed, which is the production shape of this harness; a value
    // pins them, which is what the paired run and a targeted repro need.
    std::optional<FeatureToggles> toggles;

    // Boot without the recovery phase (SimInstanceOptions::skip_recovery).
    // A fault injection, not a mode: it exists so the armed assertion above
    // can be shown to fail, which since recovery landed is the only way to
    // show it at all — no seed loses an acknowledged row any more.
    bool skip_recovery = false;
};

struct SimVerdict {
    bool ok = true;
    // First failure only: the seed, op index and detail that reproduce it.
    std::string detail;

    std::size_t iterations_run = 0;
    std::size_t ops_run = 0;
    std::size_t reads_checked = 0;
    // Mutations whose reported row count was compared against the oracle's,
    // and transactions resolved either way (SIM06).
    std::size_t writes_checked = 0;
    std::size_t transactions = 0;

    // Fault bookkeeping (SIM05). `faults_fired` is what the devices
    // actually consumed: a schedule whose faults never fire proved
    // nothing, and that has to be visible rather than assumed.
    std::size_t faults_armed = 0;
    std::size_t faults_fired = 0;
    std::size_t errored_ops = 0;
    std::size_t reads_skipped = 0;
    std::size_t counts_skipped = 0;
    // Ops naming a relation whose CREATE was answered with an error, and so
    // ops the loop threw away rather than checked. A run where this
    // approaches `ops_run` verified nothing, however green it printed.
    std::size_t ops_on_lost_relation = 0;

    // Documented-gap bookkeeping — reported, not failed (see above).
    std::size_t gated_missing_rows = 0;
    std::size_t unlogged_ddl_lost_tables = 0;

    std::string Summary(const SimConfig& config) const;

    // Fold one iteration's verdict into a run's. Counters sum; the first
    // failure wins and every later one is noise from the same cause.
    void Absorb(const SimVerdict& other);
};

// **One iteration's whole input, drawn before the first statement runs**
// (SIM07). The generator never reads a reply, so nothing here depends on
// the engine — which is what makes an iteration replayable without the
// generator, and shrinkable by deleting entries. `BuildPlan` is the only
// place the seed is consulted; `RunPlan` is the only place the engine is.
struct SimPlan {
    struct Entry {
        Op op;
        // Armed immediately before this op. They travel *with* the op, so
        // dropping the op during minimization drops its faults too — a
        // schedule keyed on op index would re-aim every fault the moment
        // anything was removed.
        std::vector<FaultKind> faults;
    };

    std::vector<Entry> entries;
    FeatureToggles toggles;
};

SimPlan BuildPlan(const SimConfig& config, std::size_t iteration);

// One iteration over a plan: fresh instance, the plan's ops, the mode's
// ending, restart, sweep, reconcile. `iteration` names the run in the
// failure detail and nothing else — the plan carries everything else.
SimVerdict RunPlan(const SimConfig& config, const SimPlan& plan, std::size_t iteration = 0);

SimVerdict RunSimulation(const SimConfig& config);

// The quiescence probe's own assertion (SIM05): every relation the oracle
// knows scans without an error and agrees with it. Exposed because a gate
// that cannot be shown to fail is not a gate - a test hands it an instance
// that stopped answering and watches it refuse.
bool ScanAgreesWithOracle(SimInstance& instance, const Oracle& oracle, std::string& why);

// The pairing's comparison rule (SIM06), exposed for the same reason: the
// rule has three tiers and the exceptions are where the risk is, so they
// are tested directly rather than only through a run that happens to pass.
bool SameOutcome(const Op& op, const std::string& bare, const std::string& full);

// The toggle pairing (SIM06): one op stream, two instances differing only
// in the three advisory switches, every reply compared byte for byte.
// Ignores `mode` and `faults` — it never crashes and never injects, both
// for the reason above.
SimVerdict RunTogglePairing(const SimConfig& config);

}  // namespace kds::sim
