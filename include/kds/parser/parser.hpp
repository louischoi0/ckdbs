#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "kds/base/status.hpp"
#include "kds/parser/ast.hpp"
#include "kds/parser/lexer.hpp"

// Recursive-descent parser for the KDS SQL subset; ast.hpp documents the
// grammar. Every Parse*() method follows one contract: return or propagate
// a non-ok Status on the first syntax error and stop. There is no error
// recovery and no backtracking - once a production fails, the parse fails.
//
// rules.md #1: `throw` is forbidden, so every fallible step here returns
// Status/StatusOr rather than throwing - this is pure, syscall-free
// engine logic (no clock reads, no I/O), so unlike main.cpp/tcp_server.cpp
// there is nothing here that needs an injectable platform interface.

namespace kds::parser {

class Parser {
public:
    explicit Parser(std::string_view sql) noexcept : lexer_(sql) {}

    // Parses exactly one statement from the input given at construction.
    // Fails with InvalidArgument (message describes the syntax error) if
    // the input is empty, uses an unsupported keyword, or is otherwise
    // malformed - including trailing garbage after an otherwise-valid
    // statement: only EOF may follow one.
    StatusOr<Statement> Parse();

private:
    StatusOr<CreateTableStmt> ParseCreateTable();
    StatusOr<InsertStmt> ParseInsert();
    StatusOr<UpdateStmt> ParseUpdate();

    // `depth` is how many query blocks enclose this one: 0 at the top
    // level, +1 per predicate-position subquery. Carried as a parameter
    // rather than as parser state because it must unwind exactly with the
    // recursion, and a member would have to be restored by hand on every
    // error path (V07).
    StatusOr<SelectStmt> ParseSelect(std::uint32_t depth);

    // `( SELECT ... )` in predicate position. Consumes both parens and
    // enforces kMaxSubqueryDepth.
    StatusOr<std::shared_ptr<SelectStmt>> ParseSubquery(std::uint32_t depth);

    // FROM-list productions (V05). ParseRelationRef takes `<name> [AS
    // <alias>]`; ParseJoins takes zero or more `JOIN <rel> ON <q> = <q>`
    // and appends them in written order.
    StatusOr<RelationRef> ParseRelationRef();
    Status ParseJoins(SelectStmt& stmt);
    Status CheckDistinctBindings(const SelectStmt& stmt) const;

    // `x` or `a.x`. ParseQualifiedColumn is the ON-clause form: the same
    // production, refusing the unqualified spelling.
    StatusOr<ColumnName> ParseColumnName();
    StatusOr<ColumnName> ParseQualifiedColumn();

    // `*`, or a comma-separated list of column names. Star leaves
    // SelectStmt::projection empty (V06).
    Status ParseSelectList(SelectStmt& stmt);

    StatusOr<AstValue> ParseValue();
    StatusOr<CompareOp> ParseCompareOp();
    StatusOr<Condition> ParseOneCondition(std::uint32_t depth);
    StatusOr<std::vector<Condition>> ParseOptionalWhere(std::uint32_t depth);

    StatusOr<std::string> ParseIdent();
    Status ExpectKeyword(std::string_view keyword);
    Status ExpectToken(TokenType type, std::string_view desc);
    void ConsumeOptionalSemicolon();

    Lexer lexer_;
};

// Convenience free function: Parser(sql).Parse().
StatusOr<Statement> Parse(std::string_view sql);

}  // namespace kds::parser
