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
    return RingFor(header.src_core, header.dst_core).TrySend(header, payload);
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
