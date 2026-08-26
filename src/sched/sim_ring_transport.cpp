#include "kds/sched/sim_ring_transport.hpp"

#include <string>

namespace kds::sched {

StatusOr<SimRingTransport> SimRingTransport::Create(std::uint32_t core_count, const Clock& clock,
                                                   SimTransportConfig config) {
    if (core_count == 0) {
        return Status::InvalidArgument("sim ring transport: core_count must be at least 1");
    }
    if (config.max_delay_ns < config.min_delay_ns) {
        return Status::InvalidArgument("sim ring transport: max_delay_ns " +
                                       std::to_string(config.max_delay_ns) +
                                       " is below min_delay_ns " +
                                       std::to_string(config.min_delay_ns));
    }
    if (config.capacity_per_edge == 0) {
        return Status::InvalidArgument("sim ring transport: capacity_per_edge must be at least 1");
    }
    return SimRingTransport(core_count, clock, config);
}

std::uint64_t SimRingTransport::NextRandom() noexcept {
    rng_state_ += 0x9E37'79B9'7F4A'7C15ULL;
    std::uint64_t z = rng_state_;
    z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBULL;
    return z ^ (z >> 31);
}

Status SimRingTransport::TrySend(const MessageHeader& header,
                                 std::span<const std::byte> payload) {
    if (header.src_core >= core_count_ || header.dst_core >= core_count_) {
        return Status::InvalidArgument(
            "sim ring transport: message from core " + std::to_string(header.src_core) +
            " to core " + std::to_string(header.dst_core) + " is outside the " +
            std::to_string(core_count_) + " cores this instance runs");
    }

    // Occupancy is counted per edge, not globally, so that one saturated
    // edge cannot make an unrelated pair look full - which is the property
    // the real per-core-pair rings have and the reason backpressure is
    // meaningful at all.
    std::size_t on_edge = 0;
    for (const Pending& p : pending_) {
        if (p.header.src_core == header.src_core && p.header.dst_core == header.dst_core) {
            ++on_edge;
        }
    }
    if (on_edge >= config_.capacity_per_edge) {
        return Status::ResourceExhausted("sim ring transport: edge " +
                                         std::to_string(header.src_core) + "->" +
                                         std::to_string(header.dst_core) + " is full (" +
                                         std::to_string(config_.capacity_per_edge) + " messages)");
    }

    MonoTimeNs delay = config_.min_delay_ns;
    if (config_.max_delay_ns > config_.min_delay_ns) {
        const MonoTimeNs span = config_.max_delay_ns - config_.min_delay_ns + 1;
        delay += NextRandom() % span;
    }

    Pending pending;
    pending.header = header;
    pending.header.payload_len = static_cast<std::uint32_t>(payload.size());
    pending.payload.assign(payload.begin(), payload.end());
    pending.deadline = clock_->Now() + delay;
    pending.seq = next_seq_++;
    pending_.push_back(std::move(pending));
    ++sent_count_;
    return Status::OK();
}

bool SimRingTransport::HasPending(std::uint32_t dst_core) const {
    if (dst_core >= core_count_) return false;
    // Deliverable *now*, which is the question the caller is asking - a
    // message whose simulated delay has not elapsed is not something this
    // reactor could take if it skipped its block.
    const MonoTimeNs now = clock_->Now();
    for (const Pending& p : pending_) {
        if (p.header.dst_core == dst_core && p.deadline <= now) return true;
    }
    return false;
}

bool SimRingTransport::TryReceive(std::uint32_t dst_core, MessageHeader& header,
                                  std::vector<std::byte>& payload) {
    if (dst_core >= core_count_) return false;

    const MonoTimeNs now = clock_->Now();

    // Earliest deadline wins; equal deadlines break on send order. Both
    // halves matter: the first is what makes an injected delay mean
    // something, the second is what makes a zero-delay run exactly
    // in-order and therefore substitutable for the real transport.
    auto best = pending_.end();
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (it->header.dst_core != dst_core) continue;
        if (it->deadline > now) continue;
        if (best == pending_.end() || it->deadline < best->deadline ||
            (it->deadline == best->deadline && it->seq < best->seq)) {
            best = it;
        }
    }
    if (best == pending_.end()) return false;

    header = best->header;
    payload = std::move(best->payload);
    pending_.erase(best);
    return true;
}

}  // namespace kds::sched
