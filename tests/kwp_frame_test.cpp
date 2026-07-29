#include "kds/wire/kwp.hpp"

#include <gtest/gtest.h>

namespace kds::wire {
namespace {

std::vector<std::byte> Bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (int v : values) out.push_back(static_cast<std::byte>(v));
    return out;
}

std::vector<std::byte> ToVector(const std::array<std::byte, kFrameHeaderSize>& arr) {
    return std::vector<std::byte>(arr.begin(), arr.end());
}

TEST(FrameHeaderTest, EncodeDecodeRoundTrip) {
    FrameHeader header{};
    header.length = 4 + 10;
    header.type = 7;
    header.flags = 3;
    header.reserved = 0;

    auto encoded = header.Encode();
    auto decoded = FrameHeader::Decode(encoded);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().length, header.length);
    EXPECT_EQ(decoded.value().type, header.type);
    EXPECT_EQ(decoded.value().flags, header.flags);
    EXPECT_EQ(decoded.value().reserved, header.reserved);
}

TEST(FrameHeaderTest, EncodeIsLittleEndian) {
    FrameHeader header{};
    header.length = 0x04030201u;
    header.type = 0xAB;
    header.flags = 0xCD;
    header.reserved = 0xEF01;

    auto encoded = header.Encode();
    EXPECT_EQ(ToVector(encoded), Bytes({0x01, 0x02, 0x03, 0x04, 0xAB, 0xCD, 0x01, 0xEF}));
}

TEST(FrameHeaderTest, DecodeRejectsShortInput) {
    std::vector<std::byte> too_short(kFrameHeaderSize - 1, std::byte{0});
    auto decoded = FrameHeader::Decode(too_short);
    EXPECT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.status().code(), StatusCode::kInvalidArgument);
}

TEST(FrameHeaderTest, ReservedFieldRoundTrips) {
    // reserved must survive the round trip even though writers set it to
    // 0 and readers ignore it - the codec itself must not silently drop it.
    FrameHeader header{};
    header.length = 4;
    header.type = 1;
    header.flags = 0;
    header.reserved = 0x1234;

    auto decoded = FrameHeader::Decode(header.Encode());
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().reserved, 0x1234);
}

std::vector<std::byte> MakePayload(std::size_t size) {
    std::vector<std::byte> payload(size);
    for (std::size_t i = 0; i < size; ++i) {
        payload[i] = static_cast<std::byte>(i % 251);
    }
    return payload;
}

TEST(FrameDecoderTest, FeedWholeFrameAtOnce) {
    std::vector<std::byte> payload = MakePayload(37);
    std::vector<std::byte> frame_bytes =
        EncodeFrame(static_cast<std::uint8_t>(ClientFrameType::kPing), 0, payload);

    FrameDecoder decoder;
    Status s = decoder.Feed(frame_bytes);
    ASSERT_TRUE(s.ok());

    auto frame = decoder.PopFrame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->type, static_cast<std::uint8_t>(ClientFrameType::kPing));
    EXPECT_EQ(frame->flags, 0);
    EXPECT_EQ(frame->payload, payload);
    EXPECT_FALSE(decoder.PopFrame().has_value());  // only one frame was fed
}

TEST(FrameDecoderTest, FeedEmptyPayloadFrame) {
    std::vector<std::byte> frame_bytes =
        EncodeFrame(static_cast<std::uint8_t>(ClientFrameType::kSync), 0, {});

    FrameDecoder decoder;
    ASSERT_TRUE(decoder.Feed(frame_bytes).ok());
    auto frame = decoder.PopFrame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->payload.empty());
}

TEST(FrameDecoderTest, MultipleFramesInOneFeedComeOutFIFO) {
    std::vector<std::byte> p1 = MakePayload(5);
    std::vector<std::byte> p2 = MakePayload(9);
    std::vector<std::byte> combined =
        EncodeFrame(static_cast<std::uint8_t>(ClientFrameType::kParse), 1, p1);
    std::vector<std::byte> second =
        EncodeFrame(static_cast<std::uint8_t>(ClientFrameType::kBind), 2, p2);
    combined.insert(combined.end(), second.begin(), second.end());

    FrameDecoder decoder;
    ASSERT_TRUE(decoder.Feed(combined).ok());

    auto first = decoder.PopFrame();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->type, static_cast<std::uint8_t>(ClientFrameType::kParse));
    EXPECT_EQ(first->payload, p1);

    auto second_out = decoder.PopFrame();
    ASSERT_TRUE(second_out.has_value());
    EXPECT_EQ(second_out->type, static_cast<std::uint8_t>(ClientFrameType::kBind));
    EXPECT_EQ(second_out->payload, p2);

    EXPECT_FALSE(decoder.PopFrame().has_value());
}

TEST(FrameDecoderTest, ByteAtATimeFuzzMatchesWholeFeed) {
    std::vector<std::byte> payload = MakePayload(123);
    std::vector<std::byte> frame_bytes =
        EncodeFrame(static_cast<std::uint8_t>(ClientFrameType::kExecute), 5, payload);

    FrameDecoder decoder;
    for (std::byte b : frame_bytes) {
        Status s = decoder.Feed(std::span<const std::byte>(&b, 1));
        ASSERT_TRUE(s.ok());
    }

    auto frame = decoder.PopFrame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->type, static_cast<std::uint8_t>(ClientFrameType::kExecute));
    EXPECT_EQ(frame->flags, 5);
    EXPECT_EQ(frame->payload, payload);
}

TEST(FrameDecoderTest, TruncationAtEveryOffsetNeverProducesAFrameOrCrashes) {
    std::vector<std::byte> payload = MakePayload(20);
    std::vector<std::byte> frame_bytes =
        EncodeFrame(static_cast<std::uint8_t>(ClientFrameType::kDescribe), 0, payload);

    for (std::size_t cut = 0; cut < frame_bytes.size(); ++cut) {
        FrameDecoder decoder;
        std::span<const std::byte> truncated(frame_bytes.data(), cut);
        Status s = decoder.Feed(truncated);
        ASSERT_TRUE(s.ok()) << "cut=" << cut;
        EXPECT_FALSE(decoder.PopFrame().has_value()) << "cut=" << cut;
        EXPECT_FALSE(decoder.failed()) << "cut=" << cut;
    }
}

TEST(FrameDecoderTest, LengthExceedingMaxFrameFailsAndLatches) {
    FrameHeader header{};
    header.length = kMaxFrame + 1;
    header.type = static_cast<std::uint8_t>(ClientFrameType::kExecute);
    header.flags = 0;
    header.reserved = 0;
    auto header_bytes = header.Encode();

    FrameDecoder decoder;
    Status s = decoder.Feed(std::span<const std::byte>(header_bytes.data(), header_bytes.size()));
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kCorruption);
    EXPECT_TRUE(decoder.failed());

    // Once failed, further Feed() calls must keep failing without being
    // fooled by subsequent well-formed-looking bytes.
    std::vector<std::byte> more = MakePayload(8);
    Status s2 = decoder.Feed(more);
    EXPECT_FALSE(s2.ok());
    EXPECT_EQ(s2.code(), StatusCode::kCorruption);
    EXPECT_FALSE(decoder.PopFrame().has_value());
}

TEST(FrameDecoderTest, LengthBelowMinimumFails) {
    FrameHeader header{};
    header.length = kMinFrameLength - 1;
    header.type = 1;
    header.flags = 0;
    header.reserved = 0;
    auto header_bytes = header.Encode();

    FrameDecoder decoder;
    Status s = decoder.Feed(std::span<const std::byte>(header_bytes.data(), header_bytes.size()));
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kCorruption);
    EXPECT_TRUE(decoder.failed());
}

TEST(ErrorTaxonomyTest, MakeErrorCodePacksCategoryAndDetail) {
    std::uint32_t code = MakeErrorCode(ErrorCategory::kTxnConflict, 0x0007);
    EXPECT_EQ(code, (static_cast<std::uint32_t>(ErrorCategory::kTxnConflict) << 16) | 0x0007u);
}

}  // namespace
}  // namespace kds::wire
