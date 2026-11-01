#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
//   WAITING   a `WaitScope` is open, or `Poll` found a park with none: I2's
//             violation. Counted in `CoreWaitContext::unscoped_parks`, its
//             time booked to no event, and an `assert` where
//             `CoreWaitContext::strict_scopes` is set (the server's reactor
//             threads set it; a unit test's synthetic coroutine, which parks
//             on a bare `WaitFor`, does not).
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

// A counter its core writes and any core may read (WE05's views are read on
// the session's core, the totals belong to every core). One writer, so an
// add is a relaxed load and store, not a read-modify-write: a plain `mov` on
// the targets we build for. Converts to its value so a test compares it as a
// number.
class RelaxedU64 {
public:
    RelaxedU64() noexcept = default;
    RelaxedU64(const RelaxedU64& o) noexcept : v_(o.get()) {}
    RelaxedU64& operator=(const RelaxedU64& o) noexcept {
        set(o.get());
        return *this;
    }
    std::uint64_t get() const noexcept { return v_.load(std::memory_order_relaxed); }
    void set(std::uint64_t v) noexcept { v_.store(v, std::memory_order_relaxed); }
    void add(std::uint64_t d) noexcept { set(get() + d); }
    void raise_to(std::uint64_t v) noexcept {
        if (v > get()) set(v);
    }
    operator std::uint64_t() const noexcept { return get(); }  // NOLINT: a number in a comparison

private:
    std::atomic<std::uint64_t> v_{0};
};

// Histogram bucket k is [2^k, 2^(k+1)) microseconds; 0 us falls in bucket 0
// and the last bucket takes everything above (WD5, §2.6).
inline constexpr std::size_t kWaitHistogramBuckets = 32;

constexpr std::size_t WaitBucketOf(std::uint64_t ns) noexcept {
    const std::size_t k = static_cast<std::size_t>(std::bit_width((ns / 1000) | 1)) - 1;
    return k < kWaitHistogramBuckets ? k : kWaitHistogramBuckets - 1;
}

// A core's cumulative wait time per event (WE05). Fixed size, no allocation.
struct EventTotals {
    RelaxedU64 count;
    RelaxedU64 total_ns;
    RelaxedU64 max_ns;
    std::array<RelaxedU64, kWaitHistogramBuckets> histogram;
};

// What a session accumulates (§2.6): count and total per event, and its CPU
// time. Its RUNNABLE time is its `sched.run_queue` row. The `current_*` words
// publish the session's task state for `sys.session_state`, written by the
// core running the task and read by any.
struct SessionWaitStats {
    struct PerEvent {
        RelaxedU64 count;
        RelaxedU64 total_ns;
    };
    std::uint64_t session_id = 0;
    std::array<PerEvent, stats::kWaitEventCount> events{};
    RelaxedU64 on_cpu_ns;
    RelaxedU64 current_state;   // `TaskState`
    RelaxedU64 current_event;   // `stats::WaitEvent`
    RelaxedU64 current_p1;
    RelaxedU64 current_p2;
    RelaxedU64 current_p3;
    RelaxedU64 current_since_ns;
    // The running statement, for ASH (WE07): its shape's fingerprint (the
    // `fetch_id` the pattern system keys on, 0 for a kind that has none) and,
    // when the statement has one, the argument hash (WD11's opt-in rides on it).
    RelaxedU64 fingerprint;
    RelaxedU64 arg_hash;
};

struct TaskWaitState {
    TaskState state = TaskState::kRunnable;  // a task not yet polled waits its turn
    stats::WaitEvent event = stats::WaitEvent::kOnCpu;
    std::uint64_t p1 = 0;
    std::uint32_t p2 = 0;
    std::uint32_t p3 = 0;
    MonoTimeNs since_ns = 0;  // when `state` was entered; a scoped wait's is its scope's start
    bool scoped = false;      // a `WaitScope` (not `Poll`) opened the WAITING state
    // The session this task runs a statement for, set by the dispatcher; null
    // for a background task. Not owned: the session outlives its statement.
    SessionWaitStats* session = nullptr;
    // A background task's number in ASH records, given by the sampler on first
    // sight (0: not yet); a statement task is told by its session.
    std::uint32_t background_id = 0;
};

// What a core accumulates and any core may read: a static slot per core id
// (`BindWaitContext`). Two threads bound to one slot lose counts and nothing
// else, because everything a thread mutates besides these counters is in its
// own `CoreWaitContext`.
struct CoreWaitTotals {
    std::array<EventTotals, stats::kWaitEventCount> totals{};
    RelaxedU64 on_cpu_ns;       // ON_CPU is a state, not an event: kept beside the table
    RelaxedU64 unscoped_parks;  // I2 violations: parks no WaitScope covered
    std::atomic<bool> bound{false};  // a reactor owns this slot
};

// The calling thread's own state, thread-local and constant-initialised.
struct CoreWaitContext {
    const Clock* clock = nullptr;      // null: the process's SystemClock
    TaskWaitState* current = nullptr;  // the task being polled, null between polls
    CoreWaitTotals* shared = nullptr;  // the bound core's slot; null: the thread's private one
    bool strict_scopes = false;  // an unscoped park is an `assert`, not just a count
    // Where the running task's current ON_CPU segment began. A segment ends
    // when a `WaitScope` opens (a blocking section or a suspension) and a new
    // one begins when it closes or the task is next resumed outside one, so
    // an interval is ON_CPU or an event's, never both (I4).
    MonoTimeNs cpu_since = 0;
};

inline constexpr std::uint32_t kWaitCores = 64;  // `kMaxCores` (superblock.hpp)

// The instance's sessions that have run a statement, for the views. A session
// registers on its first statement and leaves with its last owner, so a
// session's statistics are dropped at its end (§2.6).
std::shared_ptr<SessionWaitStats> MakeSessionWaitStats();
// Copies of every live session's statistics.
std::vector<SessionWaitStats> SnapshotSessionWaits();

// Binds the calling thread to core `core`'s context, so other cores can read
// its totals. Threads that never call it (tests, mount) keep a private one.
void BindWaitContext(std::uint32_t core) noexcept;
// A bound core's context, or null for a core never bound.
const CoreWaitTotals* WaitContextOfCore(std::uint32_t core) noexcept;

// The calling thread's context (one reactor thread per core).
CoreWaitContext& WaitContext() noexcept;
// The totals the calling thread books to.
CoreWaitTotals& WaitTotals() noexcept;

// Installs `clock` for the calling thread (null restores `SystemClock`).
void SetWaitClock(const Clock* clock) noexcept;
MonoTimeNs WaitNow() noexcept;

// Makes an unscoped park on the calling thread an `assert` (I2) rather than a
// count. Set by the server's reactor threads.
void SetStrictWaitScopes(bool strict) noexcept;

// Zeroes the calling thread's totals and violation count. Tests.
void ResetWaitContext() noexcept;

// `ANALYZE`'s DB-time line (CJ's WE10, the order's §2.8), from a session's
// statistics before and after a statement:
//   `DB time 4.21 ms = CPU 1.02 + run_queue 0.11 + commit.wal_flush 2.95`
// The total is the sum of the parts, and the parts are the session's deltas
// (`sys.session_waits`), so the two views of the same statement agree. Idle
// events and counters are not DB time (WD6) and are left out; events appear in
// registry order, those with no time omitted.
// Each part is rounded to 10 us and the total is the sum of the rounded parts,
// so the line adds up as printed.
std::string FormatDbTimeLine(const SessionWaitStats& before, const SessionWaitStats& after);
// True when the task being polled on this thread books to `session`: the only
// case in which a statement's own time is in the session's statistics.
bool RunningTaskBooksTo(const SessionWaitStats* session) noexcept;

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
// Books the running task's open ON_CPU segment to its session now and starts
// the next from here, so a statement can read its own CPU time before it
// ends. Segments are additive: this changes no total, only when it is booked.
// A no-op when no task is being polled.
void FlushOnCpu() noexcept;

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
inline void FlushOnCpu() noexcept {}

#endif

}  // namespace kds::sched
