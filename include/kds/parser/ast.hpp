#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "kds/catalog/well_known.hpp"

// AST for the KDS SQL subset. The whole grammar:
//
//   CREATE TABLE <name> (<col> <type> [, ...]) [HEAP | BTREE];
//   INSERT INTO  <name> VALUES (<val> [, ...]);
//   SELECT <list> FROM  <rel> [<join>]* [WHERE <cond> [AND <cond>]*]
//       [GROUP BY <col> [, ...]];
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
//
// Deliberate limitations: no subqueries, ORDER BY, HAVING or expressions;
// WHERE is AND-only (no OR, NOT, or nesting) and its columns are still
// unqualified; SELECT's column list is always * (no projection), which is
// why a multi-relation SELECT is refused until V06 makes an explicit list
// available. No quote-escaping in string literals.
// docs/parser-v2.md specifies the grammar this is growing into.
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
// (docs/spec-create-pattern-user-defined-patterns-v1.md). It is a value
// **position** with no value in it: a declaration is not an execution, and
// nothing ever binds one - a chain compiled from a pattern body exists to be
// type-checked and fingerprinted, never run. Every path that would consume a
// value refuses it explicitly (exec/row_codec.cpp, exec/step_vm.cpp) rather
// than treating it as a missing string.
enum class ValueType { kInt, kStr, kNull, kParam };

struct AstValue {
    ValueType type = ValueType::kNull;

    // Where the value sits in the statement text. Set for kParam, where an
    // error message has to be able to point at the offending `$x`; other
    // kinds may leave it 0.
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

enum class CompareOp { kEq, kNeq, kLt, kLte, kGt, kGte };

struct SelectStmt;  // a predicate may carry one - see Condition below

// How deep a chain of predicate-position subqueries may nest
// (docs/parser-v2.md §2, `[PROPOSED default]`). The outermost query block
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

    // `DECIMAL(p, s)`'s two arguments, as written (docs/spec-types.md TY2).
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

    // The column's cabin policy (docs/feat-cabin.md), one of
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
    // <table>` suffix (docs/impl-foreign-keys.md §1), or empty for a column
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
};

struct InsertStmt {
    std::string table_name;
    std::vector<AstValue> values;
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

// The aggregate functions v1 folds (docs/feat-aggregate.md AG2).
//
// `AVG` is deliberately **not** here. It is recognized by the parser and
// answers `Unsupported`, because `AstValue` has no decimal kind and an
// average truncated to an integer is a wrong answer wearing a right
// answer's type - so there is nothing for this enum to name until a
// decimal type exists. Adding a value for a function that cannot be folded
// would put the refusal somewhere it can be forgotten.
enum class AggFunc : std::uint8_t { kCount, kSum, kMin, kMax };

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

    // ---- Aggregation (docs/feat-aggregate.md) ---------------------------
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
};

struct UpdateStmt {
    std::string table_name;
    std::vector<Assignment> assignments;  // SET list, at least one
    std::vector<Condition> where;         // empty = no WHERE clause; AND-combined
};

// `DELETE FROM <t> [WHERE ...]`.
//
// A **delete-mark**, never a physical removal: the tuple's bytes stay for
// readers whose snapshot predates the deleter (docs/txn.md section 4.3), and
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

// `CREATE CABIN ON <table>(<column>)` / `DROP CABIN ON <table>(<column>)`
// (docs/feat-cabin.md §10).
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

using Statement = std::variant<CreateTableStmt, InsertStmt, SelectStmt, UpdateStmt,
                               DeleteStmt, CreatePatternStmt, DropPatternStmt, CabinStmt>;

// Human-readable statement type name, for logging.
const char* StatementTypeName(const Statement& stmt);

// Human-readable operator, for logging.
const char* CompareOpName(CompareOp op);

}  // namespace kds::parser
