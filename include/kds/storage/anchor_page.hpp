#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/storage/page_header.hpp"

// The relation anchor page (PageType::kAnchor; background: PW2,
// workplan-peer-writer.md §7a): one fixed page per relation holding its
// entry points, so that a root move writes a relation page and never a
// catalog page - the `sys.tables` row is CREATE-fixed.
//
// Layout, packed at kPageBodyOffset, every field read and written through
// explicit offsets and memcpy (invariant 6's discipline - no overlay
// structs on persisted bytes):
//
//   clustered_root  u32   the clustered tree's / heap chain's root
//   nr_index        u16   live index entries below
//   reserved        u16   0
//   entries[]             {index_oid u64, root u32} per secondary index
//
// The entry table is append-ordered and linear-scanned: an index count is
// bounded by DDL, not by data, and the scan runs at bind time, not per
// row. A slot is removed only once its tree is reclaimed
// (`RemoveAnchorIndexSlot`, BY-R5).
//
// Mutation protocol: under the page latch, like every relation page - any
// core's statement may write it. Every mutation is WAL-logged by the caller
// (`ANCHOR_UPDATE`) and stamped through StampPageLsn; this header only
// moves bytes.

namespace kds::storage {

inline constexpr std::size_t kAnchorClusteredRootOffset = kPageBodyOffset;
inline constexpr std::size_t kAnchorNrIndexOffset = kAnchorClusteredRootOffset + 4;
inline constexpr std::size_t kAnchorReservedOffset = kAnchorNrIndexOffset + 2;
inline constexpr std::size_t kAnchorEntriesOffset = kAnchorReservedOffset + 2;
inline constexpr std::size_t kAnchorEntrySize = 12;  // index_oid u64 + root u32
inline constexpr std::size_t kAnchorMaxIndexEntries =
    (kPageSize - kAnchorEntriesOffset) / kAnchorEntrySize;

// Formats `page` as a fresh anchor for `owner_oid` with the clustered
// root set and no index entries.
void FormatAnchorPage(std::span<std::byte, kPageSize> page, std::uint64_t owner_oid,
                      PageId clustered_root);

PageId AnchorClusteredRoot(std::span<const std::byte, kPageSize> page);
void SetAnchorClusteredRoot(std::span<std::byte, kPageSize> page, PageId root);

// `nr_index` duplicates a schema constant, so every reader treats it as
// **checked redundancy** (docs/rules/rules.md): compared against the bound,
// Corruption on disagreement, never computed from - a forged count is an
// out-of-bounds read or write otherwise (3f07eda's review, C1).

// Every `{index_oid, root}` slot the anchor holds, in slot order - a
// dropped index's and a rolled-back one's included until its tree is
// reclaimed and the slot removed (BY-R5). What a dropped relation's reclaim
// walks from (BF-R3); Corruption when the page's count is not a count.
struct AnchorIndexSlot {
    std::uint64_t index_oid = 0;
    PageId root = kInvalidPageId;
};
StatusOr<std::vector<AnchorIndexSlot>> AnchorIndexSlots(std::span<const std::byte, kPageSize> page);

// The root recorded for `index_oid`, kInvalidPageId when the anchor holds
// no entry for it, or Corruption when the page's count is not a count.
StatusOr<PageId> AnchorIndexRoot(std::span<const std::byte, kPageSize> page,
                                 std::uint64_t index_oid);

// Whether the anchor could take `index_oid`'s entry: OK when it already
// holds one (an update needs no slot), ResourceExhausted past
// kAnchorMaxIndexEntries, Corruption on a forged count. Exactly what
// SetAnchorIndexRoot would answer, and the same message - one
// implementation, two callers.
//
// It is separate so the refusal can be asked for **before** a caller
// allocates the pages it would then record here: `CREATE INDEX` builds the
// whole tree and seeds the anchor afterwards, and a tree no anchor slot
// names is never reclaimed (BF §0) - leaked for the life of the database.
Status CheckAnchorRoomForIndex(std::span<const std::byte, kPageSize> page,
                               std::uint64_t index_oid);

// Inserts or updates the entry for `index_oid`. Refuses exactly as
// CheckAnchorRoomForIndex does. The slot is written at CREATE INDEX
// (PW2-3), so at the cap the create itself is refused.
Status SetAnchorIndexRoot(std::span<std::byte, kPageSize> page, std::uint64_t index_oid,
                          PageId root);

// Removes `index_oid`'s entry and compacts the table (BY-R5,
// `instructions/v3.2.0/workorder-by-reindex-drop-index-reclaim.md`):
// `NotFound` when the anchor holds none. Written once a dropped or orphaned
// index's tree is reclaimed and every free of it durable, by its own record
// (`ANCHOR_SLOT_REMOVE`), so redo meets it after every `ANCHOR_UPDATE` that
// named the oid - the page LSN gates each - and nothing writes the oid's slot
// again: its row is gone, so no write maintains its tree. Before BY no slot
// was ever removed, and ~679 create-then-drop cycles filled the table.
Status RemoveAnchorIndexSlot(std::span<std::byte, kPageSize> page, std::uint64_t index_oid);

}  // namespace kds::storage
