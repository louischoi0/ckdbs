#include "kds/exec/captured_plans.hpp"

#include <algorithm>
#include <utility>

#include "kds/parser/lexer.hpp"

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

bool Names(const CapturedKey& key, catalog::Oid oid) noexcept {
    return std::find(key.oids.begin(), key.oids.begin() + key.n, oid) !=
           key.oids.begin() + key.n;
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
                                                    const CapturedOrder& order,
                                                    std::string_view statement) {
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
    // Queued before the partition is let go, so a `ForgetRelation` that
    // erased the entry finds its capture in the queue or not yet anywhere.
    CapturedPlan plan{key, order, parser::ShapeText(statement)};
    const LatchGuard queue(&queue_latch_);
    pending_.push_back(std::move(plan));
    pending_count_.store(pending_.size(), std::memory_order_relaxed);
    return order;
}

bool CapturedPlans::Load(const CapturedKey& key, const CapturedOrder& order) {
    Partition& partition = PartitionOf(key);
    const LatchGuard hold(&partition.latch);
    if (partition.plans.contains(key)) return false;
    if (shapes_.fetch_add(1, std::memory_order_relaxed) >= kMaxShapes) {
        shapes_.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    partition.plans.emplace(key, order);
    loaded_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool CapturedPlans::Erase(const CapturedKey& key) {
    Partition& partition = PartitionOf(key);
    const LatchGuard hold(&partition.latch);
    if (partition.plans.erase(key) == 0) return false;
    shapes_.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

void CapturedPlans::ForgetRelation(catalog::Oid oid) {
    for (Partition& partition : partitions_) {
        const LatchGuard hold(&partition.latch);
        const std::size_t erased =
            std::erase_if(partition.plans, [&](const auto& plan) { return Names(plan.first, oid); });
        shapes_.fetch_sub(erased, std::memory_order_relaxed);
    }
    const LatchGuard queue(&queue_latch_);
    std::erase_if(pending_, [&](const CapturedPlan& plan) { return Names(plan.key, oid); });
    pending_count_.store(pending_.size(), std::memory_order_relaxed);
    forgotten_.push_back(oid);
}

std::vector<CapturedPlan> CapturedPlans::TakePending(std::size_t max) {
    const LatchGuard queue(&queue_latch_);
    const std::size_t n = std::min(max, pending_.size());
    std::vector<CapturedPlan> taken(std::make_move_iterator(pending_.begin()),
                                    std::make_move_iterator(pending_.begin() +
                                                            static_cast<std::ptrdiff_t>(n)));
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
    pending_count_.store(pending_.size(), std::memory_order_relaxed);
    return taken;
}

void CapturedPlans::Requeue(std::vector<CapturedPlan> plans) {
    // A shape forgotten while its capture was out with the task is not put
    // back: asked of the table first, each partition's latch alone.
    std::erase_if(plans, [&](const CapturedPlan& plan) {
        const std::optional<CapturedOrder> held = Find(plan.key);
        return !held.has_value() || *held != plan.order;
    });
    const LatchGuard queue(&queue_latch_);
    pending_.insert(pending_.begin(), std::make_move_iterator(plans.begin()),
                    std::make_move_iterator(plans.end()));
    pending_count_.store(pending_.size(), std::memory_order_relaxed);
}

std::vector<catalog::Oid> CapturedPlans::TakeForgotten() {
    const LatchGuard queue(&queue_latch_);
    return std::exchange(forgotten_, {});
}

}  // namespace kds::exec
