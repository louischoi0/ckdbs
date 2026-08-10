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

// The recovery gate (see above). Flipping this without a WAL redo pass
// makes kCrash fail immediately — which is the acceptance test recovery
// must pass on the day it lands.
inline constexpr bool kRecoveryImplemented = false;

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
