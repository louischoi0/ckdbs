#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "kds/catalog/well_known.hpp"

// AST for the KDS SQL subset. The whole grammar:
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
// Deliberate limitations: no JOINs, subqueries, GROUP BY, ORDER BY or
// aggregates; WHERE is AND-only (no OR, NOT, or nesting); SELECT's column
// list is always * (no projection); no quote-escaping in string literals.
// docs/parser.md specifies the grammar this is to be replaced by.
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

// Human-readable statement type name, for logging.
const char* StatementTypeName(const Statement& stmt);

// Human-readable operator, for logging.
const char* CompareOpName(CompareOp op);

}  // namespace kds::parser
