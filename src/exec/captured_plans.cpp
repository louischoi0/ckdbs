#include "kds/exec/captured_plans.hpp"

namespace kds::exec {

namespace {

// splitmix64's finalizer.
std::uint64_t Mix(std::uint64_t x) noexcept {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

}  // namespace

std::size_t CapturedPlans::KeyHash::operator()(const CapturedKey& key) const noexcept {
    std::uint64_t h = Mix(key.fetch_id ^ key.n);
    for (std::size_t k = 0; k < key.n && k < key.oids.size(); ++k) h = Mix(h ^ key.oids[k]);
    return static_cast<std::size_t>(h);
}

CapturedPlans::Partition& CapturedPlans::PartitionOf(const CapturedKey& key) noexcept {
    // The high bits, so a partition's own table - which reads the low ones -
    // spreads within it.
    return partitions_[(KeyHash{}(key) >> 32) % kPartitions];
}

std::optional<CapturedOrder> CapturedPlans::Find(const CapturedKey& key) {
    Partition& partition = PartitionOf(key);
    const LatchGuard hold(&partition.latch);
    const auto it = partition.plans.find(key);
    if (it == partition.plans.end()) return std::nullopt;
    return it->second;
}

std::optional<CapturedOrder> CapturedPlans::Capture(const CapturedKey& key,
                                                    const CapturedOrder& order) {
    Partition& partition = PartitionOf(key);
    const LatchGuard hold(&partition.latch);
    const auto it = partition.plans.find(key);
    if (it != partition.plans.end()) return it->second;  // the first install's
    // The ceiling is the instance's and the latch a partition's: taken by
    // an increment that is given back, so no interleaving admits a shape
    // past it.
    if (shapes_.fetch_add(1, std::memory_order_relaxed) >= kMaxShapes) {
        shapes_.fetch_sub(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    partition.plans.emplace(key, order);
    return order;
}

}  // namespace kds::exec
