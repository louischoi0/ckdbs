#include "kds/server/step_pipeline.hpp"

#include <gtest/gtest.h>

// The pipeline data plane (workplan P4a): payload round-trips, the credit
// invariants, and the batch builder's chunking - the pure layer everything
// in crosscore.md §3-§4 will ride on, tested before either consumer
// exists, wire/row_codec.hpp's arrangement.

namespace kds::server {
namespace {

constexpr PipelineTag kTag{/*request_id=*/42, /*session_core=*/0, /*step_id=*/2};

TEST(StepPipelineTest, PayloadsRoundTripAndRefuseShortBytes) {
    std::vector<std::byte> bytes;

    StepCreditPayload credit{kTag, 3};
    EncodePipelinePayload(credit, bytes);
    auto credit_back = DecodePipelinePayload<StepCreditPayload>(bytes);
    ASSERT_TRUE(credit_back.ok());
    EXPECT_EQ(credit_back.value().tag, kTag);
    EXPECT_EQ(credit_back.value().credits, 3u);

    StepErrorPayload error{kTag, /*status_code=*/7, /*retryable=*/1};
    EncodePipelinePayload(error, bytes);
    auto error_back = DecodePipelinePayload<StepErrorPayload>(bytes);
    ASSERT_TRUE(error_back.ok());
    EXPECT_EQ(error_back.value().status_code, 7u);
    EXPECT_EQ(error_back.value().retryable, 1);

    // Short bytes are refused, never read past: the malformed-message rule.
    auto short_decode =
        DecodePipelinePayload<StepCreditPayload>(std::span<const std::byte>(bytes).first(4));
    ASSERT_FALSE(short_decode.ok());
    EXPECT_EQ(short_decode.status().code(), StatusCode::kInvalidArgument);
}

TEST(StepPipelineTest, CreditsBoundSendsAndGrantsBoundAtTheCeiling) {
    EdgeCredit edge(/*initial=*/2);

    // The invariant: never send without a credit.
    EXPECT_TRUE(edge.ConsumeOnSend().ok());
    EXPECT_TRUE(edge.ConsumeOnSend().ok());
    EXPECT_FALSE(edge.can_send());
    EXPECT_EQ(edge.ConsumeOnSend().code(), StatusCode::kResourceExhausted);

    // A drain grants back - and never past the preallocated ceiling,
    // because the downstream's buffer memory was sized at STEP_OPEN.
    EXPECT_TRUE(edge.Grant(1).ok());
    EXPECT_TRUE(edge.can_send());
    EXPECT_EQ(edge.Grant(2).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(edge.available(), 1u);
}

TEST(StepPipelineTest, TheBuilderChunksAtTheTargetAndSequencesBatches) {
    StepBatchBuilder builder(kTag, /*target_bytes=*/64);

    // Small rows accumulate below the target...
    std::vector<std::byte> row(24, std::byte{0xAB});
    EXPECT_FALSE(builder.Append(row));
    EXPECT_FALSE(builder.Append(row));
    // ...and the append that crosses it says "take me".
    EXPECT_TRUE(builder.Append(row));

    auto first = builder.Take();
    std::span<const std::byte> rows;
    auto header = DecodeStepBatchHeader(first, rows);
    ASSERT_TRUE(header.ok());
    EXPECT_EQ(header.value().tag, kTag);
    EXPECT_EQ(header.value().seq, 0u);
    EXPECT_EQ(header.value().row_count, 3u);
    EXPECT_EQ(rows.size(), 3 * row.size());
    EXPECT_TRUE(builder.empty());

    // A single row wider than the target ships alone rather than sticking.
    std::vector<std::byte> wide(200, std::byte{0xCD});
    EXPECT_TRUE(builder.Append(wide));
    auto second = builder.Take();
    auto second_header = DecodeStepBatchHeader(second, rows);
    ASSERT_TRUE(second_header.ok());
    EXPECT_EQ(second_header.value().seq, 1u);
    EXPECT_EQ(second_header.value().row_count, 1u);
    EXPECT_EQ(rows.size(), wide.size());
}

}  // namespace
}  // namespace kds::server
