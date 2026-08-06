#include "kds/parser/parser.hpp"

#include "kds/catalog/rows.hpp"

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
                                        : tok.text;
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
    // The AST owns its names: docs/parser.md I4's copy-at-the-boundary
    // rule, and what lets a Statement outlive the SQL it came from now that
    // a token is only a view into it.
    return std::string(tok.text);
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
            v.raw_int_text = std::string(tok.text);  // full-range digits; see ast.hpp
            return v;
        }
        case TokenType::kStrLit: {
            AstValue v;
            v.type = ValueType::kStr;
            v.str_val = std::string(tok.text);
            return v;
        }
        case TokenType::kNullLit: {
            AstValue v;
            v.type = ValueType::kNull;
            return v;
        }
        case TokenType::kNamedParam: {
            // Legal in exactly one place: the body of a CREATE PATTERN
            // (spec section 3.1). Outside one the token is *reserved* - for
            // the extended protocol's named binds - and refusing it by name
            // and position is what tells a client that, rather than leaving
            // them to read "expected value" and guess the sigil was a typo.
            if (param_uses_ == nullptr) {
                return Status::Unsupported(
                    "parameter '$" + std::string(tok.text) +
                    "' is only allowed in a CREATE PATTERN body (byte " +
                    std::to_string(tok.byte_offset) + ")");
            }
            param_uses_->push_back(ParamUse{std::string(tok.text), tok.byte_offset});

            AstValue v;
            v.type = ValueType::kParam;
            v.str_val = std::string(tok.text);  // the name; see AstValue::param_name()
            v.byte_offset = tok.byte_offset;
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

StatusOr<std::shared_ptr<SelectStmt>> Parser::ParseSubquery(std::uint32_t depth) {
    const std::uint32_t open_at = lexer_.Peek().byte_offset;

    // The cap is checked before recursing, so the *refused* level is the
    // one named in the message and no frame for it is ever pushed. Spec
    // section 2 puts it at 4.
    if (depth + 1 > kMaxSubqueryDepth) {
        return Status::Unsupported(
            "subquery nesting deeper than " + std::to_string(kMaxSubqueryDepth) +
            " is not supported (byte " + std::to_string(open_at) + ")");
    }

    if (Status s = ExpectToken(TokenType::kLParen, "'(' before a subquery"); !s.ok()) return s;

    // One token of lookahead is enough to know a subquery from anything
    // else here, because this grammar has no parenthesized expressions
    // (spec I10) - a '(' in predicate position can only open a SELECT or
    // a value list, and the value list is V08's.
    const Token& head = lexer_.Peek();
    if (head.type != TokenType::kIdent || !IEquals(head.text, "SELECT")) {
        return Status::InvalidArgument("expected a subquery `(SELECT ...)`, got '" +
                                        std::string(Describe(head)) + "' at byte " +
                                        std::to_string(head.byte_offset));
    }
    lexer_.Next();  // consume SELECT

    auto inner = ParseSelect(depth + 1);
    if (!inner.ok()) return inner.status();

    if (Status s = ExpectToken(TokenType::kRParen, "')' closing a subquery"); !s.ok()) return s;
    return std::make_shared<SelectStmt>(std::move(inner.value()));
}

StatusOr<Condition> Parser::ParseOneCondition(std::uint32_t depth) {
    Condition cond;

    // Leading `EXISTS` / `NOT EXISTS`: the two predicates with no column
    // on the left at all.
    const Token& lead = lexer_.Peek();
    if (lead.type == TokenType::kKeyword &&
        (lead.kw == Keyword::kExists || lead.kw == Keyword::kNot)) {
        bool negated = lead.kw == Keyword::kNot;
        lexer_.Next();
        if (negated) {
            // `NOT` here can only introduce `NOT EXISTS`. `NOT IN` is
            // reached with a column already parsed, below, and bare `NOT`
            // over an expression is excluded by spec I10 - the grammar has
            // no expression tree for it to negate.
            const Token& next = lexer_.Peek();
            if (next.type != TokenType::kKeyword || next.kw != Keyword::kExists) {
                return Status::Unsupported(
                    "NOT is supported only as `NOT EXISTS (...)` or `NOT IN (...)` (byte " +
                    std::to_string(lead.byte_offset) + ")");
            }
            lexer_.Next();
        }
        cond.kind = negated ? PredicateKind::kNotExists : PredicateKind::kExists;

        auto sub = ParseSubquery(depth);
        if (!sub.ok()) return sub.status();
        cond.subquery = std::move(sub.value());
        return cond;
    }

    // Qualified or not: a join's WHERE needs `a.x` to say which relation,
    // and a single-relation WHERE has always been bare `x`. Both spellings
    // reach the same ColumnName.
    auto col = ParseColumnName();
    if (!col.ok()) return col.status();
    cond.col = std::move(col.value());

    // `col IN (...)` / `col NOT IN (...)`.
    const Token& after_col = lexer_.Peek();
    if (after_col.type == TokenType::kKeyword &&
        (after_col.kw == Keyword::kIn || after_col.kw == Keyword::kNot)) {
        const bool negated = after_col.kw == Keyword::kNot;
        const std::uint32_t not_at = after_col.byte_offset;
        lexer_.Next();
        if (negated) {
            const Token& in_tok = lexer_.Peek();
            if (in_tok.type != TokenType::kKeyword || in_tok.kw != Keyword::kIn) {
                return Status::Unsupported(
                    "NOT is supported only as `NOT IN (...)` or `NOT EXISTS (...)` (byte " +
                    std::to_string(not_at) + ")");
            }
            lexer_.Next();
        }
        cond.kind = negated ? PredicateKind::kNotInSubquery : PredicateKind::kInSubquery;

        // `IN (1, 2)` - a value list rather than a subquery - is V08's,
        // and ParseSubquery is what reports it: it consumes the paren and
        // then says a subquery was expected, naming the token it found.
        auto sub = ParseSubquery(depth);
        if (!sub.ok()) return sub.status();
        cond.subquery = std::move(sub.value());
        return cond;
    }

    // `col BETWEEN <low> AND <high>`. Half of workplan V08; the `IN (list)`
    // half is still open and still reports through ParseSubquery above.
    //
    // Costs the fingerprint nothing: `BETWEEN` has been a reserved keyword
    // since V04 and a keyword hashes exactly as the identifier it used to
    // be, so every pattern_id for a statement containing one is unchanged
    // by this becoming parseable. The corpus pins that.
    if (after_col.type == TokenType::kKeyword && after_col.kw == Keyword::kBetween) {
        lexer_.Next();
        cond.kind = PredicateKind::kBetween;

        auto low = ParseValue();
        if (!low.ok()) return low.status();
        cond.val = std::move(low.value());

        const Token& and_tok = lexer_.Peek();
        if (and_tok.type != TokenType::kIdent || !IEquals(and_tok.text, "AND")) {
            return Status::InvalidArgument("expected AND after the low bound of BETWEEN, got '" +
                                            std::string(Describe(and_tok)) + "' (byte " +
                                            std::to_string(and_tok.byte_offset) + ")");
        }
        lexer_.Next();

        auto high = ParseValue();
        if (!high.ok()) return high.status();
        cond.val_high = std::move(high.value());
        return cond;
    }

    auto op = ParseCompareOp();
    if (!op.ok()) return op.status();
    cond.op = op.value();

    // `col op (SELECT ...)` - a scalar subquery. Its cardinality cannot
    // be proven here (spec section 2), so more than one row is a runtime
    // CardinalityViolation, not a parse error.
    if (lexer_.Peek().type == TokenType::kLParen) {
        cond.kind = PredicateKind::kCompareSubquery;
        auto sub = ParseSubquery(depth);
        if (!sub.ok()) return sub.status();
        cond.subquery = std::move(sub.value());
        return cond;
    }

    // A column on the right, rather than a value. This is how a
    // correlated subquery is written - `WHERE inner.col = outer.col` -
    // and without it §2's correlated forms could not be spelled at all.
    // One token of lookahead separates the two: a value literal is never
    // an identifier, and an identifier is never a value.
    if (lexer_.Peek().type == TokenType::kIdent) {
        auto rhs = ParseColumnName();
        if (!rhs.ok()) return rhs.status();
        cond.kind = PredicateKind::kCompareValue;
        cond.rhs_kind = RhsKind::kColumn;
        cond.rhs_col = std::move(rhs.value());
        return cond;
    }

    auto val = ParseValue();
    if (!val.ok()) return val.status();
    cond.kind = PredicateKind::kCompareValue;
    cond.rhs_kind = RhsKind::kLiteral;
    cond.val = std::move(val.value());
    return cond;
}

StatusOr<std::vector<Condition>> Parser::ParseOptionalWhere(std::uint32_t depth) {
    std::vector<Condition> conds;

    const Token& peek = lexer_.Peek();
    if (peek.type != TokenType::kIdent || !IEquals(peek.text, "WHERE")) {
        return conds;  // no WHERE clause
    }
    lexer_.Next();  // consume WHERE

    for (;;) {
        auto cond = ParseOneCondition(depth);
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

        // Optional `REFERENCES <table>` (docs/impl-foreign-keys.md §1).
        // Peeked like the cabin clause below it, and written *before* it
        // when both appear - a fixed order, because two optional suffixes
        // accepted in either order is a grammar with a shape nobody can
        // state, and the fingerprint would have to hash both spellings of
        // one declaration.
        if (const Token& refs = lexer_.Peek();
            refs.type == TokenType::kIdent && IEquals(refs.text, "REFERENCES")) {
            col.references_byte_offset = refs.byte_offset;
            lexer_.Next();

            auto parent = ParseIdent();
            if (!parent.ok()) {
                return parent.status().WithContext("REFERENCES names the parent relation");
            }
            col.references_table = std::move(parent.value());

            // `REFERENCES parent(col)` is refused rather than parsed and
            // checked: the parent side is always the Keystone id (F1), so
            // the only column that could be named is the one the engine
            // would have used anyway, and naming any other is asking for a
            // reference the engine cannot store.
            if (lexer_.Peek().type == TokenType::kLParen) {
                return Status::Unsupported(
                    "a foreign key references the parent's primary key and no other column, so "
                    "REFERENCES takes no column list (byte " +
                    std::to_string(lexer_.Peek().byte_offset) + ")");
            }
        }

        // Optional cabin policy: `CABIN`, `CABIN AUTO`, or `NO CABIN`
        // (docs/feat-cabin.md). Peeked rather than required, so every
        // pre-existing CREATE TABLE parses unchanged and lands on
        // kCabinPolicyUnset.
        const Token& policy = lexer_.Peek();
        if (policy.type == TokenType::kIdent && IEquals(policy.text, "CABIN")) {
            col.cabin_byte_offset = policy.byte_offset;
            lexer_.Next();
            const Token& mode = lexer_.Peek();
            if (mode.type == TokenType::kIdent && IEquals(mode.text, "AUTO")) {
                lexer_.Next();
                col.cabin_policy = catalog::kCabinPolicyAuto;
            } else {
                col.cabin_policy = catalog::kCabinPolicyEnabled;
            }
        } else if (policy.type == TokenType::kIdent && IEquals(policy.text, "NO")) {
            col.cabin_byte_offset = policy.byte_offset;
            lexer_.Next();
            // `NO` is only ever the start of `NO CABIN` here. Saying so
            // beats letting `no` fall through to the trailing-garbage check,
            // which would point at the wrong token.
            if (Status s = ExpectKeyword("CABIN"); !s.ok()) {
                return s.WithContext("the only NO clause on a column is `NO CABIN`");
            }
            col.cabin_policy = catalog::kCabinPolicyDisabled;
        }

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

// ---- CREATE PATTERN / DROP PATTERN ---------------------------------------

Status Parser::ParsePatternParams(CreatePatternStmt& stmt) {
    if (Status s = ExpectToken(TokenType::kLParen, "'(' before the parameter list"); !s.ok()) {
        return s;
    }

    // `()` is legal and means a pattern with exactly one instance - the
    // arg_hash of an empty argument stream. Checked before the loop so the
    // loop never has to handle an empty list.
    if (lexer_.Peek().type == TokenType::kRParen) {
        lexer_.Next();
        return Status::OK();
    }

    for (;;) {
        Token tok = lexer_.Next();
        if (tok.type != TokenType::kNamedParam) {
            return Status::InvalidArgument(
                "expected a $-sigiled parameter name, got '" + std::string(Describe(tok)) +
                "' (byte " + std::to_string(tok.byte_offset) + ")");
        }

        PatternParam param;
        param.name = std::string(tok.text);
        param.byte_offset = tok.byte_offset;

        // Mandatory, and the message says so rather than reporting a
        // generic "expected identifier": an untyped parameter is the one
        // mistake a client writing this grammar for the first time will
        // make, and inference was rejected on purpose (ast.hpp).
        const Token& next = lexer_.Peek();
        if (next.type != TokenType::kIdent) {
            return Status::InvalidArgument(
                "parameter '$" + param.name +
                "' needs a type (e.g. `$" + param.name + " int64`), got '" +
                std::string(Describe(next)) + "' (byte " + std::to_string(next.byte_offset) +
                ")");
        }
        auto type_name = ParseIdent();
        if (!type_name.ok()) return type_name.status();
        param.type_name = std::move(type_name.value());

        stmt.params.push_back(std::move(param));

        if (lexer_.Peek().type == TokenType::kComma) {
            lexer_.Next();
            continue;
        }
        break;
    }

    return ExpectToken(TokenType::kRParen, "')' after the parameter list");
}

Status Parser::ParsePatternOptions(CreatePatternStmt& stmt) {
    if (Status s = ExpectToken(TokenType::kLParen, "'(' after WITH"); !s.ok()) return s;

    for (;;) {
        const Token& key_tok = lexer_.Peek();
        const std::uint32_t at = key_tok.byte_offset;
        auto key = ParseIdent();
        if (!key.ok()) return key.status();

        if (Status s = ExpectToken(TokenType::kEq, "'=' after an option name"); !s.ok()) {
            return s;
        }

        // The value side stays text whatever it looks like: `on`, `off` and
        // `100000` are all just spellings until the validator says which
        // keys take which. An integer is accepted here and range-checked
        // there, where the key that constrains it is known.
        //
        // **kKeyword is in this list, and has to be.** `on` is a reserved
        // word - it is the `ON` of `JOIN ... ON` - so `pinned = on`, the
        // spelling the spec's own example uses, arrives here as a keyword
        // token and not an identifier. An option value is a word in value
        // position, where no reserved word means anything, so reading it by
        // text is right rather than lenient.
        Token val = lexer_.Next();
        std::string value;
        switch (val.type) {
            case TokenType::kIdent:
            case TokenType::kKeyword:
            case TokenType::kIntLit:
            case TokenType::kStrLit: value = std::string(val.text); break;
            default:
                return Status::InvalidArgument("expected a value for option '" + key.value() +
                                                "', got '" + std::string(Describe(val)) +
                                                "' (byte " + std::to_string(val.byte_offset) +
                                                ")");
        }

        stmt.options.push_back(PatternOption{std::move(key.value()), std::move(value), at});

        if (lexer_.Peek().type == TokenType::kComma) {
            lexer_.Next();
            continue;
        }
        break;
    }

    return ExpectToken(TokenType::kRParen, "')' after the option list");
}

StatusOr<CreatePatternStmt> Parser::ParseCreatePattern() {
    CreatePatternStmt stmt;

    stmt.byte_offset = lexer_.Peek().byte_offset;
    auto name = ParseIdent();
    if (!name.ok()) return name.status();
    stmt.name = std::move(name.value());

    if (Status s = ParsePatternParams(stmt); !s.ok()) return s;

    const Token& maybe_with = lexer_.Peek();
    if (maybe_with.type == TokenType::kIdent && IEquals(maybe_with.text, "WITH")) {
        lexer_.Next();
        if (Status s = ParsePatternOptions(stmt); !s.ok()) return s;
    }

    if (Status s = ExpectKeyword("OF"); !s.ok()) {
        return s.WithContext("a pattern declaration is `... [WITH (...)] OF <statement>`");
    }

    // The body starts here and runs to end of statement, so its text is a
    // suffix of the input and slicing it needs no closing delimiter to find.
    // That is the whole reason WITH precedes OF (ast.hpp).
    const std::size_t body_start = lexer_.Peek().byte_offset;

    if (Status s = ExpectKeyword("SELECT"); !s.ok()) {
        return s.WithContext(
            "only SELECT-class statements can be declared as patterns in v1");
    }

    // `$x` becomes legal exactly here and illegal again on the way out,
    // including on the error paths - hence the restore before every return.
    stmt.param_uses.clear();
    param_uses_ = &stmt.param_uses;
    auto body = ParseSelect(/*depth=*/0);
    param_uses_ = nullptr;
    if (!body.ok()) return body.status();
    stmt.body = std::make_shared<SelectStmt>(std::move(body.value()));

    // Both slices are verbatim, minus the trailing semicolon ParseSelect
    // already consumed and any whitespace after it. The fingerprint skips a
    // semicolon anyway, so trimming changes no hash; it keeps the stored
    // text from ending in punctuation the operator did not think of as part
    // of what they wrote.
    //
    // Two slices of one string, and they are not interchangeable: the whole
    // declaration is the canon that goes to sys.pattern_defs, the body is
    // what gets hashed. `Parse()` refuses trailing garbage, so the statement
    // is exactly the input and needs no end offset to find.
    auto trim_tail = [](std::string_view text) {
        while (!text.empty() && (text.back() == ';' ||
                                 std::isspace(static_cast<unsigned char>(text.back())) != 0)) {
            text.remove_suffix(1);
        }
        return text;
    };
    stmt.source_text = std::string(trim_tail(sql_));
    stmt.body_text = std::string(trim_tail(sql_.substr(body_start)));

    return stmt;
}

StatusOr<DropPatternStmt> Parser::ParseDropPattern() {
    DropPatternStmt stmt;
    stmt.byte_offset = lexer_.Peek().byte_offset;

    auto name = ParseIdent();
    if (!name.ok()) return name.status();
    stmt.name = std::move(name.value());

    ConsumeOptionalSemicolon();
    return stmt;
}

StatusOr<CabinStmt> Parser::ParseCabin(bool drop) {
    CabinStmt stmt;
    stmt.drop = drop;

    // `ON` is a reserved keyword (it joins), so it does not arrive as an
    // identifier and ExpectKeyword cannot be used for it.
    const Token& on = lexer_.Peek();
    if (on.type != TokenType::kKeyword || on.kw != Keyword::kOn) {
        return Status::InvalidArgument(
            std::string(drop ? "DROP" : "CREATE") +
            " CABIN names the column it is on: `... CABIN ON <table>(<column>)` (byte " +
            std::to_string(on.byte_offset) + ")");
    }
    lexer_.Next();

    stmt.byte_offset = lexer_.Peek().byte_offset;
    auto table = ParseIdent();
    if (!table.ok()) return table.status();
    stmt.table_name = std::move(table.value());

    if (Status s = ExpectToken(TokenType::kLParen, "'(' before the cabin's column"); !s.ok()) {
        return s;
    }

    stmt.column_byte_offset = lexer_.Peek().byte_offset;
    auto column = ParseIdent();
    if (!column.ok()) return column.status();
    stmt.column_name = std::move(column.value());

    // **One column, and the refusal is here rather than at the catalog.**
    // C3 keeps multi-column keys out of v1, and a comma is exactly what an
    // operator writes when they expect a composite - so it gets an answer
    // that says which decision it is waiting on, not "expected ')'".
    if (lexer_.Peek().type == TokenType::kComma) {
        return Status::Unsupported("a cabin covers one column in v1 (byte " +
                                    std::to_string(lexer_.Peek().byte_offset) +
                                    "); multi-column keys are out of scope by C3");
    }

    if (Status s = ExpectToken(TokenType::kRParen, "')' after the cabin's column"); !s.ok()) {
        return s;
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

StatusOr<RelationRef> Parser::ParseRelationRef() {
    const std::uint32_t offset = lexer_.Peek().byte_offset;

    // The structural rule that keeps table-position nesting out (spec
    // section 2): **this production must never reach the statement
    // production**. A derived table is refused here, by the relation
    // reference itself, rather than by failing to parse somewhere deeper -
    // which is what makes the position exact and the answer truthful.
    //
    // Why it stays out, since the error should not have to explain it: a
    // derived table's result must become a relation with a schema,
    // materialized somewhere and probed by something other than a pk.
    // That breaks pk-direct probing into the next step, which is the
    // shape of the whole execution model, and puts a temporary relation
    // in the storage layer.
    if (lexer_.Peek().type == TokenType::kLParen) {
        return Status::Unsupported(
            "a subquery cannot appear in FROM (byte " + std::to_string(offset) +
            "); derived tables and CTEs are not supported, only predicate-position subqueries");
    }

    auto name = ParseIdent();
    if (!name.ok()) return name.status();

    RelationRef rel;
    rel.table_name = std::move(name.value());
    rel.byte_offset = offset;

    // `sys.tables` - a schema-qualified relation. The dot is unambiguous
    // here: a relation reference is a single name, so nothing else can
    // follow one through a dot. Which schemas exist is the catalog's
    // question, not this production's; an unknown one fails at resolution
    // with a message that can name what does exist.
    if (lexer_.Peek().type == TokenType::kDot) {
        lexer_.Next();
        auto qualified = ParseIdent();
        if (!qualified.ok()) return qualified.status();
        rel.schema = std::move(rel.table_name);
        rel.table_name = std::move(qualified.value());
    }

    // `AS <alias>`. The bare-alias form (`FROM t a`) is deliberately not
    // accepted: it makes a typo'd keyword read as an alias, and the two
    // spellings would be one more thing to keep converging.
    const Token& peek = lexer_.Peek();
    if (peek.type == TokenType::kKeyword && peek.kw == Keyword::kAs) {
        lexer_.Next();
        auto alias = ParseIdent();
        if (!alias.ok()) return alias.status();
        rel.alias = std::move(alias.value());
    }
    return rel;
}

StatusOr<ColumnName> Parser::ParseColumnName() {
    const std::uint32_t offset = lexer_.Peek().byte_offset;

    auto first = ParseIdent();
    if (!first.ok()) return first.status();

    ColumnName out;
    out.byte_offset = offset;

    // `a.x` or bare `x`. The dot decides, and it can only mean a
    // qualifier: there is no other production in this grammar where one
    // identifier follows another through a dot.
    if (lexer_.Peek().type == TokenType::kDot) {
        lexer_.Next();
        auto column = ParseIdent();
        if (!column.ok()) return column.status();
        out.qualifier = std::move(first.value());
        out.name = std::move(column.value());
    } else {
        out.name = std::move(first.value());
    }
    return out;
}

StatusOr<ColumnName> Parser::ParseQualifiedColumn() {
    auto col = ParseColumnName();
    if (!col.ok()) return col.status();

    // Unqualified is refused here rather than resolved. `ON id = id`
    // names no relation and there is no reading of it that is not a
    // guess; when unqualified names do resolve (V14's compiler, against
    // the FROM list) this can loosen, and until then a truthful refusal
    // beats a silent choice.
    if (!col.value().qualified()) {
        return Status::Unsupported("ON requires a qualified column (`rel.col`), got '" +
                                    col.value().name + "' at byte " +
                                    std::to_string(col.value().byte_offset) +
                                    "; unqualified names in ON are not resolved");
    }
    return col;
}

Status Parser::ParseSelectList(SelectStmt& stmt) {
    // `SELECT *` leaves the projection empty. It stays available for a
    // single relation, where it is unambiguous and is what almost every
    // statement in the corpus says.
    if (lexer_.Peek().type == TokenType::kStar) {
        lexer_.Next();
        return Status::OK();
    }

    for (;;) {
        auto col = ParseColumnName();
        if (!col.ok()) return col.status();
        stmt.projection.push_back(std::move(col.value()));

        if (lexer_.Peek().type != TokenType::kComma) return Status::OK();
        lexer_.Next();
    }
}

// No `depth`: a join's relations are relation references, and a relation
// reference is never a subquery (ParseRelationRef refuses one outright).
Status Parser::ParseJoins(SelectStmt& stmt) {
    for (;;) {
        const Token& peek = lexer_.Peek();
        if (peek.type != TokenType::kKeyword) return Status::OK();

        // An outer join is a form this engine understands and declines,
        // which is what Unsupported means (docs/parser-v2.md J2) - as
        // opposed to a syntax error, which would send a client looking
        // for a typo. The position is the keyword's own.
        if (peek.kw == Keyword::kLeft || peek.kw == Keyword::kRight ||
            peek.kw == Keyword::kFull || peek.kw == Keyword::kOuter) {
            return Status::Unsupported("outer joins are not supported: '" +
                                       std::string(peek.text) +
                                        "' at byte " + std::to_string(peek.byte_offset) +
                                        "; only inner equi-joins (JOIN ... ON) are available");
        }
        if (peek.kw != Keyword::kJoin) return Status::OK();
        lexer_.Next();  // consume JOIN

        JoinClause join;
        auto rel = ParseRelationRef();
        if (!rel.ok()) return rel.status();
        join.relation = std::move(rel.value());

        // ON is mandatory: a JOIN without one is a cross join written by
        // accident, and this grammar has no cross join to mean.
        const Token& on = lexer_.Peek();
        if (on.type != TokenType::kKeyword || on.kw != Keyword::kOn) {
            return Status::InvalidArgument("expected ON after JOIN, got '" +
                                            std::string(Describe(on)) + "'");
        }
        lexer_.Next();

        auto left = ParseQualifiedColumn();
        if (!left.ok()) return left.status();
        join.left = std::move(left.value());

        if (Status s = ExpectToken(TokenType::kEq, "'=' in an ON clause"); !s.ok()) return s;

        auto right = ParseQualifiedColumn();
        if (!right.ok()) return right.status();
        join.right = std::move(right.value());

        stmt.joins.push_back(std::move(join));
    }
}

Status Parser::CheckDistinctBindings(const SelectStmt& stmt) const {
    // O(n^2) over a FROM list that the depth cap keeps tiny, and it
    // preserves written order in the error message - which a set would
    // not, and which is what makes the message point at the *second*
    // occurrence rather than an arbitrary one.
    std::vector<const RelationRef*> rels;
    rels.push_back(&stmt.from);
    for (const JoinClause& j : stmt.joins) rels.push_back(&j.relation);

    for (std::size_t i = 1; i < rels.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (!IEquals(rels[i]->binding(), rels[j]->binding())) continue;
            // Unsupported, not InvalidArgument: the statement is
            // well-formed and has a meaning in standard SQL. This engine
            // declines to guess which occurrence a predicate meant, and
            // says so with the position of the second one. Adding
            // distinct aliases is the fix, and it is also what makes a
            // self-join expressible instead of accidental.
            return Status::Unsupported(
                "relation '" + rels[i]->binding() + "' at byte " +
                std::to_string(rels[i]->byte_offset) +
                " is named twice in the FROM list; give each occurrence a distinct alias "
                "(`AS a`, `AS b`) so a predicate can say which one it means");
        }
    }
    return Status::OK();
}

StatusOr<SelectStmt> Parser::ParseSelect(std::uint32_t depth) {
    SelectStmt stmt;

    // The select list is read before the FROM list, because that is the
    // order it is written in - so whether `*` is ambiguous cannot be
    // known until the relations have been read, and the check waits until
    // both are in hand.
    const std::uint32_t star_at = lexer_.Peek().byte_offset;
    if (Status s = ParseSelectList(stmt); !s.ok()) return s;

    if (Status s = ExpectKeyword("FROM"); !s.ok()) return s;

    auto from = ParseRelationRef();
    if (!from.ok()) return from.status();
    stmt.from = std::move(from.value());

    if (Status s = ParseJoins(stmt); !s.ok()) return s;
    if (Status s = CheckDistinctBindings(stmt); !s.ok()) return s;

    // `SELECT *` over more than one relation. Which columns it means, and
    // in what order, would be a property of how the relations were joined
    // - and written order being the client's to choose (spec section 1) is
    // exactly what makes that unanswerable here. Unsupported rather than
    // InvalidArgument: the statement is well-formed, and naming the
    // columns is the fix.
    if (stmt.star() && stmt.relation_count() > 1) {
        return Status::Unsupported(
            "SELECT * is ambiguous across " + std::to_string(stmt.relation_count()) +
            " relations (byte " + std::to_string(star_at) +
            "); name the columns you want, qualified (`a.x, b.y`)");
    }

    auto where = ParseOptionalWhere(depth);
    if (!where.ok()) return where.status();
    stmt.where = std::move(where.value());

    // Only the outermost block may be followed by a semicolon. A nested
    // one ends at its ')', and swallowing a ';' inside it would accept
    // `WHERE EXISTS (SELECT * FROM u;)` - which is not a statement
    // boundary, and reads as one only because this parser happens to
    // treat the semicolon as optional.
    if (depth == 0) ConsumeOptionalSemicolon();
    return stmt;
}

StatusOr<DeleteStmt> Parser::ParseDelete() {
    DeleteStmt stmt;

    if (Status s = ExpectKeyword("FROM"); !s.ok()) return s;

    auto name = ParseIdent();
    if (!name.ok()) return name.status();
    stmt.table_name = std::move(name.value());

    // Depth 0 and the same production UPDATE uses: a DELETE's WHERE is an
    // outermost query block, and a predicate-position subquery in it nests
    // exactly as a SELECT's does.
    auto where = ParseOptionalWhere(/*depth=*/0);
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

    // Depth 0: an UPDATE's WHERE is an outermost query block, and a
    // subquery in it is `[OPEN: revisit]` in spec section 2 only for the
    // *value* position (`SET a = (SELECT ...)`), which SET does not
    // parse. A predicate-position subquery here nests exactly as a
    // SELECT's does.
    auto where = ParseOptionalWhere(/*depth=*/0);
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
        return Status::InvalidArgument("expected SQL keyword, got '" + std::string(tok.text) +
                                        "'");
    }

    Statement stmt;

    if (IEquals(tok.text, "CREATE")) {
        // One token of lookahead picks the object. `TABLE` still goes
        // through ParseCreateTable's own ExpectKeyword, so the error a
        // client gets for `CREATE INDEX` is unchanged.
        const Token& what = lexer_.Peek();
        if (what.type == TokenType::kIdent && IEquals(what.text, "PATTERN")) {
            lexer_.Next();
            auto s = ParseCreatePattern();
            if (!s.ok()) return s.status();
            stmt = std::move(s.value());
        } else if (what.type == TokenType::kIdent && IEquals(what.text, "CABIN")) {
            lexer_.Next();
            auto s = ParseCabin(/*drop=*/false);
            if (!s.ok()) return s.status();
            stmt = std::move(s.value());
        } else {
            auto s = ParseCreateTable();
            if (!s.ok()) return s.status();
            stmt = std::move(s.value());
        }
    } else if (IEquals(tok.text, "DROP")) {
        // Patterns and cabins can be dropped. There is still no DROP TABLE,
        // and saying so here beats a syntax error that points at the table
        // name - the list in the message is the whole of what exists.
        const Token& what = lexer_.Peek();
        if (what.type == TokenType::kIdent && IEquals(what.text, "CABIN")) {
            lexer_.Next();
            auto s = ParseCabin(/*drop=*/true);
            if (!s.ok()) return s.status();
            stmt = std::move(s.value());
        } else if (what.type == TokenType::kIdent && IEquals(what.text, "PATTERN")) {
            lexer_.Next();
            auto s = ParseDropPattern();
            if (!s.ok()) return s.status();
            stmt = std::move(s.value());
        } else {
            return Status::Unsupported("only DROP PATTERN and DROP CABIN are supported (byte " +
                                        std::to_string(what.byte_offset) + ")");
        }
    } else if (IEquals(tok.text, "INSERT")) {
        auto s = ParseInsert();
        if (!s.ok()) return s.status();
        stmt = std::move(s.value());
    } else if (IEquals(tok.text, "SELECT")) {
        auto s = ParseSelect(/*depth=*/0);
        if (!s.ok()) return s.status();
        stmt = std::move(s.value());
    } else if (IEquals(tok.text, "UPDATE")) {
        auto s = ParseUpdate();
        if (!s.ok()) return s.status();
        stmt = std::move(s.value());
    } else if (IEquals(tok.text, "DELETE")) {
        auto s = ParseDelete();
        if (!s.ok()) return s.status();
        stmt = std::move(s.value());
    } else if (IEquals(tok.text, "WITH")) {
        // The other half of the structural rule (spec section 2): `WITH`
        // must not lex as a statement head. It is answered here, by name,
        // rather than falling into "unknown SQL keyword" - a CTE is a form
        // this engine understands and declines for the reason
        // ParseRelationRef gives, not a word it has never heard of.
        return Status::Unsupported("common table expressions (WITH) are not supported (byte " +
                                    std::to_string(tok.byte_offset) +
                                    "); subqueries are allowed in predicate position only");
    } else {
        return Status::InvalidArgument("unknown SQL keyword '" + std::string(tok.text) +
                                        "' (supported: CREATE, DROP, INSERT, SELECT, UPDATE, DELETE)");
    }

    // After a successful parse, only EOF may remain (a trailing semicolon
    // was already consumed by the statement parser itself).
    const Token& tail = lexer_.Peek();
    if (tail.type != TokenType::kEof) {
        return Status::InvalidArgument("unexpected token '" + std::string(tail.text) +
                                        "' after end of statement");
    }

    return stmt;
}

StatusOr<Statement> Parse(std::string_view sql) { return Parser(sql).Parse(); }

}  // namespace kds::parser
