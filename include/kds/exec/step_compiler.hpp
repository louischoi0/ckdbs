#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/set_expr.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/txn/read_view.hpp"
#include "kds/parser/ast.hpp"

// `Compile(AST) -> StepChain` (docs/spec/parser-v2.md; background: the
// parser-v2 workplan's V14, at `1769487`). Compilation is a separate, pure
// step with a named contract, so a parser change is checkable as "emits
// identical chains".
//
// Pure in the sense that matters: same statement plus same catalog gives
// the same chain, bit for bit, every time. No clock, no randomness, no
// container iteration order, no address-dependent decision. The chain
// layout is therefore a function of the AST alone - and its steps of the
// AST without its select list, which only the projection mask reads, hence
// of `fetch_id` (AP-S2) - which is what makes a trail recorded under one
// execution replayable under the next, and under another select list.
//
// Name resolution lives here rather than in the parser (spec I5). The parser produces `ColumnName{qualifier, name}`; this turns
// it into `ColumnRef{up, rel_slot, col_pos}` and nothing downstream ever
// sees a name again.

namespace kds::exec {

// Compiles one SELECT statement.
//
// Fails with (among others):
//   the catalog's status  a relation the statement names does not resolve
//                         (`Catalog::FindTableOidByName`)
//   InvalidArgument       a column that resolves to no relation, or to more
//                         than one, or a mistyped comparison; the message
//                         carries the byte position
//   Unsupported           what the architecture declines - a comparison
//                         across decimal widths or scales, `SUM` over
//                         uint64, a value subquery projecting more than one
//                         column, nesting past `parser::kMaxSubqueryDepth`
//   NotImplemented        `HAVING`, `ORDER BY` over an aggregated statement
//
// `view`, when given, is the reader's catalog visibility
// (workplan-ddl-transactional.md DT3c): a relation whose creating
// transaction it cannot see does not resolve, so the statement is refused
// as "unknown relation" rather than compiled against something that does
// not exist for this reader. Null is "see everything", which is every
// caller outside a session and the fast path while no DDL is in flight.
//
// `declare` is the statement's read borrow, declared **at** the bind,
// before the schema is read (AT-R1, `workorder-at-m3-uniformity.md`):
// declared after resolution, a DDL committing in between would release its
// `X` before the reader asked for `IS`. Once granted, no DDL takes the `X`
// until the statement ends, which protects the schema this compile reads.
// The ask is non-blocking and a refusal is not an error - a read borrow
// never refuses a read (`read_borrow.hpp` says what that defends and what
// it does not). A bind's declaration is a `PositionSink` position over the
// whole id space. Null is "declare nothing", every caller that is not
// executing a client's statement.
//
// `context`, when given, is the statement's `NOW()` instead of a fresh
// reading: a scalar subquery of an UPDATE reads its UPDATE's (BJ-S7), so
// the statement has one `NOW()`.
class PositionSink;

StatusOr<StepChain> Compile(catalog::Catalog& catalog, const parser::SelectStmt& stmt,
                            const txn::ReadView* view = nullptr,
                            PositionSink* declare = nullptr,
                            const StatementContext* context = nullptr);

// Compiles a single-relation WHERE clause - what UPDATE and, later,
// DELETE have instead of a chain.
//
// Exists so those statements evaluate predicates through the *same*
// resolved-index path a chain does: one answer to "does this row qualify",
// and an unknown name refused rather than read as "no match" (V16 deleted
// the name-matching second evaluator for that).
//
// `binding` is what the statement calls the relation, so a qualified
// predicate can be checked against it.
// Returns a one-relation Step carrying the clause: `residual` for the
// ordinary conjuncts, `fn_residual` for the function conjuncts (AP-S4),
// `bool_residual` for the boolean ones (BU-S1) and `sub_chains` for the
// subquery ones, a boolean conjunct's leaves among them (`in_tree`, BU-S3).
//
// Every sub-chain is attached to the step, including uncorrelated ones -
// so an uncorrelated subquery is re-evaluated per row here, where a SELECT
// would hoist it and run it once. Correct but wasteful: UPDATE walks the
// relation itself rather than through the step VM, so it has no "before the
// chain opens" to hoist into.
// `view` as on `Compile` above: it reaches the subqueries this lowers,
// which resolve relations of their own.
// `declare` as on `Compile` above, and for the same reason: `access` is
// already resolved by the caller, but the subqueries this lowers bind
// relations of their own, and a write statement's *read* of another
// relation is a read like any other (AT-R1).
//
// `context` is the statement's (exec/functions.hpp): a write parked mid-walk
// recompiles on its resume and must hand the same one back, or `NOW()` would
// filter the rows after the park against a later instant. Null takes a fresh
// one, which is right for a statement compiled once.
StatusOr<Step> CompileWhere(catalog::Catalog& catalog, const catalog::TableAccess& access,
                            std::string_view binding,
                            const std::vector<parser::Condition>& where,
                            const txn::ReadView* view = nullptr,
                            PositionSink* declare = nullptr,
                            const StatementContext* context = nullptr);

// Resolve an UPDATE's SET list against the relation, before any storage is
// touched (keystoneid-invariant.md K-M3).
//
// Two refusals, and the distinction between their codes is the policy
// rules.md states: `kInvalidArgument` for a column the relation has not
// got - simply wrong - and **`kUnsupported` for the primary key**, which
// is understood and declined. K2 makes a Keystone id immutable for the
// life of the tuple: the clustered tree, every secondary index entry,
// every Cabin entry and every recorded trail address the row by that id,
// so changing it in place would retarget all of them at once and no
// per-tuple validation could detect it. The refusal is not a missing
// feature to be implemented later - it is the invariant - which is why
// this is J2's `Unsupported`-with-a-position and never a slow path.
//
// Both messages carry `Assignment::byte_offset`. Called at compile time,
// once per statement, so the row loop below it can assume every target is
// a real, assignable column.
//
// **BJ-S4 - what the row loop reads.** `compiled`, when given, receives one
// entry per assignment in list order: the target's schema position, and for
// an expression the typed tree (`set_expr.hpp`) the loop evaluates against
// each qualifying row's before-image. A **constant** expression (no column
// reference) into a foreign-key column is folded here, and `constant` keeps
// the foreign-key hoist's value; a non-constant one is refused
// `NotImplemented` at its byte (BJ-Q10: a per-row parent descent under the
// write walk's exclusive leaf is the open page-latch cycle).
struct CompiledAssignment {
    std::size_t column_no = 0;
    // Null for a bare literal, whose value is `Assignment::val`.
    std::unique_ptr<const TypedSetExpr> expr;
    // The value a foreign-key column's hoist resolves: the bare literal's, or
    // the folded constant. Unset for every other assignment, and for a
    // constant whose fold failed - the row that meets that failure raises it
    // before any foreign-key check.
    std::optional<parser::AstValue> constant;
};

Status CompileAssignments(const catalog::TableAccess& access,
                          const std::vector<parser::Assignment>& assignments,
                          std::vector<CompiledAssignment>* compiled = nullptr,
                          const SubqueryResolver* subqueries = nullptr);

}  // namespace kds::exec
