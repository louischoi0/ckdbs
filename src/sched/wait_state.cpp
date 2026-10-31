#include "kds/sched/wait_state.hpp"

#include <cassert>

namespace kds::sched {

namespace {

// One reactor thread per core: core-local, not atomic (I1).
thread_local CoreWaitContext g_context;
const SystemClock g_system_clock;

constexpr std::size_t Index(stats::WaitEvent e) noexcept { return static_cast<std::size_t>(e); }

}  // namespace

CoreWaitContext& WaitContext() noexcept { return g_context; }

void SetWaitClock(const Clock* clock) noexcept { g_context.clock = clock; }

MonoTimeNs WaitNow() noexcept {
    if (g_context.clock != nullptr) return g_context.clock->Now();
    return g_system_clock.Now();
}

void SetStrictWaitScopes(bool strict) noexcept { g_context.strict_scopes = strict; }

void ResetWaitContext() noexcept {
    g_context.totals = {};
    g_context.on_cpu_ns = 0;
    g_context.unscoped_parks = 0;
    g_context.current = nullptr;
}

#ifndef KDS_WAIT_INSTRUMENTATION_OFF

namespace {

void Book(stats::WaitEvent event, MonoTimeNs elapsed_ns) noexcept {
    EventTotals& t = g_context.totals[Index(event)];
    ++t.count;
    t.total_ns += elapsed_ns;
}

}  // namespace

WaitScope::WaitScope(stats::WaitEvent event, std::uint64_t p1, std::uint32_t p2,
                     std::uint32_t p3) noexcept
    : event_(event), start_ns_(WaitNow()), task_(g_context.current) {
    if (task_ == nullptr) return;
    // No nesting (§2.5): an open scope means this one is a second wait inside
    // the first.
    assert(!task_->scoped && "a WaitScope inside a WaitScope");
    g_context.on_cpu_ns += start_ns_ - g_context.cpu_since;  // the segment ends
    task_->state = stats::Def(event).in_db_time ? TaskState::kWaiting : TaskState::kIdle;
    task_->event = event;
    task_->p1 = p1;
    task_->p2 = p2;
    task_->p3 = p3;
    task_->since_ns = start_ns_;
    task_->scoped = true;
}

WaitScope::~WaitScope() {
    // A scope destroyed with its task not running (the task dropped while
    // parked, or dropped from inside another task's poll) is an abandoned
    // wait: booked to nobody, and it touches no running task's clock (I4).
    if (task_ != nullptr && task_ != g_context.current) return;
    const MonoTimeNs now = WaitNow();
    Book(event_, now - start_ns_);
    if (task_ == nullptr) return;
    g_context.cpu_since = now;  // a new ON_CPU segment
    task_->scoped = false;
    task_->event = stats::WaitEvent::kOnCpu;
    task_->p1 = 0;
    task_->p2 = 0;
    task_->p3 = 0;
    task_->state = TaskState::kOnCpu;
}

void WaitScope::Reclassify(stats::WaitEvent event, std::uint32_t p2) noexcept {
    event_ = event;
    if (task_ == nullptr) return;
    task_->event = event;
    task_->p2 = p2;
}

void RecordCounterEvent(stats::WaitEvent event) noexcept {
    assert(stats::Def(event).kind == stats::WaitKind::kCounter);
    Book(event, 0);
}

void WaitOnPollEnter(TaskWaitState& task) noexcept {
    g_context.current = &task;
    const MonoTimeNs now = WaitNow();
    if (task.scoped) return;  // still inside its scope: nothing of the wait ends here
    if (task.state == TaskState::kRunnable) {
        // The queue wait ends: WD9's `sched.run_queue`.
        Book(stats::WaitEvent::kSchedRunQueue, now - task.since_ns);
    }
    task.state = TaskState::kOnCpu;
    task.event = stats::WaitEvent::kOnCpu;
    task.since_ns = now;
    g_context.cpu_since = now;
}

void WaitOnPollExit(TaskWaitState& task, bool parked, bool done) noexcept {
    g_context.current = nullptr;
    // A scoped task's ON_CPU segment ended when its scope opened.
    if (task.scoped) return;
    const MonoTimeNs now = WaitNow();
    g_context.on_cpu_ns += now - g_context.cpu_since;
    if (done) return;
    if (parked) {
        // I2: a park outside every WaitScope. Its time is booked to no event.
        assert(!g_context.strict_scopes && "a suspension outside every WaitScope (I2)");
        ++g_context.unscoped_parks;
        task.state = TaskState::kWaiting;
        task.event = stats::WaitEvent::kOnCpu;
    } else {
        // Yielded: re-queued, waiting for its next turn.
        task.state = TaskState::kRunnable;
    }
    task.since_ns = now;
}

#endif

}  // namespace kds::sched
