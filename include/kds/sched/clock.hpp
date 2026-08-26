#pragma once

#include <cstdint>

// Injectable time source (rules.md #4: no std::chrono reads inside engine
// logic - the clock is a scheduler-provided interface). SystemClock is the
// one platform-layer implementation allowed to read a real clock; every
// other engine subsystem takes a `const Clock&` and calls Now() instead of
// touching std::chrono directly. ManualClock is the deterministic
// counterpart used by tests (and, later, the simulation harness in
// docs/spec/sched.md section 8).

namespace kds::sched {

using MonoTimeNs = std::uint64_t;

class Clock {
public:
    virtual ~Clock() = default;
    virtual MonoTimeNs Now() const = 0;
};

// Platform-layer implementation backed by std::chrono::steady_clock.
class SystemClock final : public Clock {
public:
    MonoTimeNs Now() const override;
};

// Deterministic clock for tests/simulation: time only moves when told to.
class ManualClock final : public Clock {
public:
    explicit ManualClock(MonoTimeNs start = 0) noexcept : now_(start) {}

    MonoTimeNs Now() const override { return now_; }
    void Advance(MonoTimeNs delta_ns) noexcept { now_ += delta_ns; }
    void SetNow(MonoTimeNs t) noexcept { now_ = t; }

private:
    MonoTimeNs now_;
};

}  // namespace kds::sched
