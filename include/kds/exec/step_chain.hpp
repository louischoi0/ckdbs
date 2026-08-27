#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/oid.hpp"
#include "kds/parser/ast.hpp"

// The compiled form of a SELECT-class statement: an ordered list of steps,
// each reading one relation with one access kind (docs/spec/parser-v2.md §1).
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
// (docs/spec/waystone-concpets.md §2), deliberately one decision with two
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
// Written in the order of the table in docs/spec/parser-v2.md §1.
enum class AccessKind : std::uint8_t {
    // pk equality against a value known before the chain runs.
    kLookup,
    // pk equality against a value produced by an earlier step (or, once
    // sub-chains exist, by an outer row).
    kProbe,
    // pk range through the leaf chain, from `BETWEEN <low> AND <high>` on
    // the relation's primary key. Emitted since the BETWEEN half of V08.
    kRange,
    // An equality against a literal on a non-pk column that carries a
    // **Cabin** (docs/spec/cabin.md): a probe of the Cabin's observed set,
    // falling back to the walk below when the value has not been observed.
    //
    // The third trust class, and the only kind here that is neither a pk
    // descent nor a walk. A Cabin is **authoritative for observed values**:
    // its entry set for an observed value is a superset of the qualifying
    // pks, and the read subtracts the surplus by re-checking visibility and
    // the key equality. So an observed value's *empty* set is an
    // authoritative "no rows" - which nothing else in this enum can say
    // about a non-pk predicate.
    //
    // Whether a column has a Cabin is **catalog** state, so the plan stays
    // `f(shape, catalog)` and nothing about the data influences it. Whether
    // a *value* has been observed is runtime state, and it steers only the
    // branch taken inside this kind, never the kind itself.
    kCabinProbe,
    // An equality on a **prefix of a secondary index's key**
    // (docs/spec/index.md §8): descend the index, walk its entries while the
    // prefix matches, resolve each pk through the clustered tree.
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
    // literal on a non-pk column with no index.
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
    std::uint16_t col_pos = 0;

    // The value being probed for, when the equality binds a literal.
    // Unused when `key_from` below is set.
    parser::AstValue value;

    // The correlated form (docs/spec/cabin.md §4a): the probed value is an
    // *earlier step's* or an enclosing chain's column - a join key - read
    // from the frame per outer row. The outer row is fixed for the whole
    // of this step's execution, so the value is stable exactly as long as
    // a probe or a recording needs it. This is the shape v1 deliberately
    // did not emit, and the one structure a heap relation's join column
    // can carry at all - IX3 refuses it an index.
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
// pks (docs/spec/index.md §1), and the surplus is subtracted by re-checking
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

    // The inclusive sort-key bounds the walk runs between, both a full
    // `key_width + 8` bytes.
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

// The walked-join annotation (docs/spec/join-inner-build.md §5, workplan
// JB1): the statement-local inner build's compile half.
//
// **An annotation on a kScan, never a kind.** Set when every structure arm
// declined and the step's residual still binds an own column by equality to
// an earlier step's or an enclosing chain's column - the walked-join shape,
// the one the ladder had no answer for. The step stays kScan, so trails,
// access statistics, `ShippedForm` and every kind-switch downstream are
// untouched by construction; the descriptor codec never encodes this field,
// so a shipped step never carries it. An executor that ignores it wholesale
// answers identically: the correlated conjunct stays in `Step::residual`,
// which is the same downgrade-safety every other access hint has.
//
// What makes one build serve every outer row: `PkBound` accepts literals
// only, so no scan bound varies per outer row - a kScan inner is always the
// full relation, and one walk's map answers every later key (spec §5).
// Consumed by the executor from JB3 on; until then it is compiled state
// with no reader.
struct BuildKey {
    // The own join column's schema position - what the map is keyed on.
    std::uint16_t col_pos = 0;

    // The outer source, read from the frame per outer row: an earlier
    // step's or an enclosing chain's column - the same `key_from` shape
    // both correlated probes carry.
    ColumnRef key_from;

    // Where in `Step::residual` the correlated conjunct sits, so the build
    // can bucket rows on every *other* conjunct (the non-correlated
    // residual) and the probe can re-evaluate the full list (JB3/JB4).
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
    // ignores it; the two consumers are diagnostic truthfulness: ANALYZE
    // marks the line, and CREATE PATTERN's parameter checks skip it - a
    // warning must name a predicate the client can find in their text.
    // Deliberately not serialized by step_descriptor.cpp: a shipped chain's
    // peer only evaluates residuals, and false is the safe default.
    bool derived = false;
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
    // by an earlier step). Empty for every other kind - a correlated
    // kIndexProbe's key source lives in `IndexProbe::key_from`, its single
    // authority, which the plan printer renders directly.
    std::optional<Operand> key;

    // The pk bounds, for kRange. Empty for every other kind.
    std::optional<RangeBounds> range;

    // The Cabin and the value, for kCabinProbe. Empty for every other kind.
    std::optional<CabinProbe> cabin;

    // The index and its bounds, for kIndexProbe and kIndexRange. Empty for
    // every other kind.
    std::optional<IndexProbe> index;

    // Emit each page's rows in pk order rather than in slot order.
    //
    // A walk hands the executor a page's slots in slot order, which *is* pk
    // order whenever ids were issued monotonically: each new id is appended
    // above every id already on the page. A caller-supplied id admitted below
    // the relation's high-water mark (docs/spec/heap-and-tuple.md §4.1) can be
    // appended below them, so once a relation has taken one the two orders
    // diverge - within one page only, since pages stay key-ordered by
    // `min_key` either way.
    //
    // Set only where both halves are true: the statement asked for pk order,
    // and the relation is one whose slots can be out of it. Everything else
    // keeps the walk untouched, because the sort is not free - it reads every
    // live slot's Keystone word up front, where the natural walk reads one
    // per emitted row.
    bool emit_in_key_order = false;

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

    // ---- Which of *this* relation's columns the residual reads ----------
    //
    // A bit per `col_pos`, for the columns `residual` references on this
    // step's own relation. It exists so a scan can decide whether a row
    // qualifies **before** building the whole row.
    //
    // That ordering is the difference between reading a row and materialising
    // one. A `WHERE session_no = <n>` over 60,480 rows returning 8 used to
    // decode twelve values per row and throw away 60,472 of them; the decode
    // was 75% of the scan and 96% of the decode was building `AstValue`s
    // rather than reading cells (`bench/results-scenario1-vs-pg.md`).
    //
    // Computed here rather than per row for the obvious reason, and it is
    // exactly the kind of thing the compiler exists to answer once.
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
    // trail's pk. A step decodes `filter_columns` before the residual and
    // then only `read_columns & ~filter_columns` after it, instead of
    // everything else (workplan AP01).
    //
    // **Why this is not a micro-optimization.** Without it, `filter_columns`
    // narrows the decode of a row that is about to be *rejected* and does
    // nothing for one that survives - so a statement with no WHERE at all
    // gets no benefit, because every row survives. `SELECT COUNT(*) FROM t`
    // built an AstValue for every column of every row to fold none of them,
    // which measured at 2.7x the cost of the same statement on a two-column
    // relation, and made `SELECT COUNT(*) FROM t WHERE a = 1` **3.1x faster
    // than dropping the WHERE** (`bench/results-aggregate.md`).
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

    // The walked-join annotation (workplan JB1). Unlike the per-kind
    // fields above it marks no kind - the step stays kScan. `BuildKey`
    // owns the whole contract.
    //
    // **Deliberately last.** Inserting this field mid-struct measured a
    // real ~1.7% wall regression on the correlated-inner shapes by moving
    // `residual` onto a different cache line (the JB1 A/B round, fc44ac6
    // vs 080f73a) - the arm's own cost was unmeasurable. Cold compiled
    // state sits below the hot execution fields; JB3 adds its reader here,
    // not above.
    std::optional<BuildKey> build;
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

    // A `DECIMAL` column's scale, resolved at compile for the same reason
    // `type_val` is (types.md §3.2). `SUM` folds unscaled int64 and
    // the answer's scale is the column's, so `Finish` needs the number and
    // must not have to ask the catalog for it - the fold sits outside the
    // executor and has no catalog to ask. Zero for every other type.
    std::uint8_t scale = 0;
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

    // The catalog `type_val` of each projected column, in the same order,
    // resolved at compile for the same reason `column_names` is: the
    // emission boundary renders a `DATE` as a date rather than an epoch
    // day (docs/spec/types.md §3.3), and asking the catalog for the type
    // once per column per *row* is exactly the per-row cost decode was
    // kept free of. Empty for `SELECT *`, which renders from the schema
    // the dispatcher already resolves. Never read on an execute path.
    std::vector<std::uint32_t> projection_types;

    // The fold this chain's rows are consumed by, or nothing (AG1). Set
    // for exactly the statements `parser::SelectStmt::aggregated()` is true
    // for, and read only by the dispatcher - no execute path looks at it.
    std::optional<AggregateSpec> aggregate;

    bool aggregated() const noexcept { return aggregate.has_value(); }

    // Whether the statement was written `SELECT *`.
    //
    // **Projection empty *and* no aggregate.** An aggregated chain names its
    // output and still leaves `projection` empty - the fold's items are what
    // it emits, not a list of chain columns - so the old one-field test would
    // have the dispatcher answer an aggregated statement with whole rows.
    // The same trap `parser::SelectStmt::star()` has to avoid, one layer up.
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
    // elision is why `ORDER BY <pk> ASC` still costs nothing - it compiles
    // to no sort, not to a sort that happens to find its input ordered.
    //
    // Read by the dispatcher only, like `limit`, `offset` and `aggregate`;
    // no execute path looks at it. A sort does reach into the chain for one
    // thing, `Step::read_columns` - a key must be decoded to be compared -
    // and that is the single respect in which a sorted chain differs from
    // its unsorted twin.
    std::vector<SortKey> sort_keys;

    bool sorted() const noexcept { return !sort_keys.empty(); }
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
