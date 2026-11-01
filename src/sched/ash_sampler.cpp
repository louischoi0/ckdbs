#include "kds/sched/ash_sampler.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <string>

namespace kds::sched {

namespace {

struct Slot {
    std::shared_ptr<stats::AshRing> ring;
    std::shared_ptr<AshControl> control;
};

std::mutex g_mutex;
std::array<Slot, kWaitCores> g_slots;
AshSettings g_defaults;

void Apply(const AshSettings& d, AshControl& c) {
    c.enabled.store(d.enabled, std::memory_order_relaxed);
    c.interval_ns.store(d.interval_ms * 1'000'000, std::memory_order_relaxed);
    c.capture_arg_hash.store(d.capture_arg_hash, std::memory_order_relaxed);
}

}  // namespace

AshSettings AshDefaults() {
    const std::lock_guard<std::mutex> held(g_mutex);
    return g_defaults;
}

Status CheckAshIntervalMs(std::uint64_t ms) {
    if (ms >= kAshMinIntervalMs && ms <= kAshMaxIntervalMs) return Status::OK();
    return Status::InvalidArgument("ash interval_ms must be in [" + std::to_string(kAshMinIntervalMs) + ", " +
                                   std::to_string(kAshMaxIntervalMs) + "], got " + std::to_string(ms));
}

Status CheckAshRingBytes(std::uint64_t bytes) {
    if (bytes >= kAshMinRingBytes && bytes <= kAshMaxRingBytes) return Status::OK();
    return Status::InvalidArgument("ash ring_bytes_per_core must be in [" + std::to_string(kAshMinRingBytes) +
                                   ", " + std::to_string(kAshMaxRingBytes) + "], got " + std::to_string(bytes));
}

namespace {
// One change to the defaults, pushed to every registered sampler under the lock.
template <class F>
void Update(F&& change) {
    const std::lock_guard<std::mutex> held(g_mutex);
    change(g_defaults);
    for (Slot& s : g_slots) {
        if (s.control) Apply(g_defaults, *s.control);
    }
}
}  // namespace

void SetAshEnabled(bool on) {
    Update([&](AshSettings& d) { d.enabled = on; });
}

Status SetAshIntervalMs(std::uint64_t ms) {
    if (Status s = CheckAshIntervalMs(ms); !s.ok()) return s;
    Update([&](AshSettings& d) { d.interval_ms = ms; });
    return Status::OK();
}

void SetAshCaptureArgHash(bool on) {
    Update([&](AshSettings& d) { d.capture_arg_hash = on; });
}

Status SetAshRingBytes(std::size_t bytes) {
    if (Status s = CheckAshRingBytes(bytes); !s.ok()) return s;
    Update([&](AshSettings& d) { d.ring_bytes = bytes; });
    return Status::OK();
}

void RegisterAshRing(std::uint32_t core_id, const std::shared_ptr<stats::AshRing>& ring,
                     const std::shared_ptr<AshControl>& control) {
    if (core_id >= kWaitCores) return;
    const std::lock_guard<std::mutex> held(g_mutex);
    Apply(g_defaults, *control);  // a SET issued before this core registered still reaches it
    g_slots[core_id] = {ring, control};
}

void UnregisterAshRing(std::uint32_t core_id, const stats::AshRing* ring) {
    if (core_id >= kWaitCores) return;
    const std::lock_guard<std::mutex> held(g_mutex);
    if (g_slots[core_id].ring.get() == ring) g_slots[core_id] = {};
}

std::shared_ptr<stats::AshRing> AshRingOfCore(std::uint32_t core_id) {
    if (core_id >= kWaitCores) return nullptr;
    const std::lock_guard<std::mutex> held(g_mutex);
    return g_slots[core_id].ring;
}

bool AshSampler::Due(MonoTimeNs now) noexcept {
    if (!control_->enabled.load(std::memory_order_relaxed)) {
        armed_ = false;  // a later turn-on arms afresh, on a grid of its own
        return false;
    }
    if (!armed_) {
        armed_ = true;
        next_due_ns_ = now + control_->interval_ns.load(std::memory_order_relaxed);
        return false;
    }
    // A shortened interval pulls the next sample in; a lengthened one takes
    // effect when the current one is taken.
    const std::uint64_t interval = control_->interval_ns.load(std::memory_order_relaxed);
    if (next_due_ns_ > now + interval) next_due_ns_ = now + interval;
    return now >= next_due_ns_;
}

void AshSampler::Begin(MonoTimeNs now) noexcept {
    const std::uint64_t interval = std::max<std::uint64_t>(control_->interval_ns.load(std::memory_order_relaxed), 1);
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
    } else if (session != nullptr && control_->capture_arg_hash.load(std::memory_order_relaxed)) {
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
