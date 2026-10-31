#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

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
// The settings are relaxed atomics, changed at runtime from any thread (WE09
// binds them to `kds.ash.*`). Turning sampling off keeps the ring and turning
// it on continues it.

namespace kds::sched {

class AshSampler {
public:
    static constexpr std::uint64_t kDefaultIntervalNs = 1'000'000'000;
    // A batch is late when its turn came more than interval / this after its due time.
    static constexpr std::uint64_t kLateDivisor = 10;

    explicit AshSampler(std::size_t ring_bytes = stats::AshRing::kDefaultBytes)
        : ring_(std::make_shared<stats::AshRing>(ring_bytes)) {}

    void set_core_id(std::uint32_t core_id) noexcept { core_id_ = core_id; }

    void set_enabled(bool on) noexcept { enabled_.store(on, std::memory_order_relaxed); }
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    void set_interval_ns(std::uint64_t ns) noexcept { interval_ns_.store(ns, std::memory_order_relaxed); }
    std::uint64_t interval_ns() const noexcept { return interval_ns_.load(std::memory_order_relaxed); }
    void set_capture_arg_hash(bool on) noexcept { capture_arg_hash_.store(on, std::memory_order_relaxed); }

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
    std::atomic<bool> enabled_{true};
    std::atomic<std::uint64_t> interval_ns_{kDefaultIntervalNs};
    std::atomic<bool> capture_arg_hash_{false};
    bool armed_ = false;
    MonoTimeNs next_due_ns_ = 0;
    // The current batch.
    MonoTimeNs now_ns_ = 0;
    bool late_ = false;
    std::uint32_t next_background_id_ = 0;
};

}  // namespace kds::sched
