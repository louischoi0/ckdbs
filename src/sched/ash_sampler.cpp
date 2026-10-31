#include "kds/sched/ash_sampler.hpp"

#include <algorithm>
#include <limits>

namespace kds::sched {

bool AshSampler::Due(MonoTimeNs now) noexcept {
    if (!enabled_.load(std::memory_order_relaxed)) {
        armed_ = false;  // a later turn-on arms afresh, on a grid of its own
        return false;
    }
    if (!armed_) {
        armed_ = true;
        next_due_ns_ = now + interval_ns_.load(std::memory_order_relaxed);
        return false;
    }
    return now >= next_due_ns_;
}

void AshSampler::Begin(MonoTimeNs now) noexcept {
    const std::uint64_t interval = std::max<std::uint64_t>(interval_ns_.load(std::memory_order_relaxed), 1);
    const std::uint64_t lateness = now - next_due_ns_;
    late_ = lateness > interval / kLateDivisor;
    now_ns_ = now;
    // The first grid point after `now`: one batch for however many intervals a
    // long step swallowed, and no drift.
    next_due_ns_ += interval * (1 + lateness / interval);
}

void AshSampler::Add(TaskWaitState* state, std::uint8_t sched_group) noexcept {
    if (state == nullptr || state->state == TaskState::kIdle) return;
    stats::AshRecord r;
    r.sample_ts_us = now_ns_ / 1000;
    r.core_id = static_cast<std::uint16_t>(core_id_);
    r.state = static_cast<std::uint8_t>(state->state) & stats::kAshStateMask;
    r.sched_group = sched_group;
    r.event_id = static_cast<std::uint16_t>(state->event);
    r.wait_class = static_cast<std::uint8_t>(stats::Def(state->event).wait_class);
    if (late_) r.flags |= stats::kAshLate;
    const SessionWaitStats* session = state->session;
    if (session != nullptr) {
        r.session_id = static_cast<std::uint32_t>(session->session_id);
        r.fingerprint = session->fingerprint.get();
    } else {
        // A background task (WD13): numbered by the sampler, once, on first sight.
        r.state |= stats::kAshBackground;
        if (state->background_id == 0) state->background_id = ++next_background_id_;
        r.session_id = state->background_id;
    }
    if (state->state == TaskState::kWaiting) {
        r.p1 = state->p1;
        r.p2 = state->p2;
        r.p3 = state->p3;
    } else if (session != nullptr && capture_arg_hash_.load(std::memory_order_relaxed)) {
        // ON_CPU / RUNNABLE carry no event parameters, so the opt-in hash rides in p1.
        if (const std::uint64_t h = session->arg_hash.get(); h != 0) {
            r.p1 = h;
            r.flags |= stats::kAshArgHashValid;
        }
    }
    if (state->state != TaskState::kOnCpu && now_ns_ > state->since_ns) {
        const std::uint64_t us = (now_ns_ - state->since_ns) / 1000;
        r.wait_elapsed_us = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(us, std::numeric_limits<std::uint32_t>::max()));
    }
    ring_->Record(r);
}

}  // namespace kds::sched
