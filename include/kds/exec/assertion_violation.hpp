#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "kds/exec/bound_cabin.hpp"
#include "kds/parser/ast.hpp"

// The assertion violation message (docs/spec/assertion.md §4.4, AS9,
// workplan AST08): the one place its format lives, so the wire, the tests
// and every future check site cannot drift on what a refusal says.
//
//   assertion "<name>" group (<col>=<val>, ...): <AGG> would exceed bound <N>
//
// ---- The bound in the message is the enforced ceiling ---------------------
//
// `<N>` is `AssertionStmt::enforced_max()`, not the declared literal - for a
// `<= N` declaration they are the same number, for `< N` the message says
// `N - 1`. That is AS11's truthfulness rule applied to the error surface: a
// refusal of `COUNT(*) < 5` fires when the count would reach 5, and "would
// exceed bound 5" is then literally false, while "would exceed bound 4" is
// exactly what the engine tested. The declared spelling is one SHOW
// ASSERTIONS away via the name this message carries; a message that
// re-derived the declared form from the ceiling would be the second
// derivation `enforced_max()` exists to forbid.
//
// ---- Rendering ------------------------------------------------------------
//
// Group-key values render through `FormatValue(type_val, value)` - the
// two-argument form, deliberately, because a group column may be a DATE and
// an epoch-day count in an error message is a number nobody declared
// (docs/spec/types.md §3.3; the single-argument form is deleted for exactly
// this caller's sake).
//
// ---- What this file is not ------------------------------------------------
//
// Not the check (AST07 compiles that into the write paths), not the Status
// plumbing (`Status::AssertionViolation` is the factory, base/status.hpp),
// and not the wire spelling (`server::ErrorReply` owns
// `ERR ASSERTION_VIOLATION retryable=0`). This is the sentence between them.

namespace kds::exec {

// One group column as the message renders it: the declared column name, the
// column's catalog type (`FormatValue`'s first argument), and the offending
// row's value for it.
struct GroupKeyPart {
    std::string_view column;
    std::uint32_t type_val = 0;
    parser::AstValue value;
};

// §4.4's message, exactly. `sum_column` is read only for a SUM assertion -
// a COUNT(*) one spells its aggregate with no column to name.
std::string AssertionViolationMessage(std::string_view assertion_name,
                                      std::span<const GroupKeyPart> group,
                                      BoundAggregate aggregate, std::string_view sum_column,
                                      std::int64_t enforced_max);

}  // namespace kds::exec
