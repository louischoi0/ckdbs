#pragma once

#include <cstddef>
#include <cstdint>

// Shared primitive types/constants (docs/spec/heap-and-tuple.md invariants 1, 6, 7).

namespace kds {

// Page IDs are unsigned 32-bit, never signed (16 TB target capacity =
// 2^31 pages, half the u32 space; 0xFFFFFFFF is reserved invalid).
using PageId = std::uint32_t;
inline constexpr PageId kInvalidPageId = 0xFFFFFFFFu;

inline constexpr std::size_t kPageSize = 8192;

// Design ceiling from docs/spec/page.md section 4: page_id is u32 with 2^31
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
// first byte (docs/spec/page.md section 2). The enum is **frozen and
// append-only**: values are persisted, so an existing value's meaning may
// never change and a retired value's number is never reused. 0 means
// invalid/unformatted, which is what a freshly zeroed (or sparse, never
// written) page reads back as.
//
// Only "headered" page classes appear here. A page class whose payload
// tiles the page exactly (docs/spec/page.md section 1) - a power-of-two entry
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
    // (docs/spec/waystone-concpets.md). Headered like every type above it -
    // nothing in a trail is addressed by shift and mask, so the payload
    // does not have to tile the page and the page keeps its checksum and
    // page_lsn.
    kWaystone = 9,

    // The var-heap: the out-of-line store for values too long to fit in a
    // tuple's fixed-width tagged cell (docs/spec/heap-and-tuple.md section 3.4).
    //
    // Headered, and the reason is worth stating because the recent reflex
    // runs the other way: waystone and trail pages are *advisory*, but a
    // var-heap value is committed data. Losing one loses a value, not a
    // hint, so this class is checksummed, carries a page_lsn and is fully
    // WAL-logged - an ordinary authoritative page class.
    //
    // Also relayout-exempt by construction: its values are immutable per
    // version, so the physical optimizer never has a reason to move one.
    kVarHeap = 10,

    // A secondary index (docs/spec/index.md §4): the internal nodes that
    // route a descent, and the leaves that hold the entries.
    //
    // **Not kBtreeInternal/kBtreeLeaf, and the distinction is the design.**
    // Those two are the *clustered* tree - a relation's storage, whose
    // leaves are heap pages holding tuples, and whose split deliberately
    // never divides a page's contents because doing so would decide the
    // open heap page split policy. A secondary key is not monotonic, so a
    // dividing split is mandatory here; giving these their own classes is
    // what lets that split decide nothing beyond itself. An index page
    // holds entries rather than tuples, has no min_key, and contains no
    // Keystone id, so invariants 2 and 3 have nothing to be about in it.
    //
    // Headered and checksummed like any authoritative class: an index is
    // maintained on every write and a missing entry is a lost row, not a
    // lost hint. Advisory rules do not apply.
    kIndexInternal = 11,
    kIndexLeaf = 12,

    // A Bound Cabin's entry pages (docs/spec/assertion.md §5,
    // docs/spec/cabin.md §12, workplan AST04).
    //
    // **Its own class rather than a subtype flag on a Cabin page**, and the
    // reason is `docs/spec/eviction.md` EV3: pinning is a page-class
    // attribute resolved from the page kind, and a subtype flag would put
    // the answer one indirection past where the sweep can cheaply ask. It
    // is also what makes an observational Cabin's page - if one is ever
    // made durable (cabin.md §11 leaves that open) - a *different*
    // class with a different lifecycle, which §12's class table requires.
    //
    // Headered, checksummed and authoritative: the aggregate a group header
    // carries is what an admission check reads, so losing a page loses the
    // constraint rather than a hint. Advisory rules do not apply, exactly
    // as they do not for kVarHeap.
    kCabinBound = 13,

    // A relation's anchor: the one fixed page holding its entry points -
    // the clustered root, and each secondary index's root keyed by index
    // oid (workplan-peer-writer.md §7a, PW2's decision). It exists so a
    // root move writes a *relation* page, never a catalog page: the
    // catalog columns become CREATE-fixed facts naming this page, and the
    // page's owner - whichever core that is - moves its own roots in its
    // own stream. Headered, checksummed and authoritative: losing it
    // loses the relation's entry points, kVarHeap's argument exactly.
    kAnchor = 14,
};

// Highest value currently assigned above; anything greater read off disk
// was written by a newer build. Bump when appending to the enum.
inline constexpr std::uint8_t kMaxAssignedPageType = 14;

}  // namespace kds
