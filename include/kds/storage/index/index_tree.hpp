#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>
#include <span>

#include "kds/base/status.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/insert_placement.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/storage/visit.hpp"

// The secondary index B+ tree (docs/spec/index.md §4).
//
// ---- Why this is not storage/btree/ -------------------------------------
//
// `btree.hpp` is the *clustered* tree: a relation's storage, keyed on the
// Keystone id. Since BD it places a row where its key sorts and splits a
// full leaf (an append, or a divide at the insertion point or the median,
// `btree.hpp`), so neither tree relies on monotonic keys any more. What
// still separates them is the entry: a clustered leaf is a heap page with a
// `min_key` and whole tuples, and an index page holds entries with no
// `min_key` and no Keystone id (index_page.hpp), so this tree divides
// entries and decides nothing beyond itself.
//
// Everything else is deliberately the same shape, including the
// `VisitControl` contract, so a caller can hand the same lambda to either
// walk with the same meaning and the same consequence for getting it wrong.
//
// ---- What an entry is, and what it is not -------------------------------
//
//     entry := key || tuple key || covered
//
// The tuple key resolves through the relation's clustered tree; there is no
// `(page_id, slot)` location hint, because a secondary index is refused on a
// heap-clustered relation (spec IX3). So nothing here can dangle, nothing
// needs healing, and relayout - which moves tuples but never changes a tuple key -
// cannot invalidate an index.
//
// An entry is **not** authority over a row. It says a row *once had* this
// key; the reader re-checks the predicate against the MVCC-resolved version
// (spec §1). This tree neither knows nor asks about visibility.
//
// ---- Duplicates ---------------------------------------------------------
//
// Two entries may share a key, and even a (key, tuple key) pair - an UPDATE that
// changes a covered column appends an entry with the same key and tuple key and
// different covered bytes, which is information, not noise.
//
// A **byte-identical** entry is not information, and `IndexInsert` reports
// it rather than storing it twice (`already_present`): a writer whose new
// version needs that entry then relies on the one already there, which is
// why an index entry purge removes a row's entries only under the row's
// tuple unit (DD-Q2, `exec/index_purge.hpp`); and a probe that resolved one
// tuple key twice would emit its row twice. The check is complete: an entry
// is its own sort key (index_page.hpp), so the leaf the descent lands on is
// the one place it can be - and since no two entries are equal, no two share
// a sort key, which is what keeps every separator distinct. Deduplicating
// *by tuple key* is the read path's job, not this file's.
//
// ---- Concurrency ---------------------------------------------------------
//
// Every page is taken and released through the PageStore, whose `PageRef`
// carries the page latch (shared for `GetForRead`, exclusive for `Get`).
// There is no latch coupling: a descent releases a parent before it reads
// the child. Since AT-S5 two cores write one index, so a divide on one can
// run inside another's descent; `IndexInsert` answers it the way the
// clustered tree does (AT-S5c) - once its leaf is held exclusive it asks
// whether the leaf **still covers** the sort key, and a stale descent starts
// over, a bounded number of times (`storage::kMaxDescentRestarts`).
// index_tree.cpp's `DescendTo` carries the argument; `IndexSeekLeaf` needs
// no such check, and says why.
//
// **The check rests on a premise: no leaf's first entry is ever removed.**
// An index leaf has no immutable `min_key`, so a leaf's upper bound is read
// off its right sibling's first entry, which equals the separator the
// sibling's divide copied up only while that entry stays. The index entry
// purge (DD, `IndexRemoveEntries`) removes others and refuses the first
// (`IndexLeafView::RemoveEntry`, DD-Q3 (a)); a change that would take a
// first entry out must give the leaf its bound another way (DM's U2b).
//
// **The walk back up is checked too** (AT-S16): a divide finds, asks and
// holds every parent it writes before it writes anything, re-descending
// from the root on a miss, and marks a root a level grows over so that a
// core whose memo names a stale root is refused rather than growing a
// second one. index_tree.cpp's `SecureParents` carries it.

namespace kds::index {

// A dividing split touches **two** pages per level it propagates through -
// the node that overflowed and the sibling that took half of it - plus one
// new root. Counted from this tree's own split, and so a constant of its
// own beside `storage::kMaxStructuralChanges`, which counts the clustered
// tree's.
inline constexpr std::size_t kMaxIndexChanges =
    2 * static_cast<std::size_t>(storage::kMaxBtreeDepth) + 1;

struct IndexInsertResult {
    PageId page_id = kInvalidPageId;  // the leaf the entry landed in
    // Its position within that leaf **at the moment of insert**. A later
    // insert into the same leaf shifts it, exactly as a sorted array does;
    // an index position is not a stable address and nothing stores one.
    std::uint16_t slot = 0;

    // Set when the tree grew a level: the caller repoints the index's root
    // page id in the catalog. Returned rather than written here, because
    // this layer has no catalog - and because a root published before its
    // contents are logged is a root recovery cannot reach.
    PageId new_root = kInvalidPageId;

    // True when a byte-identical entry was already present. `page_id`/`slot`
    // then name that entry and nothing was written.
    bool already_present = false;

    // Pages the append **restructured** - created or rewrote - by id. No
    // record type describes one and there is no `INDEX_PAGE_INIT`
    // (`index.md` §12.1), so the split logs an image of each as one
    // `BTREE_SPLIT`. Empty for the overwhelmingly common insert that just
    // filled a slot, which the entry bytes describe completely.
    std::array<PageId, kMaxIndexChanges> structural{};
    std::uint8_t n_structural = 0;

    // The pins granted for the parents below past the first; before
    // `held`, so the holds drop first.
    storage::PinAllowance allowance;
    // The leaf the entry landed in and the parents `SecureParents` took,
    // still held (AT-S21): an `INDEX_INSERT` names a slot, and another
    // core's insert into the leaf between the write and its record shifts
    // it. The caller logs from the page under this hold, then drops it.
    std::vector<storage::PageRef> held;
    // A divide that grew a root: its anchor's `UnlatchedPin`.
    storage::UnlatchedPin anchor;

    std::span<const PageId> changes() const {
        return std::span<const PageId>(structural.data(), n_structural);
    }
    bool restructured() const { return n_structural > 0; }
    void Record(PageId id) {
        // Bounded by kMaxIndexChanges above, which is a derivation and not a
        // hope. Dropping a change silently would be a page mutated with no
        // record describing it - the hole the WAL exists to close - so there
        // is deliberately no "if full, skip" arm.
        structural[n_structural++] = id;
    }
};

// Formats `page` as a brand-new index's root: an empty leaf with no sibling,
// so an index that never outgrows one page is exactly one page. The tree
// gains its first internal level only when this leaf splits. `owner_oid`
// (page.md §2a) is the index's own oid — the immediate-owner rule — and is
// not defaulted: every index has one.
Status FormatRoot(std::span<std::byte, kPageSize> page, const IndexLayout& layout,
                  std::uint64_t owner_oid);

// Inserts `key || tuple key || covered` into the tree rooted at `root`.
//
// `key` must be exactly `layout.key_width` bytes and `covered` exactly
// `layout.covered_width`; both are already-encoded bytes this layer never
// interprets.
//
// Fails with:
//   InvalidArgument  a width disagrees with `layout`
//   Corruption       a page of the wrong type, widths disagreeing with
//                    `layout`, or a descent past kMaxBtreeDepth
//   OutOfSpace       a layout no page could split (CheckIndexLayout)
//   ...              whatever the store reports when a page cannot be
//                    allocated
//
// A divide claims every page it creates before it writes one, as the
// clustered tree's split does (btree.hpp). On a later failure pages may
// already have been allocated, and none is freed here - a free needs
// BF-R4's replay gate. A leaf a failed propagation leaves is
// linked into the right-sibling chain, which a dropped relation's reclaim
// follows; anything else it leaves is a stated leak (`drop-table.md` DT1).
//
// `anchor_page` is the anchor a growth pins (`UnlatchedPin`); none pins nothing.
StatusOr<IndexInsertResult> IndexInsert(storage::PageStore& store, PageId root,
                                        const IndexLayout& layout,
                                        std::span<const std::byte> key, std::uint64_t tuple_key,
                                        std::span<const std::byte> covered,
                                        std::uint64_t owner_oid,
                                        PageId anchor_page = kInvalidPageId);

// The leaf that holds `sort_key`, or that *would* hold it - the descent
// alone, with no question about whether anything matches.
//
// `sort_key` is a full `layout.sort_key_width()` bytes. A probe that knows
// less - key K on a one-column index, or the first column of a composite one
// - encodes what it knows and leaves the rest **zero**, which is the lower
// bound of everything matching (index_page.hpp explains why zero is the
// floor, and why shortening the comparison instead would be wrong). The
// descent then lands on the first leaf that could hold a match, and the walk
// crosses `right_sibling` links while the key still matches - which is what
// makes a duplicated key readable across a leaf boundary.
StatusOr<PageId> IndexSeekLeaf(storage::PageStore& store, PageId root, const IndexLayout& layout,
                               std::span<const std::byte> sort_key);

// A leaf, fetched under `access` and checked - its type and its widths
// against `layout` - as every fetch of the walk below is: the page an
// ordered index run (CV) reads one at a time, holding it while it reads.
StatusOr<storage::PageRef> IndexFetchLeaf(storage::PageStore& store, PageId leaf,
                                          const IndexLayout& layout, storage::PageAccess access);

// The leaf that holds the greatest entries at or below `sort_key`, fetched
// for read and **held** (CV O3, the reverse ordered walk's step). A read
// descent lands at or left of `sort_key`'s leaf (`IndexSeekLeaf`), so
// the leaf is asked, while it is held, whether its right sibling's first
// entry is above `sort_key` - a leaf covers `[its separator, the right
// sibling's first entry)`, which holds while no first entry is removed (above) -
// and the walk moves right until it is. Holding a leaf and asking its right
// neighbour is a forward walk's order. Every entry at or below `sort_key`
// the tree held before the call is in the leaf returned or to its left.
StatusOr<storage::PageRef> IndexFetchLeafAtOrBelow(storage::PageStore& store, PageId root,
                                                   const IndexLayout& layout,
                                                   std::span<const std::byte> sort_key);

// Takes `entries` - whole entries, ascending - out of the tree where each
// still stands (DD, an index entry purge), and answers how many went. Each
// leaf is reached by a write descent and held exclusive while every entry
// it still covers is removed, then `log_leaf` is called under that hold
// with the leaf as rewritten (DD-R6), once per leaf that changed. **A
// leaf's first entry is never removed** (`IndexLeafView::RemoveEntry`), so
// no leaf is emptied and no separator changes (DD-R4); an entry that is
// absent, or first, is passed over. Holds one leaf, and its right sibling
// for the span of a coverage check, and nothing else (DD-R5).
StatusOr<std::uint64_t> IndexRemoveEntries(
    storage::PageStore& store, PageId root, const IndexLayout& layout,
    std::span<const std::vector<std::byte>> entries,
    const std::function<Status(storage::PageRef&)>& log_leaf);

// `key` made, in place, the greatest sort key of its width below it - the
// reverse walk's next bound below a leaf's first entry. `key` must not be
// all zeros, which no caller's is: it is above the walk's low bound.
void PrecedingSortKey(std::span<std::byte> key);

// Calls `fn` once per entry of every leaf from `first_leaf` rightwards -
// which is key order. Signature and contract match heap::ChainVisit and
// btree::BtreeVisit deliberately: `kStop` ends the walk with Status::OK()
// and never fetches the leaves to its right, which is what lets a range end
// at its high bound.
Status IndexVisitFrom(
    storage::PageStore& store, PageId first_leaf, const IndexLayout& layout,
    storage::PageAccess access,
    const std::function<StatusOr<storage::VisitControl>(PageId, IndexLeafView&, std::uint16_t)>&
        fn);

// IndexVisitFrom starting at the leftmost leaf.
Status IndexVisit(
    storage::PageStore& store, PageId root, const IndexLayout& layout,
    storage::PageAccess access,
    const std::function<StatusOr<storage::VisitControl>(PageId, IndexLeafView&, std::uint16_t)>&
        fn);

// Levels from root to leaf inclusive: 1 while the root is still a leaf. For
// SHOW/DESCRIBE and tests, not a hot path.
StatusOr<std::uint16_t> IndexHeight(storage::PageStore& store, PageId root,
                                    const IndexLayout& layout);

// Leaves in the tree, walked through the sibling links. Same audience.
StatusOr<std::uint32_t> IndexLeafCount(storage::PageStore& store, PageId root,
                                       const IndexLayout& layout);

// Entries in the tree, for tests and for the `SHOW INDEXES` size column.
StatusOr<std::uint64_t> IndexEntryCount(storage::PageStore& store, PageId root,
                                         const IndexLayout& layout);

}  // namespace kds::index
