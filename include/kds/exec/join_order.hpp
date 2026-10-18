#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/catalog/schema.hpp"
#include "kds/exec/budget.hpp"
#include "kds/exec/sort.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/stats/plan_stats.hpp"

// The join order's cost model (instructions/v3.2.0/workorder-cb-join-planner.md
// §3.2; instructions/v3.2.0/workorder-cc-plan-statistics.md §3).
//
// A cost is a function of a compiled chain - its kinds, its residual's
// literals (a range's span read as its half-octave band), its build
// annotations, its sort and LIMIT - of the catalog's identifier bounds (a
// unique index, a Transition's states), of the statement's limits and,
// since CC-S3, of the published plan statistics (docs/spec/plan-stats.md):
// a relation's prior rows, a block's observed join-subset cardinalities and
// its decline facts, each read as its band. One statement over one catalog
// and one statistics generation compiles to one plan. The constants below
// are the model where no statistic is published, not measurements: each
// says what it stands for, and none is a setting.
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

// The inner build's per-row costs: bucketing one row once, and probing the
// map once per outer row.
inline constexpr double kBuildInsertCost = 2.0;
inline constexpr double kBuildProbeCost = 1.0;

// The statement limits the planner prices against, handed to `Compile` by a
// client's SELECT under `join_order = planned` (CB-S2, CC-S1).
struct PlanOptions {
    // The core's view of the published statistics (CC-S3), or null: the
    // compile pins it only for a block it searches, so a point read and a
    // tuple key chain never touch it.
    stats::PlanStatsSource* statistics = nullptr;
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
    // base_step_id]` (CB-Q3) - for its identifier bounds, a unique index and
    // a Transition's states (CC-Q11). Empty, no bound is read.
    std::uint32_t base_step_id = 0;
    std::span<const catalog::TableAccess* const> written_access;
    // Each written position's rows: its published prior, at least 1, or
    // `kAssumedRelationRows` (CC-S3). Resolved once a compile, not per order.
    // Empty, every relation is the constant.
    std::span<const double> written_rows;
    // The block's posterior, found by its exact key in the snapshot the
    // compile pinned, or null (CC-S3). A fact is read only here and through
    // `written_rows` (plan-stats.md PS9).
    const stats::PlanStatsSnapshot::Posterior* posterior = nullptr;
    // Whether a published fact prices this block - a prior for one of its
    // relations, or its posterior. It, and nothing broader, turns on the
    // statistics' margins: the build's quarter cap here and the 10 % switch
    // margin in the compile (CC-Q8, CC-Q9), so a snapshot that knows nothing
    // of a block prices it exactly as no snapshot does.
    bool informed = false;
    // Out: the rows each executed slot is estimated to pass on, filled when
    // set - for ANALYZE, on the chain the compile keeps.
    std::vector<double>* step_rows = nullptr;
};

struct ChainEstimate {
    double cost = 0;  // in walked rows; +inf for an order the planner must not take
    double rows = 0;  // the reply's rows before LIMIT, at least 1
};

// A build the statistics price is taken only with this margin under the
// cap (CC-Q9): an estimate too low is the costly direction - the executor
// declines the build into a per-row walk - and a band is +-30 % wide.
inline constexpr double kBuildCapMargin = 4.0;

// The estimated cost of running `chain`'s top-level steps in their order,
// with its sort or its LIMIT (CC-Q10). Sub-chains are not costed: a planned
// block carries none.
ChainEstimate EstimateChain(const StepChain& chain, const CostInputs& in);

}  // namespace kds::exec
