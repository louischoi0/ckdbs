#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kds/base/log.hpp"
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

    // Longest the reactor will block in PollReady() when it has nothing
    // ready to run (sched.md section 7's idle policy). It is a *cap*, not
    // the sleep itself: a pending timer shortens it to that timer's
    // deadline, and a non-empty ready queue drops it to 0. The cap exists
    // so Stop() and anything that arrives outside the io backend are still
    // noticed promptly, rather than requiring a socket event to wake the
    // loop.
    int max_idle_block_ms = 10;
};

using IoHandler = std::function<void(const IoEvent&)>;

// Identifies an armed timer, for cancellation. Never reused, so cancelling
// an already-fired one-shot is a harmless no-op rather than a way to kill
// somebody else's timer.
using TimerId = std::uint64_t;
inline constexpr TimerId kInvalidTimerId = 0;

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

    // ---- Timers (sched.md section 6, phase 2) ---------------------------
    //
    // Backed by a binary min-heap keyed on deadline, *not* the hierarchical
    // timing wheel sched.md section 6 specifies - the wheel's win is O(1)
    // insertion at very high timer counts, and this reactor arms a handful
    // (checkpoint cadence, D3 flush interval). The wheel replaces this
    // without touching these signatures.
    //
    // A fired timer runs its callback directly in phase 2 rather than
    // submitting a task, so a timer callback must be as short as a phase-1
    // io handler: the thing it should do is Submit() the work.

    // Runs `fn` once, at the first RunOnce() whose clock reading is at or
    // past `deadline`. A deadline already in the past fires on the next
    // iteration, never retroactively.
    TimerId SubmitAt(MonoTimeNs deadline, std::function<void()> fn);

    // Runs `fn` every `period_ns`, first firing one period from now.
    // Re-armed from the *deadline* rather than from completion time, so a
    // slow callback does not make the interval drift outward; a callback
    // that overruns its period simply fires again immediately rather than
    // stacking up.
    TimerId SubmitEvery(MonoTimeNs period_ns, std::function<void()> fn);

    // Disarms a timer. Safe for an id that has already fired or was never
    // valid; safe to call from inside the timer's own callback.
    void CancelTimer(TimerId id);

    std::size_t armed_timers() const noexcept { return timers_.size(); }

    // Runs one iteration of the fixed-order phase loop (sched.md section
    // 2). Phase 3 (cross-core inbox drain) is still an explicit no-op - the
    // ordering is preserved so Phase 2+ work slots in later without
    // reshaping this method. Returns true if any task ran, any I/O event
    // was drained, or any timer fired.
    bool RunOnce();

    // Runs RunOnce() until Stop() is called (from within a task, e.g. one
    // handling a shutdown command). Idle iterations block in the io backend
    // for up to `max_idle_block_ms`, or until the next timer is due,
    // whichever is sooner - so an idle reactor costs no CPU.
    void Run();

    // Diagnostic log, null (discard) by default; `log` must outlive the
    // scheduler. The reactor has no caller to return a Status to - Run()
    // is the top of the stack - so an io-backend failure has nowhere else
    // to go. That is the gap RunOnce() used to mark with a (void) cast.
    void SetLogger(Logger* log) noexcept { log_ = log; }

    // Requests Run() to return after the current iteration. Idempotent.
    void Stop() noexcept { stopped_ = true; }
    bool stopped() const noexcept { return stopped_; }

private:
    struct Timer {
        MonoTimeNs deadline;
        MonoTimeNs period_ns;  // 0 = one-shot
        TimerId id;
        std::function<void()> fn;
    };

    // Min-heap order: std::push_heap/pop_heap build a max-heap, so "less"
    // is the later deadline. Ties break on id, keeping firing order stable
    // for timers armed for the same instant.
    struct LaterDeadlineFirst {
        bool operator()(const Timer& a, const Timer& b) const noexcept {
            if (a.deadline != b.deadline) return a.deadline > b.deadline;
            return a.id > b.id;
        }
    };

    bool RunReadyTasks();
    bool PickNextGroup(SchedulingGroup& out) const;
    void MaybeDecayConsumedRuntime();
    bool ExpireTimers();
    bool HasReadyTask() const noexcept;
    int IdleTimeoutMs() const noexcept;
    TimerId ArmTimer(MonoTimeNs deadline, MonoTimeNs period_ns, std::function<void()> fn);

    const Clock& clock_;
    IoBackend& io_backend_;
    SchedulerConfig config_;
    Logger* log_ = nullptr;
    // Consecutive failing PollReady() calls. A reactor whose backend is
    // broken fails every iteration, and one line per iteration would bury
    // the log - so only the transitions are reported (see RunOnce()).
    std::uint64_t consecutive_io_failures_ = 0;

    std::array<std::deque<TaskPtr>, kNumSchedulingGroups> ready_queues_;
    std::array<std::uint64_t, kNumSchedulingGroups> consumed_ns_{};
    std::unordered_map<IoHandle, IoHandler> io_handlers_;
    std::vector<IoEvent> io_events_scratch_;

    std::vector<Timer> timers_;                  // heap, LaterDeadlineFirst
    std::unordered_set<TimerId> cancelled_timers_;
    TimerId next_timer_id_ = kInvalidTimerId + 1;

    bool stopped_ = false;
};

}  // namespace kds::sched
