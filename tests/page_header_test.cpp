#include "kds/storage/page_header.hpp"

#include <array>
#include <cstring>

#include <gtest/gtest.h>

namespace kds::storage {
namespace {

using Page = std::array<std::byte, kPageSize>;

std::span<std::byte, kPageSize> Mut(Page& p) { return std::span<std::byte, kPageSize>(p); }
std::span<const std::byte, kPageSize> Const(const Page& p) {
    return std::span<const std::byte, kPageSize>(p);
}

TEST(PageHeaderTest, RoundTripsEveryField) {
    Page page{};
    PageHeaderFields written{};
    written.page_type = static_cast<std::uint8_t>(PageType::kHeap);
    written.format_version = 1;
    written.flags = 0xBEEF;
    written.checksum = 0xDEADBEEF;
    written.page_lsn = 0x0123456789ABCDEFULL;
    written.relayout_epoch = 0xFEDCBA9876543210ULL;
    written.owner_oid = 0x1122334455667788ULL;

    WritePageHeader(Mut(page), written);
    const PageHeaderFields read = ReadPageHeader(Const(page));

    EXPECT_EQ(read.page_type, written.page_type);
    EXPECT_EQ(read.format_version, written.format_version);
    EXPECT_EQ(read.flags, written.flags);
    EXPECT_EQ(read.checksum, written.checksum);
    EXPECT_EQ(read.page_lsn, written.page_lsn);
    EXPECT_EQ(read.relayout_epoch, written.relayout_epoch);
    EXPECT_EQ(read.owner_oid, written.owner_oid);
}

TEST(PageHeaderTest, TheStreamStampRoundTripsAndDefaultsToNeverStamped) {
    // PW1c-3 (page-lsn-cross-stream.md §9 rule 4): the flags word
    // carries core_id + 1 of the stream that last wrote the page, and a
    // zeroed page reads 0 - never stamped, the no-backfill default every
    // pre-PW1c-3 page relies on.
    Page page{};
    EXPECT_EQ(GetPageStreamStamp(Const(page)), 0u);

    SetPageStreamStamp(Mut(page), 3);  // core 2's stream
    EXPECT_EQ(GetPageStreamStamp(Const(page)), 3u);
    // The accessor and the whole-struct codec read the same bytes.
    EXPECT_EQ(ReadPageHeader(Const(page)).flags, 3u);
}

TEST(PageHeaderTest, WriteHeaderLeavesBodyUntouched) {
    Page page{};
    std::memset(page.data() + kPageBodyOffset, 0x5A, kPageBodySize);

    PageHeaderFields fields{};
    fields.page_type = static_cast<std::uint8_t>(PageType::kCatalog);
    fields.format_version = 1;
    WritePageHeader(Mut(page), fields);

    for (std::size_t i = kPageBodyOffset; i < kPageSize; ++i) {
        ASSERT_EQ(page[i], std::byte{0x5A}) << "body byte " << i;
    }
}

TEST(PageHeaderTest, EveryAssignedPageTypeHasAFormatVersion) {
    // **The test the kCabinBound bug got past.** The one below it asserts
    // `format_version == MaxSupportedFormatVersion(type)` - which compares
    // FormatPage's output against the very function that produced it, so it
    // passes just as happily when that function returns 0. A 0 is not a
    // version: FormatPage stamps it, and ValidatePageHeader then reads
    // `version == 0` as Corruption, so a page class missing from that switch
    // formats itself into a state its own validator rejects.
    //
    // Written over the whole assigned range rather than per type, because
    // that is what also catches the next append to the enum - and what would
    // survive someone giving the switch a `default:`, which would silence
    // -Wswitch and put the class of bug back.
    for (std::uint8_t raw = 1; raw <= kMaxAssignedPageType; ++raw) {
        EXPECT_NE(MaxSupportedFormatVersion(static_cast<PageType>(raw)), 0)
            << "page_type " << static_cast<int>(raw) << " has no format version";
    }
    EXPECT_EQ(MaxSupportedFormatVersion(PageType::kInvalid), 0)
        << "an unformatted page has no layout to version";
}

TEST(PageHeaderTest, FormatPageZeroesEverythingAndSetsCurrentVersion) {
    Page page{};
    std::memset(page.data(), 0xFF, page.size());

    FormatPage(Mut(page), PageType::kFreeMap, 0x0007);
    const PageHeaderFields fields = ReadPageHeader(Const(page));

    EXPECT_EQ(fields.page_type, static_cast<std::uint8_t>(PageType::kFreeMap));
    EXPECT_EQ(fields.format_version, MaxSupportedFormatVersion(PageType::kFreeMap));
    EXPECT_EQ(fields.flags, 0x0007);
    EXPECT_EQ(fields.checksum, 0u);
    EXPECT_EQ(fields.page_lsn, kNoPageLsn);
    EXPECT_EQ(fields.relayout_epoch, 0u);
    EXPECT_EQ(fields.owner_oid, 0u);
    for (std::size_t i = kPageBodyOffset; i < kPageSize; ++i) {
        ASSERT_EQ(page[i], std::byte{0}) << "body byte " << i;
    }
}

TEST(PageHeaderTest, FormatPageStampsOwnerOid) {
    Page page{};
    FormatPage(Mut(page), PageType::kHeap, /*flags=*/0, /*owner_oid=*/4001);

    EXPECT_EQ(GetOwnerOid(Const(page)), 4001u);
    // The stamp lives at the reserved1-era offset, so a pre-§2a page - all
    // zeroes there - reads back as unattributed through the same accessor.
    Page legacy{};
    FormatPage(Mut(legacy), PageType::kHeap);
    EXPECT_EQ(GetOwnerOid(Const(legacy)), 0u);
}

TEST(PageHeaderTest, ValidateRejectsUnformattedPage) {
    Page page{};  // all zeroes, i.e. page_type 0 - also what a sparse page reads as
    const Status status = ValidatePageHeader(Const(page));
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kCorruption);
}

TEST(PageHeaderTest, ValidateRejectsUnknownPageType) {
    Page page{};
    FormatPage(Mut(page), PageType::kHeap);
    page[kPageTypeOffset] = static_cast<std::byte>(kMaxAssignedPageType + 1);

    const Status status = ValidatePageHeader(Const(page));
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kCorruption);
}

// The version-bump gate (page.md section 18-1): a page written by a newer
// build must be refused, not misparsed.
TEST(PageHeaderTest, ValidateRejectsFutureFormatVersion) {
    Page page{};
    FormatPage(Mut(page), PageType::kHeap);
    page[kFormatVersionOffset] =
        static_cast<std::byte>(MaxSupportedFormatVersion(PageType::kHeap) + 1);

    const Status status = ValidatePageHeader(Const(page));
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kCorruption);
}

TEST(PageHeaderTest, ValidateChecksExpectedType) {
    Page page{};
    FormatPage(Mut(page), PageType::kBtreeLeaf);

    EXPECT_TRUE(ValidatePageHeader(Const(page), PageType::kBtreeLeaf).ok());
    const Status mismatch = ValidatePageHeader(Const(page), PageType::kBtreeInternal);
    EXPECT_FALSE(mismatch.ok());
    EXPECT_EQ(mismatch.code(), StatusCode::kCorruption);
}

TEST(PageHeaderTest, PageLsnAccessorsRoundTrip) {
    Page page{};
    FormatPage(Mut(page), PageType::kUndo);
    EXPECT_EQ(GetPageLsn(Const(page)), kNoPageLsn);

    SetPageLsn(Mut(page), 0xAABBCCDD11223344ULL);
    EXPECT_EQ(GetPageLsn(Const(page)), 0xAABBCCDD11223344ULL);
}

TEST(PageHeaderTest, StampedChecksumVerifies) {
    Page page{};
    FormatPage(Mut(page), PageType::kHeap);
    std::memset(page.data() + kPageBodyOffset, 0x42, 100);

    StampPageChecksum(Mut(page));
    EXPECT_NE(GetStoredChecksum(Const(page)), 0u);
    EXPECT_TRUE(VerifyPageChecksum(Const(page)).ok());
}

TEST(PageHeaderTest, ChecksumIsIndependentOfItsOwnStoredValue) {
    Page page{};
    FormatPage(Mut(page), PageType::kHeap);
    const std::uint32_t before = ComputePageChecksum(Const(page));

    StampPageChecksum(Mut(page));
    // Recomputing after the field was filled in must give the same answer -
    // that is what "computed with the field zeroed" buys, and what makes
    // Verify() work at all.
    EXPECT_EQ(ComputePageChecksum(Const(page)), before);
}

TEST(PageHeaderTest, VerifyDetectsBodyCorruption) {
    Page page{};
    FormatPage(Mut(page), PageType::kHeap);
    StampPageChecksum(Mut(page));

    page[kPageSize - 1] ^= std::byte{0x01};

    const Status status = VerifyPageChecksum(Const(page));
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kCorruption);
}

TEST(PageHeaderTest, VerifyDetectsHeaderCorruption) {
    Page page{};
    FormatPage(Mut(page), PageType::kHeap);
    StampPageChecksum(Mut(page));

    SetPageLsn(Mut(page), 99);  // a real mutation that forgot to restamp

    const Status status = VerifyPageChecksum(Const(page));
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kCorruption);
}

TEST(PageHeaderTest, UnformattedPageTypeHasNoSupportedVersion) {
    EXPECT_EQ(MaxSupportedFormatVersion(PageType::kInvalid), 0u);
}

// ---- Relayout epoch (docs/spec/physical-optimizer.md R4, workplan PX03) --

TEST(PageHeaderTest, RelayoutEpochRoundTripsAndBumpsByOne) {
    Page page{};
    FormatPage(Mut(page), PageType::kHeap);
    EXPECT_EQ(GetRelayoutEpoch(Const(page)), 0u);

    SetRelayoutEpoch(Mut(page), 41);
    EXPECT_EQ(GetRelayoutEpoch(Const(page)), 41u);

    BumpRelayoutEpoch(Mut(page));
    EXPECT_EQ(GetRelayoutEpoch(Const(page)), 42u);
    BumpRelayoutEpoch(Mut(page));
    EXPECT_EQ(GetRelayoutEpoch(Const(page)), 43u);
}

// The no-format-bump claim, tested rather than asserted: a page image laid
// out by the pre-change build is byte-identical to one this build formats
// (the field took over `reserved0`, which every writer zeroed), so the
// closest a single binary can get is writing the old layout by hand — each
// field at its old offset, zeroes at 16 — and reading the epoch through
// the new accessor. Same version, same offsets, epoch 0.
TEST(PageHeaderTest, APreEpochPageImageReadsEpochZeroWithoutAFormatBump) {
    Page page{};
    const auto store = [&](std::size_t offset, auto value) {
        std::memcpy(page.data() + offset, &value, sizeof(value));
    };
    store(kPageTypeOffset, static_cast<std::uint8_t>(PageType::kHeap));
    store(kFormatVersionOffset, std::uint8_t{1});
    store(kPageFlagsOffset, std::uint16_t{0});
    store(kPageChecksumOffset, std::uint32_t{0});
    store(kPageLsnOffset, std::uint64_t{7});
    // Offsets 16..31: the pre-change build's two reserved words, zero.

    EXPECT_TRUE(ValidatePageHeader(Const(page), PageType::kHeap).ok());
    EXPECT_EQ(ReadPageHeader(Const(page)).format_version,
              MaxSupportedFormatVersion(PageType::kHeap));
    EXPECT_EQ(GetRelayoutEpoch(Const(page)), 0u);
}

// The epoch is inside the checksummed span: a bump that forgot to restamp
// must be *detected*, exactly as a forgotten page_lsn restamp is — which
// is the proof the field is covered, not exempted.
TEST(PageHeaderTest, TheEpochIsInsideTheChecksumSpan) {
    Page page{};
    FormatPage(Mut(page), PageType::kHeap);
    StampPageChecksum(Mut(page));
    ASSERT_TRUE(VerifyPageChecksum(Const(page)).ok());

    BumpRelayoutEpoch(Mut(page));
    const Status stale = VerifyPageChecksum(Const(page));
    EXPECT_FALSE(stale.ok());
    EXPECT_EQ(stale.code(), StatusCode::kCorruption);

    StampPageChecksum(Mut(page));
    EXPECT_TRUE(VerifyPageChecksum(Const(page)).ok());
    EXPECT_EQ(GetRelayoutEpoch(Const(page)), 1u);
}

}  // namespace
}  // namespace kds::storage
