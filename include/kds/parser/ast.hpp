#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "kds/catalog/well_known.hpp"

// AST for the KDS SQL subset. The whole grammar:
//
//   CREATE TABLE <name> (<col> <type> [, ...])
//       [HEAP | BTREE] [EXPLICIT];   -- either order, each once; EXPLICIT is vacuous
//   INSERT INTO  <name> VALUES (<val> [, ...]);
//   SELECT <list> FROM  <rel> [<join>]* [WHERE <cond> [AND <cond>]*]
//       [GROUP BY <col> [, ...]] [HAVING <hcond> [AND <hcond>]*]
//       [ORDER BY <key> [ASC]] [LIMIT <int>] [OFFSET <int>];
//   UPDATE <name> SET <col> = <val> [, ...] [WHERE <cond> [AND <cond>]*];
//   CREATE PATTERN <name> ($p <type> [, ...]) [WITH (<k> = <v>, ...)]
//       OF <select>;
//   DROP PATTERN <name>;
//
//   <rel>   ::= <name> [AS <alias>]
//   <join>  ::= JOIN <rel> ON <qcol> = <qcol>
//   <qcol>  ::= <relation-binding> . <col>
//   <cond>  ::= <col> <op> <val>
//   <op>    ::= = | != | < | <= | > | >=
//   <val>   ::= integer literal | 'string literal' | NULL
//   <list>  ::= * | <item> [, <item>]*
//   <item>  ::= <col> | <agg> ( [DISTINCT] <col> | * )
//   <agg>   ::= COUNT | SUM | MIN | MAX
//   <hcond> ::= <item> <op> <val> | <item> IS [NOT] NULL
//   <key>   ::= <item>
//
// Deliberate limitations: no expressions; WHERE and HAVING are each a flat
// AND-only conjunct list (no OR; NOT only in the reserved negation forms),
// with predicate-position subqueries per V07 and explicit select lists per
// V06. HAVING compares an aggregate against a literal and nothing else
// (docs/inflight/in-progress/workplan-having.md HV3). No quote-escaping in string literals.
// docs/spec/parser-v2.md specifies the grammar this is growing into.
//
// Two decisions worth stating, because both push work downstream on
// purpose:
//
//   - A column's type stays as the raw parsed name (ColumnDef::type_name)
//     rather than being resolved to an on-disk type_val/len here. There is
//     no type registry yet, and resolving names would make the parser
//     depend on an unbuilt subsystem; as it stands it is a pure syntax
//     layer and the consumer resolves against sys.types.
//   - AstValue likewise stays in its parsed form (int/string/null) rather
//     than being encoded to on-disk bytes, which needs a resolved column
//     type first.

namespace kds::parser {

// What sits in a value position.
//
// `kParam` is a declared pattern's `$name`
// (docs/spec/create-pattern-user-defined-patterns-v1.md). It is a value
// **position** with no value in it: a declaration is not an execution, and
// nothing ever binds one - a chain compiled from a pattern body exists to be
// type-checked and fingerprinted, never run. Every path that would consume a
// value refuses it explicitly (exec/row_codec.cpp, exec/step_vm.cpp) rather
// than treating it as a missing string.
// `kDecimal` is the **one** kind the type work added, and TY5 is explicit
// that it is one rather than three: a `DATE` *is* an integer - days since
// the epoch - and a `TIMESTAMP` is microseconds since it, so both reuse
// `kInt` and differ only in how they render, which happens at the emission
// boundary and not in the value. A decimal cannot reuse it, because its
// unscaled integer means nothing without the scale beside it.
// `kDecimalWide` is the 16-byte decimal's kind (`decimal(p, s)`, p 19..38,
// stored as int128 - docs/spec/types.md TY2's separate type, built
// 2026-08-07). A kind of its own rather than a width flag on `kDecimal`,
// deliberately: one kind hiding two widths would make every consumer that
// reads `int_val` silently truncate a wide value, where a new enumerator
// makes the compiler surface each switch that has to decide. The value is
// `Int128FromHalves(dec_hi, int_val)`.
enum class ValueType { kInt, kStr, kNull, kParam, kDecimal, kDecimalWide };

struct AstValue {
    ValueType type = ValueType::kNull;

    // Where the value sits in the statement text.
    //
    // Set for every literal the parser produces, and for kParam. It was
    // kParam only until TY05, when a literal became something a *later*
    // stage can reject: the step compiler coerces one against its column's
    // type (docs/spec/types.md §3.1), so `WHERE d = '2026-02-30'` fails
    // long after the token is gone, and without the offset that failure
    // can name the column but not the byte.
    //
    // **Nothing compares it.** Chain identity renders operand values, not
    // offsets; Cabin keys are built from the value's kind and contents; the
    // fingerprint is folded from tokens. A value constructed by anything
    // other than the parser - a decoded row, a test - leaves it 0.
    std::uint32_t byte_offset = 0;

    std::int64_t int_val = 0;  // valid when type == kInt

    // kStr: the string value. **kParam: the parameter's name**, without the
    // sigil and as written.
    //
    // Sharing the field rather than adding one is deliberate. A kParam
    // carries no string value, so nothing is being overloaded away; and an
    // AstValue is what a chain frame holds one of per column per relation,
    // so a field used by one value kind on a path that never executes would
    // be paid for on every scanned row.
    std::string str_val;

    // For kInt only: the literal's original digit text, preserved
    // alongside int_val. int_val is signed and cannot represent the upper
    // half of an unsigned 64-bit value; once a type registry exists,
    // encoding a uint64 column from this raw text rather than from
    // int_val is how that full range survives.
    std::string raw_int_text;

    // kDecimal and kDecimalWide: the scale the unscaled value is scaled
    // by, so `1234` at scale 2 is 12.34 (docs/spec/types.md TY2/TY5).
    //
    // A separate field rather than a second integer packed into `int_val`,
    // because the unscaled value needs the whole 64 bits - `p` may be 18 -
    // and because a value that carries its own scale is what lets
    // `CompareValues` assert the two sides agree instead of guessing.
    std::uint8_t scale = 0;

    // kDecimalWide only: the high 64 bits of the int128 unscaled value,
    // whose low 64 ride in `int_val` - `Int128FromHalves(dec_hi, int_val)`
    // is the value, and `int128.hpp`'s helpers are the one spelling of
    // which half is which. 8 bytes per AstValue that only the wide kind
    // reads, paid on the same argument `scale` was: a chain frame holds
    // one of these per column, and the alternative - a heap-allocated
    // wide value - would cost an allocation per wide value per row.
    std::int64_t dec_hi = 0;

    // The parameter name a kParam value names. Spelled out so no call site
    // has to know which field it borrows.
    const std::string& param_name() const noexcept { return str_val; }
};

// A column as the statement names it: `x` or `a.x`. Deliberately not
// called ColumnRef - that name belongs to the *compiled* reference V14
// produces, which is `{up, rel_slot, col_pos}` and carries no text at all.
// This is the name-based form, and the difference between them is the
// whole point of the resolution step between.
//
// An ON clause requires the qualifier (a join predicate that does not say
// which relation each side belongs to has no reading that is not a
// guess); a select list and a WHERE clause allow either. Resolving a
// qualifier against the FROM list is V14's job, so nothing here checks
// that `a` names anything - except the dispatcher's single-relation path,
// which has the one binding in hand and can say so.
struct ColumnName {
    std::string qualifier;  // empty when written unqualified
    std::string name;
    std::uint32_t byte_offset = 0;  // of the qualifier if any, else of the name

    bool qualified() const noexcept { return !qualifier.empty(); }
};

// How many keys one `ORDER BY` may name (`[PROPOSED]`). A cap at all is
// the same argument the index column cap makes: the list is unbounded
// client input, every key costs a comparison on every row pair, and a cap
// refuses rather than sorting by a prefix and calling it the answer.
inline constexpr std::size_t kMaxSortKeys = 8;

// kIsNull/kIsNotNull take no right-hand side; they are CompareOps rather
// than a separate predicate kind so every carrier - step predicates, the
// evaluator, the fingerprint - takes them through the one op field it
// already has (docs/spec/null.md, workplan-null.md NU5).
enum class CompareOp { kEq, kNeq, kLt, kLte, kGt, kGte, kIsNull, kIsNotNull };

struct SelectStmt;  // a predicate may carry one - see Condition below

// How deep a chain of predicate-position subqueries may nest
// (docs/spec/parser-v2.md §2, `[PROPOSED default]`). The outermost query block
// is depth 0, so `kMaxSubqueryDepth = 4` admits four nested SELECTs under
// it and refuses the fifth.
//
// A cap at all is not a style preference: the parser recurses per level,
// so an uncapped nest is a stack overflow reachable from a single client
// string, and the executor's nested access rules (spec I15) bound
// recursion at both ends by the same number.
inline constexpr std::uint32_t kMaxSubqueryDepth = 4;

// What a WHERE conjunct actually is. Before V07 there was one shape -
// `col op literal` - and it was implied rather than named.
enum class PredicateKind : std::uint8_t {
    kCompareValue,     // col op literal. The only kind anything executes.
    kCompareSubquery,  // col op (SELECT ...) - scalar; >1 row is a runtime
                       // CardinalityViolation, which parse cannot prove.
    kInSubquery,       // col IN (SELECT ...)
    kNotInSubquery,    // col NOT IN (SELECT ...)
    kExists,           // EXISTS (SELECT ...) - no column
    kNotExists,        // NOT EXISTS (SELECT ...) - no column

    // col BETWEEN <low> AND <high>, inclusive at both ends. Carries two
    // literals rather than a subquery: `val` is the low bound and
    // `val_high` the high one.
    //
    // **It always lowers to two ordinary conjuncts** (`col >= low` and
    // `col <= high`) in the compiled chain, and the range it may *also*
    // put on the step is a hint on top of them - the same relationship
    // `Step::key` has to a lookup's repeated equality. That is what keeps
    // "downgrading any step to a plain scan cannot change the result"
    // true, which is the property invariant 9's fall-through rests on.
    kBetween,
};

// What sits on the right of a comparison.
//
// `kColumn` is what makes a **correlated** subquery expressible at all:
// the correlation is written `WHERE inner.col = outer.col`, and without a
// column on the right there is no way to spell it. Spec I10 describes the
// filter surface as `col op {slot}`; §2's correlated form requires this,
// so the two are reconciled in favour of §2 - a feature J1 put in scope
// cannot be unreachable from the grammar.
enum class RhsKind : std::uint8_t { kLiteral, kColumn };

struct Condition {
    PredicateKind kind = PredicateKind::kCompareValue;

    ColumnName col;                  // unset for kExists / kNotExists
    CompareOp op = CompareOp::kEq;   // kCompareValue and kCompareSubquery

    RhsKind rhs_kind = RhsKind::kLiteral;
    AstValue val;       // rhs_kind == kLiteral; the low bound for kBetween
    AstValue val_high;  // kBetween only: the high bound
    ColumnName rhs_col; // rhs_kind == kColumn

    // The nested query block, for every kind but kCompareValue.
    //
    // shared_ptr rather than unique_ptr for one reason: Statement is
    // copied - by value out of StatusOr, in tests, and wherever a caller
    // holds a parsed statement - and unique_ptr would make the whole AST
    // move-only. Sharing is safe because the AST is immutable once
    // parsed; nothing here is ever written through a second time.
    std::shared_ptr<SelectStmt> subquery;

    bool has_subquery() const noexcept { return subquery != nullptr; }
};

struct ColumnDef {
    std::string name;
    std::string type_name;  // unresolved - see file comment

    // `DECIMAL(p, s)`'s two arguments, as written (docs/spec/types.md TY2).
    //
    // **Both are mandatory and neither is defaulted here.** A bare
    // `decimal` is refused at parse rather than given a scale, because a
    // default scale is a silent decision about someone's money - and a
    // parser that supplied one would make the refusal impossible to state
    // later. `has_precision` is what distinguishes "written `decimal(10,2)`"
    // from "written `decimal`", which the zero values cannot.
    //
    // Left unresolved like `type_name` beside them: the bounds check
    // belongs with the type registry, not in a syntax layer.
    bool has_precision = false;
    std::uint32_t precision = 0;
    std::uint32_t scale = 0;

    // Where the type name was written, for a refusal that can point at it.
    std::uint32_t type_byte_offset = 0;

    // Nullability, D1 of `docs/workplan-null.md`: NOT NULL unless the
    // column says `NULL` - so a statement that says nothing means exactly
    // what it meant before the feature existed. `NOT NULL` also parses and
    // is the same as saying nothing. Written directly after the type,
    // before REFERENCES - the fixed suffix order the fingerprint needs.
    bool notnull = true;
    // Where the `NULL` word was written, for the first-column refusal
    // (invariant 11) to point at. 0 when nothing was said.
    std::uint32_t null_byte_offset = 0;

    // The column's cabin policy (docs/spec/cabin.md), one of
    // `catalog::kCabinPolicy*`. Written as an optional suffix on the column:
    //
    //     sym varchar CABIN            -- enabled: a cabin now, n=1
    //     sym varchar CABIN AUTO       -- auto: the engine may decide (unbuilt)
    //     sym varchar NO CABIN         -- disabled: never, by any route
    //     sym varchar                  -- unset, which reads as auto
    //
    // Unset rather than defaulted here so the catalog stores the difference
    // between "nothing was said" and "the engine may decide" - see
    // EffectiveCabinPolicy (catalog/rows.hpp).
    std::uint8_t cabin_policy = 0;  // catalog::kCabinPolicyUnset

    // Where the policy keyword was written, for an error that can point at
    // it - a cabin on the primary key is refused, and the refusal has to
    // say where.
    std::uint32_t cabin_byte_offset = 0;

    // The relation this column references, from an optional `REFERENCES
    // <table>` suffix (docs/spec/foreign-keys.md §1), or empty for a column
    // that references nothing:
    //
    //     account_id int REFERENCES accounts
    //
    // **No parent column is written and none may be.** F1 fixes the parent
    // side to the referenced relation's Keystone id for every foreign key
    // there can be, so `REFERENCES accounts(id)` would spell out the only
    // thing it could mean - and `REFERENCES accounts(balance)` would spell
    // out something the engine cannot do. Accepting the parenthesized form
    // and checking it is a grammar that exists to reject itself; the parser
    // refuses the '(' with a position instead.
    //
    // Unreserved, like the `CABIN` suffix beside it: a column may still be
    // named `references`, and the fingerprint is untouched because a
    // keyword hashes exactly as an identifier does.
    std::string references_table;

    // Where `REFERENCES` was written, for a refusal that can point at it.
    std::uint32_t references_byte_offset = 0;
};

struct CreateTableStmt {
    std::string table_name;
    std::vector<ColumnDef> columns;
    catalog::ClusteredType clustered = catalog::ClusteredType::kHeap;

    // There is no key-mode field. The trailing `ASSIGNED` | `EXPLICIT` word
    // set one until 2026-08-25; the mode is gone (docs/spec/heap-and-tuple.md
    // §4.1), so `EXPLICIT` is now accepted as the vacuous statement of what
    // every relation is and `ASSIGNED` is refused in the parser with its
    // byte. Neither reaches this struct - there is nothing left for the
    // dispatcher to resolve.

    // Whether the statement *said* a storage word, as opposed to landing on
    // `clustered`'s default above. The field cannot answer this itself:
    // `HEAP` is also what an unqualified statement means, so kHeap is both
    // "the writer asked for a heap" and "the writer said nothing".
    bool clustered_given = false;
};

// The per-statement row cap for a multi-row INSERT (docs/spec/bulkinsert.md
// BI3, `[PROPOSED]`). A config key (`max_insert_rows`), not a compiled-in
// constant - this is only the default the dispatcher starts from. It bounds
// the id burn of an aborted bulk statement (BI9) and the memory a statement
// holds before execution begins.
inline constexpr std::size_t kDefaultMaxInsertRows = 1024;

// `INSERT INTO <t> VALUES (…) [, (…)]*` (docs/spec/bulkinsert.md T1, BI3).
//
// One or more rows; the single-row statement is a rows vector of size one,
// and its parse is byte-identical in behavior to what it always was. The
// row cap is **not** enforced here: the parser is a pure syntax layer with
// no config access, so the first config-aware layer (the dispatcher)
// refuses an over-cap statement naming the cap and the count.
struct InsertStmt {
    std::string table_name;
    std::vector<std::vector<AstValue>> rows;
};


// One relation in a FROM list, with the name predicates will use to refer
// to it.
struct RelationRef {
    // Schema qualifier, empty when written unqualified. The only one that
    // resolves is `sys`, which names the catalog views - the catalog's own
    // relations, readable through typed readers because their on-disk rows
    // are not row-codec tuples (fixed offsets, no Keystone word).
    //
    // Not a general namespace mechanism: user tables live in one flat
    // space and `public.t` is not accepted. Adding one is a catalog
    // change, not a parser change.
    std::string schema;

    std::string table_name;
    std::string alias;              // empty when written without AS
    std::uint32_t byte_offset = 0;  // of the schema if written, else the table name

    // What this relation is called for the rest of the statement: the
    // alias when there is one, otherwise the table name. Two relations in
    // one FROM list may never share a binding - see SelectStmt.
    const std::string& binding() const noexcept { return alias.empty() ? table_name : alias; }
};

// `JOIN <relation> ON <left> = <right>`. Inner equi-join only: one
// equality, both sides qualified. `LEFT`/`RIGHT`/`FULL`/`OUTER` are
// reserved words that answer Unsupported with a position (spec I9), so the
// grammar does not shift when they land.
struct JoinClause {
    RelationRef relation;
    ColumnName left;   // always qualified
    ColumnName right;  // always qualified
};

// The aggregate functions the fold computes (docs/spec/aggregate.md AG2).
//
// `kAvg` joined on 2026-08-07, when §10's open question - return scale,
// rounding rule, divide semantics, "three questions, one answer" - was
// decided rather than expired: **AVG answers at exactly the scale the
// schema declared, rounding half-even**, so `AVG(DECIMAL(p,s))` returns a
// decimal of scale `s` and a column that declared no scale (the integer
// types) is refused at compile - a fractional answer would invent digits
// and a scale-0 one would silently drop them, and this engine's answer to
// "which wrong number would you like" has always been neither. The state
// is the `(sum, count)` pair AG-M mandated in advance.
enum class AggFunc : std::uint8_t { kCount, kSum, kMin, kMax, kAvg };

// The canonical lower-case spelling, for the column label a fold's output
// carries (`count(*)`, `sum(distinct x)`).
std::string_view AggFuncText(AggFunc func) noexcept;

// One entry of a select list, aggregated or not.
//
// **Both shapes in one struct, not two**: the list is written in one order
// and emitted in one order, and splitting it would put the output's column
// order in a third place that has to be kept in step with the two.
//
// A non-aggregated statement's items never reach the AST at all - they
// collapse back into `SelectStmt::projection` at the end of the parse - so
// nothing downstream of a plain SELECT sees this type or changes shape
// because it exists (workplan AG01).
struct SelectItem {
    bool is_aggregate = false;

    // Valid when is_aggregate. `star_arg` is `COUNT(*)`, the one form with
    // no column to name; `distinct` is the word as written, which
    // `MIN`/`MAX` accept and ignore (spec §3.2 `[PROPOSED]`).
    AggFunc func = AggFunc::kCount;
    bool star_arg = false;
    bool distinct = false;

    // The aggregate's argument, or - for a plain item - the item itself.
    // Unset when `star_arg`.
    ColumnName column;

    // The item's first byte, which is the function head for an aggregate
    // and the column for a plain one. AG5's refusal points here.
    std::uint32_t byte_offset = 0;
};

// One `ORDER BY` key: what to order by and the direction written beside it
// (docs/workplan-order-by.md OB1, amended by docs/inflight/in-progress/workplan-having.md HV-1).
// `ASC` and a bare key are the same key - the standard's default - so only
// descending needs a bit.
//
// **A `SelectItem` and not a `ColumnName`**, since HV4 lets an aggregated
// statement order by an aggregate: `ORDER BY COUNT(*) DESC` names something
// no column reference can. A `SelectItem` with `is_aggregate == false` *is*
// a column reference with an offset, so the non-aggregated path means
// exactly what it meant - which is why this is one carrier rather than a
// column plus an optional aggregate beside it.
struct SortKey {
    SelectItem key;
    bool descending = false;
};

// One conjunct of a `HAVING` clause (docs/inflight/in-progress/workplan-having.md HV1, HV3).
//
// The clause is a flat AND-only list, like WHERE and for WHERE's reason:
// this grammar has no expression tree, and an OR would need one. What
// differs from WHERE is the two ends - the left side is an *aggregate*
// (HV2), and the right side is a literal and nothing else, because a
// post-fold predicate has no row to read a column from and no place to put
// a sub-chain (HV3).
struct HavingCondition {
    // What is being compared: the fold's answer for this aggregate.
    //
    // Carried as a `SelectItem` because that is precisely what was
    // written - `count(*)`, `sum(distinct qty)` - and because the compiler
    // matches it against the select list's items by resolved identity, so
    // one carrier keeps that comparison honest. A plain column reaches
    // here too and is refused at compile: whether a name is a grouping key
    // is catalog knowledge (HV-2), not shape.
    SelectItem agg;

    CompareOp op = CompareOp::kEq;

    // The right-hand side. Unread for `kIsNull` / `kIsNotNull`, which take
    // none - the same rule `Condition::val` follows one clause over.
    AstValue val;
};

struct SelectStmt {
    // The select list, or empty for `SELECT *`. Star is refused once
    // there is more than one relation for it to be ambiguous across:
    // which columns `*` means, and in what order, would depend on a join
    // order the client is promised is its own.
    //
    // Projection shape must never affect the statement's class - two
    // statements differing only in which columns they name read the same
    // rows by the same access path, so they are the same kind of
    // statement. Nothing tags a class yet; when something does (V14), it
    // must not read this field.
    std::vector<ColumnName> projection;

    // The FROM list in **written order**, which spec §1 makes a client
    // contract: written order is execution order and nothing reorders it.
    // `from` is the first relation and `joins[i]` the (i+1)-th, each with
    // the predicate that attached it.
    RelationRef from;
    std::vector<JoinClause> joins;  // empty = single-relation statement

    std::vector<Condition> where;  // empty = no WHERE clause; AND-combined

    // ---- Aggregation (docs/spec/aggregate.md) ---------------------------
    //
    // `agg_items` is the select list of an **aggregated** statement, in
    // written order, and is empty for every other statement - including one
    // whose list is entirely plain columns, whose items collapse into
    // `projection` instead. So `aggregated()` is a property of the list and
    // the GROUP BY together, decided once at the end of the parse rather
    // than re-derived by each reader.
    //
    // A statement is aggregated when it writes an aggregate **or** a GROUP
    // BY: `SELECT b FROM t GROUP BY b` names no function and is still a
    // fold, because it emits one row per group rather than one per row.
    std::vector<SelectItem> agg_items;

    // The GROUP BY list in written order. Column references only (AG9) -
    // the grammar has no expressions and GROUP BY does not grow one.
    std::vector<ColumnName> group_by;

    // The `HAVING` conjuncts in written order, AND-combined, empty when no
    // clause was written (docs/inflight/in-progress/workplan-having.md HV-1).
    //
    // **A HAVING makes a statement aggregated**, exactly as a GROUP BY
    // does and for the same reason: it consumes the fold's output rows, so
    // there has to be a fold. That needs no refusal of its own - a bare
    // ungrouped column beside it meets AG5's, with its own byte.
    std::vector<HavingCondition> having;

    // ---- The pagination tail (docs/spec/parser-v2.md I11, workplan V09) ------
    //
    // `[ORDER BY <key> [ASC]] [LIMIT <n>] [OFFSET <m>]`, clauses in that
    // order and each independently optional, parsed on a depth-0 block - a
    // subquery's tail is refused at parse with a position.
    //
    // **`[AMENDED 2026-08-24 — docs/inflight/in-progress/workplan-having.md HV4]`** `ORDER BY`
    // is parsed over an aggregated statement too, where a key may be an
    // aggregate; `LIMIT` and `OFFSET` stay refused there (HV5), because
    // serving them would promote AG6's deterministic fold order into a
    // client contract.
    //
    // `LIMIT` without `ORDER BY` is well-defined here in a way general SQL
    // cannot promise: emission order is already a client contract (I12 -
    // written order across steps, pk order within one), so the clause
    // takes a prefix of an order the statement has, not one it hopes for.
    //
    // `order_by` carries the written sort keys in written order, empty when
    // no `ORDER BY` was written. The parser judges *shape* only - a column
    // reference or (over a fold) an aggregate call, never an ordinal and
    // never an expression - because which relation a name belongs to,
    // whether it is the pk, and whether it is a grouping key are catalog
    // knowledge, and the compiler is where catalog knowledge lives.
    //
    // `DESC` reaches the AST: an output sort does not walk, so descending
    // costs a comparator's sign (docs/workplan-order-by.md OB1).
    std::vector<SortKey> order_by;

    // `LIMIT n`: how many rows the statement emits, nullopt when no LIMIT
    // was written. 0 is a real value - a legal statement that emits
    // nothing - which is why this is optional rather than 0-defaulted.
    std::optional<std::uint64_t> limit;

    // `OFFSET m`: qualifying rows skipped before the first emitted one.
    // 0 when absent, and `OFFSET 0` means exactly that, so no flag is
    // needed to tell the two apart.
    std::uint64_t offset = 0;

    // Every relation's binding is distinct - the parser refuses the
    // statement otherwise, rather than picking one silently. That refusal
    // is what makes a self-join expressible: `FROM t AS a JOIN t AS b` is
    // two bindings over one table, where `FROM t JOIN t` is an ambiguity
    // with no correct reading.
    std::size_t relation_count() const noexcept { return joins.size() + 1; }

    // Whether a fold consumes this statement's rows.
    bool aggregated() const noexcept { return !agg_items.empty(); }

    // Whether the statement was written `SELECT *`.
    //
    // **`projection.empty()` alone is not the question**, now that a select
    // list can land somewhere other than `projection`: an aggregated
    // statement names its columns and leaves `projection` empty, so a star
    // test that read only that field would treat every aggregated statement
    // as a star and emit whole rows for it (workplan AG01, and the same trap
    // `StepChain::star()` has to avoid).
    bool star() const noexcept { return projection.empty() && !aggregated(); }
};

struct Assignment {
    std::string col_name;
    AstValue val;

    // Where `col_name` was written. Carried for the same reason
    // `AstValue::byte_offset` is (types.md TY05): the SET list is
    // resolved against the catalog at compile, and both refusals it can
    // produce - an unknown column, and the primary key, which is the
    // tuple's identity and not a field of it (keystoneid-invariant.md K2)
    // - owe the client an exact position. Nothing compares this field, so
    // adding it moved no fingerprint: the hash folds from the token
    // stream, not from the AST.
    std::uint32_t byte_offset = 0;
};

struct UpdateStmt {
    std::string table_name;
    std::vector<Assignment> assignments;  // SET list, at least one
    std::vector<Condition> where;         // empty = no WHERE clause; AND-combined
};

// `DELETE FROM <t> [WHERE ...]`.
//
// A **delete-mark**, never a physical removal: the tuple's bytes stay for
// readers whose snapshot predates the deleter (docs/spec/txn.md section 4.3), and
// physical retirement is a purge pass that does not exist. That is why there
// is no column list and nothing to assign - a DELETE changes a slot flag and
// a writer id, and nothing else.
//
// The WHERE is the same one UPDATE parses, and it is compiled through the
// same `exec::CompileWhere`, so a DELETE's predicate means exactly what the
// SELECT that found the rows meant.
struct DeleteStmt {
    std::string table_name;
    std::vector<Condition> where;  // empty = every row
};

// `ALTER TABLE <t> RENAME TO <new>` and
// `ALTER TABLE <t> RENAME COLUMN <old> TO <new>` (docs/spec/alter.md AL1).
//
// v1 is catalog-only: a rename changes a catalog label and no tuple
// bytes. Everything else spelled under ALTER answers Unsupported at
// parse with AL1's reason - the row size is a schema constant
// (invariant 13), so a column-set change is a relation rewrite, not a
// catalog edit. One struct for both forms, CabinStmt's precedent: they
// share the resolution and differ in which catalog call they reach.
struct AlterStmt {
    std::string table_name;
    std::uint32_t byte_offset = 0;  // of the table name

    bool rename_column = false;  // false: RENAME TO, the relation itself

    // RENAME COLUMN only: the column as written.
    std::string old_column;
    std::uint32_t old_column_byte_offset = 0;

    // The new name - of the table or the column, by `rename_column`.
    std::string new_name;
    std::uint32_t new_name_byte_offset = 0;
};

// ---- CREATE PATTERN / DROP PATTERN ---------------------------------------
//
//   CREATE PATTERN <name> ( $p <type> [, ...] ) [WITH (<k> = <v> [, ...])]
//       OF <select>
//   DROP PATTERN <name>
//
// The clause order is not a style choice. `WITH` comes *before* `OF` and the
// body comes last, so the body runs to end of statement and the parser never
// has to find where a SELECT ends - the same trick the ANALYZE prefix uses,
// wrapping an intact statement rather than reaching inside one.

// One declared parameter: `$flag bool`.
//
// The type annotation is **mandatory**. Inferring it from first use was
// considered and rejected: it would make the declared contract depend on the
// order predicates are written in, and would leave the CREATE-time type
// check with nothing stable to check against.
//
// `type_name` stays unresolved here, exactly as ColumnDef::type_name does
// and for the reason this file's header gives - the parser is a syntax
// layer, and sys.types is the catalog's business.
struct PatternParam {
    std::string name;  // without the sigil, as written
    std::string type_name;
    std::uint32_t byte_offset = 0;  // of the `$`
};

// One `WITH` option: `pinned = on`. Both sides stay text; which keys exist
// and what values they take is validated against the catalog, not here.
struct PatternOption {
    std::string key;
    std::string value;
    std::uint32_t byte_offset = 0;  // of the key
};

// One occurrence of a `$param` inside the body, in written order.
//
// Recorded separately from the AST the values landed in because the two
// CREATE-time checks that need them ask set questions - "is every use
// declared?" and "is every declaration used?" - and answering those by
// re-walking a compiled chain would miss any occurrence the compiler folded
// away.
struct ParamUse {
    std::string name;
    std::uint32_t byte_offset = 0;
};

struct CreatePatternStmt {
    std::string name;
    std::uint32_t byte_offset = 0;  // of the pattern name

    std::vector<PatternParam> params;  // may be empty: `()` is legal
    std::vector<PatternOption> options;

    // The declared body. shared_ptr for the reason Condition::subquery
    // gives: Statement is copied by value, and unique_ptr would make the
    // whole AST move-only.
    std::shared_ptr<SelectStmt> body;

    // **The whole declaration, verbatim** - `CREATE PATTERN` through the
    // last token of the body - sliced from the input rather than rebuilt
    // from the AST.
    //
    // This is the canon stored in `sys.pattern_defs`. The whole statement
    // and not just the body, because a fingerprint version bump re-registers
    // a declared pattern at boot (spec section 7) and that has to restore
    // the declared types and the `WITH` options too - neither of which is
    // recoverable from the body alone. It is also why there is no separate
    // relation for the parameters: this text already carries them.
    std::string source_text;

    // The body alone, verbatim, sigils included. **This is what gets
    // fingerprinted** - the declaration's leading clauses are not part of
    // the shape any live statement has, so hashing them would guarantee the
    // pattern matched nothing (spec section 3.2).
    std::string body_text;

    std::vector<ParamUse> param_uses;
};

struct DropPatternStmt {
    std::string name;
    std::uint32_t byte_offset = 0;
};

// `DROP TABLE <name>` (docs/spec/drop-table.md). Catalog-scoped: the
// relation becomes unreachable and its oid is tombstoned, never reissued
// (DT2); pages orphan until reclamation exists (DT1).
struct DropTableStmt {
    std::string table_name;
    std::uint32_t byte_offset = 0;
};

// `CREATE CABIN ON <table>(<column>)` / `DROP CABIN ON <table>(<column>)`
// (docs/spec/cabin.md §10).
//
// A Cabin is named by what it is on, never by a name of its own - unlike a
// pattern, which needs one because its shape is not something a client can
// point at. `(relation, column)` already identifies a Cabin uniquely (C3
// keeps v1 to single columns), so a name would be a second identity to keep
// in step with the first.
//
// One struct for both statements: they take the same two identifiers and
// differ only in which catalog call they reach, so a second struct would be
// two copies of one grammar.
struct CabinStmt {
    std::string table_name;
    std::string column_name;
    std::uint32_t byte_offset = 0;         // of the table name
    std::uint32_t column_byte_offset = 0;  // of the column name
    bool drop = false;
};

// One column named in a *declaration's* column list, with where it was
// written - which is what lets "this relation has no column x" carry a
// position.
//
// Named for the index because that is what first needed it; it serves any
// declaration's parenthesised list, and an assertion's `GROUP BY (...)`
// reuses it rather than declaring a second two-field struct. There is one
// production for all of them (`Parser::ParseDeclaredColumnList`).
struct IndexColumnRef {
    std::string name;
    std::uint32_t byte_offset = 0;
};

// `CREATE INDEX <name> ON <table>(<col>, ...) [COVERING (<col>, ...)]` and
// `DROP INDEX <name>` (docs/spec/index.md §10).
//
// **A secondary index is named**, where a Cabin is not, and the difference
// is not a style choice: `(relation, column)` identifies a Cabin uniquely
// because C3 keeps it to one column, while two indexes on one relation may
// share a leading column and differ after it. So there has to be something
// to point `DROP` at.
//
// One struct for both statements, for CabinStmt's reason: they share the
// name resolution and differ only in which catalog call they reach. `DROP`
// fills `index_name` alone - an index's name is unique instance-wide, so
// naming its relation again would be a second identity to keep in step.
struct IndexStmt {
    std::string index_name;
    std::string table_name;
    std::vector<IndexColumnRef> key_columns;
    std::vector<IndexColumnRef> covered_columns;
    std::uint32_t byte_offset = 0;        // of the index name
    std::uint32_t table_byte_offset = 0;
    bool drop = false;
};

// `CREATE ASSERTION <name> ON <rel> GROUP BY (<col>, ...)
//     CHECK COUNT(*) <op> <N> | SUM(<col>) <op> <N>` and
// `DROP ASSERTION <name>` (docs/spec/assertion.md §3, AS2).
//
// **The grammar encodes the supported class** (AS2), which is the whole
// reason this is not SQL-92's free-form `CHECK (<search condition>)`. There
// is no predicate tree here and no expression to evaluate: an assertion is
// four facts - a relation, a group-column list, one of two aggregates, and
// an integer upper bound - and anything outside that shape is refused by the
// parser with a position rather than accepted and then found unenforceable.
// That is what makes the check O(1) against a group header (§5.2) instead of
// a re-evaluation of arbitrary SQL.
//
// One struct for both statements, for IndexStmt's reason: they share the
// name resolution and differ only in which catalog call they reach. `DROP`
// fills `name` alone - an assertion's name is unique instance-wide (§3), so
// naming its relation again would be a second identity to keep in step.
struct AssertionStmt {
    std::string name;
    std::uint32_t byte_offset = 0;  // of the assertion name

    bool drop = false;

    // Everything below is CREATE-only and stays empty for a DROP.
    std::string table_name;
    std::uint32_t table_byte_offset = 0;

    std::vector<IndexColumnRef> group_columns;

    // `kCount` or `kSum`, and nothing else: AggFunc is the shared enum, but
    // this grammar admits two of its five members (§10 - MIN/MAX are not
    // incrementally maintainable under deletion, and AVG is not a bound).
    // The other three are refused by name at the paren.
    AggFunc func = AggFunc::kCount;

    // The `SUM` column, empty for a `COUNT(*)` assertion. Carried as a
    // declared-column ref so the "no such column" error can name its byte
    // exactly as a group column's can.
    IndexColumnRef sum_column;

    // `<` or `<=`, and nothing else (AS11 as revised 2026-08-08).
    //
    // `>` and `>=` parse and are refused as `Unsupported`: a lower bound
    // would have to be checked on DELETE and on every decreasing UPDATE,
    // which is the whole reason v1 can leave DELETE uninstrumented.
    //
    // **`=` is refused for the same reason, and that is the revision.** It
    // was briefly accepted and documented as meaning `aggregate <= N`, on
    // the grounds of syntactic familiarity. That was a truthfulness
    // violation: the engine would have enforced something other than what
    // the operator wrote, and a constraint that quietly means less than it
    // says is worse than one that is refused. Enforcing real equality needs
    // the lower-bound half, so `=` costs exactly what `>=` costs.
    CompareOp op = CompareOp::kLte;

    // The declared bound: a non-negative integer literal (§3.1 - literals
    // only, no expressions, per TY3 conservatism).
    std::int64_t bound = 0;
    std::uint32_t bound_byte_offset = 0;

    // The whole declaration, verbatim, for `sys.assertions.source_text` -
    // the `sys.pattern_defs` model (AS10), where the stored text is the
    // canon and the catalog row carries no per-column table beside it. It is
    // also what makes an unbounded `GROUP BY` list storable in a fixed-width
    // row: the columns are recovered by re-parsing this, not by widening a
    // catalog array.
    std::string source_text;

    // The enforced ceiling, which is **not** always `bound`: the enforced
    // invariant is `aggregate <= this`, so `< N` means `<= N - 1` and `<= N`
    // means itself. Computed once here so no later stage re-derives it and
    // no two stages can disagree about what `<` meant.
    //
    // Both accepted operators map onto a ceiling *without reinterpreting
    // anything* - which is what `=` could not do, and why it is now refused
    // rather than folded in here.
    std::int64_t enforced_max() const noexcept {
        return op == CompareOp::kLt ? bound - 1 : bound;
    }
};

using Statement = std::variant<CreateTableStmt, InsertStmt, SelectStmt, UpdateStmt,
                               DeleteStmt, CreatePatternStmt, DropPatternStmt, CabinStmt,
                               IndexStmt, AssertionStmt, AlterStmt, DropTableStmt>;

// Human-readable statement type name, for logging.
const char* StatementTypeName(const Statement& stmt);

// Human-readable operator, for logging.
const char* CompareOpName(CompareOp op);

}  // namespace kds::parser
