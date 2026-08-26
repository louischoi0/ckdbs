#pragma once

#include <cstdint>
#include <deque>
#include <span>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/ring_transport.hpp"

// The simulated cross-core transport (docs/spec/sched.md §8, workplan M9).
//
// Same seam as the real rings, different physics: a message here is held
// until an injected *delivery time* passes, so a test can make one core's
// messages arrive late, arrive out of order relative to another core's, or
// arrive in an order the sender did not choose - and get the same answer on
// every run.
//
// ---- What "deterministic" costs, and why it is worth it ------------------
//
// M9 puts this in v1 rather than deferring it, on the grounds that
// **without it every cross-core test is nondeterministic**. A pipeline bug
// that appears once in a thousand runs of a real two-thread test is a bug
// nobody can hold still; the same bug under a seeded simulation is a
// `(seed, build)` pair.
//
// The determinism rules of sched.md §8 are load-bearing here and are worth
// naming, because each is a way this class could have quietly failed:
//   - The RNG is seeded and owned (a `SplitMix64` below), never the global
//     one, and never `std::random_device`.
//   - Ordering never depends on a hash iteration order or an address. The
//     pending queue is a `std::deque` searched in insertion order.
//   - Time comes from the injected `Clock`, never from wall time
//     (rules.md §4).
//
// ---- Reordering ----------------------------------------------------------
//
// Delivery is by *deadline*, and the deadline of each message is drawn
// independently, so a later send can be delivered before an earlier one.
// That is the reorder injection: there is no separate switch for it,
// because reordering is exactly what a variable delay produces, and a
// second mechanism would be a second thing to disbelieve.
//
// With `delay_ns == 0` (the default) every message is deliverable at once
// and the queue drains in send order, which is what makes this transport
// substitutable for the real one in any test not specifically about timing.
//
// **Substitutable per edge, not per inbox.** The real transport sweeps its
// peers in rotation so that no peer starves; this one delivers by deadline.
// Two messages sent from *different* cores to the same core therefore
// arrive in an order the two implementations do not share - and nothing
// above this layer may depend on that order, which is precisely the
// assumption the reorder injection is here to break. What both guarantee is
// per-edge send order, and that no message is invented, lost or duplicated.
// `ring_transport_test.cpp` pins the distinction in both directions.

namespace kds::sched {

struct SimTransportConfig {
    // Minimum and maximum delivery delay, in injected-clock nanoseconds. A
    // message's delay is drawn uniformly from [min, max]. Equal values give
    // a fixed delay; both zero gives immediate, in-order delivery.
    MonoTimeNs min_delay_ns = 0;
    MonoTimeNs max_delay_ns = 0;

    // Seed for the delay draw. Fixed by default so an unconfigured
    // simulation is still reproducible - an accidentally-random default
    // would defeat the whole class.
    std::uint64_t seed = 0x5EED'1234'5EED'1234ULL;

    // Messages held in flight before TrySend() reports ResourceExhausted.
    // The simulated counterpart of a full ring, so that backpressure and
    // M7's retry path are exercisable under simulation too - a transport
    // that never fills would let a send-retry bug reach production
    // untested.
    std::size_t capacity_per_edge = 256;
};

class SimRingTransport final : public RingTransport {
public:
    // `clock` must outlive this transport, and is the only source of time
    // it has.
    static StatusOr<SimRingTransport> Create(std::uint32_t core_count, const Clock& clock,
                                             SimTransportConfig config = {});

    SimRingTransport(const SimRingTransport&) = delete;
    SimRingTransport& operator=(const SimRingTransport&) = delete;
    SimRingTransport(SimRingTransport&&) noexcept = default;

    Status TrySend(const MessageHeader& header, std::span<const std::byte> payload) override;

    // Delivers the **earliest-deadline** message addressed to `dst_core`
    // whose deadline has passed, or returns false. Ties break on send
    // sequence, so equal deadlines deliver in send order and the zero-delay
    // configuration is exactly in-order.
    bool TryReceive(std::uint32_t dst_core, MessageHeader& header,
                    std::vector<std::byte>& payload) override;

    // The wake path's two halves (waker.hpp), answered honestly and used by
    // nothing: a simulated reactor is pumped by the simulation loop and
    // never blocks in a real backend, so it has no sleep to interrupt.
    // `HasPending` still answers truthfully rather than `false`, because a
    // scheduler that *did* block on this transport must not be told its
    // inbox is empty when it is not.
    bool HasPending(std::uint32_t dst_core) const override;
    void SetWakeTarget(std::uint32_t, WakeTarget) override {}

    std::uint32_t core_count() const noexcept override { return core_count_; }

    // Messages sent but not yet delivered. For tests: "the pipeline is torn
    // down" is partly a claim that nothing is still in flight.
    std::size_t in_flight() const noexcept { return pending_.size(); }

    // Total messages ever sent through this transport. What the
    // single-core fast-path test asserts is zero (workplan guideline 2).
    std::uint64_t sent_count() const noexcept { return sent_count_; }

private:
    struct Pending {
        MessageHeader header;
        std::vector<std::byte> payload;
        MonoTimeNs deadline;
        std::uint64_t seq;  // send order, the tie-break - never a pointer
    };

    SimRingTransport(std::uint32_t core_count, const Clock& clock, SimTransportConfig config)
        : core_count_(core_count), clock_(&clock), config_(config), rng_state_(config.seed) {}

    // SplitMix64: a two-line generator with no external dependency and no
    // hidden global state, which is the whole requirement. Its statistical
    // quality is irrelevant here - it picks delays for a test.
    std::uint64_t NextRandom() noexcept;

    std::uint32_t core_count_ = 0;
    const Clock* clock_ = nullptr;
    SimTransportConfig config_{};
    std::uint64_t rng_state_ = 0;

    // Insertion-ordered, searched linearly. Deliberately not a priority
    // queue keyed on deadline: a heap's tie-break is unspecified, and this
    // container's whole job is to have a defined order.
    std::deque<Pending> pending_;
    std::uint64_t next_seq_ = 0;
    std::uint64_t sent_count_ = 0;
};

}  // namespace kds::sched
