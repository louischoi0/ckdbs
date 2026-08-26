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
#include "kds/txn/undo_page.hpp"

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
    // page as something this build does not understand. Sliced to the
    // payload's exact size so the *type* check is what fires, not the
    // length discrimination below.
    const auto payload = std::span<const std::byte>(out).first(kPageInitPayloadSize);
    out[kPageInitPageTypeOffset] = static_cast<std::byte>(kMaxAssignedPageType + 1);
    EXPECT_EQ(DecodePageInit(payload).status().code(), StatusCode::kCorruption);

    out[kPageInitPageTypeOffset] = static_cast<std::byte>(PageType::kInvalid);
    EXPECT_EQ(DecodePageInit(payload).status().code(), StatusCode::kCorruption);
}

TEST(WalPayloadTest, PageInitCarriesTheOwnerOid) {
    PageInitPayload fields{};
    fields.min_key = 99;
    fields.page_type = static_cast<std::uint8_t>(PageType::kHeap);
    fields.owner_oid = 4001;

    const auto record = ThroughEnvelope(RecordType::kPageInit, [&](std::span<std::byte> out) {
        return EncodePageInit(out, fields);
    });
    auto decoded = DecodePageInit(PayloadOf(record));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().owner_oid, 4001u);
}

TEST(WalPayloadTest, PageInitLegacyTwelveByteFormDecodesAsUnattributed) {
    // A record written before page.md section 2a: min_key, page_type, three
    // reserved bytes, nothing else. It must decode, and its owner is 0.
    std::array<std::byte, kPageInitPayloadSizeLegacy> legacy{};
    const std::uint64_t min_key = 77;
    std::memcpy(legacy.data() + kPageInitMinKeyOffset, &min_key, sizeof(min_key));
    legacy[kPageInitPageTypeOffset] = static_cast<std::byte>(PageType::kVarHeap);

    auto decoded = DecodePageInit(legacy);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().min_key, 77u);
    EXPECT_EQ(decoded.value().owner_oid, 0u);
}

TEST(WalPayloadTest, PageInitLegacyFormSurvivesTheEnvelopesPadding) {
    // The case the both-lengths decode exists for, and the one an equality
    // test silently refuses: DecodeRecord returns the record's 8-byte-aligned
    // tail rather than the exact payload, so a pre-section-2a 12-byte payload
    // arrives here as 16 bytes - twelve written and four of the encoder's
    // zeroed padding. A replayed pre-owner WAL comes through this path.
    const auto record =
        ThroughEnvelope(RecordType::kPageInit,
                        [](std::span<std::byte> out) -> StatusOr<std::size_t> {
                            const std::uint64_t min_key = 77;
                            std::memcpy(out.data() + kPageInitMinKeyOffset, &min_key,
                                        sizeof(min_key));
                            out[kPageInitPageTypeOffset] =
                                static_cast<std::byte>(PageType::kVarHeap);
                            return kPageInitPayloadSizeLegacy;
                        });
    const auto payload = PayloadOf(record);
    ASSERT_EQ(payload.size(), 16u);  // never 12: the alignment is the point

    auto decoded = DecodePageInit(payload);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().min_key, 77u);
    EXPECT_EQ(decoded.value().owner_oid, 0u);
}

TEST(WalPayloadTest, PageInitRejectsAPayloadShorterThanTheLegacyForm) {
    // Below the pre-section-2a floor there is no min_key to read at all:
    // CRC-vouched bytes that are intact and wrong.
    std::array<std::byte, 8> stub{};
    EXPECT_EQ(DecodePageInit(stub).status().code(), StatusCode::kCorruption);
}

// ---- PAGE_HANDOFF --------------------------------------------------------

TEST(WalPayloadTest, PageHandoffRoundTripsThroughTheEnvelope) {
    // Through the envelope, because that is the only way this record is ever
    // read: the four-byte payload comes back as the record's eight-byte
    // aligned tail. PW1c-1's decoder tested `!= 4` and refused exactly this -
    // every real record - and passed only because its test handed the codec a
    // bare 4-byte buffer.
    const auto record = ThroughEnvelope(RecordType::kPageHandoff,
                                        [](std::span<std::byte> out) {
                                            return EncodePageHandoff(out, PageHandoffPayload{3});
                                        });
    const auto payload = PayloadOf(record);
    ASSERT_EQ(payload.size(), 8u);  // never 4: the alignment is the point

    auto decoded = DecodePageHandoff(payload);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().incoming_core, 3u);
}

TEST(WalPayloadTest, PageHandoffRejectsABufferShorterThanTheCore) {
    std::array<std::byte, 2> stub{};
    EXPECT_EQ(EncodePageHandoff(stub, PageHandoffPayload{1}).status().code(),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(DecodePageHandoff(std::span<const std::byte>(stub)).status().code(),
              StatusCode::kCorruption);
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

TEST(WalPayloadTest, UndoWriteRoundTripsTheChainLinkAndTheRecordTail) {
    // The payload carries the two chain links as fields and the undo
    // record's *tail* as bytes (docs/spec/txn.md section 3.5) - so the fields
    // naming which tuple the image belongs to survive the round trip,
    // which is what redo needs and what the pre-2026-08-10 writer dropped.
    UndoWritePayload fields{};
    fields.prior_trx_id = 0x00007FFFFFFFFFFFull;
    fields.prior_undo_ptr = 0xDEADBEEFull;
    fields.offset = 1024;

    const std::vector<std::byte> image = Pattern(200, 5);
    txn::UndoRecordFields rec{};
    rec.target_page_id = 4242;
    rec.target_slot = 7;
    rec.type = static_cast<std::uint8_t>(txn::UndoRecordType::kOverwrite);
    std::vector<std::byte> tail(txn::UndoRecordTailSize(image.size()));
    ASSERT_TRUE(txn::EncodeUndoRecordTail(tail, rec, image).ok());

    const auto record = ThroughEnvelope(RecordType::kUndoWrite, [&](std::span<std::byte> out) {
        return EncodeUndoWrite(out, fields, tail);
    });
    auto decoded = DecodeUndoWrite(PayloadOf(record));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    // Prior writer + prior undo_ptr are the link that reconstructs validity
    // intervals with no xmax anywhere (wal.md section 5.1).
    EXPECT_EQ(decoded.value().fields.prior_trx_id, fields.prior_trx_id);
    EXPECT_EQ(decoded.value().fields.prior_undo_ptr, fields.prior_undo_ptr);
    EXPECT_EQ(decoded.value().fields.offset, fields.offset);

    auto back = txn::DecodeUndoRecordTail(decoded.value().tail);
    ASSERT_TRUE(back.ok()) << back.status().message();
    EXPECT_EQ(back.value().fields.target_page_id, rec.target_page_id);
    EXPECT_EQ(back.value().fields.target_slot, rec.target_slot);
    EXPECT_EQ(back.value().fields.type, rec.type);
    ASSERT_EQ(back.value().image.size(), image.size());
    EXPECT_TRUE(std::equal(image.begin(), image.end(), back.value().image.begin()));
}

TEST(WalPayloadTest, UndoWriteRejectsATailRunningPastThePage) {
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
    EXPECT_TRUE(decoded.value().tail.empty());
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
    // Each carries its undo-chain head (RV10). The third is kNoUndoPtr,
    // which is legal and means the transaction had written nothing yet.
    const std::vector<CheckpointActiveTxn> txns = {
        {7, 0x00010020ull}, {9, 0x00020038ull}, {0x0000FFFFFFFFFFFFull, 0}};
    const std::vector<CheckpointDirtyPage> dirty = {
        {1, 4096}, {2, 8192}, {kInvalidPageId - 1, 0x1234567890ull}};

    const auto record =
        ThroughEnvelope(RecordType::kCheckpointBegin, [&](std::span<std::byte> out) {
            return EncodeCheckpointBegin(out, txns, dirty);
        });
    auto decoded = DecodeCheckpointBegin(PayloadOf(record));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    ASSERT_EQ(decoded.value().active_txns.size(), txns.size());
    for (std::size_t i = 0; i < txns.size(); ++i) {
        EXPECT_EQ(decoded.value().active_txns[i].txn_id, txns[i].txn_id) << "txn " << i;
        EXPECT_EQ(decoded.value().active_txns[i].last_undo_ptr, txns[i].last_undo_ptr)
            << "txn " << i;
    }
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
    const std::vector<CheckpointActiveTxn> txns = {{1, 0}};
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
    const std::vector<CheckpointActiveTxn> txns = {{0, 0}};
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
    // The enum is frozen and append-only, so every number here is a
    // compatibility fact and this test is meant to be *extended* when one is
    // appended - never edited. UNDO_WRITE and FREE went after PAD, so PAD is
    // still 13; VARHEAP_APPEND after both; INDEX_INSERT after all of them.
    EXPECT_EQ(static_cast<std::uint8_t>(RecordType::kPad), 13);
    EXPECT_EQ(static_cast<std::uint8_t>(RecordType::kUndoWrite), 14);
    EXPECT_EQ(static_cast<std::uint8_t>(RecordType::kFree), 15);
    EXPECT_EQ(static_cast<std::uint8_t>(RecordType::kVarHeapAppend), 16);
    EXPECT_EQ(static_cast<std::uint8_t>(RecordType::kIndexInsert), 17);
    EXPECT_EQ(static_cast<std::uint8_t>(RecordType::kAssertReserve), 18);
    EXPECT_EQ(static_cast<std::uint8_t>(RecordType::kAssertCommit), 19);
    EXPECT_EQ(static_cast<std::uint8_t>(RecordType::kAssertRollback), 20);
    EXPECT_EQ(static_cast<std::uint8_t>(RecordType::kAssertBuild), 21);
    EXPECT_EQ(static_cast<std::uint8_t>(RecordType::kAssertDrop), 22);
    EXPECT_EQ(static_cast<std::uint8_t>(RecordType::kHeapDeleteUnmark), 23);
    EXPECT_EQ(static_cast<std::uint8_t>(RecordType::kAssertSnapshot), 24);
    EXPECT_EQ(static_cast<std::uint8_t>(RecordType::kPageHandoff), 25);
    EXPECT_EQ(static_cast<std::uint8_t>(RecordType::kAnchorUpdate), 26);
    // Derived from the enum now, not typed here: pinning it as a literal is
    // what let type 23 ship unwritable (record.hpp).
    EXPECT_EQ(kMaxAssignedRecordType, 26);

    EXPECT_TRUE(IsAssignedRecordType(static_cast<std::uint8_t>(RecordType::kUndoWrite)));
    EXPECT_TRUE(IsAssignedRecordType(static_cast<std::uint8_t>(RecordType::kFree)));
    EXPECT_TRUE(IsAssignedRecordType(static_cast<std::uint8_t>(RecordType::kVarHeapAppend)));
    EXPECT_TRUE(IsAssignedRecordType(static_cast<std::uint8_t>(RecordType::kIndexInsert)));
    EXPECT_TRUE(IsAssignedRecordType(static_cast<std::uint8_t>(RecordType::kPageHandoff)));
    EXPECT_TRUE(IsAssignedRecordType(static_cast<std::uint8_t>(RecordType::kAnchorUpdate)));
    EXPECT_FALSE(IsAssignedRecordType(kMaxAssignedRecordType + 1));
    EXPECT_STREQ(RecordTypeName(RecordType::kUndoWrite), "UNDO_WRITE");
    EXPECT_STREQ(RecordTypeName(RecordType::kFree), "FREE");
    EXPECT_STREQ(RecordTypeName(RecordType::kVarHeapAppend), "VARHEAP_APPEND");
    EXPECT_STREQ(RecordTypeName(RecordType::kIndexInsert), "INDEX_INSERT");
    EXPECT_STREQ(RecordTypeName(RecordType::kPageHandoff), "PAGE_HANDOFF");
    EXPECT_STREQ(RecordTypeName(RecordType::kAnchorUpdate), "ANCHOR_UPDATE");
}

TEST(WalPayloadTest, AnchorUpdateRoundTripsThroughTheEnvelope) {
    const auto record = ThroughEnvelope(RecordType::kAnchorUpdate,
                                        [](std::span<std::byte> out) {
                                            return EncodeAnchorUpdate(
                                                out, AnchorUpdatePayload{9001, 310});
                                        });
    auto decoded = DecodeAnchorUpdate(PayloadOf(record));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().index_oid, 9001u);
    EXPECT_EQ(decoded.value().root, 310u);
}

// ---- INDEX_INSERT --------------------------------------------------------

TEST(WalPayloadTest, IndexInsertRoundTrips) {
    const std::vector<std::byte> entry(37, std::byte{0xC3});
    std::vector<std::byte> buf(kIndexInsertFixedSize + entry.size());
    const IndexInsertPayload fields{1234, /*entry_len=*/0};  // set from the span
    auto n = EncodeIndexInsert(buf, fields, entry);
    ASSERT_TRUE(n.ok()) << n.status().message();
    EXPECT_EQ(n.value(), buf.size());

    auto decoded = DecodeIndexInsert(buf);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().fields.slot, 1234);
    EXPECT_EQ(decoded.value().fields.entry_len, entry.size());
    ASSERT_EQ(decoded.value().entry.size(), entry.size());
    EXPECT_EQ(std::memcmp(decoded.value().entry.data(), entry.data(), entry.size()), 0);
}

TEST(WalPayloadTest, IndexInsertRefusesAnEmptyEntryAndALyingLength) {
    // An entry is never empty - a key is at least one byte plus the pk - and
    // a length claiming more than was written must not size a read.
    std::vector<std::byte> buf(kIndexInsertFixedSize + 4);
    EXPECT_FALSE(EncodeIndexInsert(buf, IndexInsertPayload{0, 0}, {}).ok());

    const std::vector<std::byte> entry(4, std::byte{1});
    ASSERT_TRUE(EncodeIndexInsert(buf, IndexInsertPayload{0, 0}, entry).ok());
    // Truncate the payload behind the length's back.
    buf.resize(kIndexInsertFixedSize + 2);
    auto decoded = DecodeIndexInsert(buf);
    EXPECT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.status().code(), StatusCode::kCorruption);
}

// ---- VARHEAP_APPEND ------------------------------------------------------

TEST(WalPayloadTest, VarHeapAppendRoundTrips) {
    const std::string text(4000, 'v');
    auto value = std::as_bytes(std::span<const char>(text));

    std::vector<std::byte> buf(kVarHeapAppendFixedSize + value.size());
    const VarHeapAppendPayload fields{/*slot=*/7, /*reserved=*/0, /*value_len=*/0};
    auto n = EncodeVarHeapAppend(buf, fields, value);
    ASSERT_TRUE(n.ok()) << n.status().message();
    EXPECT_EQ(n.value(), buf.size());

    auto decoded = DecodeVarHeapAppend(buf);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().fields.slot, 7);
    EXPECT_EQ(decoded.value().fields.value_len, value.size());
    EXPECT_TRUE(std::equal(decoded.value().value.begin(), decoded.value().value.end(),
                           value.begin()));
}

TEST(WalPayloadTest, VarHeapAppendLengthComesFromTheSpanNotTheField) {
    // The two records of one fact cannot be allowed to disagree on disk, so
    // the caller's value_len is ignored rather than trusted.
    const std::string text("abc");
    auto value = std::as_bytes(std::span<const char>(text));

    std::vector<std::byte> buf(kVarHeapAppendFixedSize + value.size());
    const VarHeapAppendPayload lying{/*slot=*/0, /*reserved=*/0, /*value_len=*/999};
    ASSERT_TRUE(EncodeVarHeapAppend(buf, lying, value).ok());

    auto decoded = DecodeVarHeapAppend(buf);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().fields.value_len, 3u);
}

TEST(WalPayloadTest, VarHeapAppendLengthPastThePayloadIsCorruption) {
    std::vector<std::byte> buf(kVarHeapAppendFixedSize + 4);
    const std::string text("abcd");
    ASSERT_TRUE(
        EncodeVarHeapAppend(buf, VarHeapAppendPayload{}, std::as_bytes(std::span<const char>(text)))
            .ok());
    // Claim more bytes than the record carries - intact bytes that are
    // wrong, which is a hard recovery error and never something to skip.
    buf[kVarHeapAppendValueLenOffset] = std::byte{0xFF};

    auto decoded = DecodeVarHeapAppend(buf);
    ASSERT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.status().code(), StatusCode::kCorruption);
}

}  // namespace
}  // namespace kds::wal
