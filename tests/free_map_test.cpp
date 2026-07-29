#include "kds/storage/free_map.hpp"

#include <array>

#include <gtest/gtest.h>

#include "kds/storage/page_header.hpp"

namespace kds::storage {
namespace {

using Page = std::array<std::byte, kPageSize>;

std::span<std::byte, kPageSize> Mut(Page& p) { return std::span<std::byte, kPageSize>(p); }
std::span<const std::byte, kPageSize> Const(const Page& p) {
    return std::span<const std::byte, kPageSize>(p);
}

Page FormattedPage() {
    Page page{};
    // Pre-fill with garbage so the test proves Format zeroes the bitmap
    // rather than inheriting a conveniently-empty buffer.
    page.fill(std::byte{0xAB});
    FormatFreeMapPage(Mut(page));
    return page;
}

TEST(FreeMapTest, FormatProducesAnEmptyValidatableMap) {
    Page page = FormattedPage();

    EXPECT_EQ(RawPageType(Const(page)), static_cast<std::uint8_t>(PageType::kFreeMap));
    EXPECT_EQ(FreeMapCountAllocated(Const(page)), 0u);
    EXPECT_EQ(FreeMapFindFirstFree(Const(page), 0), 0u);

    StampPageChecksum(Mut(page));
    EXPECT_TRUE(ValidateFreeMapPage(Const(page)).ok());
}

TEST(FreeMapTest, ValidateRejectsCorruptionAndWrongType) {
    Page page = FormattedPage();
    StampPageChecksum(Mut(page));

    Page flipped = page;
    flipped[kPageBodyOffset + 100] ^= std::byte{0x01};
    EXPECT_EQ(ValidateFreeMapPage(Const(flipped)).code(), StatusCode::kCorruption);

    Page heap{};
    FormatPage(Mut(heap), PageType::kHeap);
    StampPageChecksum(Mut(heap));
    EXPECT_EQ(ValidateFreeMapPage(Const(heap)).code(), StatusCode::kCorruption);
}

TEST(FreeMapTest, AllocateSetsExactlyTheRequestedBits) {
    Page page = FormattedPage();

    FreeMapAllocate(Mut(page), 0);
    FreeMapAllocate(Mut(page), 1);
    FreeMapAllocate(Mut(page), 9);
    FreeMapAllocate(Mut(page), kFreeMapBitsPerPage - 1);

    EXPECT_TRUE(FreeMapIsAllocated(Const(page), 0));
    EXPECT_TRUE(FreeMapIsAllocated(Const(page), 1));
    EXPECT_TRUE(FreeMapIsAllocated(Const(page), 9));
    EXPECT_TRUE(FreeMapIsAllocated(Const(page), kFreeMapBitsPerPage - 1));
    EXPECT_FALSE(FreeMapIsAllocated(Const(page), 2));
    EXPECT_FALSE(FreeMapIsAllocated(Const(page), 8));
    EXPECT_EQ(FreeMapCountAllocated(Const(page)), 4u);

}

// The persisted addressing rule itself (header comment): bit `index` is
// bit (index & 7) of body byte (index >> 3), LSB-first. A bitfield-based
// implementation could pass every behavioural test above and still fail
// this one on another compiler, which is why invariant 5 forbids them.
TEST(FreeMapTest, BitAddressingIsExplicitAndLsbFirst) {
    Page page = FormattedPage();

    FreeMapAllocate(Mut(page), 0);
    EXPECT_EQ(page[kPageBodyOffset], std::byte{0x01});

    FreeMapAllocate(Mut(page), 7);
    EXPECT_EQ(page[kPageBodyOffset], std::byte{0x81});

    FreeMapAllocate(Mut(page), 8);
    EXPECT_EQ(page[kPageBodyOffset + 1], std::byte{0x01});

    // Nothing outside the body ever moves - the header stays intact.
    EXPECT_EQ(RawPageType(Const(page)), static_cast<std::uint8_t>(PageType::kFreeMap));
    EXPECT_EQ(GetPageLsn(Const(page)), kNoPageLsn);
}

TEST(FreeMapTest, FindFirstFreeSkipsAllocatedRunsFromAnyStart) {
    Page page = FormattedPage();

    // Allocate [0, 20) so the search has to cross whole 0xFF bytes and
    // land mid-byte.
    for (std::uint32_t i = 0; i < 20; ++i) FreeMapAllocate(Mut(page), i);

    EXPECT_EQ(FreeMapFindFirstFree(Const(page), 0), 20u);
    EXPECT_EQ(FreeMapFindFirstFree(Const(page), 5), 20u);   // unaligned start
    EXPECT_EQ(FreeMapFindFirstFree(Const(page), 20), 20u);  // already free
    EXPECT_EQ(FreeMapFindFirstFree(Const(page), 64), 64u);  // past the run
}

TEST(FreeMapTest, FullPageHasNoFreeBit) {
    Page page = FormattedPage();
    for (std::uint32_t i = 0; i < kFreeMapBitsPerPage; ++i) FreeMapAllocate(Mut(page), i);

    EXPECT_EQ(FreeMapCountAllocated(Const(page)), kFreeMapBitsPerPage);
    EXPECT_FALSE(FreeMapFindFirstFree(Const(page), 0).has_value());
}

// Out-of-range indexes must not touch a neighbouring bit: reads report
// allocated, writes do nothing at all.
TEST(FreeMapTest, OutOfRangeIndexIsInertNotWrapping) {
    Page page = FormattedPage();
    const Page before = page;

    EXPECT_TRUE(FreeMapIsAllocated(Const(page), kFreeMapBitsPerPage));
    EXPECT_TRUE(FreeMapIsAllocated(Const(page), kFreeMapBitsPerPage + 1));

    FreeMapAllocate(Mut(page), kFreeMapBitsPerPage);
    FreeMapAllocate(Mut(page), kFreeMapBitsPerPage + 12345);
    EXPECT_EQ(page, before);

    EXPECT_FALSE(FreeMapFindFirstFree(Const(page), kFreeMapBitsPerPage).has_value());
}

}  // namespace
}  // namespace kds::storage
