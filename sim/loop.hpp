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
//               WAL back yet (docs/txn.md section 8), so the loop *counts*
//               the rows recovery owes (`gated_missing_rows`) instead of
//               failing on them. The gate flips the day recovery lands:
//               set kRecoveryImplemented below to true and the counter
//               becomes an assertion. A table created after the last SYNC
//               is a separate counter again (`unlogged_ddl_lost_tables`):
//               CREATE TABLE is *unlogged by design*, so those losses are
//               not recovery's debt and stay expected even after the gate
//               flips.
//
// Every random choice forks off the iteration's seed by label, so a
// failing (seed, iteration, mode, profile) quadruple replays exactly.

#include <cstdint>
#include <string>

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

enum class SimMode : std::uint8_t {
    kClean = 0,
    kSyncCrash = 1,
    kCrash = 2,
};

const char* SimModeName(SimMode mode);

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

    // Documented-gap bookkeeping — reported, not failed (see above).
    std::size_t gated_missing_rows = 0;
    std::size_t unlogged_ddl_lost_tables = 0;

    std::string Summary(const SimConfig& config) const;
};

SimVerdict RunSimulation(const SimConfig& config);

}  // namespace kds::sim
