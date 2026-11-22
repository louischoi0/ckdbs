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

// A relation's binding as a plan writes it: a name, or a view's inner
// relation by its view's path - `v.o`, `v.w.o` (DP-Q7) - `first` being its
// first name, already read. A `.` not followed by a name is refused there.
StatusOr<std::string> BindingPath(parser::Lexer& lexer, const Token& first) {
    std::string path(first.text);
    while (lexer.Peek().type == TokenType::kDot) {
        lexer.Next();
        const Token next = lexer.Next();
        if (next.type != TokenType::kIdent) {
            return Status::InvalidArgument("a view's path names a relation after each '.'" +
                                           At(next));
        }
        path.append(".").append(next.text);
    }
    return path;
}

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
        auto path = BindingPath(lexer, relation);
        if (!path.ok()) return path.status();
        variant.order.push_back(std::move(path.value()));
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
            auto path = BindingPath(lexer, binding);
            if (!path.ok()) return path.status();
            access.binding = std::move(path.value());
            access.binding_at = binding.byte_offset;
            Token arm = lexer.Next();
            if (arm.type == TokenType::kKeyword && arm.kw == parser::Keyword::kNot) {
                access.pin.forbid = true;
                arm = lexer.Next();
            }
            const auto named = arm.type == TokenType::kIdent ? ArmFromName(arm.text)
                                                             : std::nullopt;
            if (!named.has_value()) {
                return Status::InvalidArgument(
                    "an ACCESS pin is TUPLE_KEY, INDEX [<name>], CABIN [(<column>)] or SCAN, or "
                    "NOT before one of the first three" + At(arm));
            }
            access.pin.arm = *named;
            if (access.pin.forbid && access.pin.arm == ArmPin::Arm::kScan) {
                return Status::InvalidArgument(
                    "NOT SCAN is no pin: the walk is what every other arm falls back to" +
                    At(arm));
            }
            // A pinned index may be named; a forbidden one is every index.
            if (access.pin.arm == ArmPin::Arm::kIndex &&
                lexer.Peek().type == TokenType::kIdent && !IsIdent(lexer.Peek(), "VARIANT") &&
                !IsIdent(lexer.Peek(), "BUILD")) {
                const Token index = lexer.Next();
                if (access.pin.forbid) {
                    return Status::InvalidArgument(
                        "NOT INDEX takes no name: it rules out every index" + At(index));
                }
                access.pin.name = std::string(index.text);
                access.name_at = index.byte_offset;
            }
            // A pinned Cabin may be named by the column it is on (CX-S3b):
            // the one spelling that admits a controller-made Cabin.
            if (access.pin.arm == ArmPin::Arm::kCabin &&
                lexer.Peek().type == TokenType::kLParen) {
                const Token open = lexer.Next();
                if (access.pin.forbid) {
                    return Status::InvalidArgument(
                        "NOT CABIN takes no name: it rules out every Cabin" + At(open));
                }
                const Token column = lexer.Next();
                // `()` and a list never closed are simply wrong; anything
                // else between the parentheses is a Cabin key this pin
                // cannot name.
                const Token& after = lexer.Peek();
                if (column.type == TokenType::kRParen || column.type == TokenType::kEof ||
                    (column.type == TokenType::kIdent && after.type == TokenType::kEof)) {
                    return Status::InvalidArgument(
                        "CABIN (<column>) names the column its Cabin is on" +
                        At(column.type == TokenType::kIdent ? after : column));
                }
                if (column.type != TokenType::kIdent || after.type != TokenType::kRParen) {
                    return Status::NotImplemented(
                        "a pin naming an expression or predicate Cabin is not implemented: "
                        "CABIN (<column>) names the Cabin on one column" + At(column));
                }
                lexer.Next();  // )
                access.pin.name = std::string(column.text);
                access.name_at = column.byte_offset;
            }
            variant.access.push_back(std::move(access));
            if (lexer.Peek().type != TokenType::kComma) break;
            lexer.Next();
        }
    }
    // BUILD <binding> ON | OFF {, <binding> ON | OFF} (CX-S5).
    if (IsIdent(lexer.Peek(), "BUILD")) {
        lexer.Next();
        while (true) {
            PlanVariantSpec::Build build;
            const Token binding = lexer.Next();
            if (binding.type != TokenType::kIdent) {
                return Status::InvalidArgument(
                    "BUILD names a relation of the statement, then ON or OFF" + At(binding));
            }
            auto path = BindingPath(lexer, binding);
            if (!path.ok()) return path.status();
            build.binding = std::move(path.value());
            build.binding_at = binding.byte_offset;
            const Token word = lexer.Next();
            if (word.type == TokenType::kKeyword && word.kw == parser::Keyword::kOn) {
                build.build = ArmPin::Build::kOn;
            } else if (IsIdent(word, "OFF")) {
                build.build = ArmPin::Build::kOff;
            } else {
                return Status::InvalidArgument("a BUILD pin is ON or OFF" + At(word));
            }
            variant.builds.push_back(std::move(build));
            if (lexer.Peek().type != TokenType::kComma) break;
            lexer.Next();
        }
    }
    return variant;
}

// Every token of `line`, an unreadable character refused at its byte.
StatusOr<std::vector<Token>> LexAll(std::string_view line) {
    std::vector<Token> tokens;
    parser::Lexer lexer(line);
    for (Token tok = lexer.Next(); tok.type != TokenType::kEof; tok = lexer.Next()) {
        if (tok.type == TokenType::kError) {
            return Status::InvalidArgument("unreadable character" + At(tok));
        }
        tokens.push_back(tok);
    }
    return tokens;
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

StatusOr<AdvisePlanStmt> ParseAdvisePlan(std::string_view line) {
    auto lexed = LexAll(line);
    if (!lexed.ok()) return lexed.status();
    const std::vector<Token>& tokens = lexed.value();
    // ADVISE PLAN FOR: tokens 0..2, the routing having read the first two.
    if (tokens.size() < 3 || !IsIdent(tokens[2], "FOR")) {
        return Status::InvalidArgument(
            "ADVISE PLAN is followed by FOR <select>" +
            At(tokens.size() > 2 ? tokens[2].byte_offset : line.size()));
    }
    std::size_t last = tokens.size();
    if (tokens.back().type == TokenType::kSemicolon) --last;
    if (last == 3) {
        return Status::InvalidArgument("ADVISE PLAN FOR takes a SELECT" + At(line.size()));
    }
    AdvisePlanStmt stmt;
    stmt.statement_at = tokens[3].byte_offset;
    stmt.statement_size =
        tokens[last - 1].byte_offset + tokens[last - 1].length - stmt.statement_at;
    return stmt;
}

StatusOr<CreatePlanStmt> ParseCreatePlan(std::string_view line) {
    CreatePlanStmt stmt;
    // Every token, so the statement's end can be found before anything
    // parses it.
    auto lexed = LexAll(line);
    if (!lexed.ok()) return lexed.status();
    const std::vector<Token>& tokens = lexed.value();
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

std::size_t BindingPosition(const std::vector<std::string>& bindings, std::string_view binding) {
    for (std::size_t w = 0; w < bindings.size(); ++w) {
        if (SameName(bindings[w], binding)) return w;
    }
    return bindings.size();
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
        const std::size_t position = BindingPosition(bindings, variant.order[k]);
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
    if (!spec.access.empty() || !spec.builds.empty()) variant.access.resize(bindings.size());
    const auto unbound = [&](std::string_view clause, const std::string& binding,
                             std::size_t at) {
        return Status::InvalidArgument("variant '" + spec.name + "': " + std::string(clause) +
                                       " names '" + binding +
                                       "', which the statement does not bind" + At(at));
    };
    for (const PlanVariantSpec::Access& access : spec.access) {
        const std::size_t position = BindingPosition(bindings, access.binding);
        if (position == bindings.size()) {
            return unbound("ACCESS", access.binding, access.binding_at);
        }
        if (variant.access[position].arm != ArmPin::Arm::kLadder) {
            return Status::InvalidArgument("variant '" + spec.name + "': ACCESS pins '" +
                                           access.binding + "' twice" + At(access.binding_at));
        }
        variant.access[position] = access.pin;
    }
    for (const PlanVariantSpec::Build& build : spec.builds) {
        const std::size_t position = BindingPosition(bindings, build.binding);
        if (position == bindings.size()) return unbound("BUILD", build.binding, build.binding_at);
        ArmPin& pin = variant.access[position];
        if (pin.build != ArmPin::Build::kLadder) {
            return Status::InvalidArgument("variant '" + spec.name + "': BUILD pins '" +
                                           build.binding + "' twice" + At(build.binding_at));
        }
        // The build is a walk's annotation: a pin holding the step to a
        // keyed class leaves it none to take.
        if (build.build == ArmPin::Build::kOn && pin.keyed()) {
            return Status::InvalidArgument(
                "variant '" + spec.name + "': BUILD " + build.binding + " ON is the walk's, and '" +
                build.binding + "' is pinned to " + std::string(ArmName(pin.arm)) +
                At(build.binding_at));
        }
        pin.build = build.build;
    }
    return variant;
}

std::string PlanAccessText(const std::vector<ArmPin>& access,
                           const std::vector<std::string>& bindings) {
    std::string out;
    for (std::size_t w = 0; w < access.size() && w < bindings.size(); ++w) {
        const ArmPin& pin = access[w];
        if (pin.arm == ArmPin::Arm::kLadder) continue;
        out.append(out.empty() ? " ACCESS " : ", ").append(bindings[w]);
        out.append(pin.forbid ? " NOT " : " ").append(ArmName(pin.arm));
        if (pin.name.empty()) continue;
        if (pin.arm == ArmPin::Arm::kCabin) {
            out.append(" (").append(pin.name).append(")");
        } else {
            out.append(" ").append(pin.name);
        }
    }
    bool first_build = true;
    for (std::size_t w = 0; w < access.size() && w < bindings.size(); ++w) {
        if (access[w].build == ArmPin::Build::kLadder) continue;
        out.append(first_build ? " BUILD " : ", ").append(bindings[w]);
        out.append(" ").append(BuildName(access[w].build));
        first_build = false;
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
