#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>

#include "kds/catalog/oid.hpp"

// A planned block's identity (docs/spec/plan-stats.md PS2): what the join
// planner's posteriors are keyed by, and what a planned chain carries to the
// fold. Its own header because `exec::StepChain` carries one.

namespace kds::stats {

// The relations a posterior key names at most: the planner's exhaustive
// search bound (`kMaxExhaustiveJoinRelations`, step_compiler.cpp).
inline constexpr std::size_t kMaxPosteriorRelations = 4;
// A posterior per subset of written positions; mask 0 is unused.
inline constexpr std::size_t kPosteriorMasks = std::size_t{1} << kMaxPosteriorRelations;

// The statement's fetch, and its relations by oid in written order. Matched
// exactly, never by hash alone: a fetch id hashes names, not oids, so
// `USE ns` or a DROP and re-CREATE reach new keys.
struct PlanKey {
    std::uint64_t fetch_id = 0;
    std::uint8_t n = 0;
    std::array<catalog::Oid, kMaxPosteriorRelations> oids{};

    bool operator==(const PlanKey&) const = default;
    auto operator<=>(const PlanKey&) const = default;
};

struct PlanKeyHash {
    std::size_t operator()(const PlanKey& key) const noexcept;
};

}  // namespace kds::stats
