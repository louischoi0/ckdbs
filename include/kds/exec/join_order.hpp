#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/catalog/schema.hpp"
#include "kds/exec/budget.hpp"
#include "kds/exec/sort.hpp"
#include "kds/exec/step_chain.hpp"

// The join order's cost model (instructions/v3.2.0/workorder-cb-join-planner.md
// §3.2; instructions/v3.2.0/workorder-cc-plan-statistics.md §3).
//
// A cost is a function of a compiled chain - its kinds, its residual's
// literals (a range's span read as its half-octave band), its build
// annotations, its sort and LIMIT - of the catalog's identifier bounds (a
// unique index, a Transition's states) and of the statement's limits. It
// reads no row count, page count or statistic, so one statement over one
// catalog compiles to one plan. The constants below are the model, not
// measurements: each says what it stands for, and none is a setting.
//
// The unit is one row visited by a walk.

namespace kds::exec {

// The rows a relation is assumed to hold. Only its size against the other
// constants matters: a walk must cost far more than a descent.
inline constexpr double kAssumedRelationRows = 100000.0;

// One tuple key or index descent, in walked rows: a root-to-leaf path of a
// few pages, each a binary search.
inline constexpr double kDescentCost = 20.0;

// The fraction of a relation a conjunct to a literal keeps: an equality on a
// non-tuple-key column, one end of a range (a two-sided range keeps the
// square), and any other comparison or a boolean or function conjunct.
inline constexpr double kEqualitySelectivity = 0.01;
inline constexpr double kRangeEndSelectivity = 0.3;
inline constexpr double kOtherSelectivity = 0.5;

// The fraction of a relation one outer row's join-key equality keeps, on a
// column that is not the tuple key: one row of an unfiltered relation. It
// is the tuple key join's own rate, so a join's estimated size is the same
// from either side - an order is never preferred for a result the other
// order was estimated not to produce.
inline constexpr double kJoinSelectivity = 1.0 / kAssumedRelationRows;

// The inner build's per-row costs: bucketing one row once, and probing the
// map once per outer row.
inline constexpr double kBuildInsertCost = 2.0;
inline constexpr double kBuildProbeCost = 1.0;

// The statement limits the planner prices against, handed to `Compile` by a
// client's SELECT under `join_order = planned` (CB-S2, CC-S1).
struct PlanOptions {
    // `join_build_max_rows`: a build the model expects to bucket more rows
    // is priced as the per-row walk the executor falls back to; 0, no build.
    std::size_t join_build_max_rows = kDefaultJoinBuildMaxRows;
    // `sort_max_rows`: whether a sort an order adds is bounded by LIMIT
    // (CC-Q10).
    std::size_t sort_max_rows = kDefaultSortMaxRows;
    // The statement's fetch id, 0 when it has none: a planned block's key
    // (docs/spec/plan-stats.md PS2). With 0 no key is set.
    std::uint64_t fetch_id = 0;
};

// What the model reads besides the chain itself (CC-S1).
struct CostInputs {
    PlanOptions limits;
    // Whether the written order's chain sorts. An order that adds a sort the
    // written order elides is never taken unless LIMIT bounds the sort to a
    // top-N heap, which cannot refuse (CC-Q10): a reply's size is a fact no
    // estimate here knows, so an unbounded added sort could meet
    // `sort_max_rows` where the written order streams.
    bool written_sorted = false;
    // The block's relations in **written** order, and the block's first step
    // id: step k's relation is `written_access[steps[k].step_id -
    // base_step_id]` (CB-Q3). Read for identifier bounds alone - a unique
    // index and a Transition's states (CC-Q11). Empty, no bound is read.
    std::uint32_t base_step_id = 0;
    std::span<const catalog::TableAccess* const> written_access;
};

struct ChainEstimate {
    double cost = 0;  // in walked rows; +inf for an order the planner must not take
    double rows = 0;  // the reply's rows before LIMIT, at least 1
};

// The estimated cost of running `chain`'s top-level steps in their order,
// with its sort or its LIMIT (CC-Q10). Sub-chains are not costed: a planned
// block carries none.
ChainEstimate EstimateChain(const StepChain& chain, const CostInputs& in);

}  // namespace kds::exec
