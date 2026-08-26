#include "kds/sched/ring_transport.hpp"

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
    Status sent = RingFor(header.src_core, header.dst_core).TrySend(header, payload);
    if (!sent.ok()) return sent;

    // After the push, never before (RingTransport::WakeTarget): the ring's
    // release-store is what the woken core's drain reads through, and a
    // wake issued first can be consumed by a block that ends before the
    // message is visible - which puts the core back to sleep with work
    // waiting, the exact failure this whole path exists to remove.
    WakeTarget(header.dst_core);
    return Status::OK();
}

bool RealRingTransport::HasPending(std::uint32_t dst_core) const noexcept {
    if (dst_core >= core_count_) return false;
    // The whole column: every peer's ring into this core, self-send
    // included. No rotation and no fairness question - this answers
    // "anything at all", not "what next".
    for (std::uint32_t src = 0; src < core_count_; ++src) {
        if (!RingFor(src, dst_core).empty()) return true;
    }
    return false;
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
