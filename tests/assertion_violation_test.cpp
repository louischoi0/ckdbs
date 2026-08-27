#include "kds/exec/assertion_violation.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/catalog/well_known.hpp"
#include "kds/parser/ast.hpp"
#include "kds/server/command_dispatcher.hpp"

// The assertion violation surface (docs/spec/assertion.md §4.4, AS9,
// workplan AST08): the message format, its type-correct group-key rendering,
// the enforced-ceiling rule, and the wire spelling.
//
// These are golden tests on purpose. The message and the `ERR` line are
// compatibility surfaces - a client parses them, a runbook quotes them - so
// the assertions here compare whole strings, not fragments. Nothing produces
// this Status yet (AST07 wires the check into the write paths); pinning the
// surface first is what lets AST07 land as behaviour with no new spelling
// decisions inside it.

namespace kds::exec {
namespace {

parser::AstValue Int(std::int64_t v) {
    parser::AstValue value;
    value.type = parser::ValueType::kInt;
    value.int_val = v;
    return value;
}

parser::AstValue Str(std::string v) {
    parser::AstValue value;
    value.type = parser::ValueType::kStr;
    value.str_val = std::move(v);
    return value;
}

TEST(AssertionViolationMessage, RendersTheSpecExampleExactly) {
    // §4.4's own example, byte for byte.
    const std::vector<GroupKeyPart> group = {
        {"user_id", catalog::kTypeValInt64, Int(41)},
        {"product_id", catalog::kTypeValInt64, Int(7)},
    };
    EXPECT_EQ(AssertionViolationMessage("user_product_purchase_limit", group,
                                        BoundAggregate::kCount, "", 5),
              "assertion \"user_product_purchase_limit\" group (user_id=41, product_id=7): "
              "COUNT(*) would exceed bound 5");
}

TEST(AssertionViolationMessage, SumSpellsItsColumnAndVarcharKeysRenderAsText) {
    // The workplan's "type-correct for int and varchar group columns", as one
    // message: a varchar key renders as its text, and a SUM aggregate names
    // the column the declaration summed.
    const std::vector<GroupKeyPart> group = {
        {"region", catalog::kTypeValVarchar, Str("emea")},
        {"desk", catalog::kTypeValInt64, Int(3)},
    };
    EXPECT_EQ(AssertionViolationMessage("regional_exposure_cap", group, BoundAggregate::kSum,
                                        "notional", 1000000),
              "assertion \"regional_exposure_cap\" group (region=emea, desk=3): "
              "SUM(notional) would exceed bound 1000000");
}

TEST(AssertionViolationMessage, ADateGroupKeyRendersAsADateNotAnEpochDay) {
    // The reason the two-argument FormatValue is mandatory here
    // (docs/spec/types.md §3.3): a DATE decodes to the integer it is, and an
    // error message showing `trade_date=0` would be a number nobody declared.
    const std::vector<GroupKeyPart> group = {
        {"trade_date", catalog::kTypeValDate, Int(0)},
    };
    EXPECT_EQ(AssertionViolationMessage("daily_trade_limit", group, BoundAggregate::kCount, "",
                                        10000),
              "assertion \"daily_trade_limit\" group (trade_date=1970-01-01): "
              "COUNT(*) would exceed bound 10000");
}

TEST(AssertionViolationMessage, TheBoundIsTheEnforcedCeilingNotTheDeclaredLiteral) {
    // A `COUNT(*) < 5` declaration fires when the count would reach 5, and
    // "would exceed bound 5" is then literally false. The message carries
    // `AssertionStmt::enforced_max()` - 4 here - which is AS11's
    // truthfulness rule applied to the error surface, and the ceiling is
    // taken from the parser's one computation of it rather than re-derived.
    parser::AssertionStmt stmt;
    stmt.op = parser::CompareOp::kLt;
    stmt.bound = 5;
    const std::vector<GroupKeyPart> group = {{"account", catalog::kTypeValInt64, Int(9)}};
    EXPECT_EQ(AssertionViolationMessage("order_burst_cap", group, BoundAggregate::kCount, "",
                                        stmt.enforced_max()),
              "assertion \"order_burst_cap\" group (account=9): "
              "COUNT(*) would exceed bound 4");
}

TEST(AssertionViolationWire, ErrLineSpellsTheTokenAndRetryableZero) {
    // The newline-protocol spelling, FK_VIOLATION's shape (docs/spec/protocol.md
    // §11 makes the token and the retryable bit compatibility surfaces).
    // The KWP error-frame mapping is deliberately absent: KWP has no caller,
    // and its ErrorCategory gains assertion's entry with the error registry
    // (docs/inflight/in-progress/protocol-wp.md P12) beside FK's, which is also still to land.
    const Status s = Status::AssertionViolation(
        "assertion \"limit\" group (id=1): COUNT(*) would exceed bound 5");
    EXPECT_EQ(server::ErrorReply(s),
              "ERR ASSERTION_VIOLATION retryable=0 "
              "assertion \"limit\" group (id=1): COUNT(*) would exceed bound 5");
}

}  // namespace
}  // namespace kds::exec
