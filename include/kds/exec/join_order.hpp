#pragma once

#include <cstddef>

#include "kds/exec/step_chain.hpp"

// The join order's cost model (instructions/v3.2.0/workorder-cb-join-planner.md
// §3.2, CB-Q2 (a)).
//
// **Data-free.** A cost is a pure function of a compiled chain: its kinds,
// its residual's literals and its build annotations - never a row count, a
// page count or a statistic - so one statement over one catalog compiles to
// one plan, the property `f(shape, catalog)` names (docs/spec/index.md §9).
// The constants below are the model, not measurements: each says what it
// stands for, and none is a setting.
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

// The estimated cost of running `chain`'s top-level steps in their order.
// Sub-chains are not costed: a reordered block carries none. A build the
// model expects to bucket more than `build_cap` rows is costed as the
// per-row walk the executor falls back to.
double ChainCost(const StepChain& chain, std::size_t build_cap);

}  // namespace kds::exec
