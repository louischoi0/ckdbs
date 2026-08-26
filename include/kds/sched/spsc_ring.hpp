#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/sched/ring_message.hpp"

// One directed core-to-core channel: a single-producer, single-consumer
// lock-free ring (docs/spec/sched.md §5, docs/inflight/in-progress/workplan-crosscore.md P1).
//
// ---- Concurrency protocol ----------------------------------------------
//
// **Exactly one thread sends and exactly one thread receives.** That is not
// a recommendation - the index protocol below is only correct under it, and
// it is what the whole topology buys: with one ring per *ordered* core pair
// (N² of them for N cores), every channel has one writer and one reader by
// construction, matching the shared-nothing ownership rule with no atomics
// beyond the two indices.
//
// The indices are the only atomics in this class, and per workplan
// guideline 1 they are the only atomics the engine has outside this file's
// peers. The protocol:
//
//   - `write_pos_` is written **only** by the producer, with release
//     ordering, after the slot's bytes are in place. The release is what
//     publishes those bytes.
//   - `read_pos_` is written **only** by the consumer, with release
//     ordering, after it has finished copying the slot out. The release is
//     what tells the producer the slot is reusable.
//   - Each side reads the *other* index with acquire ordering, and its own
//     with relaxed - nobody else can change it.
//
// Neither side ever blocks, spins, or sleeps. A full ring fails the send
// (sched.md §5: "no blocking, no throw... silent drop is forbidden"), and
// the caller's answer to that is sched/send_retry.hpp - yield the task and
// try again on a later reactor iteration (M7).
//
// ---- Allocation ---------------------------------------------------------
//
// Everything is allocated once, at construction. Send and receive copy into
// and out of preallocated slots and allocate nothing, which is sched.md
// §2's "the loop body performs no allocation in steady state" applied to
// the one structure the loop touches on every iteration.

namespace kds::sched {

// A slot holds one header plus up to `max_payload` bytes, so a message
// never spans slots. That costs the padding on short messages and buys the
// property the index protocol rests on: one slot is one message, so a
// reader that sees `write_pos_` advance sees a *whole* message.
class SpscRing {
public:
    // `capacity_slots` is rounded **up** to a power of two so that
    // index-to-slot is a mask rather than a modulo - the same derivation
    // rule every sized constant in this codebase carries. Rounding up
    // rather than refusing: a caller asking for 100 slots wants at least
    // 100, and 128 is the cheapest way to give it to them.
    //
    // Both sizes are parameters and neither is a compiled-in constant.
    // `crosscore.md` §4's 32 KiB batch target and its ring-capacity sizing
    // are both `[PROPOSED]` and `[OPEN]` respectively (sched.md §10), so
    // nothing here may pin either.
    static StatusOr<SpscRing> Create(std::size_t capacity_slots, std::size_t max_payload);

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&) noexcept;
    SpscRing& operator=(SpscRing&&) = delete;

    // Producer side. Copies `header` and `payload` into the next free slot.
    //
    // Fails with ResourceExhausted when the ring is full - the caller must
    // handle it (sched.md §5) and must not treat it as an error to report
    // upward; it is backpressure. Fails with InvalidArgument if the payload
    // is larger than this ring's slot, which is a programming error rather
    // than a runtime condition: the sender is expected to have sized its
    // messages against `max_payload()`.
    //
    // `header.payload_len` is overwritten with `payload.size()` - the
    // length that is actually copied is the one the receiver is told about,
    // so the two cannot disagree.
    Status TrySend(const MessageHeader& header, std::span<const std::byte> payload);

    // Consumer side. Copies the oldest unread message into `header` and
    // `payload` (resized to fit) and frees its slot. Returns false when the
    // ring is empty - which is the ordinary answer on most reactor
    // iterations, and so is not a Status.
    bool TryReceive(MessageHeader& header, std::vector<std::byte>& payload);

    // Approximate, and honestly so: either index may move under a caller on
    // the other thread. For diagnostics and tests, never for a decision -
    // the decisions are TrySend's failure and TryReceive's false, both of
    // which are exact for the thread asking.
    std::size_t size() const noexcept;
    bool empty() const noexcept { return size() == 0; }

    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t max_payload() const noexcept { return max_payload_; }

private:
    SpscRing(std::size_t capacity, std::size_t max_payload);

    std::size_t SlotOffset(std::uint64_t pos) const noexcept {
        return (static_cast<std::size_t>(pos) & mask_) * slot_stride_;
    }

    std::size_t capacity_ = 0;      // slots, a power of two
    std::size_t mask_ = 0;          // capacity_ - 1
    std::size_t max_payload_ = 0;   // bytes per slot, past the header
    std::size_t slot_stride_ = 0;   // sizeof(MessageHeader) + max_payload_

    // One flat buffer rather than a vector of slots: a slot is a header
    // followed by bytes, and giving that a type would mean a variable-length
    // struct or an allocation per slot.
    std::vector<std::byte> storage_;

    // Monotonic positions, never wrapped - `mask_` maps them to slots. The
    // difference is the occupancy, which stays correct across the 2^64
    // wrap because unsigned subtraction does.
    std::atomic<std::uint64_t> write_pos_{0};
    std::atomic<std::uint64_t> read_pos_{0};
};

}  // namespace kds::sched
