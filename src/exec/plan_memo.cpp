#include "kds/exec/plan_memo.hpp"

namespace kds::exec {

std::size_t PlanMemo::SetOf(const PlanMemoKey& key) noexcept {
    // Every field folded in, so variants of one statement spread over sets.
    std::uint64_t h = stats::PlanKeyHash{}(key.block);
    for (const std::uint64_t field : {key.arg_hash, key.generation, key.catalog_word,
                                      key.catalog_version, key.join_build_max_rows,
                                      key.sort_max_rows}) {
        h = (h ^ field) * 0x9E3779B97F4A7C15ULL;
        h ^= h >> 29;
    }
    return static_cast<std::size_t>(h % kSets);
}

std::optional<std::array<std::uint8_t, stats::kMaxPosteriorRelations>> PlanMemo::Find(
    const PlanMemoKey& key) {
    const std::size_t set = SetOf(key);
    for (std::size_t way = 0; way < kWays; ++way) {
        Entry& entry = entries_[set * kWays + way];
        if (entry.used_at != 0 && entry.key == key) {
            entry.used_at = ++clock_;
            ++hits_;
            return entry.order;
        }
    }
    ++misses_;
    return std::nullopt;
}

void PlanMemo::Store(const PlanMemoKey& key,
                     const std::array<std::uint8_t, stats::kMaxPosteriorRelations>& order) {
    const std::size_t set = SetOf(key);
    Entry* victim = &entries_[set * kWays];
    for (std::size_t way = 0; way < kWays; ++way) {
        Entry& entry = entries_[set * kWays + way];
        if (entry.used_at != 0 && entry.key == key) {
            victim = &entry;
            break;
        }
        if (entry.used_at < victim->used_at) victim = &entry;  // empty ways are 0
    }
    victim->key = key;
    victim->order = order;
    victim->used_at = ++clock_;
}

}  // namespace kds::exec
