#pragma once

#include <cstddef>
#include <cstdint>

// Shared primitive types/constants (docs/heap-and-tuple.md invariants 1, 6, 7).

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
// Only "headered" page classes appear here. A page class whose payload
// tiles the page exactly (docs/page.md section 1) - a power-of-two entry
// array addressed by shift and mask - is headerless by design and carries
// no page_type at all. The waystone directory's interior pages are the one
// such class today; see kHeaderlessMap below.
enum class PageType : std::uint8_t {
    kInvalid = 0,
    kHeap = 1,
    kBtreeInternal = 2,
    kBtreeLeaf = 3,
    kUndo = 4,
    kCatalog = 5,
    kSuperBlock = 6,
    kFreeMap = 7,
    // Bitmap of which page ids are *headerless*: pages whose exact
    // power-of-two tiling leaves no room for a common header. Same
    // one-bit-per-page-id format as kFreeMap; see free_map.hpp. One page
    // class claims it: the waystone directory's interior pages, whose 2048
    // child ids tile the page exactly (stats/waystone_dir.hpp).
    //
    // It has to be durable rather than a side table: DevicePageStore never
    // evicts, so a page is read back from the device exactly once - on
    // first touch after open - and that is precisely when a checksum
    // verify would reject a page that never carried one.
    kHeaderlessMap = 8,

    // A waystone: the recorded trail of one pattern instance
    // (docs/waystone-concpets.md). Headered like every type above it -
    // nothing in a trail is addressed by shift and mask, so the payload
    // does not have to tile the page and the page keeps its checksum and
    // page_lsn.
    kWaystone = 9,
};

// Highest value currently assigned above; anything greater read off disk
// was written by a newer build. Bump when appending to the enum.
inline constexpr std::uint8_t kMaxAssignedPageType = 9;

}  // namespace kds
