#include "kds/exec/admin_plans.hpp"

#include <utility>

#include "kds/parser/lexer.hpp"

namespace kds::exec {

namespace {

struct ArmWord {
    ArmPin::Arm arm;
    std::string_view word;
};
inline constexpr ArmWord kArmWords[] = {{ArmPin::Arm::kTupleKey, "TUPLE_KEY"},
                                        {ArmPin::Arm::kIndex, "INDEX"},
                                        {ArmPin::Arm::kCabin, "CABIN"},
                                        {ArmPin::Arm::kScan, "SCAN"}};

}  // namespace

std::string_view ArmName(ArmPin::Arm arm) noexcept {
    for (const ArmWord& entry : kArmWords) {
        if (entry.arm == arm) return entry.word;
    }
    return "LADDER";
}

std::optional<ArmPin::Arm> ArmFromName(std::string_view word) noexcept {
    for (const ArmWord& entry : kArmWords) {
        if (SameName(entry.word, word)) return entry.arm;
    }
    return std::nullopt;
}

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
    if (it->second.enabled != enabled) CountEnabled(key, enabled);
    it->second = Entry{std::move(plan), enabled};
}

void AdminPlans::CountEnabled(const CapturedKey& key, bool enabled) noexcept {
    const std::uint64_t one = enabled ? 1 : ~std::uint64_t{0};  // +1 or -1
    enabled_.fetch_add(one, std::memory_order_relaxed);
    if (key.n == 1) enabled_single_.fetch_add(one, std::memory_order_relaxed);
}

bool AdminPlans::Erase(const CapturedKey& key) {
    Partition& partition = PartitionOf(key);
    const LatchGuard hold(&partition.latch);
    const auto it = partition.plans.find(key);
    if (it == partition.plans.end()) return false;
    if (it->second.enabled) CountEnabled(key, false);
    plans_.fetch_sub(1, std::memory_order_relaxed);
    partition.plans.erase(it);
    return true;
}

std::optional<std::string> AdminPlans::EnabledNaming(catalog::Oid oid) {
    return EnabledWhere([&](const CapturedKey& key, const AdminPlan&) { return key.Names(oid); });
}

std::optional<std::string> AdminPlans::EnabledPinning(catalog::Oid oid, ArmPin::Arm arm,
                                                      std::string_view name) {
    return EnabledWhere([&](const CapturedKey& key, const AdminPlan& plan) {
        for (const PlanVariant& variant : plan.variants) {
            for (std::size_t w = 0; w < variant.access.size() && w < key.n; ++w) {
                const ArmPin& pin = variant.access[w];
                if (key.oids[w] == oid && pin.arm == arm && !pin.forbid &&
                    SameName(arm == ArmPin::Arm::kIndex ? pin.index : pin.cabin, name)) {
                    return true;
                }
            }
        }
        return false;
    });
}

std::optional<std::string> AdminPlans::EnabledPinningIndex(catalog::Oid oid,
                                                           std::string_view index) {
    return EnabledPinning(oid, ArmPin::Arm::kIndex, index);
}

std::optional<std::string> AdminPlans::EnabledPinningCabin(catalog::Oid oid,
                                                           std::string_view column) {
    return EnabledPinning(oid, ArmPin::Arm::kCabin, column);
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
            if (it->second.enabled) CountEnabled(it->first, false);
            plans_.fetch_sub(1, std::memory_order_relaxed);
            it = partition.plans.erase(it);
        }
    }
}

}  // namespace kds::exec
