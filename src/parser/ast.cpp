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
            if constexpr (std::is_same_v<T, DeleteStmt>) return "DELETE";
            if constexpr (std::is_same_v<T, CreatePatternStmt>) return "CREATE PATTERN";
            if constexpr (std::is_same_v<T, DropPatternStmt>) return "DROP PATTERN";
            if constexpr (std::is_same_v<T, CabinStmt>) {
                return s.drop ? "DROP CABIN" : "CREATE CABIN";
            }
            if constexpr (std::is_same_v<T, IndexStmt>) {
                return s.drop ? "DROP INDEX" : "CREATE INDEX";
            }
            if constexpr (std::is_same_v<T, AssertionStmt>) {
                return s.drop ? "DROP ASSERTION" : "CREATE ASSERTION";
            }
            if constexpr (std::is_same_v<T, AlterStmt>) return "ALTER TABLE";
            if constexpr (std::is_same_v<T, DropTableStmt>) return "DROP TABLE";
        },
        stmt);
}

std::string_view AggFuncText(AggFunc func) noexcept {
    switch (func) {
        case AggFunc::kCount: return "count";
        case AggFunc::kSum: return "sum";
        case AggFunc::kMin: return "min";
        case AggFunc::kMax: return "max";
        case AggFunc::kAvg: return "avg";
    }
    return "?";
}

const char* CompareOpName(CompareOp op) {
    switch (op) {
        case CompareOp::kEq: return "=";
        case CompareOp::kNeq: return "!=";
        case CompareOp::kLt: return "<";
        case CompareOp::kLte: return "<=";
        case CompareOp::kGt: return ">";
        case CompareOp::kGte: return ">=";
        case CompareOp::kIsNull: return "IS NULL";
        case CompareOp::kIsNotNull: return "IS NOT NULL";
    }
    return "?";
}

}  // namespace kds::parser
