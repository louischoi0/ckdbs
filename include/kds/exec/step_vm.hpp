#pragma once

#include <functional>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/budget.hpp"
#include "kds/exec/chain_frame.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/storage/visit.hpp"

// Executes a compiled StepChain (docs/parser-v2-workplan.md V17).
//
// ---- What this is not --------------------------------------------------
//
// There is **no shape analysis here**. The executor does not look at a
// statement and decide anything: which relation is read in which order,
// which step descends and which walks, where each predicate is evaluated -
// all of it was settled by the compiler and is sitting in the chain. This
// file is a loop that does what it is told. That is deliberate and
// grep-checkable: a second place that reasons about shape is a second
// answer to "what does this statement do", and the first one is the one a
// Waystone trail was recorded against.
//
// ---- The three rules for nested access (spec I15) -----------------------
//
// R1 - **decode before descending.** A relation walk hands out a span into
//      a live page frame. Descending into the next step fetches another
//      page, and nothing pins the first: the span may be invalidated
//      underneath. So a step decodes its row into the chain frame and
//      *releases the span* before the nested loop runs. Enforced by
//      PageSpanGuard below rather than left to discipline - it is safe
//      today only because nothing evicts, which is exactly the kind of
//      accident that stops being safe silently.
//
// R2 - **nested steps are read-only.** Every page a nested step touches is
//      fetched for read. Structural: the walk is opened with
//      PageAccess::kRead and there is no write path in this file at all.
//
// R3 - **recursion is bounded at both ends.** The parser caps nesting and
//      so does the compiler; the executor carries its own depth counter,
//      because a bound only one layer enforces is not a bound.

namespace kds::exec {

// Called once per output row, with the frame holding every bound relation.
// Returning kStop ends the statement successfully - which is what `LIMIT`
// and a top-level `EXISTS` will use, and which V03 made expressible.
using RowSink = std::function<StatusOr<storage::VisitControl>(const ChainFrame&)>;

// What an execution did, for tests and for V19's meters.
//
// `relation_opens` counts the times a step began reading its relation - a
// walk started or a descent made. It is how "a false uncorrelated EXISTS
// opens zero pages" is checkable: the claim is about work not done, and
// work not done leaves no other trace.
struct ExecStats {
    std::uint64_t relation_opens = 0;
    std::uint64_t rows_examined = 0;   // tuples decoded and filtered
    std::uint64_t sub_chain_runs = 0;  // sub-chain evaluations, hoisted or per row

    // V19's meters.
    //
    // `probe_memo_hits` counts descents skipped because the step's key
    // repeated. `correlated_scans` counts sub-chain evaluations whose
    // first step is a Scan - the shape that makes a statement quadratic,
    // and the one worth being able to point at when a statement is
    // refused for its budget.
    std::uint64_t probe_memo_hits = 0;
    std::uint64_t correlated_scans = 0;
};

// Runs `chain` and calls `sink` for each row that satisfies it.
//
// Rows arrive in the order the chain produces them: steps[0] in relation
// order, and for each of its rows, steps[1] in relation order, and so on.
// That is the written order of the statement, which spec §1 makes a client
// contract.
//
// Fails with `CardinalityViolation` when a scalar subquery returns more
// than one row - a runtime verdict, since parse time cannot prove
// cardinality in general (spec §2) - and with `ResourceExhausted` when the
// statement spends `budget` (exec/budget.hpp).
Status Execute(catalog::Catalog& catalog, storage::PageStore& store, const StepChain& chain,
               const RowSink& sink, ExecStats* stats = nullptr,
               const Budget& budget = Budget());

// Evaluates one step's whole conjunct list - ordinary predicates *and*
// sub-chains - against a frame already holding that step's row.
//
// For statements that walk a relation themselves rather than through
// `Execute`: UPDATE today, DELETE when it lands. They need it so their
// WHERE means exactly what a SELECT's does, down to `NOT IN`'s tri-state
// collapse. A second evaluator would be a second answer to "does this row
// qualify", and the two would drift on the first NULL.
StatusOr<bool> EvaluateConjuncts(catalog::Catalog& catalog, storage::PageStore& store,
                                 const std::vector<const catalog::Schema*>& schemas,
                                 const Step& step, const ChainFrame& frame,
                                 ExecStats* stats = nullptr, const Budget& budget = Budget());

// Whether a debug-build page-span guard has fired during this process.
// Exposed for the test that deliberately violates R1; production code has
// no reason to read it.
bool PageSpanGuardTripped() noexcept;
void ResetPageSpanGuard() noexcept;

}  // namespace kds::exec
