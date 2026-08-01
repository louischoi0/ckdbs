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
    // pk range through the leaf chain. Reserved: no production emits one
    // until `ORDER BY`/`LIMIT` (V09) and range predicates arrive.
    kRange,
    // Everything else: walk the relation and filter.
    kScan,
};

// True for the kinds a Waystone trail may replace outright. The one place
// this line is drawn, so the executor and the recorder cannot disagree
// about it.
constexpr bool IsTrailReplayable(AccessKind kind) noexcept {
    return kind == AccessKind::kLookup || kind == AccessKind::kProbe;
}

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
    AccessKind kind = AccessKind::kScan;

    // The pk key, for kLookup (a literal) and kProbe (a column produced
    // by an earlier step). Empty for kScan/kRange.
    std::optional<Operand> key;

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

}  // namespace kds::exec
