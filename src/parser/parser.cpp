#include "kds/parser/parser.hpp"

#include <algorithm>
#include <cctype>

namespace kds::parser {

namespace {

bool IEquals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

std::string_view Describe(const Token& tok) {
    return tok.type == TokenType::kEof ? std::string_view("<end of input>")
                                        : std::string_view(tok.text);
}

}  // namespace

Status Parser::ExpectKeyword(std::string_view keyword) {
    Token tok = lexer_.Next();
    if (tok.type != TokenType::kIdent || !IEquals(tok.text, keyword)) {
        return Status::InvalidArgument("expected '" + std::string(keyword) + "', got '" +
                                        std::string(Describe(tok)) + "'");
    }
    return Status::OK();
}

Status Parser::ExpectToken(TokenType type, std::string_view desc) {
    Token tok = lexer_.Next();
    if (tok.type != type) {
        return Status::InvalidArgument("expected " + std::string(desc) + ", got '" +
                                        std::string(Describe(tok)) + "'");
    }
    return Status::OK();
}

StatusOr<std::string> Parser::ParseIdent() {
    Token tok = lexer_.Next();
    if (tok.type != TokenType::kIdent) {
        return Status::InvalidArgument("expected identifier, got '" +
                                        std::string(Describe(tok)) + "'");
    }
    return tok.text;
}

void Parser::ConsumeOptionalSemicolon() {
    if (lexer_.Peek().type == TokenType::kSemicolon) {
        lexer_.Next();
    }
}

StatusOr<AstValue> Parser::ParseValue() {
    Token tok = lexer_.Next();

    switch (tok.type) {
        case TokenType::kIntLit: {
            AstValue v;
            v.type = ValueType::kInt;
            v.int_val = tok.int_val;
            v.raw_int_text = tok.text;  // preserves full-range digits; see ast.hpp
            return v;
        }
        case TokenType::kStrLit: {
            AstValue v;
            v.type = ValueType::kStr;
            v.str_val = tok.text;
            return v;
        }
        case TokenType::kNullLit: {
            AstValue v;
            v.type = ValueType::kNull;
            return v;
        }
        default:
            return Status::InvalidArgument(
                "expected value (integer, 'string', or NULL), got '" +
                std::string(Describe(tok)) + "'");
    }
}

StatusOr<CompareOp> Parser::ParseCompareOp() {
    Token tok = lexer_.Next();
    switch (tok.type) {
        case TokenType::kEq: return CompareOp::kEq;
        case TokenType::kNeq: return CompareOp::kNeq;
        case TokenType::kLt: return CompareOp::kLt;
        case TokenType::kLte: return CompareOp::kLte;
        case TokenType::kGt: return CompareOp::kGt;
        case TokenType::kGte: return CompareOp::kGte;
        default:
            return Status::InvalidArgument(
                "expected comparison operator (=, !=, <, <=, >, >=), got '" +
                std::string(Describe(tok)) + "'");
    }
}

StatusOr<Condition> Parser::ParseOneCondition() {
    auto col = ParseIdent();
    if (!col.ok()) return col.status();

    auto op = ParseCompareOp();
    if (!op.ok()) return op.status();

    auto val = ParseValue();
    if (!val.ok()) return val.status();

    Condition cond;
    cond.col_name = std::move(col.value());
    cond.op = op.value();
    cond.val = std::move(val.value());
    return cond;
}

StatusOr<std::vector<Condition>> Parser::ParseOptionalWhere() {
    std::vector<Condition> conds;

    const Token& peek = lexer_.Peek();
    if (peek.type != TokenType::kIdent || !IEquals(peek.text, "WHERE")) {
        return conds;  // no WHERE clause
    }
    lexer_.Next();  // consume WHERE

    for (;;) {
        auto cond = ParseOneCondition();
        if (!cond.ok()) return cond.status();
        conds.push_back(std::move(cond.value()));

        const Token& next = lexer_.Peek();
        if (next.type == TokenType::kIdent && IEquals(next.text, "AND")) {
            lexer_.Next();
            continue;
        }
        break;
    }

    return conds;
}

StatusOr<CreateTableStmt> Parser::ParseCreateTable() {
    if (Status s = ExpectKeyword("TABLE"); !s.ok()) return s;

    CreateTableStmt stmt;

    auto name = ParseIdent();
    if (!name.ok()) return name.status();
    stmt.table_name = std::move(name.value());

    if (Status s = ExpectToken(TokenType::kLParen, "'('"); !s.ok()) return s;

    for (;;) {
        ColumnDef col;

        auto col_name = ParseIdent();
        if (!col_name.ok()) return col_name.status();
        col.name = std::move(col_name.value());

        auto type_name = ParseIdent();
        if (!type_name.ok()) return type_name.status();
        col.type_name = std::move(type_name.value());

        stmt.columns.push_back(std::move(col));

        if (lexer_.Peek().type == TokenType::kComma) {
            lexer_.Next();
            continue;
        }
        break;
    }

    if (Status s = ExpectToken(TokenType::kRParen, "')'"); !s.ok()) return s;

    if (stmt.columns.empty()) {
        return Status::InvalidArgument("CREATE TABLE requires at least one column");
    }

    // Optional storage clause: HEAP | BTREE. Peek ahead - if neither
    // keyword follows, default to HEAP; any other identifier is left for
    // the trailing-garbage check at the top level.
    const Token& peek = lexer_.Peek();
    if (peek.type == TokenType::kIdent) {
        if (IEquals(peek.text, "HEAP")) {
            lexer_.Next();
            stmt.clustered = catalog::ClusteredType::kHeap;
        } else if (IEquals(peek.text, "BTREE")) {
            lexer_.Next();
            stmt.clustered = catalog::ClusteredType::kBtree;
        }
    }

    ConsumeOptionalSemicolon();
    return stmt;
}

StatusOr<InsertStmt> Parser::ParseInsert() {
    if (Status s = ExpectKeyword("INTO"); !s.ok()) return s;

    InsertStmt stmt;

    auto name = ParseIdent();
    if (!name.ok()) return name.status();
    stmt.table_name = std::move(name.value());

    if (Status s = ExpectKeyword("VALUES"); !s.ok()) return s;
    if (Status s = ExpectToken(TokenType::kLParen, "'('"); !s.ok()) return s;

    for (;;) {
        auto val = ParseValue();
        if (!val.ok()) return val.status();
        stmt.values.push_back(std::move(val.value()));

        if (lexer_.Peek().type == TokenType::kComma) {
            lexer_.Next();
            continue;
        }
        break;
    }

    if (Status s = ExpectToken(TokenType::kRParen, "')'"); !s.ok()) return s;

    if (stmt.values.empty()) {
        return Status::InvalidArgument("INSERT VALUES requires at least one value");
    }

    ConsumeOptionalSemicolon();
    return stmt;
}

StatusOr<SelectStmt> Parser::ParseSelect() {
    // SELECT list: only * is supported (no projection).
    Token star = lexer_.Next();
    if (star.type != TokenType::kStar) {
        return Status::InvalidArgument("only SELECT * is supported, got '" + star.text + "'");
    }

    if (Status s = ExpectKeyword("FROM"); !s.ok()) return s;

    SelectStmt stmt;
    auto name = ParseIdent();
    if (!name.ok()) return name.status();
    stmt.table_name = std::move(name.value());

    auto where = ParseOptionalWhere();
    if (!where.ok()) return where.status();
    stmt.where = std::move(where.value());

    ConsumeOptionalSemicolon();
    return stmt;
}

StatusOr<UpdateStmt> Parser::ParseUpdate() {
    UpdateStmt stmt;

    auto name = ParseIdent();
    if (!name.ok()) return name.status();
    stmt.table_name = std::move(name.value());

    if (Status s = ExpectKeyword("SET"); !s.ok()) return s;

    for (;;) {
        Assignment a;

        auto col_name = ParseIdent();
        if (!col_name.ok()) return col_name.status();
        a.col_name = std::move(col_name.value());

        if (Status s = ExpectToken(TokenType::kEq, "'='"); !s.ok()) return s;

        auto val = ParseValue();
        if (!val.ok()) return val.status();
        a.val = std::move(val.value());

        stmt.assignments.push_back(std::move(a));

        if (lexer_.Peek().type == TokenType::kComma) {
            lexer_.Next();
            continue;
        }
        break;
    }

    auto where = ParseOptionalWhere();
    if (!where.ok()) return where.status();
    stmt.where = std::move(where.value());

    ConsumeOptionalSemicolon();
    return stmt;
}

StatusOr<Statement> Parser::Parse() {
    Token tok = lexer_.Next();

    if (tok.type == TokenType::kEof) {
        return Status::InvalidArgument("empty statement");
    }
    if (tok.type != TokenType::kIdent) {
        return Status::InvalidArgument("expected SQL keyword, got '" + tok.text + "'");
    }

    Statement stmt;

    if (IEquals(tok.text, "CREATE")) {
        auto s = ParseCreateTable();
        if (!s.ok()) return s.status();
        stmt = std::move(s.value());
    } else if (IEquals(tok.text, "INSERT")) {
        auto s = ParseInsert();
        if (!s.ok()) return s.status();
        stmt = std::move(s.value());
    } else if (IEquals(tok.text, "SELECT")) {
        auto s = ParseSelect();
        if (!s.ok()) return s.status();
        stmt = std::move(s.value());
    } else if (IEquals(tok.text, "UPDATE")) {
        auto s = ParseUpdate();
        if (!s.ok()) return s.status();
        stmt = std::move(s.value());
    } else {
        return Status::InvalidArgument("unknown SQL keyword '" + tok.text +
                                        "' (supported: CREATE, INSERT, SELECT, UPDATE)");
    }

    // After a successful parse, only EOF may remain (a trailing semicolon
    // was already consumed by the statement parser itself).
    const Token& tail = lexer_.Peek();
    if (tail.type != TokenType::kEof) {
        return Status::InvalidArgument("unexpected token '" + tail.text +
                                        "' after end of statement");
    }

    return stmt;
}

StatusOr<Statement> Parse(std::string_view sql) { return Parser(sql).Parse(); }

}  // namespace kds::parser
