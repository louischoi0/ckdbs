#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/page_header.hpp"

// The allocation bitmap page (PageType::kFreeMap, docs/spec/page.md section 5):
// one bit per page id, 1 = allocated. This is what makes "which page ids
// exist" a durable fact rather than an in-memory side table.
//
// Two concerns, in this order: the codec for one bitmap page, then the
// placement arithmetic that says which bitmap page covers a given id.
// They live together because the region size *is* the per-page coverage
// constant - one file, one answer to "id -> (map page, bit)".
//
// The ALLOC/FREE WAL records remain page.md section 5's unbuilt work (D9
// of docs/inflight/in-progress/workplan-multi-free-map.md); the map is unlogged and repaired
// at recovery by RaiseAllocationFloor.
//
// Bit addressing is explicit shift/mask over the page body - a persisted
// format, so no bitfields (invariant 5):
//
//   byte = kPageBodyOffset + (index >> 3)
//   bit  = index & 7          (LSB-first, so "lowest free bit in a byte"
//                              is countr_one of that byte)

namespace kds::storage {

// (8192 - 32) * 8 = 65,280 page ids, i.e. 510 MiB of data file.
inline constexpr std::uint32_t kFreeMapBitsPerPage =
    static_cast<std::uint32_t>(kPageBodySize) * 8;
static_assert(kFreeMapBitsPerPage == 65280);

// Common header, every bit clear. The checksum is stamped by the owner at
// write-out time (page.md section 8), not here.
//
// `type` exists because a second bitmap of the same shape now lives beside
// the allocation map: PageType::kHeaderlessMap, one bit per page id saying
// whether that page carries a common header at all. Identical addressing,
// identical helpers, different meaning - so the format is shared and only
// the header's type byte distinguishes them on disk.
void FormatFreeMapPage(std::span<std::byte, kPageSize> page,
                       PageType type = PageType::kFreeMap);

// Checksum verifies and the header is a bitmap page of `type` this build
// can parse. Either failure is Corruption.
Status ValidateFreeMapPage(std::span<const std::byte, kPageSize> page,
                           PageType type = PageType::kFreeMap);

// An index at or above kFreeMapBitsPerPage reads as allocated and ignores
// writes: callers range-check and report OutOfRange with their own
// context, and a missed check cannot corrupt a neighbouring page's bit.
// Under the multi-page map that guard means "outside *this page*", not
// "outside the id space" - callers pass FreeMapBitIndexOf(id), which is
// in range by construction, and the fail-closed shape survives only as a
// backstop against a caller that forgot.
bool FreeMapIsAllocated(std::span<const std::byte, kPageSize> page, std::uint32_t index) noexcept;
void FreeMapAllocate(std::span<std::byte, kPageSize> page, std::uint32_t index) noexcept;

// Lowest clear bit at or above `from`, or nullopt if this page has none.
std::optional<std::uint32_t> FreeMapFindFirstFree(std::span<const std::byte, kPageSize> page,
                                                  std::uint32_t from) noexcept;

// Set bits. O(page); metering and tests, not a hot path.
std::uint32_t FreeMapCountAllocated(std::span<const std::byte, kPageSize> page) noexcept;


// ---------------------------------------------------------------------
// Placement: which bitmap pages cover an id (docs/spec/page.md section 5)
// ---------------------------------------------------------------------
//
// D1 of docs/inflight/in-progress/workplan-multi-free-map.md section 7, settled 2026-08-26 as
// that section's candidate A. Region N is the ids
// [N * kFreeMapBitsPerPage, (N+1) * kFreeMapBitsPerPage), and its two
// bitmaps are the first two ids inside it:
//
//   region(id)                = id / kFreeMapBitsPerPage
//   free map of region N      = N * kFreeMapBitsPerPage + 1
//   headerless map of region N = N * kFreeMapBitsPerPage + 2
//
// Region 0 therefore yields 1 and 2 - the fixed ids this engine has
// always used - so a database that fits in one region is byte-identical
// and there is no superblock version bump and no migration. That +1/+2
// is the scheme's one deliberate off-by-one: it exists to leave page 0 to
// the superblock, and it is written here, once, rather than re-derived.
//
// Two properties the rest of the work leans on, both by construction:
//
//  - A map page's own bits sit inside its own region (bit 1 and bit 2 of
//    that region's free map), so creating the first page of a new region
//    never has to ask another map page where to put it.
//  - A page's class follows from its id alone. IsMapPageId is what breaks
//    the apparent recursion in ResidentBytes/StampIfHeadered, which ask
//    IsHeaderless on the fault and write-back paths: a map page is
//    headered, always, and must be answered without reading a map.
//
// Domain: page_id < kMaxPageCount. The multiply runs in 64 bits so a
// caller that skipped its range check cannot wrap into a valid-looking
// id; an out-of-domain id yields a meaningless result, not UB. At the top
// of the id space the arithmetic still fits - see the static_assert
// below.

// The region an id belongs to. Also the count of regions below it.
inline constexpr std::uint32_t FreeMapRegionOf(PageId page_id) noexcept {
    return page_id / kFreeMapBitsPerPage;
}

// The first id of a region - the inverse of FreeMapRegionOf, and what
// turns a bit index back into an id. Here rather than at the two call
// sites that want it, because re-deriving `region * coverage` elsewhere is
// exactly the duplication this section exists to prevent.
inline constexpr PageId FreeMapRegionBase(std::uint32_t region) noexcept {
    return static_cast<PageId>(static_cast<std::uint64_t>(region) * kFreeMapBitsPerPage);
}

// The free-map page covering `page_id`.
inline constexpr PageId FreeMapPageIdFor(PageId page_id) noexcept {
    return static_cast<PageId>(
        static_cast<std::uint64_t>(FreeMapRegionOf(page_id)) * kFreeMapBitsPerPage + 1);
}

// The headerless-map page covering `page_id`, always the free map's
// immediate successor - adjacency is a consequence of the scheme, not a
// convention some other site could break.
inline constexpr PageId HeaderlessMapPageIdFor(PageId page_id) noexcept {
    return FreeMapPageIdFor(page_id) + 1;
}

// The bit index of `page_id` within either of its two map pages. Named
// for the map rather than left bare: kds::storage has no shortage of bit
// indices, and this one is only ever an argument to the helpers above.
inline constexpr std::uint32_t FreeMapBitIndexOf(PageId page_id) noexcept {
    return page_id % kFreeMapBitsPerPage;
}

// True for either bitmap of any region. Both classes are headered, so
// this is also the arithmetic answer to "is this page headerless" - no.
inline constexpr bool IsMapPageId(PageId page_id) noexcept {
    const std::uint32_t within = FreeMapBitIndexOf(page_id);
    return within == 1 || within == 2;
}

// The top of the id space still maps to a map page inside it: nothing
// here can produce an id at or above the design ceiling, which is what
// keeps kInvalidPageId unambiguous.
static_assert(FreeMapPageIdFor(kMaxPageCount - 1) < kMaxPageCount);
static_assert(HeaderlessMapPageIdFor(kMaxPageCount - 1) < kMaxPageCount);

// Region and bit index are a lossless split of an id, which is what lets
// the store key its cache by region and address bits within a page.
static_assert(FreeMapRegionBase(FreeMapRegionOf(0)) + FreeMapBitIndexOf(0) == 0);
static_assert(FreeMapRegionBase(FreeMapRegionOf(kMaxPageCount - 1)) +
                  FreeMapBitIndexOf(kMaxPageCount - 1) ==
              kMaxPageCount - 1);
static_assert(FreeMapRegionBase(1) == kFreeMapBitsPerPage);

}  // namespace kds::storage
