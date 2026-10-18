#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/oid.hpp"
#include "kds/exec/functions.hpp"
#include "kds/parser/ast.hpp"
#include "kds/stats/plan_key.hpp"

// The compiled form of a SELECT-class statement: an ordered list of steps,
// each reading one relation with one access kind (docs/spec/parser-v2.md §1).
//
// ---- Why a chain, and why written order --------------------------------
//
// The chain's order is execution order. Since CB-S2 the join planner chooses
// it for an inner-join block (`join_order.hpp`, parser-v2.md I12) from the
// statement's shape and the catalog alone, so a repeated query pattern still
// executes the same way every time - which is what makes a recorded
// Waystone trail replayable at all. No decorrelation rewrite exists.
//
// ---- The step-kind table IS the trust model ----------------------------
//
// `AccessKind` is not an executor implementation note. It is simultaneously
// the executor's probe strategy and Waystone's lookup/search line
// (docs/spec/waystone-concpets.md §2), deliberately one decision with two
// consumers rather than two that can drift apart:
//
//   kLookup / kProbe   tuple-key-equality descent. **Trail-replayable** -
//                      completeness follows from tuple key uniqueness, so a
//                      recorded location can replace the descent.
//   kScan / kRange     a search. **Never replayable.** A stored set that
//                      is missing a row inserted since it was recorded is
//                      wrong in a way no per-tuple validation can detect,
//                      because absence has no witness (invariant 9). A
//                      trail may prefetch for these and nothing more.
//
// A step is kLookup/kProbe **iff** its equality binds the relation's first
// schema column - the Keystone tuple key, the only column a lookup can address
// (invariant 11). Any other column, however selective, is a scan.
//
// ---- No identifiers past this point ------------------------------------
//
// A compiled chain carries no column or relation *names*. Resolution
// happens once, here, against the catalog; execution indexes. That is why
// `ColumnRef` is three integers and why `StepPredicate` holds one - a name
// on an execute path means a string compare per row per predicate, and in
// a multi-relation world it also means an unknown column silently reads as
// "no match" instead of an error.

namespace kds::exec {

// An expression Cabin's compiled key function (cabin_expr.hpp), held by
// `CabinProbe::f`.
struct CompiledCabinExpr;

// A resolved reference to one column of one relation.
//
// `up` is a de Bruijn level: 0 is the chain this reference appears in, 1
// its parent, and so on. It maps one-to-one onto the execute-time frame
// stack, which is what lets a predicate be independent of which sub-chain
// it landed in.
struct ColumnRef {
    std::uint16_t up = 0;
    std::uint16_t rel_slot = 0;  // step index within that chain
    std::uint16_t col_pos = 0;   // index into that relation's schema columns

    bool operator==(const ColumnRef&) const = default;
};

// Whether a step reads its relation by tuple key descent or by walking it.
// Written in the order of the table in docs/spec/parser-v2.md §1.
enum class AccessKind : std::uint8_t {
    // tuple key equality against a value known before the chain runs.
    kLookup,
    // tuple key equality against a value produced by an earlier step or by an
    // enclosing chain's row.
    kProbe,
    // tuple key range through the leaf chain, from a low and a high bound
    // on the relation's tuple key: `BETWEEN`, `>=`/`<=`, or with a strict
    // `>`/`<` end, which ranks last (`TupleKeyRangeOf`, step_compiler.cpp).
    kRange,
    // An equality against a literal on a non-tuple-key column that carries a
    // **Cabin** (docs/spec/cabin.md): a probe of the Cabin's observed set,
    // falling back to the walk below when the value has not been observed.
    //
    // The third trust class, and the only kind here that is neither a tuple key
    // descent nor a walk. A Cabin is **authoritative for observed values**:
    // its entry set for an observed value is a superset of the qualifying
    // tuple keys, and the read subtracts the surplus by re-checking visibility and
    // the key equality. So an observed value's *empty* set is an
    // authoritative "no rows" - which nothing else in this enum can say
    // about a non-tuple-key predicate.
    //
    // Whether a column has a Cabin is **catalog** state, so the plan stays
    // `f(shape, catalog)` and nothing about the data influences it. Whether
    // a *value* has been observed is runtime state, and it steers only the
    // branch taken inside this kind, never the kind itself.
    kCabinProbe,
    // An equality on a **prefix of a secondary index's key**
    // (docs/spec/index.md §8): descend the index, walk its entries while the
    // prefix matches, resolve each tuple key through the clustered tree.
    //
    // The second entry in the first trust class: an index is authoritative
    // for **every** key value, where a Cabin is authoritative only for the
    // ones queries have observed. So it is tried ahead of `kCabinProbe`,
    // which is tried ahead of `kFilterScan`, and each is the reason the next
    // one is not needed.
    //
    // Like every kind here it is **catalog** state - which columns carry an
    // index - so the plan stays `f(shape, catalog)` and no property of the
    // data influences it.
    kIndexProbe,
    // The same, narrowed further by an inclusive range on the key column
    // after the matched equality prefix - or a range on the first key column
    // with no equality before it.
    //
    // **Executes identically to `kIndexProbe`**: both walk the entries
    // between two encoded bounds, and the compiler has already written both
    // bounds into `IndexProbe`. The split is a *statistics* distinction, the
    // same one `kFilterScan` draws against `kScan` - an equality names a
    // point the workload asked for, a range names a span - and if a branch
    // between them ever appears in the executor, the "same rows either way"
    // tests are what should stop it.
    kIndexRange,
    // A walk **driven by a filter**: at least one equality against a
    // literal on a non-tuple-key column with no index.
    //
    // Split out of kScan because the two are identical in cost today and
    // completely different as a signal. A `kScan` is a statement that
    // asked for everything; a `kFilterScan` is a statement that asked for
    // a few rows and had to read all of them to find out which - which is
    // exactly the shape a physical optimizer wants to hear about
    // (`docs/spec/heap-and-tuple.md` §7), and which an index or a clustering
    // decision would fix.
    //
    // It is **not** a promise that anything is faster. Both walk the whole
    // relation; only the statistics can tell them apart.
    kFilterScan,
    // Everything else: walk the relation, filtering by whatever residual
    // it carries - or by nothing at all.
    kScan,
};

// True for the kinds a Waystone trail may replace outright. The one place
// this line is drawn, so the executor and the recorder cannot disagree
// about it.
//
// **Unchanged by kRange and kFilterScan**, deliberately. Both are searches:
// a range's completeness comes from the walk, not from a stored set, and a
// filter scan's from reading every row. Invariant 9 lets a trail replace a
// lookup and never a search, so adding a search-class kind cannot move this
// line - which is what made both safe to add without touching Waystone.
//
// **Unchanged by kCabinProbe too, and that one is worth stating.** A Cabin
// *is* authoritative, so it is tempting to read this line as "authoritative
// versus advisory" and let a trail replace a cabin probe. It is not: the
// line invariant 9 draws is **lookup versus search**, and it is drawn there
// because a trail's stored set has no witness for absence - a row inserted
// since it was recorded is missing from it and nothing per-tuple can detect
// that. A Cabin has such a witness (its write hook, spec section 5); a trail
// does not, and does not acquire one by being pointed at a cabined column.
// So a cabin probe stays search-class, and the fact that adding an
// authoritative kind did not move this function is the check that the two
// trust models stayed separate.
constexpr bool IsTrailReplayable(AccessKind kind) noexcept {
    return kind == AccessKind::kLookup || kind == AccessKind::kProbe;
}
// **Unchanged by kIndexProbe and kIndexRange either**, and for the reason
// stated above rather than a new one. An index is authoritative, exactly as
// a Cabin is - and invariant 9's line is *lookup versus search*, not
// authoritative versus advisory. An index probe answers with a **set**, and
// a trail's stored set has no witness for a row inserted since it was
// recorded, which no per-tuple validation can detect. So a trail may
// prefetch for an index step and may never replace one.

// Whether a driving step of `kind` emits its rows in tuple key order - the
// premise of eliding `ORDER BY <driving tuple key> ASC`
// (step_compiler.cpp's elision). A walk or range does, page by page; an
// index step sorts its tuple keys back into that order (IX8a); a lookup or
// probe emits one row. A Cabin probe is excluded by name (BB). **No
// `default`**: a kind added to the enum fails to compile here until it is
// judged, which is the guard a planner that moves any relation onto slot 0
// needs (CC §7).
constexpr bool EmitsTupleKeyOrder(AccessKind kind) noexcept {
    switch (kind) {
        case AccessKind::kLookup:
        case AccessKind::kProbe:
        case AccessKind::kRange:
        case AccessKind::kIndexProbe:
        case AccessKind::kIndexRange:
        case AccessKind::kFilterScan:
        case AccessKind::kScan:
            return true;
        case AccessKind::kCabinProbe:
            return false;
    }
    return false;
}

// The inclusive tuple key bounds a kRange step walks between.
//
// A **hint on top of the residual**, never a replacement for it: the
// conjuncts the bounds came from (`>= low`, `<= high`, or a strict `>`/`<`
// one key outside them) stay in
// `Step::residual` exactly as a lookup's equality does. That is what keeps
// "downgrading any step to a plain kScan cannot change the result" true,
// and it is the property invariant 9's fall-through and the
// scan/probe equivalence tests both rest on.
struct RangeBounds {
    std::uint64_t low = 0;
    std::uint64_t high = 0;
};

// What a kCabinProbe step probes: which Cabin, and with which value.
//
// A **hint on top of the residual**, exactly as RangeBounds is. The equality
// this was derived from stays in `Step::residual`, so downgrading the step
// to a plain kScan still cannot change the result - and here that property
// is doing more work than it does for a range. It is what lets the read path
// serve a *superset* entry set and subtract the surplus by re-filtering: the
// key re-check spec section 4 requires is not extra code, it is the residual
// the compiler already attached.
struct CabinProbe {
    // The `sys.cabins` row's `cabin_id`. Resolved at compile time, so no
    // execute path ever asks the catalog which Cabin a column has.
    std::uint64_t cabin_id = 0;

    // The cabined column's schema position, for the access statistics and
    // for re-deriving the probe value from a decoded row on the write path.
    // An expression Cabin's first input (`f` below).
    std::uint16_t col_pos = 0;

    // An **expression or predicate** Cabin's key function (AQ-R5, R6): set,
    // the step probes `F(row) = value` and the recording walk compares `F` on
    // each row; null, it is a column Cabin and compares `col_pos`. The
    // conjuncts the probe was derived from stay in `Step::residual` and
    // `Step::fn_residual` and re-check every served row.
    std::shared_ptr<const CompiledCabinExpr> f;

    // **Cover** (AQ-R8): the closed interval of `F`'s keys a range of its
    // argument maps to - a `DATE(ts)` Cabin's days for `ts` bounded on both
    // sides. Set, the step is served from the union of those keys' sets, all
    // observed or none (a walk); `value` is unused. The bounds stay in
    // `Step::residual` and re-check every served row.
    std::optional<std::pair<std::int64_t, std::int64_t>> cover;

    // The value being probed for, when the equality binds a literal.
    // Unused when `key_from` below is set.
    parser::AstValue value;

    // The correlated form (docs/spec/cabin.md §4a): the probed value is an
    // *earlier step's* or an enclosing chain's column - a join key - read
    // from the frame per outer row. The outer row is fixed for the whole
    // of this step's execution, so the value is stable exactly as long as
    // a probe or a recording needs it. This is the shape v1 deliberately
    // did not emit.
    std::optional<ColumnRef> key_from;

    // Whether an operator declared this Cabin - `CREATE CABIN`, or a
    // `CABIN` clause on the column at CREATE TABLE - as opposed to the
    // engine having created it on its own judgement.
    //
    // It decides **when a value becomes observed**: n=1 for a declared
    // Cabin, n=2 for an auto one. Exactly the rule `TrailRecorder` already
    // applies to patterns, and the same argument - a declaration *is* the
    // evidence that waiting exists to gather, so asking traffic to prove it
    // again asks a question that was answered.
    //
    // **Only for the literal shape** (cabin.md §4a, amended CB14). The
    // argument above is about a value the operator *named*; the correlated
    // form above probes a value per outer row that nobody named, so it takes
    // `n = 2` whatever this says. `key_from.has_value()` is the test, and
    // `RunCabinStep` is the one place that makes it.
    bool declared = false;

    // Whether the cabin optimizer owns this Cabin (`kCabinOriginAuto` -
    // the promotion pipeline, workplan PHY04). Read by exactly one
    // consumer, the plan printer, so ANALYZE can mark an
    // optimizer-managed probe (PO9): an operator reading a plan needs to
    // know whether the structure serving it is one they declared or one
    // the engine may drop on its own judgement. Not `!declared`, because
    // a legacy unset origin is neither.
    bool managed = false;
};

// What a kIndexProbe / kIndexRange step reads, resolved at compile time.
//
// A **hint on top of the residual**, exactly as `RangeBounds` and
// `CabinProbe` are: the equalities and bounds this was derived from stay in
// `Step::residual`, so downgrading the step to a plain kScan still cannot
// change the result. Here that property carries the whole trust argument -
// an index's entry set for a key is a *superset* of the qualifying visible
// tuple keys (docs/spec/index.md §1), and the surplus is subtracted by re-checking
// the predicate against the resolved version. That re-check is not extra
// code; it is the residual the compiler already attached.
struct IndexProbe {
    catalog::Oid index_oid = 0;

    // The tree. Copied off the cached `TableAccess::IndexRef`, which is the
    // one field on that struct a split can move - and it is republished in
    // place rather than by invalidation (index.md §12a), so a chain
    // compiled before a split still names a page that is *a* page of the
    // tree. IX11 re-reads it from the catalog rather than trusting this
    // across a write.
    PageId root_page_id = kInvalidPageId;
    std::uint16_t key_width = 0;
    std::uint16_t entry_width = 0;

    // How many leading key columns the statement pinned by equality, and
    // whether a range on the next one narrows it further. Together they are
    // what separates the two kinds - and nothing else does.
    std::uint8_t eq_prefix = 0;
    bool ranged = false;

    // The inclusive sort-key bounds the walk runs between, both a whole
    // entry's `entry_width` bytes.
    //
    // **Encoded here, at compile time.** Coercion is a compile-time act
    // (docs/spec/types.md §3.1) and so is the encoding that follows it, so
    // no per-row key building happens on the read path. `low` pads the
    // unpinned tail with 0x00 and `high` with 0xFF, which are the true
    // bounds because a key column's leading discriminator byte is 1 for
    // every value that exists (storage/index/index_page.hpp).
    std::vector<std::byte> low;
    std::vector<std::byte> high;

    // The index's key columns in declared order, and its covered columns.
    // Carried for the access statistics and for IX11's entry-side filtering.
    std::vector<std::uint16_t> key_cols;
    std::vector<std::uint16_t> covered_cols;

    // The correlated form (docs/spec/index.md §8a): the leading key
    // column's value comes from an *earlier step's row* - a join key - so
    // it cannot be encoded at compile time. When set, `eq_prefix == 1`,
    // `ranged == false`, and `low`/`high` above are pure padding templates
    // (all 0x00 / all 0xFF): the executor copies them and encodes this
    // column's frame value into the leading width per outer row, which is
    // the one exception to "no per-row key building" - priced at one
    // fixed-width encode against the full inner walk it replaces. The
    // single authority for the key source: the plan printer renders it,
    // and `Step::key` keeps its two-kind contract.
    std::optional<ColumnRef> key_from;
};

// The walked-join annotation (docs/spec/join-inner-build.md §5): the
// statement-local inner build's compile half.
//
// **An annotation on a kScan, never a kind.** Set when every structure arm
// declined and the step's residual still binds an own column by equality to
// an earlier step's or an enclosing chain's column - the walked-join shape,
// the one the ladder had no answer for. The step stays kScan, so trails,
// access statistics and every kind-switch downstream are untouched by
// construction. An executor that ignores it wholesale
// answers identically: the correlated conjunct stays in `Step::residual`,
// which is the same downgrade-safety every other access hint has.
//
// What makes one build serve every outer row: `TupleKeyBound` accepts literals
// only, so no scan bound varies per outer row - a kScan inner is always the
// full relation, and one walk's map answers every later key (spec §5).
struct BuildKey {
    // The own join column's schema position - what the map is keyed on.
    std::uint16_t col_pos = 0;

    // The outer source, read from the frame per outer row: an earlier
    // step's or an enclosing chain's column - the same `key_from` shape
    // both correlated probes carry.
    ColumnRef key_from;

    // Where in `Step::residual` the correlated conjunct sits, so the build
    // can bucket rows on every *other* conjunct (the non-correlated
    // residual) and the probe can re-evaluate the full list.
    std::uint16_t residual_pos = 0;
};

// The right-hand side of a compiled predicate: a value the statement
// wrote, or another column.
enum class OperandKind : std::uint8_t { kLiteral, kColumn };

struct Operand {
    OperandKind kind = OperandKind::kLiteral;
    parser::AstValue literal;  // kLiteral
    ColumnRef column;          // kColumn
};

// One conjunct, evaluated on a located row. `lhs` is always a column:
// the grammar has no expression on the left (spec I10).
struct StepPredicate {
    ColumnRef lhs;
    parser::CompareOp op = parser::CompareOp::kEq;
    Operand rhs;

    // True for a conjunct the compiler derived by equality propagation
    // (docs/spec/parser-v2.md §5) rather than one the client wrote. Execution
    // ignores it; the consumer is diagnostic truthfulness - ANALYZE marks
    // the line, so a plan never shows a predicate the client cannot find in
    // their text.
    bool derived = false;
};

// ---- The function conjunct (AP-S4) ---------------------------------------
//
// A comparison with a function call on a side: `DATE(ts) = '2026-10-02'`,
// `ts < NOW()` (workorder-ap-function-catalog-fetch-id.md AP-R4, §1.7).
//
// **A residual kind of its own, and the whole of its safety is that.** A
// dozen compiler readers take a `StepPredicate` as `col op value` and turn
// it into a tuple key bound, an index bound, a Cabin probe, a join key or a
// `BuildKey` (step_compiler.cpp). Lowered as one, `DATE(ts) >= '...'` would
// be read as `ts >= '...'` and return wrong rows with no error. So a function
// conjunct is never a `StepPredicate`: it lives in `Step::fn_residual`, which
// none of those readers walks, and is evaluated on a row the step already
// located - after the residual, at the two places every accepted row passes
// (`AcceptTupleAt` and `EvaluateConjuncts`). A step carrying one takes no
// build annotation.
struct FunctionTerm {
    const FunctionEntry* fn = nullptr;
    std::vector<ColumnRef> args;
};

// One side of a function conjunct: a call, or what a `StepPredicate`'s
// right side can be - a literal or a column.
struct FunctionSide {
    std::optional<FunctionTerm> call;  // set: the side is this call
    Operand plain;                     // otherwise
};

struct FunctionPredicate {
    FunctionSide lhs;
    parser::CompareOp op = parser::CompareOp::kEq;
    FunctionSide rhs;  // unused for IS [NOT] NULL

    // The catalog type both sides compare as - the call's result type, or
    // the column's - which `CompareValues` reads.
    std::uint32_t type_val = 0;

    // The statement's constants, taken once per statement at compile.
    StatementContext context;
};

// Every column reference a function conjunct reads, in no particular order.
template <typename Fn>
void ForEachColumn(const FunctionPredicate& pred, Fn&& fn) {
    for (const FunctionSide* side : {&pred.lhs, &pred.rhs}) {
        if (side->call.has_value()) {
            for (const ColumnRef& ref : side->call->args) fn(ref);
        } else if (side->plain.kind == OperandKind::kColumn) {
            fn(side->plain.column);
        }
    }
}

// ---- The boolean conjunct (BU-S1) -----------------------------------------
//
// A conjunct holding `OR` or `NOT`: `a = 1 OR b = 2`, `NOT (c > 3)`
// (workorder-bu-basic-query-surface.md). **A residual kind of its own, for
// the function conjunct's reason** (above): its comparisons are
// `StepPredicate`s, and a reader that took one for a conjunct of the
// statement would key, bound or probe on a comparison the `OR` makes
// optional (BU-R5). So it lives in `Step::bool_residual`, which no key,
// bound, index, Cabin, join-key or build reader walks, and is evaluated
// beside `fn_residual` at the same two sites.
//
// **Three-valued, collapsed once, at the root** (BU-R4): a comparison with
// a NULL operand is unknown inside the tree, `NOT unknown` is unknown, and
// only the root's unknown becomes false. `CompareValues` collapses per
// comparison, which is right for a flat AND-list and wrong under `NOT`.
//
// **A subquery predicate is a leaf too since BU-S3**: `kSubChain` names a
// sub-chain of the step the conjunct is placed at, marked `in_tree` there so
// every walk of the step tree still sees it and no conjunct loop runs it on
// its own. A conjunct that reads one is evaluated with the step's sub-chains,
// after the row's ordinary conjuncts, rather than with the residual.
struct BoolPredicate {
    enum class Op : std::uint8_t { kAnd, kOr, kNot, kCompare, kFunction, kSubChain };
    Op op = Op::kCompare;
    std::vector<BoolPredicate> children;   // kAnd, kOr (two or more), kNot (one)
    StepPredicate compare;                 // kCompare
    std::optional<FunctionPredicate> fn;   // kFunction
    std::uint32_t sub_chain = 0;           // kSubChain: an index into `Step::sub_chains`
    bool reads_sub_chain = false;          // a kSubChain at or under this node
};

// Every column reference a boolean conjunct reads, in no particular order.
// A `kSubChain` leaf's columns are its sub-chain's, which every reader of
// `Step::sub_chains` walks already.
template <typename Fn>
void ForEachColumn(const BoolPredicate& pred, Fn&& fn) {
    switch (pred.op) {
        case BoolPredicate::Op::kCompare:
            fn(pred.compare.lhs);
            if (pred.compare.rhs.kind == OperandKind::kColumn) fn(pred.compare.rhs.column);
            return;
        case BoolPredicate::Op::kFunction:
            ForEachColumn(*pred.fn, fn);
            return;
        case BoolPredicate::Op::kSubChain:
            return;
        default:
            for (const BoolPredicate& child : pred.children) ForEachColumn(child, fn);
            return;
    }
}

// Every function term a boolean conjunct calls, for the determinism class.
template <typename Fn>
void ForEachFunction(const BoolPredicate& pred, Fn&& fn) {
    if (pred.op == BoolPredicate::Op::kFunction) {
        fn(*pred.fn);
        return;
    }
    for (const BoolPredicate& child : pred.children) ForEachFunction(child, fn);
}

struct Step;

// A predicate-position subquery, lowered (V15). It is a chain in its own
// right, plus how its rows turn into a boolean or a value.
//
// The two placements, and the difference is a performance property with a
// correctness consequence:
//
//   hoisted     no reference escapes to an enclosing chain (`up == 0`
//               everywhere), so the answer cannot vary per outer row.
//               Executed **once**, before the outer chain opens.
//   nested      some reference has `up > 0`. Executed once per outer row,
//               with the correlation values read through the frame stack.
//
// Classification is structural - "does any reference point outward?" - not
// a heuristic, so it is stable across executions and therefore safe to
// bake into a chain a trail is recorded against.
struct SubChain {
    // Which predicate this lowers. kCompareValue never appears here.
    parser::PredicateKind kind = parser::PredicateKind::kExists;

    // The nested chain's own steps, sharing the statement's global
    // step_id counter.
    std::vector<Step> steps;

    // The uncorrelated EXISTS / NOT EXISTS the nested block hoisted out of
    // its own row loop, which the top-level `StepChain::hoisted` is for the
    // outermost block. Each runs once per evaluation of this sub-chain,
    // before `steps` opens, and one that is not true leaves this sub-chain
    // with no rows.
    std::vector<SubChain> hoisted;

    // False when every reference inside resolves within the sub-chain.
    bool correlated = false;

    // For IN / NOT IN and scalar comparison: the outer column being
    // tested, and the operator for the scalar form. Unused for
    // EXISTS / NOT EXISTS, which test only whether a row appeared.
    ColumnRef lhs;
    parser::CompareOp op = parser::CompareOp::kEq;

    // The column of the sub-chain the value is taken from - the single
    // projected column for IN and the scalar form. EXISTS projects
    // nothing, since only the existence of a row matters.
    ColumnRef value;
    bool has_value = false;

    // A leaf of a boolean conjunct on the same step (BU-S3): not a conjunct
    // of its own, so the step's conjunct loops pass it by and the tree that
    // names it evaluates it. Never hoisted, correlated or not - a false
    // `EXISTS` under an `OR` must not empty the statement.
    bool in_tree = false;

    // `UPDATE ... FROM`'s EXISTS (BU-Q16): reads up to two rows, and a
    // second is `CardinalityViolation`. Never hoisted.
    bool at_most_one = false;
};

struct Step {
    // Global across the whole statement, in compile order - the outer
    // chain and every sub-chain share one counter, so a trail entry's
    // step_id is unambiguous without parent linkage.
    std::uint32_t step_id = 0;

    catalog::Oid rel_oid = 0;

    // The relation's name, for display only.
    //
    // Filled at compile time for the same reason `column_names` is: a
    // compiled chain otherwise carries no identifiers at all (spec I11 -
    // "no identifier survives onto an execute path"), and a plan a person
    // reads has to say which relation a step reads. **Nothing on an
    // execute path may read this**, and nothing does - resolving a name
    // during execution is exactly what the rule forbids, so it is
    // resolved once, here, where the catalog lookup already happened.
    std::string rel_name;

    AccessKind kind = AccessKind::kScan;

    // The tuple key, for kLookup (a literal) and kProbe (a column produced
    // by an earlier step). Empty for every other kind - a correlated
    // kIndexProbe's key source lives in `IndexProbe::key_from`, its single
    // authority, which the plan printer renders directly.
    std::optional<Operand> key;

    // The tuple key bounds, for kRange. Empty for every other kind.
    std::optional<RangeBounds> range;

    // The Cabin and the value, for kCabinProbe. Empty for every other kind.
    std::optional<CabinProbe> cabin;

    // The index and its bounds, for kIndexProbe and kIndexRange. Empty for
    // every other kind.
    std::optional<IndexProbe> index;

    // The columns this step's kind was assigned for, in schema order:
    // the filtered columns for kFilterScan, the tuple key for kLookup/kProbe/
    // kRange, empty for a bare kScan.
    //
    // Computed once here rather than re-derived per execution, because it
    // is what the access statistics key on - and re-deriving it would mean
    // walking the residual on every statement to answer a question the
    // compiler already answered.
    std::vector<std::uint16_t> access_columns;

    // Every conjunct that becomes evaluable at this step - that is, whose
    // references are all satisfied by this step or an earlier one. A
    // conjunct is attached to the *latest* step it references, which is
    // the earliest point it can be evaluated. Deterministic, and not an
    // optimizer choice.
    //
    // **The key is repeated here.** A kLookup's tuple key equality is already
    // enforced by the descent, so re-checking it costs one comparison on
    // exactly one row. It is kept because it makes an important property
    // structural rather than argued: since the residual list alone fully
    // expresses the statement's predicate, downgrading any kProbe or
    // kLookup to a kScan cannot change the result. That is what "the
    // probe strategy and the scan strategy agree row-for-row" means, and
    // it is also what makes invariant 9's fall-through safe - a trail
    // miss falls back to a walk that filters on exactly the same list.
    std::vector<StepPredicate> residual;

    // Correlated sub-chains that become evaluable at this step, in written
    // order. Placed by the same rule as `residual`: the latest step any of
    // their outward references reaches.
    std::vector<SubChain> sub_chains;

    // ---- Which of *this* relation's columns the residual reads ----------
    //
    // A bit per `col_pos`, for the columns `residual` references on this
    // step's own relation. It exists so a scan can decide whether a row
    // qualifies **before** building the whole row.
    //
    // That ordering is the difference between reading a row and
    // materialising one: a selective filter otherwise decodes every column
    // of every rejected row (background: measured pre-v3,
    // `bench/results-scenario1-vs-pg.md` at `1769487`).
    //
    // **`kAllColumns` means "decode everything"** - set when a referenced
    // column sits past bit 63, and the value a zero-initialised Step would
    // *not* carry, so a step built without the compiler decodes fully and is
    // merely slow rather than wrong.
    static constexpr std::uint64_t kAllColumns = ~std::uint64_t{0};
    std::uint64_t filter_columns = kAllColumns;

    // ---- Which of this relation's columns anything reads at all ---------
    //
    // The superset of `filter_columns`: every column of this step's relation
    // that *any* consumer of the row touches - a later step's join predicate
    // or probe key, the projection, the fold's items and group keys, the
    // trail's tuple key. A step decodes `filter_columns` before the residual and
    // then only `read_columns & ~filter_columns` after it, instead of
    // everything else (workplan AP01). `filter_columns` alone narrows only a
    // rejected row's decode; this narrows a surviving one's, so `SELECT
    // COUNT(*) FROM t` decodes no column (`bench/results-aggregate.md` at
    // `1769487`).
    //
    // **The rule that keeps it correct: this must be a superset of every
    // reader.** A slot outside it still holds the *previous* row's value,
    // so a missing column is a silently wrong answer rather than a crash.
    // Two consequences the compiler enforces: a chain containing any
    // sub-chain answers `kAllColumns` everywhere, since a correlated
    // reference reaches outward into any earlier step's row; and `SELECT *`
    // answers `kAllColumns` for the step it projects.
    //
    // **It is deliberately not part of AG1's chain-identity contract.** Spec
    // §9.1 fixes that on "steps, kinds, residuals, class", and a decode mask
    // is none of the four - it describes what a row is read *for*, not how
    // it is found. An aggregated statement and its unaggregated twin do
    // differ here, and must: `SELECT COUNT(*)` reads no column where
    // `SELECT qty` reads one.
    //
    // `kAllColumns` is the zero-initialised default's opposite on purpose,
    // the same way `filter_columns` has it: a Step built by anything other
    // than the compiler decodes everything and is merely slow.
    std::uint64_t read_columns = kAllColumns;

    // The walked-join annotation. Unlike the per-kind fields above it
    // marks no kind - the step stays kScan. `BuildKey` owns the contract.
    //
    // **Deliberately last**: cold compiled state sits below the hot
    // execution fields. Inserted mid-struct it moved `residual` onto another
    // cache line, a ~1.7% wall regression on correlated-inner shapes (JB1,
    // fc44ac6 vs 080f73a, 2026-08).
    std::optional<BuildKey> build;

    // The function conjuncts placed at this step (see FunctionPredicate).
    // Below the hot fields with `build`, for `build`'s reason: most steps
    // carry none, and the evaluation sites test for empty first.
    std::vector<FunctionPredicate> fn_residual;

    // The boolean conjuncts placed at this step (see BoolPredicate), for
    // `fn_residual`'s reasons and evaluated after it.
    std::vector<BoolPredicate> bool_residual;
};

// Execution shape, dispatched on by a `switch` - there is no plan
// enumeration anywhere in this engine.
//
// `[PROPOSED]`, per docs/spec/parser-v2.md §3 and CLAUDE.md's open list: the
// class list is not ratified. What v2 settles is that **every step-chain
// statement is kJoinSelect**, read as "step-chain select" - the concept
// generalized from "join chain" to "step chain" and the enum did not grow
// (J3). Single-relation point and range forms keep their own classes.
enum class StatementClass : std::uint8_t {
    kPointSelect,
    kRangeSelect,
    kJoinSelect,
    kUnclassified,
};

// The value `SysPatternRow::stmt_class` stores for a class.
//
// **Not a cast.** That row reserves 0 for "unclassified" (catalog/rows.hpp)
// and `kPointSelect` is also 0, so a straight cast writes every point-lookup
// pattern to disk as unclassified - a collision that reads as a mystery
// later. Mapping it explicitly also means this enum can be reordered without
// silently changing what stored rows mean.
std::uint8_t StoredStatementClass(StatementClass klass) noexcept;

// ---- Aggregation (docs/spec/aggregate.md §4) -----------------------------
//
// **A property of the chain, never a step in it.** AG1 puts the fold
// outside the executor: the dispatcher wraps its row sink, and the steps
// compiled for `SELECT b, COUNT(*) FROM t GROUP BY b` are byte-identical to
// those compiled for `SELECT b FROM t`. That identity is the whole reason
// this feature needed no new access kind, no change to `IsTrailReplayable`,
// and no second proof of anything already proved about a chain - trail
// replay, Cabin probes, the scan/probe equivalence, "downgrading any step to
// a scan cannot change the result". A spec that lived on a `Step` would have
// forfeited every one of those.

// One entry of the fold's output row, in written order.
struct AggregateItem {
    // False for a grouping column carried through to the output, which
    // AG5 requires to appear in `group_keys` as well.
    bool is_aggregate = false;

    parser::AggFunc func = parser::AggFunc::kCount;

    // `COUNT(*)` - the one form with no column to read, and the only one
    // whose answer does not depend on a value being non-NULL.
    bool star_arg = false;

    // The word as written. `MIN`/`MAX` carry it and ignore it (spec §3.2
    // `[PROPOSED]`), since an extreme of a set equals the extreme of its
    // support.
    bool distinct = false;

    // The column read, resolved. Unset when `star_arg`.
    ColumnRef ref;

    // The referenced column's catalog `type_val`, carried so the fold can
    // compare through `CompareValues` without asking the catalog per row -
    // and so a `uint64` above `INT64_MAX` compares through its digit text
    // rather than through a signed reading that cannot hold it.
    std::uint32_t type_val = 0;

    // A decimal column's packed `(p, s)` (`catalog::PackDecimalLen`), resolved
    // at compile for the same reason `type_val` is (types.md §3.2). `SUM`
    // folds unscaled int64 and the answer's scale is the column's, so
    // `Finish` needs the scale and must not have to ask the catalog for it -
    // the fold sits outside the executor and has no catalog to ask. The
    // precision is the output description's: `MIN`/`MAX` and a grouping
    // column answer a member of the column, so they are described in its own
    // `(p, s)`. Zero for every other type.
    std::uint32_t decimal_len = 0;
};

struct AggregateSpec {
    // The output row, in written order. Empty is impossible: a statement
    // with no items is not aggregated.
    std::vector<AggregateItem> items;

    // The GROUP BY list, resolved, in written order. **Empty means the
    // global form** - one output row, even over empty input - which is a
    // different shape rather than a degenerate one, and the fold keeps no
    // map for it.
    std::vector<ColumnRef> group_keys;
};

// ---- Sorting (docs/workplan-order-by.md OB3) -----------------------------

// One resolved `ORDER BY` key. Written order is significant: the first key
// decides and later keys break its ties.
struct SortKey {
    // Which column, structurally. Any relation in the top-level scope may
    // be named - the frame holds every step's values by the time the sink
    // sees a row, so ordering by a joined relation's column costs the same
    // as ordering by the driving one's.
    ColumnRef ref;

    // The catalog `type_val`, resolved here for the reason
    // `projection_types` is: the comparator has to know a uint64 from an
    // int64 to order the upper half of the range correctly, and asking the
    // catalog once per key per *comparison* is the per-row cost the frame
    // design exists to avoid.
    std::uint32_t type_val = 0;

    bool descending = false;
};

// What the join planner says of a searched block (CC-S3): the rows it
// estimated each executed slot passes on, the statistics generation the
// estimates read (0: none published), and the chosen order's cost against
// the written order's (1 when the written order runs).
struct PlanNote {
    std::vector<double> estimated_rows;
    std::uint64_t generation = 0;
    double cost_ratio = 1.0;
    // Whether the order came from the core's memo rather than a search (CC-S4).
    bool from_memo = false;
};

// The relations a block the join planner searches may hold and still have
// its relations named for priors (`StepChain::planned_relations`).
inline constexpr std::size_t kMaxPlannedRelations = 16;

struct StepChain {
    StatementClass klass = StatementClass::kUnclassified;

    // The most volatile purity among the functions this statement's
    // predicates call, at any depth (AR1 §3; exec/functions.hpp). D2 is
    // neither recorded as a trail nor read from one; D1 is D0 until AQ folds
    // its values into the instance key (AP-Q5).
    DeterminismClass determinism = DeterminismClass::kD0;

    // Uncorrelated sub-chains, executed once each before `steps` opens.
    // Hoisting is not an optimizer rewrite - an uncorrelated subquery's
    // answer is by definition the same for every outer row, so running it
    // per row would compute one value n times. Kept separate from `steps`
    // because a false uncorrelated EXISTS answers the whole statement
    // without opening the outer relation at all.
    std::vector<SubChain> hoisted;

    // In execution order: the join planner's (CB-S2), or the written order
    // - steps[0] the FROM relation, steps[i] the i-th JOIN - where it kept
    // it. A step's `step_id` is its relation's written position either way.
    std::vector<Step> steps;

    // The select list, resolved. Empty means `SELECT *`, which the
    // grammar only admits for a single relation (V06) and which therefore
    // means "every column of steps[0]".
    std::vector<ColumnRef> projection;

    // Column headings for the projection, in the same order. The one
    // place names survive compilation, because a result set has to label
    // its columns - they are never read on an execute path.
    std::vector<std::string> column_names;

    // The catalog `type_val` of each projected column, in the same order,
    // resolved at compile for the same reason `column_names` is: the
    // emission boundary renders a `DATE` as a date rather than an epoch
    // day (docs/spec/types.md §3.3), and asking the catalog for the type
    // once per column per *row* is exactly the per-row cost decode was
    // kept free of. Empty for `SELECT *`, which renders from the schema
    // the dispatcher already resolves. Never read on an execute path.
    std::vector<std::uint32_t> projection_types;

    // The same columns' catalog `len` word, resolved by the same lookup at
    // the same moment - a decimal's packed `(p, s)`, zero for every other
    // type (`catalog::PackDecimalLen`).
    //
    // It exists because a *typed* emission boundary needs it and the text
    // one did not: `FormatDecimal` reads the scale off the value, so the
    // newline protocol never had to know a column's, but KWP's
    // `S_ROW_DESC` states the scale **once per result set**
    // (protocol.md §6) - including for a result set with no rows, where
    // there is no value to read it from. Carried rather than looked up for
    // `projection_types`'s reason, and beside it rather than folded into
    // it because they are two facts about one column and every existing
    // reader of `projection_types` wants only the first.
    std::vector<std::uint32_t> projection_type_mods;

    // The fold this chain's rows are consumed by, or nothing (AG1). Set
    // for exactly the statements `parser::SelectStmt::aggregated()` is true
    // for, and read only by the dispatcher - no execute path looks at it.
    std::optional<AggregateSpec> aggregate;

    bool aggregated() const noexcept { return aggregate.has_value(); }

    // Whether the statement was written `SELECT *`.
    //
    // **Projection empty *and* no aggregate.** An aggregated chain also
    // leaves `projection` empty - the fold's items are what it emits - so
    // testing `projection` alone would answer it with whole rows; the same
    // trap `parser::SelectStmt::star()` avoids one layer up.
    bool star() const noexcept { return projection.empty() && !aggregated(); }

    // ---- The pagination tail (docs/spec/parser-v2.md I11, workplan V09) ------
    //
    // `LIMIT n` / `OFFSET m`, copied from the AST and read only by the
    // dispatcher's emission quota - no execute path looks at either,
    // exactly as `aggregate` above, and for AG1's reason: the quota is a
    // sink decorator, so the steps, kinds and residuals of a limited
    // statement are its unlimited twin's, bit for bit.
    std::optional<std::uint64_t> limit;
    std::uint64_t offset = 0;

    // The `ORDER BY` keys, in written order (docs/workplan-order-by.md OB3).
    //
    // **Empty means "emit in chain order"**, which covers two different
    // statements: one that wrote no `ORDER BY`, and one whose `ORDER BY` the
    // compiler *elided* because the chain already emits that order. The
    // elision is why `ORDER BY <tuple key> ASC` still costs nothing - it compiles
    // to no sort, not to a sort that happens to find its input ordered.
    //
    // Read by the dispatcher only, like `limit`, `offset` and `aggregate`;
    // no execute path looks at it. A sort does reach into the chain for one
    // thing, `Step::read_columns` - a key must be decoded to be compared -
    // and that is the single respect in which a sorted chain differs from
    // its unsorted twin.
    std::vector<SortKey> sort_keys;

    bool sorted() const noexcept { return !sort_keys.empty(); }

    // Set on a block the join planner searched (CC-S2): its key, which the
    // dispatcher folds the run's counts under (docs/spec/plan-stats.md PS2).
    // Read by the dispatcher only, like `limit` and `aggregate`; never by an
    // execute path, and not part of a chain's identity (AG1).
    std::optional<stats::PlanKey> planned;
    // Every searched block's relations in written order, keyed or not - the
    // steward seeds a prior for each the snapshot lacks (plan-stats.md PS2).
    std::vector<catalog::Oid> planned_relations;
    // What ANALYZE prints of a searched block's choice (CC-S3); set with
    // `planned_relations`, read by the plan printer alone.
    std::optional<PlanNote> plan_note;
};

// **The one walk of a step tree.** Every step under a chain, sub-chain or
// step, in one order: a chain's hoisted sub-chains, then its steps, and each
// step before the sub-chains it carries. `fn(const Step&)` answers whether to
// go on, and the walk answers whether it finished. Every walk that needs
// neither depth nor structure is written over this, so a field added to the
// tree is followed by all of them or none.
namespace detail {
template <typename Fn>
bool VisitSteps(const std::vector<Step>& steps, Fn& fn);

template <typename Fn>
bool VisitSubChains(const std::vector<SubChain>& subs, Fn& fn) {
    for (const SubChain& sub : subs) {
        if (!VisitSubChains(sub.hoisted, fn) || !VisitSteps(sub.steps, fn)) return false;
    }
    return true;
}

template <typename Fn>
bool VisitSteps(const std::vector<Step>& steps, Fn& fn) {
    for (const Step& step : steps) {
        if (!fn(step) || !VisitSubChains(step.sub_chains, fn)) return false;
    }
    return true;
}
}  // namespace detail

template <typename Fn>
bool ForEachStep(const StepChain& chain, Fn fn) {
    return detail::VisitSubChains(chain.hoisted, fn) && detail::VisitSteps(chain.steps, fn);
}

template <typename Fn>
bool ForEachStep(const Step& step, Fn fn) {
    return fn(step) && detail::VisitSubChains(step.sub_chains, fn);
}

// Whether any step anywhere in `chain` - sub-chains included - is one a
// Waystone trail may replace.
//
// The dispatcher skips the whole Waystone path when it is false: a chain
// with no keyed step can neither record nor replay, and asking costs a
// fingerprint, the most expensive thing on that path.
bool HasReplayableStep(const StepChain& chain) noexcept;

// The value `SysAccessStatRow::kind` stores for an access kind.
//
// **Not a cast**, for `StoredStatementClass`'s reason: `kLookup` is 0 and so is a zeroed catalog row, so a straight cast
// would make every never-written row read as a recorded tuple key lookup. Mapping
// it explicitly also means this enum can gain a value or be reordered
// without silently changing what stored statistics mean.
std::uint8_t StoredAccessKind(AccessKind kind) noexcept;

// The reverse, for rendering a stored row. Returns nullopt for a value no
// build of this engine ever wrote - which a zeroed row decodes to.
std::optional<AccessKind> AccessKindOfStored(std::uint8_t stored) noexcept;

}  // namespace kds::exec
