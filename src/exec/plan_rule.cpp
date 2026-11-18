#include "kds/exec/plan_rule.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <utility>

namespace kds::exec {

namespace {

using parser::StatementArgument;
using parser::Token;
using parser::TokenType;

std::string At(const Token& tok) { return " (byte " + std::to_string(tok.byte_offset) + ")"; }

// The integer `text` spells, or nullopt past 64 bits: the lexer's own
// decode wraps there (`Token::int_val`), and a wrapped value would compare.
std::optional<std::int64_t> IntegerOf(std::string_view text) noexcept {
    std::int64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

std::optional<PlanRuleOp> OpOf(TokenType type) noexcept {
    switch (type) {
        case TokenType::kEq: return PlanRuleOp::kEq;
        case TokenType::kNeq: return PlanRuleOp::kNeq;
        case TokenType::kLt: return PlanRuleOp::kLt;
        case TokenType::kLte: return PlanRuleOp::kLte;
        case TokenType::kGt: return PlanRuleOp::kGt;
        case TokenType::kGte: return PlanRuleOp::kGte;
        default: return std::nullopt;
    }
}

std::string_view OpText(PlanRuleOp op) noexcept {
    switch (op) {
        case PlanRuleOp::kEq: return "=";
        case PlanRuleOp::kNeq: return "!=";
        case PlanRuleOp::kLt: return "<";
        case PlanRuleOp::kLte: return "<=";
        case PlanRuleOp::kGt: return ">";
        case PlanRuleOp::kGte: return ">=";
    }
    return "?";
}

template <typename T>
bool Compare(const T& a, PlanRuleOp op, const T& b) noexcept {
    switch (op) {
        case PlanRuleOp::kEq: return a == b;
        case PlanRuleOp::kNeq: return a != b;
        case PlanRuleOp::kLt: return a < b;
        case PlanRuleOp::kLte: return a <= b;
        case PlanRuleOp::kGt: return a > b;
        case PlanRuleOp::kGte: return a >= b;
    }
    return false;
}

StatusOr<PlanRuleOperand> ParseOperand(parser::Lexer& lexer) {
    const Token tok = lexer.Next();
    PlanRuleOperand operand;
    switch (tok.type) {
        case TokenType::kIntLit: {
            const auto value = IntegerOf(tok.text);
            if (!value.has_value()) {
                return Status::InvalidArgument("a WHEN integer must fit 64 bits" + At(tok));
            }
            operand.kind = PlanRuleOperand::Kind::kInt;
            operand.value = *value;
            return operand;
        }
        case TokenType::kNumLit:
        case TokenType::kStrLit:
            // A rule's text is one line of a `sys.plans` row.
            if (std::any_of(tok.text.begin(), tok.text.end(),
                            [](char c) { return static_cast<unsigned char>(c) < 0x20; })) {
                return Status::InvalidArgument(
                    "a WHEN string constant holds no control character" + At(tok));
            }
            operand.kind = PlanRuleOperand::Kind::kStr;
            operand.text = std::string(tok.text);
            return operand;
        case TokenType::kIdent: {
            if (!SameName(tok.text, "ARG")) break;
            if (lexer.Next().type != TokenType::kLParen) {
                return Status::InvalidArgument("ARG takes a value position: ARG(k)" + At(tok));
            }
            const Token position = lexer.Next();
            const auto k = position.type == TokenType::kIntLit ? IntegerOf(position.text)
                                                               : std::nullopt;
            if (!k.has_value() || *k < 1 || *k > UINT32_MAX) {
                return Status::InvalidArgument(
                    "ARG(k) names a value position, counted from 1" + At(position));
            }
            if (const Token close = lexer.Next(); close.type != TokenType::kRParen) {
                return Status::InvalidArgument("ARG(k) is closed by ')'" + At(close));
            }
            operand.kind = PlanRuleOperand::Kind::kArg;
            operand.arg = static_cast<std::uint32_t>(*k);
            return operand;
        }
        case TokenType::kNullLit:
            return Status::Unsupported(
                "NULL in a WHEN rule is not supported: a NULL in a value position is another "
                "statement shape, with a plan of its own" + At(tok));
        case TokenType::kParam:
        case TokenType::kNamedParam:
            return Status::InvalidArgument(
                "a WHEN rule names a value position as ARG(k), not as a placeholder" + At(tok));
        default:
            break;
    }
    return Status::InvalidArgument(
        "a WHEN rule compares ARG(k), integers and strings; '" + std::string(tok.text) +
        "' is none" + At(tok));
}

StatusOr<std::vector<PlanRuleOperand>> ParseSum(parser::Lexer& lexer) {
    std::vector<PlanRuleOperand> sum;
    auto first = ParseOperand(lexer);
    if (!first.ok()) return first.status();
    sum.push_back(std::move(first.value()));
    while (true) {
        const Token& next = lexer.Peek();
        // `ARG(1) -5`: the lexer fuses a `-` with the digit after it, and in
        // binary position that literal is the subtraction (BJ-Q9 (a)).
        const bool fused = next.type == TokenType::kIntLit && next.negative;
        if (next.type != TokenType::kPlus && next.type != TokenType::kMinus && !fused) break;
        const bool subtract = next.type == TokenType::kMinus;
        const Token op = fused ? next : lexer.Next();
        auto operand = ParseOperand(lexer);
        if (!operand.ok()) return operand.status();
        operand.value().subtracted = subtract;
        sum.push_back(std::move(operand.value()));
        // Arithmetic is the integers': a string is compared alone.
        for (const PlanRuleOperand& o : sum) {
            if (o.kind == PlanRuleOperand::Kind::kStr) {
                return Status::InvalidArgument(
                    "a WHEN rule adds and subtracts integers only; a string is compared "
                    "alone" + At(op));
            }
        }
    }
    return sum;
}

bool IsString(const std::vector<PlanRuleOperand>& sum) noexcept {
    return sum.size() == 1 && sum[0].kind == PlanRuleOperand::Kind::kStr;
}

bool ReadsArgument(const std::vector<PlanRuleOperand>& sum) noexcept {
    return std::any_of(sum.begin(), sum.end(), [](const PlanRuleOperand& o) {
        return o.kind == PlanRuleOperand::Kind::kArg;
    });
}

// A sum that is one argument and nothing else: what a string is compared
// with.
bool IsBareArgument(const std::vector<PlanRuleOperand>& sum) noexcept {
    return sum.size() == 1 && sum[0].kind == PlanRuleOperand::Kind::kArg;
}

void AppendSum(std::string& out, const std::vector<PlanRuleOperand>& sum) {
    for (std::size_t i = 0; i < sum.size(); ++i) {
        if (i != 0) out.append(sum[i].subtracted ? " - " : " + ");
        switch (sum[i].kind) {
            case PlanRuleOperand::Kind::kArg:
                out.append("ARG(").append(std::to_string(sum[i].arg)).push_back(')');
                break;
            case PlanRuleOperand::Kind::kInt: out.append(std::to_string(sum[i].value)); break;
            case PlanRuleOperand::Kind::kStr:
                out.append("'").append(sum[i].text).push_back('\'');
                break;
        }
    }
}

const StatementArgument* ArgumentAt(std::span<const StatementArgument> args,
                                    std::uint32_t k) noexcept {
    return k >= 1 && k <= args.size() ? &args[k - 1] : nullptr;
}

// The sum's value, or nullopt where it has none: an operand that is not an
// integer of 64 bits, or an overflow.
std::optional<std::int64_t> SumOf(const std::vector<PlanRuleOperand>& sum,
                                  std::span<const StatementArgument> args) noexcept {
    std::int64_t total = 0;
    for (const PlanRuleOperand& operand : sum) {
        std::optional<std::int64_t> value;
        if (operand.kind == PlanRuleOperand::Kind::kInt) {
            value = operand.value;
        } else if (operand.kind == PlanRuleOperand::Kind::kArg) {
            const StatementArgument* arg = ArgumentAt(args, operand.arg);
            if (arg != nullptr && arg->kind == StatementArgument::Kind::kInt) {
                value = IntegerOf(arg->text);
            }
        }
        if (!value.has_value()) return std::nullopt;
        if (operand.subtracted ? __builtin_sub_overflow(total, *value, &total)
                               : __builtin_add_overflow(total, *value, &total)) {
            return std::nullopt;
        }
    }
    return total;
}

// The string a one-operand sum is, or nullopt: a constant's text, or an
// argument that is a string.
std::optional<std::string_view> StringOf(const std::vector<PlanRuleOperand>& sum,
                                         std::span<const StatementArgument> args) noexcept {
    if (IsString(sum)) return sum[0].text;
    if (!IsBareArgument(sum)) return std::nullopt;
    const StatementArgument* arg = ArgumentAt(args, sum[0].arg);
    if (arg == nullptr || arg->kind != StatementArgument::Kind::kStr) return std::nullopt;
    return arg->text;
}

bool Holds(const PlanRuleComparison& term, std::span<const StatementArgument> args) noexcept {
    const bool equality = term.op == PlanRuleOp::kEq || term.op == PlanRuleOp::kNeq;
    if (equality) {
        // Two strings: a constant and an argument, or two arguments that
        // both arrived as strings.
        const auto a = StringOf(term.lhs, args);
        const auto b = StringOf(term.rhs, args);
        if (a.has_value() && b.has_value()) return Compare(*a, term.op, *b);
    }
    const auto a = SumOf(term.lhs, args);
    const auto b = SumOf(term.rhs, args);
    return a.has_value() && b.has_value() && Compare(*a, term.op, *b);
}

}  // namespace

bool SameName(std::string_view a, std::string_view b) noexcept {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        const auto fold = [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        };
        return fold(x) == fold(y);
    });
}

std::uint32_t PlanRule::max_arg() const noexcept {
    std::uint32_t highest = 0;
    for (const PlanRuleComparison& term : terms) {
        for (const auto* sum : {&term.lhs, &term.rhs}) {
            for (const PlanRuleOperand& operand : *sum) {
                if (operand.kind == PlanRuleOperand::Kind::kArg) {
                    highest = std::max(highest, operand.arg);
                }
            }
        }
    }
    return highest;
}

StatusOr<PlanRule> ParsePlanRule(parser::Lexer& lexer) {
    PlanRule rule;
    while (true) {
        PlanRuleComparison term;
        const Token head = lexer.Peek();
        auto lhs = ParseSum(lexer);
        if (!lhs.ok()) return lhs.status();
        term.lhs = std::move(lhs.value());
        const Token op = lexer.Next();
        const auto parsed_op = OpOf(op.type);
        if (!parsed_op.has_value()) {
            if (op.type == TokenType::kIdent && SameName(op.text, "IS")) {
                return Status::Unsupported(
                    "IS NULL in a WHEN rule is not supported: a NULL in a value position is "
                    "another statement shape, with a plan of its own" + At(op));
            }
            return Status::InvalidArgument(
                "a WHEN comparison takes =, !=, <, <=, > or >= here" + At(op));
        }
        term.op = *parsed_op;
        auto rhs = ParseSum(lexer);
        if (!rhs.ok()) return rhs.status();
        term.rhs = std::move(rhs.value());

        if (!ReadsArgument(term.lhs) && !ReadsArgument(term.rhs)) {
            return Status::InvalidArgument(
                "a WHEN comparison reads at least one ARG(k); one between constants always "
                "or never holds" + At(head));
        }
        if (IsString(term.lhs) || IsString(term.rhs)) {
            // A string against the one thing it can equal: a bare argument.
            const auto& other = IsString(term.lhs) ? term.rhs : term.lhs;
            if (!IsBareArgument(other)) {
                return Status::InvalidArgument(
                    "a WHEN rule compares a string with ARG(k) alone" + At(head));
            }
            if (term.op != PlanRuleOp::kEq && term.op != PlanRuleOp::kNeq) {
                return Status::NotImplemented(
                    "ordering strings in a WHEN rule is not implemented: a string takes = "
                    "and != only" + At(op));
            }
        }
        rule.terms.push_back(std::move(term));

        const Token& next = lexer.Peek();
        if (next.type != TokenType::kIdent) break;
        if (SameName(next.text, "AND")) {
            lexer.Next();
            continue;
        }
        if (SameName(next.text, "OR")) {
            return Status::NotImplemented(
                "OR in a WHEN rule is not implemented: a rule is comparisons joined by AND, "
                "and another variant is its OR" + At(next));
        }
        break;
    }
    return rule;
}

std::string PlanRuleText(const PlanRule& rule) {
    std::string out;
    for (std::size_t i = 0; i < rule.terms.size(); ++i) {
        if (i != 0) out.append(" AND ");
        AppendSum(out, rule.terms[i].lhs);
        out.append(" ").append(OpText(rule.terms[i].op)).append(" ");
        AppendSum(out, rule.terms[i].rhs);
    }
    return out;
}

bool PlanRuleHolds(const PlanRule& rule, std::span<const StatementArgument> args) {
    return std::all_of(rule.terms.begin(), rule.terms.end(),
                       [&](const PlanRuleComparison& term) { return Holds(term, args); });
}

}  // namespace kds::exec
