#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/budget.hpp"
#include "kds/exec/join_order.hpp"
#include "kds/exec/set_expr.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/txn/read_view.hpp"
#include "kds/parser/ast.hpp"

// `Compile(AST) -> StepChain` (docs/spec/parser-v2.md; background: the
// parser-v2 workplan's V14, at `1769487`). Compilation is a separate, pure
// step with a named contract, so a parser change is checkable as "emits
// identical chains".
//
// Pure in the sense that matters: same statement plus same catalog plus
// same limits and the same published statistics generation gives the same
// chain, bit for bit, every time. No clock, no
// randomness, no container iteration order, no address-dependent decision.
// A written-order chain is a function of the AST alone - and its steps of
// the AST without its select list, which only the projection mask reads,
// hence of `fetch_id` (AP-S2); a planned block's order also reads the
// catalog and `plan` (parser-v2.md I12). A trail recorded under one order is
// replayable under another: a step's id is its written position.
//
// Name resolution lives here rather than in the parser (spec I5). The parser produces `ColumnName{qualifier, name}`; this turns
// it into `ColumnRef{up, rel_slot, col_pos}` and nothing downstream ever
// sees a name again.

namespace kds::exec {

// Compiles one SELECT statement.
//
// Fails with (among others):
//   the catalog's status  a relation the statement names does not resolve
//                         (`Catalog::ResolveRelation`)
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
// it does not). A bind's declaration is a `PositionSink::Position` of the
// relation, and nothing finer. Null is "declare nothing", every caller that
// is not executing a client's statement.
//
// `context`, when given, is the statement's `NOW()` instead of a fresh
// reading: a scalar subquery of an UPDATE reads its UPDATE's (BJ-S7), so
// the statement has one `NOW()`.
//
// `plan`, when given, asks the join planner to choose the top-level block's
// join order (CB-S2, instructions/v3.2.0/workorder-cb-join-planner.md §3.2);
// null keeps the written order. Only a client's SELECT asks
// (`join_order = planned`); a compile that only checks a statement does not.

// **The relations a statement declares, told to whoever holds its borrow**
// (AO-S6e-b, the read borrow; **the relation alone since CP-S2a**, CP-Q3
// (a)). One caller: the compiler, once per relation it binds, between the
// name and the schema (AT-S1). Nothing finer than the relation is declared
// - nothing in this engine moves a key between units (`heap-and-tuple.md`
// §7.1), so the slice `IS` the outermost walk reported at every page
// boundary until CP-S2a had no consumer and left with the slice unit
// (`txn.md` §5). The compiler knows nothing about locks: the implementation
// is `read_borrow.hpp`'s.
class PositionSink {
public:
    virtual ~PositionSink() = default;
    // `unqualified_in`: the namespace the name was resolved in where it was
    // written unqualified (NS5), 0 for a qualified name - what tells one
    // text's two statements apart (`stats/statement_binding.hpp`).
    virtual void Position(catalog::Oid rel, catalog::Oid unqualified_in) = 0;
};

StatusOr<StepChain> Compile(catalog::Catalog& catalog, const parser::SelectStmt& stmt,
                            const txn::ReadView* view = nullptr,
                            PositionSink* declare = nullptr,
                            const StatementContext* context = nullptr,
                            const PlanOptions* plan = nullptr);

// Whether an administrator's plan can be made for `stmt`'s top-level block
// (CX-S1b, CX-S3b): a join block the join planner orders - the one test
// `Compile` applies before it considers any order but the written one - or
// a statement over one relation, which has no order and an arm to pin. OK,
// or `NotImplemented` saying why: a joined `*`, or a subquery predicate
// (CB-Q4 (a)).
Status PlannableBlock(const parser::SelectStmt& stmt);

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
//
// `absorbed`, when given, is the one-relation block a view absorbed to
// (DP-Q10, `view_absorb.hpp`): `access` is its relation, and `where` and the
// select list below are its names - the reader's through the view, the
// body's under its own text.
StatusOr<Step> CompileWhere(catalog::Catalog& catalog, const catalog::TableAccess& access,
                            std::string_view binding,
                            const std::vector<parser::Condition>& where,
                            const txn::ReadView* view = nullptr,
                            PositionSink* declare = nullptr,
                            const StatementContext* context = nullptr,
                            const parser::SelectStmt* absorbed = nullptr);

// **Where a write descends instead of walking** (CU-S3, CU-S3b), read off
// `CompileWhere`'s step by the rules a read's step is keyed by: a conjunct
// `<tuple key> = <literal>` names one key (`WriteKeyOf`), and a value list
// on the tuple key names a set (`WriteKeySetOf`; `KeySet`, step_chain.hpp),
// its items ascending and distinct. Any conjunct may stand beside either:
// the descent picks the slot to look at and the caller tests the row it
// finds against the whole WHERE, the key's own conjunct included (CU-R1),
// so `id = 1 AND v = 3` writes what its walk writes.
//
// **A WHERE holding a subquery names neither**, in a conjunct or under a
// boolean one. A write evaluates no subquery under the leaf it holds
// exclusive, so such a statement collects its rows in a read walk first
// (`CommandDispatcher::WriteWalk::CollectTargets`), and that pass is not
// keyed (`docs/pending/keyed-write-beside-a-subquery.md`).
//
// **An embedded primary key pinned whole names one key too** (DR-S3e): on a
// relation whose key is its Keystone id, `a = 1 AND b = 2` is the id those
// values encode to (`exec/embedded_key.hpp`), found by the same descent.
std::optional<std::uint64_t> WriteKeyOf(const catalog::TableAccess& access, const Step& where);
std::optional<KeySet> WriteKeySetOf(const Step& where);

// **A write that names a unique key whole descends too** (DR-S3a): where
// the WHERE pins every key column of a unique index by an equality with a
// literal, the rows it can write are the ones that key's run names
// (`IndexRunTupleKeys`, index_maintain.hpp) - a superset, as every index
// run is (IX1), each row tested against the whole WHERE where it is found
// (CU-R1). This is what makes a primary key that is not the Keystone a key
// a write is found by: without it the statement walks the relation.
//
// The lowest `index_oid` whose key the WHERE pins whole; nullopt where
// there is none, where the WHERE holds a subquery (`WriteKeyOf`'s reason)
// or where a value does not encode, and the walk answers. A unique index
// alone: its run for one key is short by the check that keeps it unique,
// where any other index's may be the relation.
struct WriteIndexKey {
    const catalog::TableAccess::IndexRef* index = nullptr;  // into `access.indexes`
    std::vector<std::byte> key;                             // `index->key_width` bytes
};
std::optional<WriteIndexKey> WriteIndexKeyOf(const catalog::TableAccess& access,
                                             const Step& where);

// **The select list of a statement over one relation, as `Compile` resolves
// a chain's**: each written column's reference, heading, type and type mod,
// by the same code (`Compile`'s step 5), so a row rendered from it is the
// row the chain would render. `star` is `SelectStmt::star()` - never
// "the list is empty", which an aggregated statement's is too - and gives
// the visible columns' headings alone, the references being the caller's to
// build from the schema, as they are for a chain. For the dispatcher's
// locking read (DC), which compiles no chain.
struct SelectList {
    std::vector<ColumnRef> projection;
    std::vector<std::string> names;
    std::vector<std::uint32_t> types;
    std::vector<std::uint32_t> type_mods;
};
StatusOr<SelectList> CompileSelectList(const catalog::TableAccess& access,
                                       std::string_view binding,
                                       const std::vector<parser::ColumnName>& list, bool star,
                                       const parser::SelectStmt* absorbed = nullptr);

// Resolve an UPDATE's SET list against the relation, before any storage is
// touched (keystoneid-invariant.md K-M3).
//
// Two refusals, and the distinction between their codes is the policy
// rules.md states: `kInvalidArgument` for a column the relation has not
// got - simply wrong - and **`kUnsupported` for the tuple key**, which
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
