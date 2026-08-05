#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/sched/spsc_ring.hpp"

// The cross-core transport seam (docs/sched.md §5, docs/workplan-crosscore.md
// M9 and P1).
//
// **This interface is the only channel between cores.** Workplan guideline
// 1 states it as an invariant - no shared engine state, no atomics outside
// ring indices - and the seam is what makes that checkable rather than
// aspirational: engine code holds a `RingTransport&` and has no other way
// to reach a peer.
//
// It exists in two implementations for a reason M9 calls in-scope for v1
// rather than a later luxury: **without a simulated transport every
// cross-core test is nondeterministic.** The real one is the N² SPSC ring
// matrix; the simulated one delivers through the same interface with
// injectable delay and reordering, so a failure reproduces from
// `(seed, build)` alone (sched.md §8).
//
// ---- Threading ----------------------------------------------------------
//
// `TrySend(src -> dst)` may only be called on the thread owning `src`, and
// `TryReceive(dst)` only on the thread owning `dst`. That is what makes
// each underlying ring single-producer/single-consumer. The simulated
// transport runs every reactor on one thread and so satisfies it trivially.

namespace kds::sched {

class RingTransport {
public:
    virtual ~RingTransport() = default;

    // Delivers `header` + `payload` to `header.dst_core`.
    //
    // Fails with ResourceExhausted when the channel is full. Callers must
    // handle that (sched.md §5) - the engine-wide answer is M7's yield and
    // retry, sched/send_retry.hpp - and must never drop the message
    // instead. Fails with InvalidArgument for an out-of-range core or an
    // oversized payload.
    virtual Status TrySend(const MessageHeader& header, std::span<const std::byte> payload) = 0;

    // Takes the next message addressed to `dst_core` from any peer, or
    // returns false if there is none. `payload` is resized to the message's
    // length; passing the same vector back in on every call is what keeps
    // the drain allocation-free in steady state.
    //
    // Fairness across peers is the implementation's business, but it must
    // not be able to starve one: a core with a noisy neighbour still has to
    // hear from everybody.
    virtual bool TryReceive(std::uint32_t dst_core, MessageHeader& header,
                            std::vector<std::byte>& payload) = 0;

    virtual std::uint32_t core_count() const noexcept = 0;
};

// The real transport: one SpscRing per **ordered** core pair, so a channel
// has exactly one writer and one reader (N² rings for N cores, sched.md
// §5). Self-sends are included in the matrix and are legal - a core
// messaging itself is the degenerate case of the same protocol, and
// excluding it would put a special case at every call site.
//
// Every ring is allocated at construction. Nothing is allocated afterwards,
// and at `core_count == 1` the whole matrix is one ring that the fast path
// never touches - workplan guideline 2's "zero messages, zero allocations"
// requirement for the single-core build.
class RealRingTransport final : public RingTransport {
public:
    // `capacity_slots` and `max_payload` are per ring, and both are
    // `[OPEN]`/`[PROPOSED]` upstream (sched.md §10 ring sizing,
    // crosscore.md §4 batch size), so they are parameters here and nothing
    // may depend on a particular value.
    static StatusOr<RealRingTransport> Create(std::uint32_t core_count,
                                              std::size_t capacity_slots,
                                              std::size_t max_payload);

    RealRingTransport(const RealRingTransport&) = delete;
    RealRingTransport& operator=(const RealRingTransport&) = delete;
    RealRingTransport(RealRingTransport&&) noexcept = default;

    Status TrySend(const MessageHeader& header, std::span<const std::byte> payload) override;
    bool TryReceive(std::uint32_t dst_core, MessageHeader& header,
                    std::vector<std::byte>& payload) override;
    std::uint32_t core_count() const noexcept override { return core_count_; }

private:
    RealRingTransport(std::uint32_t core_count, std::vector<SpscRing> rings)
        : core_count_(core_count), rings_(std::move(rings)),
          next_peer_(core_count, 0) {}

    // Row-major (src, dst): rings_[src * n + dst].
    SpscRing& RingFor(std::uint32_t src, std::uint32_t dst) noexcept {
        return rings_[static_cast<std::size_t>(src) * core_count_ + dst];
    }

    std::uint32_t core_count_ = 0;
    std::vector<SpscRing> rings_;

    // Where the next TryReceive(dst) starts its sweep over peers. A
    // rotating start is what keeps a busy peer from starving a quiet one:
    // always starting at peer 0 would let core 0 monopolize every drain.
    // Per destination, and only ever touched by that destination's own
    // thread.
    std::vector<std::uint32_t> next_peer_;
};

}  // namespace kds::sched
