#pragma once

#include <atomic>

#include "kds/base/status.hpp"
#include "kds/sched/io_backend.hpp"

// **The wake a cross-core kick needs** (`docs/spec/sched.md` §5, §7).
//
// A reactor with nothing to run blocks in its I/O backend. Sockets and
// timers wake it because both are things the kernel knows about; a write to
// shared state by another thread is neither, and `epoll_wait` cannot see
// it. Until this existed, work for an idle core waited for that block to
// expire on its own.
//
// **What that cost, measured** (on the ring transport, retired at AT-S10d):
// `Scheduler::IdleTimeoutMs` returns whole
// milliseconds and rounds *up*, so the floor was 1 ms, and statement
// shipping — which put a ring message on a client's critical path twice —
// paid it twice per statement. SS-B measured the shipped-minus-seated delta
// at a flat 1,064 µs, identical with the device sync in the path and with
// it removed, tracking the idle block over a fivefold range
// (`bench/v2.2.0/results-shipping-ssb-v2.2.0-11-g982e133.md` §4a). The wire
// itself is ~20 µs against a ~0.9 ms sync; none of that delta was the wire.
//
// ---- What this is, and what it is not -----------------------------------
//
// An `eventfd`, one per reactor, registered with that reactor's backend
// like any other readable handle. `Wake()` is a single 8-byte write and is
// safe from any thread — that is the whole point, since the caller is
// another core.
//
// It is **not** a queue and carries no data: the shared state the kicker
// wrote is what the woken task re-reads, and a wake only says "look". Counting
// semantics are therefore irrelevant and the counter is drained to zero
// whenever it fires; N wakes that arrive before the reactor looks are one
// wake, which is exactly right.
//
// ---- Why it is not written on every kick --------------------------------
//
// A write to an eventfd is a syscall, and a busy reactor is never asleep, so
// waking it would be a syscall bought for nothing. The kicker therefore
// reads the destination's `sleeping` flag first and writes only when it is
// set; `waker_table.hpp` carries that protocol, and why a kick that reads
// the flag a moment too early costs one idle block.

namespace kds::sched {

class Waker {
public:
    static StatusOr<Waker> Create();

    Waker(const Waker&) = delete;
    Waker& operator=(const Waker&) = delete;
    Waker(Waker&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Waker& operator=(Waker&&) = delete;
    ~Waker();

    // The handle to register with an `IoBackend` as `kReadable`.
    IoHandle handle() const noexcept { return fd_; }

    // **Callable from any thread.** One 8-byte write; a failure is
    // swallowed rather than reported, because the caller is a sender with
    // nowhere to return it and the consequence of a lost wake is a
    // statement that waits out the idle block — slow, never wrong. The
    // count exists so a swallowed failure is still visible from outside.
    void Wake() const noexcept;

    // Resets the counter after a wake fires. Called by the reactor's own
    // handler, on the reactor's thread.
    void Drain() const noexcept;

    // Wakes written and wakes that failed to write. Diagnostics; the second
    // should be 0.
    std::uint64_t wakes() const noexcept { return wakes_.load(std::memory_order_relaxed); }
    std::uint64_t wake_failures() const noexcept {
        return wake_failures_.load(std::memory_order_relaxed);
    }

private:
    explicit Waker(IoHandle fd) noexcept : fd_(fd) {}

    IoHandle fd_ = -1;
    // Written from sender threads, so atomic even though they are only ever
    // read for diagnostics.
    mutable std::atomic<std::uint64_t> wakes_{0};
    mutable std::atomic<std::uint64_t> wake_failures_{0};
};

}  // namespace kds::sched
