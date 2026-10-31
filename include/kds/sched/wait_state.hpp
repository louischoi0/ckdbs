#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "kds/sched/clock.hpp"
#include "kds/stats/wait_event.hpp"

// **A task's wait state and the `WaitScope` that sets it**
// (`instructions/v3.2.0/workorder-cj-wait-events-ash.md` §2.1-§2.5, CJ's WE02;
// decisions WD3, WD9).
//
// ---- The protocol --------------------------------------------------------
//
// A wait is a coroutine suspended in the cooperative loop, not a blocked
// thread, so every suspension point records itself. Each `CoroTask` owns one
// `TaskWaitState`; the core thread that polls it is its only reader and
// writer (I1) - plain stores, no atomic. Which task is running is the
// core-local `CoreWaitContext::current`, set by `CoroTask::Poll` around the
// span it resumes the coroutine, so a `WaitScope` needs no task argument.
//
//   ON_CPU    the task is being polled.
//   RUNNABLE  `Poll` returned without a park: the task is re-queued, and the
//             time to its next resume is booked as `sched.run_queue` (WD9).
//   WAITING   a `WaitScope` is open, or `Poll` found a park with none
//             (booked as `sched.unclassified`, counted in
//             `CoreWaitContext::unscoped_parks` - I2's violation counter,
//             which G1 requires to read 0).
//   IDLE      a `WaitScope` on an `idle.*` event: not DB time.
//
// The hooks fire only on a poll that resumes the coroutine: a poll that asks
// a predicate, hears no, and returns is neither an exit nor an enter.
//
// ---- Time ----------------------------------------------------------------
//
// Read through the core's `Clock` seam (WD3): `sched::SystemClock`
// (`steady_clock`, `CLOCK_MONOTONIC` through the vDSO) in production, a
// `ManualClock` in tests (`SetWaitClock`). Instrumentation never reads a
// system clock directly.
//
// ---- Cost ----------------------------------------------------------------
//
// `KDS_WAIT_INSTRUMENTATION_OFF` compiles every hook to nothing. It exists
// for the P1 overhead measurement (WD12) and is never set in a production
// build.

namespace kds::sched {

enum class TaskState : std::uint8_t { kOnCpu = 0, kRunnable, kWaiting, kIdle };

struct TaskWaitState {
    TaskState state = TaskState::kRunnable;  // a task not yet polled waits its turn
    stats::WaitEvent event = stats::WaitEvent::kOnCpu;
    std::uint64_t p1 = 0;
    std::uint32_t p2 = 0;
    std::uint32_t p3 = 0;
    MonoTimeNs since_ns = 0;  // when `state` was entered; a scoped wait's is its scope's start
    bool scoped = false;      // a `WaitScope` (not `Poll`) opened the WAITING state
};

// A core's cumulative wait time per event (WE05 adds the histogram, the max
// and the per-session split). Plain integers: one writer, the core.
struct EventTotals {
    std::uint64_t count = 0;
    std::uint64_t total_ns = 0;
};

struct CoreWaitContext {
    const Clock* clock = nullptr;      // null: the process's SystemClock
    TaskWaitState* current = nullptr;  // the task being polled, null between polls
    std::array<EventTotals, stats::kWaitEventCount> totals{};
    std::uint64_t on_cpu_ns = 0;       // ON_CPU is a state, not an event: kept beside the table
    std::uint64_t unscoped_parks = 0;  // I2 violations: parks no WaitScope covered
    // Where the running task's current ON_CPU segment began. A segment ends
    // when a `WaitScope` opens (a blocking section or a suspension) and a new
    // one begins when it closes or the task is next resumed outside one, so
    // an interval is ON_CPU or an event's, never both (I4).
    MonoTimeNs cpu_since = 0;
};

// The calling thread's context (one reactor thread per core).
CoreWaitContext& WaitContext() noexcept;

// Installs `clock` for the calling thread (null restores `SystemClock`).
void SetWaitClock(const Clock* clock) noexcept;
MonoTimeNs WaitNow() noexcept;

// Zeroes the calling thread's totals and violation count. Tests.
void ResetWaitContext() noexcept;

#ifndef KDS_WAIT_INSTRUMENTATION_OFF

// `WaitScope w(Event, p1, p2, p3); co_await ...;` - the task is WAITING on
// the event from construction to destruction, which books the elapsed time to
// the core's totals. Nesting is forbidden (`assert`). A scope opened with no
// task being polled (a unit test, a synchronous caller) books to the totals
// and changes no task's state.
class WaitScope {
public:
    explicit WaitScope(stats::WaitEvent event, std::uint64_t p1 = 0, std::uint32_t p2 = 0,
                       std::uint32_t p3 = 0) noexcept;
    ~WaitScope();
    // Names the wait for what it turned out to be, once known (a page read
    // learns its page's kind from the bytes it read). The span and its start
    // are unchanged.
    void Reclassify(stats::WaitEvent event, std::uint32_t p2 = 0) noexcept;
    WaitScope(const WaitScope&) = delete;
    WaitScope& operator=(const WaitScope&) = delete;

private:
    stats::WaitEvent event_;
    MonoTimeNs start_ns_;
    TaskWaitState* task_;
};

// A `counter` event (WD10): a conflict that did not wait. Raises the count;
// the time is 0.
void RecordCounterEvent(stats::WaitEvent event) noexcept;

// The `CoroTask::Poll` hooks. `Enter` runs when a poll is about to resume the
// coroutine; `Exit` when that poll ends. `parked`: the coroutine is left on a
// wait; `done`: it finished. Neither: it yielded.
void WaitOnPollEnter(TaskWaitState& task) noexcept;
void WaitOnPollExit(TaskWaitState& task, bool parked, bool done) noexcept;

#else

class WaitScope {
public:
    explicit WaitScope(stats::WaitEvent, std::uint64_t = 0, std::uint32_t = 0,
                       std::uint32_t = 0) noexcept {}
    void Reclassify(stats::WaitEvent, std::uint32_t = 0) noexcept {}
};
inline void RecordCounterEvent(stats::WaitEvent) noexcept {}
inline void WaitOnPollEnter(TaskWaitState&) noexcept {}
inline void WaitOnPollExit(TaskWaitState&, bool, bool) noexcept {}

#endif

}  // namespace kds::sched
