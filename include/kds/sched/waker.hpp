#pragma once

#include <atomic>

#include "kds/base/status.hpp"
#include "kds/sched/io_backend.hpp"

// **The wake a cross-core message needs** (`docs/spec/sched.md` §4).
//
// A reactor with nothing to run blocks in its I/O backend. Sockets and
// timers wake it because both are things the kernel knows about; a ring
// message is neither — it is a store to shared memory by another thread,
// and `epoll_wait` cannot see it. Until this existed, a message to an idle
// core waited for that block to expire on its own.
//
// **What that cost, measured**: `Scheduler::IdleTimeoutMs` returns whole
// milliseconds and rounds *up*, so the floor was 1 ms, and statement
// shipping — which puts a ring message on a client's critical path twice —
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
// It is **not** a queue and carries no data: the ring is the queue, and a
// wake only says "look at it". Counting semantics are therefore irrelevant
// and the counter is drained to zero whenever it fires; N wakes that arrive
// before the reactor looks are one wake, which is exactly right.
//
// ---- Why it is not written on every send --------------------------------
//
// A write to an eventfd is a syscall. A busy reactor is never asleep, so
// waking it would be a syscall per message bought for nothing — and a busy
// owner is the case shipping is *fast* in (0.93-0.99x from four sessions
// up, same file §5). The sender therefore reads the destination's
// `sleeping` flag first and writes only when it is set; `ring_transport.hpp`
// carries that protocol and the argument for why the flag cannot be missed.

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
