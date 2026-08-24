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
// row. A dropped index's entry is removed by swapping the last entry in -
// order carries no meaning.
//
// Mutation protocol: single-writer under the owning core's statement
// execution, like every relation page. Every mutation is WAL-logged by
// the caller (PW2-3's record) and stamped through StampPageLsn; this
// header only moves bytes.

namespace kds::storage {

inline constexpr std::size_t kAnchorClusteredRootOffset = kPageBodyOffset;
inline constexpr std::size_t kAnchorNrIndexOffset = kAnchorClusteredRootOffset + 4;
inline constexpr std::size_t kAnchorEntriesOffset = kAnchorNrIndexOffset + 4;
inline constexpr std::size_t kAnchorEntrySize = 12;  // index_oid u64 + root u32
inline constexpr std::size_t kAnchorMaxIndexEntries =
    (kPageSize - kAnchorEntriesOffset) / kAnchorEntrySize;

// Formats `page` as a fresh anchor for `owner_oid` with the clustered
// root set and no index entries.
void FormatAnchorPage(std::span<std::byte, kPageSize> page, std::uint64_t owner_oid,
                      PageId clustered_root);

PageId AnchorClusteredRoot(std::span<const std::byte, kPageSize> page);
void SetAnchorClusteredRoot(std::span<std::byte, kPageSize> page, PageId root);

// The root recorded for `index_oid`, or kInvalidPageId when the anchor
// holds no entry for it.
PageId AnchorIndexRoot(std::span<const std::byte, kPageSize> page, std::uint64_t index_oid);

// Inserts or updates the entry for `index_oid`. Refuses with
// ResourceExhausted past kAnchorMaxIndexEntries - unreachable through DDL
// (index counts are capped far below), stated so a direct caller cannot
// overrun silently.
Status SetAnchorIndexRoot(std::span<std::byte, kPageSize> page, std::uint64_t index_oid,
                          PageId root);

// Removes `index_oid`'s entry (swap-with-last). Removing an absent entry
// is a no-op, not an error: DROP INDEX's compensation may run twice.
void RemoveAnchorIndexRoot(std::span<std::byte, kPageSize> page, std::uint64_t index_oid);

}  // namespace kds::storage
