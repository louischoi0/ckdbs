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
    written.reserved0 = 0xFEDCBA9876543210ULL;
    written.reserved1 = 0x1122334455667788ULL;

    WritePageHeader(Mut(page), written);
    const PageHeaderFields read = ReadPageHeader(Const(page));

    EXPECT_EQ(read.page_type, written.page_type);
    EXPECT_EQ(read.format_version, written.format_version);
    EXPECT_EQ(read.flags, written.flags);
    EXPECT_EQ(read.checksum, written.checksum);
    EXPECT_EQ(read.page_lsn, written.page_lsn);
    EXPECT_EQ(read.reserved0, written.reserved0);
    EXPECT_EQ(read.reserved1, written.reserved1);
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
    EXPECT_EQ(fields.reserved0, 0u);
    EXPECT_EQ(fields.reserved1, 0u);
    for (std::size_t i = kPageBodyOffset; i < kPageSize; ++i) {
        ASSERT_EQ(page[i], std::byte{0}) << "body byte " << i;
    }
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

}  // namespace
}  // namespace kds::storage
