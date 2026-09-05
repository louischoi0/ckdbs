#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/sched/spsc_ring.hpp"
#include "kds/sched/waker.hpp"
#include "kds/sched/waker_table.hpp"

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

    virtual std::uint32_t core_count() const noexcept = 0;

    // The largest payload `TrySend` will accept, in bytes - the same
    // number that call enforces, so a sender sizing against this one
    // cannot drift from what it will actually be allowed to send.
    virtual std::size_t max_payload() const noexcept = 0;
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
    // Defaulted since AU-S1b moved the wake counter to `WakerTable` and left
    // nothing unmovable here. Moving a transport two reactors are using is
    // still not a supported operation - `Create` returns by value and the
    // Expeditor stores it, both before any worker exists.
    RealRingTransport(RealRingTransport&& other) noexcept = default;

    // **Where this transport's sends find a sleeping destination**
    // (AR0-6-R1, `waker_table.hpp`). Instance-wide and installed once, not
    // once per core: the table is registered into by each reactor for
    // itself, so a send needs only to know which table to ask. Called on the
    // startup thread before any worker exists; a transport with none simply
    // never kicks, and every destination falls back to its idle block -
    // which is what every build did before the wake existed at all.
    void AttachWakers(const WakerTable* wakers) noexcept { wakers_ = wakers; }

    // Which table, so an assembly test can say "this one" rather than
    // "some one" - `expeditor_test.cpp`'s two-core cell does, because a
    // transport left pointing at nothing is silent: every send still
    // succeeds and every destination waits out its block.
    const WakerTable* wakers() const noexcept { return wakers_; }

    Status TrySend(const MessageHeader& header, std::span<const std::byte> payload) override;
    bool TryReceive(std::uint32_t dst_core, MessageHeader& header,
                    std::vector<std::byte>& payload) override;
    bool HasPending(std::uint32_t dst_core) const override;
    std::uint32_t core_count() const noexcept override { return core_count_; }

    // Every ring in the matrix is created with the same `max_payload`
    // (`Create`'s one parameter), so any of them answers for all.
    //
    // The empty guard answers no live caller: `Create` refuses
    // `core_count == 0`, and the one other empty state - a **moved-from**
    // transport - is not a usable object at all (the move constructor
    // above says so, and `TrySend` on one would index a `rings_` the move
    // emptied while `core_count_` survived it). The guard is here so
    // `front()` cannot be UB, not because something reaches this in that
    // state; every `max_payload()` call in the engine is
    // `server::MakeStepSend`'s, on a transport it was handed live.
    std::size_t max_payload() const noexcept override {
        return rings_.empty() ? 0 : rings_.front().max_payload();
    }

private:
    RealRingTransport(std::uint32_t core_count, std::vector<SpscRing> rings)
        : core_count_(core_count), rings_(std::move(rings)), next_peer_(core_count, 0) {}

    // Row-major (src, dst): rings_[src * n + dst].
    SpscRing& RingFor(std::uint32_t src, std::uint32_t dst) noexcept {
        return rings_[static_cast<std::size_t>(src) * core_count_ + dst];
    }
    const SpscRing& RingFor(std::uint32_t src, std::uint32_t dst) const noexcept {
        return rings_[static_cast<std::size_t>(src) * core_count_ + dst];
    }

    std::uint32_t core_count_ = 0;
    std::vector<SpscRing> rings_;

    // Borrowed, never owned: the table outlives this transport by design
    // (AR0-6 retires the ring and keeps the wake), and the Expeditor builds
    // it beside this one for exactly that reason.
    const WakerTable* wakers_ = nullptr;

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
