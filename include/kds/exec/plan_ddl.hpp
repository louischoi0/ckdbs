#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/exec/captured_plans.hpp"
#include "kds/exec/plan_rule.hpp"

// **The plan DDL's grammar** (CX-S1b, instructions/v3.2.0/
// workorder-cx-managed-plans.md CX-S1). Routed on the statement's first two
// words, before the parser, as every declaration's DDL is; this reads the
// rest.
//
//   CREATE PLAN <name> FOR <select>
//     VARIANT <v> WHEN <rule> ORDER (<binding>, ...)
//     ...
//     VARIANT <w> ELSE ORDER (<binding>, ...)
//   CREATE PLAN <name> FOR <select> AS SEARCHED
//   ALTER PLAN <name> ENABLE | DISABLE
//   ALTER PLAN <name> REPLACE VARIANT <v> (WHEN <rule> | ELSE) ORDER (...)
//
// **Where the `<select>` ends.** No word here is reserved (CLAUDE.md,
// Working Rules), so the statement's end is found on the token stream: the
// first `VARIANT <identifier> WHEN|ELSE` outside parentheses, or a closing
// `AS SEARCHED`. Neither sequence occurs in a statement a plan can be made
// for: a join's last relation is followed by its `ON`, so no such statement
// ends in an alias, and no production places two identifiers before a
// `WHEN` or an `ELSE` at depth 0.
//
// **`ELSE` is mandatory and last** (CX-R4): the variants are a rule list
// walked in written order, and a list with no `ELSE` would leave a
// statement with no variant.
//
// An `ORDER` list names the statement's relations by their **bindings** -
// the alias where one was written, the table's name otherwise - which this
// file keeps as written; resolving them against the statement is the
// caller's. `ACCESS` and `BUILD` clauses (CX-S3, CX-S5) are refused
// `NotImplemented` at their word.
//
// Every refusal carries the byte of the offending token in the command's
// own text.

namespace kds::exec {

struct PlanVariantSpec {
    std::string name;
    std::size_t name_at = 0;
    PlanRule rule;  // empty: `ELSE`
    std::vector<std::string> order;
    std::size_t order_at = 0;  // the `(` of the list
};

struct CreatePlanStmt {
    std::string name;
    std::size_t name_at = 0;
    // The `<select>`, as a range of the command's bytes.
    std::size_t statement_at = 0;
    std::size_t statement_size = 0;
    // `AS SEARCHED`: the search's order of this moment, as the plan's one
    // variant (CX-Q28 (a)). `variants` is then empty.
    bool searched = false;
    std::vector<PlanVariantSpec> variants;
};

struct AlterPlanStmt {
    enum class Action : std::uint8_t { kEnable, kDisable, kReplaceVariant };
    std::string name;
    std::size_t name_at = 0;
    Action action = Action::kEnable;
    PlanVariantSpec variant;  // kReplaceVariant
};

StatusOr<CreatePlanStmt> ParseCreatePlan(std::string_view line);
StatusOr<AlterPlanStmt> ParseAlterPlan(std::string_view line);

// A variant's `ORDER` list as written positions in executed order, against
// the statement's `bindings` in written order: every binding named once, by
// name, case-insensitively. `InvalidArgument` at the list's byte otherwise.
StatusOr<CapturedOrder> ResolvePlanOrder(const std::vector<std::string>& bindings,
                                         const PlanVariantSpec& variant);

}  // namespace kds::exec
