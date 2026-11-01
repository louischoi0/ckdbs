#include "kds/sched/wait_state.hpp"

#include <cassert>
#include <cstdio>
#include <mutex>

namespace kds::sched {

namespace {

// One reactor thread per core. The counters views read are in a static slot
// per core id; a thread that never binds one (a test, the mount) books to a
// private set nobody reads. Both are constant-initialised, so a hook pays no
// per-thread init guard.
CoreWaitTotals g_cores[kWaitCores];
constinit thread_local CoreWaitContext t_ctx;
thread_local CoreWaitTotals t_private;
const SystemClock g_system_clock;

CoreWaitContext& Ctx() noexcept { return t_ctx; }
CoreWaitTotals& TotalsOf(CoreWaitContext& c) noexcept {
    return c.shared != nullptr ? *c.shared : t_private;
}

constexpr std::size_t Index(stats::WaitEvent e) noexcept { return static_cast<std::size_t>(e); }

std::mutex g_sessions_mu;
std::vector<SessionWaitStats*> g_sessions;
std::uint64_t g_next_session_id = 0;

}  // namespace

std::shared_ptr<SessionWaitStats> MakeSessionWaitStats() {
    auto* stats = new SessionWaitStats();
    std::shared_ptr<SessionWaitStats> owner(stats, [](SessionWaitStats* gone) {
        {
            std::lock_guard<std::mutex> guard(g_sessions_mu);
            std::erase(g_sessions, gone);
        }
        delete gone;
    });
    std::lock_guard<std::mutex> guard(g_sessions_mu);
    stats->session_id = ++g_next_session_id;
    // Between statements a session is idle on its client, from now.
    stats->current_state.set(static_cast<std::uint64_t>(TaskState::kIdle));
    stats->current_event.set(static_cast<std::uint64_t>(stats::WaitEvent::kIdleClient));
    stats->current_since_ns.set(WaitNow());
    g_sessions.push_back(stats);
    return owner;
}

std::vector<SessionWaitStats> SnapshotSessionWaits() {
    std::lock_guard<std::mutex> guard(g_sessions_mu);
    std::vector<SessionWaitStats> out;
    out.reserve(g_sessions.size());
    for (const SessionWaitStats* s : g_sessions) out.push_back(*s);
    return out;
}

std::string FormatDbTimeLine(const SessionWaitStats& before, const SessionWaitStats& after) {
    static_assert(Index(stats::WaitEvent::kOnCpu) == 0, "ON_CPU is kept beside the table, not in it");
    const auto delta = [](std::uint64_t a, std::uint64_t b) { return a > b ? a - b : 0; };
    // Units of 10 us, so a part and the total print without a visible gap.
    const auto units = [](std::uint64_t ns) { return (ns + 5'000) / 10'000; };
    const auto ms = [](std::uint64_t u) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%llu.%02llu", static_cast<unsigned long long>(u / 100),
                      static_cast<unsigned long long>(u % 100));
        return std::string(buf);
    };
    const std::uint64_t cpu = units(delta(after.on_cpu_ns.get(), before.on_cpu_ns.get()));
    std::uint64_t total = cpu;
    std::string parts = " = CPU " + ms(cpu);
    for (std::size_t i = 1; i < stats::kWaitEventCount; ++i) {
        const stats::WaitEventDef& d = stats::kWaitEvents[i];
        if (d.kind != stats::WaitKind::kTimed || !d.in_db_time) continue;
        const std::uint64_t t = units(delta(after.events[i].total_ns.get(), before.events[i].total_ns.get()));
        if (t == 0) continue;
        total += t;
        std::string_view name = d.name;
        if (i == Index(stats::WaitEvent::kSchedRunQueue)) name = "run_queue";
        parts += " + " + std::string(name) + " " + ms(t);
    }
    return "DB time " + ms(total) + " ms" + parts;
}

bool RunningTaskBooksTo(const SessionWaitStats* session) noexcept {
    const TaskWaitState* t = Ctx().current;
    return t != nullptr && session != nullptr && t->session == session;
}

void BindWaitContext(std::uint32_t core) noexcept {
    CoreWaitTotals& t = g_cores[core % kWaitCores];
    t.bound.store(true, std::memory_order_release);
    t_ctx.shared = &t;
}

const CoreWaitTotals* WaitContextOfCore(std::uint32_t core) noexcept {
    const CoreWaitTotals& t = g_cores[core % kWaitCores];
    return t.bound.load(std::memory_order_acquire) ? &t : nullptr;
}

CoreWaitTotals& WaitTotals() noexcept { return TotalsOf(Ctx()); }

CoreWaitContext& WaitContext() noexcept { return Ctx(); }

void SetWaitClock(const Clock* clock) noexcept { Ctx().clock = clock; }

MonoTimeNs WaitNow() noexcept {
    const Clock* const clock = Ctx().clock;
    return clock != nullptr ? clock->Now() : g_system_clock.Now();
}

void SetStrictWaitScopes(bool strict) noexcept { Ctx().strict_scopes = strict; }

void ResetWaitContext() noexcept {
    CoreWaitContext& c = Ctx();
    CoreWaitTotals& t = TotalsOf(c);
    t.totals = {};
    t.on_cpu_ns.set(0);
    t.unscoped_parks.set(0);
    c.current = nullptr;
}

#ifndef KDS_WAIT_INSTRUMENTATION_OFF

namespace {

// `c` is the caller's `Ctx()`, read once per hook: the TLS pointer is
// reloaded after every out-of-line call (`WaitNow`) otherwise.
void Book(CoreWaitContext& c, stats::WaitEvent event, MonoTimeNs elapsed_ns,
          SessionWaitStats* session) noexcept {
    EventTotals& t = TotalsOf(c).totals[Index(event)];
    t.count.add(1);
    t.total_ns.add(elapsed_ns);
    t.max_ns.raise_to(elapsed_ns);
    t.histogram[WaitBucketOf(elapsed_ns)].add(1);
    if (session != nullptr) {
        SessionWaitStats::PerEvent& e = session->events[Index(event)];
        e.count.add(1);
        e.total_ns.add(elapsed_ns);
    }
}

// An ON_CPU segment of `task`'s: the core's and the session's.
void BookCpu(CoreWaitContext& c, const TaskWaitState& task, MonoTimeNs elapsed_ns) noexcept {
    TotalsOf(c).on_cpu_ns.add(elapsed_ns);
    if (task.session != nullptr) task.session->on_cpu_ns.add(elapsed_ns);
}

// The task's state, for `sys.session_state`.
void Publish(const TaskWaitState& task) noexcept {
    SessionWaitStats* s = task.session;
    if (s == nullptr) return;
    s->current_state.set(static_cast<std::uint64_t>(task.state));
    s->current_event.set(static_cast<std::uint64_t>(task.event));
    s->current_p1.set(task.p1);
    s->current_p2.set(task.p2);
    s->current_p3.set(task.p3);
    s->current_since_ns.set(task.since_ns);
}

}  // namespace

WaitScope::WaitScope(stats::WaitEvent event, std::uint64_t p1, std::uint32_t p2,
                     std::uint32_t p3) noexcept
    : event_(event), start_ns_(WaitNow()), task_(Ctx().current) {
    if (task_ == nullptr) return;
    // No nesting (§2.5): an open scope means this one is a second wait inside
    // the first.
    assert(!task_->scoped && "a WaitScope inside a WaitScope");
    CoreWaitContext& c = Ctx();
    BookCpu(c, *task_, start_ns_ - c.cpu_since);  // the segment ends
    task_->state = stats::Def(event).in_db_time ? TaskState::kWaiting : TaskState::kIdle;
    task_->event = event;
    task_->p1 = p1;
    task_->p2 = p2;
    task_->p3 = p3;
    task_->since_ns = start_ns_;
    task_->scoped = true;
    Publish(*task_);
}

WaitScope::~WaitScope() {
    // A scope destroyed with its task not running (the task dropped while
    // parked, or dropped from inside another task's poll) is an abandoned
    // wait: booked to nobody, and it touches no running task's clock (I4).
    if (task_ != nullptr && task_ != Ctx().current) return;
    const MonoTimeNs now = WaitNow();
    CoreWaitContext& c = Ctx();
    Book(c, event_, now - start_ns_, task_ != nullptr ? task_->session : nullptr);
    if (task_ == nullptr) return;
    c.cpu_since = now;  // a new ON_CPU segment
    task_->scoped = false;
    task_->event = stats::WaitEvent::kOnCpu;
    task_->p1 = 0;
    task_->p2 = 0;
    task_->p3 = 0;
    task_->state = TaskState::kOnCpu;
    task_->since_ns = now;
    Publish(*task_);
}

void WaitScope::Reclassify(stats::WaitEvent event, std::uint32_t p2) noexcept {
    event_ = event;
    if (task_ == nullptr) return;
    task_->event = event;
    task_->p2 = p2;
    Publish(*task_);
}

void RecordCounterEvent(stats::WaitEvent event) noexcept {
    assert(stats::Def(event).kind == stats::WaitKind::kCounter);
    CoreWaitContext& c = Ctx();
    Book(c, event, 0, c.current != nullptr ? c.current->session : nullptr);
}

void WaitOnPollEnter(TaskWaitState& task) noexcept {
    CoreWaitContext& c = Ctx();
    c.current = &task;
    const MonoTimeNs now = WaitNow();
    if (task.scoped) return;  // still inside its scope: nothing of the wait ends here
    if (task.state == TaskState::kRunnable) {
        // The queue wait ends: WD9's `sched.run_queue`.
        Book(c, stats::WaitEvent::kSchedRunQueue, now - task.since_ns, task.session);
    }
    task.state = TaskState::kOnCpu;
    task.event = stats::WaitEvent::kOnCpu;
    task.since_ns = now;
    c.cpu_since = now;
    Publish(task);
}

void FlushOnCpu() noexcept {
    CoreWaitContext& c = Ctx();
    if (c.current == nullptr || c.current->scoped) return;
    const MonoTimeNs now = WaitNow();
    BookCpu(c, *c.current, now - c.cpu_since);
    c.cpu_since = now;
}

void WaitOnPollExit(TaskWaitState& task, bool parked, bool done) noexcept {
    CoreWaitContext& c = Ctx();
    c.current = nullptr;
    // A scoped task's ON_CPU segment ended when its scope opened.
    if (task.scoped) return;
    const MonoTimeNs now = WaitNow();
    BookCpu(c, task, now - c.cpu_since);
    if (done) {
        // The statement ended: the session now waits on its client.
        if (task.session != nullptr) {
            task.state = TaskState::kIdle;
            task.event = stats::WaitEvent::kIdleClient;
            task.since_ns = now;
            Publish(task);
        }
        return;
    }
    if (parked) {
        // I2: a park outside every WaitScope. Its time is booked to no event.
        assert(!c.strict_scopes && "a suspension outside every WaitScope (I2)");
        TotalsOf(c).unscoped_parks.add(1);
        task.state = TaskState::kWaiting;
        task.event = stats::WaitEvent::kOnCpu;
    } else {
        // Yielded: re-queued, waiting for its next turn.
        task.state = TaskState::kRunnable;
    }
    task.since_ns = now;
    Publish(task);
}

#endif

}  // namespace kds::sched
