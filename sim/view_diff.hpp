#pragma once

// sim/view_diff.hpp - DP-R1's differential cell
// (`instructions/v3.2.0/workorder-dp-view.md` DP-S3): **a statement through a
// view returns what the same statement with the view's body written as a
// derived table returns.**
//
// Seeded and engine-independent like the rest of the corpus: two relations
// filled with seeded rows (nullable columns among them, so three-valued
// comparisons and `NOT IN` over a NULL are in the tested surface), then views
// over random absorbable bodies - one relation, with a subquery conjunct one
// time in three, or an inner equi-join, with random conjuncts and boolean
// trees, their column list sometimes permuting the body's names; or a view
// over an earlier view, to the depth bound - each read by random outer
// statements: the view alone, joined to a relation on either side, joined to
// itself, folded by `GROUP BY`, beside an `IN` or a correlated `EXISTS`, or
// read inside an `IN` subquery. Every outer statement runs twice on one
// instance: through the view, which must absorb natively (its `ANALYZE`
// names the view's relations by their path), and with the body spelled as
// `(<body>) AS <view>`, which the native grammar refuses and the query layer
// answers as a derived table. Each sorts by every column it returns, or is
// compared with its rows sorted, so the two replies are equal byte for byte
// or the run fails, naming the seed, the body and the statement; a refusal
// on either side is a failure too.

#include "sim/loop.hpp"

namespace kds::sim {

SimVerdict RunViewDifferential(const SimConfig& config);

}  // namespace kds::sim
