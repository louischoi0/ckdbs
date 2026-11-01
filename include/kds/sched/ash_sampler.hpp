#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "kds/base/status.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/wait_state.hpp"
#include "kds/stats/ash_ring.hpp"

// **The ASH sampler** (`instructions/v3.2.0/workorder-cj-wait-events-ash.md`
// §2.7, CJ's WE07; decisions WD6, WD11, WD13, CJ-Q6, CJ-Q11): one per core,
// driven by the reactor's turn, not by a thread or a timer (I1: it reads only
// its own core's tasks).
//
// ---- The protocol --------------------------------------------------------
//
// Each turn the scheduler asks `Due(now)`: one relaxed load and one compare
// while sampling is off or not yet due. When due, it calls `Begin`, then `Add`
// for every task in its queues. Between polls every task is queued, so
// no task is mid-resume and each one's `TaskWaitState` is stable.
//
// * **The grid.** The first call sets the first due time to `now + interval`;
//   after that the next due time is `previous scheduled + interval`, so the
//   samples stay on a grid and do not drift. A turn that arrives after one or
//   more whole intervals have passed (a long step) writes **one** batch at the
//   real time and moves the next due time to the first grid point after it:
//   nothing is interpolated.
// * **`late`** (CJ-Q11): a batch whose turn came more than a tenth of an
//   interval after its scheduled time carries `kAshLate` on every record.
// * **Who is sampled.** Every coroutine task that is not IDLE (WD6), background
//   tasks included and told apart by the record's background bit (WD13). A
//   task with no `TaskWaitState` (a `FunctionTask`) is not sampled.
// * **`arg_hash`** (WD11): off by default. When on, an `ON_CPU` or `RUNNABLE`
//   sample of a statement that has one carries it in `p1` with
//   `kAshArgHashValid`; a `WAITING` sample keeps the event's parameters. Bind
//   values are never recorded.
//
// The settings (WE09, `kds.ash.*`) are relaxed atomics in an `AshControl` the
// sampler shares with a process-wide registry, so one `SET ASH ...` reaches
// every core's sampler without a message to its reactor. Turning sampling off
// keeps the ring and turning it on continues it. The process defaults below
// are what a sampler adopts when it takes its core id; the ring size is read
// once, at construction, and changes only on restart.

namespace kds::stats {
class OptimizerSignals;
}

namespace kds::sched {

// `kds.ash.interval_ms` and `kds.ash.ring_bytes_per_core` ranges (the order's
// §2.9; the ring range is `[PROPOSED]`, adopted under CJ-Q13).
inline constexpr std::uint64_t kAshMinIntervalMs = 100;
inline constexpr std::uint64_t kAshMaxIntervalMs = 10'000;
inline constexpr std::size_t kAshMinRingBytes = 1u << 20;
inline constexpr std::size_t kAshMaxRingBytes = 64u << 20;

struct AshControl {
    std::atomic<bool> enabled{true};
    std::atomic<std::uint64_t> interval_ns{1'000'000'000};
    std::atomic<bool> capture_arg_hash{false};
};

// The process-wide settings. Each setter range-checks (InvalidArgument naming
// the range), then applies to the defaults and to every registered sampler.
struct AshSettings {
    bool enabled = true;
    std::uint64_t interval_ms = 1000;
    bool capture_arg_hash = false;
    std::size_t ring_bytes = stats::AshRing::kDefaultBytes;
};
AshSettings AshDefaults();
// The ranges' one home; the setters and the config file both ask.
Status CheckAshIntervalMs(std::uint64_t ms);
Status CheckAshRingBytes(std::uint64_t bytes);
void SetAshEnabled(bool on);
Status SetAshIntervalMs(std::uint64_t ms);
void SetAshCaptureArgHash(bool on);
// Takes effect for samplers built after the call (restart-only).
Status SetAshRingBytes(std::size_t bytes);

// The rings of the cores that have a sampler, by core id, for `sys.ash`
// (WE08). A slot is replaced when a core id is set again and cleared by the
// sampler that owns it, so a view never reads a ring a destroyed scheduler left.
void RegisterAshRing(std::uint32_t core_id, const std::shared_ptr<stats::AshRing>& ring,
                     const std::shared_ptr<AshControl>& control);
void UnregisterAshRing(std::uint32_t core_id, const stats::AshRing* ring);
// Null when no sampler is registered for `core_id`.
std::shared_ptr<stats::AshRing> AshRingOfCore(std::uint32_t core_id);

class AshSampler {
public:
    // A batch is late when its turn came more than interval / this after its due time.
    static constexpr std::uint64_t kLateDivisor = 10;

    explicit AshSampler(std::size_t ring_bytes = AshDefaults().ring_bytes)
        : ring_(std::make_shared<stats::AshRing>(ring_bytes)), control_(std::make_shared<AshControl>()) {}

    ~AshSampler() { UnregisterAshRing(core_id_, ring_.get()); }
    AshSampler(const AshSampler&) = delete;
    AshSampler& operator=(const AshSampler&) = delete;

    void set_core_id(std::uint32_t core_id) {
        UnregisterAshRing(core_id_, ring_.get());
        core_id_ = core_id;
        RegisterAshRing(core_id_, ring_, control_);
    }

    // Unchecked, for tests that drive a logical clock; the checked settings
    // are `SetAsh*` above.
    void set_enabled(bool on) noexcept { control_->enabled.store(on, std::memory_order_relaxed); }
    bool enabled() const noexcept { return control_->enabled.load(std::memory_order_relaxed); }
    void set_interval_ns(std::uint64_t ns) noexcept { control_->interval_ns.store(ns, std::memory_order_relaxed); }
    std::uint64_t interval_ns() const noexcept { return control_->interval_ns.load(std::memory_order_relaxed); }
    void set_capture_arg_hash(bool on) noexcept {
        control_->capture_arg_hash.store(on, std::memory_order_relaxed);
    }

    // The Cabin controller's collector, which gets one note per WAITING sample
    // of a statement that has a shape (CJ's WE13; called on this core's thread,
    // the collector latches itself). Set at startup, before the worker exists;
    // null (the default) notes nothing.
    void set_wait_sink(stats::OptimizerSignals* sink) noexcept { wait_sink_ = sink; }

    // The ring, shared so a reader on another core outlives the scheduler's
    // teardown (WE08).
    const std::shared_ptr<stats::AshRing>& ring() const noexcept { return ring_; }

    // True when a batch is due at `now`. The first call arms the grid and
    // returns false.
    bool Due(MonoTimeNs now) noexcept;

    // One batch: `Begin(now)`, `Add` per task, no end call needed.
    void Begin(MonoTimeNs now) noexcept;
    // `state` is null for a task that has none.
    void Add(TaskWaitState* state, std::uint8_t sched_group) noexcept;

private:
    std::uint32_t core_id_ = 0;
    std::shared_ptr<stats::AshRing> ring_;
    std::shared_ptr<AshControl> control_;
    stats::OptimizerSignals* wait_sink_ = nullptr;
    bool armed_ = false;
    MonoTimeNs next_due_ns_ = 0;
    // The current batch.
    MonoTimeNs now_ns_ = 0;
    bool late_ = false;
    std::uint32_t next_background_id_ = 0;
};

}  // namespace kds::sched
