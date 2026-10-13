#pragma once

#include <functional>
#include <memory>

// The type-erased "runnable task handle" contract the scheduler needs
// (docs/spec/sched.md §3: a queue of runnable task handles). Scheduler only
// ever calls Task::Poll(). Two implementations: FunctionTask here, wrapping
// a plain callable, and `CoroTask` (`coro.hpp`) - C++20 stackless
// coroutines, the engine's chosen task representation (sched.md §3).

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
    // in it, and treating it as runnable spins a reactor with any parked
    // coroutine at ~90% of a core re-asking a predicate
    // (`docs/spec/sched.md` §7).
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
// scheduler that owns its queue: a task is core-local, rules.md §3).
class FunctionTask final : public Task {
public:
    using Fn = std::function<PollResult()>;

    FunctionTask(SchedulingGroup group, Fn fn) : Task(group), fn_(std::move(fn)) {}

    PollResult Poll() override { return fn_(); }

private:
    Fn fn_;
};

}  // namespace kds::sched
