#include "kds/sched/wait_state.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <new>

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

// ---- The session registry: slots, no latch (the header's note) -------------
//
// Chunks of `kSlotsPerChunk` slots, published into a fixed directory by a
// compare-exchange and never freed, so a slot's address holds for the
// process's life and a reader may walk the directory while sessions come
// and go. `g_slots_made` is how many slots the bump has ever handed out; a
// given-back slot goes on a free list threaded through the slots, a Treiber
// stack whose head carries a tag in its high half, so a head that was
// popped and pushed again between a pop's load and its exchange is not
// mistaken for the same head.
constexpr std::uint32_t kSlotsPerChunk = 64;
constexpr std::uint32_t kSessionChunks = kMaxRegisteredSessions / kSlotsPerChunk;
constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;

struct SessionSlot {
    SessionWaitStats stats;
    std::atomic<std::uint32_t> next_free{kNoSlot};
};
struct SessionChunk {
    SessionSlot slots[kSlotsPerChunk];
};

std::atomic<SessionChunk*> g_session_chunks[kSessionChunks];
std::atomic<std::uint32_t> g_slots_made{0};
std::atomic<std::uint64_t> g_free_head{kNoSlot};  // tag:32 | index:32
std::atomic<std::uint64_t> g_next_session_id{0};

SessionSlot* SlotAt(std::uint32_t index) noexcept {
    SessionChunk* chunk =
        g_session_chunks[index / kSlotsPerChunk].load(std::memory_order_acquire);
    return chunk == nullptr ? nullptr : &chunk->slots[index % kSlotsPerChunk];
}

// A slot nobody holds: off the free list, else the next unused one, its
// chunk made if this is the chunk's first slot. `kNoSlot` where the registry
// is full or the chunk cannot be allocated.
std::uint32_t TakeSlot() noexcept {
    std::uint64_t head = g_free_head.load(std::memory_order_acquire);
    while (static_cast<std::uint32_t>(head) != kNoSlot) {
        const std::uint32_t index = static_cast<std::uint32_t>(head);
        // The slot is in a published chunk: it was handed out once.
        const std::uint32_t next = SlotAt(index)->next_free.load(std::memory_order_relaxed);
        const std::uint64_t popped = ((head >> 32) + 1) << 32 | next;
        if (g_free_head.compare_exchange_weak(head, popped, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            return index;
        }
    }
    std::uint32_t index = g_slots_made.load(std::memory_order_relaxed);
    do {
        if (index >= kMaxRegisteredSessions) return kNoSlot;
    } while (!g_slots_made.compare_exchange_weak(index, index + 1, std::memory_order_relaxed));
    std::atomic<SessionChunk*>& entry = g_session_chunks[index / kSlotsPerChunk];
    if (entry.load(std::memory_order_acquire) == nullptr) {
        // The one allocation, a chunk's first session.
        SessionChunk* fresh = new (std::nothrow) SessionChunk();
        SessionChunk* expected = nullptr;
        if (fresh != nullptr &&
            !entry.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            delete fresh;  // another session's creation published the chunk first
        }
        // No chunk: the index is spent and its slot is never listed.
        if (entry.load(std::memory_order_acquire) == nullptr) return kNoSlot;
    }
    return index;
}

void GiveSlot(std::uint32_t index) noexcept {
    SessionSlot* slot = SlotAt(index);
    // Zeroed first: a reader copying the words sees the id change and
    // leaves the row out.
    slot->stats.session_id.set(0);
    std::uint64_t head = g_free_head.load(std::memory_order_acquire);
    for (;;) {
        slot->next_free.store(static_cast<std::uint32_t>(head), std::memory_order_relaxed);
        const std::uint64_t pushed = ((head >> 32) + 1) << 32 | index;
        if (g_free_head.compare_exchange_weak(head, pushed, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            return;
        }
    }
}

// A session's words as its first statement finds them.
void StartSession(SessionWaitStats& stats) noexcept {
    // Between statements a session is idle on its client, from now.
    stats.current_state.set(static_cast<std::uint64_t>(TaskState::kIdle));
    stats.current_event.set(static_cast<std::uint64_t>(stats::WaitEvent::kIdleClient));
    stats.current_since_ns.set(WaitNow());
    // Last: the id is what lists the slot. The fence orders every word above
    // before it (paired with the reader's fence after its first read).
    std::atomic_thread_fence(std::memory_order_release);
    stats.session_id.set(g_next_session_id.fetch_add(1, std::memory_order_relaxed) + 1);
}

}  // namespace

std::shared_ptr<SessionWaitStats> MakeSessionWaitStats() {
    const std::uint32_t index = TakeSlot();
    if (index == kNoSlot) {
        // Kept and not listed (the header's note).
        auto unlisted = std::make_shared<SessionWaitStats>();
        StartSession(*unlisted);
        return unlisted;
    }
    SessionWaitStats* stats = &SlotAt(index)->stats;
    // The slot's last session left its words; they are cleared here, while
    // the id still reads 0 and no reader lists the slot. The fence orders the
    // give-back's zeroed id before these words: a reader mid-copy of the last
    // session that sees one of them sees the id moved on its second read.
    std::atomic_thread_fence(std::memory_order_release);
    *stats = SessionWaitStats{};
    // Should the owner's control block not be had, the deleter runs and the
    // slot goes back.
    std::shared_ptr<SessionWaitStats> owner(
        stats, [index](SessionWaitStats*) noexcept { GiveSlot(index); });
    StartSession(*stats);
    return owner;
}

std::vector<SessionWaitStats> SnapshotSessionWaits() {
    std::vector<SessionWaitStats> out;
    const std::uint32_t made = g_slots_made.load(std::memory_order_relaxed);
    for (std::uint32_t index = 0; index < made; ++index) {
        const SessionSlot* slot = SlotAt(index);
        if (slot == nullptr) continue;
        const std::uint64_t id = slot->stats.session_id.get();
        if (id == 0) continue;
        std::atomic_thread_fence(std::memory_order_acquire);  // `StartSession`'s pair
        out.push_back(slot->stats);
        // The slot changed hands under the copy: the row is two sessions'.
        std::atomic_thread_fence(std::memory_order_acquire);  // the take's pair
        if (slot->stats.session_id.get() != id) out.pop_back();
    }
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

SessionCostSample SampleSessionCost(const SessionWaitStats& session) noexcept {
    SessionCostSample out;
    out.on_cpu_ns = session.on_cpu_ns.get();
    for (std::size_t i = 1; i < stats::kWaitEventCount; ++i) {
        out.class_ns[static_cast<std::size_t>(stats::kWaitEvents[i].wait_class)] +=
            session.events[i].total_ns.get();
    }
    out.page_reads = session.events[Index(stats::WaitEvent::kIoPageRead)].count.get() +
                     session.events[Index(stats::WaitEvent::kIoVarHeapRead)].count.get() +
                     session.events[Index(stats::WaitEvent::kIoScanRingRead)].count.get();
    out.log_bytes = session.log_bytes.get();
    out.log_records = session.log_records.get();
    return out;
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
    if (abandoned_) {
        // No event: the span was the task's own time (`Abandon`).
        if (task_ != nullptr) BookCpu(c, *task_, now - start_ns_);
    } else {
        Book(c, event_, now - start_ns_, task_ != nullptr ? task_->session : nullptr);
    }
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
    if (task.scoped) return;  // still inside its scope: nothing of the wait ends here
    const MonoTimeNs now = WaitNow();
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

void BookLogAppend(std::uint64_t bytes) noexcept {
    const TaskWaitState* const task = Ctx().current;
    if (task == nullptr || task->session == nullptr) return;
    task->session->log_bytes.add(bytes);
    task->session->log_records.add(1);
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
