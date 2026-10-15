#include <atomic>
#include "kds/parser/parser.hpp"

#include "kds/catalog/rows.hpp"
#include "kds/catalog/schema.hpp"  // kMaxSpareSlots (BW-R1)

#include <algorithm>
#include <cctype>
#include <charconv>
#include <utility>

namespace kds::parser {

namespace {
// SUS-1's bypass (parser.hpp).
//
// **Atomic**: `SuspendHeapStorageForTest` writes it from a cell while a
// multi-threaded reactor fixture may parse on another thread, a race tsan
// reports (AS-R6 runs under tsan). Relaxed: nothing is published through it.
std::atomic<bool> g_heap_storage_allowed_for_test{false};
}  // namespace

void SetHeapStorageAllowedForTest(bool allowed) noexcept {
    g_heap_storage_allowed_for_test.store(allowed, std::memory_order_relaxed);
}

bool HeapStorageAllowedForTest() noexcept {
    return g_heap_storage_allowed_for_test.load(std::memory_order_relaxed);
}

namespace {

bool IEquals(std::string_view a, std::string_view b);

// A contextual word - `AND`, `OR` - is an identifier token spelled that way.
bool IsWord(const Token& t, std::string_view word) {
    return t.type == TokenType::kIdent && IEquals(t.text, word);
}

// The comparison that reads the same with its sides swapped.
CompareOp MirroredOp(CompareOp op) {
    switch (op) {
        case CompareOp::kLt: return CompareOp::kGt;
        case CompareOp::kLte: return CompareOp::kGte;
        case CompareOp::kGt: return CompareOp::kLt;
        case CompareOp::kGte: return CompareOp::kLte;
        default: return op;  // =, != and the forms no literal stands left of
    }
}

BoolExpr Leaf(Condition cond, std::uint32_t at) {
    BoolExpr leaf;
    leaf.byte_offset = at;
    leaf.leaf = std::move(cond);
    return leaf;
}

BoolExpr Not(BoolExpr child, std::uint32_t at) {
    BoolExpr node;
    node.op = BoolExpr::Op::kNot;
    node.byte_offset = at;
    node.children.push_back(std::move(child));
    return node;
}

// The subquery cap, checked before the recursion it bounds, so the refused
// level is the one named and no frame for it is pushed. Spec section 2 puts
// it at 4.
Status CheckSubqueryDepth(std::uint32_t depth, std::uint32_t open_at) {
    if (depth + 1 <= kMaxSubqueryDepth) return Status::OK();
    return Status::Unsupported("subquery nesting deeper than " +
                               std::to_string(kMaxSubqueryDepth) + " is not supported (byte " +
                               std::to_string(open_at) + ")");
}

// A boolean combination as the one conjunct it is (PredicateKind::kBoolTree).
Condition TreeCondition(BoolExpr node) {
    Condition cond;
    cond.kind = PredicateKind::kBoolTree;
    cond.tree = std::make_shared<const BoolExpr>(std::move(node));
    return cond;
}

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
// `AVG`'s type rule - decimal columns only (aggregate.md §10, 2026-08-07) -
// is the compiler's `CheckAggregateArgType`, with every other per-type
// aggregate rule.
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
        // The byte, as `ExpectToken` gives it (BW-Q12.2 of
        // `instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`).
        return Status::InvalidArgument("expected '" + std::string(keyword) + "', got '" +
                                        std::string(Describe(tok)) + "' at byte " +
                                        std::to_string(tok.byte_offset));
    }
    return Status::OK();
}

Status Parser::ExpectToken(TokenType type, std::string_view desc) {
    Token tok = lexer_.Next();
    if (tok.type != type) {
        // **The byte, on every refusal this function produces** (the
        // engine-wide position rule): many productions route their arity
        // errors through here (`char(8, 1)`, `decimal(8)`), so it is fixed
        // once rather than at each call site.
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
                                        std::string(Describe(tok)) + "' at byte " +
                                        std::to_string(tok.byte_offset));
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

// A type name and its optional arguments. Recognized by the paren rather
// than by the name, so a type that takes no arguments refuses them here
// instead of each type name needing its own production - and the arity is
// decided by the name *inside* that branch, so `char(8, 1)` fails on the
// comma with a byte rather than on a count nobody wrote. A bare `decimal`
// is the caller's to refuse, because the message that is useful differs: a
// column names itself, a CAST names its own byte.
Status Parser::ParseTypeSpec(TypeSpec& out) {
    out.byte_offset = lexer_.Peek().byte_offset;
    auto name = ParseIdent();
    if (!name.ok()) return name.status();
    out.name = std::move(name.value());

    if (lexer_.Peek().type == TokenType::kLParen) {
        const std::uint32_t paren_at = lexer_.Peek().byte_offset;
        const bool takes_precision = IEquals(out.name, "DECIMAL");
        const bool takes_width = IEquals(out.name, "CHAR") || IEquals(out.name, "VARCHAR");
        if (!takes_precision && !takes_width) {
            return Status::InvalidArgument("type '" + out.name + "' takes no arguments (byte " +
                                           std::to_string(paren_at) + ")");
        }
        lexer_.Next();  // consume '('
        out.arg_byte_offset = lexer_.Peek().byte_offset;

        if (takes_width) {
            auto width = ParseTypeArgument("width");
            if (!width.ok()) return width.status();
            if (Status s = ExpectToken(TokenType::kRParen, "')' after a character type's width");
                !s.ok()) {
                return s;
            }
            out.has_width = true;
            out.width = width.value();
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
            out.has_precision = true;
            out.precision = precision.value();
            out.scale = scale.value();
        }
    }
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
    if (inside_value_subquery_) return;
    if (lexer_.Peek().type == TokenType::kSemicolon) {
        lexer_.Next();
    }
}

StatusOr<AstValue> Parser::ParseValue() {
    Token tok = lexer_.Next();

    // Every literal carries where it was written, so a later stage's
    // refusal (the compiler's type coercion, types.md §3.1) can name the
    // byte. Nothing compares it (AstValue::byte_offset).
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
            // Accepted by **no** production (`CREATE PATTERN`, its one
            // user, was withdrawn 2026-08-31). The token stays reserved for
            // the extended protocol's named binds (D4) and is refused here
            // by name and position rather than as "expected value".
            return Status::Unsupported("parameter '$" + std::string(tok.text) +
                                       "' is not bindable in the newline protocol (byte " +
                                       std::to_string(tok.byte_offset) + ")");
        default:
            // A call where a value is read - a BETWEEN bound, a SET value,
            // an INSERT value: understood and not built (AP-S4), refused by
            // name rather than as a malformed value.
            if (tok.type == TokenType::kIdent && lexer_.Peek().type == TokenType::kLParen) {
                return Status::NotImplemented(
                    "a function call is supported only in a WHERE comparison and a SET value (byte " +
                    std::to_string(tok.byte_offset) + ")");
            }
            return Status::InvalidArgument(
                "expected value (integer, number, 'string', or NULL), got '" +
                std::string(Describe(tok)) + "' (byte " + std::to_string(tok.byte_offset) + ")");
    }
}

// ---- A SET value (BJ-S2, BJ-S5) -------------------------------------------
//
// Families 1-3 of BJ-R1: column references to the target relation,
// literals, parentheses, unary `+`/`-`, binary `+ - * / %` and `||`; the
// comparisons, `AND`, `OR`, `NOT` and `IS [NOT] NULL` as boolean values;
// `CASE`, `COALESCE`, `NULLIF`, `GREATEST`, `LEAST`; `CAST` and `::`; the
// scalar functions (BJ-S6) and an uncorrelated scalar subquery (BJ-S7).
// Every other form is refused `NotImplemented` at its byte, never parsed
// and dropped: a row subquery and the comparison forms BJ leaves out (`IN`,
// `BETWEEN`, `LIKE`, `EXISTS`).
// Precedence is PostgreSQL's, tightest first: `::`, unary sign, `* / %`,
// `+ -`, `||`, the comparisons (which do not chain), `IS`, `NOT`, `AND`,
// `OR`.

namespace {

constexpr std::uint32_t kMaxExprDepth = 64;
// The height of a tree, counting a left-leaning chain (`a+b+c+...`) that the
// nesting depth above does not see. Every stage that walks the tree - the
// destructor included - recurses once per level.
constexpr std::uint32_t kMaxExprHeight = 256;
// The nodes a typed tree holds, a shared child counted per reference
// (`Expr::size`): what typing and every row's evaluation cost.
constexpr std::uint32_t kMaxExprSize = 4096;

Status ExprTooDeep(std::uint32_t byte) {
    return Status::Unsupported("an expression nested deeper than " +
                               std::to_string(kMaxExprDepth) + " levels, taller than " +
                               std::to_string(kMaxExprHeight) + " or larger than " +
                               std::to_string(kMaxExprSize) + " nodes is not supported (byte " +
                               std::to_string(byte) + ")");
}

bool TooBig(const Expr& e) { return e.height > kMaxExprHeight || e.size > kMaxExprSize; }

// A sum that cannot wrap: past the bound the exact figure does not matter.
std::uint32_t AddSize(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(a) + b, static_cast<std::uint64_t>(kMaxExprSize) + 1));
}

// Holds one level of the parser's nesting count for its scope.
struct DepthScope {
    std::uint32_t& depth;
    explicit DepthScope(std::uint32_t& d) : depth(d) { ++depth; }
    ~DepthScope() { --depth; }
};

std::shared_ptr<const Expr> MakeLiteral(AstValue v, bool bare_numeric) {
    auto e = std::make_shared<Expr>();
    e->kind = Expr::Kind::kLiteral;
    e->byte_offset = v.byte_offset;
    e->literal = std::move(v);
    e->bare_numeric = bare_numeric;
    e->height = 1;
    return e;
}

std::shared_ptr<const Expr> MakeBinary(ExprOp op, std::uint32_t op_at,
                                       std::shared_ptr<const Expr> lhs,
                                       std::shared_ptr<const Expr> rhs) {
    auto e = std::make_shared<Expr>();
    e->kind = Expr::Kind::kBinary;
    e->byte_offset = lhs->byte_offset;
    e->op_byte_offset = op_at;
    e->op = op;
    e->height = std::max(lhs->height, rhs->height) + 1;
    e->size = AddSize(AddSize(lhs->size, rhs->size), 1);
    e->lhs = std::move(lhs);
    e->rhs = std::move(rhs);
    return e;
}

std::shared_ptr<const Expr> MakeCall(ExprOp op, std::uint32_t at, std::uint32_t op_at,
                                     std::vector<std::shared_ptr<const Expr>> args) {
    auto e = std::make_shared<Expr>();
    e->kind = Expr::Kind::kCall;
    e->byte_offset = at;
    e->op_byte_offset = op_at;
    e->op = op;
    std::uint32_t tallest = 0;
    std::uint32_t size = 1;
    for (const auto& a : args) {
        tallest = std::max(tallest, a->height);
        size = AddSize(size, a->size);
    }
    e->height = tallest + 1;
    e->size = size;
    e->args = std::move(args);
    return e;
}

std::shared_ptr<const Expr> MakeUnary(ExprOp op, std::uint32_t at, std::uint32_t op_at,
                                      std::shared_ptr<const Expr> operand) {
    auto e = std::make_shared<Expr>();
    e->kind = Expr::Kind::kUnary;
    e->byte_offset = at;
    e->op_byte_offset = op_at;
    e->op = op;
    e->height = operand->height + 1;
    e->size = AddSize(operand->size, 1);
    e->lhs = std::move(operand);
    return e;
}

std::shared_ptr<const Expr> MakeCast(std::uint32_t at, std::uint32_t op_at,
                                     std::shared_ptr<const Expr> operand, TypeSpec to) {
    auto e = std::make_shared<Expr>();
    e->kind = Expr::Kind::kUnary;
    e->byte_offset = at;
    e->op_byte_offset = op_at;
    e->op = ExprOp::kCast;
    e->height = operand->height + 1;
    e->size = AddSize(operand->size, 1);
    e->lhs = std::move(operand);
    e->cast_to = std::move(to);
    return e;
}

// A comparison operator's token, or nothing: the one list of them.
std::optional<ExprOp> ComparisonOf(TokenType t) {
    switch (t) {
        case TokenType::kEq: return ExprOp::kEq;
        case TokenType::kNeq: return ExprOp::kNe;
        case TokenType::kLt: return ExprOp::kLt;
        case TokenType::kLte: return ExprOp::kLe;
        case TokenType::kGt: return ExprOp::kGt;
        case TokenType::kGte: return ExprOp::kGe;
        default: return std::nullopt;
    }
}

// The n-ary calls that are BJ-S5's, by name (CAST has its own production).
std::optional<ExprOp> NaryOf(std::string_view name) {
    if (IEquals(name, "COALESCE")) return ExprOp::kCoalesce;
    if (IEquals(name, "NULLIF")) return ExprOp::kNullif;
    if (IEquals(name, "GREATEST")) return ExprOp::kGreatest;
    if (IEquals(name, "LEAST")) return ExprOp::kLeast;
    return std::nullopt;
}

// The most arguments one call or CASE takes, so a statement cannot make the
// parser build a list as wide as its text is long.
constexpr std::size_t kMaxExprArgs = 128;

// A token after which a SET value is complete: the next assignment, the
// statement's end, a row value's `)`, or the WHERE.
std::string At(std::uint32_t byte) { return " (byte " + std::to_string(byte) + ")"; }

bool EndsSetValue(const Token& t) {
    return t.type == TokenType::kComma || t.type == TokenType::kEof ||
           t.type == TokenType::kSemicolon || t.type == TokenType::kRParen ||
           (t.type == TokenType::kIdent && IEquals(t.text, "WHERE"));
}

// A token after which a query-grammar value is complete (BU-S4): a clause
// word, a join word, a sort direction. Only what decides whether `case` is
// a column reads it.
bool EndsQueryValue(const Token& t) {
    if (t.type == TokenType::kKeyword) {
        return t.kw == Keyword::kJoin || t.kw == Keyword::kOn || t.kw == Keyword::kLeft ||
               t.kw == Keyword::kRight || t.kw == Keyword::kFull || t.kw == Keyword::kOuter;
    }
    if (t.type != TokenType::kIdent) return false;
    for (std::string_view w : {"FROM", "WHERE", "GROUP", "HAVING", "ORDER", "LIMIT", "OFFSET",
                               "UNION", "INTERSECT", "EXCEPT", "INNER", "CROSS", "USING", "ASC",
                               "DESC"}) {
        if (IEquals(t.text, w)) return true;
    }
    return false;
}

// `IN`, `BETWEEN`, `LIKE`: comparison forms BJ does not build (BJ-R1 family
// 2), refused by name. None can open a value, so none starts a CASE operand.
bool IsUnbuiltComparison(const Token& t) {
    return (t.type == TokenType::kKeyword && (t.kw == Keyword::kIn || t.kw == Keyword::kBetween)) ||
           (t.type == TokenType::kIdent && IEquals(t.text, "LIKE"));
}

// A `-` the lexer fused into the literal that follows it (`v -1`) is, in
// binary position, a minus and the literal's magnitude (BJ-Q9 (a)).
bool IsSignedLiteral(const Token& tok) {
    return (tok.type == TokenType::kIntLit && tok.negative) ||
           (tok.type == TokenType::kNumLit && !tok.text.empty() && tok.text[0] == '-');
}

// The magnitude of a signed literal token, as the literal it would have
// been without its sign. Read from the digits, never from the fused value,
// so `INT64_MIN`'s magnitude survives (BJ-Q9).
std::shared_ptr<const Expr> MagnitudeOf(const Token& tok) {
    AstValue v;
    v.byte_offset = tok.byte_offset + 1;  // past the sign
    if (tok.type == TokenType::kIntLit) {
        v.type = ValueType::kInt;
        v.raw_int_text = std::string(tok.digits());
        std::uint64_t m = 0;
        for (char d : tok.digits()) m = m * 10 + static_cast<std::uint64_t>(d - '0');
        v.int_val = static_cast<std::int64_t>(m);  // wraps past INT64_MAX; the digits are the truth
        return MakeLiteral(std::move(v), /*bare_numeric=*/false);
    }
    v.type = ValueType::kStr;
    v.str_val = std::string(tok.text.substr(1));
    return MakeLiteral(std::move(v), /*bare_numeric=*/true);
}

}  // namespace

Status Parser::ParseSetValue(Assignment& a) {
    // **A bare literal is nearly every SET value**, and it should not pay for the
    // expression grammar: eleven nested levels and a heap node to read one
    // token and throw the node away (BJ's close measured the literal path
    // 1.6 µs slower). The literal is read here, and if nothing continues it
    // that is the answer; if an operator does, it is handed to the chain as its
    // first primary.
    switch (lexer_.Peek().type) {
        case TokenType::kIntLit:
        case TokenType::kNumLit:
        case TokenType::kStrLit:
        case TokenType::kNullLit:
        case TokenType::kParam:
        case TokenType::kNamedParam: {
            const bool bare_numeric = lexer_.Peek().type == TokenType::kNumLit;
            auto literal = ParseValue();
            if (!literal.ok()) return literal.status();
            const Token& next = lexer_.Peek();
            if (EndsSetValue(next)) {
                a.val = std::move(literal.value());
                return Status::OK();
            }
            pending_primary_ = MakeLiteral(std::move(literal.value()), bare_numeric);
            break;
        }
        default: break;
    }
    auto expr = ParseExprOr();
    if (!expr.ok()) return expr.status();

    const Expr& e = *expr.value();
    // One bare literal keeps today's shape and today's gate. A parenthesised
    // one does not: its node starts at the `(`, the literal's own byte is
    // later, and the two differ.
    if (e.kind == Expr::Kind::kLiteral && e.byte_offset == e.literal.byte_offset) {
        a.val = e.literal;
        return Status::OK();
    }
    a.expr = std::move(expr.value());
    return Status::OK();
}

StatusOr<std::shared_ptr<const Expr>> Parser::ParseExprOr() {
    auto lhs = ParseExprAnd();
    if (!lhs.ok()) return lhs;
    std::shared_ptr<const Expr> acc = std::move(lhs.value());
    while (IsWord(lexer_.Peek(), "OR")) {
        const Token op = lexer_.Next();
        auto rhs = ParseExprAnd();
        if (!rhs.ok()) return rhs;
        acc = MakeBinary(ExprOp::kOr, op.byte_offset, std::move(acc), std::move(rhs.value()));
        if (TooBig(*acc)) return ExprTooDeep(acc->op_byte_offset);
    }
    return acc;
}

StatusOr<std::shared_ptr<const Expr>> Parser::ParseExprAnd() {
    auto lhs = ParseExprNot();
    if (!lhs.ok()) return lhs;
    std::shared_ptr<const Expr> acc = std::move(lhs.value());
    while (IsWord(lexer_.Peek(), "AND")) {
        const Token op = lexer_.Next();
        auto rhs = ParseExprNot();
        if (!rhs.ok()) return rhs;
        acc = MakeBinary(ExprOp::kAnd, op.byte_offset, std::move(acc), std::move(rhs.value()));
        if (TooBig(*acc)) return ExprTooDeep(acc->op_byte_offset);
    }
    return acc;
}

StatusOr<std::shared_ptr<const Expr>> Parser::ParseExprNot() {
    const Token& t = lexer_.Peek();
    if (pending_primary_ != nullptr || t.type != TokenType::kKeyword || t.kw != Keyword::kNot) {
        return ParseExprIs();
    }
    if (expr_depth_ >= kMaxExprDepth) return ExprTooDeep(t.byte_offset);
    const Token op = lexer_.Next();
    StatusOr<std::shared_ptr<const Expr>> operand = Status::OK();
    {
        DepthScope scope(expr_depth_);
        operand = ParseExprNot();
    }
    if (!operand.ok()) return operand;
    auto e = MakeUnary(ExprOp::kNot, op.byte_offset, op.byte_offset, std::move(operand.value()));
    if (TooBig(*e)) return ExprTooDeep(op.byte_offset);
    return e;
}

StatusOr<std::shared_ptr<const Expr>> Parser::ParseExprIs() {
    auto lhs = ParseExprComparison();
    if (!lhs.ok()) return lhs;
    std::shared_ptr<const Expr> acc = std::move(lhs.value());
    while (lexer_.Peek().type == TokenType::kIdent && IEquals(lexer_.Peek().text, "IS")) {
        const Token is = lexer_.Next();
        bool negated = false;
        if (lexer_.Peek().type == TokenType::kKeyword && lexer_.Peek().kw == Keyword::kNot) {
            lexer_.Next();
            negated = true;
        }
        if (lexer_.Peek().type != TokenType::kNullLit) {
            // `IS [NOT] TRUE`, `IS DISTINCT FROM`: the standard's other
            // tests, outside BJ's list (BJ-R1 family 2).
            return Status::NotImplemented(
                std::string("only IS [NOT] NULL is supported in ") +
                (query_mode_ ? "an expression" : "a SET value") + " (byte " +
                std::to_string(lexer_.Peek().byte_offset) + ")");
        }
        lexer_.Next();
        const std::uint32_t at = acc->byte_offset;  // read before acc is moved from
        acc = MakeUnary(negated ? ExprOp::kIsNotNull : ExprOp::kIsNull, at, is.byte_offset,
                        std::move(acc));
        if (TooBig(*acc)) return ExprTooDeep(acc->op_byte_offset);
    }
    return acc;
}

StatusOr<std::shared_ptr<const Expr>> Parser::ParseExprComparison() {
    auto lhs = ParseExprConcat();
    if (!lhs.ok()) return lhs;
    const Token& t = lexer_.Peek();
    const std::optional<ExprOp> compare = ComparisonOf(t.type);
    if (!compare.has_value() && query_mode_) return ParseExprQueryComparison(std::move(lhs.value()));
    if (!compare.has_value()) {
        // The comparison forms BJ does not build, named at their byte rather
        // than left to a generic complaint about what follows.
        const bool not_form = t.type == TokenType::kKeyword && t.kw == Keyword::kNot;
        if (IsUnbuiltComparison(t) || not_form) {
            return Status::NotImplemented("'" + std::string(t.text) +
                                          "' in a SET value is not supported (byte " +
                                          std::to_string(t.byte_offset) + ")");
        }
        return lhs;
    }
    const ExprOp op = *compare;
    const Token tok = lexer_.Next();
    // `a = NOT b`: NOT binds looser than a comparison but may still open its
    // right operand, as in PostgreSQL - `a = NOT (b)`, and `a = NOT b AND c`
    // is `(a = NOT b) AND c`.
    const Token& after = lexer_.Peek();
    auto rhs = after.type == TokenType::kKeyword && after.kw == Keyword::kNot ? ParseExprNot()
                                                                               : ParseExprConcat();
    if (!rhs.ok()) return rhs;
    if (ComparisonOf(lexer_.Peek().type).has_value()) {
        return Status::InvalidArgument("comparison operators do not chain; parenthesise one "
                                       "(byte " +
                                       std::to_string(lexer_.Peek().byte_offset) + ")");
    }
    auto e = MakeBinary(op, tok.byte_offset, std::move(lhs.value()), std::move(rhs.value()));
    if (TooBig(*e)) return ExprTooDeep(e->op_byte_offset);
    return e;
}

StatusOr<std::shared_ptr<const Expr>> Parser::ParseExprConcat() {
    auto lhs = ParseExprAdditive();
    if (!lhs.ok()) return lhs;
    std::shared_ptr<const Expr> acc = std::move(lhs.value());
    while (lexer_.Peek().type == TokenType::kConcat) {
        const Token op = lexer_.Next();
        auto rhs = ParseExprAdditive();
        if (!rhs.ok()) return rhs;
        acc = MakeBinary(ExprOp::kConcat, op.byte_offset, std::move(acc), std::move(rhs.value()));
        if (TooBig(*acc)) return ExprTooDeep(acc->op_byte_offset);
    }
    return acc;
}

StatusOr<std::shared_ptr<const Expr>> Parser::ParseExprAdditive() {
    auto first = ParseExprMultiplicative(nullptr);
    if (!first.ok()) return first;
    std::shared_ptr<const Expr> acc = std::move(first.value());
    for (;;) {
        const Token& t = lexer_.Peek();
        if (t.type == TokenType::kPlus || t.type == TokenType::kMinus) {
            const Token op = lexer_.Next();
            auto rhs = ParseExprMultiplicative(nullptr);
            if (!rhs.ok()) return rhs;
            acc = MakeBinary(op.type == TokenType::kPlus ? ExprOp::kAdd : ExprOp::kSub,
                             op.byte_offset, std::move(acc), std::move(rhs.value()));
            if (TooBig(*acc)) return ExprTooDeep(acc->op_byte_offset);
        } else if (IsSignedLiteral(t)) {
            // `v -1`: the minus is the literal's sign, and what follows it
            // is the left operand of any `*` that comes next.
            const Token lit = lexer_.Next();
            auto magnitude = ParseCastSuffix(MagnitudeOf(lit));
            if (!magnitude.ok()) return magnitude;
            auto rhs = ParseExprMultiplicative(std::move(magnitude.value()));
            if (!rhs.ok()) return rhs;
            acc = MakeBinary(ExprOp::kSub, lit.byte_offset, std::move(acc),
                             std::move(rhs.value()));
            if (TooBig(*acc)) return ExprTooDeep(acc->op_byte_offset);
        } else {
            return acc;
        }
    }
}

StatusOr<std::shared_ptr<const Expr>> Parser::ParseExprMultiplicative(
    std::shared_ptr<const Expr> first) {
    std::shared_ptr<const Expr> acc = std::move(first);
    if (acc == nullptr) {
        auto lhs = ParseExprUnary();
        if (!lhs.ok()) return lhs;
        acc = std::move(lhs.value());
    }
    for (;;) {
        const TokenType t = lexer_.Peek().type;
        ExprOp op;
        if (t == TokenType::kStar) {
            op = ExprOp::kMul;
        } else if (t == TokenType::kSlash) {
            op = ExprOp::kDiv;
        } else if (t == TokenType::kPercent) {
            op = ExprOp::kMod;
        } else {
            return acc;
        }
        const Token tok = lexer_.Next();
        auto rhs = ParseExprUnary();
        if (!rhs.ok()) return rhs;
        acc = MakeBinary(op, tok.byte_offset, std::move(acc), std::move(rhs.value()));
        if (TooBig(*acc)) return ExprTooDeep(acc->op_byte_offset);
    }
}

StatusOr<std::shared_ptr<const Expr>> Parser::ParseExprUnary() {
    const Token& t = lexer_.Peek();
    if (pending_primary_ != nullptr || (t.type != TokenType::kPlus && t.type != TokenType::kMinus)) {
        return ParseExprPostfix();
    }

    if (expr_depth_ >= kMaxExprDepth) return ExprTooDeep(t.byte_offset);
    const Token op = lexer_.Next();
    StatusOr<std::shared_ptr<const Expr>> operand = Status::OK();
    {
        DepthScope scope(expr_depth_);
        operand = ParseExprUnary();
    }
    if (!operand.ok()) return operand;
    auto e = MakeUnary(op.type == TokenType::kMinus ? ExprOp::kNeg : ExprOp::kPos, op.byte_offset,
                       op.byte_offset, std::move(operand.value()));
    if (TooBig(*e)) return ExprTooDeep(op.byte_offset);
    return e;
}

// `x::type`, the reference dialect's cast, binding tighter than a unary sign.
StatusOr<std::shared_ptr<const Expr>> Parser::ParseExprPostfix() {
    auto operand = ParseExprPrimary();
    if (!operand.ok()) return operand;
    return ParseCastSuffix(std::move(operand.value()));
}

// Any `::type` that follows `operand`. Also taken by the magnitude of a
// signed literal (`v -1::int64`), which never passes through the postfix level.
StatusOr<std::shared_ptr<const Expr>> Parser::ParseCastSuffix(
    std::shared_ptr<const Expr> operand) {
    std::shared_ptr<const Expr> acc = std::move(operand);
    while (lexer_.Peek().type == TokenType::kDoubleColon) {
        const Token op = lexer_.Next();
        TypeSpec spec;
        if (Status s = ParseTypeSpec(spec); !s.ok()) return s;
        const std::uint32_t at = acc->byte_offset;  // read before acc is moved from
        acc = MakeCast(at, op.byte_offset, std::move(acc), std::move(spec));
        if (TooBig(*acc)) return ExprTooDeep(acc->op_byte_offset);
    }
    return acc;
}

// `NAME ( expr [, expr ...] )` for the n-ary forms (COALESCE, NULLIF,
// GREATEST, LEAST) and `CAST ( expr AS type )`. The keyword and its `(` are
// already consumed.
StatusOr<std::shared_ptr<const Expr>> Parser::ParseExprCall(const Token& name) {
    if (expr_depth_ >= kMaxExprDepth) return ExprTooDeep(name.byte_offset);
    DepthScope scope(expr_depth_);

    if (IEquals(name.text, "CAST")) {
        auto operand = ParseExprOr();
        if (!operand.ok()) return operand;
        if (lexer_.Peek().type != TokenType::kKeyword || lexer_.Peek().kw != Keyword::kAs) {
            return Status::InvalidArgument("expected AS in CAST (byte " +
                                           std::to_string(lexer_.Peek().byte_offset) + ")");
        }
        lexer_.Next();
        TypeSpec spec;
        if (Status s = ParseTypeSpec(spec); !s.ok()) return s;
        if (Status s = ExpectToken(TokenType::kRParen, "')' after a CAST's type"); !s.ok()) {
            return s;
        }
        auto cast = MakeCast(name.byte_offset, name.byte_offset, std::move(operand.value()),
                             std::move(spec));
        if (TooBig(*cast)) return ExprTooDeep(name.byte_offset);
        return cast;
    }

    const ExprOp op = *NaryOf(name.text);  // the caller dispatched on this
    std::vector<std::shared_ptr<const Expr>> args;
    for (;;) {
        auto arg = ParseExprOr();
        if (!arg.ok()) return arg;
        args.push_back(std::move(arg.value()));
        if (args.size() > kMaxExprArgs) {
            return Status::Unsupported("more than " + std::to_string(kMaxExprArgs) +
                                       " arguments (byte " + std::to_string(name.byte_offset) + ")");
        }
        if (lexer_.Peek().type == TokenType::kComma) {
            lexer_.Next();
            continue;
        }
        break;
    }
    if (Status s = ExpectToken(TokenType::kRParen, "')' closing the argument list"); !s.ok()) {
        return s;
    }
    // At least one argument: an empty list was refused at its `)`.
    if (op == ExprOp::kNullif && args.size() != 2) {
        return Status::InvalidArgument("'" + std::string(name.text) +
                                       "' takes exactly 2 argument(s) (byte " +
                                       std::to_string(name.byte_offset) + ")");
    }
    auto call = MakeCall(op, name.byte_offset, name.byte_offset, std::move(args));
    if (TooBig(*call)) return ExprTooDeep(name.byte_offset);
    return call;
}

StatusOr<std::shared_ptr<const Expr>> Parser::ParseExprFunction(const Token& name) {
    if (expr_depth_ >= kMaxExprDepth) return ExprTooDeep(name.byte_offset);
    DepthScope scope(expr_depth_);

    std::vector<std::shared_ptr<const Expr>> args;
    const bool position = IEquals(name.text, "POSITION");
    if (lexer_.Peek().type != TokenType::kRParen) {
        for (;;) {
            // `POSITION(sub IN string)`: the first operand stops before `IN`,
            // which a comparison would otherwise refuse.
            auto arg = position && args.empty() ? ParseExprConcat() : ParseExprOr();
            if (!arg.ok()) return arg;
            args.push_back(std::move(arg.value()));
            if (args.size() > kMaxExprArgs) {
                return Status::Unsupported("more than " + std::to_string(kMaxExprArgs) +
                                           " arguments (byte " + std::to_string(name.byte_offset) +
                                           ")");
            }
            if (position && args.size() == 1) {
                if (lexer_.Peek().type != TokenType::kKeyword || lexer_.Peek().kw != Keyword::kIn) {
                    return Status::InvalidArgument("expected IN in POSITION (byte " +
                                                   std::to_string(lexer_.Peek().byte_offset) + ")");
                }
                lexer_.Next();
                continue;
            }
            if (!position && lexer_.Peek().type == TokenType::kComma) {
                lexer_.Next();
                continue;
            }
            break;
        }
    }
    if (Status s = ExpectToken(TokenType::kRParen, "')' closing the argument list"); !s.ok()) {
        return s;
    }
    // `MOD(a, b)` is `a % b`.
    if (IEquals(name.text, "MOD") && args.size() != 2) {
        return Status::InvalidArgument("'" + std::string(name.text) +
                                       "' takes 2 argument(s) (byte " +
                                       std::to_string(name.byte_offset) + ")");
    }
    if (IEquals(name.text, "MOD")) {
        auto e = std::make_shared<Expr>(*MakeBinary(ExprOp::kMod, name.byte_offset,
                                                    std::move(args[0]), std::move(args[1])));
        e->byte_offset = name.byte_offset;  // the expression starts at `MOD`
        if (TooBig(*e)) return ExprTooDeep(name.byte_offset);
        return std::shared_ptr<const Expr>(std::move(e));
    }
    auto call = std::make_shared<Expr>(
        *MakeCall(ExprOp::kFunction, name.byte_offset, name.byte_offset, std::move(args)));
    call->function = std::string(name.text);
    if (TooBig(*call)) return ExprTooDeep(name.byte_offset);
    return std::shared_ptr<const Expr>(std::move(call));
}

// `CASE [operand] WHEN a THEN b [WHEN ...] [ELSE e] END`. The `CASE` keyword
// is consumed. A simple CASE becomes the searched form, `WHEN operand = a`.
StatusOr<std::shared_ptr<const Expr>> Parser::ParseExprCase(const Token& kw) {
    if (expr_depth_ >= kMaxExprDepth) return ExprTooDeep(kw.byte_offset);
    DepthScope scope(expr_depth_);

    const auto IsWord = [&](std::string_view w) {
        return lexer_.Peek().type == TokenType::kIdent && IEquals(lexer_.Peek().text, w);
    };
    std::shared_ptr<const Expr> operand;
    if (!IsWord("WHEN")) {
        auto o = ParseExprOr();
        if (!o.ok()) return o;
        operand = std::move(o.value());
    }
    std::vector<std::shared_ptr<const Expr>> args;
    while (IsWord("WHEN")) {
        const Token when = lexer_.Next();
        auto cond = ParseExprOr();
        if (!cond.ok()) return cond;
        if (!IsWord("THEN")) {
            return Status::InvalidArgument("expected THEN in CASE (byte " +
                                           std::to_string(lexer_.Peek().byte_offset) + ")");
        }
        lexer_.Next();
        auto result = ParseExprOr();
        if (!result.ok()) return result;
        std::shared_ptr<const Expr> test = std::move(cond.value());
        if (operand != nullptr) {
            test = MakeBinary(ExprOp::kEq, when.byte_offset, operand, std::move(test));
        }
        args.push_back(std::move(test));
        args.push_back(std::move(result.value()));
        if (args.size() > kMaxExprArgs) {
            return Status::Unsupported("a CASE with more than " + std::to_string(kMaxExprArgs / 2) +
                                       " branches (byte " + std::to_string(kw.byte_offset) + ")");
        }
    }
    if (args.empty()) {
        return Status::InvalidArgument("CASE needs at least one WHEN (byte " +
                                       std::to_string(lexer_.Peek().byte_offset) + ")");
    }
    if (IsWord("ELSE")) {
        lexer_.Next();
        auto other = ParseExprOr();
        if (!other.ok()) return other;
        args.push_back(std::move(other.value()));
    }
    if (!IsWord("END")) {
        return Status::InvalidArgument("expected WHEN, ELSE or END in CASE (byte " +
                                       std::to_string(lexer_.Peek().byte_offset) + ")");
    }
    lexer_.Next();
    auto call = MakeCall(ExprOp::kCase, kw.byte_offset, kw.byte_offset, std::move(args));
    if (TooBig(*call)) return ExprTooDeep(kw.byte_offset);
    return call;
}

StatusOr<std::shared_ptr<const Expr>> Parser::ParseExprAfterIdent(const Token& tok) {
    const Token& after = lexer_.Peek();
    if (after.type == TokenType::kLParen) {
        // `CASE (b) WHEN ...`: no function is named `case` and no
        // column is followed by `(`, so this is a simple CASE whose
        // operand opens with a parenthesis - the one way to write a
        // negative operand, since `case -1` is a column's subtraction.
        if (IEquals(tok.text, "CASE")) return ParseExprCase(tok);
        if (AggFunc func; query_mode_ && AggFuncOf(tok.text, func)) {
            lexer_.Next();  // the '('
            return ParseExprAggregate(tok, func);
        }
        if (IEquals(tok.text, "CAST") || NaryOf(tok.text).has_value()) {
            lexer_.Next();  // the '('
            return ParseExprCall(tok);
        }
        // Every other call is a scalar function, BJ-S6's.
        lexer_.Next();  // the '('
        return ParseExprFunction(tok);
    }
    if (after.type == TokenType::kDot) {
        if (!query_mode_) {
            return Status::NotImplemented(
                "a qualified column in a SET value is not supported (byte " +
                std::to_string(tok.byte_offset) + ")");
        }
        lexer_.Next();  // the '.'
        auto name = ParseIdent();
        if (!name.ok()) return name.status();
        auto e = std::make_shared<Expr>();
        e->kind = Expr::Kind::kColumn;
        e->byte_offset = tok.byte_offset;
        e->qualifier = std::string(tok.text);
        e->column = std::move(name.value());
        return std::shared_ptr<const Expr>(std::move(e));
    }
    // `CASE` is not reserved, so a column may be named `case`; it
    // opens an expression only when what follows is not something
    // that ends or continues a value.
    const bool case_is_a_column =
        EndsSetValue(after) || IsSignedLiteral(after) ||
        after.type == TokenType::kPlus || after.type == TokenType::kMinus ||
        after.type == TokenType::kStar || after.type == TokenType::kSlash ||
        after.type == TokenType::kPercent || after.type == TokenType::kConcat ||
        ComparisonOf(after.type).has_value() || after.type == TokenType::kDoubleColon ||
        IsUnbuiltComparison(after) ||
        (after.type == TokenType::kKeyword && after.kw == Keyword::kAs) ||
        (query_mode_ && EndsQueryValue(after)) ||
        (after.type == TokenType::kIdent &&
         (IEquals(after.text, "AND") ||
          IEquals(after.text, "OR") || IEquals(after.text, "IS") ||
          IEquals(after.text, "THEN") || IEquals(after.text, "ELSE") ||
          IEquals(after.text, "END")));
    if (IEquals(tok.text, "CASE") && !case_is_a_column) return ParseExprCase(tok);
    auto e = std::make_shared<Expr>();
    e->kind = Expr::Kind::kColumn;
    e->byte_offset = tok.byte_offset;
    e->column = std::string(tok.text);
    return std::shared_ptr<const Expr>(std::move(e));
}

StatusOr<std::shared_ptr<const Expr>> Parser::ParseExprPrimary() {
    if (pending_primary_ != nullptr) {
        std::shared_ptr<const Expr> literal = std::move(pending_primary_);
        pending_primary_ = nullptr;
        return literal;
    }
    const Token tok = lexer_.Peek();
    switch (tok.type) {
        case TokenType::kIntLit:
        case TokenType::kNumLit:
        case TokenType::kStrLit:
        case TokenType::kNullLit:
        case TokenType::kParam:
        case TokenType::kNamedParam: {
            auto v = ParseValue();
            if (!v.ok()) return v.status();
            return MakeLiteral(std::move(v.value()), tok.type == TokenType::kNumLit);
        }
        case TokenType::kIdent:
            lexer_.Next();
            return ParseExprAfterIdent(tok);
        case TokenType::kLParen: {
            if (expr_depth_ >= kMaxExprDepth) return ExprTooDeep(tok.byte_offset);
            lexer_.Next();
            const Token& inner = lexer_.Peek();
            if (query_mode_ && inner.type == TokenType::kIdent && IEquals(inner.text, "SELECT")) {
                return ParseExprQuerySubquery(tok, ExprOp::kScalarSubquery, nullptr);
            }
            if (inner.type == TokenType::kIdent && IEquals(inner.text, "SELECT")) {
                // A scalar subquery (BJ-S7). It runs as a SELECT of its own, so
                // it is parsed as an outermost block (an aggregate is allowed);
                // what it nests is counted from here.
                std::uint32_t close_at = 0;
                const bool was_inside = std::exchange(inside_value_subquery_, true);
                auto sub = ParseSubqueryAfterOpen(0, &close_at);
                inside_value_subquery_ = was_inside;
                if (!sub.ok()) return sub.status();
                auto node = std::make_shared<Expr>();
                node->kind = Expr::Kind::kSubquery;
                node->byte_offset = tok.byte_offset;
                node->subquery = std::move(sub.value());
                node->subquery_sql =
                    std::string(sql_.substr(tok.byte_offset + 1, close_at - tok.byte_offset - 1));
                return std::shared_ptr<const Expr>(std::move(node));
            }
            StatusOr<std::shared_ptr<const Expr>> e = Status::OK();
            {
                DepthScope scope(expr_depth_);
                e = ParseExprOr();
            }
            if (!e.ok()) return e;
            if (Status s = ExpectToken(TokenType::kRParen, "')'"); !s.ok()) return s;
            // The node is re-stamped to start at the '(' so a parenthesised
            // literal is told from a bare one (ParseSetValue).
            auto copy = std::make_shared<Expr>(*e.value());
            copy->byte_offset = tok.byte_offset;
            return std::shared_ptr<const Expr>(std::move(copy));
        }
        case TokenType::kKeyword:
            if (tok.kw == Keyword::kExists && query_mode_) {
                lexer_.Next();
                if (lexer_.Peek().type != TokenType::kLParen) {
                    return Status::InvalidArgument("expected '(' after EXISTS (byte " +
                                                   std::to_string(lexer_.Peek().byte_offset) + ")");
                }
                const Token open = lexer_.Next();
                if (!IsWord(lexer_.Peek(), "SELECT")) {
                    return Status::InvalidArgument("expected SELECT after EXISTS ( (byte " +
                                                   std::to_string(lexer_.Peek().byte_offset) + ")");
                }
                auto node = ParseExprQuerySubquery(open, ExprOp::kExists, nullptr);
                if (!node.ok()) return node;
                auto e = std::make_shared<Expr>(*node.value());
                e->byte_offset = tok.byte_offset;
                return std::shared_ptr<const Expr>(std::move(e));
            }
            if (tok.kw == Keyword::kExists) {
                return Status::NotImplemented(
                    "'EXISTS' in a SET value is not supported (byte " +
                    std::to_string(tok.byte_offset) + ")");
            }
            [[fallthrough]];
        default:
            return Status::InvalidArgument(
                "expected a value or a column in a SET value, got '" +
                std::string(Describe(tok)) + "' (byte " + std::to_string(tok.byte_offset) + ")");
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

    if (Status s = CheckSubqueryDepth(depth, open_at); !s.ok()) return s;

    if (Status s = ExpectToken(TokenType::kLParen, "'(' before a subquery"); !s.ok()) return s;
    return ParseSubqueryAfterOpen(depth + 1);
}

StatusOr<std::shared_ptr<SelectStmt>> Parser::ParseSubqueryAfterOpen(std::uint32_t select_depth,
                                                                      std::uint32_t* close_at) {
    // Every caller has already told its `(` apart - a WHERE group (BU-S1),
    // an `IN` value list, a SET value - and calls this only where a subquery
    // is the one legal form, so anything but SELECT here is refused.
    const Token& head = lexer_.Peek();
    if (head.type != TokenType::kIdent || !IEquals(head.text, "SELECT")) {
        return Status::InvalidArgument("expected a subquery `(SELECT ...)`, got '" +
                                        std::string(Describe(head)) + "' at byte " +
                                        std::to_string(head.byte_offset));
    }
    lexer_.Next();  // consume SELECT

    auto inner = ParseSelect(select_depth);
    if (!inner.ok()) return inner.status();

    if (close_at != nullptr) *close_at = lexer_.Peek().byte_offset;
    if (Status s = ExpectToken(TokenType::kRParen, "')' closing a subquery"); !s.ok()) return s;
    return std::make_shared<SelectStmt>(std::move(inner.value()));
}

StatusOr<BoolExpr> Parser::ParseOneCondition(std::uint32_t depth) {
    Condition cond;

    // Leading `EXISTS`: a predicate with no column on the left. `NOT
    // EXISTS` never reaches here - `ParseBoolNot` reads the `NOT` and makes
    // the pair one leaf, since a bare `NOT` there is the boolean operator.
    const Token& lead = lexer_.Peek();
    const std::uint32_t at = lead.byte_offset;
    if (lead.type == TokenType::kKeyword && lead.kw == Keyword::kExists) {
        lexer_.Next();
        cond.kind = PredicateKind::kExists;
        auto sub = ParseSubquery(depth);
        if (!sub.ok()) return sub.status();
        cond.subquery = std::move(sub.value());
        return Leaf(std::move(cond), at);
    }

    // A literal on the left (BU-S2): `100 < amount` is `amount > 100`, the
    // mirrored comparison, so everything past the parser sees one shape.
    if (lead.type == TokenType::kIntLit || lead.type == TokenType::kStrLit ||
        lead.type == TokenType::kNumLit || lead.type == TokenType::kNullLit) {
        auto val = ParseValue();
        if (!val.ok()) return val.status();
        auto op = ParseCompareOp();
        if (!op.ok()) return op.status();
        auto rhs = ParseColumnName();
        if (!rhs.ok()) return rhs.status();
        if (StartsFunctionCall(rhs.value())) {
            return Status::NotImplemented(
                "a function call compared with a literal on its left is not supported yet; "
                "write the call first (byte " + std::to_string(rhs.value().byte_offset) + ")");
        }
        cond.kind = PredicateKind::kCompareValue;
        cond.col = std::move(rhs.value());
        cond.op = MirroredOp(op.value());
        cond.rhs_kind = RhsKind::kLiteral;
        cond.val = std::move(val.value());
        return Leaf(std::move(cond), at);
    }

    // Qualified or not: a join's WHERE needs `a.x` to say which relation,
    // and a single-relation WHERE has always been bare `x`. Both spellings
    // reach the same ColumnName.
    auto col = ParseColumnName();
    if (!col.ok()) return col.status();
    cond.col = std::move(col.value());

    // A call on the left (AP-S4): `DATE(ts) = '2026-10-02'`.
    if (StartsFunctionCall(cond.col)) {
        auto call = ParseFunctionCall(std::move(cond.col));
        if (!call.ok()) return call.status();
        cond.col = ColumnName{};
        cond.lhs_fn = std::move(call.value());
        auto fn = ParseFunctionComparison(std::move(cond));
        if (!fn.ok()) return fn.status();
        return Leaf(std::move(fn.value()), at);
    }

    // `col NOT IN`, `col NOT BETWEEN`, `col NOT LIKE`: the form, negated.
    // `NOT IN (SELECT ...)` keeps the kind it has always had; the other two
    // become the boolean conjunct `NOT (<form>)` (BU-S2).
    bool negated = false;
    const Token& after_col = lexer_.Peek();
    if (after_col.type == TokenType::kKeyword && after_col.kw == Keyword::kNot) {
        const std::uint32_t not_at = after_col.byte_offset;
        lexer_.Next();
        const Token& form = lexer_.Peek();
        const bool known = (form.type == TokenType::kKeyword &&
                            (form.kw == Keyword::kIn || form.kw == Keyword::kBetween)) ||
                           IsWord(form, "LIKE");
        if (!known) {
            return Status::NotImplemented(
                "NOT after a column is supported only as `NOT IN`, `NOT BETWEEN` or `NOT LIKE`; "
                "a negated predicate is written `NOT <predicate>` (byte " +
                std::to_string(not_at) + ")");
        }
        negated = true;
    }
    const auto negate = [&cond, negated, at]() -> BoolExpr {
        BoolExpr leaf = Leaf(std::move(cond), at);
        return negated ? Not(std::move(leaf), at) : leaf;
    };

    // `col IN (SELECT ...)` / `col IN (v, ...)`, and their NOT.
    const Token& form = lexer_.Peek();
    if (form.type == TokenType::kKeyword && form.kw == Keyword::kIn) {
        lexer_.Next();
        const std::uint32_t open_at = lexer_.Peek().byte_offset;
        if (Status s = ExpectToken(TokenType::kLParen, "'(' after IN"); !s.ok()) return s;
        if (IsWord(lexer_.Peek(), "SELECT")) {
            if (Status s = CheckSubqueryDepth(depth, open_at); !s.ok()) return s;
            cond.kind = negated ? PredicateKind::kNotInSubquery : PredicateKind::kInSubquery;
            auto sub = ParseSubqueryAfterOpen(depth + 1);
            if (!sub.ok()) return sub.status();
            cond.subquery = std::move(sub.value());
            return Leaf(std::move(cond), at);
        }
        return ParseValueList(std::move(cond.col), negated, at);
    }

    // `col BETWEEN <low> AND <high>` (V08). Free to the fingerprint: a
    // keyword hashes as an identifier, which the corpus pins.
    if (form.type == TokenType::kKeyword && form.kw == Keyword::kBetween) {
        lexer_.Next();
        cond.kind = PredicateKind::kBetween;

        auto low = ParseValue();
        if (!low.ok()) return low.status();
        cond.val = std::move(low.value());

        const Token& and_tok = lexer_.Peek();
        if (!IsWord(and_tok, "AND")) {
            return Status::InvalidArgument("expected AND after the low bound of BETWEEN, got '" +
                                            std::string(Describe(and_tok)) + "' (byte " +
                                            std::to_string(and_tok.byte_offset) + ")");
        }
        lexer_.Next();

        auto high = ParseValue();
        if (!high.ok()) return high.status();
        cond.val_high = std::move(high.value());
        return negate();
    }

    // `col LIKE 'pattern'` (BU-S2, BU-Q6). `LIKE` is a contextual word, not
    // reserved. Its op travels only inside a boolean conjunct (`Flatten`).
    if (IsWord(form, "LIKE")) {
        lexer_.Next();
        if (lexer_.Peek().type != TokenType::kStrLit) {
            return Status::InvalidArgument("LIKE takes a string literal pattern (byte " +
                                            std::to_string(lexer_.Peek().byte_offset) + ")");
        }
        auto pattern = ParseValue();
        if (!pattern.ok()) return pattern.status();
        if (IsWord(lexer_.Peek(), "ESCAPE")) {
            return Status::NotImplemented("LIKE ... ESCAPE is not supported (byte " +
                                          std::to_string(lexer_.Peek().byte_offset) + ")");
        }
        cond.kind = PredicateKind::kCompareValue;
        cond.op = CompareOp::kLike;
        cond.rhs_kind = RhsKind::kLiteral;
        cond.val = std::move(pattern.value());
        return negate();
    }

    // `col IS [NOT] NULL` (docs/spec/null.md; NU5).
    auto is_null = ParseIsNull(cond.op);
    if (!is_null.ok()) return is_null.status();
    if (is_null.value()) return Leaf(std::move(cond), at);

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
        return Leaf(std::move(cond), at);
    }

    // A column on the right, rather than a value. This is how a
    // correlated subquery is written - `WHERE inner.col = outer.col` -
    // and without it §2's correlated forms could not be spelled at all.
    // One token of lookahead separates the two: a value literal is never
    // an identifier, and an identifier is never a value.
    cond.kind = PredicateKind::kCompareValue;
    if (Status s = ParseComparisonRhs(cond); !s.ok()) return s;
    return Leaf(std::move(cond), at);
}

// One token of lookahead separates a column from a value: a value literal
// is never an identifier, and an identifier is never a value. An
// identifier with `(` after it is a call (raft-marks-2026-10-02.md §5:
// `ts < NOW()`), and makes the whole condition a function comparison.
Status Parser::ParseComparisonRhs(Condition& cond) {
    if (lexer_.Peek().type == TokenType::kIdent) {
        auto rhs = ParseColumnName();
        if (!rhs.ok()) return rhs.status();
        if (StartsFunctionCall(rhs.value())) {
            auto call = ParseFunctionCall(std::move(rhs.value()));
            if (!call.ok()) return call.status();
            cond.kind = PredicateKind::kCompareFunction;
            cond.rhs_fn = std::move(call.value());
            return Status::OK();
        }
        cond.rhs_kind = RhsKind::kColumn;
        cond.rhs_col = std::move(rhs.value());
        return Status::OK();
    }
    auto val = ParseValue();
    if (!val.ok()) return val.status();
    cond.rhs_kind = RhsKind::kLiteral;
    cond.val = std::move(val.value());
    return Status::OK();
}

// `IS` is contextual - an unreserved word like REFERENCES, so it still
// names a column everywhere else and the fingerprint hashes it as the
// identifier it lexes as. No right-hand side: the op is the whole
// predicate.
StatusOr<bool> Parser::ParseIsNull(CompareOp& op) {
    const Token& is_tok = lexer_.Peek();
    if (is_tok.type != TokenType::kIdent || !IEquals(is_tok.text, "IS")) return false;
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
    op = negated ? CompareOp::kIsNotNull : CompareOp::kIsNull;
    return true;
}

bool Parser::StartsFunctionCall(const ColumnName& head) {
    AggFunc ignored;
    return lexer_.Peek().type == TokenType::kLParen && !head.qualified() &&
           !AggFuncOf(head.name, ignored);
}

StatusOr<FunctionCall> Parser::ParseFunctionCall(ColumnName head) {
    FunctionCall call;
    call.name = std::move(head.name);
    call.byte_offset = head.byte_offset;
    lexer_.Next();  // consume '('

    if (lexer_.Peek().type == TokenType::kRParen) {
        lexer_.Next();
        return call;
    }
    for (;;) {
        // A column, and only a column: neither shipped function takes a
        // literal, and a call inside a call is an expression tree this
        // grammar does not have (manual/sql/sql.md).
        const Token& arg_tok = lexer_.Peek();
        if (arg_tok.type != TokenType::kIdent) {
            return Status::NotImplemented(
                "a function's argument is a column reference (byte " +
                std::to_string(arg_tok.byte_offset) + ")");
        }
        auto arg = ParseColumnName();
        if (!arg.ok()) return arg.status();
        if (lexer_.Peek().type == TokenType::kLParen) {
            return Status::NotImplemented("a function call inside a function call (byte " +
                                          std::to_string(arg.value().byte_offset) + ")");
        }
        call.args.push_back(std::move(arg.value()));
        if (lexer_.Peek().type == TokenType::kComma) {
            lexer_.Next();
            continue;
        }
        if (Status s = ExpectToken(TokenType::kRParen, "')' closing a function's arguments");
            !s.ok()) {
            return s;
        }
        return call;
    }
}

StatusOr<Condition> Parser::ParseFunctionComparison(Condition cond) {
    cond.kind = PredicateKind::kCompareFunction;

    // A comparison and nothing else. `IN`, `NOT IN` and `BETWEEN` against a
    // call are understood and not built.
    if (const Token& next = lexer_.Peek();
        next.type == TokenType::kKeyword &&
        (next.kw == Keyword::kIn || next.kw == Keyword::kNot || next.kw == Keyword::kBetween)) {
        return Status::NotImplemented(
            "a function call is compared with =, !=, <, <=, >, >= or IS [NOT] NULL (byte " +
            std::to_string(next.byte_offset) + ")");
    }

    auto is_null = ParseIsNull(cond.op);
    if (!is_null.ok()) return is_null.status();
    if (is_null.value()) return cond;

    auto op = ParseCompareOp();
    if (!op.ok()) return op.status();
    cond.op = op.value();

    if (const Token& next = lexer_.Peek(); next.type == TokenType::kLParen) {
        return Status::NotImplemented("a function call compared with a subquery (byte " +
                                      std::to_string(next.byte_offset) + ")");
    }
    if (Status s = ParseComparisonRhs(cond); !s.ok()) return s;
    return cond;
}

namespace {

// BU-Q2: a boolean conjunct takes the caps a SET expression does. The
// nesting is counted across the whole statement (`Parser::bool_nest_`), as
// `expr_depth_` is, so a WHERE inside each of four subqueries shares one
// stack budget; the size is counted per conjunct, in conditions, and a flat
// top-level AND-list - flattened into ordinary conjuncts - is not one.
constexpr std::uint32_t kMaxBoolNest = 64;
constexpr std::uint32_t kMaxBoolConditions = 4096;

Status BoolTooBig(std::uint32_t byte) {
    return Status::Unsupported("a WHERE nested deeper than " + std::to_string(kMaxBoolNest) +
                               " levels, or an OR/NOT group of more than " +
                               std::to_string(kMaxBoolConditions) +
                               " conditions, is not supported (byte " + std::to_string(byte) +
                               ")");
}

std::uint32_t ConditionsIn(const BoolExpr& node) {
    if (node.op == BoolExpr::Op::kLeaf) return 1;
    std::uint32_t n = 0;
    for (const BoolExpr& child : node.children) n += ConditionsIn(child);
    return n;
}

// The top-level AND-list of a WHERE tree, into `out`: a leaf is an ordinary
// conjunct and an `AND` - parenthesized or not - is flattened, so a WHERE
// written without `OR` or `NOT` lowers exactly as it did before BU and keeps
// every access path (BU-R5). Anything else is one `kBoolTree` conjunct.
Status Flatten(BoolExpr node, std::vector<Condition>& out) {
    if (node.op == BoolExpr::Op::kLeaf) {
        // `LIKE` is never a flat conjunct: every reader of a flat comparison
        // - keys, ranges, the scan kernel - would have to learn an operator
        // none of them can use, so it is a one-leaf boolean conjunct instead
        // (BU-S2).
        if (node.leaf.kind == PredicateKind::kCompareValue && node.leaf.op == CompareOp::kLike) {
            out.push_back(TreeCondition(std::move(node)));
            return Status::OK();
        }
        out.push_back(std::move(node.leaf));
        return Status::OK();
    }
    if (node.op == BoolExpr::Op::kAnd) {
        for (BoolExpr& child : node.children) {
            if (Status s = Flatten(std::move(child), out); !s.ok()) return s;
        }
        return Status::OK();
    }
    if (ConditionsIn(node) > kMaxBoolConditions) return BoolTooBig(node.byte_offset);
    out.push_back(TreeCondition(std::move(node)));
    return Status::OK();
}

}  // namespace

StatusOr<std::vector<Condition>> Parser::ParseOptionalWhere(std::uint32_t depth) {
    std::vector<Condition> conds;

    const Token& peek = lexer_.Peek();
    if (peek.type != TokenType::kIdent || !IEquals(peek.text, "WHERE")) {
        return conds;  // no WHERE clause
    }
    lexer_.Next();  // consume WHERE

    auto tree = ParseBoolChain(depth, /*is_or=*/true);
    if (!tree.ok()) return tree.status();
    if (Status s = Flatten(std::move(tree.value()), conds); !s.ok()) return s;
    return conds;
}

StatusOr<std::vector<Condition>> Parser::ParseConditions() {
    std::vector<Condition> conds;
    auto tree = ParseBoolChain(0, /*is_or=*/true);
    if (!tree.ok()) return tree.status();
    if (Status s = Flatten(std::move(tree.value()), conds); !s.ok()) return s;
    if (const Token& tail = lexer_.Peek(); tail.type != TokenType::kEof) {
        return Status::InvalidArgument("unexpected token '" + std::string(Describe(tail)) +
                                       "' after a condition (byte " +
                                       std::to_string(tail.byte_offset) + ")");
    }
    return conds;
}

StatusOr<BoolExpr> Parser::ParseBoolChain(std::uint32_t depth, bool is_or) {
    const std::string_view word = is_or ? "OR" : "AND";
    const auto operand = [&]() {
        return is_or ? ParseBoolChain(depth, /*is_or=*/false) : ParseBoolNot(depth);
    };
    const std::uint32_t at = lexer_.Peek().byte_offset;
    auto first = operand();
    if (!first.ok() || !IsWord(lexer_.Peek(), word)) return first;

    BoolExpr node;
    node.op = is_or ? BoolExpr::Op::kOr : BoolExpr::Op::kAnd;
    node.byte_offset = at;
    node.children.push_back(std::move(first.value()));
    while (IsWord(lexer_.Peek(), word)) {
        lexer_.Next();
        auto next = operand();
        if (!next.ok()) return next;
        node.children.push_back(std::move(next.value()));
    }
    return node;
}

StatusOr<BoolExpr> Parser::ParseBoolNot(std::uint32_t depth) {
    const Token& lead = lexer_.Peek();
    const std::uint32_t at = lead.byte_offset;

    if (lead.type == TokenType::kKeyword && lead.kw == Keyword::kNot) {
        lexer_.Next();
        // `NOT EXISTS (...)` stays the one leaf it has always been.
        if (lexer_.Peek().type == TokenType::kKeyword && lexer_.Peek().kw == Keyword::kExists) {
            lexer_.Next();
            BoolExpr leaf;
            leaf.byte_offset = at;
            leaf.leaf.kind = PredicateKind::kNotExists;
            auto sub = ParseSubquery(depth);
            if (!sub.ok()) return sub.status();
            leaf.leaf.subquery = std::move(sub.value());
            return leaf;
        }
        if (bool_nest_ >= kMaxBoolNest) return BoolTooBig(at);
        DepthScope scope(bool_nest_);
        auto operand = ParseBoolNot(depth);
        if (!operand.ok()) return operand;
        return Not(std::move(operand.value()), at);
    }

    if (lead.type == TokenType::kLParen) {
        if (bool_nest_ >= kMaxBoolNest) return BoolTooBig(at);
        DepthScope scope(bool_nest_);
        lexer_.Next();
        auto inner = ParseBoolChain(depth, /*is_or=*/true);
        if (!inner.ok()) return inner;
        if (Status s = ExpectToken(TokenType::kRParen, "')' closing a parenthesized condition");
            !s.ok()) {
            return s;
        }
        return inner;
    }

    // A leaf, or - a value list, a negated form - the subtree it means.
    return ParseOneCondition(depth);
}

StatusOr<BoolExpr> Parser::ParseValueList(ColumnName col, bool negated, std::uint32_t at) {
    // `col IN (v1, v2)` is `col = v1 OR col = v2`, and `NOT IN` is
    // `col != v1 AND col != v2` - three-valued exactly as the standard
    // reads a list: a NULL item is unknown, never a match, so a `NOT IN`
    // list holding one keeps no row. The `(` is read.
    BoolExpr node;
    node.op = negated ? BoolExpr::Op::kAnd : BoolExpr::Op::kOr;
    node.byte_offset = at;
    for (;;) {
        auto val = ParseValue();
        if (!val.ok()) return val.status();
        Condition item;
        item.kind = PredicateKind::kCompareValue;
        item.col = col;
        item.op = negated ? CompareOp::kNeq : CompareOp::kEq;
        item.rhs_kind = RhsKind::kLiteral;
        item.val = std::move(val.value());
        node.children.push_back(Leaf(std::move(item), at));
        if (lexer_.Peek().type != TokenType::kComma) break;
        lexer_.Next();
    }
    if (Status s = ExpectToken(TokenType::kRParen, "')' closing a value list"); !s.ok()) return s;
    // One item is the plain comparison it means, keys and all.
    if (node.children.size() == 1) return std::move(node.children.front());
    return node;
}

// One column definition, `CREATE TABLE`'s and `ALTER TABLE ... ADD
// COLUMN`'s (BW-R1): a name, a type with its arguments, then the suffixes
// in their fixed order - nullability, `REFERENCES`, the cabin policy.
StatusOr<ColumnDef> Parser::ParseColumnDef() {
    ColumnDef col;

    col.name_byte_offset = lexer_.Peek().byte_offset;
    auto col_name = ParseIdent();
    if (!col_name.ok()) return col_name.status();
    col.name = std::move(col_name.value());

    // The type, with its arguments: `DECIMAL(p, s)` takes two
    // (docs/spec/types.md §2), `CHAR(n)` and `VARCHAR(n)` one
    // (`instructions/v2.5.0/varchar-char-architecture.md` §3).
    TypeSpec spec;
    if (Status s = ParseTypeSpec(spec); !s.ok()) return s;
    col.type_name = std::move(spec.name);
    col.type_byte_offset = spec.byte_offset;
    col.type_arg_byte_offset = spec.arg_byte_offset;
    col.has_precision = spec.has_precision;
    col.precision = spec.precision;
    col.scale = spec.scale;
    col.has_width = spec.has_width;
    col.width = spec.width;
    if (IEquals(col.type_name, "DECIMAL") && !col.has_precision) {
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
                "a foreign key references the parent's tuple key and no other column, so "
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

    return col;
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
        auto col = ParseColumnDef();
        if (!col.ok()) return col.status();
        stmt.columns.push_back(std::move(col.value()));

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
    // The key-mode words, since the mode's removal (2026-08-25):
    // `EXPLICIT` is **accepted and does nothing** - it states what is true
    // of every relation, that the caller may name keys. `ASSIGNED` is
    // **refused** `Unsupported` with its byte: it means "supplying an id is
    // refused", which this relation would not enforce (the truthfulness
    // rule).
    //
    // `SPARE <n>` (BW-R1) joins the loop: the relation's reserved 16-byte
    // column slots, a fact about the whole relation like the other two, and
    // an identifier like them.
    bool storage_given = false;
    bool key_word_given = false;
    bool spare_given = false;
    for (;;) {
        const Token word = lexer_.Peek();
        if (word.type != TokenType::kIdent) break;

        const bool is_heap = IEquals(word.text, "HEAP");
        const bool is_btree = IEquals(word.text, "BTREE");
        const bool is_assigned = IEquals(word.text, "ASSIGNED");
        const bool is_explicit = IEquals(word.text, "EXPLICIT");
        const bool is_spare = IEquals(word.text, "SPARE");
        if (!is_heap && !is_btree && !is_assigned && !is_explicit && !is_spare) break;

        if (is_spare) {
            if (spare_given) {
                return Status::InvalidArgument(
                    "CREATE TABLE takes one SPARE and this is the second (byte " +
                    std::to_string(word.byte_offset) + ")");
            }
            spare_given = true;
            lexer_.Next();
            const std::uint32_t count_at = lexer_.Peek().byte_offset;
            auto count = ParseTypeArgument("SPARE slot count");
            if (!count.ok()) return count.status();
            if (count.value() > catalog::kMaxSpareSlots) {
                return Status::InvalidArgument(
                    "SPARE " + std::to_string(count.value()) + " exceeds the " +
                    std::to_string(catalog::kMaxSpareSlots) + " slots a relation may reserve (byte " +
                    std::to_string(count_at) + ")");
            }
            stmt.spare_slots = static_cast<std::uint16_t>(count.value());
            continue;
        }

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
            // **SUS-1: heap relations are suspended.** Refused rather than
            // quietly turned into a btree (the truthfulness rule).
            // `Unsupported` is the operator's choice (AS-R1) though the heap
            // is built and existing heaps still serve, so the message
            // carries the reason and names the order.
            if (is_heap && !HeapStorageAllowedForTest()) {
                return Status::Unsupported(
                    "HEAP storage is suspended (SUS-1) and no new heap relation is created "
                    "(byte " +
                    std::to_string(word.byte_offset) +
                    "); BTREE is the default and every existing heap relation still mounts "
                    "and serves. The suspension, its rulings and the condition that lifts it "
                    "are instructions/v3.0.0/workorder-as-sus1-heap-suspended.md");
            }
            storage_given = true;
            stmt.clustered_given = true;
            stmt.clustered = is_heap ? catalog::ClusteredType::kHeap
                                     : catalog::ClusteredType::kBtree;
        } else if (is_assigned) {
            return Status::Unsupported(
                "the ASSIGNED key mode no longer exists (byte " +
                std::to_string(word.byte_offset) +
                ") - every relation takes a caller-supplied tuple key or issues one when "
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

// `CREATE PATTERN` / `DROP PATTERN` were withdrawn on 2026-08-31
// (`docs/spec/create-pattern-user-defined-patterns-v1.md`); nothing took
// their place.

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

    // **The key function's shape, decided by reading ahead and rewinding**
    // (AQ-S1): a bare name is a column, a bare call is an expression, and
    // anything else is a predicate, parsed by the WHERE grammar from the
    // start. The lexer is a value, so the rewind is a copy.
    stmt.column_byte_offset = lexer_.Peek().byte_offset;
    stmt.expr_byte_offset = stmt.column_byte_offset;
    const Lexer start = lexer_;
    const bool named = lexer_.Peek().type == TokenType::kIdent;
    if (named) {
        auto column = ParseIdent();
        if (!column.ok()) return column.status();
        stmt.column_name = std::move(column.value());
    }

    if (named && lexer_.Peek().type == TokenType::kLParen) {
        ColumnName head;
        head.name = stmt.column_name;
        head.byte_offset = stmt.column_byte_offset;
        auto call = ParseFunctionCall(std::move(head));
        if (!call.ok()) return call.status();
        if (lexer_.Peek().type == TokenType::kRParen) {
            stmt.shape = catalog::kCabinShapeExpression;
            stmt.expr = std::move(call.value());
            stmt.column_name.clear();
        }
    }
    if (!named || (stmt.shape != catalog::kCabinShapeExpression &&
                   lexer_.Peek().type != TokenType::kRParen &&
                   lexer_.Peek().type != TokenType::kComma)) {
        lexer_ = start;
        auto tree = ParseBoolChain(/*depth=*/0, /*is_or=*/true);
        if (!tree.ok()) return tree.status();
        if (Status s = Flatten(std::move(tree.value()), stmt.predicate); !s.ok()) return s;
        stmt.shape = catalog::kCabinShapePredicate;
        stmt.column_name.clear();
    }

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
// `NAMESPACE` is matched by text, so **nothing is reserved by it**: a
// column may still be named `namespace`, and CREATE/DROP are not
// patternable, so no fingerprint moves.
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
//   * `NotImplemented` for a form nobody built - a lower bound or `=`
//     (AS11), an aggregate outside {COUNT, SUM} (§10), `DISTINCT`,
//     `COUNT(<column>)` - and `Unsupported` for the constraint-timing
//     clauses the architecture declines (`DEFERRABLE` AS3, `NOT VALID` AS7).
//     These are reserved grammar: they parse, so the answer names the
//     decision rather than pointing at a syntax error somewhere else.
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
        case CompareOp::kLike:
            // Unreachable through ParseCompareOp, which never produces the
            // IS forms - they parse in ParseCondition only. Refused rather
            // than defaulted, so a grammar change here has to decide.
            return Status::Unsupported("an assertion bound takes a relational operator (byte " +
                                       std::to_string(op_at) + ")");
        case CompareOp::kEq:
            // AS11 (revised 2026-08-08): reading `=` as `<=` would enforce
            // less than was written, and real equality needs the lower
            // bound v1 excludes - so `=` is refused beside `>=`.
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

StatusOr<TransitionStmt> Parser::ParseTransition(bool drop) {
    TransitionStmt stmt;
    stmt.drop = drop;

    stmt.byte_offset = lexer_.Peek().byte_offset;
    auto name = ParseIdent();
    if (!name.ok()) return name.status();
    stmt.name = std::move(name.value());

    // A Transition's name is unique instance-wide, so DROP names nothing else.
    if (drop) {
        ConsumeOptionalSemicolon();
        return stmt;
    }

    // `ON` is a reserved keyword (it joins); ParseAssertion has the same
    // wrinkle. Every other word of this grammar - TRANSITION, INSERT,
    // UPDATE, DELETE, FROM - is matched by text and reserves nothing (BV-R5).
    const auto is_on = [](const Token& t) {
        return t.type == TokenType::kKeyword && t.kw == Keyword::kOn;
    };
    if (const Token& on = lexer_.Peek(); !is_on(on)) {
        return Status::InvalidArgument(
            "CREATE TRANSITION names the column it governs: `CREATE TRANSITION <name> ON "
            "<table>(<column>) ON INSERT (...) ON UPDATE (... => ...)` (byte " +
            std::to_string(on.byte_offset) + ")");
    }
    lexer_.Next();
    if (Status s = ParseQualifiedName(stmt.schema, stmt.table_name, &stmt.table_byte_offset);
        !s.ok()) {
        return s;
    }
    if (Status s = ExpectToken(TokenType::kLParen, "'(' and the governed column"); !s.ok()) {
        return s;
    }
    stmt.column_byte_offset = lexer_.Peek().byte_offset;
    auto column = ParseIdent();
    if (!column.ok()) return column.status();
    stmt.column = std::move(column.value());
    if (Status s = ExpectToken(TokenType::kRParen, "')' after the governed column"); !s.ok()) {
        return s;
    }

    // `ON <event>`, in the fixed order INSERT, UPDATE, DELETE. INSERT and
    // UPDATE are required (CN-4 T10): without both, §2's path property
    // does not follow from the graph, and a declaration that gave it up
    // silently would promise what it does not enforce.
    const auto clause = [&](std::string_view event) -> bool {
        const Token& on = lexer_.Peek();
        if (!is_on(on)) return false;
        Lexer probe = lexer_;
        probe.Next();
        if (!IsWord(probe.Peek(), event)) return false;
        lexer_.Next();
        lexer_.Next();
        return true;
    };
    const auto required = [&](std::string_view event) -> Status {
        if (clause(event)) return Status::OK();
        return Status::InvalidArgument("CREATE TRANSITION requires ON " + std::string(event) +
                                       ", in the order ON INSERT, ON UPDATE, ON DELETE FROM "
                                       "(byte " +
                                       std::to_string(lexer_.Peek().byte_offset) + ")");
    };

    if (Status s = required("INSERT"); !s.ok()) return s;
    if (Status s = ParseTransitionStates(stmt.initial, "ON INSERT"); !s.ok()) return s;

    if (Status s = required("UPDATE"); !s.ok()) return s;
    if (Status s = ExpectToken(TokenType::kLParen, "'(' and the ON UPDATE edges"); !s.ok()) {
        return s;
    }
    for (;;) {
        TransitionStmt::Edge edge;
        auto from = ParseValue();
        if (!from.ok()) return from.status();
        edge.from = std::move(from.value());
        // `=>` is `=` immediately followed by `>`: two tokens, adjacent,
        // so the arrow costs the lexer nothing and the fingerprint nothing.
        const Token eq = lexer_.Next();
        const Token gt = lexer_.Next();
        if (eq.type != TokenType::kEq || gt.type != TokenType::kGt ||
            gt.byte_offset != eq.byte_offset + 1) {
            return Status::InvalidArgument("an ON UPDATE edge is `<state> => <state>` (byte " +
                                           std::to_string(eq.byte_offset) + ")");
        }
        auto to = ParseValue();
        if (!to.ok()) return to.status();
        edge.to = std::move(to.value());
        stmt.edges.push_back(std::move(edge));
        const Token next = lexer_.Next();
        if (next.type == TokenType::kRParen) break;
        if (next.type != TokenType::kComma) {
            return Status::InvalidArgument("expected ',' or ')' in ON UPDATE (byte " +
                                           std::to_string(next.byte_offset) + ")");
        }
    }

    if (clause("DELETE")) {
        if (const Token& from = lexer_.Next(); !IsWord(from, "FROM")) {
            return Status::InvalidArgument("ON DELETE is `ON DELETE FROM (<state>, ...)` (byte " +
                                           std::to_string(from.byte_offset) + ")");
        }
        stmt.delete_declared = true;
        if (Status s = ParseTransitionStates(stmt.deletable, "ON DELETE FROM"); !s.ok()) return s;
    }

    ConsumeOptionalSemicolon();

    // Sliced from the input, never rebuilt from the AST, for ParseAssertion's
    // reason: a rebuild is a second spelling that can drift. `Parse()`
    // refuses trailing garbage, so the statement is exactly the input.
    std::string_view text = sql_;
    while (!text.empty() && (text.back() == ';' ||
                             std::isspace(static_cast<unsigned char>(text.back())) != 0)) {
        text.remove_suffix(1);
    }
    stmt.source_text = std::string(text);
    return stmt;
}

Status Parser::ParseTransitionStates(std::vector<AstValue>& out, std::string_view what) {
    if (Status s = ExpectToken(TokenType::kLParen, "'(' and a list of states"); !s.ok()) return s;
    for (;;) {
        auto value = ParseValue();
        if (!value.ok()) return value.status();
        out.push_back(std::move(value.value()));
        const Token next = lexer_.Next();
        if (next.type == TokenType::kRParen) return Status::OK();
        if (next.type != TokenType::kComma) {
            return Status::InvalidArgument("expected ',' or ')' in " + std::string(what) +
                                           " (byte " + std::to_string(next.byte_offset) + ")");
        }
    }
}

StatusOr<InsertStmt> Parser::ParseInsert() {
    if (Status s = ExpectKeyword("INTO"); !s.ok()) return s;

    InsertStmt stmt;

    if (Status s = ParseQualifiedName(stmt.schema, stmt.table_name, &stmt.table_byte_offset);
        !s.ok()) {
        return s;
    }

    // `( <col> [, <col>]* )` (BW-S6, BW-Q12.1 of
    // `instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`): the
    // columns the rows' values are for, so no column change can shift them.
    // The paren decides it unambiguously - a positional statement has
    // `VALUES` here. Names are resolved by the dispatcher, under the
    // relation's `IX`; an empty list is `ParseIdent`'s refusal at the `)`.
    if (lexer_.Peek().type == TokenType::kLParen) {
        stmt.columns_byte_offset = lexer_.Peek().byte_offset;
        if (Status s = ParseDeclaredColumnList(stmt.columns, "column list", /*cap=*/0); !s.ok()) {
            return s;
        }
    }

    // `INSERT ... SELECT` (BU-S8): the source is a query of the query
    // grammar, run by the query layer.
    if (IsWord(lexer_.Peek(), "SELECT")) {
        stmt.select_byte_offset = lexer_.Peek().byte_offset;
        const bool was = query_mode_;
        query_mode_ = true;
        auto source = ParseQueryExpr();
        query_mode_ = was;
        if (!source.ok()) return source.status();
        stmt.select = std::move(source.value());
        stmt.select_text = std::string(sql_);
        if (Status s = ParseInsertTail(stmt); !s.ok()) return s;
        ConsumeOptionalSemicolon();
        return stmt;
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
            // A value is a SET value's grammar (BU-S8): a bare literal reads
            // as it always did, anything longer as an expression.
            Assignment value;
            if (Status s = ParseSetValue(value); !s.ok()) return s;
            if (value.expr != nullptr) {
                AstValue placeholder;
                placeholder.type = ValueType::kNull;
                placeholder.byte_offset = value.expr->byte_offset;
                stmt.value_exprs.push_back({stmt.rows.size(), row.size(), std::move(value.expr)});
                row.push_back(std::move(placeholder));
            } else {
                row.push_back(std::move(value.val));
            }

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

    if (Status s = ParseInsertTail(stmt); !s.ok()) return s;
    ConsumeOptionalSemicolon();
    return stmt;
}

// What follows an INSERT's rows (BU-S8): `ON CONFLICT [(col)] DO NOTHING`
// (BU-Q18). `DO UPDATE` is understood and not built.
Status Parser::ParseInsertTail(InsertStmt& stmt) {
    const Token& on = lexer_.Peek();
    if (on.type != TokenType::kKeyword || on.kw != Keyword::kOn) return Status::OK();
    lexer_.Next();
    if (!IsWord(lexer_.Peek(), "CONFLICT")) {
        return Status::InvalidArgument("expected CONFLICT after ON, got '" +
                                       std::string(Describe(lexer_.Peek())) + "'" +
                                       At(lexer_.Peek().byte_offset));
    }
    lexer_.Next();
    if (lexer_.Peek().type == TokenType::kLParen) {
        lexer_.Next();
        IndexColumnRef target;
        target.byte_offset = lexer_.Peek().byte_offset;
        auto name = ParseIdent();
        if (!name.ok()) return name.status();
        target.name = std::move(name.value());
        if (lexer_.Peek().type != TokenType::kRParen) {
            return Status::NotImplemented("a conflict target of more than one column is not "
                                          "supported" + At(lexer_.Peek().byte_offset) +
                                          "; the tuple key is the one conflict there is");
        }
        lexer_.Next();
        stmt.conflict_target = std::move(target);
    }
    if (!IsWord(lexer_.Peek(), "DO")) {
        return Status::InvalidArgument("expected DO, got '" + std::string(Describe(lexer_.Peek())) +
                                       "'" + At(lexer_.Peek().byte_offset));
    }
    lexer_.Next();
    if (IsWord(lexer_.Peek(), "UPDATE")) {
        return Status::NotImplemented("ON CONFLICT DO UPDATE is not supported" +
                                      At(lexer_.Peek().byte_offset) + "; DO NOTHING is");
    }
    if (!IsWord(lexer_.Peek(), "NOTHING")) {
        return Status::InvalidArgument("expected NOTHING, got '" +
                                       std::string(Describe(lexer_.Peek())) + "'" +
                                       At(lexer_.Peek().byte_offset));
    }
    lexer_.Next();
    stmt.on_conflict_do_nothing = true;
    return Status::OK();
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
    // materialized somewhere and probed by something other than a tuple key.
    // That breaks tuple-key-direct probing into the next step, which is the
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
    // A scalar function call is a WHERE conjunct's alone (AP-S4, AP-R4
    // item 4): here, in ORDER BY and in HAVING it is understood and not
    // built. Refused by name, at its first byte, before `(` could be read
    // as whatever follows a column.
    if (!col.value().qualified() && lexer_.Peek().type == TokenType::kLParen &&
        !AggFuncOf(col.value().name, func)) {
        return Status::NotImplemented(
            "a function call is supported only in a WHERE comparison and a SET value (byte " +
            std::to_string(col.value().byte_offset) + ")");
    }
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
// What this production decides is shape: the conjuncts are AND-combined,
// `IS [NOT] NULL` has no right side, and every other right side is a
// **literal** - a post-fold predicate has no row to read a column from.
// A column or aggregate on the right is refused here with its own byte.
// Typing and grouping-key questions are the compiler's - which today
// refuses every HAVING `NotImplemented` (step_compiler.cpp).
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
// Tokens from `Peek()` are bound **by value** throughout: a reference held
// across a `Next()` is safe only while `Token` owns nothing.
Status Parser::ParsePaginationTail(SelectStmt& stmt, bool aggregated, std::uint32_t depth) {
    if (const Token order = lexer_.Peek();
        order.type == TokenType::kIdent && IEquals(order.text, "ORDER")) {
        // Parsed over a fold too, with an aggregate admitted as a key
        // (`docs/inflight/in-progress/workplan-having.md` HV4); the compiler
        // refuses an aggregated ORDER BY `NotImplemented` (step_compiler.cpp).
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
    // make a statement aggregated (HV-1). It is refused `NotImplemented` at
    // compile (step_compiler.cpp).
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

std::vector<Condition> ExistsOver(RelationRef rel, std::vector<Condition> where, bool at_most_one);

StatusOr<DeleteStmt> Parser::ParseDelete() {
    DeleteStmt stmt;

    if (Status s = ExpectKeyword("FROM"); !s.ok()) return s;

    if (Status s = ParseQualifiedName(stmt.schema, stmt.table_name, &stmt.table_byte_offset);
        !s.ok()) {
        return s;
    }

    // `DELETE FROM t USING u WHERE ...` (BU-S8): one more relation the
    // WHERE reads, made an EXISTS below.
    std::optional<RelationRef> using_rel;
    if (IsWord(lexer_.Peek(), "USING")) {
        lexer_.Next();
        auto rel = ParseOtherRelation(stmt.table_name);
        if (!rel.ok()) return rel.status();
        using_rel = std::move(rel.value());
    }

    // Depth 0 and the same production UPDATE uses: a DELETE's WHERE is an
    // outermost query block, and a predicate-position subquery in it nests
    // exactly as a SELECT's does.
    auto where = ParseOptionalWhere(/*depth=*/0);
    if (!where.ok()) return where.status();
    stmt.where = std::move(where.value());
    if (using_rel.has_value()) {
        stmt.where = ExistsOver(std::move(*using_rel), std::move(stmt.where), /*at_most_one=*/false);
    }

    ConsumeOptionalSemicolon();
    return stmt;
}

// The relation `UPDATE ... FROM` or `DELETE ... USING` names (BU-S8): one,
// bound apart from the statement's own `target`.
StatusOr<RelationRef> Parser::ParseOtherRelation(const std::string& target) {
    auto rel = ParseRelationRef();
    if (!rel.ok()) return rel.status();
    const Token& next = lexer_.Peek();
    if (next.type == TokenType::kComma ||
        (next.type == TokenType::kKeyword && next.kw == Keyword::kJoin)) {
        return Status::NotImplemented("more than one relation after FROM or USING is not "
                                      "supported" + At(next.byte_offset) +
                                      "; name one, and put the rest in an EXISTS");
    }
    if (IEquals(rel.value().binding(), target)) {
        return Status::InvalidArgument("the name '" + rel.value().binding() +
                                       "' is bound twice" + At(rel.value().byte_offset) +
                                       "; give the second one an alias with AS");
    }
    return rel;
}

// The statement's WHERE as one EXISTS over `rel` (BU-S8): the subquery
// reads `rel` under `where`, in one name space with the statement, so a row
// is written when some `rel` row meets the conditions - and for `UPDATE`
// (`at_most_one`) when exactly one does.
std::vector<Condition> ExistsOver(RelationRef rel, std::vector<Condition> where, bool at_most_one) {
    auto sub = std::make_shared<SelectStmt>();
    sub->from = std::move(rel);
    sub->where = std::move(where);
    sub->shares_parent_names = true;
    Condition exists;
    exists.kind = PredicateKind::kExists;
    exists.at_most_one = at_most_one;
    exists.subquery = std::move(sub);
    std::vector<Condition> out;
    out.push_back(std::move(exists));
    return out;
}

StatusOr<PurgeStmt> Parser::ParsePurge() {
    // `DELETE`'s production, with a `WHERE` required (PU1, PU10): a bare
    // `PURGE FROM t` would be a sweep of the relation, which nobody asked
    // for and a later word could add - so it is declined, not wrong. Which
    // conjuncts the statement admits is the dispatcher's question, asked of
    // the relation's tuple key column; the grammar is `DELETE`'s, so the parse
    // gives every byte a refusal there needs.
    PurgeStmt stmt;
    if (Status s = ExpectKeyword("FROM"); !s.ok()) return s;
    if (Status s = ParseQualifiedName(stmt.schema, stmt.table_name, &stmt.table_byte_offset);
        !s.ok()) {
        return s;
    }
    const Token& peek = lexer_.Peek();
    stmt.where_byte_offset = peek.byte_offset;
    if (peek.type != TokenType::kIdent || !IEquals(peek.text, "WHERE")) {
        return Status::NotImplemented("PURGE needs a WHERE naming the tuple key; purging every "
                                      "deleted key of a relation is not implemented (byte " +
                                      std::to_string(peek.byte_offset) + ")");
    }
    auto where = ParseOptionalWhere(/*depth=*/0);
    if (!where.ok()) return where.status();
    stmt.where = std::move(where.value());

    ConsumeOptionalSemicolon();
    return stmt;
}

Status Parser::ParseRowValueAssignments(std::vector<Assignment>& out) {
    const std::uint32_t open_at = lexer_.Peek().byte_offset;
    lexer_.Next();  // '('
    std::vector<Assignment> targets;
    for (;;) {
        Assignment a;
        a.byte_offset = lexer_.Peek().byte_offset;
        auto name = ParseIdent();
        if (!name.ok()) return name.status();
        a.col_name = std::move(name.value());
        targets.push_back(std::move(a));
        if (lexer_.Peek().type == TokenType::kComma) {
            lexer_.Next();
            continue;
        }
        break;
    }
    if (Status s = ExpectToken(TokenType::kRParen, "')' closing the column list"); !s.ok()) {
        return s;
    }
    if (Status s = ExpectToken(TokenType::kEq, "'='"); !s.ok()) return s;
    if (lexer_.Peek().type != TokenType::kLParen) {
        return Status::InvalidArgument("expected '(' opening the row of values (byte " +
                                       std::to_string(lexer_.Peek().byte_offset) + ")");
    }
    lexer_.Next();
    if (lexer_.Peek().type == TokenType::kIdent && IEquals(lexer_.Peek().text, "SELECT")) {
        return Status::NotImplemented(
            "a row subquery in a SET value is not implemented (byte " +
            std::to_string(lexer_.Peek().byte_offset) + ")");
    }
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (Status s = ParseSetValue(targets[i]); !s.ok()) return s;
        if (i + 1 < targets.size()) {
            if (lexer_.Peek().type == TokenType::kRParen) {
                return Status::InvalidArgument(
                    "the row of values has fewer entries than the " +
                    std::to_string(targets.size()) + " columns before '=' (byte " +
                    std::to_string(open_at) + ")");
            }
            if (Status s = ExpectToken(TokenType::kComma, "',' between the row's values");
                !s.ok()) {
                return s;
            }
        }
    }
    if (lexer_.Peek().type == TokenType::kComma) {
        return Status::InvalidArgument("the row of values has more entries than the " +
                                       std::to_string(targets.size()) +
                                       " columns before '=' (byte " +
                                       std::to_string(lexer_.Peek().byte_offset) + ")");
    }
    if (Status s = ExpectToken(TokenType::kRParen, "')' closing the row of values"); !s.ok()) {
        return s;
    }
    for (Assignment& a : targets) out.push_back(std::move(a));
    return Status::OK();
}

StatusOr<UpdateStmt> Parser::ParseUpdate() {
    UpdateStmt stmt;

    if (Status s = ParseQualifiedName(stmt.schema, stmt.table_name, &stmt.table_byte_offset);
        !s.ok()) {
        return s;
    }

    if (Status s = ExpectKeyword("SET"); !s.ok()) return s;

    for (;;) {
        // The row-value form (BJ-Q7): `SET (a, b) = (x, y)` is `SET a = x,
        // b = y`, every value read from the before-image (BJ-R2).
        if (lexer_.Peek().type == TokenType::kLParen) {
            if (Status s = ParseRowValueAssignments(stmt.assignments); !s.ok()) return s;
            if (lexer_.Peek().type == TokenType::kComma) {
                lexer_.Next();
                continue;
            }
            break;
        }

        Assignment a;

        // Taken before the ident is consumed: the compiler reports the
        // column's own byte, not the one after it.
        a.byte_offset = lexer_.Peek().byte_offset;

        auto col_name = ParseIdent();
        if (!col_name.ok()) return col_name.status();
        a.col_name = std::move(col_name.value());

        if (Status s = ExpectToken(TokenType::kEq, "'='"); !s.ok()) return s;

        if (Status s = ParseSetValue(a); !s.ok()) return s;

        stmt.assignments.push_back(std::move(a));

        if (lexer_.Peek().type == TokenType::kComma) {
            lexer_.Next();
            continue;
        }
        break;
    }

    // `UPDATE t SET ... FROM u WHERE ...` (BU-S8): one more relation the
    // WHERE reads, made an EXISTS that refuses a second match (BU-Q16). A
    // SET value reads the target's columns only.
    std::optional<RelationRef> from_rel;
    if (IsWord(lexer_.Peek(), "FROM")) {
        lexer_.Next();
        auto rel = ParseOtherRelation(stmt.table_name);
        if (!rel.ok()) return rel.status();
        from_rel = std::move(rel.value());
    }

    // Depth 0: an UPDATE's WHERE is an outermost query block, and a
    // subquery in it is `[OPEN: revisit]` in spec section 2 only for the
    // *value* position (`SET a = (SELECT ...)`), which BJ-S7 built as an
    // uncorrelated scalar subquery. A predicate-position subquery here nests exactly as a
    // SELECT's does.
    auto where = ParseOptionalWhere(/*depth=*/0);
    if (!where.ok()) return where.status();
    stmt.where = std::move(where.value());
    if (from_rel.has_value()) {
        stmt.where = ExistsOver(std::move(*from_rel), std::move(stmt.where), /*at_most_one=*/true);
    }

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
        } else if (what.type == TokenType::kIdent && IEquals(what.text, "TRANSITION")) {
            lexer_.Next();
            auto s = ParseTransition(/*drop=*/false);
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
        // Tables, namespaces, cabins, indexes and assertions can be
        // dropped; saying so here beats a syntax error that points at the
        // object's name - the list in the message is the whole of what
        // exists (`DROP PATTERN`, withdrawn 2026-08-31, is refused by it).
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
        } else if (what.type == TokenType::kIdent && IEquals(what.text, "TRANSITION")) {
            lexer_.Next();
            auto s = ParseTransition(/*drop=*/true);
            if (!s.ok()) return s.status();
            stmt = std::move(s.value());
        } else if (what.type == TokenType::kIdent && IEquals(what.text, "NAMESPACE")) {
            lexer_.Next();
            auto s = ParseNamespace(/*drop=*/true);
            if (!s.ok()) return s.status();
            stmt = std::move(s.value());
        } else if (what.type == TokenType::kIdent && IEquals(what.text, "TABLE")) {
            // docs/spec/drop-table.md DT6: catalog-scoped, oid tombstoned,
            // pages reclaimed after the commit (DT1) - the refusals live in
            // the dispatcher, which is the layer that can name a blocker.
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
                "only DROP TABLE, DROP NAMESPACE, DROP CABIN, DROP INDEX, DROP ASSERTION and "
                "DROP TRANSITION are supported (byte " +
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
    } else if (IEquals(tok.text, "PURGE")) {
        auto s = ParsePurge();
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
            "' (supported: CREATE, DROP, ALTER, INSERT, SELECT, UPDATE, DELETE, PURGE)");
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

// `ALTER TABLE <t> RENAME TO <new> | RENAME COLUMN <old> TO <new> |
// ADD [COLUMN] <column definition> | DROP [COLUMN] <name>`
// (docs/spec/alter.md AL1, AL7, AL11), with `ALTER` already consumed.
//
// `TABLE`, `RENAME`, `ADD`, `DROP`, `COLUMN` and `TO` are ordinary identifiers
// matched by text, like every clause head this grammar has grown - and the
// refusal surface is parsed *before* the accepted forms, so `MODIFY`/`SET`
// answer at their own byte rather than with a syntax error pointing past
// them.
StatusOr<AlterStmt> Parser::ParseAlter() {
    if (Status s = ExpectKeyword("TABLE"); !s.ok()) return s;

    AlterStmt stmt;
    if (Status s = ParseQualifiedName(stmt.schema, stmt.table_name, &stmt.byte_offset);
        !s.ok()) {
        return s;
    }

    const Token verb = lexer_.Peek();
    // `ADD [COLUMN] <column definition>` (BW-R1): a column in one of the
    // relation's reserved slots, every row of which already has the bytes.
    if (verb.type == TokenType::kIdent && IEquals(verb.text, "ADD")) {
        lexer_.Next();
        if (const Token what = lexer_.Peek();
            what.type == TokenType::kIdent && IEquals(what.text, "COLUMN")) {
            lexer_.Next();
        }
        stmt.action = AlterAction::kAddColumn;
        stmt.column_byte_offset = lexer_.Peek().byte_offset;
        auto column = ParseColumnDef();
        if (!column.ok()) return column.status();
        stmt.column = std::move(column.value());
        ConsumeOptionalSemicolon();
        return stmt;
    }
    if (verb.type == TokenType::kIdent &&
        (IEquals(verb.text, "MODIFY") || IEquals(verb.text, "SET"))) {
        return Status::NotImplemented(
            "ALTER TABLE " + std::string(verb.text) +
            " is not supported (byte " + std::to_string(verb.byte_offset) +
            "); ALTER changes catalog rows and no tuple byte, and this form would rewrite "
            "every row of the relation (invariant 13)");
    }
    // `DROP [COLUMN] <name>` (BW-R1): a flag on the column's catalog row,
    // its bytes left in every tuple (BW-R6).
    if (verb.type == TokenType::kIdent && IEquals(verb.text, "DROP")) {
        lexer_.Next();
        if (const Token what = lexer_.Peek();
            what.type == TokenType::kIdent && IEquals(what.text, "COLUMN")) {
            lexer_.Next();
        }
        stmt.action = AlterAction::kDropColumn;
        stmt.old_column_byte_offset = lexer_.Peek().byte_offset;
        auto column = ParseIdent();
        if (!column.ok()) return column.status();
        stmt.old_column = std::move(column.value());
        ConsumeOptionalSemicolon();
        return stmt;
    }
    if (verb.type != TokenType::kIdent || !IEquals(verb.text, "RENAME")) {
        return Status::InvalidArgument("expected RENAME, ADD or DROP after the table name, got '" +
                                        std::string(Describe(verb)) + "' at byte " +
                                        std::to_string(verb.byte_offset));
    }
    lexer_.Next();

    if (const Token what = lexer_.Peek();
        what.type == TokenType::kIdent && IEquals(what.text, "COLUMN")) {
        lexer_.Next();
        stmt.action = AlterAction::kRenameColumn;
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

// ---- The query grammar (BU-S4, workorder-bu-basic-query-surface.md BU-Q8) ----
//
// Reached only through `ParseQuery`, which the dispatcher calls after the
// native grammar or compiler refused a SELECT (BU-Q9). It shares the
// expression levels above with the SET value grammar, `query_mode_` turning
// on what only a query reads: qualified columns, aggregates, `[NOT] IN`,
// `[NOT] BETWEEN`, `[NOT] LIKE`, `EXISTS` and a subquery as a value.

StatusOr<Query> Parser::ParseQuery() {
    query_mode_ = true;
    const Token& first = lexer_.Peek();
    if (!IsWord(first, "SELECT") && first.type != TokenType::kLParen) {
        return Status::InvalidArgument("expected SELECT, got '" + std::string(Describe(first)) +
                                       "'" + At(first.byte_offset));
    }
    auto q = ParseQueryExpr();
    if (!q.ok()) return q.status();
    ConsumeOptionalSemicolon();
    if (const Token& tail = lexer_.Peek(); tail.type != TokenType::kEof) {
        return Status::InvalidArgument("unexpected token '" + std::string(Describe(tail)) +
                                       "' after end of statement" + At(tail.byte_offset));
    }
    return *q.value();
}

StatusOr<std::shared_ptr<const Query>> Parser::ParseQueryExpr() {
    auto lhs = ParseQueryTerm();
    if (!lhs.ok()) return lhs;
    std::shared_ptr<const Query> acc = std::move(lhs.value());
    while (IsWord(lexer_.Peek(), "UNION") || IsWord(lexer_.Peek(), "EXCEPT")) {
        const Token op = lexer_.Next();
        auto node = std::make_shared<Query>();
        node->op = IEquals(op.text, "UNION") ? Query::Op::kUnion : Query::Op::kExcept;
        if (IsWord(lexer_.Peek(), "ALL")) {
            lexer_.Next();
            node->all = true;
        } else if (IsWord(lexer_.Peek(), "DISTINCT")) {
            lexer_.Next();
        }
        auto rhs = ParseQueryTerm();
        if (!rhs.ok()) return rhs;
        node->byte_offset = acc->byte_offset;
        node->lhs = std::move(acc);
        node->rhs = std::move(rhs.value());
        acc = std::move(node);
    }
    // The tail orders and counts the whole expression. A parenthesized
    // operand keeps its own; one tail over another is refused rather than
    // read as either.
    const Token& next = lexer_.Peek();
    if (IsWord(next, "ORDER") || IsWord(next, "LIMIT") || IsWord(next, "OFFSET")) {
        if (!acc->order_by.empty() || acc->limit.has_value() || acc->offset != 0) {
            return Status::NotImplemented(
                "a second ORDER BY, LIMIT or OFFSET over a parenthesized query is not "
                "supported" + At(next.byte_offset));
        }
        auto whole = std::make_shared<Query>(*acc);
        if (Status s = ParseQueryTail(*whole); !s.ok()) return s;
        acc = std::move(whole);
    }
    return acc;
}

StatusOr<std::shared_ptr<const Query>> Parser::ParseQueryTerm() {
    auto lhs = ParseQueryPrimary();
    if (!lhs.ok()) return lhs;
    std::shared_ptr<const Query> acc = std::move(lhs.value());
    while (IsWord(lexer_.Peek(), "INTERSECT")) {
        lexer_.Next();
        auto node = std::make_shared<Query>();
        node->op = Query::Op::kIntersect;
        if (IsWord(lexer_.Peek(), "ALL")) {
            lexer_.Next();
            node->all = true;
        } else if (IsWord(lexer_.Peek(), "DISTINCT")) {
            lexer_.Next();
        }
        auto rhs = ParseQueryPrimary();
        if (!rhs.ok()) return rhs;
        node->byte_offset = acc->byte_offset;
        node->lhs = std::move(acc);
        node->rhs = std::move(rhs.value());
        acc = std::move(node);
    }
    return acc;
}

StatusOr<std::shared_ptr<const Query>> Parser::ParseQueryPrimary() {
    const Token& t = lexer_.Peek();
    if (t.type == TokenType::kLParen) {
        const Token open = lexer_.Next();
        return ParseNestedQuery(open);
    }
    const std::uint32_t at = t.byte_offset;
    auto block = ParseQueryBlock();
    if (!block.ok()) return block.status();
    auto q = std::make_shared<Query>();
    q->op = Query::Op::kBlock;
    q->block = std::move(block.value());
    q->byte_offset = at;
    return std::shared_ptr<const Query>(std::move(q));
}

StatusOr<std::shared_ptr<const Query>> Parser::ParseNestedQuery(const Token& open) {
    if (Status s = CheckSubqueryDepth(query_depth_, open.byte_offset); !s.ok()) return s;
    ++query_depth_;
    auto q = ParseQueryExpr();
    --query_depth_;
    if (!q.ok()) return q;
    if (Status s = ExpectToken(TokenType::kRParen, "')' closing a subquery"); !s.ok()) return s;
    return q;
}

StatusOr<std::shared_ptr<const QueryBlock>> Parser::ParseQueryBlock() {
    auto block = std::make_shared<QueryBlock>();
    block->byte_offset = lexer_.Peek().byte_offset;
    if (Status s = ExpectKeyword("SELECT"); !s.ok()) return s;
    if (IsWord(lexer_.Peek(), "DISTINCT")) {
        lexer_.Next();
        if (lexer_.Peek().type == TokenType::kKeyword && lexer_.Peek().kw == Keyword::kOn) {
            return Status::NotImplemented("DISTINCT ON is not supported" +
                                          At(lexer_.Peek().byte_offset));
        }
        block->distinct = true;
    } else if (IsWord(lexer_.Peek(), "ALL")) {
        lexer_.Next();
    }

    // ---- The select list ----------------------------------------------------
    for (;;) {
        OutputItem item;
        const Token first = lexer_.Peek();
        item.byte_offset = first.byte_offset;
        if (first.type == TokenType::kStar) {
            lexer_.Next();
            item.star = true;
        } else {
            if (first.type == TokenType::kIdent) {
                // `rel.*` needs the token after the dot: the identifier is
                // read, and if no star follows it is handed to the
                // expression as its first primary.
                lexer_.Next();
                if (lexer_.Peek().type == TokenType::kDot) {
                    lexer_.Next();
                    if (lexer_.Peek().type == TokenType::kStar) {
                        lexer_.Next();
                        item.star = true;
                        item.star_qualifier = std::string(first.text);
                    } else {
                        auto name = ParseIdent();
                        if (!name.ok()) return name.status();
                        auto e = std::make_shared<Expr>();
                        e->kind = Expr::Kind::kColumn;
                        e->byte_offset = first.byte_offset;
                        e->qualifier = std::string(first.text);
                        e->column = std::move(name.value());
                        pending_primary_ = std::move(e);
                    }
                } else {
                    auto primary = ParseExprAfterIdent(first);
                    if (!primary.ok()) return primary.status();
                    pending_primary_ = std::move(primary.value());
                }
            }
            if (!item.star) {
                auto expr = ParseExprOr();
                if (!expr.ok()) return expr.status();
                item.expr = std::move(expr.value());
                const std::uint32_t end = lexer_.Peek().byte_offset;
                std::string_view text = sql_.substr(first.byte_offset, end - first.byte_offset);
                while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
                    text.remove_suffix(1);
                }
                item.text = std::string(text);
                if (lexer_.Peek().type == TokenType::kKeyword && lexer_.Peek().kw == Keyword::kAs) {
                    lexer_.Next();
                    auto alias = ParseIdent();
                    if (!alias.ok()) return alias.status();
                    item.alias = std::move(alias.value());
                }
            }
        }
        block->items.push_back(std::move(item));
        if (lexer_.Peek().type != TokenType::kComma) break;
        lexer_.Next();
    }

    // ---- FROM ---------------------------------------------------------------
    if (IsWord(lexer_.Peek(), "FROM")) {
        lexer_.Next();
        auto from = ParseFromItem();
        if (!from.ok()) return from.status();
        block->has_from = true;
        block->from = std::move(from.value());
        if (Status s = ParseQueryJoins(*block); !s.ok()) return s;
    }

    // ---- WHERE: each top-level AND term with its text span ------------------
    if (IsWord(lexer_.Peek(), "WHERE")) {
        lexer_.Next();
        std::shared_ptr<const Expr> acc;
        for (;;) {
            const std::uint32_t begin = lexer_.Peek().byte_offset;
            auto term = ParseExprNot();
            if (!term.ok()) return term.status();
            block->where_spans.emplace_back(begin, lexer_.Peek().byte_offset);
            acc = acc == nullptr ? std::move(term.value())
                                 : MakeBinary(ExprOp::kAnd, begin, std::move(acc),
                                              std::move(term.value()));
            if (TooBig(*acc)) return ExprTooDeep(begin);
            if (!IsWord(lexer_.Peek(), "AND")) break;
            lexer_.Next();
        }
        // An `OR` at the top makes the whole WHERE one term.
        while (IsWord(lexer_.Peek(), "OR")) {
            const Token op = lexer_.Next();
            auto rhs = ParseExprAnd();
            if (!rhs.ok()) return rhs.status();
            acc = MakeBinary(ExprOp::kOr, op.byte_offset, std::move(acc), std::move(rhs.value()));
            if (TooBig(*acc)) return ExprTooDeep(op.byte_offset);
            block->where_spans.clear();
        }
        block->where = std::move(acc);
    }

    // ---- GROUP BY, HAVING ---------------------------------------------------
    if (IsWord(lexer_.Peek(), "GROUP")) {
        lexer_.Next();
        if (Status s = ExpectKeyword("BY"); !s.ok()) return s;
        for (;;) {
            auto e = ParseExprOr();
            if (!e.ok()) return e.status();
            block->group_by.push_back(std::move(e.value()));
            if (lexer_.Peek().type != TokenType::kComma) break;
            lexer_.Next();
        }
    }
    if (IsWord(lexer_.Peek(), "HAVING")) {
        lexer_.Next();
        auto e = ParseExprOr();
        if (!e.ok()) return e.status();
        block->having = std::move(e.value());
    }
    if (const Token& w = lexer_.Peek(); IsWord(w, "WINDOW") || IsWord(w, "QUALIFY")) {
        return Status::NotImplemented("'" + std::string(w.text) + "' is not supported" +
                                      At(w.byte_offset));
    }
    return std::shared_ptr<const QueryBlock>(std::move(block));
}

StatusOr<FromItem> Parser::ParseFromItem() {
    FromItem item;
    const Token& t = lexer_.Peek();
    item.byte_offset = t.byte_offset;
    if (t.type == TokenType::kLParen) {
        const Token open = lexer_.Next();
        if (!IsWord(lexer_.Peek(), "SELECT") && lexer_.Peek().type != TokenType::kLParen) {
            return Status::NotImplemented("a parenthesized join in FROM is not supported" +
                                          At(open.byte_offset));
        }
        auto q = ParseNestedQuery(open);
        if (!q.ok()) return q.status();
        item.derived = std::move(q.value());
        if (lexer_.Peek().type != TokenType::kKeyword || lexer_.Peek().kw != Keyword::kAs) {
            return Status::InvalidArgument("a derived table needs a name: `(SELECT ...) AS name`" +
                                           At(lexer_.Peek().byte_offset));
        }
        lexer_.Next();
        auto alias = ParseIdent();
        if (!alias.ok()) return alias.status();
        item.rel.alias = std::move(alias.value());
        item.rel.byte_offset = open.byte_offset;
        return item;
    }
    auto rel = ParseRelationRef();
    if (!rel.ok()) return rel.status();
    item.rel = std::move(rel.value());
    return item;
}

Status Parser::ParseQueryJoins(QueryBlock& block) {
    for (;;) {
        const Token t = lexer_.Peek();
        FromJoin join;
        join.byte_offset = t.byte_offset;
        if (t.type == TokenType::kComma) {
            lexer_.Next();
            join.kind = JoinKind::kCross;
            join.comma = true;
        } else if (t.type == TokenType::kKeyword && t.kw == Keyword::kJoin) {
            lexer_.Next();
            join.kind = JoinKind::kInner;
        } else if (IsWord(t, "INNER") || IsWord(t, "CROSS") ||
                   (t.type == TokenType::kKeyword &&
                    (t.kw == Keyword::kLeft || t.kw == Keyword::kRight || t.kw == Keyword::kFull))) {
            lexer_.Next();
            if (IsWord(t, "INNER")) {
                join.kind = JoinKind::kInner;
            } else if (IsWord(t, "CROSS")) {
                join.kind = JoinKind::kCross;
            } else {
                join.kind = t.kw == Keyword::kLeft    ? JoinKind::kLeft
                            : t.kw == Keyword::kRight ? JoinKind::kRight
                                                      : JoinKind::kFull;
                if (lexer_.Peek().type == TokenType::kKeyword &&
                    lexer_.Peek().kw == Keyword::kOuter) {
                    lexer_.Next();
                }
            }
            if (lexer_.Peek().type != TokenType::kKeyword || lexer_.Peek().kw != Keyword::kJoin) {
                return Status::InvalidArgument("expected JOIN, got '" +
                                               std::string(Describe(lexer_.Peek())) + "'" +
                                               At(lexer_.Peek().byte_offset));
            }
            lexer_.Next();
        } else if (IsWord(t, "NATURAL")) {
            return Status::NotImplemented("NATURAL JOIN is not supported" + At(t.byte_offset));
        } else {
            return Status::OK();
        }

        auto item = ParseFromItem();
        if (!item.ok()) return item.status();
        join.item = std::move(item.value());

        if (!join.comma) {
            const Token& cond = lexer_.Peek();
            if (cond.type == TokenType::kKeyword && cond.kw == Keyword::kOn) {
                if (join.kind == JoinKind::kCross) {
                    return Status::InvalidArgument("a CROSS JOIN takes no ON" +
                                                   At(cond.byte_offset));
                }
                lexer_.Next();
                auto on = ParseExprOr();
                if (!on.ok()) return on.status();
                join.on = std::move(on.value());
            } else if (IsWord(cond, "USING")) {
                if (join.kind == JoinKind::kCross) {
                    return Status::InvalidArgument("a CROSS JOIN takes no USING" +
                                                   At(cond.byte_offset));
                }
                lexer_.Next();
                if (Status s = ExpectToken(TokenType::kLParen, "'(' after USING"); !s.ok()) {
                    return s;
                }
                for (;;) {
                    auto col = ParseColumnName();
                    if (!col.ok()) return col.status();
                    if (col.value().qualified()) {
                        return Status::InvalidArgument("a USING column is not qualified" +
                                                       At(col.value().byte_offset));
                    }
                    join.using_columns.push_back(std::move(col.value()));
                    if (lexer_.Peek().type != TokenType::kComma) break;
                    lexer_.Next();
                }
                if (Status s = ExpectToken(TokenType::kRParen, "')' closing USING"); !s.ok()) {
                    return s;
                }
            } else if (join.kind != JoinKind::kCross) {
                return Status::InvalidArgument("expected ON or USING after JOIN, got '" +
                                               std::string(Describe(cond)) + "'" +
                                               At(cond.byte_offset));
            }
        }
        block.joins.push_back(std::move(join));
    }
}

Status Parser::ParseQueryTail(Query& q) {
    if (IsWord(lexer_.Peek(), "ORDER")) {
        lexer_.Next();
        if (Status s = ExpectKeyword("BY"); !s.ok()) return s;
        for (;;) {
            const std::uint32_t key_at = lexer_.Peek().byte_offset;
            if (q.order_by.size() >= kMaxSortKeys) {
                return Status::NotImplemented(
                    "ORDER BY takes at most " + std::to_string(kMaxSortKeys) + " keys" +
                    At(key_at) +
                    "; every key costs a comparison on every row pair, and sorting by a "
                    "prefix of what was written would answer a question nobody asked");
            }
            auto e = ParseExprOr();
            if (!e.ok()) return e.status();
            OrderItem item;
            item.expr = std::move(e.value());
            if (IsWord(lexer_.Peek(), "ASC")) {
                lexer_.Next();
            } else if (IsWord(lexer_.Peek(), "DESC")) {
                lexer_.Next();
                item.descending = true;
            }
            if (IsWord(lexer_.Peek(), "NULLS")) {
                return Status::NotImplemented(
                    "NULLS FIRST/LAST is not supported" + At(lexer_.Peek().byte_offset) +
                    "; NULLs sort largest");
            }
            q.order_by.push_back(std::move(item));
            if (lexer_.Peek().type != TokenType::kComma) break;
            lexer_.Next();
        }
    }
    if (IsWord(lexer_.Peek(), "LIMIT")) {
        lexer_.Next();
        auto n = ParsePaginationCount("LIMIT");
        if (!n.ok()) return n.status();
        q.limit = n.value();
    }
    if (IsWord(lexer_.Peek(), "OFFSET")) {
        lexer_.Next();
        auto n = ParsePaginationCount("OFFSET");
        if (!n.ok()) return n.status();
        q.offset = n.value();
    }
    return Status::OK();
}

StatusOr<std::shared_ptr<const Expr>> Parser::ParseExprQuerySubquery(
    const Token& open, ExprOp op, std::shared_ptr<const Expr> lhs) {
    auto q = ParseNestedQuery(open);
    if (!q.ok()) return q.status();
    auto node = std::make_shared<Expr>();
    node->kind = Expr::Kind::kSubquery;
    node->op = op;
    node->byte_offset = lhs != nullptr ? lhs->byte_offset : open.byte_offset;
    node->op_byte_offset = open.byte_offset;
    node->query = std::move(q.value());
    if (lhs != nullptr) {
        node->height = lhs->height + 1;
        node->size = AddSize(lhs->size, 1);
        node->lhs = std::move(lhs);
    }
    return std::shared_ptr<const Expr>(std::move(node));
}

StatusOr<std::shared_ptr<const Expr>> Parser::ParseExprAggregate(const Token& name, AggFunc func) {
    if (expr_depth_ >= kMaxExprDepth) return ExprTooDeep(name.byte_offset);
    DepthScope scope(expr_depth_);
    auto node = std::make_shared<Expr>();
    node->kind = Expr::Kind::kAggregate;
    node->agg_func = func;
    node->byte_offset = name.byte_offset;
    node->op_byte_offset = name.byte_offset;
    if (lexer_.Peek().type == TokenType::kStar) {
        const Token star = lexer_.Next();
        if (func != AggFunc::kCount) {
            return Status::InvalidArgument(std::string(AggFuncText(func)) +
                                           " takes a value, not '*'" + At(star.byte_offset) +
                                           "; '*' is only an argument of COUNT");
        }
    } else {
        if (IsWord(lexer_.Peek(), "DISTINCT")) {
            lexer_.Next();
            node->agg_distinct = true;
        }
        auto arg = ParseExprOr();
        if (!arg.ok()) return arg;
        node->height = arg.value()->height + 1;
        node->size = AddSize(arg.value()->size, 1);
        node->lhs = std::move(arg.value());
    }
    if (Status s = ExpectToken(TokenType::kRParen, "')' closing an aggregate's argument"); !s.ok()) {
        return s;
    }
    if (const Token& w = lexer_.Peek(); IsWord(w, "OVER") || IsWord(w, "FILTER")) {
        return Status::NotImplemented("'" + std::string(w.text) + "' after an aggregate is not "
                                      "supported" + At(w.byte_offset));
    }
    return std::shared_ptr<const Expr>(std::move(node));
}

StatusOr<std::shared_ptr<const Expr>> Parser::ParseExprQueryComparison(
    std::shared_ptr<const Expr> lhs) {
    const Token first = lexer_.Peek();
    bool negated = false;
    if (first.type == TokenType::kKeyword && first.kw == Keyword::kNot) {
        lexer_.Next();
        negated = true;
    }
    const Token form = lexer_.Peek();
    std::shared_ptr<const Expr> node;
    if (form.type == TokenType::kKeyword && form.kw == Keyword::kIn) {
        lexer_.Next();
        if (lexer_.Peek().type != TokenType::kLParen) {
            return Status::InvalidArgument("expected '(' after IN" +
                                           At(lexer_.Peek().byte_offset));
        }
        const Token open = lexer_.Next();
        if (IsWord(lexer_.Peek(), "SELECT")) {
            auto sub = ParseExprQuerySubquery(open, ExprOp::kInSubquery, std::move(lhs));
            if (!sub.ok()) return sub;
            node = std::move(sub.value());
        } else {
            std::vector<std::shared_ptr<const Expr>> args{lhs};
            for (;;) {
                auto v = ParseExprOr();
                if (!v.ok()) return v;
                args.push_back(std::move(v.value()));
                if (args.size() > kMaxExprArgs) {
                    return Status::Unsupported("an IN list of more than " +
                                               std::to_string(kMaxExprArgs - 1) + " values" +
                                               At(form.byte_offset));
                }
                if (lexer_.Peek().type != TokenType::kComma) break;
                lexer_.Next();
            }
            if (Status s = ExpectToken(TokenType::kRParen, "')' closing an IN list"); !s.ok()) {
                return s;
            }
            node = MakeCall(ExprOp::kInList, lhs->byte_offset, form.byte_offset, std::move(args));
        }
    } else if (form.type == TokenType::kKeyword && form.kw == Keyword::kBetween) {
        lexer_.Next();
        auto lo = ParseExprConcat();
        if (!lo.ok()) return lo;
        if (!IsWord(lexer_.Peek(), "AND")) {
            return Status::InvalidArgument("expected AND in BETWEEN" +
                                           At(lexer_.Peek().byte_offset));
        }
        lexer_.Next();
        auto hi = ParseExprConcat();
        if (!hi.ok()) return hi;
        node = MakeBinary(ExprOp::kAnd, form.byte_offset,
                          MakeBinary(ExprOp::kGe, form.byte_offset, lhs, std::move(lo.value())),
                          MakeBinary(ExprOp::kLe, form.byte_offset, lhs, std::move(hi.value())));
    } else if (IsWord(form, "LIKE")) {
        lexer_.Next();
        auto pattern = ParseExprConcat();
        if (!pattern.ok()) return pattern;
        if (IsWord(lexer_.Peek(), "ESCAPE")) {
            return Status::NotImplemented("LIKE ... ESCAPE is not supported" +
                                          At(lexer_.Peek().byte_offset));
        }
        node = MakeBinary(ExprOp::kLike, form.byte_offset, std::move(lhs),
                          std::move(pattern.value()));
    } else if (negated) {
        return Status::InvalidArgument("expected IN, BETWEEN or LIKE after NOT" +
                                       At(form.byte_offset));
    } else {
        return lhs;
    }
    if (TooBig(*node)) return ExprTooDeep(form.byte_offset);
    if (negated) {
        const std::uint32_t at = node->byte_offset;  // read before node is moved from
        node = MakeUnary(ExprOp::kNot, at, first.byte_offset, std::move(node));
        if (TooBig(*node)) return ExprTooDeep(first.byte_offset);
    }
    return node;
}

StatusOr<Statement> Parse(std::string_view sql) { return Parser(sql).Parse(); }

}  // namespace kds::parser
