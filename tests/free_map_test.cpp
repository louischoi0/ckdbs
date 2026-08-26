#include "kds/storage/free_map.hpp"

#include <array>
#include <vector>

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


// ---- Placement arithmetic (FM1, docs/inflight/in-progress/workplan-multi-free-map.md) ----

// Ids worth checking: the two region heads either side of a boundary, the
// map pages themselves, the last id of a region, and the top of the id
// space. Sampled rather than exhaustive - 2^31 ids is not a unit test.
std::vector<PageId> PlacementProbeIds() {
    std::vector<PageId> ids{0, 1, 2, 3, 127, 128, 1000};
    for (std::uint32_t region : {0u, 1u, 2u, 17u, 32895u}) {
        const std::uint64_t base = std::uint64_t{region} * kFreeMapBitsPerPage;
        for (std::uint64_t delta :
             {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{3},
              std::uint64_t{kFreeMapBitsPerPage} - 1}) {
            const std::uint64_t id = base + delta;
            if (id < kMaxPageCount) ids.push_back(static_cast<PageId>(id));
        }
    }
    ids.push_back(kMaxPageCount - 1);
    return ids;
}

TEST(FreeMapPlacementTest, EveryIdMapsToExactlyOneMapPageAndBit) {
    for (PageId id : PlacementProbeIds()) {
        const PageId map = FreeMapPageIdFor(id);
        const std::uint32_t bit = FreeMapBitIndexOf(id);

        // Exactness: the pair reconstructs the id, so no two ids can share
        // one (map page, bit) and none is left uncovered. The map page is
        // the region head plus one, hence the -1.
        ASSERT_LT(bit, kFreeMapBitsPerPage) << "id " << id;
        EXPECT_EQ(map - 1 + bit, id) << "id " << id;
        EXPECT_EQ(HeaderlessMapPageIdFor(id), map + 1) << "id " << id;
        EXPECT_LT(map, kMaxPageCount) << "id " << id;
    }
}

TEST(FreeMapPlacementTest, MapPagesFallInsideTheirOwnCoverage) {
    for (PageId id : PlacementProbeIds()) {
        const PageId free_map = FreeMapPageIdFor(id);
        const PageId headerless = HeaderlessMapPageIdFor(id);

        // Both map pages of a region are covered by that region's own free
        // map, at bits 1 and 2 - which is what lets a new region be created
        // without consulting any other map page.
        EXPECT_EQ(FreeMapPageIdFor(free_map), free_map) << "id " << id;
        EXPECT_EQ(FreeMapPageIdFor(headerless), free_map) << "id " << id;
        EXPECT_EQ(FreeMapBitIndexOf(free_map), 1u) << "id " << id;
        EXPECT_EQ(FreeMapBitIndexOf(headerless), 2u) << "id " << id;
    }
}

TEST(FreeMapPlacementTest, IsMapPageIdAgreesWithTheConstructors) {
    for (PageId id : PlacementProbeIds()) {
        const bool is_map = (id == FreeMapPageIdFor(id)) || (id == HeaderlessMapPageIdFor(id));
        EXPECT_EQ(IsMapPageId(id), is_map) << "id " << id;
    }
    // Region 0 is the case every existing test and file already assumes.
    EXPECT_TRUE(IsMapPageId(1));
    EXPECT_TRUE(IsMapPageId(2));
    EXPECT_FALSE(IsMapPageId(0));  // the superblock
    EXPECT_FALSE(IsMapPageId(3));
}

TEST(FreeMapPlacementTest, RegionZeroReproducesTodaysFixedIds) {
    // The compile-time half of this lives at the declarations of
    // kFreeMapPageId / kHeaderlessMapPageId; stated here too because it is
    // the reason an existing single-region database needs no migration.
    EXPECT_EQ(FreeMapPageIdFor(0), 1u);
    EXPECT_EQ(HeaderlessMapPageIdFor(0), 2u);
    EXPECT_EQ(FreeMapRegionOf(kFreeMapBitsPerPage - 1), 0u);
    EXPECT_EQ(FreeMapRegionOf(kFreeMapBitsPerPage), 1u);
    EXPECT_EQ(FreeMapPageIdFor(kFreeMapBitsPerPage), kFreeMapBitsPerPage + 1);
}

}  // namespace
}  // namespace kds::storage
