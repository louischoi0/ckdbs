#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/task.hpp"

// Single-core reactor (docs/sched.md sections 2-4). This is Phase 1 of the
// scheduler blueprint: exactly one Scheduler instance runs on the calling
// thread (no worker-thread spawning or CPU pinning yet - sched.md's
// thread-per-core fan-out, cross-core SPSC rings, hierarchical timing
// wheel, SLO-feedback controller, and deterministic multi-reactor
// simulation are all Phase 2+ and are not built here; see
// docs/sched.md's "Implementation Status" section for what's left).
//
// Concurrency protocol: a Scheduler is core-local by construction - all of
// Submit()/RegisterIoHandler()/RunOnce()/Run()/Stop() must be called from
// the single thread that owns this reactor. There is nothing to lock
// because (per rules.md #3) there is exactly one reactor today; the ready
// queues and consumed-runtime counters are plain (non-atomic) fields.

namespace kds::sched {

struct SchedulerConfig {
    // Max tasks drained from ready queues per RunOnce() (sched.md phase 4
    // loop budget) - keeps I/O-completion draining latency bounded under
    // load. Tunable; not a hard invariant.
    int max_tasks_per_iteration = 64;

    // Share weight per scheduling group (sched.md section 4's share-
    // proportional picking), indexed by SchedulingGroupIndex().
    std::array<std::uint32_t, kNumSchedulingGroups> group_shares{1000, 100, 50};

    // Consumed-runtime counters are halved (all groups at once, so
    // relative shares are preserved) once the largest counter exceeds this
    // many nanoseconds - sched.md section 4's "consumption counters decay
    // periodically so history does not dominate." The decay law itself is
    // Phase 2+ tuning; this is a simple placeholder that satisfies the
    // invariant without needing the SLO controller.
    std::uint64_t decay_threshold_ns = 1'000'000'000;  // 1 second
};

using IoHandler = std::function<void(const IoEvent&)>;

class Scheduler {
public:
    Scheduler(const Clock& clock, IoBackend& io_backend, SchedulerConfig config = {});

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // Enqueues a ready task into its scheduling group's run queue. Never
    // fails (rules.md #1: void-returning functions must be infallible) -
    // the queue is an unbounded std::deque in Phase 1; sched.md's fixed-
    // capacity/preallocated queue requirement is Phase 2+ work.
    void Submit(TaskPtr task);

    // Registers `handle` with the io backend for `interest`; any ready
    // event on it during phase 1 of a later RunOnce() invokes `handler`
    // (the handler is expected to Submit() a Task to actually do the work,
    // keeping phase-1 completion draining itself cheap and bounded).
    Status RegisterIoHandler(IoHandle handle, IoInterest interest, IoHandler handler);
    Status ModifyIoHandler(IoHandle handle, IoInterest interest);
    Status UnregisterIoHandler(IoHandle handle);

    // Runs one iteration of the fixed-order phase loop (sched.md section
    // 2). Phases 2 (timer expiry) and 3 (cross-core inbox drain) are
    // explicit no-ops in Phase 1 - the ordering is preserved so Phase 2+
    // work slots in later without reshaping this method. Returns true if
    // any task ran or any I/O event was drained.
    bool RunOnce();

    // Runs RunOnce() until Stop() is called (from within a task, e.g. one
    // handling a shutdown command). Phase 1 always busy-polls (sched.md
    // section 7's blocking idle policy is Phase 2+).
    void Run();

    // Requests Run() to return after the current iteration. Idempotent.
    void Stop() noexcept { stopped_ = true; }
    bool stopped() const noexcept { return stopped_; }

private:
    bool RunReadyTasks();
    bool PickNextGroup(SchedulingGroup& out) const;
    void MaybeDecayConsumedRuntime();

    const Clock& clock_;
    IoBackend& io_backend_;
    SchedulerConfig config_;

    std::array<std::deque<TaskPtr>, kNumSchedulingGroups> ready_queues_;
    std::array<std::uint64_t, kNumSchedulingGroups> consumed_ns_{};
    std::unordered_map<IoHandle, IoHandler> io_handlers_;
    std::vector<IoEvent> io_events_scratch_;
    bool stopped_ = false;
};

}  // namespace kds::sched
