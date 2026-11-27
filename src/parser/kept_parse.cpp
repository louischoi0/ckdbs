#include "kds/parser/kept_parse.hpp"

#include <cctype>
#include <utility>
#include <variant>

#include "kds/parser/parser.hpp"

// The header argues every rule; this file is the walk and the checks.

namespace kds::parser {

namespace {

// Whether a substituted literal against `c` could lex into another token
// than the literal and its neighbour: the header's delimiting rule.
bool Fuses(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '\'' ||
           c == '-' || c == '$' || c == '?';
}

// Every `?` in `sql` delimited. The lexer's own scan decides what a `?` is
// (`ArgumentsOf`'s `kUnbound`, which a `$name` also gives and the grammar
// refuses before this is asked); only the bytes around each are read here.
// A `?` right after the word `CASE` is not kept either: the grammar reads
// `CASE -5 WHEN` as the column `case` minus 5 (`case_is_a_column`, a signed
// literal after it), and `CASE ? WHEN` as a simple CASE, so a negative bind
// would answer what its text refuses.
bool IsCaseWord(const Token& tok) noexcept {
    if (tok.type != TokenType::kIdent || tok.text.size() != 4) return false;
    for (std::size_t i = 0; i < 4; ++i) {
        if ((tok.text[i] | 0x20) != "case"[i]) return false;
    }
    return true;
}

bool HolesDelimited(std::string_view sql) {
    Lexer lexer(sql);
    Token prev;
    for (Token tok = lexer.Next(); tok.type != TokenType::kEof && tok.type != TokenType::kError;
         prev = tok, tok = lexer.Next()) {
        if (tok.type != TokenType::kParam) continue;
        if (IsCaseWord(prev)) return false;
        const std::size_t at = tok.byte_offset;
        if (at > 0 && Fuses(sql[at - 1])) return false;
        if (at + 1 < sql.size() && Fuses(sql[at + 1])) return false;
    }
    return true;
}

// The walk over every value position `kept_parse.hpp` names. With `values`
// null it fills nothing and only marks what it reaches; with values it fills
// each hole of the statement it is handed, which is the caller's copy - every
// node shared with the kept parse is copied before it is written.
class HoleWalk {
public:
    HoleWalk(const std::vector<HoleValue>* values, std::uint32_t holes)
        : values_(values), reached_(holes, false) {}

    bool all_reached() const {
        for (const bool r : reached_) {
            if (!r) return false;
        }
        return true;
    }

    void Walk(parser::Statement& stmt) {
        std::visit([this](auto& s) { Body(s); }, stmt);
    }

private:
    template <typename T>
    void Body(T&) {}  // a statement `KeptParse::Of` does not keep

    void Body(SelectStmt& s) { Select(s); }
    void Body(InsertStmt& s) {
        for (auto& row : s.rows) {
            for (AstValue& v : row) Value(v);
        }
        for (InsertStmt::ValueExpr& ve : s.value_exprs) ve.expr = ExprOf(ve.expr);
        for (Assignment& a : s.conflict_assignments) Assign(a);
    }
    void Body(UpdateStmt& s) {
        for (Assignment& a : s.assignments) Assign(a);
        Where(s.where);
    }
    void Body(DeleteStmt& s) { Where(s.where); }

    void Select(SelectStmt& s) {
        Where(s.where);
        for (HavingCondition& h : s.having) Value(h.val);
    }

    void Where(std::vector<Condition>& where) {
        for (Condition& c : where) Cond(c);
    }

    void Cond(Condition& c) {
        Value(c.val);
        Value(c.val_high);
        if (c.subquery != nullptr) {
            auto copy = std::make_shared<SelectStmt>(*c.subquery);
            const std::uint32_t before = filled_;
            Select(*copy);
            if (filled_ != before) c.subquery = std::move(copy);
        }
        if (c.tree != nullptr) {
            auto copy = std::make_shared<BoolExpr>(*c.tree);
            const std::uint32_t before = filled_;
            Tree(*copy);
            if (filled_ != before) c.tree = std::move(copy);
        }
    }

    void Tree(BoolExpr& t) {
        Cond(t.leaf);
        for (BoolExpr& child : t.children) Tree(child);
    }

    void Assign(Assignment& a) {
        Value(a.val);
        a.expr = ExprOf(a.expr);
    }

    // A node with a hole beneath it comes back a copy; any other, itself. A
    // scalar subquery's and a query-grammar node's holes are not reached
    // (the header): both are read again from their text.
    std::shared_ptr<const parser::Expr> ExprOf(const std::shared_ptr<const parser::Expr>& e) {
        if (e == nullptr) return e;
        std::shared_ptr<parser::Expr> copy;
        auto edit = [&]() -> parser::Expr& {
            if (copy == nullptr) copy = std::make_shared<parser::Expr>(*e);
            return *copy;
        };
        if (e->kind == parser::Expr::Kind::kLiteral && e->literal.type == ValueType::kParam) {
            if (const HoleValue* v = Take(e->literal); v != nullptr) {
                parser::Expr& out = edit();
                const std::uint32_t at = out.literal.byte_offset;
                out.literal = v->value;
                out.literal.byte_offset = at;
                out.bare_numeric = v->bare_numeric;
            }
        }
        if (auto lhs = ExprOf(e->lhs); lhs != e->lhs) edit().lhs = std::move(lhs);
        if (auto rhs = ExprOf(e->rhs); rhs != e->rhs) edit().rhs = std::move(rhs);
        for (std::size_t i = 0; i < e->args.size(); ++i) {
            if (auto arg = ExprOf(e->args[i]); arg != e->args[i]) edit().args[i] = std::move(arg);
        }
        return copy != nullptr ? copy : e;
    }

    void Value(AstValue& v) {
        if (v.type != ValueType::kParam) return;
        if (const HoleValue* hole = Take(v); hole != nullptr) {
            const std::uint32_t at = v.byte_offset;
            v = hole->value;
            v.byte_offset = at;  // the marker's byte (EA-R3)
        }
    }

    // Marks the hole reached and answers its value, or null when probing.
    const HoleValue* Take(const AstValue& hole) {
        const auto index = static_cast<std::size_t>(hole.int_val);
        if (index >= reached_.size()) return nullptr;
        reached_[index] = true;
        if (values_ == nullptr) return nullptr;
        ++filled_;
        return &(*values_)[index];
    }

    const std::vector<HoleValue>* values_;
    std::vector<bool> reached_;
    std::uint32_t filled_ = 0;
};

}  // namespace

StatusOr<HoleValue> HoleValueOfLiteral(std::string_view literal) {
    // Read by the lexer itself, and taken as `ParseValue` takes the token.
    Lexer lexer(literal);
    const Token tok = lexer.Next();
    if (lexer.Next().type != TokenType::kEof ||
        (tok.type != TokenType::kIntLit && tok.type != TokenType::kNumLit &&
         tok.type != TokenType::kStrLit)) {
        return Status::InvalidArgument("not one value literal: '" + std::string(literal) + "'");
    }
    HoleValue out;
    if (tok.type == TokenType::kIntLit) {
        out.value.type = ValueType::kInt;
        out.value.int_val = tok.int_val;
        out.value.raw_int_text = std::string(tok.text);
    } else {
        out.value.type = ValueType::kStr;
        out.value.str_val = std::string(tok.text);
        out.bare_numeric = tok.type == TokenType::kNumLit;
    }
    return out;
}

HoleValue HoleValueOfString(std::string value) {
    HoleValue out;
    out.value.type = ValueType::kStr;
    out.value.str_val = std::move(value);
    return out;
}

std::optional<KeptParse> KeptParse::Of(std::string_view sql) {
    if (sql.size() > kMaxText || !HolesDelimited(sql)) return std::nullopt;
    Parser parser(sql);
    parser.AdmitHoles();
    auto parsed = parser.Parse();
    if (!parsed.ok()) return std::nullopt;
    const Statement& stmt = parsed.value();
    if (!std::holds_alternative<SelectStmt>(stmt) && !std::holds_alternative<InsertStmt>(stmt) &&
        !std::holds_alternative<UpdateStmt>(stmt) && !std::holds_alternative<DeleteStmt>(stmt)) {
        return std::nullopt;
    }
    const auto fingerprint = parser.fingerprint();
    if (!fingerprint.has_value() || fingerprint->param_count != parser.holes_read()) {
        return std::nullopt;  // a `?` some production read other than as a value
    }

    KeptParse kept;
    kept.statement_ = std::move(parsed.value());
    kept.holes_ = parser.holes_read();
    kept.fingerprint_ = *fingerprint;
    for (const StatementArgument& arg : ArgumentsOf(sql)) {
        kept.arguments_.emplace_back(arg.kind, std::string(arg.text));
    }
    if (!kept.Fill(kept.statement_, nullptr)) return std::nullopt;
    return kept;
}

bool KeptParse::Fill(Statement& stmt, const std::vector<HoleValue>* values) const {
    HoleWalk walk(values, holes_);
    walk.Walk(stmt);
    return walk.all_reached();
}

StatusOr<std::shared_ptr<const BoundStatement>> Bind(std::shared_ptr<const KeptParse> kept,
                                                     std::vector<HoleValue> values,
                                                     std::string text) {
    if (values.size() != kept->holes_) {
        return Status::InvalidArgument(std::to_string(values.size()) +
                                       " values were bound to a statement with " +
                                       std::to_string(kept->holes_) + " '?' placeholders");
    }
    // Built in place and never moved: `arguments` views `argument_texts`.
    auto bound = std::make_shared<BoundStatement>(BoundStatement::Key{});
    bound->text = std::move(text);
    bound->argument_texts.reserve(kept->arguments_.size());
    std::vector<StatementArgument::Kind> kinds;
    kinds.reserve(kept->arguments_.size());
    std::size_t next = 0;
    for (const auto& [kind, arg_text] : kept->arguments_) {
        if (kind == StatementArgument::Kind::kUnbound && next < values.size()) {
            // The token the literal lexes to: an integer's digits, a string's
            // or a numeric's text (`ArgumentsOf`'s kinds).
            const AstValue& v = values[next++].value;
            const bool integer = v.type == ValueType::kInt;
            kinds.push_back(integer ? StatementArgument::Kind::kInt : StatementArgument::Kind::kStr);
            bound->argument_texts.push_back(integer ? v.raw_int_text : v.str_val);
        } else {
            kinds.push_back(kind);
            bound->argument_texts.push_back(arg_text);
        }
    }
    bound->arguments.reserve(kinds.size());
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        bound->arguments.push_back({kinds[i], bound->argument_texts[i]});
    }
    bound->fingerprint = kept->fingerprint_;
    bound->fingerprint.arg_hash = ArgumentHashOf(bound->arguments);
    bound->fingerprint.literal_count += bound->fingerprint.param_count;
    bound->fingerprint.param_count = 0;
    bound->values = std::move(values);
    bound->kept = std::move(kept);
    return std::shared_ptr<const BoundStatement>(std::move(bound));
}

Statement BoundStatement::Materialize() const {
    Statement stmt = kept->statement_;
    // Every hole was reached when the parse was kept, and the walk is the
    // same walk, so this fill cannot miss one.
    (void)kept->Fill(stmt, &values);
    return stmt;
}

}  // namespace kds::parser
