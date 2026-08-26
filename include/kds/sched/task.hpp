#pragma once

#include <functional>
#include <memory>

// Task representation is an explicit open decision (docs/spec/sched.md section
// 3, docs/rules/rules.md section 7): callback/future chains vs C++20 stackless
// coroutines vs stackful fibers are all still viable. This header does not
// choose one - it defines the type-erased "runnable task handle" contract
// sched.md says the scheduler needs (a queue of runnable task handles),
// and ships exactly one concrete implementation, FunctionTask, wrapping a
// plain callable. A future coroutine- or fiber-backed Task subclass can be
// added later without any change to Scheduler, since Scheduler only ever
// calls Task::Poll().

namespace kds::sched {

enum class SchedulingGroup : int {
    kForeground = 0,  // OLTP execution
    kMaintenance = 1,  // physical relayout, statistics, hint-index upkeep
    kSystem = 2,       // checkpoint/WAL housekeeping
};

inline constexpr int kNumSchedulingGroups = 3;

inline int SchedulingGroupIndex(SchedulingGroup group) noexcept {
    return static_cast<int>(group);
}

enum class PollResult {
    kSuspended,  // task yielded; not finished, re-enqueue for a later slice
    kDone,       // task finished; drop it, never call Poll() again
};

// Run-to-completion-until-yield unit of work. Every task belongs to
// exactly one scheduling group (sched.md section 4) for its whole
// lifetime. Poll() must return promptly (cooperative yield, docs/spec/sched.md
// section 3) - there is no preemption.
class Task {
public:
    explicit Task(SchedulingGroup group) noexcept : group_(group) {}
    virtual ~Task() = default;

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    virtual PollResult Poll() = 0;

    // Did the last Poll() actually run any of this task's code?
    //
    // `kSuspended` answers two different questions with one word: a task
    // that ran and yielded, and a task that was asked whether its condition
    // holds, answered no, and executed nothing. The reactor's idle policy
    // needs them apart — a queue holding only the second kind has no work
    // in it, and treating it as runnable is what made a reactor with any
    // parked coroutine spin at ~90% of a core doing nothing but re-asking
    // a predicate (`bench/v2.2.0/results-shipping-ssb-v2.2.0-11-g982e133.md`
    // §7, `docs/spec/sched.md` §7).
    //
    // **The default is `true`, and the direction is the point.** A task
    // that does not track this is treated as having advanced, which costs
    // one wasted iteration and can never cost a missed wake. Only
    // `CoroTask` — the one place a park is a real, detectable state —
    // answers otherwise.
    virtual bool advanced_in_last_poll() const noexcept { return true; }

    SchedulingGroup group() const noexcept { return group_; }

private:
    SchedulingGroup group_;
};

using TaskPtr = std::unique_ptr<Task>;

// Phase-1 concrete Task: wraps a callable returning PollResult. Most call
// sites should use this instead of writing a new Task subclass; it is not
// re-entrant (a single FunctionTask instance is only ever driven by the
// scheduler that owns its queue, per the thread-per-core/shared-nothing
// rule - rules.md #3).
class FunctionTask final : public Task {
public:
    using Fn = std::function<PollResult()>;

    FunctionTask(SchedulingGroup group, Fn fn) : Task(group), fn_(std::move(fn)) {}

    PollResult Poll() override { return fn_(); }

private:
    Fn fn_;
};

}  // namespace kds::sched
