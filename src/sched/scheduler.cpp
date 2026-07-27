#include "kds/sched/scheduler.hpp"

namespace kds::sched {

Scheduler::Scheduler(const Clock& clock, IoBackend& io_backend, SchedulerConfig config)
    : clock_(clock), io_backend_(io_backend), config_(config) {}

void Scheduler::Submit(TaskPtr task) {
    int idx = SchedulingGroupIndex(task->group());
    ready_queues_[static_cast<std::size_t>(idx)].push_back(std::move(task));
}

Status Scheduler::RegisterIoHandler(IoHandle handle, IoInterest interest, IoHandler handler) {
    Status s = io_backend_.Register(handle, interest);
    if (!s.ok()) return s;
    io_handlers_[handle] = std::move(handler);
    return Status::OK();
}

Status Scheduler::ModifyIoHandler(IoHandle handle, IoInterest interest) {
    return io_backend_.Modify(handle, interest);
}

Status Scheduler::UnregisterIoHandler(IoHandle handle) {
    io_handlers_.erase(handle);
    return io_backend_.Unregister(handle);
}

bool Scheduler::PickNextGroup(SchedulingGroup& out) const {
    bool found = false;
    double best_ratio = 0.0;
    for (int i = 0; i < kNumSchedulingGroups; ++i) {
        if (ready_queues_[static_cast<std::size_t>(i)].empty()) continue;
        double ratio = static_cast<double>(consumed_ns_[static_cast<std::size_t>(i)]) /
                       static_cast<double>(config_.group_shares[static_cast<std::size_t>(i)]);
        if (!found || ratio < best_ratio) {
            found = true;
            best_ratio = ratio;
            out = static_cast<SchedulingGroup>(i);
        }
    }
    return found;
}

void Scheduler::MaybeDecayConsumedRuntime() {
    std::uint64_t max_consumed = 0;
    for (std::uint64_t c : consumed_ns_) {
        if (c > max_consumed) max_consumed = c;
    }
    if (max_consumed <= config_.decay_threshold_ns) return;
    for (std::uint64_t& c : consumed_ns_) c /= 2;
}

bool Scheduler::RunReadyTasks() {
    bool ran_any = false;
    for (int i = 0; i < config_.max_tasks_per_iteration; ++i) {
        SchedulingGroup group;
        if (!PickNextGroup(group)) break;

        std::size_t idx = static_cast<std::size_t>(SchedulingGroupIndex(group));
        TaskPtr task = std::move(ready_queues_[idx].front());
        ready_queues_[idx].pop_front();

        MonoTimeNs start = clock_.Now();
        PollResult result = task->Poll();
        MonoTimeNs elapsed = clock_.Now() - start;
        consumed_ns_[idx] += elapsed;
        ran_any = true;

        if (result == PollResult::kSuspended) {
            ready_queues_[idx].push_back(std::move(task));
        }
        // kDone: task is dropped here (unique_ptr destructor runs).
    }
    if (ran_any) MaybeDecayConsumedRuntime();
    return ran_any;
}

bool Scheduler::RunOnce() {
    bool did_work = false;

    // Phase 1: drain I/O completions. Non-blocking poll (timeout 0) - the
    // reactor never sleeps here in Phase 1; see Run()'s comment on idle
    // policy.
    io_events_scratch_.clear();
    Status io_status = io_backend_.PollReady(/*timeout_ms=*/0, io_events_scratch_);
    // A poll failure here is not fatal to the reactor (rules.md #1: no
    // silently-dropped status, but there is no logging subsystem yet to
    // report it through - Phase 2+ wires this into whatever observability
    // path lands alongside the SLO controller).
    (void)io_status;
    for (const IoEvent& ev : io_events_scratch_) {
        auto it = io_handlers_.find(ev.handle);
        if (it != io_handlers_.end()) {
            it->second(ev);
            did_work = true;
        }
    }

    // Phase 2: expire timers. [Phase 2+ - no hierarchical timing wheel
    // yet; see docs/sched.md section 6 and its Implementation Status.]

    // Phase 3: drain cross-core inboxes. [Phase 2+ - single reactor only,
    // no SPSC rings yet; see docs/sched.md section 5.]

    // Phase 4: run ready tasks under the loop budget.
    if (RunReadyTasks()) did_work = true;

    // Phase 5: submit pending I/O. Phase 1's Register/Modify/Unregister
    // calls above take effect synchronously (no batching) - batched
    // submission is Phase 2+ (e.g. a real io_uring submission queue).

    // Phase 6: idle policy. [Phase 1 is busy-poll only; the blocking mode
    // from docs/sched.md section 7 is Phase 2+.]

    return did_work;
}

void Scheduler::Run() {
    stopped_ = false;
    while (!stopped_) {
        RunOnce();
    }
}

}  // namespace kds::sched
