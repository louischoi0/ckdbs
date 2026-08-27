#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/sched/spsc_ring.hpp"
#include "kds/sched/waker.hpp"

// The cross-core transport seam (docs/spec/sched.md §5, docs/inflight/in-progress/workplan-crosscore.md
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

// **Where a sender finds a sleeping destination** (`sched/waker.hpp`).
//
// Both halves belong to the *destination's* reactor and outlive every send:
// the flag it raises before it blocks, and the handle that unblocks it. A
// sender reads the flag and writes the handle only when it is set, so a
// busy core costs no syscalls at all.
struct WakeTarget {
    const std::atomic<bool>* sleeping = nullptr;
    const Waker* waker = nullptr;
};

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

    // Whether anything is queued for `dst_core` **right now**.
    //
    // Its one caller is the destination's own reactor, in the window
    // between raising its `sleeping` flag and blocking, and its one job is
    // to close the race that window opens: a sender that enqueued just
    // before the flag went up would have read it as clear and skipped the
    // wake. It is therefore allowed to be *conservative in one direction
    // only* — reporting work that has since been taken costs one skipped
    // sleep, while missing work that was published before the caller's
    // fence would cost that message the whole idle block. Implementations
    // must load with at least acquire ordering for that reason.
    virtual bool HasPending(std::uint32_t dst_core) const = 0;

    // Installs the destination's wake target. Called by that core's own
    // reactor at `AttachTransport`, before any peer can send to it, and
    // never again — so this is not synchronised and does not need to be.
    // A core with no target set is simply never woken and falls back to
    // its idle block, which is what every build did before this existed.
    virtual void SetWakeTarget(std::uint32_t core, WakeTarget target) = 0;

    virtual std::uint32_t core_count() const noexcept = 0;

    // Wakes written across every destination (waker.hpp). Instance-wide
    // and diagnostic, so it is given a default rather than made pure: a
    // transport that cannot wake anything - the simulated one, whose
    // reactors are multiplexed by the harness - answers 0 truthfully and
    // has nothing to override.
    virtual std::uint64_t wakes_sent() const noexcept { return 0; }
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
    // Hand-written for one member's sake: `wakes_sent_` is an atomic and so
    // not movable, which would otherwise delete this. Moving a transport
    // two reactors are using is not a supported operation and cannot be
    // made one - `Create` returns by value and the Expeditor stores it,
    // both before any worker exists - so carrying the counter's *value* is
    // the honest move, exactly as `SpscRing` does for its indices.
    RealRingTransport(RealRingTransport&& other) noexcept
        : core_count_(other.core_count_),
          rings_(std::move(other.rings_)),
          wake_(std::move(other.wake_)),
          wakes_sent_(other.wakes_sent_.load(std::memory_order_relaxed)),
          next_peer_(std::move(other.next_peer_)) {}

    Status TrySend(const MessageHeader& header, std::span<const std::byte> payload) override;
    bool TryReceive(std::uint32_t dst_core, MessageHeader& header,
                    std::vector<std::byte>& payload) override;
    bool HasPending(std::uint32_t dst_core) const override;
    void SetWakeTarget(std::uint32_t core, WakeTarget target) override;
    std::uint32_t core_count() const noexcept override { return core_count_; }

    // Wakes actually written across every destination. Zero on a
    // single-core build and on any run where no core ever slept with work
    // arriving; it is the count that says the path is live.
    std::uint64_t wakes_sent() const noexcept override {
        return wakes_sent_.load(std::memory_order_relaxed);
    }

private:
    RealRingTransport(std::uint32_t core_count, std::vector<SpscRing> rings)
        : core_count_(core_count), rings_(std::move(rings)),
          wake_(core_count), next_peer_(core_count, 0) {}

    // Row-major (src, dst): rings_[src * n + dst].
    SpscRing& RingFor(std::uint32_t src, std::uint32_t dst) noexcept {
        return rings_[static_cast<std::size_t>(src) * core_count_ + dst];
    }
    const SpscRing& RingFor(std::uint32_t src, std::uint32_t dst) const noexcept {
        return rings_[static_cast<std::size_t>(src) * core_count_ + dst];
    }

    std::uint32_t core_count_ = 0;
    std::vector<SpscRing> rings_;

    // One per destination core, written once by that core's reactor at
    // AttachTransport and read by every sender thereafter (waker.hpp). A
    // plain vector because it is not mutated after that: the write happens
    // on the startup thread before the destination's worker exists, which
    // is the same ordering every other per-core wiring in this engine
    // relies on.
    std::vector<WakeTarget> wake_;
    std::atomic<std::uint64_t> wakes_sent_{0};

    // Where the next TryReceive(dst) starts its sweep over peers. A
    // rotating start is what keeps a busy peer from starving a quiet one:
    // always starting at peer 0 would let core 0 monopolize every drain.
    // Per destination, and only ever touched by that destination's own
    // thread.
    std::vector<std::uint32_t> next_peer_;
};

// Per-core-pair ring sizing (docs/spec/sched.md §10 leaves it `[OPEN]`).
//
// Both `[PROPOSED]`: they are the parameters `RealRingTransport::Create()`
// takes, held beside it so there is one place to change them when the
// pipeline gives them a workload to be measured against. 256 slots of
// 1 KiB is 256 KiB per directed pair - at 4 cores, 4 MiB of rings for the
// whole instance.
//
// The payload is deliberately *not* `crosscore.md` §4's 32 KiB batch
// target: no batch is sent yet, and sizing every ring for a message that
// does not exist would cost 8 MiB per pair on a promise. P4 raises it when
// it has something to put in it.
//
// **A message struct may now be sized from this** - SS1's shipped
// statement fills exactly one slot - so raising it widens the longest
// shippable statement with it, and lowering it narrows one. That coupling
// is deliberate and asserted at the struct rather than left to be
// discovered (server/statement_ship_service.hpp).
inline constexpr std::size_t kCoreRingSlots = 256;
inline constexpr std::size_t kCoreRingPayloadBytes = 1024;

}  // namespace kds::sched
