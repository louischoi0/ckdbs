#pragma once

#include <cstddef>
#include <cstdint>

// Shared primitive types/constants (KDS-DESIGN.md invariants 1, 6, 7).

namespace kds {

// Page IDs are unsigned 32-bit, never signed (16 TB target capacity =
// 2^31 pages, half the u32 space; 0xFFFFFFFF is reserved invalid).
using PageId = std::uint32_t;
inline constexpr PageId kInvalidPageId = 0xFFFFFFFFu;

inline constexpr std::size_t kPageSize = 8192;

// Design ceiling from docs/page.md section 4: page_id is u32 with 2^31
// target pages, i.e. half the u32 space, giving a 16 TiB single-file
// ceiling. Asserted rather than merely documented so the arithmetic
// mapping (file_offset = page_id * kPageSize) can never silently overflow
// the intended range.
inline constexpr std::uint32_t kMaxPageCount = 1u << 31;
inline constexpr std::uint64_t kMaxFileBytes =
    static_cast<std::uint64_t>(kMaxPageCount) * kPageSize;
static_assert(kMaxFileBytes == 16ULL * 1024 * 1024 * 1024 * 1024);
static_assert(kInvalidPageId >= kMaxPageCount, "invalid sentinel must sit outside valid ids");

// On-disk page type discriminator, stored in the common page header's
// first byte (docs/page.md section 2). The enum is **frozen and
// append-only**: values are persisted, so an existing value's meaning may
// never change and a retired value's number is never reused. 0 means
// invalid/unformatted, which is what a freshly zeroed (or sparse, never
// written) page reads back as.
//
// Only "headered" page classes appear here. Waystone entry and directory
// pages are headerless by design (docs/page.md section 1) - their tilings
// are exact powers of two and a header would break the shift/mask
// addressing - so they carry no page_type at all.
enum class PageType : std::uint8_t {
    kInvalid = 0,
    kHeap = 1,
    kBtreeInternal = 2,
    kBtreeLeaf = 3,
    kUndo = 4,
    kCatalog = 5,
    kSuperBlock = 6,
    kFreeMap = 7,
};

// Highest value currently assigned above; anything greater read off disk
// was written by a newer build. Bump when appending to the enum.
inline constexpr std::uint8_t kMaxAssignedPageType = 7;

}  // namespace kds
