#pragma once

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/parser/ast.hpp"

// `Compile(AST) -> StepChain` (docs/parser-v2-workplan.md V14).
//
// This is the seam the whole v2 plan is built around. The parser is a
// bolt-on to the existing recursive-descent one and will be replaced by
// the blueprint parser in phase V-6; the acceptance criterion for that
// rewrite is that it emits **identical chains** over V01's corpus. That is
// a checkable statement only because compilation is a separate, pure step
// with a named contract - which is this.
//
// Pure in the sense that matters: same statement plus same catalog gives
// the same chain, bit for bit, every time. No clock, no randomness, no
// container iteration order, no address-dependent decision. The chain
// layout is therefore a function of the AST alone, hence of `pattern_id` -
// which is what makes a trail recorded under one execution replayable
// under the next.
//
// Name resolution lives here rather than in the parser (spec I5, under the
// bolt-on). The parser produces `ColumnName{qualifier, name}`; this turns
// it into `ColumnRef{up, rel_slot, col_pos}` and nothing downstream ever
// sees a name again.

namespace kds::exec {

// Compiles one SELECT statement.
//
// Fails with:
//   NotFound        a relation the statement names is not in the catalog
//   InvalidArgument a column that resolves to no relation, or to more
//                   than one; the message carries the byte position
//   Unsupported     a form the compiler does not yet lower - today that
//                   is any predicate carrying a subquery (V15)
StatusOr<StepChain> Compile(catalog::Catalog& catalog, const parser::SelectStmt& stmt);

// Compiles a single-relation WHERE clause - what UPDATE and, later,
// DELETE have instead of a chain.
//
// Exists so those statements evaluate predicates through the *same*
// resolved-index path a chain does. The alternative is a second evaluator
// that matches column names against one schema, which is what V16 deleted
// and why: it cannot say which relation a name belongs to, and an unknown
// name comes back as "no match" - dropping every row instead of failing.
// Two evaluators would also be two answers to "does this row qualify".
//
// `binding` is what the statement calls the relation, so a qualified
// predicate can be checked against it. Predicates carrying subqueries are
// Unsupported here until V18.
// Returns a one-relation Step carrying the clause: `residual` for the
// ordinary conjuncts and `sub_chains` for the subquery ones.
//
// Every sub-chain is attached to the step, including uncorrelated ones -
// so an uncorrelated subquery is re-evaluated per row here, where a SELECT
// would hoist it and run it once. Correct but wasteful, and deliberately
// so: UPDATE walks the relation itself rather than through the step VM, so
// it has no "before the chain opens" to hoist into. Worth revisiting if
// UPDATE ever runs through a chain.
StatusOr<Step> CompileWhere(catalog::Catalog& catalog, const catalog::TableAccess& access,
                            std::string_view binding,
                            const std::vector<parser::Condition>& where);

}  // namespace kds::exec
