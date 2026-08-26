#include "kds/sched/ring_transport.hpp"

#include <atomic>
#include <string>

namespace kds::sched {

StatusOr<RealRingTransport> RealRingTransport::Create(std::uint32_t core_count,
                                                     std::size_t capacity_slots,
                                                     std::size_t max_payload) {
    if (core_count == 0) {
        return Status::InvalidArgument("ring transport: core_count must be at least 1");
    }

    std::vector<SpscRing> rings;
    rings.reserve(static_cast<std::size_t>(core_count) * core_count);
    for (std::uint32_t i = 0; i < core_count; ++i) {
        for (std::uint32_t j = 0; j < core_count; ++j) {
            auto ring = SpscRing::Create(capacity_slots, max_payload);
            if (!ring.ok()) return ring.status();
            rings.push_back(std::move(ring.value()));
        }
    }
    return RealRingTransport(core_count, std::move(rings));
}

Status RealRingTransport::TrySend(const MessageHeader& header,
                                  std::span<const std::byte> payload) {
    if (header.src_core >= core_count_ || header.dst_core >= core_count_) {
        return Status::InvalidArgument(
            "ring transport: message from core " + std::to_string(header.src_core) + " to core " +
            std::to_string(header.dst_core) + " is outside the " + std::to_string(core_count_) +
            " cores this instance runs");
    }
    if (Status sent = RingFor(header.src_core, header.dst_core).TrySend(header, payload);
        !sent.ok()) {
        return sent;
    }

    // **The wake** (waker.hpp). The message is published; if the
    // destination is asleep it will not see it until its idle block
    // expires, which was measured at a millisecond and is what this exists
    // to remove.
    //
    // **Why the flag cannot be missed**, which is the whole correctness of
    // this path. Two threads, two variables, in opposite orders:
    //
    //   sender:   publish the message, then read `sleeping`
    //   receiver: set `sleeping`, then read the ring
    //
    // The fence below and its twin in `Scheduler::RunOnce` make that the
    // store-buffer pattern, where sequential consistency forbids *both*
    // reads returning the stale value. So at least one of two things
    // happens: the sender sees the flag and writes the wake, or the
    // receiver sees the message and does not sleep. Never neither - which
    // would be a message stranded for the whole block - and both is
    // harmless, costing one skipped sleep.
    const WakeTarget& target = wake_[header.dst_core];
    if (target.sleeping == nullptr) return Status::OK();
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!target.sleeping->load(std::memory_order_seq_cst)) return Status::OK();
    target.waker->Wake();
    wakes_sent_.fetch_add(1, std::memory_order_relaxed);
    return Status::OK();
}

bool RealRingTransport::HasPending(std::uint32_t dst_core) const {
    if (dst_core >= core_count_) return false;
    // Every peer's ring into this core, `size()`'s acquire loads doing the
    // ordering work the header's contract asks for. Non-destructive and
    // O(cores) - it runs once per iteration, and only on an iteration that
    // was about to block.
    for (std::uint32_t src = 0; src < core_count_; ++src) {
        if (!RingFor(src, dst_core).empty()) return true;
    }
    return false;
}

void RealRingTransport::SetWakeTarget(std::uint32_t core, WakeTarget target) {
    if (core >= core_count_) return;
    wake_[core] = target;
}

bool RealRingTransport::TryReceive(std::uint32_t dst_core, MessageHeader& header,
                                   std::vector<std::byte>& payload) {
    if (dst_core >= core_count_) return false;

    // One sweep over every peer, starting where the last one left off, so
    // no peer can be starved by a busier one (see next_peer_).
    const std::uint32_t start = next_peer_[dst_core];
    for (std::uint32_t offset = 0; offset < core_count_; ++offset) {
        const std::uint32_t src = (start + offset) % core_count_;
        if (RingFor(src, dst_core).TryReceive(header, payload)) {
            next_peer_[dst_core] = (src + 1) % core_count_;
            return true;
        }
    }
    return false;
}

}  // namespace kds::sched
