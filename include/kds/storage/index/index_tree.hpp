#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>

#include "kds/base/status.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/insert_placement.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/storage/visit.hpp"

// The secondary index B+ tree (docs/spec/index.md §4, workplan IX02).
//
// ---- Why this is not storage/btree/ -------------------------------------
//
// `btree.hpp` is the *clustered* tree: a relation's storage, keyed on the
// Keystone id, whose split is an append of a fresh rightmost leaf that moves
// nothing. It refuses `OutOfSpace` for any key sorting below its target
// leaf's contents, and its header says why - dividing a full page's contents
// would decide the **heap page split policy**, which CLAUDE.md leaves open.
// That bargain is available to it because invariant 11 makes every pk
// monotonic.
//
// A secondary key is not monotonic. Arbitrary-order arrival is the defining
// property of the thing, so a dividing split is mandatory and the clustered
// tree cannot be taught one without settling the decision it exists to
// avoid. This tree divides - and decides nothing beyond itself, because an
// index page holds entries rather than tuples, has no `min_key`, and
// contains no Keystone id (index_page.hpp).
//
// Everything else is deliberately the same shape, including the
// `VisitControl` contract, so a caller can hand the same lambda to either
// walk with the same meaning and the same consequence for getting it wrong.
//
// ---- What an entry is, and what it is not -------------------------------
//
//     entry := key || pk || covered
//
// The pk resolves through the relation's clustered tree; there is no
// `(page_id, slot)` location hint, because a secondary index is refused on a
// heap-clustered relation (spec IX3). So nothing here can dangle, nothing
// needs healing, and relayout - which moves tuples but never changes a pk -
// cannot invalidate an index.
//
// An entry is **not** authority over a row. It says a row *once had* this
// key; the reader re-checks the predicate against the MVCC-resolved version
// (spec §1). This tree neither knows nor asks about visibility.
//
// ---- Duplicates ---------------------------------------------------------
//
// Two entries may share a key, and even a (key, pk) pair - an UPDATE that
// changes a covered column appends an entry with the same key and pk and
// different covered bytes, which is information, not noise.
//
// A **byte-identical** entry is not information, and `IndexInsert` reports
// it rather than storing it twice (`already_present`). Nothing reclaims an
// index entry, so a duplicate is permanent; and a probe that resolved one
// pk twice would emit its row twice. The check is complete for a duplicate
// in the leaf the descent lands on, which is where an exact duplicate always
// sorts. Deduplicating *by pk* is the read path's job, not this file's.
//
// Concurrency: none of its own, like btree.hpp. A descent takes and releases
// each page through the PageStore; the caller holds the pin/latch discipline
// (CLAUDE.md's page-latch consistency model). No latch coupling and no
// B-link protocol, because there is no concurrent mutation to protect
// against - the server is one cooperative thread per core and nothing here
// suspends.

namespace kds::index {

// One page an insert created or restructured, as the WAL will need it
// described (spec §12.1, built by IX08).
struct IndexChange {
    PageId page_id = kInvalidPageId;
    // A brand-new page, which `INDEX_PAGE_INIT` describes completely.
    // Everything else - a node whose entries were divided, a leaf whose
    // sibling link moved - is false and needs a `FULL_PAGE_IMAGE`, because
    // no record type describes an entry-array division on its own.
    bool is_new_page = false;
};

// A dividing split touches **two** pages per level it propagates through -
// the node that overflowed and the sibling that took half of it - plus one
// new root. `storage::kMaxStructuralChanges` is derived from the clustered
// tree's split, which creates one page per level and moves nothing, so it is
// the wrong bound here and is deliberately not reused.
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

    // Pages the append **restructured**, which no record type describes -
    // so a caller logs a full page image of each. Empty for the
    // overwhelmingly common insert that just filled a slot, which the entry
    // bytes describe completely.
    std::array<IndexChange, kMaxIndexChanges> structural{};
    std::uint8_t n_structural = 0;

    std::span<const IndexChange> changes() const {
        return std::span<const IndexChange>(structural.data(), n_structural);
    }
    bool restructured() const { return n_structural > 0; }
    void Record(PageId id, bool is_new_page) {
        // Bounded by kMaxIndexChanges above, which is a derivation and not a
        // hope. Dropping a change silently would be a page mutated with no
        // record describing it - the hole the WAL exists to close - so there
        // is deliberately no "if full, skip" arm.
        structural[n_structural++] = IndexChange{id, is_new_page};
    }
};

// Formats `page` as a brand-new index's root: an empty leaf with no sibling,
// so an index that never outgrows one page is exactly one page. The tree
// gains its first internal level only when this leaf splits. `owner_oid`
// (page.md §2a) is the index's own oid — the immediate-owner rule — and is
// not defaulted: every index has one.
Status FormatRoot(std::span<std::byte, kPageSize> page, const IndexLayout& layout,
                  std::uint64_t owner_oid);

// Inserts `key || pk || covered` into the tree rooted at `root`.
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
// On failure pages may already have been allocated and are never linked in,
// so nothing reaches them - the same bargain BtreeInsert and ChainInsert
// strike, for the same missing free-page path.
StatusOr<IndexInsertResult> IndexInsert(storage::PageStore& store, PageId root,
                                        const IndexLayout& layout,
                                        std::span<const std::byte> key, std::uint64_t pk,
                                        std::span<const std::byte> covered,
                                        std::uint64_t owner_oid);

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
