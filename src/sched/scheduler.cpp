#include "kds/sched/scheduler.hpp"

#include <atomic>

#include <algorithm>
#include <utility>

namespace kds::sched {

Scheduler::Scheduler(const Clock& clock, IoBackend& io_backend, SchedulerConfig config)
    : clock_(clock), io_backend_(io_backend), config_(config) {
    // RunReadyTasks' second floor - one poll per ready group per iteration -
    // needs a budget of at least one poll per group, or a small budget
    // would hand every iteration to group 0 and starve the rest outright
    // (the PW7 review's C3). Clamped here so the floor is a fact of the
    // scheduler, not of the configuration. Zero keeps its meaning - phase 4
    // runs nothing at all, which is how a test observes the drain alone.
    if (config_.max_tasks_per_iteration > 0 &&
        config_.max_tasks_per_iteration < kNumSchedulingGroups) {
        config_.max_tasks_per_iteration = kNumSchedulingGroups;
    }
}

void Scheduler::Submit(TaskPtr task) {
    int idx = SchedulingGroupIndex(task->group());
    ready_queues_[static_cast<std::size_t>(idx)].push_back(std::move(task));
    // Counted, not flagged: RunOnce compares the count across its whole
    // body, so a task queued by an io handler in phase 1 and one queued by
    // another task in phase 4 are both seen, and neither needs to know the
    // idle policy exists.
    ++submits_;
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

Status Scheduler::AttachTransport(RingTransport* transport, std::uint32_t core_id) {
    transport_ = transport;
    core_id_ = core_id;
    if (transport_ == nullptr) return Status::OK();

    // **The wake path** (waker.hpp). Armed here rather than at
    // construction because this is the first moment a peer exists to be
    // woken by, and because a single-core reactor must not pay for a
    // handle it can never need.
    auto waker = Waker::Create();
    if (!waker.ok()) return waker.status();
    waker_.emplace(std::move(waker.value()));

    if (Status s = io_backend_.Register(waker_->handle(), IoInterest::kReadable); !s.ok()) {
        waker_.reset();
        return s;
    }
    // Draining is all the handler does: the wake carries no data, the ring
    // is the queue, and phase 3 of this same iteration is what reads it.
    io_handlers_[waker_->handle()] = [this](const IoEvent&) {
        waker_->Drain();
        // D7's `sched_spurious_wakes`: read once, after phase 3, against
        // whether that drain found anything.
        woken_by_waker_ = true;
    };

    // Published last, so no peer can find a target that is not yet
    // registered. Safe unsynchronised: this runs on the startup thread
    // before this core's worker exists, which is where every other per-core
    // wiring in this engine is done.
    transport_->SetWakeTarget(core_id_, WakeTarget{&sleeping_, &*waker_});
    return Status::OK();
}

Status Scheduler::RegisterMessageHandler(RingMessageKind kind, MessageHandler handler) {
    const auto raw = static_cast<std::uint16_t>(kind);
    if (!IsKnownRingMessageKind(raw)) {
        // Including kUnset, which is the point of kUnset. A handler bound
        // to a number no sender can produce is a no-op nobody would notice,
        // and the central kind enum exists precisely so that number does
        // not exist.
        return Status::InvalidArgument("scheduler: message kind " + std::to_string(raw) +
                                       " is not one this build knows");
    }
    message_handlers_[raw] = std::move(handler);
    return Status::OK();
}

bool Scheduler::DrainInbox() {
    if (transport_ == nullptr) return false;

    bool did_work = false;
    for (int drained = 0; drained < config_.max_messages_per_iteration; ++drained) {
        MessageHeader header{};
        if (!transport_->TryReceive(core_id_, header, message_payload_scratch_)) break;

        did_work = true;
        ++messages_drained_;

        auto it = message_handlers_.find(header.kind);
        if (it == message_handlers_.end()) {
            // Dropped, not failed. A message with no handler here is the
            // same situation as one whose tag matches no live pipeline
            // state - normal operation under workplan guideline 5, since a
            // request can be torn down while its messages are still in
            // flight. It is logged because the *other* reading, a handler
            // somebody forgot to register, looks identical from here and
            // would otherwise be invisible.
            if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
                log_->Debug("sched", std::string("dropped a ") +
                                         RingMessageKindName(
                                             static_cast<RingMessageKind>(header.kind)) +
                                         " message from core " + std::to_string(header.src_core) +
                                         ": no handler on core " + std::to_string(core_id_));
            }
            continue;
        }

        // The handler runs inside a task, not here. Phase 3's job is to
        // move messages out of the ring - doing the work in the drain would
        // put an unbounded amount of it inside a phase that has to stay
        // cheap, which is the same contract the phase-1 io handlers are
        // under.
        //
        // The payload is copied into the task because the scratch buffer is
        // overwritten by the very next iteration of this loop, and the task
        // does not run until phase 4.
        Submit(std::make_unique<FunctionTask>(
            GroupOf(header),
            [handler = it->second, header,
             payload = message_payload_scratch_]() mutable -> PollResult {
                handler(header, std::span<const std::byte>(payload));
                return PollResult::kDone;
            }));
    }
    return did_work;
}

TimerId Scheduler::ArmTimer(MonoTimeNs deadline, MonoTimeNs period_ns,
                            std::function<void()> fn) {
    const TimerId id = next_timer_id_++;
    timers_.push_back(Timer{deadline, period_ns, id, std::move(fn)});
    std::push_heap(timers_.begin(), timers_.end(), LaterDeadlineFirst{});
    return id;
}

TimerId Scheduler::SubmitAt(MonoTimeNs deadline, std::function<void()> fn) {
    return ArmTimer(deadline, /*period_ns=*/0, std::move(fn));
}

TimerId Scheduler::SubmitEvery(MonoTimeNs period_ns, std::function<void()> fn) {
    // A zero period would re-arm for the same instant forever and starve
    // everything else in phase 2; one nanosecond is still effectively
    // "every iteration" but always makes progress.
    if (period_ns == 0) period_ns = 1;
    return ArmTimer(clock_.Now() + period_ns, period_ns, std::move(fn));
}

void Scheduler::CancelTimer(TimerId id) {
    if (id == kInvalidTimerId) return;
    // Tombstoned rather than removed: erasing from the middle of a heap
    // means a rebuild, and cancelling from inside a callback would
    // invalidate the entry phase 2 is holding. The tombstone is consumed
    // when the timer surfaces.
    cancelled_timers_.insert(id);
}

bool Scheduler::ExpireTimers() {
    bool fired_any = false;
    const MonoTimeNs now = clock_.Now();

    while (!timers_.empty()) {
        const Timer& top = timers_.front();

        if (cancelled_timers_.erase(top.id) > 0) {
            std::pop_heap(timers_.begin(), timers_.end(), LaterDeadlineFirst{});
            timers_.pop_back();
            continue;
        }
        if (top.deadline > now) break;  // heap is ordered; nothing else is due

        // Moved out before firing: the callback may arm or cancel timers,
        // which can reallocate the heap out from under a reference into it.
        std::pop_heap(timers_.begin(), timers_.end(), LaterDeadlineFirst{});
        Timer due = std::move(timers_.back());
        timers_.pop_back();

        if (due.period_ns != 0) {
            // Re-armed from the deadline, not from completion, so a slow
            // callback does not push the interval outward.
            //
            // Missed periods **coalesce**: the next deadline skips to the
            // first instant strictly after now, so a reactor that stalled
            // for ten periods fires once on recovery, not ten times. For
            // the work this carries - the checkpoint cadence - a burst of
            // back-to-back runs after a stall is the opposite of what the
            // cadence is for, and nothing here is a tick counter that a
            // caller could be counting on to be conserved.
            MonoTimeNs next = due.deadline + due.period_ns;
            if (next <= now) {
                const MonoTimeNs behind = now - next;
                next += ((behind / due.period_ns) + 1) * due.period_ns;
            }
            const TimerId id = due.id;
            std::function<void()> fn = due.fn;
            timers_.push_back(Timer{next, due.period_ns, id, std::move(fn)});
            std::push_heap(timers_.begin(), timers_.end(), LaterDeadlineFirst{});
        }

        due.fn();
        fired_any = true;
    }
    return fired_any;
}

bool Scheduler::HasReadyTask() const noexcept {
    for (const auto& queue : ready_queues_) {
        if (!queue.empty()) return true;
    }
    return false;
}

int Scheduler::IdleTimeoutMs() const noexcept {
    // **Anything runnable means do not sleep at all - and a parked
    // coroutine is not runnable** (sched.md §7).
    //
    // A task that suspends goes back on its queue, so a queue is a poor
    // test of whether there is work: a statement waiting for a peer's reply
    // sits in one for the whole wait, answering "still false" to a
    // predicate a few nanoseconds at a time. Reading that as runnable is
    // what made a reactor with a single parked waiter burn ~90% of a core
    // (`bench/v2.2.0/results-shipping-ssb-v2.2.0-11-g982e133.md` §7: 3.1M
    // polls/s at 0.059 µs each, the cost per poll flat while the count
    // climbed).
    //
    // The test is therefore not "is a queue non-empty" but "did the last
    // full iteration change anything" - any I/O event, any timer, any
    // message drained, any task that completed or executed a line, any
    // task newly submitted, or the post-task hook doing work. If none of
    // that happened, polling the same queue again cannot produce a
    // different answer, and only an outside event can: the three that
    // exist are an fd, the timer that caps this block, and a peer's wake.
    //
    // Conservative in the one direction that matters: anything unaccounted
    // reads as *advanced*, costing a wasted iteration, never a missed wake.
    if (HasReadyTask() && last_iteration_advanced_) return 0;

    int timeout = config_.max_idle_block_ms;
    if (timeout < 0) timeout = 0;
    if (timers_.empty()) return timeout;

    const MonoTimeNs now = clock_.Now();
    const MonoTimeNs deadline = timers_.front().deadline;
    if (deadline <= now) return 0;  // already due

    // Rounded up: sleeping slightly long is a late timer, sleeping short is
    // a spin. Late is the cheaper mistake.
    const MonoTimeNs remaining_ms = (deadline - now + 999'999) / 1'000'000;
    if (remaining_ms < static_cast<MonoTimeNs>(timeout)) {
        timeout = static_cast<int>(remaining_ms);
    }
    return timeout;
}

bool Scheduler::PickNextGroup(const std::array<std::size_t, kNumSchedulingGroups>& remaining,
                              SchedulingGroup& out) const {
    bool found = false;
    double best_ratio = 0.0;
    for (int i = 0; i < kNumSchedulingGroups; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        if (remaining[idx] == 0) continue;
        double ratio = static_cast<double>(consumed_ns_[idx]) /
                       static_cast<double>(config_.group_shares[idx]);
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

bool Scheduler::RunReadyTasks(bool& advanced) {
    // The round's population is what was ready when it began: a task polled
    // this round that suspends goes to the back of its queue and is not
    // polled again until the next iteration - `remaining` counts it out,
    // and a task submitted by a poll waits for the next round too.
    //
    // Two floors under the share law, both forced by the lease-refill
    // trace (docs/inflight/in-progress/workplan-peer-writer.md PW7; sched.md §4 carries them).
    // A *parked* coroutine answers kSuspended in nanoseconds. Without the
    // first floor the loop budget re-polled one parked system task up to
    // 64 times an iteration and charged every poll to a group with share
    // 50, so the group ran up a debt the foreground (share 1000, and mostly
    // idle inside polls - a statement's time is the drain's fdatasync,
    // outside any group's account) took hundreds of iterations to match;
    // without the second, the next system task - the next relation's
    // refill - sat behind that debt for 395 iterations before its first
    // poll. The share law governs everything past one poll per group.
    std::array<std::size_t, kNumSchedulingGroups> remaining{};
    for (std::size_t i = 0; i < static_cast<std::size_t>(kNumSchedulingGroups); ++i) {
        remaining[i] = ready_queues_[i].size();
    }
    int polled = 0;
    bool ran_any = false;
    const auto poll_one = [&](std::size_t idx) {
        TaskPtr task = std::move(ready_queues_[idx].front());
        ready_queues_[idx].pop_front();
        --remaining[idx];

        MonoTimeNs start = clock_.Now();
        PollResult result = task->Poll();
        const MonoTimeNs spent = clock_.Now() - start;
        consumed_ns_[idx] += spent;
        // The undecayed pair; scheduler.hpp's accessors say why.
        polled_ns_total_[idx] += spent;
        ++polls_total_[idx];
        ran_any = true;
        ++polled;

        // **Did this poll do anything?** A completed task plainly did; a
        // suspended one only if it executed code rather than re-asking a
        // predicate that is still false (Task::advanced_in_last_poll). This
        // is the whole input to the idle policy's "parked is not ready",
        // and it is read before the task is moved back onto its queue.
        if (result == PollResult::kDone || task->advanced_in_last_poll()) advanced = true;

        if (result == PollResult::kSuspended) {
            ready_queues_[idx].push_back(std::move(task));
        }
        // kDone: task is dropped here (unique_ptr destructor runs).
    };

    // Floor two: one poll for every group with a task ready this round.
    for (std::size_t i = 0; i < static_cast<std::size_t>(kNumSchedulingGroups) &&
                            polled < config_.max_tasks_per_iteration;
         ++i) {
        if (remaining[i] > 0) poll_one(i);
    }
    // Then share-proportional picking over the rest of the round.
    while (polled < config_.max_tasks_per_iteration) {
        SchedulingGroup group;
        if (!PickNextGroup(remaining, group)) break;
        poll_one(static_cast<std::size_t>(SchedulingGroupIndex(group)));
    }
    if (ran_any) MaybeDecayConsumedRuntime();
    return ran_any;
}

bool Scheduler::RunOnce() {
    ++iterations_;
    // The wall-clock origin for group accounting (scheduler.hpp's
    // run_wall_ns): the first iteration, not construction.
    if (!run_started_) {
        run_start_ns_ = clock_.Now();
        run_started_ = true;
    }
    bool did_work = false;
    // **Did this iteration change anything?** Stricter than `did_work` and
    // kept separate from it: `did_work` says the reactor was busy, this
    // says the world moved. A round of polls that all found their
    // predicates still false is `did_work == true` and `advanced == false`,
    // and it is exactly the round the reactor may sleep after. Read by the
    // *next* iteration's IdleTimeoutMs.
    //
    // A task submitted mid-iteration counts, wherever it came from - an io
    // handler, a message, another task - which is why the count is taken
    // across the whole body rather than at any one phase.
    bool advanced = false;
    const std::uint64_t submits_at_entry = submits_;
    woken_by_waker_ = false;

    // Phase 1: drain I/O completions. The wait is phase 6's idle policy
    // pulled to the front, which is where a reactor can actually sleep: 0
    // when anything is runnable, otherwise up to the next timer deadline
    // (sched.md section 7).
    io_events_scratch_.clear();
    int timeout_ms = IdleTimeoutMs();

    // **Raise the flag before blocking, then look once more** (waker.hpp).
    //
    // The flag is what lets a peer know this reactor must be woken; the
    // re-check is what closes the window the flag opens. A sender that
    // enqueued between this iteration's phase-3 drain and the store below
    // read the flag as clear and skipped the wake, so its message would
    // wait out the whole block — the millisecond this path exists to
    // remove. The fence makes the pair a store-buffer, and sequential
    // consistency forbids both sides reading stale: `TrySend` carries the
    // argument.
    //
    // Only when this iteration would actually sleep. A timeout of 0 is a
    // reactor with work to do, and it neither needs waking nor may pay two
    // atomics to say so.
    const bool may_sleep = timeout_ms != 0 && transport_ != nullptr && waker_.has_value();
    // The block this iteration is about to take is one the reactor used to
    // spin through whenever a parked task was queued. Counted where the
    // decision is visible.
    if (timeout_ms != 0 && HasReadyTask()) {
        parked_idle_blocks_.fetch_add(1, std::memory_order_relaxed);
    }
    if (may_sleep) {
        sleeping_.store(true, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (transport_->HasPending(core_id_)) {
            sleeping_.store(false, std::memory_order_seq_cst);
            timeout_ms = 0;
            wake_race_skips_.fetch_add(1, std::memory_order_relaxed);
        } else {
            idle_blocks_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // The block itself, timed (D7). Two clock reads, and only on an
    // iteration that was about to sleep - the same "by construction there
    // is nothing to run" argument the counters above stand on.
    const MonoTimeNs block_entry_ns = timeout_ms != 0 ? clock_.Now() : 0;
    Status io_status = io_backend_.PollReady(timeout_ms, io_events_scratch_);
    if (timeout_ms != 0) {
        const MonoTimeNs now = clock_.Now();
        if (now > block_entry_ns) idle_block_ns_ += static_cast<std::uint64_t>(now - block_entry_ns);
    }
    // Cleared the moment the block ends, so a wake written from here on is
    // one this reactor did not need - which costs a syscall, never a
    // missed message.
    if (may_sleep) sleeping_.store(false, std::memory_order_seq_cst);
    // A poll failure here is not fatal to the reactor: the loop keeps
    // running and the next iteration may succeed. But it is the top of the
    // stack, so nothing else can report it - it goes to the log, and only
    // on the transitions, because a permanently broken backend fails on
    // every iteration and would otherwise write a line per spin.
    if (!io_status.ok()) {
        ++consecutive_io_failures_;
        if (consecutive_io_failures_ == 1 && log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("sched", "io backend poll failed: " + io_status.message());
        }
    } else if (consecutive_io_failures_ > 0) {
        if (log_ != nullptr && log_->enabled(LogLevel::kWarn)) {
            log_->Warn("sched", "io backend recovered after " +
                                    std::to_string(consecutive_io_failures_) +
                                    " failed poll(s)");
        }
        consecutive_io_failures_ = 0;
    }
    for (const IoEvent& ev : io_events_scratch_) {
        auto it = io_handlers_.find(ev.handle);
        if (it != io_handlers_.end()) {
            it->second(ev);
            did_work = true;
            // An outside event: whatever a parked task is waiting for, this
            // is one of the three things that can have changed it.
            advanced = true;
        }
    }

    // Phase 2: expire timers. Min-heap, not the hierarchical timing wheel
    // of sched.md section 6 - see SubmitAt()'s comment on why, and on what
    // replacing it would cost.
    if (ExpireTimers()) {
        did_work = true;
        advanced = true;
    }

    // Phase 3: drain cross-core inboxes (sched.md section 5). A no-op with
    // no transport attached, which is every single-core build.
    const bool drained = DrainInbox();
    if (drained) {
        did_work = true;
        advanced = true;
    }
    // A wake that ended the block and left the inbox empty. Ordinary
    // rather than wrong - the sender publishes before it wakes, so the
    // message can be drained by the iteration that raced the flag, one
    // ahead of the eventfd being read (scheduler.hpp's accessor).
    if (woken_by_waker_ && !drained) ++spurious_wakes_;

    // Phase 4: run ready tasks under the loop budget.
    if (RunReadyTasks(advanced)) did_work = true;

    // Phase 4.5: the post-task hook (see SetPostTaskHook). One call per
    // iteration, after every task that could stage work has had its turn -
    // which is what lets one device sync cover every commit staged this
    // iteration instead of one per commit.
    //
    // **Its answer feeds the idle policy**, and this is the one place the
    // "parked is not ready" rule could have broken a durability path: a
    // committing statement parks on `durable_lsn`, and the only thing that
    // moves it is this hook. A hook that synced and said nothing happened
    // would let the next iteration block before re-polling the waiter,
    // putting the drain interval on every commit.
    if (post_task_hook_ && post_task_hook_()) {
        did_work = true;
        advanced = true;
    }

    // A task submitted at any point this iteration is work nobody has
    // looked at yet, whoever queued it.
    if (submits_ != submits_at_entry) advanced = true;
    last_iteration_advanced_ = advanced;

    // Phase 5: submit pending I/O. Phase 1's Register/Modify/Unregister
    // calls above take effect synchronously (no batching) - batched
    // submission is Phase 2+ (e.g. a real io_uring submission queue).

    // Phase 6: idle policy. Applied at phase 1's PollReady() rather than
    // as a separate sleep here - a reactor with an event loop already has
    // exactly one place it can block, and adding a second would be a
    // second source of latency.

    return did_work;
}

void Scheduler::Run() {
    stopped_ = false;
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("sched", "reactor loop entered, " + std::to_string(timers_.size()) +
                                 " timer(s) armed");
    }
    while (!stopped_) {
        RunOnce();
    }
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("sched", "reactor loop left on Stop()");
    }
}

}  // namespace kds::sched
