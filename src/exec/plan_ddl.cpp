#include "kds/exec/plan_ddl.hpp"

#include <algorithm>
#include <utility>

#include "kds/exec/admin_plans.hpp"
#include "kds/parser/lexer.hpp"

namespace kds::exec {

namespace {

using parser::Token;
using parser::TokenType;

bool IsIdent(const Token& tok, std::string_view word) noexcept {
    return tok.type == TokenType::kIdent && SameName(tok.text, word);
}

std::string At(std::size_t byte) { return " (byte " + std::to_string(byte) + ")"; }
std::string At(const Token& tok) { return At(tok.byte_offset); }

// `<verb> PLAN <name>`, the head both statements share.
Status ParseHead(parser::Lexer& lexer, std::string_view verb, std::string& name,
                 std::size_t& name_at) {
    lexer.Next();  // the verb, which routed the statement here
    lexer.Next();  // PLAN
    const Token tok = lexer.Next();
    if (tok.type != TokenType::kIdent) {
        return Status::InvalidArgument(std::string(verb) + " PLAN takes a plan's name" + At(tok));
    }
    name = std::string(tok.text);
    name_at = tok.byte_offset;
    return Status::OK();
}

// `VARIANT <name> (WHEN <rule> | ELSE) ORDER (<binding>, ...)`, the lexer
// standing on `VARIANT`.
StatusOr<PlanVariantSpec> ParseVariant(parser::Lexer& lexer) {
    PlanVariantSpec variant;
    if (const Token head = lexer.Next(); !IsIdent(head, "VARIANT")) {
        return Status::InvalidArgument("expected VARIANT <name> WHEN <rule> | ELSE" + At(head));
    }
    const Token name = lexer.Next();
    if (name.type != TokenType::kIdent) {
        return Status::InvalidArgument("VARIANT takes a variant's name" + At(name));
    }
    variant.name = std::string(name.text);
    variant.name_at = name.byte_offset;
    const Token kind = lexer.Next();
    if (IsIdent(kind, "WHEN")) {
        auto rule = ParsePlanRule(lexer);
        if (!rule.ok()) return rule.status();
        variant.rule = std::move(rule.value());
    } else if (!IsIdent(kind, "ELSE")) {
        return Status::InvalidArgument("a variant is chosen by WHEN <rule> or by ELSE" + At(kind));
    }
    if (const Token order = lexer.Next(); !IsIdent(order, "ORDER")) {
        return Status::InvalidArgument("a variant states its join order: ORDER (<relation>, "
                                       "...)" + At(order));
    }
    const Token open = lexer.Next();
    if (open.type != TokenType::kLParen) {
        return Status::InvalidArgument("ORDER takes a parenthesised list of relations" + At(open));
    }
    variant.order_at = open.byte_offset;
    while (true) {
        const Token relation = lexer.Next();
        if (relation.type != TokenType::kIdent) {
            return Status::InvalidArgument(
                "ORDER lists the statement's relations by alias, or by table name where it "
                "wrote none" + At(relation));
        }
        variant.order.emplace_back(relation.text);
        const Token sep = lexer.Next();
        if (sep.type == TokenType::kRParen) break;
        if (sep.type != TokenType::kComma) {
            return Status::InvalidArgument("ORDER's relations are separated by ','" + At(sep));
        }
    }
    // ACCESS <binding> <pin> {, <binding> <pin>} (CX-S3a).
    if (IsIdent(lexer.Peek(), "ACCESS")) {
        lexer.Next();
        while (true) {
            PlanVariantSpec::Access access;
            const Token binding = lexer.Next();
            if (binding.type != TokenType::kIdent) {
                return Status::InvalidArgument(
                    "ACCESS names a relation of the statement, then its pin" + At(binding));
            }
            access.binding = std::string(binding.text);
            access.binding_at = binding.byte_offset;
            Token arm = lexer.Next();
            if (arm.type == TokenType::kKeyword && arm.kw == parser::Keyword::kNot) {
                access.pin.forbid = true;
                arm = lexer.Next();
            }
            if (IsIdent(arm, "TUPLE_KEY")) {
                access.pin.arm = ArmPin::Arm::kTupleKey;
            } else if (IsIdent(arm, "INDEX")) {
                access.pin.arm = ArmPin::Arm::kIndex;
                // A pinned index may be named; a forbidden one is every index.
                if (!access.pin.forbid && lexer.Peek().type == TokenType::kIdent &&
                    !IsIdent(lexer.Peek(), "VARIANT") && !IsIdent(lexer.Peek(), "BUILD")) {
                    access.pin.index = std::string(lexer.Next().text);
                }
            } else if (IsIdent(arm, "CABIN")) {
                access.pin.arm = ArmPin::Arm::kCabin;
            } else if (IsIdent(arm, "SCAN")) {
                if (access.pin.forbid) {
                    return Status::InvalidArgument(
                        "NOT SCAN is no pin: the walk is what every other arm falls back "
                        "to" + At(arm));
                }
                access.pin.arm = ArmPin::Arm::kScan;
            } else {
                return Status::InvalidArgument(
                    "an ACCESS pin is TUPLE_KEY, INDEX [<name>], CABIN or SCAN, or NOT before "
                    "one of the first three" + At(arm));
            }
            variant.access.push_back(std::move(access));
            if (lexer.Peek().type != TokenType::kComma) break;
            lexer.Next();
        }
    }
    // The clause of the stage that pins a build.
    if (const Token& next = lexer.Peek(); IsIdent(next, "BUILD")) {
        return Status::NotImplemented(
            "BUILD in a plan variant is not implemented: a variant states a join order and "
            "access pins" + At(next));
    }
    return variant;
}

// What may follow a statement's last clause: one `;`, then nothing.
Status ExpectEnd(parser::Lexer& lexer, std::string_view what) {
    Token tok = lexer.Next();
    if (tok.type == TokenType::kSemicolon) tok = lexer.Next();
    if (tok.type != TokenType::kEof) {
        return Status::InvalidArgument("nothing follows " + std::string(what) + At(tok));
    }
    return Status::OK();
}

}  // namespace

StatusOr<CreatePlanStmt> ParseCreatePlan(std::string_view line) {
    CreatePlanStmt stmt;
    // Every token, so the statement's end can be found before anything
    // parses it.
    std::vector<Token> tokens;
    {
        parser::Lexer lexer(line);
        for (Token tok = lexer.Next(); tok.type != TokenType::kEof; tok = lexer.Next()) {
            if (tok.type == TokenType::kError) {
                return Status::InvalidArgument("unreadable character" + At(tok));
            }
            tokens.push_back(tok);
        }
    }
    {
        parser::Lexer head(line);
        if (Status s = ParseHead(head, "CREATE", stmt.name, stmt.name_at); !s.ok()) return s;
    }
    // CREATE PLAN <name> FOR: tokens 0..3.
    if (tokens.size() < 4 || !IsIdent(tokens[3], "FOR")) {
        return Status::InvalidArgument(
            "CREATE PLAN <name> is followed by FOR <select>" +
            At(tokens.size() > 3 ? tokens[3].byte_offset : line.size()));
    }
    std::size_t last = tokens.size();
    if (tokens.back().type == TokenType::kSemicolon) --last;
    // The statement's end: `AS SEARCHED` closing the command, or the first
    // variant head at depth 0.
    std::size_t clause = last;  // index of the first token past the statement
    if (last >= 6 && tokens[last - 2].type == TokenType::kKeyword &&
        tokens[last - 2].kw == parser::Keyword::kAs && IsIdent(tokens[last - 1], "SEARCHED")) {
        stmt.searched = true;
        clause = last - 2;
    } else {
        std::size_t depth = 0;
        for (std::size_t i = 4; i + 2 < last; ++i) {
            if (tokens[i].type == TokenType::kLParen) ++depth;
            if (tokens[i].type == TokenType::kRParen && depth > 0) --depth;
            if (depth == 0 && IsIdent(tokens[i], "VARIANT") &&
                tokens[i + 1].type == TokenType::kIdent &&
                (IsIdent(tokens[i + 2], "WHEN") || IsIdent(tokens[i + 2], "ELSE"))) {
                clause = i;
                break;
            }
        }
        if (clause == last) {
            return Status::InvalidArgument(
                "CREATE PLAN ... FOR <select> is followed by VARIANT <name> WHEN <rule> | "
                "ELSE ORDER (...), or by AS SEARCHED" + At(line.size()));
        }
    }
    if (clause == 4) {
        return Status::InvalidArgument("CREATE PLAN ... FOR takes a SELECT" +
                                       At(tokens[4].byte_offset));
    }
    stmt.statement_at = tokens[4].byte_offset;
    stmt.statement_size = tokens[clause - 1].byte_offset + tokens[clause - 1].length -
                          stmt.statement_at;
    if (stmt.searched) return stmt;

    // The variants, read from the command with everything before them
    // blanked, so every byte a refusal names is the command's own.
    std::string clauses(line);
    std::fill(clauses.begin(),
              clauses.begin() + static_cast<std::ptrdiff_t>(tokens[clause].byte_offset), ' ');
    parser::Lexer lexer(clauses);
    while (true) {
        auto variant = ParseVariant(lexer);
        if (!variant.ok()) return variant.status();
        for (const PlanVariantSpec& earlier : stmt.variants) {
            if (SameName(earlier.name, variant.value().name)) {
                return Status::InvalidArgument("the plan already has a variant named '" +
                                               variant.value().name + "'" +
                                               At(variant.value().name_at));
            }
        }
        const bool is_else = variant.value().rule.is_else();
        stmt.variants.push_back(std::move(variant.value()));
        if (is_else) break;
        if (stmt.variants.size() == kMaxPlanVariants) {
            return Status::Unsupported("a plan holds at most " +
                                       std::to_string(kMaxPlanVariants) +
                                       " variants, its ELSE among them" +
                                       At(lexer.Peek().byte_offset));
        }
        // CX-R4: the list ends in `ELSE`, so a statement no rule holds for
        // has a variant.
        if (const Token& next = lexer.Peek(); !IsIdent(next, "VARIANT")) {
            return Status::InvalidArgument(
                "a plan's last variant is VARIANT <name> ELSE ORDER (...): every statement "
                "must have one" + At(next));
        }
    }
    if (Status s = ExpectEnd(lexer, "a plan's ELSE variant"); !s.ok()) return s;
    return stmt;
}

StatusOr<CapturedOrder> ResolvePlanOrder(const std::vector<std::string>& bindings,
                                         const PlanVariantSpec& variant) {
    const auto refuse = [&](const std::string& what) {
        return Status::InvalidArgument("variant '" + variant.name + "': " + what +
                                       At(variant.order_at));
    };
    CapturedOrder order{};
    std::vector<bool> placed(bindings.size(), false);
    for (std::size_t k = 0; k < variant.order.size(); ++k) {
        std::size_t position = bindings.size();
        for (std::size_t w = 0; w < bindings.size(); ++w) {
            if (SameName(bindings[w], variant.order[k])) position = w;
        }
        if (position == bindings.size()) {
            return refuse("ORDER names '" + variant.order[k] +
                          "', which the statement does not bind");
        }
        if (placed[position]) return refuse("ORDER names '" + variant.order[k] + "' twice");
        placed[position] = true;
        order[k] = static_cast<std::uint8_t>(position);
    }
    if (variant.order.size() != bindings.size()) {
        return refuse("ORDER names " + std::to_string(variant.order.size()) + " of the statement's " +
                      std::to_string(bindings.size()) + " relations; it must name each once");
    }
    return order;
}

StatusOr<PlanVariant> ResolvePlanVariant(const std::vector<std::string>& bindings,
                                         std::uint32_t arguments, const PlanVariantSpec& spec) {
    if (spec.rule.max_arg() > arguments) {
        return Status::InvalidArgument(
            "variant '" + spec.name + "' reads ARG(" + std::to_string(spec.rule.max_arg()) +
            "), and the statement has " + std::to_string(arguments) + " value position(s)" +
            At(spec.name_at));
    }
    auto order = ResolvePlanOrder(bindings, spec);
    if (!order.ok()) return order.status();
    PlanVariant variant{spec.name, spec.rule, order.value(), {}};
    if (!spec.access.empty()) variant.access.resize(bindings.size());
    for (const PlanVariantSpec::Access& access : spec.access) {
        std::size_t position = bindings.size();
        for (std::size_t w = 0; w < bindings.size(); ++w) {
            if (SameName(bindings[w], access.binding)) position = w;
        }
        if (position == bindings.size()) {
            return Status::InvalidArgument("variant '" + spec.name + "': ACCESS names '" +
                                           access.binding +
                                           "', which the statement does not bind" +
                                           At(access.binding_at));
        }
        if (variant.access[position].pins()) {
            return Status::InvalidArgument("variant '" + spec.name + "': ACCESS pins '" +
                                           access.binding + "' twice" + At(access.binding_at));
        }
        variant.access[position] = access.pin;
    }
    return variant;
}

std::string PlanAccessText(const std::vector<ArmPin>& access,
                           const std::vector<std::string>& bindings) {
    std::string out;
    for (std::size_t w = 0; w < access.size() && w < bindings.size(); ++w) {
        const ArmPin& pin = access[w];
        if (!pin.pins()) continue;
        out.append(out.empty() ? " ACCESS " : ", ").append(bindings[w]);
        out.append(pin.forbid ? " NOT " : " ").append(ArmName(pin.arm));
        if (!pin.index.empty()) out.append(" ").append(pin.index);
    }
    return out;
}

StatusOr<AlterPlanStmt> ParseAlterPlan(std::string_view line) {
    AlterPlanStmt stmt;
    parser::Lexer lexer(line);
    if (Status s = ParseHead(lexer, "ALTER", stmt.name, stmt.name_at); !s.ok()) return s;
    const Token action = lexer.Next();
    if (IsIdent(action, "ENABLE") || IsIdent(action, "DISABLE")) {
        stmt.action = IsIdent(action, "ENABLE") ? AlterPlanStmt::Action::kEnable
                                                : AlterPlanStmt::Action::kDisable;
        if (Status s = ExpectEnd(lexer, "ALTER PLAN's " + std::string(action.text)); !s.ok()) {
            return s;
        }
        return stmt;
    }
    if (!IsIdent(action, "REPLACE")) {
        return Status::InvalidArgument(
            "ALTER PLAN takes ENABLE, DISABLE or REPLACE VARIANT" + At(action));
    }
    stmt.action = AlterPlanStmt::Action::kReplaceVariant;
    auto variant = ParseVariant(lexer);
    if (!variant.ok()) return variant.status();
    stmt.variant = std::move(variant.value());
    if (Status s = ExpectEnd(lexer, "the replaced variant"); !s.ok()) return s;
    return stmt;
}

}  // namespace kds::exec
