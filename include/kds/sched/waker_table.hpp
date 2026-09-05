#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "kds/sched/waker.hpp"

// AU-S1: the instance's map from core id to "how to wake that reactor".
//
// **This is AR0-6's one cross-core primitive.** The amendment retires the
// ring transport and keeps the interrupt underneath it: a cross-core wake is
// *write the shared state under its own latch, then kick the destination*.
// The kick carries no meaning; a woken reactor learns why from the structure
// it parks on, never from a payload (AR0-6-R1).
//
// It exists because the pair it needs - the destination's `sleeping` flag and
// its `Waker` - lived on `RingTransport` as `WakeTarget`, reachable only
// through a transport that is being retired. Both halves belong to the
// destination's reactor and outlive every send.
//
// **This is the instance's only wake registry** (AU-S1b). A ring send is a
// caller of this path, not a second copy of it, so AU-S5 removes a user
// rather than unpicking a registry.
//
// ---- Why the flag, and what a missed kick costs -------------------------
//
// A write to an eventfd is a syscall, and a busy reactor is never asleep, so
// kicking one unconditionally buys a syscall for nothing. The sender reads
// the destination's flag first and writes only when it is set.
//
// That is a race, and it is an **accepted** one: a sender that published
// between the destination's last look and its raising of the flag reads the
// flag as clear and skips the kick, so the destination waits out its idle
// block (`Scheduler::Config::max_idle_block_ms`, 10 ms). Slow, never wrong -
// which is exactly `waker.hpp`'s existing contract for a lost or coalesced
// wake, and AR0-6-R1 adopts it deliberately rather than inheriting it.
//
// **Whether that race is closed is a property of the caller, not of this
// table**, and the argument sits on `Kick`, with the fence it is about.

namespace kds::sched {

class WakerTable {
public:
    explicit WakerTable(std::uint32_t core_count) : entries_(core_count) {}

    // Called by each reactor for itself, before any peer can kick it, and
    // never again - so this is not synchronised and does not need to be.
    // A core that never registers is simply never kicked and falls back to
    // its idle block, which is what every build did before this existed.
    void Register(std::uint32_t core, const std::atomic<bool>* sleeping, const Waker* waker) {
        if (core >= entries_.size()) return;
        entries_[core] = Entry{sleeping, waker};
    }

    // **Callable from any thread**, which is the whole point: the caller is
    // another core. The caller has already published whatever it wants seen;
    // this reads the destination's flag and writes the eventfd only when it
    // is set.
    //
    // **The fence is the sender's half of a store-buffer pair, and it lives
    // here because every caller needs it and none can be trusted to write
    // it.** Two threads, two variables, opposite orders:
    //
    //   sender:   publish, then read `sleeping`
    //   receiver: set `sleeping`, then read the published state
    //
    // This fence and its twin in `Scheduler::RunOnce` make sequential
    // consistency forbid *both* reads returning the stale value, so at least
    // one of two things happens: the sender sees the flag and kicks, or the
    // receiver sees the state and does not sleep. It moved here from
    // `RealRingTransport::TrySend` when the ring became a caller of this
    // path rather than a second copy of it.
    //
    // **What the fence buys depends on the caller, and is the one thing to
    // read carefully.** The pair has three legs, and this supplies one: the
    // receiver must *also* store its flag with a fence (`RunOnce` does) and
    // *re-read the predicate after raising it*. The ring has that third leg
    // in `HasPending`, so for a send the window is genuinely closed. A caller
    // with no such predicate has only two legs, and for it the kick stays
    // **best-effort**: a publisher landing between the destination's last
    // look and its raising of the flag reads clear, skips the kick, and the
    // destination waits out one idle block. Slow, never wrong, and
    // AR0-6-R1's stated cost.
    void Kick(std::uint32_t core) const noexcept {
        if (core >= entries_.size()) return;
        const Entry& entry = entries_[core];
        if (entry.sleeping == nullptr || entry.waker == nullptr) return;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (!entry.sleeping->load(std::memory_order_seq_cst)) {
            skipped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        entry.waker->Wake();
        kicks_.fetch_add(1, std::memory_order_relaxed);
    }

    // Kicks written, and kicks a busy destination made unnecessary. The
    // second is the counter that shows the flag is doing its job: on a
    // loaded instance it should dwarf the first.
    //
    // `kicks()` is what `SHOW META`'s `sched_wakes_sent` reports, and
    // `sched.md` §4's check - that it equals the sum of the cores'
    // `sched_wakes_received` - **holds by construction now and did not
    // before**. Every counted kick is one `Waker::Wake()`, and `Wake()`
    // increments the destination's own received counter on the sender's
    // thread. While the transport kept its own counter, AU-S3's stop-kicks
    // moved a destination's received count and left the sent count alone,
    // so the identity was already false on any instance that stopped a
    // peer. The one residual gap is a *failed* eventfd write, which is
    // `EAGAIN` at 2^64 pending wakes and lands in `wake_failures_`.
    std::uint64_t kicks() const noexcept { return kicks_.load(std::memory_order_relaxed); }
    std::uint64_t kicks_skipped() const noexcept {
        return skipped_.load(std::memory_order_relaxed);
    }

    // **The size, and it has exactly one caller for a reason.** `Kick`
    // returns silently for a core outside the table, so a table built
    // smaller than the transport it serves disables send-wakes for the high
    // cores with no failure, no counter and no wrong answer - the same
    // silence `AttachWakers` being forgotten would produce. The two-core
    // assembly cell compares this against the transport's own count, which
    // is the only thing in the tree that would notice.
    std::uint32_t core_count() const noexcept {
        return static_cast<std::uint32_t>(entries_.size());
    }

private:
    struct Entry {
        const std::atomic<bool>* sleeping = nullptr;
        const Waker* waker = nullptr;
    };

    std::vector<Entry> entries_;
    mutable std::atomic<std::uint64_t> kicks_{0};
    mutable std::atomic<std::uint64_t> skipped_{0};
};

}  // namespace kds::sched
