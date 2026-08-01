#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "kds/catalog/well_known.hpp"

// AST for the KDS SQL subset. The whole grammar:
//
//   CREATE TABLE <name> (<col> <type> [, ...]) [HEAP | BTREE];
//   INSERT INTO  <name> VALUES (<val> [, ...]);
//   SELECT *     FROM   <rel> [<join>]* [WHERE <cond> [AND <cond>]*];
//   UPDATE <name> SET <col> = <val> [, ...] [WHERE <cond> [AND <cond>]*];
//
//   <rel>   ::= <name> [AS <alias>]
//   <join>  ::= JOIN <rel> ON <qcol> = <qcol>
//   <qcol>  ::= <relation-binding> . <col>
//   <cond>  ::= <col> <op> <val>
//   <op>    ::= = | != | < | <= | > | >=
//   <val>   ::= integer literal | 'string literal' | NULL
//
// Deliberate limitations: no subqueries, GROUP BY, ORDER BY or aggregates;
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

enum class ValueType { kInt, kStr, kNull };

struct AstValue {
    ValueType type = ValueType::kNull;
    std::int64_t int_val = 0;  // valid when type == kInt
    std::string str_val;       // valid when type == kStr

    // For kInt only: the literal's original digit text, preserved
    // alongside int_val. int_val is signed and cannot represent the upper
    // half of an unsigned 64-bit value; once a type registry exists,
    // encoding a uint64 column from this raw text rather than from
    // int_val is how that full range survives.
    std::string raw_int_text;
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
    AstValue val;       // rhs_kind == kLiteral
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

    // Every relation's binding is distinct - the parser refuses the
    // statement otherwise, rather than picking one silently. That refusal
    // is what makes a self-join expressible: `FROM t AS a JOIN t AS b` is
    // two bindings over one table, where `FROM t JOIN t` is an ambiguity
    // with no correct reading.
    std::size_t relation_count() const noexcept { return joins.size() + 1; }

    // Whether the statement was written `SELECT *`.
    bool star() const noexcept { return projection.empty(); }
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

using Statement = std::variant<CreateTableStmt, InsertStmt, SelectStmt, UpdateStmt>;

// Human-readable statement type name, for logging.
const char* StatementTypeName(const Statement& stmt);

// Human-readable operator, for logging.
const char* CompareOpName(CompareOp op);

}  // namespace kds::parser
