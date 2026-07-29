#include "kds/wal/payload.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/keystone.hpp"
#include "kds/wal/record.hpp"

// Per-type payload codecs. Two things every case here is really about:
// a round-trip that loses nothing, and a decoder that refuses bytes it
// cannot replay rather than replaying them wrong.

namespace kds::wal {
namespace {

std::vector<std::byte> Pattern(std::size_t n, std::uint8_t seed) {
    std::vector<std::byte> bytes(n);
    for (std::size_t i = 0; i < n; ++i) {
        bytes[i] = static_cast<std::byte>((i + seed * 11u) & 0xFF);
    }
    return bytes;
}

// Encodes `payload` into a full record and decodes it back, so the payload
// codecs are exercised through the envelope they actually travel in -
// including the up-to-7 padding bytes DecodeRecord hands back with them.
template <typename EncodeFn>
std::vector<std::byte> ThroughEnvelope(RecordType type, EncodeFn encode) {
    std::array<std::byte, kPageSize + 512> payload_buffer{};
    auto payload_size = encode(std::span<std::byte>(payload_buffer));
    EXPECT_TRUE(payload_size.ok()) << payload_size.status().message();
    if (!payload_size.ok()) {
        return {};
    }

    std::vector<std::byte> record(EncodedRecordSize(payload_size.value()));
    const RecordSpec spec{type, 42, 7, 0};
    auto written = EncodeRecord(record, spec, kSegmentHeaderSize,
                                std::span(payload_buffer).first(payload_size.value()));
    EXPECT_TRUE(written.ok()) << written.status().message();
    return record;
}

std::span<const std::byte> PayloadOf(const std::vector<std::byte>& record) {
    auto decoded = DecodeRecord(record);
    EXPECT_TRUE(decoded.ok()) << decoded.status().message();
    return decoded.ok() ? decoded.value().payload : std::span<const std::byte>{};
}

// ---- PAGE_INIT -----------------------------------------------------------

TEST(WalPayloadTest, PageInitRoundTripsThroughTheEnvelope) {
    PageInitPayload fields{};
    fields.min_key = 1234567;
    fields.page_type = static_cast<std::uint8_t>(PageType::kHeap);

    const auto record = ThroughEnvelope(RecordType::kPageInit, [&](std::span<std::byte> out) {
        return EncodePageInit(out, fields);
    });
    auto decoded = DecodePageInit(PayloadOf(record));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().min_key, fields.min_key);
    EXPECT_EQ(decoded.value().page_type, fields.page_type);
}

TEST(WalPayloadTest, PageInitRejectsAnIdBeyondFortyBits) {
    PageInitPayload fields{};
    fields.min_key = kMaxKeystoneId + 1;
    fields.page_type = static_cast<std::uint8_t>(PageType::kHeap);

    std::array<std::byte, 32> out{};
    EXPECT_EQ(EncodePageInit(out, fields).status().code(), StatusCode::kInvalidArgument);
}

TEST(WalPayloadTest, PageInitRejectsAnUnknownPageType) {
    PageInitPayload fields{};
    fields.page_type = static_cast<std::uint8_t>(PageType::kHeap);
    std::array<std::byte, 32> out{};
    ASSERT_TRUE(EncodePageInit(out, fields).ok());

    // Written by a newer build, or garbage: replaying it would format the
    // page as something this build does not understand.
    out[kPageInitPageTypeOffset] = static_cast<std::byte>(kMaxAssignedPageType + 1);
    EXPECT_EQ(DecodePageInit(out).status().code(), StatusCode::kCorruption);

    out[kPageInitPageTypeOffset] = static_cast<std::byte>(PageType::kInvalid);
    EXPECT_EQ(DecodePageInit(out).status().code(), StatusCode::kCorruption);
}

// ---- HEAP_INSERT / HEAP_OVERWRITE ---------------------------------------

TEST(WalPayloadTest, HeapWriteRoundTripsTupleBytesExactly) {
    HeapWritePayload fields{};
    fields.trx_id = 0xFFFFFFFFFFFFull;  // the widest legal 48-bit writer
    fields.undo_ptr = 0x1122334455667788ull;
    fields.slot = 9;
    const std::vector<std::byte> tuple = Pattern(37, 3);  // deliberately not 8-aligned

    const auto record = ThroughEnvelope(RecordType::kHeapInsert, [&](std::span<std::byte> out) {
        return EncodeHeapWrite(out, fields, tuple);
    });
    auto decoded = DecodeHeapWrite(PayloadOf(record));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().fields.trx_id, fields.trx_id);
    EXPECT_EQ(decoded.value().fields.undo_ptr, fields.undo_ptr);
    EXPECT_EQ(decoded.value().fields.slot, fields.slot);
    // The whole point of the explicit length: the envelope's padding must
    // not come back as part of the tuple.
    ASSERT_EQ(decoded.value().tuple.size(), tuple.size());
    EXPECT_TRUE(std::equal(tuple.begin(), tuple.end(), decoded.value().tuple.begin()));
}

TEST(WalPayloadTest, HeapWriteRoundTripsAZeroLengthTuple) {
    HeapWritePayload fields{};
    fields.slot = 0;
    const auto record = ThroughEnvelope(RecordType::kHeapOverwrite, [&](std::span<std::byte> out) {
        return EncodeHeapWrite(out, fields, {});
    });
    auto decoded = DecodeHeapWrite(PayloadOf(record));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_TRUE(decoded.value().tuple.empty());
}

TEST(WalPayloadTest, HeapWriteLengthComesFromTheBytesNotTheField) {
    HeapWritePayload fields{};
    fields.tuple_len = 999;  // a lie the encoder must ignore
    const std::vector<std::byte> tuple = Pattern(8, 1);

    std::array<std::byte, 64> out{};
    auto size = EncodeHeapWrite(out, fields, tuple);
    ASSERT_TRUE(size.ok()) << size.status().message();

    auto decoded = DecodeHeapWrite(std::span(out).first(size.value()));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().fields.tuple_len, tuple.size());
}

TEST(WalPayloadTest, HeapWriteRejectsALengthPastThePayload) {
    HeapWritePayload fields{};
    const std::vector<std::byte> tuple = Pattern(8, 1);
    std::array<std::byte, 64> out{};
    auto size = EncodeHeapWrite(out, fields, tuple);
    ASSERT_TRUE(size.ok());

    // Intact bytes that say something impossible: a hard error, not a torn
    // tail (the envelope's CRC already vouched for them).
    out[kHeapWriteTupleLenOffset] = std::byte{0xFF};
    out[kHeapWriteTupleLenOffset + 1] = std::byte{0x00};
    EXPECT_EQ(DecodeHeapWrite(std::span(out).first(size.value())).status().code(),
              StatusCode::kCorruption);
}

TEST(WalPayloadTest, HeapWriteRejectsATrxIdBeyondFortyEightBits) {
    HeapWritePayload fields{};
    fields.trx_id = kMaxTxnId + 1;
    std::array<std::byte, 64> out{};
    EXPECT_EQ(EncodeHeapWrite(out, fields, {}).status().code(), StatusCode::kInvalidArgument);

    // And on the way back in, since the upper 16 bits are an invariant of
    // the format, not just of this build's writers.
    fields.trx_id = kMaxTxnId;
    auto size = EncodeHeapWrite(out, fields, {});
    ASSERT_TRUE(size.ok());
    out[kHeapWriteTrxIdOffset + 6] = std::byte{0x01};
    EXPECT_EQ(DecodeHeapWrite(std::span(out).first(size.value())).status().code(),
              StatusCode::kCorruption);
}

TEST(WalPayloadTest, HeapWriteRejectsATooSmallBuffer) {
    HeapWritePayload fields{};
    std::array<std::byte, kHeapWriteFixedSize - 1> out{};
    EXPECT_EQ(EncodeHeapWrite(out, fields, {}).status().code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(DecodeHeapWrite(out).status().code(), StatusCode::kCorruption);
}

// ---- HEAP_DELETE_MARK / SLOT_RETIRE --------------------------------------

TEST(WalPayloadTest, DeleteMarkRoundTrips) {
    HeapDeleteMarkPayload fields{};
    fields.trx_id = 0x0000FFFFFFFFFFFFull;
    fields.slot = 300;

    const auto record = ThroughEnvelope(RecordType::kHeapDeleteMark, [&](std::span<std::byte> out) {
        return EncodeHeapDeleteMark(out, fields);
    });
    auto decoded = DecodeHeapDeleteMark(PayloadOf(record));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().trx_id, fields.trx_id);
    EXPECT_EQ(decoded.value().slot, fields.slot);
}

TEST(WalPayloadTest, DeleteMarkRejectsATrxIdBeyondFortyEightBits) {
    HeapDeleteMarkPayload fields{};
    fields.trx_id = kMaxTxnId + 1;
    std::array<std::byte, 32> out{};
    EXPECT_EQ(EncodeHeapDeleteMark(out, fields).status().code(), StatusCode::kInvalidArgument);
}

TEST(WalPayloadTest, SlotRetireRoundTrips) {
    // Retirement belongs to a purge pass, not a transaction, so it travels
    // in an envelope with no txn_id - the payload is just the slot.
    SlotRetirePayload fields{};
    fields.slot = 65535;

    std::array<std::byte, 16> out{};
    auto size = EncodeSlotRetire(out, fields);
    ASSERT_TRUE(size.ok()) << size.status().message();
    EXPECT_EQ(size.value(), kSlotRetirePayloadSize);

    auto decoded = DecodeSlotRetire(std::span(out).first(size.value()));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().slot, fields.slot);
}

// ---- UNDO_WRITE ----------------------------------------------------------

TEST(WalPayloadTest, UndoWriteRoundTripsTheChainLinkAndBeforeImage) {
    UndoWritePayload fields{};
    fields.prior_trx_id = 0x00007FFFFFFFFFFFull;
    fields.prior_undo_ptr = 0xDEADBEEFull;
    fields.offset = 1024;
    const std::vector<std::byte> image = Pattern(200, 5);

    const auto record = ThroughEnvelope(RecordType::kUndoWrite, [&](std::span<std::byte> out) {
        return EncodeUndoWrite(out, fields, image);
    });
    auto decoded = DecodeUndoWrite(PayloadOf(record));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    // Prior writer + prior undo_ptr are the link that reconstructs validity
    // intervals with no xmax anywhere (wal.md section 5.1).
    EXPECT_EQ(decoded.value().fields.prior_trx_id, fields.prior_trx_id);
    EXPECT_EQ(decoded.value().fields.prior_undo_ptr, fields.prior_undo_ptr);
    EXPECT_EQ(decoded.value().fields.offset, fields.offset);
    ASSERT_EQ(decoded.value().image.size(), image.size());
    EXPECT_TRUE(std::equal(image.begin(), image.end(), decoded.value().image.begin()));
}

TEST(WalPayloadTest, UndoWriteRejectsAnImageRunningPastThePage) {
    UndoWritePayload fields{};
    fields.offset = static_cast<std::uint16_t>(kPageSize - 4);
    const std::vector<std::byte> image = Pattern(8, 2);

    std::vector<std::byte> out(64);
    EXPECT_EQ(EncodeUndoWrite(out, fields, image).status().code(), StatusCode::kInvalidArgument);

    // Same check on the way back: replaying it would write outside the page.
    fields.offset = 0;
    auto size = EncodeUndoWrite(out, fields, image);
    ASSERT_TRUE(size.ok());
    const auto bad_offset = static_cast<std::uint16_t>(kPageSize - 4);
    std::memcpy(out.data() + kUndoOffsetOffset, &bad_offset, sizeof(bad_offset));
    EXPECT_EQ(DecodeUndoWrite(std::span(out).first(size.value())).status().code(),
              StatusCode::kCorruption);
}

TEST(WalPayloadTest, UndoWriteChainTerminatesOnAZeroPointer) {
    UndoWritePayload fields{};
    fields.prior_trx_id = 5;
    fields.prior_undo_ptr = 0;  // no predecessor: the chain ends here

    std::vector<std::byte> out(64);
    auto size = EncodeUndoWrite(out, fields, {});
    ASSERT_TRUE(size.ok()) << size.status().message();
    auto decoded = DecodeUndoWrite(std::span(out).first(size.value()));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().fields.prior_undo_ptr, 0u);
    EXPECT_TRUE(decoded.value().image.empty());
}

// ---- ALLOC / FREE --------------------------------------------------------

TEST(WalPayloadTest, PageRunRoundTripsAndRejectsAnEmptyRun) {
    PageRunPayload fields{};
    fields.nr_pages = 64;

    const auto record = ThroughEnvelope(RecordType::kAlloc, [&](std::span<std::byte> out) {
        return EncodePageRun(out, fields);
    });
    auto decoded = DecodePageRun(PayloadOf(record));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().nr_pages, fields.nr_pages);

    fields.nr_pages = 0;
    std::array<std::byte, 16> out{};
    EXPECT_EQ(EncodePageRun(out, fields).status().code(), StatusCode::kInvalidArgument);
    // Zeroed bytes read as an allocation of nothing; that is corruption, not
    // a no-op to replay.
    std::array<std::byte, kPageRunPayloadSize> zeroes{};
    EXPECT_EQ(DecodePageRun(zeroes).status().code(), StatusCode::kCorruption);
}

// ---- FULL_PAGE_IMAGE -----------------------------------------------------

TEST(WalPayloadTest, FullPageImageRoundTripsAWholePage) {
    std::array<std::byte, kPageSize> page{};
    for (std::size_t i = 0; i < kPageSize; ++i) {
        page[i] = static_cast<std::byte>((i * 31u) & 0xFF);
    }

    const auto record = ThroughEnvelope(RecordType::kFullPageImage, [&](std::span<std::byte> out) {
        return EncodeFullPageImage(out, std::span<const std::byte, kPageSize>(page));
    });
    // kPageSize is a multiple of the record alignment, so an FPI record
    // carries no padding at all.
    EXPECT_EQ(record.size(), kRecordHeaderSize + kPageSize);

    auto decoded = DecodeFullPageImage(PayloadOf(record));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    ASSERT_EQ(decoded.value().size(), kPageSize);
    EXPECT_TRUE(std::equal(page.begin(), page.end(), decoded.value().begin()));
}

TEST(WalPayloadTest, FullPageImageRejectsAShortPayload) {
    std::vector<std::byte> short_payload(kPageSize - 1);
    EXPECT_EQ(DecodeFullPageImage(short_payload).status().code(), StatusCode::kCorruption);
}

// ---- CHECKPOINT ----------------------------------------------------------

TEST(WalPayloadTest, CheckpointBeginRoundTripsBothTables) {
    const std::vector<std::uint64_t> txns = {7, 9, 0x0000FFFFFFFFFFFFull};
    const std::vector<CheckpointDirtyPage> dirty = {
        {1, 4096}, {2, 8192}, {kInvalidPageId - 1, 0x1234567890ull}};

    const auto record =
        ThroughEnvelope(RecordType::kCheckpointBegin, [&](std::span<std::byte> out) {
            return EncodeCheckpointBegin(out, txns, dirty);
        });
    auto decoded = DecodeCheckpointBegin(PayloadOf(record));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().active_txns, txns);
    ASSERT_EQ(decoded.value().dirty_pages.size(), dirty.size());
    for (std::size_t i = 0; i < dirty.size(); ++i) {
        EXPECT_EQ(decoded.value().dirty_pages[i].page_id, dirty[i].page_id) << "entry " << i;
        EXPECT_EQ(decoded.value().dirty_pages[i].rec_lsn, dirty[i].rec_lsn) << "entry " << i;
    }
}

TEST(WalPayloadTest, CheckpointBeginRoundTripsEmptyTables) {
    // A quiet core still checkpoints; nothing live and nothing dirty is the
    // normal steady state, not an edge case.
    const auto record =
        ThroughEnvelope(RecordType::kCheckpointBegin,
                        [&](std::span<std::byte> out) { return EncodeCheckpointBegin(out, {}, {}); });
    auto decoded = DecodeCheckpointBegin(PayloadOf(record));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_TRUE(decoded.value().active_txns.empty());
    EXPECT_TRUE(decoded.value().dirty_pages.empty());
}

TEST(WalPayloadTest, CheckpointBeginRejectsCountsTheRecordDoesNotBack) {
    const std::vector<std::uint64_t> txns = {1};
    const std::vector<CheckpointDirtyPage> dirty = {{5, 64}};
    std::vector<std::byte> out(CheckpointBeginSize(txns.size(), dirty.size()));
    auto size = EncodeCheckpointBegin(out, txns, dirty);
    ASSERT_TRUE(size.ok()) << size.status().message();

    // A count that would have the decoder read - and reserve - far past the
    // bytes the record actually carries.
    const std::uint32_t huge = 0xFFFFFFFFu;
    std::memcpy(out.data() + kCheckpointDirtyCountOffset, &huge, sizeof(huge));
    EXPECT_EQ(DecodeCheckpointBegin(std::span(out).first(size.value())).status().code(),
              StatusCode::kCorruption);
}

TEST(WalPayloadTest, CheckpointBeginRejectsAnInvalidPageIdInTheDirtyTable) {
    const std::vector<CheckpointDirtyPage> dirty = {{kInvalidPageId, 64}};
    std::vector<std::byte> out(CheckpointBeginSize(0, dirty.size()));
    auto size = EncodeCheckpointBegin(out, {}, dirty);
    ASSERT_TRUE(size.ok());
    EXPECT_EQ(DecodeCheckpointBegin(std::span(out).first(size.value())).status().code(),
              StatusCode::kCorruption);
}

TEST(WalPayloadTest, CheckpointBeginRejectsAZeroTxnIdInTheActiveTable) {
    // 0 means "non-transactional" in the envelope, so it can never name a
    // live transaction.
    const std::vector<std::uint64_t> txns = {0};
    std::vector<std::byte> out(CheckpointBeginSize(txns.size(), 0));
    EXPECT_EQ(EncodeCheckpointBegin(out, txns, {}).status().code(), StatusCode::kInvalidArgument);
}

TEST(WalPayloadTest, CheckpointEndRoundTrips) {
    CheckpointEndPayload fields{};
    fields.redo_start_lsn = 0x0102030405060708ull;

    const auto record = ThroughEnvelope(RecordType::kCheckpointEnd, [&](std::span<std::byte> out) {
        return EncodeCheckpointEnd(out, fields);
    });
    auto decoded = DecodeCheckpointEnd(PayloadOf(record));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().redo_start_lsn, fields.redo_start_lsn);
}

// ---- Record type registry ------------------------------------------------

TEST(WalPayloadTest, AppendedTypesAreAssignedAndNamed) {
    // The enum is frozen and append-only: UNDO_WRITE and FREE were appended
    // after PAD, so PAD must still be 13.
    EXPECT_EQ(static_cast<std::uint8_t>(RecordType::kPad), 13);
    EXPECT_EQ(static_cast<std::uint8_t>(RecordType::kUndoWrite), 14);
    EXPECT_EQ(static_cast<std::uint8_t>(RecordType::kFree), 15);
    EXPECT_EQ(kMaxAssignedRecordType, 15);

    EXPECT_TRUE(IsAssignedRecordType(static_cast<std::uint8_t>(RecordType::kUndoWrite)));
    EXPECT_TRUE(IsAssignedRecordType(static_cast<std::uint8_t>(RecordType::kFree)));
    EXPECT_FALSE(IsAssignedRecordType(kMaxAssignedRecordType + 1));
    EXPECT_STREQ(RecordTypeName(RecordType::kUndoWrite), "UNDO_WRITE");
    EXPECT_STREQ(RecordTypeName(RecordType::kFree), "FREE");
}

}  // namespace
}  // namespace kds::wal
