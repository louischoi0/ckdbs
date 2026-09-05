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
// destination's reactor and outlive every send; only their *home* moves.
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
// **The transport closed that window and this does not.** `RingTransport`
// had `HasPending`: the destination, having raised its flag, asked the queue
// once more whether anything had arrived. There is no queue here, so there is
// no such question to ask - the general form is "re-evaluate the predicate
// you are about to park on", and that belongs with a consumer that has one.
// AU-S2 (AO-S5 re-based onto the kick) is the first, and is where the
// re-check goes in with something to test it. Building it now would be a
// mechanism with no caller.

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
    // another core. Reads the destination's flag with sequential
    // consistency - the same ordering the reactor stores it with, so the
    // pair is a store-buffer and neither side may read stale - and writes
    // the eventfd only when it is set.
    void Kick(std::uint32_t core) const noexcept {
        if (core >= entries_.size()) return;
        const Entry& entry = entries_[core];
        if (entry.sleeping == nullptr || entry.waker == nullptr) return;
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
    std::uint64_t kicks() const noexcept { return kicks_.load(std::memory_order_relaxed); }
    std::uint64_t kicks_skipped() const noexcept {
        return skipped_.load(std::memory_order_relaxed);
    }

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
