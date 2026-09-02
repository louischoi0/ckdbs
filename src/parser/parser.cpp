#include "kds/parser/parser.hpp"

#include "kds/catalog/rows.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

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

// The aggregate `name` spells, or false if it spells none.
//
// `AVG` was deliberately absent until 2026-08-07, when aggregate.md
// §10's open question was decided (see AggFunc's note in ast.hpp); the
// grammar half is now ordinary and the type half - decimal columns only -
// is the compiler's `CheckAggregateArgType`, where every other per-type
// aggregate rule already lives.
bool AggFuncOf(std::string_view name, AggFunc& out) noexcept {
    if (IEquals(name, "COUNT")) { out = AggFunc::kCount; return true; }
    if (IEquals(name, "SUM")) { out = AggFunc::kSum; return true; }
    if (IEquals(name, "MIN")) { out = AggFunc::kMin; return true; }
    if (IEquals(name, "MAX")) { out = AggFunc::kMax; return true; }
    if (IEquals(name, "AVG")) { out = AggFunc::kAvg; return true; }
    return false;
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
        // **The byte, on every refusal this function produces.** CLAUDE.md's
        // rule is engine-wide - "every refusal carries the byte position of
        // the offending token" - and this one function is the shortest path
        // between a grammar production and a message, so a production that
        // routes its arity error through it used to lose the position
        // silently. `char(8, 1)` and `decimal(8)` were both landing here
        // without one (the phase-A review's C-1); fixing it here rather
        // than at each call site is what keeps a future production from
        // inheriting the same gap.
        return Status::InvalidArgument("expected " + std::string(desc) + ", got '" +
                                        std::string(Describe(tok)) + "' at byte " +
                                        std::to_string(tok.byte_offset));
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

Status Parser::ParseQualifiedName(std::string& schema, std::string& name,
                                  std::uint32_t* offset) {
    if (offset != nullptr) *offset = lexer_.Peek().byte_offset;

    auto first = ParseIdent();
    if (!first.ok()) return first.status();
    name = std::move(first.value());
    schema.clear();

    // The dot is unambiguous here: a relation reference is a single name,
    // so nothing else can follow one through a dot. Whether the qualifier
    // names a namespace that exists is resolution's question, not this
    // production's - see the declaration.
    if (lexer_.Peek().type != TokenType::kDot) return Status::OK();
    lexer_.Next();

    auto qualified = ParseIdent();
    if (!qualified.ok()) {
        return qualified.status().WithContext("a namespace qualifier names a relation after it");
    }
    schema = std::move(name);
    name = std::move(qualified.value());
    return Status::OK();
}

StatusOr<std::uint32_t> Parser::ParseTypeArgument(std::string_view what) {
    const Token tok = lexer_.Next();
    if (tok.type != TokenType::kIntLit || tok.negative) {
        return Status::InvalidArgument("expected a non-negative integer " + std::string(what) +
                                        ", got '" + std::string(Describe(tok)) + "' at byte " +
                                        std::to_string(tok.byte_offset));
    }
    // The *bounds* are the type registry's business (TY2), not this
    // layer's; what a syntax layer can say is that the digits fit at all.
    if (tok.int_val < 0 || tok.int_val > 0xFFFF) {
        return Status::InvalidArgument(std::string(what) + " " + std::to_string(tok.int_val) +
                                        " is implausible at byte " +
                                        std::to_string(tok.byte_offset));
    }
    return static_cast<std::uint32_t>(tok.int_val);
}

void Parser::ConsumeOptionalSemicolon() {
    if (lexer_.Peek().type == TokenType::kSemicolon) {
        lexer_.Next();
    }
}

StatusOr<AstValue> Parser::ParseValue() {
    Token tok = lexer_.Next();

    // Every literal carries where it was written, not just `$param`.
    // A literal is now something a *later* stage can reject - the step
    // compiler coerces one against its column's type (types.md §3.1),
    // and `WHERE d = '2026-02-30'` fails there, long after the token is
    // gone. Without the offset that failure can only say which column it
    // was about; with it, it can point at the byte.
    //
    // Nothing downstream compares it: chain identity renders operand
    // *values* (the aggregate contract suite's RenderOperand), Cabin keys
    // are built from the value's kind and contents, and the fingerprint is
    // folded from tokens. So this adds a fact to error messages and
    // changes no plan, no `pattern_id`, and no comparison.
    switch (tok.type) {
        case TokenType::kIntLit: {
            AstValue v;
            v.type = ValueType::kInt;
            v.int_val = tok.int_val;
            v.raw_int_text = std::string(tok.text);  // full-range digits; see ast.hpp
            v.byte_offset = tok.byte_offset;
            return v;
        }
        case TokenType::kStrLit: {
            AstValue v;
            v.type = ValueType::kStr;
            v.str_val = std::string(tok.text);
            v.byte_offset = tok.byte_offset;
            return v;
        }
        case TokenType::kNumLit: {
            // Deliberately the kStrLit arm's value, byte for byte: a bare
            // numeric is sugar for the quoted string of its spelling
            // (token.hpp), so `= 12.34` and `= '12.34'` produce one AST and
            // every stage past this line has exactly one case to be right
            // about. The column's type gives it meaning at the same gate
            // the quoted form goes through - `CoerceLiteralToColumn` for a
            // predicate, `EncodeOneValue` for an INSERT - and a column no
            // string satisfies (an integer one) refuses or misses exactly
            // as it would the quoted form.
            AstValue v;
            v.type = ValueType::kStr;
            v.str_val = std::string(tok.text);
            v.byte_offset = tok.byte_offset;
            return v;
        }
        case TokenType::kNullLit: {
            AstValue v;
            v.type = ValueType::kNull;
            v.byte_offset = tok.byte_offset;
            return v;
        }
        case TokenType::kNamedParam:
            // Accepted by **no** production. It was legal in exactly one
            // place - the body of a `CREATE PATTERN` - and the operator
            // withdrew that grammar on 2026-08-31, so `$x` now reaches this
            // refusal wherever it is written. The token still lexes and
            // still fingerprints as a value (token.hpp): it stays reserved
            // for the extended protocol's named binds (D4), and refusing it
            // by name and position is what tells a client that, rather than
            // leaving them to read "expected value" and guess the sigil was
            // a typo.
            return Status::Unsupported("parameter '$" + std::string(tok.text) +
                                       "' is not bindable in the newline protocol (byte " +
                                       std::to_string(tok.byte_offset) + ")");
        default:
            return Status::InvalidArgument(
                "expected value (integer, number, 'string', or NULL), got '" +
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
                return Status::NotImplemented(
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
                return Status::NotImplemented(
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

    // `col IS [NOT] NULL` (docs/spec/null.md; NU5). `IS` is contextual -
    // an unreserved word like REFERENCES, so it still names a column
    // everywhere else and the fingerprint hashes it as the identifier it
    // lexes as. No right-hand side: the op is the whole predicate.
    if (const Token& is_tok = lexer_.Peek();
        is_tok.type == TokenType::kIdent && IEquals(is_tok.text, "IS")) {
        lexer_.Next();
        bool negated = false;
        if (lexer_.Peek().type == TokenType::kKeyword && lexer_.Peek().kw == Keyword::kNot) {
            lexer_.Next();
            negated = true;
        }
        if (lexer_.Peek().type != TokenType::kNullLit) {
            return Status::InvalidArgument("expected NULL after IS (byte " +
                                            std::to_string(lexer_.Peek().byte_offset) + ")");
        }
        lexer_.Next();
        cond.op = negated ? CompareOp::kIsNotNull : CompareOp::kIsNull;
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

    // The one qualifier in the grammar that *decides* something: it names
    // the namespace the relation is created in, and through AF-T2 the core
    // that will own it.
    if (Status s = ParseQualifiedName(stmt.schema, stmt.table_name, &stmt.table_byte_offset);
        !s.ok()) {
        return s;
    }

    if (Status s = ExpectToken(TokenType::kLParen, "'('"); !s.ok()) return s;

    for (;;) {
        ColumnDef col;

        auto col_name = ParseIdent();
        if (!col_name.ok()) return col_name.status();
        col.name = std::move(col_name.value());

        col.type_byte_offset = lexer_.Peek().byte_offset;
        auto type_name = ParseIdent();
        if (!type_name.ok()) return type_name.status();
        col.type_name = std::move(type_name.value());

        // The types whose declaration carries arguments: `DECIMAL(p, s)`
        // takes two (docs/spec/types.md §2), `CHAR(n)` and `VARCHAR(n)` one
        // (`instructions/v2.5.0/varchar-char-architecture.md` §3).
        // Recognized by the paren rather than by the name, so a type that
        // takes no arguments refuses them here instead of each type name
        // needing its own production - and the arity is decided by the name
        // *inside* that branch, so `char(8, 1)` fails on the comma with a
        // byte rather than on a count nobody wrote.
        if (lexer_.Peek().type == TokenType::kLParen) {
            const std::uint32_t paren_at = lexer_.Peek().byte_offset;
            const bool takes_precision = IEquals(col.type_name, "DECIMAL");
            const bool takes_width =
                IEquals(col.type_name, "CHAR") || IEquals(col.type_name, "VARCHAR");
            if (!takes_precision && !takes_width) {
                return Status::InvalidArgument("type '" + col.type_name +
                                                "' takes no arguments (byte " +
                                                std::to_string(paren_at) + ")");
            }
            lexer_.Next();  // consume '('
            col.type_arg_byte_offset = lexer_.Peek().byte_offset;

            if (takes_width) {
                auto width = ParseTypeArgument("width");
                if (!width.ok()) return width.status();
                if (Status s = ExpectToken(TokenType::kRParen,
                                           "')' after a character type's width");
                    !s.ok()) {
                    return s;
                }
                col.has_width = true;
                col.width = width.value();
            } else {
                auto precision = ParseTypeArgument("precision");
                if (!precision.ok()) return precision.status();
                if (Status s = ExpectToken(TokenType::kComma,
                                           "',' between a decimal's precision and scale");
                    !s.ok()) {
                    return s;
                }
                auto scale = ParseTypeArgument("scale");
                if (!scale.ok()) return scale.status();
                if (Status s = ExpectToken(TokenType::kRParen, "')' after a decimal's scale");
                    !s.ok()) {
                    return s;
                }

                col.has_precision = true;
                col.precision = precision.value();
                col.scale = scale.value();
            }
        } else if (IEquals(col.type_name, "DECIMAL")) {
            // **A bare `decimal` is refused, never defaulted.** A default
            // scale is a silent decision about someone's money, and the
            // parser is the only layer that can still tell the difference
            // between "said nothing" and "said zero".
            return Status::InvalidArgument(
                "column '" + col.name + "' needs a precision and a scale - `decimal(p, s)` - "
                "at byte " + std::to_string(col.type_byte_offset) +
                "; there is no default scale, because one would be a silent decision about "
                "what a stored value means");
        }

        // Optional nullability, directly after the type and before every
        // other suffix (docs/spec/null.md §2.3, D1): `NULL` opts a column
        // into nullability; `NOT NULL` spells the default for
        // standard-minded schemas and changes nothing. First in the suffix
        // chain because it modifies the type, not the column's relations.
        if (lexer_.Peek().type == TokenType::kNullLit) {
            col.null_byte_offset = lexer_.Peek().byte_offset;
            lexer_.Next();
            col.notnull = false;
        } else if (lexer_.Peek().type == TokenType::kKeyword &&
                   lexer_.Peek().kw == Keyword::kNot) {
            lexer_.Next();
            if (lexer_.Peek().type != TokenType::kNullLit) {
                return Status::InvalidArgument(
                    "expected NULL after NOT in a column declaration (byte " +
                    std::to_string(lexer_.Peek().byte_offset) + ")");
            }
            lexer_.Next();
            col.notnull = true;  // the default, said out loud
        }

        // Optional `REFERENCES <table>` (docs/spec/foreign-keys.md §1).
        // Peeked like the cabin clause below it, and written *before* it
        // when both appear - a fixed order, because two optional suffixes
        // accepted in either order is a grammar with a shape nobody can
        // state, and the fingerprint would have to hash both spellings of
        // one declaration.
        if (const Token& refs = lexer_.Peek();
            refs.type == TokenType::kIdent && IEquals(refs.text, "REFERENCES")) {
            col.references_byte_offset = refs.byte_offset;
            lexer_.Next();

            if (Status s = ParseQualifiedName(col.references_schema, col.references_table);
                !s.ok()) {
                return s.WithContext("REFERENCES names the parent relation");
            }

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
        // (docs/spec/cabin.md). Peeked rather than required, so every
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

    // Optional trailing words: the storage clause (HEAP | BTREE) and the
    // vestigial key-mode word (docs/spec/heap-and-tuple.md §4.1). Both are facts
    // about the whole relation, both are matched as identifiers and neither
    // is reserved, so this is one loop over bare words rather than two
    // peeks. Order between the two categories is free - unlike a column's
    // suffixes, these are not two clauses on one thing whose spellings would
    // both have to hash, since CREATE is not patternable. Anything else is
    // left untouched for the trailing-garbage check at the top level.
    //
    // The key-mode words are what the 2026-08-25 removal left behind:
    //
    //   `EXPLICIT` is **accepted and does nothing**. It used to select a
    //   mode; it now states what is true of every relation - the caller may
    //   name this relation's keys - so accepting it keeps written SQL
    //   working and says nothing false. It sets no field because there is no
    //   field left to set.
    //
    //   `ASSIGNED` is **refused**, `Unsupported` with its byte. Accepting it
    //   would be accepting a spelling and enforcing something other than
    //   what was written (CLAUDE.md's truthfulness rule): the word means
    //   "the engine issues every id and supplying one is refused", and on
    //   the relation this statement creates, supplying one is admitted.
    //   Ignoring it would be worse than refusing it.
    bool storage_given = false;
    bool key_word_given = false;
    for (;;) {
        const Token word = lexer_.Peek();
        if (word.type != TokenType::kIdent) break;

        const bool is_heap = IEquals(word.text, "HEAP");
        const bool is_btree = IEquals(word.text, "BTREE");
        const bool is_assigned = IEquals(word.text, "ASSIGNED");
        const bool is_explicit = IEquals(word.text, "EXPLICIT");
        if (!is_heap && !is_btree && !is_assigned && !is_explicit) break;

        // A category given twice is refused rather than last-one-wins:
        // `HEAP BTREE` names two different relations, so silently keeping
        // one of them would be the parser deciding what the writer meant.
        if (is_heap || is_btree) {
            if (storage_given) {
                return Status::InvalidArgument(
                    "CREATE TABLE takes one storage word - HEAP or BTREE - and this is the "
                    "second (byte " +
                    std::to_string(word.byte_offset) + ")");
            }
            storage_given = true;
            stmt.clustered_given = true;
            stmt.clustered = is_heap ? catalog::ClusteredType::kHeap
                                     : catalog::ClusteredType::kBtree;
        } else if (is_assigned) {
            return Status::Unsupported(
                "the ASSIGNED key mode no longer exists (byte " +
                std::to_string(word.byte_offset) +
                ") - every relation takes a caller-supplied primary key or issues one when "
                "INSERT omits it, so there is nothing for this word to select");
        } else {
            if (key_word_given) {
                return Status::InvalidArgument(
                    "CREATE TABLE takes one EXPLICIT and this is the second (byte " +
                    std::to_string(word.byte_offset) + ")");
            }
            key_word_given = true;
        }

        lexer_.Next();
    }

    ConsumeOptionalSemicolon();
    return stmt;
}

// `CREATE PATTERN` / `DROP PATTERN` and their two bracketed-list helpers
// stood here until 2026-08-31, when the operator withdrew user-declared
// patterns (`docs/spec/create-pattern-user-defined-patterns-v1.md`, marked
// withdrawn and kept as the design record). Nothing took their place: the
// `$param` production above is now a refusal, and a pattern is what the
// fingerprint and `sys.patterns` say it is.

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

    if (Status s = ParseQualifiedName(stmt.schema, stmt.table_name, &stmt.byte_offset);
        !s.ok()) {
        return s;
    }

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
        return Status::NotImplemented("a cabin covers one column in v1 (byte " +
                                    std::to_string(lexer_.Peek().byte_offset) +
                                    "); multi-column keys are out of scope by C3");
    }

    if (Status s = ExpectToken(TokenType::kRParen, "')' after the cabin's column"); !s.ok()) {
        return s;
    }

    ConsumeOptionalSemicolon();
    return stmt;
}

Status Parser::ParseDeclaredColumnList(std::vector<IndexColumnRef>& out, const char* what,
                                       std::size_t cap) {
    if (Status s = ExpectToken(TokenType::kLParen,
                                (std::string("'(' before the ") + what).c_str());
        !s.ok()) {
        return s;
    }

    for (;;) {
        IndexColumnRef col;
        col.byte_offset = lexer_.Peek().byte_offset;
        auto name = ParseIdent();
        if (!name.ok()) return name.status();
        col.name = std::move(name.value());

        // **The cap is refused here, with a position, and again in the
        // catalog without one.** Not a duplicated check: this is the only
        // layer that knows *where* the offending column was written, and
        // `Catalog::CreateIndex` is the door every non-parser caller comes
        // through. A cap refuses and never truncates (spec §11) - a
        // truncated index declared complete is a wrong answer with a right
        // answer's shape.
        if (cap != 0 && out.size() == cap) {
            return Status::NotImplemented("at most " + std::to_string(cap) + " " + what +
                                        " are supported (byte " +
                                        std::to_string(col.byte_offset) + ")");
        }
        out.push_back(std::move(col));

        if (lexer_.Peek().type != TokenType::kComma) break;
        lexer_.Next();
    }

    return ExpectToken(TokenType::kRParen, (std::string("')' after the ") + what).c_str());
}

StatusOr<IndexStmt> Parser::ParseIndex(bool drop) {
    IndexStmt stmt;
    stmt.drop = drop;

    stmt.byte_offset = lexer_.Peek().byte_offset;
    auto name = ParseIdent();
    if (!name.ok()) return name.status();
    stmt.index_name = std::move(name.value());

    // An index's name is unique instance-wide, so DROP names nothing else.
    // Naming its relation again would be a second identity to keep in step
    // with the first.
    if (drop) {
        ConsumeOptionalSemicolon();
        return stmt;
    }

    // `ON` is a reserved keyword (it joins), so it arrives as a keyword
    // token and ExpectKeyword cannot be used for it - the same wrinkle
    // ParseCabin has.
    const Token& on = lexer_.Peek();
    if (on.type != TokenType::kKeyword || on.kw != Keyword::kOn) {
        return Status::InvalidArgument(
            "CREATE INDEX names the relation it is on: `CREATE INDEX <name> ON "
            "<table>(<column>, ...)` (byte " +
            std::to_string(on.byte_offset) + ")");
    }
    lexer_.Next();

    if (Status s = ParseQualifiedName(stmt.schema, stmt.table_name, &stmt.table_byte_offset);
        !s.ok()) {
        return s;
    }

    if (Status s = ParseDeclaredColumnList(stmt.key_columns, "index key columns",
                                           catalog::kMaxIndexKeyColumns);
        !s.ok()) {
        return s;
    }

    // `COVERING` is optional and peeked, exactly as the cabin policy clause
    // is: an index without one is the ordinary case and must not have to
    // spell anything.
    const Token& covering = lexer_.Peek();
    if (covering.type == TokenType::kIdent && IEquals(covering.text, "COVERING")) {
        lexer_.Next();
        if (Status s = ParseDeclaredColumnList(stmt.covered_columns, "covered columns",
                                               catalog::kMaxIndexCoveredColumns);
            !s.ok()) {
            return s;
        }
    }

    ConsumeOptionalSemicolon();
    return stmt;
}

// `{CREATE | DROP} NAMESPACE <name>` (AF-T3, AF-6's operator-taken shape
// (a)), with both leading words already consumed.
//
// `NAMESPACE` is matched by text like every clause head this grammar has
// grown, so **nothing is reserved by it**: a column may still be named
// `namespace`, and `kFingerprintVersion` does not move - a keyword hashes
// exactly as an identifier does (fingerprint.hpp's bump rule, which asks
// whether an *already fingerprintable* statement would hash differently;
// this statement did not parse at all before).
//
// There is no `IF NOT EXISTS`: implicit or absorbed creation is exactly
// what made shape (b) lose, because it leaves a typo indistinguishable from
// an intent.
StatusOr<NamespaceStmt> Parser::ParseNamespace(bool drop) {
    NamespaceStmt stmt;
    stmt.drop = drop;

    stmt.byte_offset = lexer_.Peek().byte_offset;
    auto name = ParseIdent();
    if (!name.ok()) return name.status();
    stmt.name = std::move(name.value());

    // A namespace is not in a namespace - its own row says so by carrying
    // its own oid (`catalog.cpp`'s CreateNamespace) - so a qualifier here
    // is a mistake worth naming rather than a form to accept and ignore.
    if (lexer_.Peek().type == TokenType::kDot) {
        return Status::InvalidArgument(
            "a namespace name is not qualified (byte " +
            std::to_string(lexer_.Peek().byte_offset) +
            "); namespaces do not nest - a namespace's own namespace is itself");
    }

    ConsumeOptionalSemicolon();
    return stmt;
}

// `{CREATE | DROP} ASSERTION ...` (docs/spec/assertion.md §3).
//
// **Every refusal in here is a create-time refusal by design** (AS2, §3.1):
// the grammar *is* the supported predicate class, so a form outside the class
// is answered here, with the byte that caused it, instead of being accepted
// and then discovered unenforceable by a builder or - worse - by a write path.
// The refusals split three ways and the split is deliberate:
//
//   * `Unsupported` for a form this engine understands and declines - a lower
//     bound (AS11), `DEFERRABLE` (AS3), `NOT VALID` (AS7), an aggregate
//     outside {COUNT, SUM} (§10). These are reserved grammar: they parse, so
//     the answer names the decision rather than pointing at a syntax error
//     somewhere else, and the grammar will not shift when any of them lands.
//   * `InvalidArgument` for a declaration that is simply wrong - a negative
//     bound, `!=`, a degenerate predicate that could never admit a row.
//   * Nothing at all for the catalog's questions. Whether the relation
//     exists, whether a group column exists, whether the `SUM` column is
//     int64, whether the name is taken: all of that needs the catalog and is
//     `Catalog::CreateAssertion`'s, which is the door every non-parser caller
//     comes through. This layer knows *where* each name was written and
//     hands the offsets on so those errors can carry a position too.
StatusOr<AssertionStmt> Parser::ParseAssertion(bool drop) {
    AssertionStmt stmt;
    stmt.drop = drop;

    stmt.byte_offset = lexer_.Peek().byte_offset;
    auto name = ParseIdent();
    if (!name.ok()) return name.status();
    stmt.name = std::move(name.value());

    // An assertion's name is unique instance-wide (§3), so DROP names nothing
    // else - IndexStmt's rule, and for its reason.
    if (drop) {
        ConsumeOptionalSemicolon();
        return stmt;
    }

    // `ON` is a reserved keyword (it joins), so it arrives as a keyword token
    // and ExpectKeyword cannot be used for it - the wrinkle ParseCabin and
    // ParseIndex both have.
    const Token& on = lexer_.Peek();
    if (on.type != TokenType::kKeyword || on.kw != Keyword::kOn) {
        return Status::InvalidArgument(
            "CREATE ASSERTION names the relation it is on: `CREATE ASSERTION <name> ON "
            "<table> GROUP BY (<column>, ...) CHECK ...` (byte " +
            std::to_string(on.byte_offset) + ")");
    }
    lexer_.Next();

    if (Status s = ParseQualifiedName(stmt.schema, stmt.table_name, &stmt.table_byte_offset);
        !s.ok()) {
        return s;
    }

    // ---- GROUP BY (<column>, ...) ---------------------------------------
    //
    // Mandatory, and parenthesised where a SELECT's GROUP BY is not. Both
    // halves are deliberate: an assertion with no grouping is a whole-relation
    // bound, which is a different structure (one header, no directory) and is
    // not what v1 built - so it is refused rather than silently read as one
    // group. The parens are what make the list's end unambiguous with `CHECK`
    // following it, without `CHECK` having to be reserved.
    if (const Token& group = lexer_.Peek();
        group.type != TokenType::kIdent || !IEquals(group.text, "GROUP")) {
        return Status::InvalidArgument(
            "CREATE ASSERTION requires a GROUP BY list: `... ON <table> GROUP BY (<column>, "
            "...) CHECK ...` (byte " +
            std::to_string(group.byte_offset) + ")");
    }
    lexer_.Next();
    if (Status s = ExpectKeyword("BY"); !s.ok()) return s;

    // No cap: §3 declares none, and inventing one here would settle a number
    // nothing has measured. The list is storable regardless because the
    // catalog row keeps `source_text` and not a column array (AS10).
    if (Status s = ParseDeclaredColumnList(stmt.group_columns, "assertion GROUP BY columns",
                                           /*cap=*/0);
        !s.ok()) {
        return s;
    }

    // ---- CHECK COUNT(*) | SUM(<column>) ---------------------------------
    //
    // `CHECK` is an ordinary identifier matched by text, like `GROUP`,
    // `COVERING` and `DISTINCT` before it: nothing here is reserved, so a
    // column may still be named `check` or `assertion` (V04's property - all
    // keywords share one token type, and these are not keywords at all).
    if (const Token& check = lexer_.Peek();
        check.type != TokenType::kIdent || !IEquals(check.text, "CHECK")) {
        return Status::InvalidArgument(
            "CREATE ASSERTION requires a CHECK clause: `... CHECK COUNT(*) <= <N>` or `... "
            "CHECK SUM(<column>) <= <N>` (byte " +
            std::to_string(check.byte_offset) + ")");
    }
    lexer_.Next();

    const Token agg_tok = lexer_.Peek();
    auto agg_name = ParseIdent();
    if (!agg_name.ok()) {
        return Status::InvalidArgument(
            "CHECK takes COUNT(*) or SUM(<column>) (byte " +
            std::to_string(agg_tok.byte_offset) + ")");
    }
    AggFunc func = AggFunc::kCount;
    if (!AggFuncOf(agg_name.value(), func)) {
        return Status::InvalidArgument("'" + agg_name.value() +
                                        "' is not an aggregate; CHECK takes COUNT(*) or "
                                        "SUM(<column>) (byte " +
                                        std::to_string(agg_tok.byte_offset) + ")");
    }
    if (func != AggFunc::kCount && func != AggFunc::kSum) {
        // §10: MIN and MAX are not incrementally maintainable under deletion
        // without extra structure, and AVG is not a bound. Reserved and
        // refused by name, so the answer says which decision it is waiting on.
        return Status::NotImplemented(
            std::string(AggFuncText(func)) +
            " bounds are out of scope for assertions (byte " +
            std::to_string(agg_tok.byte_offset) +
            "); v1 takes COUNT(*) and SUM(<int64 column>) upper bounds only "
            "(docs/spec/assertion.md §10)");
    }
    stmt.func = func;

    if (Status s = ExpectToken(TokenType::kLParen, "'(' after the aggregate's name"); !s.ok()) {
        return s;
    }

    // `DISTINCT` is refused rather than folded away: a distinct count is a
    // different aggregate, and maintaining one incrementally needs per-value
    // multiplicity that no group header carries.
    if (const Token& distinct = lexer_.Peek();
        distinct.type == TokenType::kIdent && IEquals(distinct.text, "DISTINCT")) {
        return Status::NotImplemented("DISTINCT is not supported in an assertion's CHECK (byte " +
                                    std::to_string(distinct.byte_offset) +
                                    "); a distinct aggregate is not incrementally maintainable "
                                    "from a group header alone");
    }

    if (stmt.func == AggFunc::kCount) {
        // `COUNT(*)` exactly. `COUNT(<column>)` is a different aggregate -
        // it skips NULLs - and reading it as `COUNT(*)` would enforce a bound
        // the operator did not write.
        const Token& star = lexer_.Peek();
        if (star.type != TokenType::kStar) {
            return Status::NotImplemented(
                "an assertion's cardinality bound is written COUNT(*) (byte " +
                std::to_string(star.byte_offset) +
                "); COUNT(<column>) counts non-NULLs, which is a different aggregate");
        }
        lexer_.Next();
    } else {
        const Token& arg = lexer_.Peek();
        if (arg.type == TokenType::kStar) {
            return Status::InvalidArgument("SUM takes a column, not '*' (byte " +
                                            std::to_string(arg.byte_offset) + ")");
        }
        stmt.sum_column.byte_offset = arg.byte_offset;
        auto col = ParseIdent();
        if (!col.ok()) return col.status();
        stmt.sum_column.name = std::move(col.value());
    }

    if (Status s = ExpectToken(TokenType::kRParen, "')' closing the aggregate's argument");
        !s.ok()) {
        return s;
    }

    // ---- <op> <N> -------------------------------------------------------

    const std::uint32_t op_at = lexer_.Peek().byte_offset;
    auto op = ParseCompareOp();
    if (!op.ok()) return op.status();

    switch (op.value()) {
        case CompareOp::kLt:
        case CompareOp::kLte:
            break;
        case CompareOp::kIsNull:
        case CompareOp::kIsNotNull:
            // Unreachable through ParseCompareOp, which never produces the
            // IS forms - they parse in ParseCondition only. Refused rather
            // than defaulted, so a grammar change here has to decide.
            return Status::Unsupported("an assertion bound takes a relational operator (byte " +
                                       std::to_string(op_at) + ")");
        case CompareOp::kEq:
            // **AS11 as revised 2026-08-08.** `=` was briefly accepted and
            // documented as meaning `aggregate <= N`. That is refused now,
            // and the reason is truthfulness rather than cost: the engine
            // would have enforced something other than what the operator
            // wrote, and a constraint that quietly means less than it says is
            // worse than one that is refused outright. Enforcing real
            // equality means enforcing a *lower* bound too, which is the
            // DELETE and decreasing-UPDATE write path v1 excludes - so `=`
            // costs exactly what `>=` costs, and is refused beside it.
            return Status::NotImplemented(
                "equality assertions (=) are not supported (byte " + std::to_string(op_at) +
                "); enforcing = means enforcing a lower bound, which v1 excludes, and reading "
                "it as <= would enforce something other than what was written "
                "(docs/spec/assertion.md AS11)");
        case CompareOp::kGt:
        case CompareOp::kGte:
            // AS11, and the one refusal that pays for a whole write path:
            // a lower bound has to be re-checked on DELETE and on every
            // decreasing UPDATE, which is exactly why v1 leaves DELETE
            // uninstrumented (§4.2). Reserved grammar - it parses.
            return Status::NotImplemented(
                std::string("lower-bound assertions (") + CompareOpName(op.value()) +
                ") are not supported (byte " + std::to_string(op_at) +
                "); v1 enforces upper bounds only, which is what makes DELETE check-free "
                "(docs/spec/assertion.md AS11)");
        case CompareOp::kNeq:
            // Distinct from `=` and from `>`: those name a constraint this
            // engine understands and declines, so they say which decision
            // they wait on. `!=` names no ceiling in any direction, so there
            // is no decision pending and it is simply wrong.
            return Status::InvalidArgument(
                "'!=' is not a bound (byte " + std::to_string(op_at) +
                "); an assertion's comparison is < or <=");
    }
    stmt.op = op.value();

    const Token bound = lexer_.Peek();
    if (bound.type != TokenType::kIntLit || bound.negative || bound.int_val < 0) {
        // §3.1: a non-negative integer *literal*, no expressions (TY3
        // conservatism). A negative one is caught by the same test, which is
        // why the message names both halves.
        return Status::InvalidArgument(
            "an assertion's bound is a non-negative integer literal, got '" +
            std::string(Describe(bound)) + "' (byte " + std::to_string(bound.byte_offset) + ")");
    }
    lexer_.Next();
    stmt.bound = bound.int_val;
    stmt.bound_byte_offset = bound.byte_offset;

    // ---- Degenerate predicates (§3.1) -----------------------------------
    //
    // Only for COUNT, and the asymmetry is a proof rather than an oversight:
    // a group *exists* only because it holds at least one row, so its count is
    // at least 1 and any ceiling below 1 admits nothing - `COUNT(*) <= 0` and
    // `COUNT(*) < 1` both declare a relation that may never be written to
    // again. (`= 0`, the spelling §3.1 named, is now refused one step earlier
    // by the operator itself.) A SUM has no such floor, because an int64
    // column may hold negative values, so no non-negative bound is provably
    // unsatisfiable and refusing one would be inventing a restriction.
    if (stmt.func == AggFunc::kCount && stmt.enforced_max() < 1) {
        return Status::InvalidArgument(
            "assertion \"" + stmt.name + "\" can never admit a row: COUNT(*) " +
            CompareOpName(stmt.op) + " " + std::to_string(stmt.bound) +
            " enforces count <= " + std::to_string(stmt.enforced_max()) +
            ", and a group holds at least one row (byte " +
            std::to_string(stmt.bound_byte_offset) + ")");
    }

    // ---- Reserved trailing clauses (AS3, AS7) ---------------------------
    //
    // `DEFERRABLE`, `NOT DEFERRABLE`, `INITIALLY DEFERRED/IMMEDIATE` and
    // `NOT VALID` are SQL-92's and PostgreSQL's spellings for constraint
    // timing. All parse and all answer `Unsupported` with their own position,
    // which is what a client wants when they wrote a word this engine has
    // heard of but does not honour - the alternative is "unexpected token"
    // pointing at a clause that was the whole point of the statement.
    for (;;) {
        const Token& tail = lexer_.Peek();
        if (tail.type == TokenType::kKeyword && tail.kw == Keyword::kNot) {
            // `NOT DEFERRABLE` and `NOT VALID`. Both are refused, and `NOT
            // DEFERRABLE` too even though it names the behaviour v1 *has*:
            // accepting it would make it a promise, and AS3 reserves the whole
            // timing clause rather than half of it.
            return Status::Unsupported(
                "constraint timing clauses (NOT DEFERRABLE / NOT VALID) are not supported "
                "(byte " +
                std::to_string(tail.byte_offset) +
                "); an assertion is checked at statement time, always "
                "(docs/spec/assertion.md AS3, AS7)");
        }
        if (tail.type == TokenType::kIdent &&
            (IEquals(tail.text, "DEFERRABLE") || IEquals(tail.text, "INITIALLY") ||
             IEquals(tail.text, "VALID"))) {
            return Status::Unsupported(
                "'" + std::string(tail.text) +
                "' is reserved and not supported (byte " + std::to_string(tail.byte_offset) +
                "); an assertion is checked at statement time, always "
                "(docs/spec/assertion.md AS3, AS7)");
        }
        break;
    }

    ConsumeOptionalSemicolon();

    // The whole declaration verbatim, for `sys.assertions.source_text`
    // (AS10) - the stored-text model, and what makes an uncapped GROUP
    // BY list storable in a fixed-width catalog row. Sliced from the input
    // rather than rebuilt from the AST, because a rebuild is a second
    // spelling of the operator's declaration that can drift from theirs.
    // `Parse()` refuses trailing garbage, so the statement is exactly the
    // input and needs no end offset to find.
    std::string_view text = sql_;
    while (!text.empty() && (text.back() == ';' ||
                             std::isspace(static_cast<unsigned char>(text.back())) != 0)) {
        text.remove_suffix(1);
    }
    stmt.source_text = std::string(text);

    return stmt;
}

StatusOr<InsertStmt> Parser::ParseInsert() {
    if (Status s = ExpectKeyword("INTO"); !s.ok()) return s;

    InsertStmt stmt;

    if (Status s = ParseQualifiedName(stmt.schema, stmt.table_name, &stmt.table_byte_offset);
        !s.ok()) {
        return s;
    }

    if (Status s = ExpectKeyword("VALUES"); !s.ok()) return s;

    // One or more parenthesised rows, comma-separated (bulkinsert.md
    // BI3). The row *cap* is the dispatcher's - this layer is config-blind
    // - so the loop is bounded only by the statement text, which the
    // server already accepted whole.
    for (;;) {
        if (Status s = ExpectToken(TokenType::kLParen, "'('"); !s.ok()) return s;

        std::vector<AstValue> row;
        for (;;) {
            auto val = ParseValue();
            if (!val.ok()) return val.status();
            row.push_back(std::move(val.value()));

            if (lexer_.Peek().type == TokenType::kComma) {
                lexer_.Next();
                continue;
            }
            break;
        }

        if (Status s = ExpectToken(TokenType::kRParen, "')'"); !s.ok()) return s;
        stmt.rows.push_back(std::move(row));

        if (lexer_.Peek().type == TokenType::kComma) {
            lexer_.Next();
            continue;
        }
        break;
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
        return Status::NotImplemented(
            "a subquery cannot appear in FROM (byte " + std::to_string(offset) +
            "); derived tables and CTEs are not supported, only predicate-position subqueries");
    }

    // `sys.tables`, and since AF-T3 `orders.customer` too - one production
    // for every relation name in the grammar.
    RelationRef rel;
    if (Status s = ParseQualifiedName(rel.schema, rel.table_name); !s.ok()) return s;
    rel.byte_offset = offset;

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
        return Status::NotImplemented("ON requires a qualified column (`rel.col`), got '" +
                                    col.value().name + "' at byte " +
                                    std::to_string(col.value().byte_offset) +
                                    "; unqualified names in ON are not resolved");
    }
    return col;
}

StatusOr<SelectItem> Parser::ParseSelectItem() {
    SelectItem item;
    item.byte_offset = lexer_.Peek().byte_offset;

    // ---- Is this a function head? ---------------------------------------
    //
    // A head is an **unqualified name from the function set followed by
    // `(`**, and both halves carry weight. No production in this grammar
    // puts a paren after a column reference, so the paren is what makes the
    // test unambiguous - which is why none of these words is reserved and
    // why a column may still be named `count` (spec §2). Parsing the head as
    // a column name first and checking the paren after is the one token of
    // lookahead this needs.
    auto col = ParseColumnName();
    if (!col.ok()) return col.status();

    AggFunc func = AggFunc::kCount;
    if (col.value().qualified() || lexer_.Peek().type != TokenType::kLParen ||
        !AggFuncOf(col.value().name, func)) {
        item.column = std::move(col.value());
        return item;
    }

    item.is_aggregate = true;
    item.func = func;
    lexer_.Next();  // consume '('

    // `DISTINCT`, unreserved like every other word here. `COUNT(distinct)`
    // - the word used as the argument's name - therefore reads as the
    // qualifier and then fails for a missing argument, which is the same
    // reading standard SQL gives it.
    if (const Token& maybe = lexer_.Peek();
        maybe.type == TokenType::kIdent && IEquals(maybe.text, "DISTINCT")) {
        lexer_.Next();
        item.distinct = true;
    }

    if (lexer_.Peek().type == TokenType::kStar) {
        const std::uint32_t star_at = lexer_.Peek().byte_offset;
        lexer_.Next();
        if (item.func != AggFunc::kCount) {
            return Status::InvalidArgument(
                std::string(AggFuncText(item.func)) + " takes a column, not '*' (byte " +
                std::to_string(star_at) + "); '*' is only an argument of COUNT");
        }
        if (item.distinct) {
            return Status::InvalidArgument(
                "COUNT(DISTINCT *) is not a thing this engine can mean (byte " +
                std::to_string(star_at) +
                "); distinctness of whole rows was never written, name a column");
        }
        item.star_arg = true;
    } else {
        auto arg = ParseColumnName();
        if (!arg.ok()) return arg.status();
        item.column = std::move(arg.value());
    }

    if (Status s = ExpectToken(TokenType::kRParen, "')' closing an aggregate's argument");
        !s.ok()) {
        return s;
    }
    return item;
}

Status Parser::ParseSelectList(std::vector<SelectItem>& items, bool& star) {
    // `SELECT *` produces no items at all. It stays available for a single
    // relation, where it is unambiguous and is what almost every statement
    // in the corpus says.
    if (lexer_.Peek().type == TokenType::kStar) {
        lexer_.Next();
        star = true;
        return Status::OK();
    }
    star = false;

    for (;;) {
        auto item = ParseSelectItem();
        if (!item.ok()) return item.status();
        items.push_back(std::move(item.value()));

        if (lexer_.Peek().type != TokenType::kComma) return Status::OK();
        lexer_.Next();
    }
}

Status Parser::ParseGroupBy(SelectStmt& stmt) {
    for (;;) {
        auto col = ParseColumnName();
        if (!col.ok()) return col.status();

        // AG9: column references only. A paren here is the one shape that
        // reads as an expression - `GROUP BY count(x)` - and refusing it by
        // position beats letting `count` resolve as a column that does not
        // exist and reporting that instead.
        if (lexer_.Peek().type == TokenType::kLParen) {
            return Status::NotImplemented(
                "GROUP BY takes column references only, not expressions (byte " +
                std::to_string(col.value().byte_offset) + ")");
        }
        stmt.group_by.push_back(std::move(col.value()));

        if (lexer_.Peek().type != TokenType::kComma) return Status::OK();
        lexer_.Next();
    }
}

// `HAVING <agg> <op> <val> [AND ...]`, the word already consumed
// (docs/inflight/in-progress/workplan-having.md HV-1, HV3).
//
// The left-hand side comes from `ParseSelectItem`, which already parses
// exactly what this needs - an aggregate call or a plain column reference,
// each with the byte it starts at - so the two clauses that name an
// aggregate name it through one production. Sharing it is what keeps
// `count(distinct x)` mean the same thing in a select list and here.
//
// What this production decides is shape, and it decides three things: the
// conjuncts are AND-combined, `IS [NOT] NULL` is a predicate with no right
// side, and every other right side is a **literal**. The last is the one
// worth arguing: a post-fold predicate has no row to read a column from,
// and an aggregate on the right would need an expression grammar this
// language does not have (AG9's argument, two clauses over). Both are
// refused here with the offending token's own byte rather than left to
// fail as trailing garbage somewhere past it.
//
// What it does *not* decide: whether the aggregate typechecks, and whether
// a plain column is a grouping key. Both are catalog questions, and HV-2
// answers them where catalog knowledge lives.
Status Parser::ParseHaving(SelectStmt& stmt) {
    for (;;) {
        HavingCondition cond;

        auto lhs = ParseSelectItem();
        if (!lhs.ok()) return lhs.status();
        cond.agg = std::move(lhs.value());

        // A paren still open past the left side is a call this grammar has
        // no function for - `HAVING foo(q) > 1` - and refusing it at the
        // name beats the "expected a comparison operator" the paren would
        // otherwise produce, which points at a token the client did not get
        // wrong. The same check `ORDER BY` makes one clause down.
        if (lexer_.Peek().type == TokenType::kLParen) {
            return Status::NotImplemented(
                "HAVING takes an aggregate or a column, not an expression (byte " +
                std::to_string(cond.agg.byte_offset) + ")");
        }

        // `IS [NOT] NULL`, the one predicate with no right-hand side. A
        // group that folded no non-NULL argument answers NULL for SUM,
        // MIN, MAX and AVG (AG4), so this is the only way to ask for it -
        // every relational operator drops it under null.md §4's
        // three-valued collapse.
        if (const Token is_tok = lexer_.Peek();
            is_tok.type == TokenType::kIdent && IEquals(is_tok.text, "IS")) {
            lexer_.Next();
            bool negated = false;
            if (lexer_.Peek().type == TokenType::kKeyword &&
                lexer_.Peek().kw == Keyword::kNot) {
                lexer_.Next();
                negated = true;
            }
            if (lexer_.Peek().type != TokenType::kNullLit) {
                return Status::InvalidArgument("expected NULL after IS (byte " +
                                                std::to_string(lexer_.Peek().byte_offset) +
                                                ")");
            }
            lexer_.Next();
            cond.op = negated ? CompareOp::kIsNotNull : CompareOp::kIsNull;
        } else {
            auto op = ParseCompareOp();
            if (!op.ok()) return op.status();
            cond.op = op.value();

            // A sub-chain under a post-fold predicate puts an aggregation
            // boundary where the execution model has none - AG8's refusal,
            // which this is the same statement about from the other side.
            if (const Token paren = lexer_.Peek(); paren.type == TokenType::kLParen) {
                return Status::NotImplemented(
                    "HAVING compares against a literal, not a subquery (byte " +
                    std::to_string(paren.byte_offset) +
                    "); a fold has no row for a sub-chain to correlate with");
            }

            // An identifier on the right is one of two statements, and they
            // deserve different sentences. Parsed through the same
            // production the left side used, so the refusal can say which
            // it read and point at where it started.
            if (lexer_.Peek().type == TokenType::kIdent) {
                auto rhs = ParseSelectItem();
                if (!rhs.ok()) return rhs.status();
                if (rhs.value().is_aggregate) {
                    return Status::NotImplemented(
                        "HAVING compares one aggregate against a literal, not against "
                        "another aggregate (byte " +
                        std::to_string(rhs.value().byte_offset) +
                        "); this grammar has no expressions");
                }
                return Status::NotImplemented(
                    "HAVING compares against a literal, not a column (byte " +
                    std::to_string(rhs.value().byte_offset) +
                    "); the fold has already consumed the rows a column would be read "
                    "from - filter those with WHERE");
            }

            auto val = ParseValue();
            if (!val.ok()) return val.status();
            cond.val = std::move(val.value());
        }
        stmt.having.push_back(std::move(cond));

        const Token next = lexer_.Peek();
        if (next.type == TokenType::kIdent && IEquals(next.text, "AND")) {
            lexer_.Next();
            continue;
        }
        return Status::OK();
    }
}

// The pagination tail (spec I11, workplan V09, amended by
// docs/workplan-order-by.md OB1): `[ORDER BY <col> [ASC|DESC] [, ...]]
// [LIMIT <n>] [OFFSET <m>]`, clause order fixed as written and each clause
// independently optional. `ORDER`, `BY`, `ASC`, `DESC`, `LIMIT` and
// `OFFSET` are ordinary identifiers matched by text at clause position,
// like `GROUP` above and for its reason: no column reference can stand
// here, so a column may carry any of those names and the fingerprint does
// not move.
//
// What this clause decides and what it refuses to decide: it reads the
// *shape* - column references, a direction per key, a cap on how many -
// and stores what was written. Which relation a name belongs to, whether
// the column exists, and whether the order asked for is one the chain
// already emits are all catalog questions, answered in the compiler (OB3).
//
// Tokens from `Peek()` are bound **by value** throughout. A reference is
// safe today only because `Token` is trivially copyable and `Next()`
// leaves the peeked token intact; the day `Token` owns anything, every
// reference held across a `Next()` becomes a use-after-move, silently.
Status Parser::ParsePaginationTail(SelectStmt& stmt, bool aggregated, std::uint32_t depth) {
    if (const Token order = lexer_.Peek();
        order.type == TokenType::kIdent && IEquals(order.text, "ORDER")) {
        // Over aggregated output this used to be a refusal citing "an
        // output sort this engine does not have". The sort exists (OB4) and
        // `docs/inflight/in-progress/workplan-having.md` HV4 decides the half AG's `[PROPOSED]`
        // row left open, so the clause is parsed over a fold too - with an
        // aggregate admitted as a key, since `ORDER BY COUNT(*)` names
        // something no column reference can.
        //
        // A subquery's rows feed a predicate - IN's set, EXISTS's witness,
        // a scalar's one value - and no order of them is observable, so an
        // ORDER BY there is a clause with nothing to mean.
        if (depth > 0) {
            return Status::NotImplemented(
                "ORDER BY inside a subquery is not supported (byte " +
                std::to_string(order.byte_offset) +
                "); a subquery's rows feed a predicate, and no order of them is observable");
        }
        lexer_.Next();
        if (Status s = ExpectKeyword("BY"); !s.ok()) return s;

        // The key list. Written order is significant and preserved: the
        // first key decides, later keys break its ties.
        while (true) {
            // `ORDER BY 1` is a form this engine understands and declines -
            // the ordinal names a select-list position, and positional naming
            // is a second spelling of something that already has one.
            if (const Token ordinal = lexer_.Peek(); ordinal.type == TokenType::kIntLit) {
                return Status::NotImplemented(
                    "ORDER BY an ordinal is not supported (byte " +
                    std::to_string(ordinal.byte_offset) + "); name the column");
            }

            auto item = ParseSelectItem();
            if (!item.ok()) return item.status();
            const std::uint32_t key_at = item.value().byte_offset;

            // AG9's argument, one clause over: a paren still open after the
            // key reads as an expression - `ORDER BY foo(x)`, a call this
            // grammar has no function for - and refusing it by position
            // beats a trailing-garbage error pointing past it. Over a
            // non-aggregated statement an *aggregate* key is the same
            // refusal and keeps OB1's wording, because there is no fold for
            // it to be an answer of.
            const bool wrote_call =
                item.value().is_aggregate || lexer_.Peek().type == TokenType::kLParen;
            if (wrote_call && !aggregated) {
                return Status::NotImplemented(
                    "ORDER BY takes a column reference only, not an expression (byte " +
                    std::to_string(key_at) + ")");
            }
            if (lexer_.Peek().type == TokenType::kLParen) {
                return Status::NotImplemented(
                    "ORDER BY takes a column reference or an aggregate, not an expression "
                    "(byte " + std::to_string(key_at) + ")");
            }

            // The cap is checked against what this key would make the list,
            // and refuses at the byte of the key that crossed it - so the
            // client is pointed at the ninth key, not at the clause.
            if (stmt.order_by.size() >= kMaxSortKeys) {
                return Status::NotImplemented(
                    "ORDER BY takes at most " + std::to_string(kMaxSortKeys) +
                    " keys (byte " + std::to_string(key_at) +
                    "); every key costs a comparison on every row pair, and sorting by a "
                    "prefix of what was written would answer a question nobody asked");
            }

            SortKey key;
            key.key = std::move(item.value());

            // `ASC` is the default spelled out; `DESC` reverses this key's
            // comparison and nothing else. Neither word is reserved, so a
            // trailing identifier that is neither falls through to
            // trailing-garbage exactly as it did before this clause grew.
            if (const Token dir = lexer_.Peek(); dir.type == TokenType::kIdent) {
                if (IEquals(dir.text, "ASC")) {
                    lexer_.Next();
                } else if (IEquals(dir.text, "DESC")) {
                    lexer_.Next();
                    key.descending = true;
                }
            }
            stmt.order_by.push_back(std::move(key));

            if (lexer_.Peek().type != TokenType::kComma) break;
            lexer_.Next();
        }
    }

    std::optional<std::uint64_t> limit;
    if (Status s = ParseCountClause("LIMIT", aggregated, depth, limit); !s.ok()) return s;
    stmt.limit = limit;

    std::optional<std::uint64_t> offset;
    if (Status s = ParseCountClause("OFFSET", aggregated, depth, offset); !s.ok()) return s;
    if (offset.has_value()) stmt.offset = offset.value();

    return Status::OK();
}

// One count clause of the tail: `<word> <n>`, absent-is-ok. The class
// refusals live here per clause rather than gating the tail once, because
// each names the clause the client wrote and the byte it starts at -
// "LIMIT ... (byte 40)" against a statement whose ORDER BY would also
// have been refused is the error that points at what was typed.
Status Parser::ParseCountClause(std::string_view word, bool aggregated, std::uint32_t depth,
                                std::optional<std::uint64_t>& out) {
    const Token head = lexer_.Peek();
    if (head.type != TokenType::kIdent || !IEquals(head.text, word)) return Status::OK();

    // Groups are emitted in fold order, which is deterministic and
    // deliberately not a contract - so which rows survive the clause would
    // be a guess, and this engine's answer to "which wrong number would
    // you like" has always been neither.
    //
    // **Kept refused by HV5** (docs/inflight/in-progress/workplan-having.md), where HAVING and
    // ORDER BY over a fold were lifted beside it: serving this clause is
    // what would promote AG6's fold order into a client contract, and
    // ordering the groups first makes that a decision worth making on its
    // own rather than one taken by three other clauses.
    if (aggregated) {
        return Status::NotImplemented(
            std::string(word) + " is not supported over an aggregated statement (byte " +
            std::to_string(head.byte_offset) +
            "); groups are emitted in fold order, which is not a contract, so which rows "
            "survive the clause would be a guess");
    }
    // AG8's shape: pagination applies at the statement's emission
    // boundary, and a sub-chain has none - its rows feed a predicate.
    if (depth > 0) {
        return Status::NotImplemented(
            std::string(word) + " inside a subquery is not supported (byte " +
            std::to_string(head.byte_offset) + "); paginate the outer statement instead");
    }
    lexer_.Next();

    auto count = ParsePaginationCount(word);
    if (!count.ok()) return count.status();
    out = count.value();
    return Status::OK();
}

// The count in a LIMIT or OFFSET clause. Decoded from the digits, not from
// `int_val`: the signed decode wraps past 64 bits (token.hpp), and TY11's
// lesson stands one layer down - a wrapped count silently means a number
// the client did not write, so past-uint64 refuses instead.
StatusOr<std::uint64_t> Parser::ParsePaginationCount(std::string_view clause) {
    const Token tok = lexer_.Next();
    if (tok.type != TokenType::kIntLit || tok.negative) {
        return Status::InvalidArgument(std::string(clause) +
                                        " takes a non-negative integer literal, got '" +
                                        std::string(Describe(tok)) + "' at byte " +
                                        std::to_string(tok.byte_offset));
    }
    // The lexer guarantees a non-empty ASCII digit run, so the only way
    // from_chars fails here is overflow.
    const std::string_view digits = tok.digits();
    std::uint64_t value = 0;
    if (std::from_chars(digits.data(), digits.data() + digits.size(), value).ec !=
        std::errc()) {
        return Status::InvalidArgument(std::string(clause) + " count '" + std::string(digits) +
                                        "' does not fit in 64 bits (byte " +
                                        std::to_string(tok.byte_offset) + ")");
    }
    return value;
}

// No `depth`: a join's relations are relation references, and a relation
// reference is never a subquery (ParseRelationRef refuses one outright).
Status Parser::ParseJoins(SelectStmt& stmt) {
    for (;;) {
        const Token& peek = lexer_.Peek();
        if (peek.type != TokenType::kKeyword) return Status::OK();

        // An outer join is a form this engine understands and declines,
        // and one a later release could build - `NotImplemented`, the half
        // of J2's pair that says so (status.hpp) - as opposed to a syntax
        // error, which would send a client looking for a typo. The
        // position is the keyword's own.
        if (peek.kw == Keyword::kLeft || peek.kw == Keyword::kRight ||
            peek.kw == Keyword::kFull || peek.kw == Keyword::kOuter) {
            return Status::NotImplemented("outer joins are not supported: '" +
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
            // well-formed and has a meaning in standard SQL. And
            // Unsupported rather than NotImplemented, because "give each
            // occurrence a distinct alias" is the answer on every later
            // release too - there is nothing here to build. This engine
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
    std::vector<SelectItem> items;
    bool wrote_star = false;
    if (Status s = ParseSelectList(items, wrote_star); !s.ok()) return s;

    if (Status s = ExpectKeyword("FROM"); !s.ok()) return s;

    auto from = ParseRelationRef();
    if (!from.ok()) return from.status();
    stmt.from = std::move(from.value());

    if (Status s = ParseJoins(stmt); !s.ok()) return s;
    if (Status s = CheckDistinctBindings(stmt); !s.ok()) return s;

    // `SELECT *` over more than one relation. Which columns it means, and
    // in what order, would be a property of how the relations were joined
    // - and written order being the client's to choose (spec section 1) is
    // exactly what makes that unanswerable here. NotImplemented rather
    // than InvalidArgument: the statement is well-formed, naming the
    // columns is the fix today, and cross-relation `*` is a real SQL
    // semantic a later release could adopt.
    // `wrote_star`, not `stmt.star()`: the items are still staged in a
    // local at this point, so the statement's own star test would read a
    // named list as a star and refuse every multi-relation SELECT that
    // spells its columns out.
    if (wrote_star && stmt.relation_count() > 1) {
        return Status::NotImplemented(
            "SELECT * is ambiguous across " + std::to_string(stmt.relation_count()) +
            " relations (byte " + std::to_string(star_at) +
            "); name the columns you want, qualified (`a.x, b.y`)");
    }

    auto where = ParseOptionalWhere(depth);
    if (!where.ok()) return where.status();
    stmt.where = std::move(where.value());

    // ---- GROUP BY, and the three clauses that may follow it -------------
    //
    // `GROUP` is read as a clause head only *here*, past the WHERE, where
    // no column reference can stand - which is what lets it stay
    // unreserved, and a column stay named `group` (spec §2).
    if (const Token& peek = lexer_.Peek();
        peek.type == TokenType::kIdent && IEquals(peek.text, "GROUP")) {
        lexer_.Next();
        if (Status s = ExpectKeyword("BY"); !s.ok()) return s;
        if (Status s = ParseGroupBy(stmt); !s.ok()) return s;
    }

    // A statement is aggregated when it wrote an aggregate **or** a GROUP
    // BY. The second half is not a technicality: `SELECT b FROM t GROUP BY
    // b` names no function and still emits one row per group rather than
    // one per row, which is a fold by every property that matters.
    bool aggregated = false;
    std::uint32_t agg_at = 0;
    for (const SelectItem& item : items) {
        if (!item.is_aggregate) continue;
        aggregated = true;
        agg_at = item.byte_offset;
        break;
    }
    if (!stmt.group_by.empty()) {
        if (!aggregated) agg_at = stmt.group_by.front().byte_offset;
        aggregated = true;
    }

    // HAVING is read by text exactly where it would be written, past the
    // GROUP BY list and before the tail, and it is the third thing that can
    // make a statement aggregated (HV-1). AG7's refusal used to be here;
    // `docs/inflight/in-progress/workplan-having.md` retracts it, and the clause it refused is
    // now parsed in its place.
    if (const Token having = lexer_.Peek();
        having.type == TokenType::kIdent && IEquals(having.text, "HAVING")) {
        lexer_.Next();
        if (Status s = ParseHaving(stmt); !s.ok()) return s;
        if (!aggregated) agg_at = having.byte_offset;
        aggregated = true;
    }

    // Which columns `*` folds - and in what order - was never written, and
    // there is no reading of it that is not a guess. InvalidArgument rather
    // than Unsupported: naming the columns is not a feature request.
    //
    // Checked after HAVING rather than before it, so `SELECT * FROM t
    // HAVING COUNT(*) > 1` meets this answer rather than falling through to
    // a fold with a star it cannot mean.
    if (wrote_star && aggregated) {
        return Status::InvalidArgument(
            "SELECT * cannot be combined with GROUP BY or HAVING (byte " +
            std::to_string(star_at) +
            "); name the grouping columns and the aggregates you want");
    }

    // ---- The pagination tail: ORDER BY, LIMIT, OFFSET (I11, V09) --------
    //
    // Parsed here and refused here - over aggregated output, in a
    // subquery, `DESC` - so the aggregated ORDER BY answer the corpus pins
    // did not move when the non-aggregated form became parseable.
    if (Status s = ParsePaginationTail(stmt, aggregated, depth); !s.ok()) return s;

    // AG8 / J2: a fold inside a sub-chain puts an aggregation boundary
    // where the execution model has none. Refused with the position of
    // whichever half made the block aggregated.
    if (aggregated && depth > 0) {
        return Status::NotImplemented(
            "an aggregate or GROUP BY inside a subquery is not supported (byte " +
            std::to_string(agg_at) + "); aggregate in the outer statement instead");
    }

    // ---- Where the items land -------------------------------------------
    //
    // A statement that aggregates keeps them; every other statement's items
    // are plain columns and collapse into `projection`, which is what makes
    // this whole clause invisible to a plain SELECT - the same AST, the same
    // compiled chain, the same fingerprint.
    if (aggregated) {
        stmt.agg_items = std::move(items);
    } else {
        stmt.projection.reserve(items.size());
        for (SelectItem& item : items) stmt.projection.push_back(std::move(item.column));
    }

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

    if (Status s = ParseQualifiedName(stmt.schema, stmt.table_name, &stmt.table_byte_offset);
        !s.ok()) {
        return s;
    }

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

    if (Status s = ParseQualifiedName(stmt.schema, stmt.table_name, &stmt.table_byte_offset);
        !s.ok()) {
        return s;
    }

    if (Status s = ExpectKeyword("SET"); !s.ok()) return s;

    for (;;) {
        Assignment a;

        // Taken before the ident is consumed: the compiler reports the
        // column's own byte, not the one after it.
        a.byte_offset = lexer_.Peek().byte_offset;

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
        // through ParseCreateTable's own ExpectKeyword.
        const Token& what = lexer_.Peek();
        if (what.type == TokenType::kIdent && IEquals(what.text, "INDEX")) {
            lexer_.Next();
            auto s = ParseIndex(/*drop=*/false);
            if (!s.ok()) return s.status();
            stmt = std::move(s.value());
        } else if (what.type == TokenType::kIdent && IEquals(what.text, "UNIQUE")) {
            // Refused here rather than parsed and rejected later, because
            // the position that matters is this word's. Enforcing
            // uniqueness makes the index a *constraint*, which needs a
            // second write-conflict path and would let an index failure
            // abort a write (docs/spec/index.md IX11).
            return Status::NotImplemented(
                "UNIQUE indexes are not supported (byte " + std::to_string(what.byte_offset) +
                "); v1 is a read accelerator that cannot fail a write for a reason of its own "
                "(docs/spec/index.md IX11)");
        } else if (what.type == TokenType::kIdent && IEquals(what.text, "CABIN")) {
            lexer_.Next();
            auto s = ParseCabin(/*drop=*/false);
            if (!s.ok()) return s.status();
            stmt = std::move(s.value());
        } else if (what.type == TokenType::kIdent && IEquals(what.text, "ASSERTION")) {
            lexer_.Next();
            auto s = ParseAssertion(/*drop=*/false);
            if (!s.ok()) return s.status();
            stmt = std::move(s.value());
        } else if (what.type == TokenType::kIdent && IEquals(what.text, "NAMESPACE")) {
            lexer_.Next();
            auto s = ParseNamespace(/*drop=*/false);
            if (!s.ok()) return s.status();
            stmt = std::move(s.value());
        } else {
            auto s = ParseCreateTable();
            if (!s.ok()) return s.status();
            stmt = std::move(s.value());
        }
    } else if (IEquals(tok.text, "DROP")) {
        // Tables, cabins, indexes and assertions can be dropped; saying so
        // here beats a syntax error that points at the object's name - the
        // list in the message is the whole of what exists. `DROP PATTERN`
        // was in it until 2026-08-31, when the operator withdrew declared
        // patterns, and it is now refused by that list like any other word.
        const Token& what = lexer_.Peek();
        if (what.type == TokenType::kIdent && IEquals(what.text, "INDEX")) {
            lexer_.Next();
            auto s = ParseIndex(/*drop=*/true);
            if (!s.ok()) return s.status();
            stmt = std::move(s.value());
        } else if (what.type == TokenType::kIdent && IEquals(what.text, "CABIN")) {
            lexer_.Next();
            auto s = ParseCabin(/*drop=*/true);
            if (!s.ok()) return s.status();
            stmt = std::move(s.value());
        } else if (what.type == TokenType::kIdent && IEquals(what.text, "ASSERTION")) {
            lexer_.Next();
            auto s = ParseAssertion(/*drop=*/true);
            if (!s.ok()) return s.status();
            stmt = std::move(s.value());
        } else if (what.type == TokenType::kIdent && IEquals(what.text, "NAMESPACE")) {
            lexer_.Next();
            auto s = ParseNamespace(/*drop=*/true);
            if (!s.ok()) return s.status();
            stmt = std::move(s.value());
        } else if (what.type == TokenType::kIdent && IEquals(what.text, "TABLE")) {
            // docs/spec/drop-table.md DT6: catalog-scoped, oid tombstoned,
            // pages orphaned - the refusals live in the dispatcher, which
            // is the layer that can name a blocker.
            lexer_.Next();
            DropTableStmt drop;
            if (Status s = ParseQualifiedName(drop.schema, drop.table_name, &drop.byte_offset);
                !s.ok()) {
                return s;
            }
            ConsumeOptionalSemicolon();
            stmt = std::move(drop);
        } else {
            return Status::NotImplemented(
                "only DROP TABLE, DROP NAMESPACE, DROP CABIN, DROP INDEX and DROP ASSERTION "
                "are supported (byte " +
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
    } else if (IEquals(tok.text, "ALTER")) {
        auto s = ParseAlter();
        if (!s.ok()) return s.status();
        stmt = std::move(s.value());
    } else if (IEquals(tok.text, "WITH")) {
        // The other half of the structural rule (spec section 2): `WITH`
        // must not lex as a statement head. It is answered here, by name,
        // rather than falling into "unknown SQL keyword" - a CTE is a form
        // this engine understands and declines for the reason
        // ParseRelationRef gives, not a word it has never heard of.
        return Status::NotImplemented("common table expressions (WITH) are not supported (byte " +
                                    std::to_string(tok.byte_offset) +
                                    "); subqueries are allowed in predicate position only");
    } else {
        return Status::InvalidArgument(
            "unknown SQL keyword '" + std::string(tok.text) +
            "' (supported: CREATE, DROP, ALTER, INSERT, SELECT, UPDATE, DELETE)");
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

// `ALTER TABLE <t> RENAME TO <new> | RENAME COLUMN <old> TO <new>`
// (docs/spec/alter.md AL1, AL7), with `ALTER` already consumed.
//
// `TABLE`, `RENAME`, `COLUMN` and `TO` are ordinary identifiers matched by
// text, like every clause head this grammar has grown - and the refusal
// surface is parsed *before* the accepted form, so `ADD`/`DROP`/`MODIFY`/
// `SET` answer with AL1's reason at their own byte rather than a syntax
// error pointing past them.
StatusOr<AlterStmt> Parser::ParseAlter() {
    if (Status s = ExpectKeyword("TABLE"); !s.ok()) return s;

    AlterStmt stmt;
    if (Status s = ParseQualifiedName(stmt.schema, stmt.table_name, &stmt.byte_offset);
        !s.ok()) {
        return s;
    }

    const Token verb = lexer_.Peek();
    if (verb.type == TokenType::kIdent &&
        (IEquals(verb.text, "ADD") || IEquals(verb.text, "MODIFY") ||
         IEquals(verb.text, "SET"))) {
        return Status::NotImplemented(
            "ALTER TABLE " + std::string(verb.text) +
            " is not supported (byte " + std::to_string(verb.byte_offset) +
            "); v1 is catalog-only - RENAME TO and RENAME COLUMN - because a relation's row "
            "size is a schema constant (invariant 13), so changing the column set is a "
            "relation rewrite, not a catalog edit");
    }
    // `DROP` under ALTER is the same class, worded for what was written:
    // dropping a column is the rewrite, dropping the table is DROP TABLE's
    // feature, and neither is a rename.
    if (verb.type == TokenType::kIdent && IEquals(verb.text, "DROP")) {
        return Status::NotImplemented(
            "ALTER TABLE DROP is not supported (byte " + std::to_string(verb.byte_offset) +
            "); dropping a column changes the row size (invariant 13), and dropping the "
            "relation is DROP TABLE's feature, which does not exist yet");
    }
    if (verb.type != TokenType::kIdent || !IEquals(verb.text, "RENAME")) {
        return Status::InvalidArgument("expected RENAME after the table name, got '" +
                                        std::string(Describe(verb)) + "' at byte " +
                                        std::to_string(verb.byte_offset));
    }
    lexer_.Next();

    if (const Token what = lexer_.Peek();
        what.type == TokenType::kIdent && IEquals(what.text, "COLUMN")) {
        lexer_.Next();
        stmt.rename_column = true;
        stmt.old_column_byte_offset = lexer_.Peek().byte_offset;
        auto old_col = ParseIdent();
        if (!old_col.ok()) return old_col.status();
        stmt.old_column = std::move(old_col.value());
    }

    if (Status s = ExpectKeyword("TO"); !s.ok()) return s;
    stmt.new_name_byte_offset = lexer_.Peek().byte_offset;
    auto new_name = ParseIdent();
    if (!new_name.ok()) return new_name.status();
    stmt.new_name = std::move(new_name.value());

    ConsumeOptionalSemicolon();
    return stmt;
}

StatusOr<Statement> Parse(std::string_view sql) { return Parser(sql).Parse(); }

}  // namespace kds::parser
