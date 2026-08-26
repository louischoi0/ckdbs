#include "kds/txn/undo_page.hpp"

#include <array>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/page_header.hpp"

// docs/spec/txn.md section 10-1, the codec half: record round-trips, undo_ptr
// packing over the whole page-id range, kNoUndoPtr unreachable from any
// legal (page, offset), and appending until OutOfSpace.

namespace kds::txn {
namespace {

using PageBuf = std::array<std::byte, kPageSize>;

std::span<std::byte, kPageSize> AsSpan(PageBuf& buf) {
    return std::span<std::byte, kPageSize>(buf);
}

std::span<const std::byte, kPageSize> AsConstSpan(const PageBuf& buf) {
    return std::span<const std::byte, kPageSize>(buf);
}

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

UndoRecordFields OverwriteRecord(std::uint64_t prior_trx_id, std::uint64_t prior_undo_ptr) {
    UndoRecordFields r{};
    r.prior_trx_id = prior_trx_id;
    r.prior_undo_ptr = prior_undo_ptr;
    r.target_page_id = 4242;
    r.target_slot = 7;
    r.image_len = 0;  // set from the image on write
    r.type = static_cast<std::uint8_t>(UndoRecordType::kOverwrite);
    r.flags = 0;
    r.reserved = 0;
    return r;
}

TEST(UndoPageTest, FormatStampsTheCommonHeaderAndTheOwner) {
    PageBuf buf{};
    ASSERT_TRUE(FormatUndoPage(AsSpan(buf), /*first_trx_id=*/91, /*prev_page_id=*/kInvalidPageId)
                    .ok());

    EXPECT_TRUE(storage::ValidatePageHeader(AsConstSpan(buf), PageType::kUndo).ok());

    const UndoPageHeaderFields h = ReadUndoPageHeader(AsConstSpan(buf));
    EXPECT_EQ(h.flags, kUndoPageFlagInitialized);
    EXPECT_EQ(h.nr_records, 0);
    EXPECT_EQ(h.lower, kUndoRecordsOffset);
    EXPECT_EQ(h.first_trx_id, 91u);
    EXPECT_EQ(h.prev_page_id, kInvalidPageId);
    EXPECT_EQ(h.reserved0, 0);
    EXPECT_EQ(h.reserved1, 0u);
}

TEST(UndoPageTest, AnOverwriteRecordRoundTrips) {
    PageBuf buf{};
    ASSERT_TRUE(FormatUndoPage(AsSpan(buf), 91, kInvalidPageId).ok());

    const std::vector<std::byte> image = BytesOf("the version being superseded");
    auto offset = UndoPageAppend(AsSpan(buf), OverwriteRecord(77, kNoUndoPtr), image);
    ASSERT_TRUE(offset.ok()) << offset.status().message();
    EXPECT_EQ(offset.value(), kUndoRecordsOffset);

    auto read = UndoPageRead(AsConstSpan(buf), offset.value());
    ASSERT_TRUE(read.ok()) << read.status().message();
    EXPECT_EQ(read.value().fields.prior_trx_id, 77u);
    EXPECT_EQ(read.value().fields.prior_undo_ptr, kNoUndoPtr);
    EXPECT_EQ(read.value().fields.target_page_id, 4242u);
    EXPECT_EQ(read.value().fields.target_slot, 7);
    EXPECT_EQ(read.value().fields.image_len, image.size());
    EXPECT_EQ(read.value().fields.type, static_cast<std::uint8_t>(UndoRecordType::kOverwrite));
    EXPECT_EQ(StringOf(read.value().image), "the version being superseded");

    const UndoPageHeaderFields h = ReadUndoPageHeader(AsConstSpan(buf));
    EXPECT_EQ(h.nr_records, 1);
    EXPECT_EQ(h.lower, kUndoRecordsOffset + kUndoRecordHeaderSize + image.size());
}

// A delete-mark changes no tuple bytes, so its image is empty by
// definition (txn.md section 3.3) - and an image supplied anyway is
// refused rather than silently dropped.
TEST(UndoPageTest, ADeleteMarkCarriesNoImage) {
    PageBuf buf{};
    ASSERT_TRUE(FormatUndoPage(AsSpan(buf), 91, kInvalidPageId).ok());

    UndoRecordFields r = OverwriteRecord(77, kNoUndoPtr);
    r.type = static_cast<std::uint8_t>(UndoRecordType::kDeleteMark);

    auto offset = UndoPageAppend(AsSpan(buf), r, {});
    ASSERT_TRUE(offset.ok()) << offset.status().message();
    auto read = UndoPageRead(AsConstSpan(buf), offset.value());
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value().fields.image_len, 0);
    EXPECT_TRUE(read.value().image.empty());

    const std::vector<std::byte> image = BytesOf("bytes nobody would read");
    auto refused = UndoPageAppend(AsSpan(buf), r, image);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kInvalidArgument);
}

TEST(UndoPageTest, RecordsChainByPriorUndoPtr) {
    PageBuf buf{};
    ASSERT_TRUE(FormatUndoPage(AsSpan(buf), 91, kInvalidPageId).ok());

    auto first = UndoPageAppend(AsSpan(buf), OverwriteRecord(70, kNoUndoPtr), BytesOf("v1"));
    ASSERT_TRUE(first.ok());
    const std::uint64_t first_ptr = EncodeUndoPtr(9, first.value());

    auto second = UndoPageAppend(AsSpan(buf), OverwriteRecord(71, first_ptr), BytesOf("v2"));
    ASSERT_TRUE(second.ok()) << second.status().message();
    EXPECT_GT(second.value(), first.value());

    auto read = UndoPageRead(AsConstSpan(buf), second.value());
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value().fields.prior_undo_ptr, first_ptr);
    EXPECT_EQ(UndoPtrPageId(read.value().fields.prior_undo_ptr), 9u);
    EXPECT_EQ(UndoPtrOffset(read.value().fields.prior_undo_ptr), first.value());
}

TEST(UndoPageTest, AppendFillsThePageAndThenReportsOutOfSpace) {
    PageBuf buf{};
    ASSERT_TRUE(FormatUndoPage(AsSpan(buf), 91, kInvalidPageId).ok());

    // 100-byte images. Derived rather than written out: the count moved
    // from 63 to 56 when RV10 grew the record header from 28 to 44 bytes,
    // and a literal here would have to be chased every time the record
    // changes shape - which is the thing this test exists to notice.
    const std::vector<std::byte> image(100, std::byte{0xAB});
    const int kFits = static_cast<int>(kUndoPageCapacity /
                                       (kUndoRecordHeaderSize + image.size()));
    int appended = 0;
    for (;;) {
        auto offset = UndoPageAppend(AsSpan(buf), OverwriteRecord(77, kNoUndoPtr), image);
        if (!offset.ok()) {
            EXPECT_EQ(offset.status().code(), StatusCode::kOutOfSpace);
            // Named as the *undo* page, so the reader is not sent to the
            // heap page the caller was updating.
            EXPECT_NE(offset.status().message().find("undo page"), std::string::npos)
                << offset.status().message();
            break;
        }
        ++appended;
        ASSERT_LT(appended, 1000) << "the page never filled";
    }
    EXPECT_EQ(appended, kFits);
    EXPECT_EQ(ReadUndoPageHeader(AsConstSpan(buf)).nr_records, kFits);
}

TEST(UndoPageTest, AMaxLengthImageFitsAndOneMoreByteDoesNot) {
    PageBuf buf{};
    ASSERT_TRUE(FormatUndoPage(AsSpan(buf), 91, kInvalidPageId).ok());

    const std::vector<std::byte> too_long(kMaxUndoImageLen + 1, std::byte{0x01});
    auto refused = UndoPageAppend(AsSpan(buf), OverwriteRecord(77, kNoUndoPtr), too_long);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kInvalidArgument);

    const std::vector<std::byte> exact(kMaxUndoImageLen, std::byte{0x01});
    auto fits = UndoPageAppend(AsSpan(buf), OverwriteRecord(77, kNoUndoPtr), exact);
    ASSERT_TRUE(fits.ok()) << fits.status().message();
    auto read = UndoPageRead(AsConstSpan(buf), fits.value());
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value().image.size(), kMaxUndoImageLen);
    EXPECT_EQ(UndoPageFreeSpace(AsConstSpan(buf)), 0);
}

// The known ceiling of txn.md section 3.3, mechanized: undo overhead (84
// bytes) exceeds the heap page's (77), so the widest tuple a heap page can
// hold has a before-image too large for an undo page. The test states the
// arithmetic rather than the consequence, so it fails loudly if either
// layout moves.
TEST(UndoPageTest, TheWidestHeapTupleCannotBeUndone) {
    EXPECT_GT(heap::kMaxTuplePayloadSize, kMaxUndoImageLen);
    // 23 since RV10, 7 before it: the two fields it added to every undo
    // record widen the band of heap tuples that cannot be updated.
    EXPECT_EQ(heap::kMaxTuplePayloadSize - kMaxUndoImageLen, 23u);
}

TEST(UndoPtrTest, PackingRoundTripsOverTheWholePageIdRange) {
    // Every power-of-two boundary plus the top of the id space, which is
    // where a shift that lost a bit would show first.
    for (PageId page_id : {PageId{1}, PageId{2}, PageId{255}, PageId{256}, PageId{65535},
                           PageId{65536}, PageId{1u << 30}, PageId{(1u << 31) - 1}}) {
        for (std::uint16_t offset : {std::uint16_t{kUndoRecordsOffset}, std::uint16_t{1024},
                                     std::uint16_t{kPageSize - kUndoRecordHeaderSize}}) {
            const std::uint64_t ptr = EncodeUndoPtr(page_id, offset);
            EXPECT_EQ(UndoPtrPageId(ptr), page_id);
            EXPECT_EQ(UndoPtrOffset(ptr), offset);
            // Invariant 6's zero-extension convention: bits 48..63 always 0.
            EXPECT_EQ(ptr >> 48, 0u) << "page_id " << page_id;
            EXPECT_TRUE(UndoPtrIsPlausible(ptr).ok());
        }
    }
}

// kNoUndoPtr is unambiguous *structurally*, not by convention: page 0 is
// the superblock and offset 0 is inside the common page header. This is
// the mechanized form of that claim.
TEST(UndoPtrTest, NoUndoPtrIsUnreachableFromAnyLegalLocation) {
    for (PageId page_id : {PageId{1}, PageId{128}, PageId{1u << 20}}) {
        for (std::size_t offset = kUndoRecordsOffset;
             offset <= kPageSize - kUndoRecordHeaderSize; ++offset) {
            EXPECT_NE(EncodeUndoPtr(page_id, static_cast<std::uint16_t>(offset)), kNoUndoPtr);
        }
    }
    EXPECT_FALSE(UndoPtrIsPlausible(kNoUndoPtr).ok());
}

TEST(UndoPtrTest, ImplausiblePointersAreCorruptionNotAMiss) {
    // Page 0 is the superblock.
    EXPECT_EQ(UndoPtrIsPlausible(EncodeUndoPtr(0, 1024)).code(), StatusCode::kCorruption);
    // Below the record area: inside the page header.
    EXPECT_EQ(UndoPtrIsPlausible(EncodeUndoPtr(9, kUndoRecordsOffset - 1)).code(),
              StatusCode::kCorruption);
    // A record header that would run past the end of the page.
    EXPECT_EQ(UndoPtrIsPlausible(EncodeUndoPtr(9, kPageSize - kUndoRecordHeaderSize + 1)).code(),
              StatusCode::kCorruption);
    // Nonzero bits above 48.
    EXPECT_EQ(UndoPtrIsPlausible((std::uint64_t{1} << 48) | EncodeUndoPtr(9, 1024)).code(),
              StatusCode::kCorruption);
}

TEST(UndoPageTest, ReadRefusesAnOffsetOutsideTheRecordArea) {
    PageBuf buf{};
    ASSERT_TRUE(FormatUndoPage(AsSpan(buf), 91, kInvalidPageId).ok());
    EXPECT_EQ(UndoPageRead(AsConstSpan(buf), 0).status().code(), StatusCode::kCorruption);
    EXPECT_EQ(UndoPageRead(AsConstSpan(buf), kUndoRecordsOffset - 1).status().code(),
              StatusCode::kCorruption);
}

TEST(UndoPageTest, ReadRefusesAnImageRunningPastThePage) {
    PageBuf buf{};
    ASSERT_TRUE(FormatUndoPage(AsSpan(buf), 91, kInvalidPageId).ok());
    auto offset = UndoPageAppend(AsSpan(buf), OverwriteRecord(77, kNoUndoPtr), BytesOf("v1"));
    ASSERT_TRUE(offset.ok());

    // Corrupt image_len in place: the bytes are intact and wrong, which is
    // a hard error rather than something to tolerate.
    const std::uint16_t absurd = 60000;
    std::memcpy(buf.data() + offset.value() + kUndoRecImageLenOffset, &absurd, sizeof(absurd));
    EXPECT_EQ(UndoPageRead(AsConstSpan(buf), offset.value()).status().code(),
              StatusCode::kCorruption);
}

TEST(UndoPageTest, WriteAtIsIdempotentSoRedoMayRunTwice) {
    PageBuf buf{};
    ASSERT_TRUE(FormatUndoPage(AsSpan(buf), 91, kInvalidPageId).ok());

    const std::vector<std::byte> image = BytesOf("replayed image");
    const std::uint16_t at = static_cast<std::uint16_t>(kUndoRecordsOffset);
    ASSERT_TRUE(UndoPageWriteAt(AsSpan(buf), at, OverwriteRecord(77, kNoUndoPtr), image).ok());
    const UndoPageHeaderFields after_first = ReadUndoPageHeader(AsConstSpan(buf));

    ASSERT_TRUE(UndoPageWriteAt(AsSpan(buf), at, OverwriteRecord(77, kNoUndoPtr), image).ok());
    const UndoPageHeaderFields after_second = ReadUndoPageHeader(AsConstSpan(buf));

    EXPECT_EQ(after_first.lower, after_second.lower);
    EXPECT_EQ(after_first.nr_records, after_second.nr_records);
    auto read = UndoPageRead(AsConstSpan(buf), at);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(StringOf(read.value().image), "replayed image");
}

TEST(UndoPageTest, RefusesATrxIdAboveFortyEightBits) {
    PageBuf buf{};
    ASSERT_TRUE(FormatUndoPage(AsSpan(buf), 91, kInvalidPageId).ok());
    auto refused =
        UndoPageAppend(AsSpan(buf), OverwriteRecord(heap::kMaxTrxId + 1, kNoUndoPtr), {});
    EXPECT_EQ(refused.status().code(), StatusCode::kInvalidArgument);

    EXPECT_EQ(FormatUndoPage(AsSpan(buf), heap::kMaxTrxId + 1, kInvalidPageId).code(),
              StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace kds::txn
