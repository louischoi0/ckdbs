#pragma once

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
    StatusOr<SelectStmt> ParseSelect();
    StatusOr<UpdateStmt> ParseUpdate();

    StatusOr<AstValue> ParseValue();
    StatusOr<CompareOp> ParseCompareOp();
    StatusOr<Condition> ParseOneCondition();
    StatusOr<std::vector<Condition>> ParseOptionalWhere();

    StatusOr<std::string> ParseIdent();
    Status ExpectKeyword(std::string_view keyword);
    Status ExpectToken(TokenType type, std::string_view desc);
    void ConsumeOptionalSemicolon();

    Lexer lexer_;
};

// Convenience free function: Parser(sql).Parse().
StatusOr<Statement> Parse(std::string_view sql);

}  // namespace kds::parser
