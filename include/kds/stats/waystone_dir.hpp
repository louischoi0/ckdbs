#pragma once

#include <cstdint>

#include "kds/base/status.hpp"
#include "kds/stats/waystone.hpp"
#include "kds/storage/page_store.hpp"

// The per-relation Waystone page directory (waystone-concpets.md section
// 6): the inode-block-map pattern, turning a pk into the entry page that
// holds it.
//
// Entry pages cannot be physically contiguous - ids arrive in order but
// the store hands out whatever page id is free - so continuity is logical.
// Interior pages hold 2048 child PageIds each; a pk's *logical* entry page
// number (`pk >> 8`) is read as a base-2048 numeral, one digit per level,
// most significant first.
//
//   pk 4096, depth 1:  logical = 4096 >> 8 = 16
//                      root[16] -> leaf page, slot = 4096 & 0xFF = 0
//
//   pk 2^20, depth 2:  logical = 4096
//                      root[4096 >> 11 = 2] -> L1[4096 & 0x7FF = 0] -> leaf
//
// ---- Depth is a property of the relation, not of the pk -----------------
//
// Every walk in one directory uses the same depth, so the same pk resolves
// through the same digits every time. Depth is chosen from the relation's
// id high-water mark (DirDepthFor) and grows by relinking the root
// (GrowDirectory), never by rewriting what is already there - a deeper
// directory reaches the old contents through digit 0 at every new level,
// which is exactly what makes growth O(1) instead of a rebuild.
//
// ---- Lazy allocation ----------------------------------------------------
//
// Unpopulated ranges hold kEmptyDirSlot at every level. A lookup that
// meets one stops and reports kInvalidPageId - a *successful* answer
// meaning "no entry page exists for this range", not an error, because on
// the probe path that is the ordinary case for a pk nobody has inserted.
// Only LookupOrCreate allocates, and only the pages one pk actually needs:
// a sparse id space costs what it touches and nothing more.
//
// ---- These pages are headerless -----------------------------------------
//
// 2048 x 4 bytes tiles 8 KiB exactly, like the 256 x 32 of an entry page,
// and for the same reason: a header would cost a slot and break the
// shift/mask walk. Consequence worth knowing before wiring these into the
// server's store: DevicePageStore::Flush() stamps a checksum into every
// dirty frame at byte offset 4, which on a headerless page is data. See
// the note in waystone-workplan.md; it is a decision, and it belongs to
// W05, not here.
//
// Concurrency: none of its own. Core-local, owned by the relation's owning
// core (rules.md section 3); the caller holds whatever pin/latch
// discipline applies, exactly as with PageView.

namespace kds::stats {

// Smallest depth whose coverage includes `pk`, in [1, kMaxDirDepth]. A pk
// above kMaxPk is a caller bug (invariant 6 range-checks ids at the front
// door) and reports InvalidArgument rather than silently clamping.
StatusOr<int> DirDepthFor(std::uint64_t pk);

// Allocates an empty directory page - every slot kEmptyDirSlot - and
// returns its id. One of these is the root of a new relation's directory;
// the same shape serves at every level.
StatusOr<PageId> CreateDirPage(storage::PageStore& store);

// Resolves `pk` to the entry page holding it, walking `depth` levels from
// `root`. Returns kInvalidPageId when any level holds kEmptyDirSlot: that
// range was never populated, which is a normal answer and not an error.
//
// Fails with InvalidArgument for a depth outside [1, kMaxDirDepth] or a pk
// the depth cannot address, and with whatever the store reports for a
// child id that does not resolve.
StatusOr<PageId> LookupEntryPage(storage::PageStore& store, PageId root, int depth,
                                 std::uint64_t pk);

// Same walk, but allocates every page the path needs - interior levels and
// the leaf - and links each into its parent. Returns the leaf entry page.
//
// A newly allocated leaf is zeroed, so its 256 entries read back with
// flags 0, i.e. not kEntryLive. That is the correct starting state: the
// page exists, and no entry in it means anything yet.
StatusOr<PageId> LookupOrCreateEntryPage(storage::PageStore& store, PageId root, int depth,
                                         std::uint64_t pk);

// Deepens a directory by one level: allocates a new root whose slot 0
// points at `root`, and returns it. The caller raises its stored depth by
// one at the same time.
//
// Correct because the old root covered logical entry pages
// [0, 2048^depth), and at depth+1 those are exactly the numerals whose
// most significant digit is 0. Every prior mapping therefore still
// resolves, through one extra hop, with nothing rewritten. Fails with
// OutOfRange at kMaxDirDepth, which already covers the whole pk space.
StatusOr<PageId> GrowDirectory(storage::PageStore& store, PageId root, int depth);

// Child slot index a walk at `level` (0 = root) uses for `pk`, in a
// directory of `depth` levels. Exposed for tests and for callers that want
// to reason about the walk without performing it.
constexpr std::size_t DirIndexAt(std::uint64_t pk, int depth, int level) noexcept {
    const std::uint64_t logical = LogicalEntryPageOf(pk);
    const int shift = kDirFanoutBits * (depth - 1 - level);
    return static_cast<std::size_t>((logical >> shift) & kDirIndexMask);
}

}  // namespace kds::stats
