#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/waker.hpp"
#include "kds/sched/task.hpp"

// The reactor (docs/spec/sched.md sections 2-4). One Scheduler runs on the
// calling thread. Still missing from sched.md's blueprint: worker-thread
// spawning and CPU pinning (workplan-crosscore.md P2), the hierarchical
// timing wheel, and the SLO-feedback controller. The cross-core inbox
// (phase 3) is built - see AttachTransport() - though nothing constructs a
// transport in production yet, so today every instance runs alone.
//
// Concurrency protocol: a Scheduler is core-local by construction - all of
// Submit()/RegisterIoHandler()/RegisterMessageHandler()/RunOnce()/Run()/
// Stop() must be called from the single thread that owns this reactor.
// There is nothing to lock: the ready queues, the handler tables and the
// consumed-runtime counters are plain (non-atomic) fields, and the only
// atomics anywhere near this class are the two indices inside each SpscRing
// (workplan-crosscore.md guideline 1).
//
// **The read-only accessors are covered by that same rule**, and it has to
// be said because a getter reads as safe: stopped(), iterations(),
// messages_drained() and the group-accounting block below all read plain
// fields the reactor's thread writes without synchronization, so calling
// one from another thread while Run() proceeds is a data race - the same
// one kShutdown exists to avoid for stopped(). Every caller today reaches
// them from the reactor's own thread (`SHOW META` runs as a task on it) or
// after a join; a future off-core reader needs a message, not a getter.

namespace kds::sched {

struct SchedulerConfig {
    // Max tasks drained from ready queues per RunOnce() (sched.md phase 4
    // loop budget) - keeps I/O-completion draining latency bounded under
    // load. Tunable; not a hard invariant.
    int max_tasks_per_iteration = 64;

    // Share weight per scheduling group (sched.md section 4's share-
    // proportional picking), indexed by SchedulingGroupIndex().
    std::array<std::uint32_t, kNumSchedulingGroups> group_shares{1000, 100, 50};

    // Max cross-core messages drained per RunOnce() (phase 3). The same
    // loop-budget rule phase 4 follows and for the same reason (sched.md
    // §2): a core under a message flood must still get to its I/O
    // completions and its ready tasks this iteration. Undrained messages
    // stay in the ring and are picked up next iteration - nothing is lost
    // by stopping early.
    int max_messages_per_iteration = 64;

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

// What a core does with a message addressed to it. Invoked from inside the
// task phase 3 creates, never from the drain itself - so a handler may take
// as long as a task may, and must yield like one.
//
// `payload` is a view into the scheduler's receive buffer and is **not**
// valid past the call; a handler that needs to keep the bytes copies them.
// The alternative - handing over an owning vector per message - would put
// an allocation on the one path sched.md §2 says has none.
using MessageHandler = std::function<void(const MessageHeader&, std::span<const std::byte>)>;

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

    // ---- Cross-core messaging (sched.md §5, workplan P1) ----------------

    // Attaches this reactor to `transport` as the reactor for `core_id`.
    // `transport` must outlive the scheduler.
    //
    // Null is the default and the whole single-core story: a scheduler with
    // no transport skips phase 3 at the cost of one null test, so every
    // existing construction site is untouched and the `cores = 1` build
    // contributes zero messages and zero allocations (workplan guideline
    // 2).
    // Attaches this reactor to the ring matrix as `core_id`, and **arms
    // the wake path** (waker.hpp): an eventfd registered with this
    // reactor's backend, published to the transport so peers can unblock a
    // sleep that a ring message would otherwise not interrupt.
    //
    // No longer `noexcept`, and no longer infallible: creating the eventfd
    // and registering it can fail. A failure is returned rather than
    // swallowed because the consequence is silent — every cross-core
    // message on this core would pay the idle block, which is a
    // millisecond nobody would think to look for.
    //
    // Single-core builds never call this, so they gain no fd, no flag
    // stores and no syscalls: the fast path is exactly what it was
    // (guideline 2).
    Status AttachTransport(RingTransport* transport, std::uint32_t core_id);

    // What to run when a message of `kind` arrives. Replaces any previous
    // handler for that kind. Fails with InvalidArgument for `kUnset`, which
    // names nothing, and for a kind this build does not know - a handler
    // registered against a number nobody can send is a silent no-op, and
    // the point of the central enum is that such a number does not exist.
    Status RegisterMessageHandler(RingMessageKind kind, MessageHandler handler);

    std::uint32_t core_id() const noexcept { return core_id_; }

    // Messages this reactor has drained. Diagnostics and tests - notably
    // the single-core assertion that it stays 0.
    std::uint64_t messages_drained() const noexcept { return messages_drained_; }
    // The clock this reactor was built on - what a task or handler stamps
    // time with, so nothing threads a second clock pointer beside it.
    const Clock& clock() const noexcept { return clock_; }
    // RunOnce() calls so far: a stamp for "did the loop iterate between two
    // events, or was it blocked" (server/lease_refill_stats.hpp).
    std::uint64_t iterations() const noexcept { return iterations_; }

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

    // ---- Group accounting, read from outside (sched.md §4) --------------
    //
    // §4's last bullet states the gap this exists to make measurable:
    // *"reactor time spent outside task polls (the drain, the idle block) is
    // charged to no group"*. `bench/v2.1.0` §11-5 could not report on it at
    // all - `consumed_ns_` is private, `SHOW META` never printed it, and a
    // measurement run may not add instrumentation - and left it owed by
    // whoever next touched §4. These are that accessor.
    //
    // Two counters per group, because one cannot answer the question:
    //
    // * `consumed_ns()` is the **scheduling** quantity - the share law's
    //   input - and it is *periodically halved* (MaybeDecayConsumedRuntime),
    //   so history does not dominate the pick. It says what the next pick
    //   will weigh, never how much time a group has had.
    // * `polled_ns_total()` and `polls_total()` are cumulative and never
    //   decay. `sum(polled_ns_total) / run_wall_ns()` is the fraction of
    //   reactor wall time spent inside task polls; one minus it is the time
    //   charged to no group, which is the number §4 owes.
    //
    // Reading these costs nothing; keeping them costs two integer adds per
    // poll, on the poll path, stated rather than hidden.
    std::uint64_t consumed_ns(SchedulingGroup group) const noexcept {
        return consumed_ns_[static_cast<std::size_t>(SchedulingGroupIndex(group))];
    }
    std::uint64_t polled_ns_total(SchedulingGroup group) const noexcept {
        return polled_ns_total_[static_cast<std::size_t>(SchedulingGroupIndex(group))];
    }
    std::uint64_t polls_total(SchedulingGroup group) const noexcept {
        return polls_total_[static_cast<std::size_t>(SchedulingGroupIndex(group))];
    }
    // Wall time since this reactor's first RunOnce(), by the injected clock.
    // Zero before the first iteration - never a negative or a made-up span,
    // which is what a construction-time origin would give a scheduler that
    // was built and not yet run.
    std::uint64_t run_wall_ns() const noexcept {
        if (!run_started_) return 0;
        const MonoTimeNs now = clock_.Now();
        return now > run_start_ns_ ? now - run_start_ns_ : 0;
    }

    // Runs one iteration of the fixed-order phase loop (sched.md section
    // 2). Returns true if any task ran, any I/O event was drained, any
    // timer fired, or any cross-core message was received.
    // Work to do once per loop iteration, **after** the ready tasks have
    // run and before the reactor blocks again.
    //
    // It exists for group commit, and the position is the whole of it. A
    // committing statement stages its commit record and parks; every other
    // runnable statement does the same in the same iteration; then this runs
    // once and makes all of them durable with one device sync. Put on a
    // timer instead, it would add that timer's period to every commit's
    // latency; run inside the task, it would sync per commit, which is the
    // batch-of-one this exists to end.
    //
    // Cheap when there is nothing to do - the WAL's drain returns
    // immediately when nothing is staged - so it is called unconditionally
    // rather than gated on whether a task ran.
    //
    // **It returns whether it did anything, and that is load-bearing.** The
    // idle policy (§7, `IdleTimeoutMs`) lets the reactor sleep on an
    // iteration where nothing advanced, and this hook is the one source of
    // progress that is neither a task poll nor an event: a statement parks
    // on `durable_lsn` and it is *this* that moves it. A hook that answered
    // "nothing happened" while it was syncing would let the reactor block
    // between the staging and the wake-up, adding the drain interval to
    // every commit - the one regression this whole change must not cause.
    // `false` is for a tick that found nothing staged and did no I/O.
    void SetPostTaskHook(std::function<bool()> hook) { post_task_hook_ = std::move(hook); }

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

    // Runs phase 4. `advanced` is set when any polled task actually ran
    // code - a task that completed, or one that suspended having executed
    // something (Task::advanced_in_last_poll). A round in which every poll
    // found a predicate still false leaves it alone, and that is what lets
    // the reactor sleep instead of re-asking forever.
    bool RunReadyTasks(bool& advanced);
    bool DrainInbox();
    // The share-proportional pick over the groups with tasks still unpolled
    // this round (`remaining`, RunReadyTasks' count-down).
    bool PickNextGroup(const std::array<std::size_t, kNumSchedulingGroups>& remaining,
                       SchedulingGroup& out) const;
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
    // The undecayed pair; the accessors above say why.
    std::array<std::uint64_t, kNumSchedulingGroups> polled_ns_total_{};
    std::array<std::uint64_t, kNumSchedulingGroups> polls_total_{};
    // Set on the first RunOnce(), so run_wall_ns() spans iterations rather
    // than a lifetime that may have begun long before the reactor started.
    // The flag rather than a zero sentinel: an injected test clock legally
    // reads 0 at its first tick, and a sentinel would then report a
    // never-started reactor forever.
    MonoTimeNs run_start_ns_ = 0;
    bool run_started_ = false;
    std::unordered_map<IoHandle, IoHandler> io_handlers_;
    std::vector<IoEvent> io_events_scratch_;

    // ---- Cross-core (sched.md §5) ---------------------------------------
    RingTransport* transport_ = nullptr;

    // **The wake path** (waker.hpp), armed at AttachTransport and absent
    // on every single-core build.
    //
    // `sleeping_` is read by *other cores' threads*, which is the one place
    // in this reactor where that is true and why it is atomic. It is
    // sequentially consistent on both sides on purpose: the argument that
    // a message cannot be stranded is the store-buffer one, and it needs
    // seq_cst — `RealRingTransport::TrySend` carries it in full.
    std::optional<Waker> waker_;
    std::atomic<bool> sleeping_{false};
    // Iterations that blocked with the flag raised, and iterations whose
    // pre-block re-check found work and skipped the block. The second is
    // the race actually happening, and a run where it stays 0 has not
    // exercised it.
    //
    // **Atomic, unlike every other counter on this class**, and the
    // exception is deliberate: "is that reactor asleep yet" is a question
    // only another thread can usefully ask, so the accessors below are the
    // one pair the class-level "reactor thread only" rule does not cover.
    // Relaxed, and free: both are touched only on an iteration that was
    // about to block, which by construction is an iteration with nothing
    // ready to run.
    std::atomic<std::uint64_t> idle_blocks_{0};
    std::atomic<std::uint64_t> wake_race_skips_{0};
    // Idle blocks taken with tasks still sitting in the ready queues - all
    // of them parked. Every one of these is an iteration the reactor used
    // to spin through (§7's "parked is not ready"), so this counter is the
    // fix's own measure of itself: zero on a build without parks, and
    // climbing on exactly the workload that used to burn a core.
    std::atomic<std::uint64_t> parked_idle_blocks_{0};
    // How long this reactor has actually spent inside the block, and how
    // many wakes arrived to find nothing on the ring (D7 of the v2.3.0
    // order). Plain integers, unlike the three above: nothing outside this
    // reactor's own thread has a use for them, so they follow the
    // class-level rule rather than the wake protocol's exception. Both are
    // touched only on an iteration that was about to block or that a wake
    // ended, which is by construction an iteration with nothing to run.
    //
    // `idle_block_ns_` is what makes the reactor's wall clock add up:
    // `run_wall_ns - sum(polled_ns_total) - idle_block_ns` is the time
    // charged to no group *and* spent doing something (sched.md §4's gap),
    // where before it was that plus the sleep in one lump.
    std::uint64_t idle_block_ns_ = 0;
    std::uint64_t spurious_wakes_ = 0;
    // Set by the waker's own io handler, read after the inbox drain, reset
    // at the top of every iteration: an iteration the eventfd ended.
    bool woken_by_waker_ = false;

public:
    // The wake path, from this reactor's side. `idle_blocks` counts
    // iterations that actually blocked with the flag raised;
    // `wake_race_skips` counts the ones whose pre-block re-check found work
    // and skipped the block - which *is* the race the flag exists for, so a
    // run where it stays 0 has not exercised it. Diagnostics and tests, and
    // the only two accessors on this class that may be read from another
    // thread (see the members).
    std::uint64_t idle_blocks() const noexcept {
        return idle_blocks_.load(std::memory_order_relaxed);
    }
    std::uint64_t wake_race_skips() const noexcept {
        return wake_race_skips_.load(std::memory_order_relaxed);
    }
    // Blocks taken while parked tasks were queued - the spin that used to
    // happen instead. See the member.
    std::uint64_t parked_idle_blocks() const noexcept {
        return parked_idle_blocks_.load(std::memory_order_relaxed);
    }
    // Wall time inside a `PollReady` this reactor was **allowed** to block
    // in - its sleep, plus the syscall around it. An iteration with
    // anything runnable passes a timeout of 0 and is not counted, so this
    // is the idle policy's own time and never a busy reactor's. The
    // denominator `run_wall_ns()` already provides.
    std::uint64_t idle_block_ns() const noexcept { return idle_block_ns_; }
    // Wakes written to **this** reactor's eventfd, by any sender. Zero
    // where the wake path is not armed.
    std::uint64_t wakes_received() const noexcept {
        return waker_.has_value() ? waker_->wakes() : 0;
    }
    // Wakes this instance's transport has written to *every* destination,
    // so the same number on every core and equal to the sum of every
    // core's `wakes_received()`. Instance-wide because the counter is the
    // transport's: one object serves all N reactors.
    std::uint64_t wakes_sent() const noexcept {
        return transport_ != nullptr ? transport_->wakes_sent() : 0;
    }
    // Wakes this reactor read whose iteration then drained no message.
    // Not a defect: the sender publishes before it wakes, so the message
    // can be taken by the iteration that raced the flag, one ahead of the
    // eventfd being read - the ordinary outcome of what `wake_race_skips`
    // counts from the other side. A number climbing far past that one
    // would mean senders waking a core they had nothing for.
    std::uint64_t spurious_wakes() const noexcept { return spurious_wakes_; }
    // Whether this reactor can be woken at all. False on every single-core
    // build, where nothing can send to it.
    bool wake_armed() const noexcept { return waker_.has_value(); }

private:
    std::uint32_t core_id_ = 0;
    // Keyed by the enum's underlying value. A small flat map would do as
    // well; what matters is that nothing iterates it, so its order is never
    // observable (sched.md §8's deterministic-container rule).
    std::unordered_map<std::uint16_t, MessageHandler> message_handlers_;
    // Reused across drains, which is what keeps a received message from
    // allocating in steady state - the same arrangement io_events_scratch_
    // has.
    std::vector<std::byte> message_payload_scratch_;
    std::uint64_t messages_drained_ = 0;
    std::uint64_t iterations_ = 0;
    // Tasks ever queued (Submit). Only ever compared across one RunOnce, to
    // notice work that appeared mid-iteration; the absolute value is not
    // meaningful and it is allowed to wrap.
    std::uint64_t submits_ = 0;
    // Whether the previous full iteration changed anything - the input to
    // "parked is not ready" (IdleTimeoutMs). **Starts true**: a reactor
    // that has never iterated has not established that there is nothing to
    // do, and must not open by sleeping on a queue it has not looked at.
    bool last_iteration_advanced_ = true;

    std::vector<Timer> timers_;                  // heap, LaterDeadlineFirst
    std::unordered_set<TimerId> cancelled_timers_;
    TimerId next_timer_id_ = kInvalidTimerId + 1;

    bool stopped_ = false;
    std::function<bool()> post_task_hook_;
};

}  // namespace kds::sched
