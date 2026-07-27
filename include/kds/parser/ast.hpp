#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "kds/catalog/well_known.hpp"

// AST for the KDS SQL subset, ported from the legacy kernel engine's
// kds_parser.h AST types. Same grammar scope as documented there:
//
//   CREATE TABLE <name> (<col> <type> [, ...]) [HEAP | BTREE];
//   INSERT INTO  <name> VALUES (<val> [, ...]);
//   SELECT *     FROM   <name> [WHERE <cond> [AND <cond>]*];
//   UPDATE <name> SET <col> = <val> [, ...] [WHERE <cond> [AND <cond>]*];
//
//   <cond>  ::= <col> <op> <val>
//   <op>    ::= = | != | < | <= | > | >=
//   <val>   ::= integer literal | 'string literal' | NULL
//
// Limitations (deliberate, same as the legacy engine): no JOINs,
// subqueries, GROUP BY, ORDER BY, or aggregates; WHERE is AND-only (no
// OR/NOT/nesting); SELECT's column list is always * (no projection); no
// quote-escaping in string literals. CREATE INDEX and SHOW META/ALLOC
// (which the legacy parser also supports) are not in scope here.
//
// Differences from the legacy AST, and why:
//   - std::vector instead of fixed-capacity arrays (KDS_PARSER_MAX_COLS
//     etc.) - no arbitrary cap to size right or overflow.
//   - Statement is a std::variant of the four statement structs instead
//     of a hand-tagged union - std::get/std::visit give type-safe access
//     the legacy `type` tag + union required manual discipline for.
//   - A column's type is kept as the raw parsed name (ColumnDef::type_name),
//     NOT resolved to an on-disk type_val/len the way the legacy parser's
//     kds_type_lookup_by_name() call did at parse time. This project's
//     type registry (a port of the legacy types.c) doesn't exist yet, so
//     resolving type names is left to whatever consumes this AST later,
//     keeping the parser a pure syntax layer with no dependency on an
//     unbuilt subsystem. Likewise, AstValue stays in its parsed form
//     (int/string/null) rather than being encoded to on-disk bytes here -
//     that also needs a resolved column type first (mirrors the legacy
//     engine's own separation between kds_ast_val_t and the later
//     kds_encode_ast_val() call).

namespace kds::parser {

enum class ValueType { kInt, kStr, kNull };

struct AstValue {
    ValueType type = ValueType::kNull;
    std::int64_t int_val = 0;  // valid when type == kInt
    std::string str_val;       // valid when type == kStr

    // For kInt only: the literal's original digit text, preserved
    // alongside int_val. int_val is a signed 64-bit value and cannot
    // represent the upper half of an unsigned 64-bit primary key
    // (KDS-DESIGN.md's Keystone id is effectively unsigned); once a type
    // registry exists, encoding a uint64 column from this raw text
    // (rather than int_val) is how that full range survives, mirroring
    // the legacy engine's kds_ast_val_t.str_val dual-purpose field.
    std::string raw_int_text;
};

enum class CompareOp { kEq, kNeq, kLt, kLte, kGt, kGte };

struct Condition {
    std::string col_name;
    CompareOp op = CompareOp::kEq;
    AstValue val;
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

struct SelectStmt {
    std::string table_name;
    std::vector<Condition> where;  // empty = no WHERE clause; AND-combined
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

// Human-readable statement type name, for logging - mirrors the legacy
// engine's kds_stmt_type_name().
const char* StatementTypeName(const Statement& stmt);

// Human-readable operator, for logging/EXPLAIN - mirrors the legacy
// engine's kds_cond_op_name().
const char* CompareOpName(CompareOp op);

}  // namespace kds::parser
