#include "kds/wal/record.hpp"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace kds::wal {
namespace {

std::vector<std::byte> BytesOf(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

std::string StringOf(std::span<const std::byte> b) {
    std::string out(b.size(), '\0');
    std::memcpy(out.data(), b.data(), b.size());
    return out;
}

// One stream's worth of records, laid out exactly as a segment body would
// be, so the reader tests walk real bytes rather than a mock.
struct Stream {
    std::vector<std::byte> bytes;
    Lsn base_lsn = kSegmentHeaderSize;

    Lsn Append(const RecordSpec& spec, std::span<const std::byte> payload) {
        const Lsn lsn = base_lsn + bytes.size();
        const std::size_t at = bytes.size();
        bytes.resize(at + EncodedRecordSize(payload.size()));
        auto written = EncodeRecord(std::span<std::byte>(bytes).subspan(at), spec, lsn, payload);
        EXPECT_TRUE(written.ok()) << written.status().message();
        return lsn;
    }
};

TEST(WalRecordTest, RoundTripsHeaderAndPayload) {
    std::vector<std::byte> buf(EncodedRecordSize(5));
    const RecordSpec spec{RecordType::kHeapInsert, /*txn_id=*/42, /*page_id=*/128, /*flags=*/0x7};

    auto written = EncodeRecord(buf, spec, /*lsn=*/4096, BytesOf("hello"));
    ASSERT_TRUE(written.ok()) << written.status().message();
    EXPECT_EQ(written.value(), 40u);  // 32 header + 5 payload, padded to 8

    auto decoded = DecodeRecord(buf);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().type(), RecordType::kHeapInsert);
    EXPECT_EQ(decoded.value().header.txn_id, 42u);
    EXPECT_EQ(decoded.value().header.page_id, 128u);
    EXPECT_EQ(decoded.value().header.flags, 0x7);
    EXPECT_EQ(decoded.value().header.lsn, 4096u);
    EXPECT_EQ(decoded.value().header.reserved, 0u);
    EXPECT_EQ(StringOf(decoded.value().payload.subspan(0, 5)), "hello");
}

TEST(WalRecordTest, EveryRecordIsEightByteAlignedAndPaddingIsZeroed) {
    for (std::size_t payload_size = 0; payload_size < 24; ++payload_size) {
        const std::size_t size = EncodedRecordSize(payload_size);
        EXPECT_EQ(size % kRecordAlignment, 0u) << "payload " << payload_size;
        EXPECT_GE(size, kRecordHeaderSize + payload_size);
        EXPECT_LT(size, kRecordHeaderSize + payload_size + kRecordAlignment);

        std::vector<std::byte> buf(size, std::byte{0xEE});
        const std::vector<std::byte> payload(payload_size, std::byte{0x5A});
        ASSERT_TRUE(EncodeRecord(buf, {RecordType::kPad}, 4096, payload).ok());

        for (std::size_t i = kRecordHeaderSize + payload_size; i < size; ++i) {
            EXPECT_EQ(buf[i], std::byte{0}) << "padding byte " << i;
        }
    }
}

TEST(WalRecordTest, RejectsUnassignedTypeAndOversizedTxnId) {
    std::vector<std::byte> buf(kRecordHeaderSize);

    EXPECT_EQ(EncodeRecord(buf, {RecordType::kInvalid}, 4096, {}).status().code(),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(EncodeRecord(buf, {static_cast<RecordType>(kMaxAssignedRecordType + 1)}, 4096, {})
                  .status()
                  .code(),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(EncodeRecord(buf, {RecordType::kTxnCommit, kMaxTxnId + 1}, 4096, {}).status().code(),
              StatusCode::kInvalidArgument);
    EXPECT_TRUE(EncodeRecord(buf, {RecordType::kTxnCommit, kMaxTxnId}, 4096, {}).ok());

    std::vector<std::byte> too_small(kRecordHeaderSize - 1);
    EXPECT_EQ(EncodeRecord(too_small, {RecordType::kTxnCommit}, 4096, {}).status().code(),
              StatusCode::kInvalidArgument);
}

// The whole point of the CRC: any single flipped bit anywhere the CRC
// covers is caught. Recovery reads that as the end of the stream.
TEST(WalRecordTest, AnyFlippedBitFailsTheCrc) {
    std::vector<std::byte> buf(EncodedRecordSize(16));
    ASSERT_TRUE(EncodeRecord(buf, {RecordType::kHeapOverwrite, 9, 5},
                             4096, std::vector<std::byte>(16, std::byte{0x33}))
                    .ok());
    ASSERT_TRUE(DecodeRecord(buf).ok());

    for (std::size_t i = kRecordCrcCoverageOffset; i < buf.size(); ++i) {
        std::vector<std::byte> corrupted = buf;
        corrupted[i] ^= std::byte{0x01};
        EXPECT_EQ(DecodeRecord(corrupted).status().code(), StatusCode::kCorruption)
            << "flipped byte " << i;
    }
}

TEST(WalRecordTest, ImpossibleLengthIsCorruption) {
    std::vector<std::byte> buf(EncodedRecordSize(8));
    ASSERT_TRUE(EncodeRecord(buf, {RecordType::kTxnBegin, 1}, 4096, BytesOf("abcdefgh")).ok());

    const auto with_total_len = [&](std::uint32_t value) {
        std::vector<std::byte> copy = buf;
        std::memcpy(copy.data() + kRecordTotalLenOffset, &value, sizeof(value));
        return DecodeRecord(copy).status().code();
    };

    EXPECT_EQ(with_total_len(kRecordHeaderSize - 8), StatusCode::kCorruption);  // too small
    EXPECT_EQ(with_total_len(41), StatusCode::kCorruption);                     // unaligned
    EXPECT_EQ(with_total_len(4096), StatusCode::kCorruption);                   // past the buffer
    EXPECT_EQ(DecodeRecord(std::span<const std::byte>(buf).subspan(0, 8)).status().code(),
              StatusCode::kCorruption);  // fewer bytes than a header
}

TEST(WalRecordTest, ReaderWalksEveryRecordInOrder) {
    Stream stream;
    const Lsn first = stream.Append({RecordType::kTxnBegin, 7}, {});
    const Lsn second = stream.Append({RecordType::kHeapInsert, 7, 128}, BytesOf("row-bytes"));
    const Lsn third = stream.Append({RecordType::kTxnCommit, 7}, {});

    RecordReader reader(stream.bytes, stream.base_lsn);

    auto r1 = reader.Next();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->header.lsn, first);
    EXPECT_EQ(r1->type(), RecordType::kTxnBegin);

    auto r2 = reader.Next();
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->header.lsn, second);
    EXPECT_EQ(r2->header.page_id, 128u);
    EXPECT_EQ(StringOf(r2->payload.subspan(0, 9)), "row-bytes");

    auto r3 = reader.Next();
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(r3->header.lsn, third);

    EXPECT_FALSE(reader.Next().has_value());
    EXPECT_FALSE(reader.stopped_early());
    EXPECT_EQ(reader.end_lsn(), stream.base_lsn + stream.bytes.size());
}

// wal.md section 16-1: truncate at every byte boundary of the last record
// and recovery must still find the durable end - never a phantom record,
// never a crash.
TEST(WalRecordTest, TornTailStopsAtTheLastWholeRecord) {
    Stream stream;
    stream.Append({RecordType::kTxnBegin, 3}, {});
    const Lsn torn_at = stream.Append({RecordType::kHeapInsert, 3, 64}, BytesOf("second record"));
    const std::size_t whole_prefix = torn_at - stream.base_lsn;

    for (std::size_t cut = 1; cut < stream.bytes.size() - whole_prefix; ++cut) {
        std::span<const std::byte> truncated(stream.bytes.data(), whole_prefix + cut);
        RecordReader reader(truncated, stream.base_lsn);

        std::size_t seen = 0;
        while (reader.Next().has_value()) ++seen;

        EXPECT_EQ(seen, 1u) << "truncated at +" << cut;
        EXPECT_EQ(reader.end_lsn(), torn_at) << "truncated at +" << cut;
        EXPECT_TRUE(reader.stopped_early()) << "truncated at +" << cut;
    }
}

// Garbage left over from a recycled segment must not read as a record just
// because its bytes happen to decode.
TEST(WalRecordTest, RecordWhoseLsnDisagreesWithItsPositionEndsTheStream) {
    Stream stream;
    stream.Append({RecordType::kTxnBegin, 1}, {});

    const std::size_t at = stream.bytes.size();
    stream.bytes.resize(at + EncodedRecordSize(0));
    ASSERT_TRUE(EncodeRecord(std::span<std::byte>(stream.bytes).subspan(at), {RecordType::kTxnCommit, 1},
                             /*lsn=*/999999, {})
                    .ok());

    RecordReader reader(stream.bytes, stream.base_lsn);
    EXPECT_TRUE(reader.Next().has_value());
    EXPECT_FALSE(reader.Next().has_value());
    EXPECT_TRUE(reader.stopped_early());
    EXPECT_EQ(reader.end_lsn(), stream.base_lsn + at);
}

TEST(WalSegmentTest, HeaderRoundTripsAndRejectsDamage) {
    std::vector<std::byte> block(kSegmentHeaderSize, std::byte{0xCC});
    SegmentHeaderFields fields{};
    fields.format_version = kSegmentFormatVersion;
    fields.core_id = 3;
    fields.segment_no = 17;
    fields.start_lsn = 64 * 1024;
    ASSERT_TRUE(EncodeSegmentHeader(block, fields).ok());

    auto decoded = DecodeSegmentHeader(block);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().magic, kSegmentMagic);
    EXPECT_EQ(decoded.value().format_version, kSegmentFormatVersion);
    EXPECT_EQ(decoded.value().core_id, 3u);
    EXPECT_EQ(decoded.value().segment_no, 17u);
    EXPECT_EQ(decoded.value().start_lsn, 64u * 1024);

    // The block is zeroed beyond the fields, so a stale segment's leftovers
    // never leak into a fresh header.
    for (std::size_t i = kSegmentUsedSize; i < kSegmentHeaderSize; ++i) {
        ASSERT_EQ(block[i], std::byte{0}) << "byte " << i;
    }

    std::vector<std::byte> flipped = block;
    flipped[kSegmentNoOffset] ^= std::byte{0x01};
    EXPECT_EQ(DecodeSegmentHeader(flipped).status().code(), StatusCode::kCorruption);

    std::vector<std::byte> zeroed(kSegmentHeaderSize, std::byte{0});
    EXPECT_EQ(DecodeSegmentHeader(zeroed).status().code(), StatusCode::kCorruption);
}

TEST(WalSegmentTest, NewerFormatVersionIsRefusedNotMisparsed) {
    std::vector<std::byte> block(kSegmentHeaderSize);
    SegmentHeaderFields fields{};
    fields.format_version = kSegmentFormatVersion + 1;
    ASSERT_TRUE(EncodeSegmentHeader(block, fields).ok());

    auto decoded = DecodeSegmentHeader(block);
    EXPECT_EQ(decoded.status().code(), StatusCode::kCorruption);
    EXPECT_NE(decoded.status().message().find("format_version"), std::string::npos);
}

// The other half of the bump, and the half a version field does not give for
// free: `DecodeSegmentHeader` refuses only what is *newer*, so a stream written
// before `AssertEntryPayload` grew `group_id` would otherwise be accepted and
// its ASSERT_* records decoded four bytes out of place.
TEST(WalSegmentTest, AStreamOlderThanTheRecordLayoutIsRefusedNotMisparsed) {
    ASSERT_GT(kMinReadableSegmentFormatVersion, 1u)
        << "this build claims to read v1 streams, whose ASSERT_* offsets moved";

    std::vector<std::byte> block(kSegmentHeaderSize);
    SegmentHeaderFields fields{};
    fields.format_version = kMinReadableSegmentFormatVersion - 1;
    ASSERT_TRUE(EncodeSegmentHeader(block, fields).ok());

    auto decoded = DecodeSegmentHeader(block);
    EXPECT_EQ(decoded.status().code(), StatusCode::kCorruption);
    // Names both versions: there is no migration, so the operator's next step
    // is to discard the stream and the message has to be enough to decide that.
    EXPECT_NE(decoded.status().message().find("predates"), std::string::npos)
        << decoded.status().message();
    EXPECT_NE(decoded.status().message().find(std::to_string(kMinReadableSegmentFormatVersion)),
              std::string::npos)
        << decoded.status().message();
}

TEST(WalRecordTest, TypeNamesCoverEveryAssignedValue) {
    for (std::uint8_t raw = 1; raw <= kMaxAssignedRecordType; ++raw) {
        EXPECT_TRUE(IsAssignedRecordType(raw));
        EXPECT_STRNE(RecordTypeName(static_cast<RecordType>(raw)), "UNKNOWN") << "type " << +raw;
    }
    EXPECT_FALSE(IsAssignedRecordType(0));
    EXPECT_FALSE(IsAssignedRecordType(kMaxAssignedRecordType + 1));
}

TEST(WalRecordTest, EveryNamedTypeIsWritable) {
    // **The converse of the test above, and the one that was missing.**
    //
    // That test walks up to `kMaxAssignedRecordType`, so a type appended *past*
    // a stale bound is invisible to it - which is how `kHeapDeleteUnmark = 23`
    // shipped unwritable at RC05 with the constant left at 22: `EncodeRecord`
    // answered "unassigned record type" for it, so rolling back a DELETE could
    // not be logged, and every test of that path runs unlogged.
    //
    // A named type is a type some site intends to write. Walking well past the
    // bound is the point: the failure mode is a name that exists above it.
    for (std::uint8_t raw = 1; raw < 64; ++raw) {
        const auto type = static_cast<RecordType>(raw);
        if (std::string_view(RecordTypeName(type)) == "UNKNOWN") continue;

        EXPECT_TRUE(IsAssignedRecordType(raw))
            << RecordTypeName(type) << " (" << +raw
            << ") has a name but is above kMaxAssignedRecordType, so EncodeRecord refuses it";

        std::array<std::byte, 128> buf{};
        EXPECT_TRUE(EncodeRecord(buf, {type, /*txn_id=*/7, /*page_id=*/100}, /*lsn=*/4096, {}).ok())
            << RecordTypeName(type) << " cannot be encoded";
    }
}

}  // namespace
}  // namespace kds::wal
