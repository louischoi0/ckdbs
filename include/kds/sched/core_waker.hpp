#pragma once

#include <atomic>

#include "kds/sched/io_backend.hpp"

// How a core that is asleep is told that a message is waiting for it
// (docs/spec/sched.md §5 and §7; the v2.3.0 order's D2 and D3).
//
// Before this existed, an idle reactor blocked in `PollReady` for up to a
// millisecond and **nothing ended that block when a ring message arrived** -
// the ring is memory, not a descriptor, and phase 3 only looks at it after
// the block returns. A shipped statement therefore paid the owner's whole
// idle block, measured as a flat 1,064 µs and tracking the block over a
// fivefold range (`bench/v2.2.0/results-shipping-ssb-v2.2.0-11-g982e133.md`
// §4a). This is the missing half.
//
// ---- The atomic protocol ------------------------------------------------
//
// Two participants, one flag, and a store-load pair on each side that must
// not be reordered - this is Dekker's argument, and both sides need a
// `seq_cst` store followed by a `seq_cst` load for it to hold:
//
//   sender (peer core)                 sleeper (this core)
//   -------------------                -------------------
//   1. push onto the ring              1. sleeping_ = true      (store)
//   2. read sleeping_       (load)     2. is the ring non-empty? (load)
//   3. if true -> backend->Wake()      3. if empty -> block, else skip it
//                                      4. sleeping_ = false
//
// At least one of them sees the other. If the sender's load runs after the
// sleeper's store, it wakes. If it runs before, then its push happened
// before the sleeper's load, so the sleeper sees a non-empty ring and does
// not block.
//
// **That argument needs a StoreLoad barrier on *both* sides, and one of
// them is not free.** The sleeper's side is: its store is `seq_cst`, which
// is a full barrier. The sender's is not - **its store is the ring's, a
// plain release inside `SpscRing`**, and StoreLoad is precisely the one
// reordering x86's TSO permits (the push sits in the store buffer while the
// flag load executes and reads a stale `false`). A `seq_cst` *load* does
// not fix it: on x86 that is an ordinary MOV, and in the C++ model the
// total order over `seq_cst` operations says nothing about a release store
// on another object. So this file issues an explicit
// `atomic_thread_fence(seq_cst)` before the load, and both sides carry a
// fence rather than one side hoping.
//
// The cost is one fence per cross-core send - tens of cycles against the
// ~1 µs syscall it is deciding whether to make, and against the ~1 ms block
// it exists to end. Removing it would leave a message that lands in exactly
// the wrong nanosecond waiting out the whole idle block: rare, silent, and
// indistinguishable from the defect this path was built to fix.
//
// The eventfd behind `IoBackend::Wake()` closes the remaining window on its
// own - a wake that lands between the ring check and `epoll_wait` leaves
// the counter set, so that block returns immediately (io_backend.hpp).
// Belt and braces, deliberately: the flag is what keeps a wake off the hot
// path, the eventfd is what makes a mistimed one harmless.
//
// ---- Why a flag at all --------------------------------------------------
//
// Waking unconditionally is one `write(2)` per cross-core message on the
// **sender's** critical path, paid for every message whether or not the
// target was asleep - and the cells this work must not regress
// (`bench/v2.2.0/results-shipping-ssb-v2.2.0-11-g982e133.md` §5, §7) are
// exactly the loaded ones, where the target is never asleep. The flag makes
// the syscall the exception rather than the rule: a busy owner is woken
// zero times.
//
// ---- Lifetime -----------------------------------------------------------
//
// A handle holds two raw pointers into a `Scheduler` (its flag) and its
// `IoBackend`. Registration happens at startup, before any worker thread
// runs, and neither object may move afterwards - the same rule the
// transport itself is already under. A peer that wakes a destroyed reactor
// is a shutdown-ordering bug, not something this type can rescue.

namespace kds::sched {

struct CoreWakeHandle {
    // The target reactor's "I am about to block, or blocking" flag.
    const std::atomic<bool>* sleeping = nullptr;
    // The target reactor's backend, whose Wake() ends that block. The only
    // method on it callable from this (peer) thread.
    IoBackend* backend = nullptr;

    // Called on the *sender's* thread, after its message is visible to the
    // reader. Cheap and branchy on purpose: one seq_cst load, and a syscall
    // only for a target that is actually asleep.
    void WakeIfSleeping() const noexcept {
        if (sleeping == nullptr || backend == nullptr) return;
        // The sender's StoreLoad barrier, between the ring push above and
        // the flag read below. See the header comment: without it x86 may
        // read a stale `false` over a push still in the store buffer, and
        // the message waits out the block.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (!sleeping->load(std::memory_order_seq_cst)) return;
        backend->Wake();
    }
};

}  // namespace kds::sched
