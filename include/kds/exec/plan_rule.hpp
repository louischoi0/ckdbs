#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/parser/lexer.hpp"

// **A managed plan's `WHEN` rule** (CX-S1b, instructions/v3.2.0/
// workorder-cx-managed-plans.md CX-R4, CX-Q5): what chooses one of an
// administrator's plan's variants for a statement, from the statement's
// arguments alone.
//
//   rule       := comparison { AND comparison }
//   comparison := sum op sum          op: = != < <= > >=
//   sum        := operand { (+ | -) operand }
//   operand    := ARG(k) | <integer> | <string>
//
// `ARG(k)` is the statement's k-th value position, 1-based: its literals in
// token order, however they arrived - written in the text or substituted at
// `C_BIND` (`parser::ArgumentsOf`, CX-R1a).
//
// **Total and pure** (CX-R4) `[quiet-wrong]-adjacent`: a rule reads nothing
// but the arguments - no page, no statistic, no clock - and every
// evaluation answers true or false. A comparison that cannot be decided is
// **false**, never an error: an argument of the other type (a string where
// the rule adds, an integer against a string constant), an integer past 64
// bits, a sum that overflows, a `?` that reached the compile unbound. A
// plan's last variant has no rule (`ELSE`), so a statement no rule holds
// for still has a variant. Whichever variant is chosen the statement's rows
// are the same - a variant is an order of an inner block (CX-R2) - so a
// rule that reads "wrong" costs time, never an answer.
//
// **Two types.** Integers take `+`, `-` and all six comparisons, in 64-bit
// arithmetic checked for overflow. A string takes `=` and `!=` against a
// string, by its bytes; a numeric literal with a point is a string, as it
// is in every statement (`token.hpp`), so `ARG(1) = 12.50` compares
// spellings. Ordering strings is refused `NotImplemented`.
//
// **No `IS NULL`**, against CX-Q5's adopted text: NULL is shape to the
// fingerprint (`fingerprint.hpp`), so a statement with a NULL in a value
// position is another shape, with its own fetch id and its own plan, and no
// argument of a matching statement is ever NULL. A spelling that can never
// hold is refused `Unsupported` rather than accepted.
//
// **Every comparison reads an argument**: one between constants is refused,
// since it would make a variant always or never chosen under a name that
// says otherwise.
//
// A rule's text is its identity in `sys.plans`: `PlanRuleText` writes the
// canonical form and `ParsePlanRule` reads it back at a mount.

namespace kds::exec {

struct PlanRuleOperand {
    enum class Kind : std::uint8_t { kArg, kInt, kStr };
    Kind kind = Kind::kInt;
    bool subtracted = false;  // `- operand`; never set on a sum's first
    std::uint32_t arg = 0;    // kArg: the value position, 1-based
    std::int64_t value = 0;   // kInt
    std::string text;         // kStr

    bool operator==(const PlanRuleOperand&) const = default;
};

enum class PlanRuleOp : std::uint8_t { kEq, kNeq, kLt, kLte, kGt, kGte };

struct PlanRuleComparison {
    std::vector<PlanRuleOperand> lhs;
    PlanRuleOp op = PlanRuleOp::kEq;
    std::vector<PlanRuleOperand> rhs;

    bool operator==(const PlanRuleComparison&) const = default;
};

// A conjunction. Empty is `ELSE`: it holds for every statement.
struct PlanRule {
    std::vector<PlanRuleComparison> terms;

    bool is_else() const noexcept { return terms.empty(); }
    // The highest value position the rule reads, 0 for `ELSE`.
    std::uint32_t max_arg() const noexcept;
    bool operator==(const PlanRule&) const = default;
};

// Reads a rule from `lexer`, which stands on its first token, and leaves it
// on the first token that does not continue the rule. Every refusal names
// the offending token's byte in the lexer's text.
StatusOr<PlanRule> ParsePlanRule(parser::Lexer& lexer);

// The canonical text, which `ParsePlanRule` reads back to an equal rule.
// Empty for `ELSE`.
std::string PlanRuleText(const PlanRule& rule);

// Whether every comparison of `rule` holds for `args`.
bool PlanRuleHolds(const PlanRule& rule, std::span<const parser::StatementArgument> args);

}  // namespace kds::exec
