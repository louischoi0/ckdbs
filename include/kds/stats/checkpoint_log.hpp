#pragma once

#include <cstdint>
#include <vector>

#include "kds/sched/checkpoint_census.hpp"
#include "kds/stats/per_core_ring.hpp"

// **`sys.checkpoints`' rows: the recent checkpoint runs, one row each**
// (`instructions/v3.0.0/workorder-bo-sys-statistics-show-sugar.md` §3.4,
// BO-S7). Memory-resident (BO-Q5 (b)): the runs before a crash or a restore
// are gone with the process, and the mount's recovery block stays in
// `sys.meta`. A ring of 1024 per core (BO-Q7 (a)), `PerCoreRing`'s protocol.
//
// **The checkpointer hands its finished report over and writes nothing
// else** - no catalog page (BI-S2's rule): `Checkpointer::EndRun` calls
// `Record` once per run, beside `CheckpointCensus::Publish`.

namespace kds::stats {

// A projection of BI's `CheckpointRunReport` (`sched/checkpoint_census.hpp`).
struct CheckpointRecord {
    std::uint64_t seq = 0;         // the core's own count of runs, from 1
    std::int64_t started_us = 0;   // UTC epoch microseconds (a TIMESTAMP)
    std::uint64_t duration_us = 0;
    std::uint64_t steps = 0;
    std::uint64_t pages_flushed = 0;
    std::uint64_t dirty_at_start = 0;  // 0 on a resumed run, which takes no snapshot
    // The run's waits on the log and on the data file's sync: the time a
    // task run spent parked on each, and the time a synchronous call spent
    // inside the phases that wait on each (the log gates and the end
    // record's; a step's and the publish's sync). A log gate inside the
    // publish's writeback, and a claim wait's sync, are inside the phase
    // that holds them (`kPublishWriteback`, `kClaimWait`) and in neither
    // column; a task run's publish parks are in `log_wait_us`.
    std::uint64_t log_wait_us = 0;
    std::uint64_t data_wait_us = 0;
    std::uint32_t core = 0;
    bool resumed = false;    // a failed run's snapshot, taken up again
    bool completed = false;  // `false`: the run failed, its snapshot left for the next
};

class CheckpointLog {
public:
    static constexpr std::size_t kRingCapacity = 1024;  // BO-Q7 (a), per core
    using Ring = PerCoreRing<CheckpointRecord, kRingCapacity>;

    // One finished run of `core`'s checkpointer. `ended_wall_us` is the wall
    // instant the run ended; `started` is it less the run's duration.
    static void Record(std::uint32_t core, const CheckpointRunReport& report,
                       std::int64_t ended_wall_us) noexcept;

    static std::vector<CheckpointRecord> Snapshot() { return ring().Snapshot(); }
    static Ring::Totals Read() noexcept { return ring().Read(); }
    static void ResetForTest() noexcept { ring().Reset(); }

private:
    static Ring& ring() noexcept;
};

}  // namespace kds::stats
