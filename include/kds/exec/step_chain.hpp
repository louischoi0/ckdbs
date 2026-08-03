#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/oid.hpp"
#include "kds/parser/ast.hpp"

// The compiled form of a SELECT-class statement: an ordered list of steps,
// each reading one relation with one access kind (docs/parser-v2.md §1).
//
// ---- Why a chain, and why written order --------------------------------
//
// Written order is execution order. That is a **client contract**, not an
// implementation detail: the statement is the chain, nothing reorders it,
// and no decorrelation rewrite exists. A client that wants a different
// join order writes a different statement. This costs the optimizations a
// cost-based planner would find, and buys the thing this engine is for -
// a repeated query pattern executes the same way every time, which is what
// makes a recorded Waystone trail replayable at all.
//
// ---- The step-kind table IS the trust model ----------------------------
//
// `AccessKind` is not an executor implementation note. It is simultaneously
// the executor's probe strategy and Waystone's lookup/search line
// (docs/waystone-concpets.md §2), deliberately one decision with two
// consumers rather than two that can drift apart:
//
//   kLookup / kProbe   pk-equality descent. **Trail-replayable** -
//                      completeness follows from pk uniqueness, so a
//                      recorded location can replace the descent.
//   kScan / kRange     a search. **Never replayable.** A stored set that
//                      is missing a row inserted since it was recorded is
//                      wrong in a way no per-tuple validation can detect,
//                      because absence has no witness (invariant 9). A
//                      trail may prefetch for these and nothing more.
//
// A step is kLookup/kProbe **iff** its equality binds the relation's first
// schema column - the Keystone pk, the only column a lookup can address
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

// A resolved reference to one column of one relation.
//
// `up` is a de Bruijn level: 0 is the chain this reference appears in, 1
// its parent, and so on. It maps one-to-one onto the execute-time frame
// stack, which is what lets a predicate be independent of which sub-chain
// it landed in. Only 0 occurs until sub-chains land (V15).
struct ColumnRef {
    std::uint16_t up = 0;
    std::uint16_t rel_slot = 0;  // step index within that chain
    std::uint16_t col_pos = 0;   // index into that relation's schema columns

    bool operator==(const ColumnRef&) const = default;
};

// Whether a step reads its relation by pk descent or by walking it.
// Written in the order of the table in docs/parser-v2.md §1.
enum class AccessKind : std::uint8_t {
    // pk equality against a value known before the chain runs.
    kLookup,
    // pk equality against a value produced by an earlier step (or, once
    // sub-chains exist, by an outer row).
    kProbe,
    // pk range through the leaf chain, from `BETWEEN <low> AND <high>` on
    // the relation's primary key. Emitted since the BETWEEN half of V08.
    kRange,
    // A walk **driven by a filter**: at least one equality against a
    // literal on a non-pk column with no index.
    //
    // Split out of kScan because the two are identical in cost today and
    // completely different as a signal. A `kScan` is a statement that
    // asked for everything; a `kFilterScan` is a statement that asked for
    // a few rows and had to read all of them to find out which - which is
    // exactly the shape a physical optimizer wants to hear about
    // (`docs/heap-and-tuple.md` §7), and which an index or a clustering
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
constexpr bool IsTrailReplayable(AccessKind kind) noexcept {
    return kind == AccessKind::kLookup || kind == AccessKind::kProbe;
}

// The inclusive pk bounds a kRange step walks between.
//
// A **hint on top of the residual**, never a replacement for it: the two
// conjuncts a `BETWEEN` lowers to (`>= low`, `<= high`) stay in
// `Step::residual` exactly as a lookup's equality does. That is what keeps
// "downgrading any step to a plain kScan cannot change the result" true,
// and it is the property invariant 9's fall-through and the
// scan/probe equivalence tests both rest on.
struct RangeBounds {
    std::uint64_t low = 0;
    std::uint64_t high = 0;
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
};

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

    // The pk key, for kLookup (a literal) and kProbe (a column produced
    // by an earlier step). Empty for every other kind.
    std::optional<Operand> key;

    // The pk bounds, for kRange. Empty for every other kind.
    std::optional<RangeBounds> range;

    // The columns this step's kind was assigned for, in schema order:
    // the filtered columns for kFilterScan, the pk for kLookup/kProbe/
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
    // **The key is repeated here.** A kLookup's pk equality is already
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
};

// Execution shape, dispatched on by a `switch` - there is no plan
// enumeration anywhere in this engine.
//
// `[PROPOSED]`, per docs/parser-v2.md §3 and CLAUDE.md's open list: the
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

struct StepChain {
    StatementClass klass = StatementClass::kUnclassified;

    // Uncorrelated sub-chains, executed once each before `steps` opens.
    // Hoisting is not an optimizer rewrite - an uncorrelated subquery's
    // answer is by definition the same for every outer row, so running it
    // per row would compute one value n times. Kept separate from `steps`
    // because a false uncorrelated EXISTS answers the whole statement
    // without opening the outer relation at all.
    std::vector<SubChain> hoisted;

    // In written order. steps[0] is the FROM relation, steps[i] the i-th
    // JOIN. Never sorted, never reordered.
    std::vector<Step> steps;

    // The select list, resolved. Empty means `SELECT *`, which the
    // grammar only admits for a single relation (V06) and which therefore
    // means "every column of steps[0]".
    std::vector<ColumnRef> projection;

    // Column headings for the projection, in the same order. The one
    // place names survive compilation, because a result set has to label
    // its columns - they are never read on an execute path.
    std::vector<std::string> column_names;

    bool star() const noexcept { return projection.empty(); }
};
// Whether any step anywhere in `chain` - sub-chains included - is one a
// Waystone trail may replace.
//
// Two callers, asking the same question for opposite reasons. `CREATE
// PATTERN` warns when the answer is false, because such a pattern's trail
// could never replay. The dispatcher skips the whole Waystone path when it
// is false, because a chain with no keyed step can neither record nor
// replay - and asking costs a fingerprint, which is the most expensive
// thing on that path.
bool HasReplayableStep(const StepChain& chain) noexcept;

// The value `SysAccessStatRow::kind` stores for an access kind.
//
// **Not a cast**, for the reason `StoredStatementClass` already had to
// learn: `kLookup` is 0 and so is a zeroed catalog row, so a straight cast
// would make every never-written row read as a recorded pk lookup. Mapping
// it explicitly also means this enum can gain a value or be reordered
// without silently changing what stored statistics mean.
std::uint8_t StoredAccessKind(AccessKind kind) noexcept;

// The reverse, for rendering a stored row. Returns nullopt for a value no
// build of this engine ever wrote - which a zeroed row decodes to.
std::optional<AccessKind> AccessKindOfStored(std::uint8_t stored) noexcept;

}  // namespace kds::exec
