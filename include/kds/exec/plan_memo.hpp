#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "kds/stats/plan_key.hpp"

// **The join planner's order memo** (CC-S4, instructions/v3.2.0/
// workorder-cc-plan-statistics.md CC-Q12 as revised): the order a searched
// block chose, remembered by everything the choice read, so a statement
// that repeats compiles its written order and its remembered one - two
// compiles - where the search compiles up to 24.
//
// **An exact cache, never a policy.** The key holds every input the search
// reads besides the catalog's facts it compiles from: the block's identity
// (fetch id and written relation oids), the statement's literals
// (`arg_hash` - a literal can change a step's kind, a range's span, a
// LIMIT's bound, in an order the written chain does not show), the
// statistics generation, the catalog's stamps (any DDL or memo rebuild moves
// one) and the statement's limits. A hit names the order the search would
// choose, and the order is compiled again through the full ladder, so no
// stale chain, literal or page id is reused. Hashes - the fetch id and the
// literals' - can collide; a collision serves another statement's order,
// which returns the same rows, as every order does.
//
// One per core, owned by its dispatcher, touched by that core alone: no
// latch. A fixed table, two ways a set, the least recently used way
// replaced.

namespace kds::exec {

struct PlanMemoKey {
    stats::PlanKey block;
    std::uint64_t arg_hash = 0;
    std::uint64_t generation = 0;
    std::uint64_t catalog_word = 0;
    std::uint64_t catalog_version = 0;
    std::uint64_t join_build_max_rows = 0;
    std::uint64_t sort_max_rows = 0;

    bool operator==(const PlanMemoKey&) const = default;
};

class PlanMemo {
public:
    static constexpr std::size_t kSets = 512;
    static constexpr std::size_t kWays = 2;

    // The remembered order - written positions in executed order - or
    // nullopt.
    std::optional<std::array<std::uint8_t, stats::kMaxPosteriorRelations>> Find(
        const PlanMemoKey& key);
    // Remembers `order` for `key`, replacing what the key held.
    void Store(const PlanMemoKey& key,
               const std::array<std::uint8_t, stats::kMaxPosteriorRelations>& order);

    std::uint64_t hits() const noexcept { return hits_; }
    std::uint64_t misses() const noexcept { return misses_; }

private:
    struct Entry {
        PlanMemoKey key;
        std::array<std::uint8_t, stats::kMaxPosteriorRelations> order{};
        std::uint64_t used_at = 0;  // 0: empty
    };
    static std::size_t SetOf(const PlanMemoKey& key) noexcept;

    std::vector<Entry> entries_ = std::vector<Entry>(kSets * kWays);
    std::uint64_t clock_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
};

}  // namespace kds::exec
