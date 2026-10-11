#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "kds/base/current_core.hpp"
#include "kds/sched/clock.hpp"

// **BI-S1's instrument** (`instructions/v3.0.0/workorder-bi-checkpoint-off-the-reactor.md`
// W3): where a checkpoint run's time goes, per phase, and whether its core's
// reactor turned meanwhile. `SHOW META`'s `checkpoint_last_*` fields print
// the session's core's last run; the checkpointer logs one Info line per run.
//
// **Timed by the scheduler's injected clock** (`sched.md` invariant 8), which
// the checkpointer installs for the length of one synchronous call - `Start`,
// one `Step`, `Complete` - through `CheckpointPhaseScope`. A `PhaseTimer` with
// no scope installed on its thread reads no clock and records nothing, so the
// page store's writeback pays one thread-local load on every path that is not
// a checkpoint's. The scope is per call and never spans a park: a task
// polled between two steps on the same thread is not charged to the run.
//
// **The outermost phase wins.** The publish's writeback walks the same
// `WriteBack` a step does; while `kPublishWriteback` is open, the gate, write
// and claim timers inside it record nothing, so the phases partition the run
// rather than overlap.
//
// **Per core, written by the core's own thread** (or the shutdown thread
// acting as it, under `CurrentCoreGuard`), read by `SHOW META` on the same
// core. Relaxed atomics because the shutdown path and a test read across
// threads; a statistic, never a fence.

namespace kds {

enum class CheckpointPhase : std::uint8_t {
    kStart,             // `Checkpointer::Start`: the snapshot and its records
    kLogGateBatch,      // `AwaitWalGate`'s `EnsureDurable` for a batch
    kLogGateRun,        // `WriteBack`'s per-run `EnsureDurable`
    kPageWrite,         // a device page write or run write
    kMapWrite,          // a step's `FlushMaps`; none since BI-S2 (BI-Q12), kept so A and B read alike
    kClaimWait,         // waiting out another writeback's claim
    kDataSync,          // a step's data-file `fsync`
    kEndWait,           // `CHECKPOINT_END`'s `EnsureDurable`
    kPublishWriteback,  // the publish's whole-store `Flush`
    kPublishSync,       // the publish's data-file `fsync`
    kCount,
};

inline constexpr std::size_t kCheckpointPhaseCount = static_cast<std::size_t>(CheckpointPhase::kCount);

// The `SHOW META` and log spelling of each phase, in enum order.
inline constexpr std::array<const char*, kCheckpointPhaseCount> kCheckpointPhaseNames = {
    "start",     "log_gate_batch", "log_gate_run",      "page_write",   "map_write",
    "claim_wait", "data_sync",     "end_wait",          "publish_writeback", "publish_sync",
};
static_assert(kCheckpointPhaseNames.back() != nullptr, "a CheckpointPhase without a name");

// One run, accumulated while it runs and published whole at its end.
struct CheckpointRunReport {
    sched::MonoTimeNs start_ns = 0;  // the gate's entry
    sched::MonoTimeNs end_ns = 0;    // the publish's return, or the failure
    std::uint64_t steps = 0;
    std::uint64_t pages = 0;           // the snapshot pages its steps flushed
    std::uint64_t dirty_at_start = 0;  // the dirty table `Start` took; 0 when resumed
    bool resumed = false;              // a failed run's snapshot, taken up again
    bool completed = false;
    // One step's whole reactor hold, and one step's page writes summed - the
    // question BI-Q9 (ii) asks, which a per-occurrence max (one run write of
    // up to 8 pages) cannot answer.
    std::uint64_t step_max_ns = 0;
    std::uint64_t step_page_write_max_ns = 0;
    std::array<std::uint64_t, kCheckpointPhaseCount> sum_ns{};
    std::array<std::uint64_t, kCheckpointPhaseCount> max_ns{};  // one occurrence's longest
    std::array<std::uint64_t, kCheckpointPhaseCount> count{};   // occurrences timed
    // BI-S3: the task's asks of the log - a step handed back for an LSN not
    // yet durable, and an end record not yet durable - each answered by a
    // park unless the writer's sync landed first.
    std::uint64_t gate_parks = 0;
    std::uint64_t end_parks = 0;
    // Wall time the task spent parked - not reactor time: the reactor ran
    // other work meanwhile - on the log and on the data file's sync.
    std::uint64_t log_park_ns = 0;
    std::uint64_t data_park_ns = 0;
};

namespace detail {
struct CheckpointPhaseSink {
    const sched::Clock* clock = nullptr;
    CheckpointRunReport* report = nullptr;
    bool in_phase = false;  // an outer phase is open: inner timers record nothing
};
inline thread_local CheckpointPhaseSink* g_checkpoint_sink = nullptr;
}  // namespace detail

// Installs `report` as this thread's run for the scope's length. Nests: the
// previous sink comes back at the end.
class CheckpointPhaseScope {
public:
    CheckpointPhaseScope(const sched::Clock* clock, CheckpointRunReport* report) noexcept
        : sink_{clock, report, false}, saved_(detail::g_checkpoint_sink) {
        if (clock != nullptr && report != nullptr) detail::g_checkpoint_sink = &sink_;
    }
    ~CheckpointPhaseScope() { detail::g_checkpoint_sink = saved_; }
    CheckpointPhaseScope(const CheckpointPhaseScope&) = delete;
    CheckpointPhaseScope& operator=(const CheckpointPhaseScope&) = delete;

private:
    detail::CheckpointPhaseSink sink_;
    detail::CheckpointPhaseSink* saved_;
};

// Times one occurrence of `phase` into the installed run, if there is one and
// no outer phase is open.
class PhaseTimer {
public:
    explicit PhaseTimer(CheckpointPhase phase) noexcept : phase_(phase) {
        detail::CheckpointPhaseSink* sink = detail::g_checkpoint_sink;
        if (sink == nullptr || sink->in_phase) return;
        sink_ = sink;
        sink->in_phase = true;
        start_ = sink->clock->Now();
    }
    ~PhaseTimer() {
        if (sink_ == nullptr) return;
        const sched::MonoTimeNs now = sink_->clock->Now();
        const std::uint64_t ns = now > start_ ? now - start_ : 0;
        const auto i = static_cast<std::size_t>(phase_);
        sink_->report->sum_ns[i] += ns;
        ++sink_->report->count[i];
        if (ns > sink_->report->max_ns[i]) sink_->report->max_ns[i] = ns;
        sink_->in_phase = false;
    }
    PhaseTimer(const PhaseTimer&) = delete;
    PhaseTimer& operator=(const PhaseTimer&) = delete;

private:
    CheckpointPhase phase_;
    detail::CheckpointPhaseSink* sink_ = nullptr;
    sched::MonoTimeNs start_ = 0;
};

// The per-core record: whether a run is in flight, the runs it started, and
// its last run whole.
class CheckpointCensus {
public:
    static constexpr std::uint32_t kSlots = 64;  // `kMaxWalCores` (superblock.hpp)

    struct Slot {
        // Bumped at a run's entry and again at its end, so an odd value is a
        // run in flight and a reactor iteration that saw the value move
        // overlapped a run (`Scheduler`'s inside/outside split).
        // Runs taken, outside a run, are `run_edges / 2`.
        std::atomic<std::uint64_t> run_edges{0};
        std::atomic<std::uint64_t> start_refused{0};
        std::atomic<std::uint64_t> last_wall_ns{0};
        std::atomic<std::uint64_t> last_step_max_ns{0};
        std::atomic<std::uint64_t> last_step_page_write_max_ns{0};
        std::atomic<std::uint64_t> last_steps{0};
        std::atomic<std::uint64_t> last_pages{0};
        std::atomic<std::uint64_t> last_dirty_at_start{0};
        std::atomic<bool> last_completed{false};
        std::array<std::atomic<std::uint64_t>, kCheckpointPhaseCount> last_sum_ns{};
        std::array<std::atomic<std::uint64_t>, kCheckpointPhaseCount> last_max_ns{};
        std::array<std::atomic<std::uint64_t>, kCheckpointPhaseCount> last_count{};
        std::atomic<std::uint64_t> last_gate_parks{0};
        std::atomic<std::uint64_t> last_end_parks{0};
    };

    static Slot& Of(std::uint32_t core) noexcept { return slots()[core % kSlots]; }

    static void RunEdge(std::uint32_t core) noexcept {
        Of(core).run_edges.fetch_add(1, std::memory_order_relaxed);
    }

    static void Publish(std::uint32_t core, const CheckpointRunReport& r) noexcept {
        Slot& s = Of(core);
        s.last_wall_ns.store(r.end_ns > r.start_ns ? r.end_ns - r.start_ns : 0,
                             std::memory_order_relaxed);
        s.last_step_max_ns.store(r.step_max_ns, std::memory_order_relaxed);
        s.last_step_page_write_max_ns.store(r.step_page_write_max_ns, std::memory_order_relaxed);
        s.last_steps.store(r.steps, std::memory_order_relaxed);
        s.last_pages.store(r.pages, std::memory_order_relaxed);
        s.last_dirty_at_start.store(r.dirty_at_start, std::memory_order_relaxed);
        s.last_completed.store(r.completed, std::memory_order_relaxed);
        for (std::size_t i = 0; i < kCheckpointPhaseCount; ++i) {
            s.last_sum_ns[i].store(r.sum_ns[i], std::memory_order_relaxed);
            s.last_max_ns[i].store(r.max_ns[i], std::memory_order_relaxed);
            s.last_count[i].store(r.count[i], std::memory_order_relaxed);
        }
        s.last_gate_parks.store(r.gate_parks, std::memory_order_relaxed);
        s.last_end_parks.store(r.end_parks, std::memory_order_relaxed);
    }

private:
    static std::array<Slot, kSlots>& slots() noexcept {
        static std::array<Slot, kSlots> s{};
        return s;
    }
};

}  // namespace kds
