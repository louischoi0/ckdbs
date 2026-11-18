#include "kds/exec/admin_plans.hpp"

#include <utility>

#include "kds/parser/lexer.hpp"

namespace kds::exec {

const PlanVariant& AdminPlan::Choose(std::string_view statement) const {
    if (variants.size() == 1) return variants.front();
    const std::vector<parser::StatementArgument> args = parser::ArgumentsOf(statement);
    for (const PlanVariant& variant : variants) {
        if (PlanRuleHolds(variant.rule, args)) return variant;
    }
    // Unreachable for a plan the DDL made, whose last variant is `ELSE`.
    return variants.back();
}

AdminPlans::Partition& AdminPlans::PartitionOf(const CapturedKey& key) noexcept {
    return partitions_[(CapturedKeyHash{}(key) >> 32) % kPartitions];
}

std::shared_ptr<const AdminPlan> AdminPlans::Find(const CapturedKey& key) {
    Partition& partition = PartitionOf(key);
    const LatchGuard hold(&partition.latch);
    const auto it = partition.plans.find(key);
    if (it == partition.plans.end() || !it->second.enabled) return nullptr;
    return it->second.plan;
}

std::optional<std::string> AdminPlans::Holder(const CapturedKey& key) {
    Partition& partition = PartitionOf(key);
    const LatchGuard hold(&partition.latch);
    const auto it = partition.plans.find(key);
    if (it == partition.plans.end()) return std::nullopt;
    return it->second.plan->name;
}

void AdminPlans::Install(std::shared_ptr<const AdminPlan> plan, bool enabled) {
    Partition& partition = PartitionOf(plan->key);
    const LatchGuard hold(&partition.latch);
    const CapturedKey key = plan->key;
    auto [it, fresh] = partition.plans.try_emplace(key);
    if (fresh) plans_.fetch_add(1, std::memory_order_relaxed);
    if (it->second.enabled != enabled) {
        if (enabled) enabled_.fetch_add(1, std::memory_order_relaxed);
        else enabled_.fetch_sub(1, std::memory_order_relaxed);
    }
    it->second = Entry{std::move(plan), enabled};
}

bool AdminPlans::Erase(const CapturedKey& key) {
    Partition& partition = PartitionOf(key);
    const LatchGuard hold(&partition.latch);
    const auto it = partition.plans.find(key);
    if (it == partition.plans.end()) return false;
    if (it->second.enabled) enabled_.fetch_sub(1, std::memory_order_relaxed);
    plans_.fetch_sub(1, std::memory_order_relaxed);
    partition.plans.erase(it);
    return true;
}

std::optional<std::string> AdminPlans::EnabledNaming(catalog::Oid oid) {
    for (Partition& partition : partitions_) {
        const LatchGuard hold(&partition.latch);
        for (const auto& [key, entry] : partition.plans) {
            if (entry.enabled && key.Names(oid)) return entry.plan->name;
        }
    }
    return std::nullopt;
}

void AdminPlans::ForgetRelation(catalog::Oid oid) {
    if (plans() == 0) return;
    for (Partition& partition : partitions_) {
        const LatchGuard hold(&partition.latch);
        for (auto it = partition.plans.begin(); it != partition.plans.end();) {
            if (!it->first.Names(oid)) {
                ++it;
                continue;
            }
            if (it->second.enabled) enabled_.fetch_sub(1, std::memory_order_relaxed);
            plans_.fetch_sub(1, std::memory_order_relaxed);
            it = partition.plans.erase(it);
        }
    }
}

}  // namespace kds::exec
