#pragma once

#include <cstdint>
#include <span>

#include "kds/base/status.hpp"
#include "kds/storage/page_header.hpp"

// The relation anchor page (PageType::kAnchor; workplan-peer-writer.md
// §7a, PW2's decision): one fixed page per relation holding its entry
// points, so that a root move writes a relation page - owned, granted and
// PL-stamped like any of the relation's pages - and never a catalog page
// only the system core may write.
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
// row. Removal deliberately does not exist yet - the note at the foot of
// this header says why, and what shape it takes when it arrives.
//
// Mutation protocol: single-writer under the owning core's statement
// execution, like every relation page. Every mutation is WAL-logged by
// the caller (PW2-3's record) and stamped through StampPageLsn; this
// header only moves bytes.

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
// Corruption on disagreement, never computed from - a forged count was an
// ASan-demonstrated out-of-bounds read, and one branch over, a write
// (this file's 3f07eda review, C1).

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
// allocates the pages it would then record here. `CREATE INDEX` builds
// the whole tree and seeds the anchor afterwards, so on a full table
// every attempt allocated an index tree - 32 pages - and threw it away.
// Nothing frees, so those pages are gone for the life of the database.
// The storm that measured it, and its numbers, are in
// `docs/inflight/known-gaps.md`; they are not restated here, so there is one
// figure to keep true.
Status CheckAnchorRoomForIndex(std::span<const std::byte, kPageSize> page,
                               std::uint64_t index_oid);

// Inserts or updates the entry for `index_oid`. Refuses exactly as
// CheckAnchorRoomForIndex does. The cap is **not** unreachable (the
// f5686f8 review's C4.2 corrected the first claim here): entries
// accumulate across DROP INDEX - no removal exists yet, and index oids
// never reissue - so ~679 create-then-drop cycles on one relation fill
// the table, and the next index's first root split fails an ordinary
// INSERT. Remote, and named as the debt it is in
// workplan-peer-writer.md §7a.
Status SetAnchorIndexRoot(std::span<std::byte, kPageSize> page, std::uint64_t index_oid,
                          PageId root);

// There is deliberately no RemoveAnchorIndexRoot yet: a removal is a
// mutation the log could not describe (redo of an older ANCHOR_UPDATE
// would resurrect the entry), and DROP INDEX is transactional - a
// rollback must keep the slot - so the removal belongs at DDL
// *resolution*, the §5d catalog-mark purge's shape, with its own record.
// Its absence is what makes the accumulation above real.

}  // namespace kds::storage
