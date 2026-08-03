#include "kds/parser/ast.hpp"

namespace kds::parser {

const char* StatementTypeName(const Statement& stmt) {
    return std::visit(
        [](const auto& s) -> const char* {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, CreateTableStmt>) return "CREATE TABLE";
            if constexpr (std::is_same_v<T, InsertStmt>) return "INSERT";
            if constexpr (std::is_same_v<T, SelectStmt>) return "SELECT";
            if constexpr (std::is_same_v<T, UpdateStmt>) return "UPDATE";
            if constexpr (std::is_same_v<T, CreatePatternStmt>) return "CREATE PATTERN";
            if constexpr (std::is_same_v<T, DropPatternStmt>) return "DROP PATTERN";
            if constexpr (std::is_same_v<T, CabinStmt>) {
                return s.drop ? "DROP CABIN" : "CREATE CABIN";
            }
        },
        stmt);
}

const char* CompareOpName(CompareOp op) {
    switch (op) {
        case CompareOp::kEq: return "=";
        case CompareOp::kNeq: return "!=";
        case CompareOp::kLt: return "<";
        case CompareOp::kLte: return "<=";
        case CompareOp::kGt: return ">";
        case CompareOp::kGte: return ">=";
    }
    return "?";
}

}  // namespace kds::parser
