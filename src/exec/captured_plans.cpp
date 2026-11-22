#include "kds/exec/captured_plans.hpp"

#include <algorithm>
#include <utility>

#include "kds/parser/lexer.hpp"
#include "kds/storage/varheap.hpp"

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

std::size_t CapturedKeyHash::operator()(const CapturedKey& key) const noexcept {
    std::uint64_t h = Mix(key.fetch_id ^ key.n);
    for (std::size_t k = 0; k < key.n && k < key.oids.size(); ++k) h = Mix(h ^ key.oids[k]);
    for (std::size_t k = 0; k < key.v && k < key.views.size(); ++k) h = Mix(h ^ ~key.views[k]);
    return static_cast<std::size_t>(h);
}

CapturedPlans::Partition& CapturedPlans::PartitionOf(const CapturedKey& key) noexcept {
    // The high bits, so a partition's own table - which reads the low ones -
    // spreads within it.
    return partitions_[(CapturedKeyHash{}(key) >> 32) % kPartitions];
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
    // The shape text before any latch - a statement can be megabytes - and
    // no longer than a row keeps of it (`EncodePlanText`). A capture is one
    // call a shape, bar a lost race, so the text is rarely made for nothing.
    CapturedPlan plan{key, order, parser::ShapeText(statement)};
    if (plan.statement.size() > varheap::kMaxValueSize) {
        plan.statement.resize(varheap::kMaxValueSize);
    }
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
    std::size_t forgot = 0;
    for (Partition& partition : partitions_) {
        const LatchGuard hold(&partition.latch);
        const std::size_t erased =
            std::erase_if(partition.plans, [&](const auto& plan) { return plan.first.Names(oid); });
        shapes_.fetch_sub(erased, std::memory_order_relaxed);
        forgot += erased;
    }
    const LatchGuard queue(&queue_latch_);
    std::erase_if(pending_, [&](const CapturedPlan& plan) { return plan.key.Names(oid); });
    pending_count_.store(pending_.size(), std::memory_order_relaxed);
    // Owed only where a shape was forgotten: a row exists for a shape the
    // table held - written, or out with a step, which keeps its entry until
    // this erased it - so a drop of a relation no plan names costs the task
    // no scan.
    if (forgot != 0) forgotten_.push_back(oid);
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
    // back. Asked and queued under one hold of its partition, as an install
    // is: asked first and queued after the hold, a `ForgetRelation` passing
    // in between would erase the shape, find the queue not yet holding it,
    // and owe its oid to a step that runs before the capture is written - a
    // row for a dropped relation that only the next mount removes.
    // Back to front, each at the head, so the queue keeps the step's order.
    for (auto plan = plans.rbegin(); plan != plans.rend(); ++plan) {
        Partition& partition = PartitionOf(plan->key);
        const LatchGuard hold(&partition.latch);
        const auto held = partition.plans.find(plan->key);
        if (held == partition.plans.end() || held->second != plan->order) continue;
        const LatchGuard queue(&queue_latch_);
        pending_.insert(pending_.begin(), std::move(*plan));
        pending_count_.store(pending_.size(), std::memory_order_relaxed);
    }
}

std::vector<catalog::Oid> CapturedPlans::TakeForgotten() {
    const LatchGuard queue(&queue_latch_);
    return std::exchange(forgotten_, {});
}

}  // namespace kds::exec
