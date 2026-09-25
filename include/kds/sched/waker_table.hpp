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
// through a transport that AT-S10d retired. Both halves belong to the
// destination's reactor.
//
// **This is the instance's only wake registry** (AU-S1b), and since AT-S10d
// the only cross-core path there is: a stop, a lock-table decide and a
// connection handoff kick through it.
//
// ---- The seam, and why it is the table rather than the waker ------------
//
// `WakeRegistry` is what `Scheduler::AttachWakerTable` takes, and
// `WakerTable` is its one production implementation. The interface exists for exactly one other
// implementation, `SimWakerTable` (`sim_waker_table.hpp`), which *wraps* a
// real table for the two-core rig: it logs `(tick, dst)` per kick and
// forwards at a seeded tick. AU-R3 first placed the seam on `Waker` itself
// ("a `Waker`-shaped seam"), and that could not be built: `Waker::Wake()`
// is not virtual and `Kick` calls it statically, so a derived waker through
// the table's pointer would run the base and write to an eventfd it does not
// own (`workorder-av-two-core-rig.md` AV-R1). The operator took the table
// instead (AV-R1's mark, 2026-09-07): `Waker` gains no vtable, the wake path
// keeps its static call, and the cost sits on the *kick* path rather than
// the wake's - one indirect call per cross-core `Kick`, and with it the
// flag load and the skip behind a pointer the caller cannot devirtualize,
// since what it holds may be the sim. That is the trade the mark accepts,
// stated as what it is. What the wrap buys over a second implementation is
// that there is no second implementation: the flag read and the skip
// counter live once, below, and a rig cannot disagree with the protocol it
// is testing.
//
// ---- Why the flag, and what a missed kick costs -------------------------
//
// A write to an eventfd is a syscall, and a busy reactor is never asleep, so
// kicking one unconditionally buys a syscall for nothing. The kicker reads
// the destination's flag first and writes only when it is set. That is a
// race, accepted deliberately; `Kick` states it and its price.

namespace kds::sched {

class WakeRegistry {
public:
    virtual ~WakeRegistry() = default;

    // Called by each reactor for itself, before any peer can kick it, and
    // never again (`WakerTable::Register` says why that needs no latch).
    virtual void Register(std::uint32_t core, const std::atomic<bool>* sleeping,
                          const Waker* waker) = 0;

    // **Callable from any thread.** The caller has already published what
    // it wants seen; this ends the destination's idle block if it is in one.
    virtual void Kick(std::uint32_t core) const noexcept = 0;

    // Kicks actually written - what `Scheduler::wakes_sent()` reports.
    virtual std::uint64_t kicks() const noexcept = 0;
};

class WakerTable final : public WakeRegistry {
public:
    explicit WakerTable(std::uint32_t core_count) : entries_(core_count) {}

    // Called by each reactor for itself, before any peer can kick it, and
    // never again - so this is not synchronised and does not need to be.
    // A core that never registers is simply never kicked and falls back to
    // its idle block, which is what every build did before this existed.
    void Register(std::uint32_t core, const std::atomic<bool>* sleeping,
                  const Waker* waker) override {
        if (core >= entries_.size()) return;
        entries_[core] = Entry{sleeping, waker};
    }

    // **Callable from any thread**, which is the whole point: the caller is
    // another core. The caller has already published whatever it wants seen;
    // this reads the destination's flag and writes the eventfd only when it
    // is set.
    //
    // **The kick is best-effort - a choice, priced at one idle block**
    // (AR0-6-R1). A publisher landing between the destination's last look
    // and its raising of the flag reads clear, skips the kick, and the
    // destination waits out one block (`max_idle_block_ms`); every consumer
    // is level-triggered, re-polled after the block, so this is slow and
    // never wrong. Closing the window would take a `seq_cst` fence here, one
    // after the flag's store, and the reactor re-polling its parked tasks
    // after raising the flag. The ring had that third leg for its own queue
    // in `HasPending`; AT-S10d retired it with the ring rather than
    // generalise it, so the fences ordered nothing and went too, and the
    // flag is a hint read and written relaxed.
    void Kick(std::uint32_t core) const noexcept override {
        if (core >= entries_.size()) return;
        const Entry& entry = entries_[core];
        if (entry.sleeping == nullptr || entry.waker == nullptr) return;
        if (!entry.sleeping->load(std::memory_order_relaxed)) {
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
    std::uint64_t kicks() const noexcept override {
        return kicks_.load(std::memory_order_relaxed);
    }
    std::uint64_t kicks_skipped() const noexcept {
        return skipped_.load(std::memory_order_relaxed);
    }

    // **The size.** `Kick` returns silently for a core outside the table,
    // so a table built smaller than the instance disables every wake to the
    // high cores with no failure, no counter and no wrong answer. The
    // two-core assembly cell compares this against the instance's core
    // count, which is the only thing in the tree that would notice.
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
