#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/sched/waker_table.hpp"

// AU-S1c: the deterministic wake registry for the two-core rig
// (`instructions/v3.0.0/workorder-au-ring-retirement.md` AU-R3, as AV-R1's
// mark rewrote it; `workorder-av-two-core-rig.md`). `waker_table.hpp` says
// why the seam is the table and not the waker; this header says only what
// the sim adds around the real table's `Kick`, which is a log and a hold.
//
// ---- The tick, and who moves it ----------------------------------------
//
// Time here is a counter the rig advances (`Advance`), never the clock and
// never wall time. A reactor's idle block is a real `IoBackend` wait, so a
// held kick cannot end it from inside the simulation; it ends when the rig
// advances the tick to the kick's `due` and the forward writes the eventfd.
// That answers AV's H2 by construction: the delay reaches the reactor only
// through the rig's own thread.
//
// **What the seed fixes, exactly.** A kick's delay is a pure function of
// `(seed, dst, tick)` - SplitMix64's mixer over the three, with no state
// carried between draws - so the delay a kick draws does not depend on how
// many kicks preceded it. That is the whole of the determinism claim: in
// the rig two reactors on two threads kick at moments the interleaving
// chooses (the peer's anchor send is one), so *which* kicks land, and at
// which tick, is not the seed's to fix; *what delay each one draws* is. A
// stateful stream would have let one thread's kick shift the delay of
// every later kick from the other, which is the nondeterminism AV-R3
// excludes. A single-threaded driver reproduces the whole log.
//
// A kick whose delay is 0 is forwarded on the kicking thread at once - the
// production path with a log line in front of it (AU-5's H1). Under
// `Advance`, due kicks forward in `(due, kick order)`.
//
// ---- Concurrency ---------------------------------------------------------
//
// `Kick` is called from any reactor's thread and `Advance` from the rig's,
// so the tick, the log and the held kicks are guarded by `latch_` - a
// `std::mutex`, on `base/latch.hpp`'s terms, acceptable because nothing in
// the engine ever holds it: this class is a test instrument. The forward
// to the real table runs **outside** the latch, so the wake path itself
// takes no lock and a kicked reactor never contends with the rig for one;
// `forwarded_` is atomic and counted after the forward, so a reader that
// sees it has a real table that has already seen the kick.

namespace kds::sched {

struct SimWakerConfig {
    // A kick's delay in ticks, drawn from [min, max]. Equal values give a
    // fixed delay; both zero forwards every kick at once, which is what
    // makes this table substitutable for the real one in any cell not
    // about latency.
    std::uint64_t min_delay_ticks = 0;
    std::uint64_t max_delay_ticks = 0;

    // Fixed by default so an unconfigured rig is still reproducible - an
    // accidentally-random default would defeat the whole class.
    std::uint64_t seed = 0x5EED'1234'5EED'1234ULL;
};

class SimWakerTable final : public WakeRegistry {
public:
    // One kick, as the log records it: when it was asked for, for whom, and
    // when it reaches the real table.
    struct Record {
        std::uint64_t tick = 0;
        std::uint32_t dst = 0;
        std::uint64_t due = 0;

        bool operator==(const Record&) const = default;
    };

    // `real` must outlive this table; it is the one every forward lands on.
    SimWakerTable(WakerTable& real, SimWakerConfig config = {});

    SimWakerTable(const SimWakerTable&) = delete;
    SimWakerTable& operator=(const SimWakerTable&) = delete;

    // Straight through: the entry belongs to the real table, which is the
    // only place a `Kick` ever reads it.
    void Register(std::uint32_t core, const std::atomic<bool>* sleeping,
                  const Waker* waker) override {
        real_.Register(core, sleeping, waker);
    }

    // Records `(tick, core)`, draws the delay, and either forwards now
    // (delay 0) or holds the kick until `Advance` reaches its due tick.
    void Kick(std::uint32_t core) const noexcept override;

    // The real table's count, so `Scheduler::wakes_sent()` and `SHOW META`
    // report kicks that were *written*, never kicks that were asked for.
    std::uint64_t kicks() const noexcept override { return real_.kicks(); }

    // Moves the tick by `ticks` and forwards every held kick whose due tick
    // has been reached, in `(due, kick order)`.
    void Advance(std::uint64_t ticks = 1);

    std::uint64_t tick() const;
    // Every kick asked for, in the order asked. A copy, taken under the
    // latch, so a cell can compare two runs' logs whole.
    std::vector<Record> log() const;
    // Kicks recorded and not yet forwarded.
    std::size_t in_flight() const;
    // Kicks the real table has been handed, now or by `Advance`.
    std::uint64_t forwarded() const noexcept {
        return forwarded_.load(std::memory_order_acquire);
    }

private:
    std::uint64_t DelayFor(std::uint32_t dst, std::uint64_t tick) const noexcept;

    WakerTable& real_;
    const SimWakerConfig config_;

    mutable Latch latch_;
    mutable std::uint64_t tick_ = 0;
    mutable std::vector<Record> log_;
    mutable std::vector<Record> pending_;  // kick order
    mutable std::atomic<std::uint64_t> forwarded_{0};
};

}  // namespace kds::sched
