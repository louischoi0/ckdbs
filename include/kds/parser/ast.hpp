#pragma once

#include <charconv>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "kds/catalog/well_known.hpp"

// AST for the KDS SQL subset. The core of the grammar:
//
//   CREATE TABLE <name> (<col> <type> [, ...])
//       [HEAP | BTREE] [EXPLICIT];   -- either order, each once; EXPLICIT is vacuous
//   INSERT INTO  <name> VALUES (<val> [, ...]) [, (...)]*;
//   SELECT <list> FROM  <rel> [<join>]* [WHERE <where>]
//       [GROUP BY <col> [, ...]] [HAVING <hcond> [AND <hcond>]*]
//       [ORDER BY <key> [ASC | DESC] [, ...]] [LIMIT <int>] [OFFSET <int>];
//   UPDATE <name> SET <col> = <expr> [, ...] [WHERE <where>];
//   DELETE FROM <name> [WHERE <where>];   PURGE FROM <name> WHERE <where>;
//
//   <rel>   ::= <name> [AS <alias>]
//   <join>  ::= JOIN <rel> ON <qcol> = <qcol>
//   <qcol>  ::= <relation-binding> . <col>
//   <where> ::= a boolean tree of <cond> under AND / OR / NOT / ( ) (BU-S1)
//   <cond>  ::= <col> <op> <val> | <col> [NOT] IN (...) | [NOT] BETWEEN
//             | [NOT] LIKE | IS [NOT] NULL | a function comparison | a
//             predicate-position subquery
//   <op>    ::= = | != | < | <= | > | >=
//   <val>   ::= integer or numeric literal | 'string literal' | NULL
//   <list>  ::= * | <item> [, <item>]*
//   <item>  ::= <col> | <agg> ( [DISTINCT] <col> | * )
//   <agg>   ::= COUNT | SUM | MIN | MAX | AVG
//   <hcond> ::= <item> <op> <val> | <item> IS [NOT] NULL
//   <key>   ::= <item>
//
// plus the DDL statements below. HAVING is a flat AND-only list comparing
// an aggregate against a literal; a value expression exists only as a SET
// value (`Expr`). No quote-escaping in string literals.
// docs/spec/parser-v2.md is the grammar's spec.
//
// A column's type stays the raw parsed name (ColumnDef::type_name) and an
// AstValue its parsed form (int/string/null/decimal): the parser is a pure
// syntax layer, and the consumer resolves types and encodes values.

namespace kds::parser {

// What sits in a value position.
//
// `kParam` is a value **position** with no value in it: the `$name` of the
// withdrawn user-declared pattern (2026-08-31,
// docs/spec/create-pattern-user-defined-patterns-v1.md). **Nothing
// constructs one** - the parser refuses `$x` everywhere. The enumerator
// stays, and the consuming paths (exec/row_codec.cpp, exec/step_vm.cpp,
// exec/aggregate.cpp) keep unreachable refusals of it.
//
// `kDecimal` is the one kind TY5 added: `DATE` (days since the epoch) and
// `TIMESTAMP` (microseconds) reuse `kInt` and differ only in rendering, but
// a decimal's unscaled integer means nothing without its scale.
// `kDecimalWide` is the 16-byte decimal (`decimal(p, s)`, p 19..38, int128,
// types.md TY2), a kind of its own so no consumer reading `int_val` can
// silently truncate one. The value is `Int128FromHalves(dec_hi, int_val)`.
enum class ValueType { kInt, kStr, kNull, kParam, kDecimal, kDecimalWide };

struct AstValue {
    ValueType type = ValueType::kNull;

    // Where the value sits in the statement text.
    //
    // Set for every literal the parser produces (TY05): the step compiler
    // coerces a literal against its column's type (docs/spec/types.md
    // §3.1) long after the token is gone, and its refusal names this byte.
    //
    // **Nothing compares it.** Chain identity renders operand values, not
    // offsets; Cabin keys are built from the value's kind and contents; the
    // fingerprint is folded from tokens. A value constructed by anything
    // other than the parser - a decoded row, a test - leaves it 0.
    std::uint32_t byte_offset = 0;

    std::int64_t int_val = 0;  // valid when type == kInt

    // kStr: the string value. kParam: the parameter's name, as written
    // (nothing produces one; the unreachable arms quote it). Shared rather
    // than a second field, since a chain frame holds an AstValue per column
    // per relation and would pay for it on every scanned row.
    std::string str_val;

    // For kInt only: the literal's original digit text, preserved
    // alongside int_val. int_val is signed and cannot represent the upper
    // half of an unsigned 64-bit value, so a uint64 column is encoded from
    // this text when it is set (exec/row_codec.cpp `ValueAsUint64`; a
    // decoded row leaves it empty).
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
    // has to know which field it borrows; its callers are the unreachable
    // kParam arms, which quote the name in what they refuse.
    const std::string& param_name() const noexcept { return str_val; }
};

// **Whether an integer literal wrapped in the lexer** - its value is outside
// int64, so `int_val` is not its value and must not answer a range question
// (`token.hpp`'s digits() note). Judged from the digits by value, so leading
// zeros are no wrap; a text shorter than 19 characters cannot hold one, so
// the common case costs a length test.
inline bool IntLiteralWrapped(const AstValue& v) {
    if (v.type != ValueType::kInt || v.raw_int_text.size() < 19) return false;
    std::int64_t value = 0;
    const char* first = v.raw_int_text.data();
    const char* last = first + v.raw_int_text.size();
    return std::from_chars(first, last, value).ec == std::errc::result_out_of_range;
}

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
// `kLike` (BU-S2) is a text pattern match and travels only inside a boolean
// conjunct (`PredicateKind::kBoolTree`): no flat-conjunct reader - a key,
// a range, the scan kernel - ever sees it.
enum class CompareOp { kEq, kNeq, kLt, kLte, kGt, kGte, kIsNull, kIsNotNull, kLike };

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

    // A comparison with a function call on at least one side (AP-S4,
    // workorder-ap-function-catalog-fetch-id.md AP-R4): `DATE(ts) = '...'`,
    // `ts < NOW()`. `lhs_fn` set means the left side is that call and `col`
    // is unset; `rhs_fn` set means the right side is that call and `val` /
    // `rhs_col` are unset. `op` is the comparison, `IS [NOT] NULL` included.
    //
    // **A kind of its own, so every reader that acts on a `kCompareValue`
    // passes it by** - the point lookup's tuple key test, the write's lock window -
    // rather than reading `col op val` off a conjunct that is about `f(col)`.
    // It lowers to a function conjunct (step_chain.hpp) and never to a
    // `StepPredicate`, which is what keeps it from ever being a key.
    kCompareFunction,

    // A boolean combination holding `OR` or `NOT` (BU-S1,
    // workorder-bu-basic-query-surface.md): `a = 1 OR b = 2`,
    // `NOT (c > 3)`. `tree` holds it; `col`, `op` and `val` are unset.
    //
    // **One conjunct of the top-level AND-list, and never a key** (BU-R5).
    // The parser flattens every top-level AND - parenthesized or not - into
    // ordinary conjuncts, so only a group containing `OR` or `NOT` becomes
    // one of these, and every reader that acts on a `kCompareValue` passes
    // it by, as it passes a `kCompareFunction`. It lowers to
    // `Step::bool_residual` (step_chain.hpp).
    kBoolTree,
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

// A scalar function call as written: `DATE(ts)`, `NOW()`. Its arguments are
// column references - a literal or a nested call is refused at parse
// (`NotImplemented`), since neither shipped function needs one. The name is
// resolved against the function catalog at compile (exec/functions.hpp),
// which is where an unknown one is refused.
struct FunctionCall {
    std::string name;
    std::vector<ColumnName> args;
    std::uint32_t byte_offset = 0;  // the name's first byte
};

struct BoolExpr;

struct Condition {
    PredicateKind kind = PredicateKind::kCompareValue;

    ColumnName col;                  // unset for kExists / kNotExists
    CompareOp op = CompareOp::kEq;   // kCompareValue and kCompareSubquery

    // **Test `kind` before `op`, and before `val`.** Both default to a
    // value that reads as a legal equality - `op` to `kEq`, and for a
    // `kBetween` `val` holds a real integer literal (the low bound) - so a
    // consumer that reads them without checking `kind` sees
    // `WHERE id BETWEEN 2 AND 5` as `WHERE id = 2` (a past bug in two
    // direct readers, once making an UPDATE write the low bound's row
    // alone). Anything lowered through `exec::CompileWhere` is safe - it
    // splits a `kBetween` first; the hazard is in sites reading this struct.

    RhsKind rhs_kind = RhsKind::kLiteral;
    AstValue val;       // rhs_kind == kLiteral; the low bound for kBetween
    AstValue val_high;  // kBetween only: the high bound
    ColumnName rhs_col; // rhs_kind == kColumn

    // kCompareFunction only: the call on each side, where there is one.
    std::optional<FunctionCall> lhs_fn;
    std::optional<FunctionCall> rhs_fn;

    // The nested query block, for every kind but kCompareValue.
    //
    // shared_ptr rather than unique_ptr for one reason: Statement is
    // copied - by value out of StatusOr, in tests, and wherever a caller
    // holds a parsed statement - and unique_ptr would make the whole AST
    // move-only. Sharing is safe because the AST is immutable once
    // parsed; nothing here is ever written through a second time.
    std::shared_ptr<SelectStmt> subquery;

    // kBoolTree only: the combination. Immutable once parsed, and shared
    // for `subquery`'s reason.
    std::shared_ptr<const BoolExpr> tree;

    bool has_subquery() const noexcept { return subquery != nullptr; }
};

// A WHERE's boolean structure as written (BU-S1). A leaf is one ordinary
// condition; `kAnd`/`kOr` have two or more children, `kNot` exactly one.
// What reaches a statement is not this tree but the flat AND-list
// `Parser::ParseOptionalWhere` makes of it (see `PredicateKind::kBoolTree`).
struct BoolExpr {
    enum class Op : std::uint8_t { kLeaf, kAnd, kOr, kNot };
    Op op = Op::kLeaf;
    std::uint32_t byte_offset = 0;  // the node's first token
    Condition leaf;                 // kLeaf
    std::vector<BoolExpr> children; // kAnd, kOr, kNot
};

struct ColumnDef {
    std::string name;
    std::uint32_t name_byte_offset = 0;  // the name's first byte - a repeated name's refusal
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

    // `CHAR(n)` / `VARCHAR(n)`'s single argument, as written
    // (docs/spec/types.md §2b). **A separate pair from `precision`/`scale`
    // above, deliberately**: one number in one field means one thing, so a
    // reader of this AST never has to know the type name to know what it is
    // looking at. Left unresolved and unbounded like `type_name`, for the
    // reason the decimal pair is.
    bool has_width = false;
    std::uint32_t width = 0;

    // Where the type name was written, for a refusal that can point at it.
    std::uint32_t type_byte_offset = 0;

    // Where the type's argument list opened, for a refusal that has to
    // point at the argument rather than at the name - a width outside the
    // legal range is wrong *there*, not where the type was spelled.
    std::uint32_t type_arg_byte_offset = 0;

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
    //     sym varchar CABIN AUTO       -- auto: the Cabin controller decides
    //     sym varchar NO CABIN         -- disabled: never, by any route
    //     sym varchar                  -- unset, which reads as auto
    //
    // Unset rather than defaulted here so the catalog stores the difference
    // between "nothing was said" and "the engine may decide" - see
    // EffectiveCabinPolicy (catalog/rows.hpp).
    std::uint8_t cabin_policy = 0;  // catalog::kCabinPolicyUnset

    // Where the policy keyword was written, for an error that can point at
    // it - a cabin on the tuple key is refused, and the refusal has to
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

    // The namespace qualifier written before the parent, if any - see
    // `namespace-qualifier rule above.
    std::string references_schema;

    // Where `REFERENCES` was written, for a refusal that can point at it.
    std::uint32_t references_byte_offset = 0;
};

// ---- The namespace qualifier a statement may write (AF-T3) ----------------
//
// `ns.t` is accepted everywhere a relation is named
// (`instructions/v2.8.0/ratification-af-namespace.md` AF-6, operator shape
// (a)), and every statement struct below therefore carries a `schema`
// beside its `table_name`, empty when the name was written unqualified.
// Stated once here rather than nine times:
//
// **A relation's name is instance-global, and a qualifier does not change
// which relation a name reaches.** That is AF-T3's decision, and it is the
// half of `docs/spec/namespace.md` NS5/NS6 that AF declines: AF-2 makes a
// namespace a *placement* declaration and AF-9 says outright that it is not
// a name-collision domain. So `orders.customer` and `customer` resolve to
// the same row, and two namespaces still cannot hold two relations of one
// name.
//
// What a qualifier *is*, then, is an assertion the engine checks:
//
//   - at `CREATE TABLE ns.t` it **selects** the namespace, which is the one
//     place it decides anything (the namespace's affinity is declared and
//     read by nothing, docs/spec/namespace.md);
//   - everywhere else it is verified against the relation's stored
//     `namespace_oid` and a disagreement is refused by name with its byte
//     (`Catalog::CheckRelationQualifier`). Accepting a false qualifier
//     silently would be the "truthfulness beats convenience" rule broken on
//     the one statement a reader uses to see where a relation lives.
//
// `sys` keeps its older meaning - the catalog views - and is refused as a
// user namespace (`well_known.hpp`), so no statement changes meaning.
//
// The parser does not resolve any of this: which namespaces exist is the
// catalog's question, exactly as `RelationRef::schema` has always said.
//
// Cited from elsewhere as "ast.hpp's namespace-qualifier rule". It is a
// comment and stays one: a `constexpr` string nothing reads would be a doc
// anchor wearing a symbol's clothes, and the block above is the content.

// `CREATE NAMESPACE <name>` / `DROP NAMESPACE <name>` (AF-T3, AF-6's
// operator-taken shape (a); `docs/spec/namespace.md` NS7).
//
// One struct for both statements, `CabinStmt`'s precedent: they take one
// identifier and differ only in which catalog call they reach. There is no
// `IF NOT EXISTS` and no `CASCADE` - the first would make a typo
// indistinguishable from an intent, which is the whole reason shape (b)
// lost, and the second is NS7's declined scope.
struct NamespaceStmt {
    std::string name;
    std::uint32_t byte_offset = 0;
    bool drop = false;
};

struct CreateTableStmt {
    std::string table_name;
    // The namespace this relation is created in - and the only qualifier
    // in the grammar that *decides* rather than asserts. Empty means
    // `public`.
    std::string schema;
    std::uint32_t table_byte_offset = 0;
    std::vector<ColumnDef> columns;
    // **BTREE since SUS-1** (2026-09-05,
    // `instructions/v3.0.0/workorder-as-sus1-heap-suspended.md`): heap
    // relations are suspended, so an unqualified `CREATE TABLE` makes a
    // btree one. The default is spelled **three** times, all `kBtree`:
    // here (test fixtures read it off the parsed statement),
    // `HandleCreateTableSql`'s `clustered_given` fallback, and
    // `HandleCreateTable`'s call for the bare debug form.
    catalog::ClusteredType clustered = catalog::ClusteredType::kBtree;

    // There is no key-mode field (removed 2026-08-25, heap-and-tuple.md
    // §4.1): `EXPLICIT` is accepted as vacuous and `ASSIGNED` is refused in
    // the parser with its byte.

    // Whether the statement *said* a storage word, as opposed to landing on
    // `clustered`'s default - which is one of the two nameable values, so
    // the field alone cannot tell.
    bool clustered_given = false;

    // `SPARE <n>` (BW-R1 of
    // `instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`): the
    // relation's reserved 16-byte column slots, 0 when the word is absent.
    std::uint16_t spare_slots = 0;
};

// The per-statement row cap for a multi-row INSERT (docs/spec/bulkinsert.md
// BI3, `[PROPOSED]`). A config key (`max_insert_rows`), not a compiled-in
// constant - this is only the default the dispatcher starts from. It bounds
// the id burn of an aborted bulk statement (BI9) and the memory a statement
// holds before execution begins.
inline constexpr std::size_t kDefaultMaxInsertRows = 1024;

// One column named in a *declaration's* column list, with where it was
// written - which is what lets "this relation has no column x" carry a
// position.
//
// Named for the index because that is what first needed it; it serves any
// parenthesised column list - an assertion's `GROUP BY (...)` and an
// `INSERT`'s column list (BW-S6) reuse it rather than declaring a second
// two-field struct. There is one production for all of them
// (`Parser::ParseDeclaredColumnList`).
struct IndexColumnRef {
    std::string name;
    std::uint32_t byte_offset = 0;
};

// The relation and column shape a KWP load described at LOAD BEGIN (BW-Q10
// of `instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`): the
// load's rows are positional against it, so a relation that is not that
// relation in that shape when a chunk takes its `IX` refuses the chunk.
// `oid == 0` - every SQL statement - asks nothing.
struct DescribedShape {
    std::uint64_t oid = 0;
    std::uint32_t physical_columns = 0;
    std::uint32_t live_columns = 0;
};

// `INSERT INTO <t> [(<col>, …)] VALUES (…) [, (…)]*` (docs/spec/bulkinsert.md
// T1, BI3; the column list is BW-S6 of
// `instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`).
//
// One or more rows; the single-row statement is a rows vector of size one,
// and its parse is byte-identical in behavior to what it always was. The
// row cap is **not** enforced here: the parser is a pure syntax layer with
// no config access, so the first config-aware layer (the dispatcher)
// refuses an over-cap statement naming the cap and the count.
struct InsertStmt {
    std::string table_name;
    std::string schema;  // see the namespace-qualifier rule above
    std::uint32_t table_byte_offset = 0;
    // The column list (BW-R13), in written order, each name with its byte;
    // empty for a positional statement, whose values name the live columns
    // in order. `columns_byte_offset` is the list's `(`, where a NOT NULL
    // column the list leaves out is refused.
    std::vector<IndexColumnRef> columns;
    std::uint32_t columns_byte_offset = 0;
    std::vector<std::vector<AstValue>> rows;
    DescribedShape described;
};


// One relation in a FROM list, with the name predicates will use to refer
// to it.
struct RelationRef {
    // Schema qualifier, empty when written unqualified: a user namespace
    // (the namespace-qualifier rule above) or `sys`, which names the
    // catalog views - the catalog's own relations, read through typed
    // readers because their rows are not row-codec tuples.
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
// reserved words that answer NotImplemented with a position (spec I9), so
// the grammar does not shift if they land.
struct JoinClause {
    RelationRef relation;
    ColumnName left;   // always qualified
    ColumnName right;  // always qualified
};

// The aggregate functions the fold computes (docs/spec/aggregate.md AG2).
//
// `kAvg` (2026-08-07, aggregate.md §10): **AVG answers at exactly the scale
// the schema declared, rounding half-even**, so `AVG(DECIMAL(p,s))` returns
// scale `s` and a column with no declared scale (the integer types) is
// refused at compile - neither inventing nor dropping digits. The state is
// the `(sum, count)` pair.
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
// **A `SelectItem` and not a `ColumnName`** (HV4): over an aggregated
// statement a key may parse as an aggregate (`ORDER BY COUNT(*) DESC`),
// which the compiler then refuses `NotImplemented` (step_compiler.cpp). A
// `SelectItem` with `is_aggregate == false` *is* a column reference.
struct SortKey {
    SelectItem key;
    bool descending = false;
};

// One conjunct of a `HAVING` clause (docs/inflight/in-progress/workplan-having.md HV1, HV3).
//
// Parsed, and refused `NotImplemented` at compile (step_compiler.cpp's
// post-fold consumers): the fold has no HAVING filter built.
//
// The clause is a flat AND-only list (no OR, unlike WHERE since BU-S1).
// The left side is an *aggregate* (HV2), and the right side is a literal
// and nothing else, because a post-fold predicate has no row to read a
// column from and no place to put a sub-chain (HV3).
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
    // statement. The chain's class (`exec::StatementClass`) must not read
    // this field.
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

    // The GROUP BY list in written order. Column references only (AG9).
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
    // `[ORDER BY <key> [ASC|DESC] [, ...]] [LIMIT <n>] [OFFSET <m>]`,
    // clauses in that order and each independently optional, parsed on a
    // depth-0 block - a subquery's tail is refused at parse with a position.
    //
    // Over an aggregated statement `ORDER BY` parses (HV4; a key may be an
    // aggregate) and is refused at compile; `LIMIT` and `OFFSET` are refused
    // at parse (HV5), since serving them would promote AG6's fold order
    // into a client contract.
    //
    // `LIMIT` without `ORDER BY` is well-defined here in a way general SQL
    // cannot promise: emission order is already a client contract (I12 -
    // written order across steps, tuple key order within one), so the clause
    // takes a prefix of an order the statement has, not one it hopes for.
    //
    // `order_by` carries the written sort keys in written order, empty when
    // no `ORDER BY` was written. The parser judges *shape* only - a column
    // reference or (over a fold) an aggregate call, never an ordinal and
    // never an expression - because which relation a name belongs to,
    // whether it is the tuple key, and whether it is a grouping key are catalog
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

// A type as a statement writes it, unresolved: `int64`, `varchar(32)`,
// `decimal(10, 2)`. The same fields `ColumnDef` carries for a column, for a
// `CAST` target (BJ-S5); the bounds are the catalog's to check.
struct TypeSpec {
    std::string name;
    bool has_precision = false;
    std::uint32_t precision = 0;
    std::uint32_t scale = 0;
    bool has_width = false;
    std::uint32_t width = 0;
    std::uint32_t byte_offset = 0;      // the name
    std::uint32_t arg_byte_offset = 0;  // the first argument
};

// A value expression in a SET value (BJ-R1 families 1-3, BJ-S2, BJ-S5):
// column references to the target relation, literals, parentheses, unary
// `+`/`-`, binary `+ - * / %` and `||`; the comparisons, `AND`, `OR`, `NOT`
// and `IS [NOT] NULL` as boolean values; `CASE`, `COALESCE`, `NULLIF`,
// `GREATEST`, `LEAST`; `CAST(x AS type)` and `x::type`. Parsed here, **typed
// at compile and evaluated at the write** (BJ-S3, BJ-S4).
//
// Nodes are immutable and shared (`shared_ptr<const Expr>`), so an
// `Assignment`, and with it an `UpdateStmt`, stays copyable.
enum class ExprOp : std::uint8_t {
    kAdd, kSub, kMul, kDiv, kMod, kConcat, kNeg, kPos,
    // Boolean values (BJ-S5): three-valued, yielding `bool`.
    kEq, kNe, kLt, kLe, kGt, kGe, kAnd, kOr, kNot, kIsNull, kIsNotNull,
    // `kCall` nodes: the n-ary forms. `kCase`'s args are
    // `[when, then, when, then, ..., else?]` - a simple `CASE x WHEN a` is
    // desugared at parse to `WHEN x = a`, so an odd count is an `ELSE`.
    kCase, kCoalesce, kNullif, kGreatest, kLeast,
    // `kUnary`: `lhs` converted to `cast_to`.
    kCast,
    // `kCall`: a scalar function by name (`Expr::function`), BJ-S6. Which
    // names exist, their signatures and their results are the typer's.
    kFunction,
    // The query layer's forms (BU-S4, `workorder-bu-basic-query-surface.md`
    // BU-Q8), parsed only by `Parser::ParseQuery`; a SET value never holds
    // one. `kBinary`: `lhs LIKE rhs` (BU-Q6). `kCall`: `args[0] IN (args[1..])`.
    // `kSubquery`: a scalar `(SELECT ...)`, `EXISTS (SELECT ...)`, and
    // `lhs IN (SELECT ...)`. `NOT LIKE`, `NOT IN` and `NOT EXISTS` are `kNot`
    // over the form; `BETWEEN` is the `AND` of two comparisons.
    kLike,
    kInList,
    kScalarSubquery,
    kExists,
    kInSubquery,
};

struct Query;  // a query-layer subquery - see Expr::query

struct Expr {
    // `kAggregate` is the query layer's (BU-S4): `COUNT(*)`, `SUM(x)`, ... over
    // an expression, its function in `agg_func` and its argument in `lhs`
    // (null for `COUNT(*)`).
    enum class Kind : std::uint8_t {
        kLiteral, kColumn, kUnary, kBinary, kCall, kSubquery, kAggregate
    };

    Kind kind = Kind::kLiteral;

    // The first byte of the node as written, and for a unary or binary node
    // the operator's own byte. BJ-R10's refusals name the expression's first
    // byte; a typing refusal (BJ-R3) names the operator it is about.
    std::uint32_t byte_offset = 0;
    // For a binary node the operator's own byte; a unary node's is its
    // `byte_offset`; a call's is the keyword's.
    std::uint32_t op_byte_offset = 0;

    // 1 for a leaf, `max(children) + 1` above. Bounded by the parser
    // (`kMaxExprHeight`): a chain `1+1+1+...` leans entirely left, and a
    // tree that tall is freed, typed and evaluated one nested call per
    // level.
    std::uint32_t height = 1;

    // The nodes under this one **counting a shared child once per reference**
    // (a simple CASE reuses its operand in every WHEN), which is what the
    // typer and the evaluator walk. Bounded by the parser (`kMaxExprSize`):
    // height and nesting depth cannot see a short tree whose size is
    // exponential in its text.
    std::uint32_t size = 1;

    AstValue literal;           // kLiteral
    // kLiteral: the literal was a bare number (`1.5`, not `'1.5'`). The
    // parser turns both into a kStr value (types.md TY3), and an expression
    // has to tell a numeric from a string operand, which a predicate's
    // column never had to.
    bool bare_numeric = false;

    std::string column;         // kColumn, as written
    // kColumn: the relation it is qualified by, `a` in `a.x`; empty when
    // written bare. Only the query grammar reads one (BU-S4).
    std::string qualifier;
    ExprOp op = ExprOp::kAdd;   // kUnary, kBinary, kCall
    std::shared_ptr<const Expr> lhs;  // kUnary's operand, kBinary's left
    std::shared_ptr<const Expr> rhs;  // kBinary's right
    std::vector<std::shared_ptr<const Expr>> args;  // kCall
    TypeSpec cast_to;                               // kUnary with kCast
    std::string function;                           // kCall with kFunction, as written
    // kSubquery (BJ-S7): `(SELECT ...)` as a value. The statement as parsed, and
    // its text without the parentheses - the dispatcher runs the text through
    // the SELECT path once, before the walk.
    std::shared_ptr<SelectStmt> subquery;
    std::string subquery_sql;
    // kSubquery under the query grammar (BU-S4): the block as a query-layer
    // query, `op` saying which form; `subquery` is then unset.
    std::shared_ptr<const Query> query;
    // kAggregate: the function, `DISTINCT` as written.
    AggFunc agg_func = AggFunc::kCount;
    bool agg_distinct = false;
};

// ---- The query layer's statement (BU-S4, BU-Q8) ---------------------------
//
// What `Parser::ParseQuery` makes of a SELECT the native grammar refuses:
// value expressions in the select list, `DISTINCT`, `ORDER BY` an
// expression, an alias or an ordinal, `HAVING` over expressions, every join
// kind, derived tables and set operations. **Never a `Statement`**: the
// dispatcher reaches it only after the native parse or compile refused
// (BU-Q9), and runs it through `exec/query_layer.hpp`.

// One select-list item: an expression with its alias, or `*` / `rel.*`.
struct OutputItem {
    std::shared_ptr<const Expr> expr;  // unset for a star
    std::string alias;                 // `AS alias`, empty when none
    bool star = false;
    std::string star_qualifier;  // `rel` in `rel.*`; empty for a bare `*`
    std::string text;  // the expression as written, which heads an unaliased column
    std::uint32_t byte_offset = 0;
};

enum class JoinKind : std::uint8_t { kInner, kLeft, kRight, kFull, kCross };

// One FROM item: a relation, or a derived table `(SELECT ...) AS name`.
struct FromItem {
    RelationRef rel;                      // `rel.alias` is a derived table's name
    std::shared_ptr<const Query> derived;  // set for a derived table
    std::uint32_t byte_offset = 0;
};

// A FROM item joined to everything left of it.
struct FromJoin {
    JoinKind kind = JoinKind::kInner;
    FromItem item;
    std::shared_ptr<const Expr> on;    // unset for CROSS, a comma and USING
    std::vector<ColumnName> using_columns;
    std::uint32_t byte_offset = 0;     // the join's first word
};

// One `SELECT ... [FROM ...] [WHERE ...] [GROUP BY ...] [HAVING ...]` block.
struct QueryBlock {
    bool distinct = false;
    std::vector<OutputItem> items;
    bool has_from = false;  // `SELECT 1 + 1` has none
    FromItem from;
    std::vector<FromJoin> joins;
    std::shared_ptr<const Expr> where;
    // A conjunct's text span `[begin, end)` per top-level `AND` term of the
    // WHERE, in written order, so a term the native grammar reads can be
    // walked natively (BU-Q8).
    std::vector<std::pair<std::uint32_t, std::uint32_t>> where_spans;
    std::vector<std::shared_ptr<const Expr>> group_by;
    std::shared_ptr<const Expr> having;
    std::uint32_t byte_offset = 0;
};

struct OrderItem {
    std::shared_ptr<const Expr> expr;
    bool descending = false;
};

// A block, or a set operation over two queries; the order and the count
// apply to the whole.
struct Query {
    enum class Op : std::uint8_t { kBlock, kUnion, kIntersect, kExcept };
    Op op = Op::kBlock;
    bool all = false;  // `UNION ALL`, ...
    std::shared_ptr<const QueryBlock> block;  // kBlock
    std::shared_ptr<const Query> lhs;         // the set operations
    std::shared_ptr<const Query> rhs;
    std::vector<OrderItem> order_by;
    std::optional<std::uint64_t> limit;
    std::uint64_t offset = 0;
    std::uint32_t byte_offset = 0;
};

struct Assignment {
    std::string col_name;

    // The value when the SET value is one bare literal (`NULL`
    // included), exactly as before BJ: such an assignment keeps the per-row
    // type gate at the encode (BJ-Q3 (c)) and every reader of `val` stays
    // right. Unset when `expr` is non-null.
    AstValue val;

    // Non-null when the SET value is anything but one bare literal: a column
    // reference, an operator, a parenthesised form. `val` is then unset, and
    // a reader that wants the value must go through the typed expression.
    std::shared_ptr<const Expr> expr;

    // Where `col_name` was written. Carried for the same reason
    // `AstValue::byte_offset` is (types.md TY05): the SET list is
    // resolved against the catalog at compile, and both refusals it can
    // produce - an unknown column, and the tuple key, which is the
    // tuple's identity and not a field of it (keystoneid-invariant.md K2)
    // - owe the client an exact position. Nothing compares this field, so
    // adding it moved no fingerprint: the hash folds from the token
    // stream, not from the AST.
    std::uint32_t byte_offset = 0;
};

struct UpdateStmt {
    std::string table_name;
    std::string schema;  // see the namespace-qualifier rule above
    std::uint32_t table_byte_offset = 0;
    std::vector<Assignment> assignments;  // SET list, at least one
    std::vector<Condition> where;         // empty = no WHERE clause; AND-combined
};

// `DELETE FROM <t> [WHERE ...]`.
//
// A **delete-mark**, never a physical removal: the tuple's bytes stay for
// readers whose snapshot predates the deleter (docs/spec/txn.md section 4.3),
// and physical retirement is `PURGE`'s (`PurgeStmt` below). That is why there
// is no column list and nothing to assign - a DELETE changes a slot flag and
// a writer id, and nothing else.
//
// The WHERE is the same one UPDATE parses, and it is compiled through the
// same `exec::CompileWhere`, so a DELETE's predicate means exactly what the
// SELECT that found the rows meant.
struct DeleteStmt {
    std::string table_name;
    std::string schema;  // see the namespace-qualifier rule above
    std::uint32_t table_byte_offset = 0;
    std::vector<Condition> where;  // empty = every row
};

// `PURGE FROM <t> WHERE <tuple key comparisons>` (BH,
// `instructions/v3.0.0/workorder-bh-purge-key.md` PU1): retires, keyless,
// the slot of every committed delete-marked row the window names once no
// reader can see it, so the key may be named again. `DELETE`'s fields, and
// the `WHERE` token's byte, which a conjunct with no position of its own
// (a bare subquery) is refused at. `PURGE` is a head matched by text and
// never a reserved word.
struct PurgeStmt {
    std::string table_name;
    std::string schema;
    std::uint32_t table_byte_offset = 0;
    std::uint32_t where_byte_offset = 0;
    std::vector<Condition> where;  // never empty: the parser refuses a bare PURGE
};

// `ALTER TABLE <t> RENAME TO <new>`, `ALTER TABLE <t> RENAME COLUMN <old>
// TO <new>` (docs/spec/alter.md AL1), `ALTER TABLE <t> DROP [COLUMN] <name>`
// and `ALTER TABLE <t> ADD [COLUMN] <column definition>` (BW-R1 of
// `instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`).
//
// Every form changes catalog rows and no tuple byte: a rename a label, a
// drop a flag on a column whose bytes stay in every row (BW-R6), an add a
// column in a slot every row already has (BW-R2). One struct
// for every form, CabinStmt's precedent: they share the resolution and
// differ in which catalog call they reach.
enum class AlterAction : std::uint8_t {
    kRenameTable,
    kRenameColumn,
    kDropColumn,
    kAddColumn,
};

struct AlterStmt {
    std::string table_name;
    std::string schema;  // see the namespace-qualifier rule above
    std::uint32_t byte_offset = 0;  // of the table name

    AlterAction action = AlterAction::kRenameTable;

    // RENAME COLUMN and DROP COLUMN: the column as written.
    std::string old_column;
    std::uint32_t old_column_byte_offset = 0;

    // The new name - of the table or the column, by `action`.
    std::string new_name;
    std::uint32_t new_name_byte_offset = 0;

    // ADD COLUMN only: the definition, in `CREATE TABLE`'s grammar, with
    // the byte its name was written at.
    ColumnDef column;
    std::uint32_t column_byte_offset = 0;
};

// `CREATE PATTERN` / `DROP PATTERN` were withdrawn on 2026-08-31
// (`docs/spec/create-pattern-user-defined-patterns-v1.md`): a pattern is
// fingerprint-identified, and nothing in the Waystone path read the
// declaration.

// `DROP TABLE <name>` (docs/spec/drop-table.md). Catalog-scoped: the
// relation becomes unreachable and its oid is tombstoned, never reissued
// (DT2); its pages are reclaimed after the commit (DT1).
struct DropTableStmt {
    std::string table_name;
    std::string schema;  // see the namespace-qualifier rule above
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
    std::string schema;  // see the namespace-qualifier rule above
    std::string column_name;
    std::uint32_t byte_offset = 0;         // of the table name
    std::uint32_t column_byte_offset = 0;  // of the column name
    bool drop = false;
};

// `IndexColumnRef` is declared above `InsertStmt`, whose column list uses it.

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
    std::string schema;  // see the namespace-qualifier rule above
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
    std::string schema;  // see the namespace-qualifier rule above
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
    // `>`, `>=` and `=` parse and are refused `NotImplemented`: a lower
    // bound would have to be checked on DELETE and on every decreasing
    // UPDATE, which v1 leaves uninstrumented, and reading `=` as `<=` would
    // enforce less than was written.
    CompareOp op = CompareOp::kLte;

    // The declared bound: a non-negative integer literal (§3.1 - literals
    // only, no expressions, per TY3 conservatism).
    std::int64_t bound = 0;
    std::uint32_t bound_byte_offset = 0;

    // The whole declaration, verbatim, for `sys.assertions.source_text` -
    // the stored-text model (AS10), where the stored text is the
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

// `CREATE TRANSITION <name> ON <table>(<column>) ON INSERT (<state>, ...)
// ON UPDATE (<state> => <state>, ...) [ON DELETE FROM (<state>, ...)]` and
// `DROP TRANSITION <name>` (`instructions/v3.2.0/workorder-bv-transition-
// constraints.md`). A state is an integer or a string literal; which one a
// column admits is the catalog's question, not the grammar's. One struct for
// both statements, for AssertionStmt's reason: DROP fills `name` alone.
struct TransitionStmt {
    std::string name;
    std::uint32_t byte_offset = 0;  // of the transition name

    bool drop = false;

    // CREATE only.
    std::string table_name;
    std::string schema;  // see the namespace-qualifier rule above
    std::uint32_t table_byte_offset = 0;
    std::string column;
    std::uint32_t column_byte_offset = 0;

    // `ON INSERT`: the states a row may be inserted in.
    std::vector<AstValue> initial;
    // `ON UPDATE`: the allowed `from => to` moves, as written.
    struct Edge {
        AstValue from;
        AstValue to;
    };
    std::vector<Edge> edges;
    // `ON DELETE FROM`: the states a row may be deleted from. Absent, every
    // delete is allowed (CN-4 T10, adopted under BV's go-ahead).
    bool delete_declared = false;
    std::vector<AstValue> deletable;

    // The whole declaration verbatim, for `sys.transitions.source_text`:
    // the canon the reader re-parses (BV-Q1 (a)).
    std::string source_text;
};

using Statement = std::variant<CreateTableStmt, InsertStmt, SelectStmt, UpdateStmt,
                               DeleteStmt, PurgeStmt, CabinStmt, IndexStmt, AssertionStmt,
                               TransitionStmt, AlterStmt, DropTableStmt, NamespaceStmt>;

// Human-readable statement type name, for logging.
const char* StatementTypeName(const Statement& stmt);

// Human-readable operator, for logging.
const char* CompareOpName(CompareOp op);

}  // namespace kds::parser
