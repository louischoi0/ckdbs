#pragma once

#include <atomic>
#include <thread>

// The one latch primitive AR0's revised G1 admits beside the ring indices
// (`instructions/v3.0.0/ar0-architecture-revision.md` §3): a spin latch for
// a critical section measured in nanoseconds - a cursor bump and a memcpy
// - held by reactor threads that are never preempted mid-section by the
// engine itself.
//
// **It costs nothing where nothing is shared.** `SpinLatchGuard` takes a
// pointer, and a null pointer is a guard that touches no atomic at all: a
// structure that can be shared carries a `SpinLatch*` that is null on the
// `cores = 1` path, and the guard's one branch is the accepted cost class
// (`docs/spec/sched.md` §5, "phase 3 costs one null test"). This is what
// keeps AR0 G2 - `cores = 1` zero overhead - a property of the code rather
// than of a build flag.
//
// Not a mutex: no waiters' queue, no fairness, no ownership check, and a
// section that does I/O under it stalls every spinner for that I/O
// (`wal/stream.hpp` names the one place that is accepted, and why).
//
// Concurrency: `lock()`/`unlock()` are callable from any thread. Acquire on
// lock, release on unlock: everything written under the latch is visible
// to the next holder.

namespace kds {

class SpinLatch {
public:
    SpinLatch() = default;
    SpinLatch(const SpinLatch&) = delete;
    SpinLatch& operator=(const SpinLatch&) = delete;

    void lock() noexcept {
        for (;;) {
            if (!locked_.exchange(true, std::memory_order_acquire)) return;
            // Read until it looks free before exchanging again: an exchange
            // loop alone would bounce the line between spinners.
            while (locked_.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
        }
    }

    void unlock() noexcept { locked_.store(false, std::memory_order_release); }

private:
    std::atomic<bool> locked_{false};
};

// RAII over an optional latch. `nullptr` means "not shared": the guard is
// then two null tests and no atomic.
class SpinLatchGuard {
public:
    explicit SpinLatchGuard(SpinLatch* latch) noexcept : latch_(latch) {
        if (latch_ != nullptr) latch_->lock();
    }
    ~SpinLatchGuard() {
        if (latch_ != nullptr) latch_->unlock();
    }
    SpinLatchGuard(const SpinLatchGuard&) = delete;
    SpinLatchGuard& operator=(const SpinLatchGuard&) = delete;

private:
    SpinLatch* latch_;
};

}  // namespace kds
