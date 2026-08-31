#include "kds/server/step_descriptor.hpp"

#include <gtest/gtest.h>

// The STEP_OPEN descriptor codec (workplan P4a second half): a compiled
// step round-trips with full fidelity, the refused classes are refused by
// name, and corrupt bytes are errors rather than out-of-range enums.

namespace kds::server {
namespace {

exec::Step LookupStep() {
    exec::Step step;
    step.step_id = 3;
    step.rel_oid = 1017;
    step.rel_name = "accounts";
    step.kind = exec::AccessKind::kLookup;

    exec::Operand key;
    key.kind = exec::OperandKind::kLiteral;
    key.literal.type = parser::ValueType::kInt;
    key.literal.int_val = 42;
    key.literal.raw_int_text = "42";
    step.key = key;

    exec::StepPredicate str_pred;
    str_pred.lhs = exec::ColumnRef{0, 3, 2};
    str_pred.op = parser::CompareOp::kNeq;
    str_pred.rhs.kind = exec::OperandKind::kLiteral;
    str_pred.rhs.literal.type = parser::ValueType::kStr;
    str_pred.rhs.literal.str_val = "frozen";
    step.residual.push_back(str_pred);

    exec::StepPredicate dec_pred;
    dec_pred.lhs = exec::ColumnRef{0, 3, 4};
    dec_pred.op = parser::CompareOp::kLte;
    dec_pred.rhs.kind = exec::OperandKind::kLiteral;
    dec_pred.rhs.literal.type = parser::ValueType::kDecimal;
    dec_pred.rhs.literal.int_val = 12345;
    dec_pred.rhs.literal.scale = 2;
    step.residual.push_back(dec_pred);

    step.range = exec::RangeBounds{10, 99};
    step.access_columns = {0, 2, 4};
    step.filter_columns = 0b10100;
    step.read_columns = 0b10101;
    return step;
}

TEST(StepDescriptorTest, ALookupStepRoundTripsWithFullFidelity) {
    const exec::Step step = LookupStep();
    auto bytes = EncodeStepDescriptor(step);
    ASSERT_TRUE(bytes.ok()) << bytes.status().message();

    auto back = DecodeStepDescriptor(bytes.value());
    ASSERT_TRUE(back.ok()) << back.status().message();
    const exec::Step& d = back.value();

    EXPECT_EQ(d.step_id, step.step_id);
    EXPECT_EQ(d.rel_oid, step.rel_oid);
    EXPECT_EQ(d.rel_name, step.rel_name);
    EXPECT_EQ(d.kind, step.kind);
    ASSERT_TRUE(d.key.has_value());
    EXPECT_EQ(d.key->literal.int_val, 42);
    EXPECT_EQ(d.key->literal.raw_int_text, "42");
    ASSERT_TRUE(d.range.has_value());
    EXPECT_EQ(d.range->low, 10u);
    EXPECT_EQ(d.range->high, 99u);
    ASSERT_EQ(d.residual.size(), 2u);
    EXPECT_EQ(d.residual[0].rhs.literal.str_val, "frozen");
    EXPECT_EQ(d.residual[0].op, parser::CompareOp::kNeq);
    EXPECT_EQ(d.residual[1].rhs.literal.scale, 2);
    EXPECT_EQ(d.residual[1].rhs.literal.int_val, 12345);
    EXPECT_EQ(d.residual[1].lhs.col_pos, 4);
    EXPECT_EQ(d.access_columns, step.access_columns);
    EXPECT_EQ(d.filter_columns, step.filter_columns);
    EXPECT_EQ(d.read_columns, step.read_columns);
}

TEST(StepDescriptorTest, AProbeKeyedByAColumnRoundTrips) {
    exec::Step step;
    step.kind = exec::AccessKind::kProbe;
    step.rel_oid = 1018;
    exec::Operand key;
    key.kind = exec::OperandKind::kColumn;
    key.column = exec::ColumnRef{1, 0, 5};
    step.key = key;

    auto bytes = EncodeStepDescriptor(step);
    ASSERT_TRUE(bytes.ok());
    auto back = DecodeStepDescriptor(bytes.value());
    ASSERT_TRUE(back.ok());
    ASSERT_TRUE(back.value().key.has_value());
    EXPECT_EQ(back.value().key->kind, exec::OperandKind::kColumn);
    EXPECT_EQ(back.value().key->column.up, 1);
    EXPECT_EQ(back.value().key->column.col_pos, 5);
}

TEST(StepDescriptorTest, TheRefusedClassesAreRefusedByName) {
    // A subquery-bearing step is P4d's work, said so.
    exec::Step with_sub = LookupStep();
    with_sub.sub_chains.emplace_back();
    auto sub = EncodeStepDescriptor(with_sub);
    ASSERT_FALSE(sub.ok());
    EXPECT_EQ(sub.status().code(), StatusCode::kUnsupported);

    // Core-local structure state cannot ship.
    exec::Step index_step;
    index_step.kind = exec::AccessKind::kIndexProbe;
    auto ix = EncodeStepDescriptor(index_step);
    ASSERT_FALSE(ix.ok());
    EXPECT_EQ(ix.status().code(), StatusCode::kUnsupported);

    // A $param never executes, so it never ships.
    exec::Step param_step = LookupStep();
    param_step.key->literal.type = parser::ValueType::kParam;
    auto param = EncodeStepDescriptor(param_step);
    ASSERT_FALSE(param.ok());
    EXPECT_EQ(param.status().code(), StatusCode::kUnsupported);
}

// `IS [NOT] NULL` is a CompareOp like any other (docs/spec/null.md NU5),
// so it ships: a peer-owned relation's `WHERE col IS NULL` is evaluated
// remotely, not refused by a bound written against the old last enumerator.
TEST(StepDescriptorTest, TheIsNullOpsShipLikeEveryOtherComparison) {
    exec::Step step;
    step.kind = exec::AccessKind::kScan;
    step.rel_oid = 1019;
    for (const parser::CompareOp op :
         {parser::CompareOp::kIsNull, parser::CompareOp::kIsNotNull}) {
        exec::StepPredicate pred;
        pred.lhs = exec::ColumnRef{0, 0, 2};
        pred.op = op;
        pred.rhs.kind = exec::OperandKind::kLiteral;  // the kNull filler
        step.residual.push_back(pred);
    }

    auto bytes = EncodeStepDescriptor(step);
    ASSERT_TRUE(bytes.ok()) << bytes.status().message();
    auto back = DecodeStepDescriptor(bytes.value());
    ASSERT_TRUE(back.ok()) << back.status().message();
    ASSERT_EQ(back.value().residual.size(), 2u);
    EXPECT_EQ(back.value().residual[0].op, parser::CompareOp::kIsNull);
    EXPECT_EQ(back.value().residual[1].op, parser::CompareOp::kIsNotNull);
    EXPECT_EQ(back.value().residual[0].rhs.literal.type, parser::ValueType::kNull);

    // One past the last enumerator is still a refusal, not a cast.
    auto bad = bytes.value();
    for (std::byte& b : bad) {
        if (b == std::byte{static_cast<std::uint8_t>(parser::CompareOp::kIsNull)}) {
            b = std::byte{static_cast<std::uint8_t>(parser::CompareOp::kIsNotNull) + 1};
            break;
        }
    }
    EXPECT_FALSE(DecodeStepDescriptor(bad).ok());
}

TEST(StepDescriptorTest, CorruptBytesAreErrorsNeverEnums) {
    auto bytes = EncodeStepDescriptor(LookupStep());
    ASSERT_TRUE(bytes.ok());

    // Truncation at every prefix length: always a clean refusal.
    for (std::size_t len = 0; len < bytes.value().size(); ++len) {
        auto cut = DecodeStepDescriptor(
            std::span<const std::byte>(bytes.value()).first(len));
        ASSERT_FALSE(cut.ok()) << "prefix of " << len << " bytes decoded";
        EXPECT_EQ(cut.status().code(), StatusCode::kInvalidArgument);
    }

    // A wrong version is named, not guessed at.
    auto wrong = bytes.value();
    wrong[0] = std::byte{99};
    EXPECT_FALSE(DecodeStepDescriptor(wrong).ok());

    // Trailing bytes are a refusal too: a descriptor is exactly itself.
    auto padded = bytes.value();
    padded.push_back(std::byte{0});
    auto trail = DecodeStepDescriptor(padded);
    ASSERT_FALSE(trail.ok());
    EXPECT_EQ(trail.status().code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace kds::server
