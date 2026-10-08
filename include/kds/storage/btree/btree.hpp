#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>

#include "kds/base/status.hpp"
#include "kds/storage/btree/btree_page.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/insert_placement.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/storage/visit.hpp"

// The clustered B+ tree: a relation whose `sys.tables.clustered_type` is
// `kBtree` stores its tuples in this tree's leaves, rooted at the same
// `desc_page_id` a heap-clustered relation roots its chain at.
//
// ---- What this is, next to heap_chain.hpp -------------------------------
//
// A heap-clustered relation is a linked list of pages: point lookup by pk
// is O(pages), and a range scan has to start at the head. This is the same tuple storage with an index above it - the
// leaves *are* heap pages (heap_page.hpp's CreateEmptyAs under a
// kBtreeLeaf header), linked by the same `next_page_id`, holding the same
// slotted tuples with the same MVCC header. What is added is the internal
// levels (btree_page.hpp), which turn a pk lookup into O(log n) page
// fetches and let a range scan start at the first qualifying leaf.
//
// Reusing the leaf body rather than defining a second tuple format is
// deliberate and load-bearing: `heap::PageView` insert/read/overwrite/
// delete-mark and `exec::EncodeRow`/`DecodeRow` apply to a leaf verbatim.
// A clustered-btree relation is therefore not a second storage engine,
// it is the heap with a directory over it. What differs is where a row
// goes and how its insert is logged (below).
//
// ---- A leaf is in key order by placement (BD-R1, BD-R2) -----------------
//
// `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`. A row takes
// the slot its key sorts to (`heap::PageView::InsertTupleAt`): the
// directory entries after it move up one, no tuple byte moves, and every
// keyed slot of a leaf - live and delete-marked - ascends by Keystone id,
// whatever order keys arrive in and at every core count. That is the
// premise the `ORDER BY <pk>` elision reads, and `SearchLeaf` below is the
// one binary search every caller asks. An insert logs `BTREE_INSERT`, whose
// redo shifts as the placement did.
//
// **A full leaf grows three ways**, and every page a split writes is logged
// as an image in one `BTREE_SPLIT` record that carries the row (BD-R12):
//
//   - **append** - the id sorts above every key the leaf holds: a new leaf
//     whose low key is the id, nothing moved, the old leaf linked to it and
//     the id copied up as the parent's separator;
//   - **insertion point** - the rightmost leaf, the id below one of its
//     keys: the id opens the new leaf and the keys above it move there, so
//     the left leaf stays full (BD-Q11 (a));
//   - **median** - any other divide (`SplitLeafAndInsert`): the keys cut at
//     their median, the upper half moved to a new leaf.
//
// A divide bumps the old leaf's `relayout_epoch`, and a separator sorting
// inside a full internal node divides that node. docs/spec/heap-and-tuple.md
// section 4.1 carries why a divide keeps invariants 2 and 3.
//
// Two consequences worth stating because they are easy to assume away:
//
//   1. **A placement and a divide move rows; nothing rewrites a `min_key`**,
//      so a leaf's `min_key` is immutable exactly as a heap page's is
//      (invariant 2). There is no relayout and no compaction. A `(page,
//      slot)` is therefore true only under the leaf's hold (`Location`).
//   2. Leaves are never merged, and nothing reuses space freed by DELETE,
//      for the same missing page compaction as the heap chain's.
//

// ---- Structural changes are reported, not logged here -------------------
//
// This file mutates pages; it does not know about the WAL. An insert
// reports every page it created or restructured (`BtreeInsertResult::
// changes()`), in the order redo has to apply them, and the caller emits
// the records. Same division as heap_chain.hpp, where `linked_from` and
// `grew_chain` exist for exactly that.
//
// ---- Concurrency: one page at a time, and one window that is checked ---
//
// No synchronisation of its own, like heap_chain.hpp: a descent takes and
// releases each page through the `PageStore`, and the pin and the page
// latch ride in the `PageRef` it holds (`page.md` §6 - the latch is held
// by a *core*, so it serialises cores as readily as tasks on one). There
// is still **no latch coupling and no B-link right-link protocol**: a
// descent never holds two pages at once, and an internal node is read and
// let go before its child is asked for.
//
// *"There is no concurrent mutation to protect against yet - the server is
// one cooperative thread and nothing here suspends"* stood here until
// AT-S5c. AT-S5 made a write run where the session is, so two cores reach
// one relation's leaves, and the sentence stopped being true of the one
// place in this file that depends on it: **a write descent's re-fetch**,
// which must drop the leaf's shared hold before asking for it exclusive
// because the page latch is never upgraded. `DescendTo` carries the
// window's whole account; what closes it is that a leaf's coverage
// interval can shrink in exactly one way, and that way rewrites the leaf's
// own `next_page_id`.
//
// **What is still owed to the single-core reading** is the *absence* of a
// protocol rather than any particular line: a reader holds one page's
// share while it reads, and nothing here reasons across two pages without
// holding both. A structural change that had to be seen atomically across
// levels - a merge, a rebalance - would need the coupling this file does
// not have, and none exists.

namespace kds::btree {

// The descent's depth bound and the shape an insert reports back are
// shared with the heap chain and live in insert_placement.hpp:
// `kMaxBtreeDepth`, `StructuralChange`, `InsertPlacement`.

// Where a tuple lives, and the leaf it lives in, **held**: what a descent
// hands back to a reader or a writer.
//
// **`slot` is true only while `leaf` is held** (AT-0 item 12). Another
// core's insert below the row shifts it a slot up (BD-R2), and a divide
// rebuilds the leaf and renumbers its slots (`SplitLeafAndInsert`), so a
// `(page_id, slot)` read after the hold is
// gone can name a different row: a point read answers zero rows through
// its residual, a point write declines the row it was sent to, an FK check
// decides on another row. Read or write the slot through `leaf`, and never
// re-fetch `page_id` to reach it. `leaf` is a `PageRef` - the pin and the
// page latch (AM-S1) - and every other site in the engine cites this
// comment rather than restating it.
struct Location {
    PageId page_id = kInvalidPageId;
    std::uint16_t slot = 0;
    storage::PageRef leaf;
};

// **Where `id` sits among a leaf's keyed slots** (BD-R2,
// `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`).
//
// Every keyed slot of a leaf ascends by Keystone id - live rows and
// delete-marked rows alike - because a row is placed at the slot its key
// sorts to and a divide writes each half back in key order. A retired slot
// carries no key and sits anywhere. So one binary search answers what an
// insert, a lookup, redo's BTREE_INSERT and recovery undo's re-find ask: is
// the key present, and where would it go. `at` is the first keyed slot whose
// key is at or above `id`, or `slot_count()` when there is none - the slot
// a placement takes, so redo checks a record's slot against it.
struct LeafPosition {
    std::uint16_t at = 0;
    bool present = false;  // the key at `at` is `id`
};
StatusOr<LeafPosition> SearchLeaf(heap::PageView& leaf, std::uint64_t id);

// The duplicate a named key meets (BD-R5's first reason): *"duplicate primary
// key k"*, or *"... k: a row with this key was deleted; a Keystone id is
// bound once"* for a delete-marked row (BD-R4's tombstone).
Status DuplicateKey(std::uint64_t id, PageId leaf_id, std::uint16_t slot, bool deleted);

// Formats `page` as a brand-new relation's root: an empty leaf with
// min_key 0, so a relation that never outgrows one page is exactly one
// page, the same as a heap-clustered one. The tree gains its first
// internal level only when this leaf splits. `owner_oid` (page.md §2a) is
// the relation's oid, stamped into this and every page the tree ever
// creates — not defaulted, because a clustered tree always has a relation.
Status FormatRoot(std::span<std::byte, kPageSize> page, std::uint64_t owner_oid);

// Inserts `payload` (whose leading Keystone word must carry `id`) into the
// tree rooted at `root`, at the slot `id` sorts to, splitting and growing as
// needed. **The one door a row comes through** (BD-R6): any id, issued or
// named - the issue-under-hold doors BB-R1 asked for are deleted with it.
//
// Fails with:
//   AlreadyExists  a tuple in the target leaf already carries `id`, live or
//                  delete-marked (`DuplicateKey`'s text says which)
//   OutOfRange     `id` is below the target leaf's min_key (invariant 3)
//   OutOfSpace     the leaf is full, `id` sorts inside it, and it holds
//                  fewer than two live tuples, so no division makes room;
//                  or a tuple no empty leaf could hold
//   Corruption     the payload is too short for a Keystone word, its id
//                  disagrees with `id`, or the descent exceeded
//                  kMaxBtreeDepth / hit a page of the wrong type
//   ...            whatever the store reports when a page cannot be
//                  allocated
//
// On failure nothing is reported as structural, but pages *may* already
// have been allocated, and none is freed here - a free needs BF-R4's replay
// gate. The new leaf a failed promotion leaves is linked into the leaf
// chain on a dividing split and reached by no descent; an append split's
// new leaf, and any internal node the promotion created, are reached by
// neither. A dropped relation's reclaim follows the leaf chain, so it frees
// the first; the rest is a stated leak (`drop-table.md` DT1).
StatusOr<storage::InsertPlacement> BtreeInsert(storage::PageStore& store, PageId root,
                                                std::uint64_t id,
                                                std::span<const std::byte> payload,
                                                std::uint64_t trx_id,
                                                std::uint64_t owner_oid);

// Descends to the leaf that owns `id` and finds its live slot. This is the
// point-lookup the whole structure exists for: O(depth) page fetches plus
// one leaf scan, against the heap chain's O(pages).
//
// Fails with NotFound if no live tuple in that leaf carries `id` - which,
// because the descent is exact, means the row does not exist. The answer
// is **authoritative**: the tree is the relation's storage, not a hint
// over it - and so is the payload, because the leaf comes back held.
//
// `access` is how the leaf is held: `kRead` shared, `kWrite` exclusive and
// marked dirty, for a caller that writes the slot it was handed. A writer
// must ask for `kWrite` rather than re-fetch: the latch is never upgraded,
// so dropping a shared hold to take an exclusive one reopens the window
// `Location` exists to close.
StatusOr<Location> BtreeLookup(storage::PageStore& store, PageId root, std::uint64_t id,
                               storage::PageAccess access = storage::PageAccess::kRead);

// Calls `fn` once per slot of every leaf, left to right - which is pk
// order page by page, and within a leaf slot order, which is key order on
// every leaf (BD-R1). Signature matches heap::ChainVisit deliberately, so a
// caller can hand the same lambda to either - `access` and the
// VisitControl contract included, with the same meaning and the same
// consequence for getting either wrong. kStop ends the walk with
// Status::OK() and leaves the remaining leaves unread.
// The leaf that holds `id`, or that *would* hold it - the descent alone,
// without asking whether the key is present.
//
// It exists for a range: `WHERE id BETWEEN low AND high` on a clustered
// relation should start at `low`, not at the leftmost leaf. Walking from the
// head and stopping at the tail reads everything before the range, which for
// a uniformly drawn bound is half the relation - measured at 44% of a full
// scan on a 60,480-row table (bench/results-scenario1-vs-pg.md).
//
// The distinction from BtreeLookup is the whole point: a lookup answers
// NotFound when the key is absent, and a range's low bound very often is.
StatusOr<PageId> BtreeSeekLeaf(storage::PageStore& store, PageId root, std::uint64_t id);

// BtreeVisit, starting at a given leaf rather than the leftmost one.
//
// `first_leaf` comes from BtreeSeekLeaf. Everything else is identical -
// including that a `kStop` never fetches the leaves to its right, which is
// what lets a range end at its high bound.
Status BtreeVisitFrom(
    storage::PageStore& store, PageId first_leaf, storage::PageAccess access,
    const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&, std::uint16_t)>&
        fn);

Status BtreeVisit(
    storage::PageStore& store, PageId root, storage::PageAccess access,
    const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&, std::uint16_t)>&
        fn);

// The leftmost leaf - where a full walk starts. BtreeVisit descends
// through it; exposed for callers that own the page loop themselves
// (exec's RunWalkStep, workplan-crosscore.md P4d-3).
StatusOr<PageId> BtreeLeftmostLeaf(storage::PageStore& store, PageId root);

// One leaf of the walk BtreeVisit makes: calls `fn` per slot of exactly
// `leaf`, under the same visitor contract, and returns the right sibling
// the walk continues at - kInvalidPageId when `fn` stopped the walk or
// this is the rightmost leaf. Pin discipline as heap::ChainVisitOnePage:
// the leaf's pin is held only inside this call, so the gap between two
// calls is a legal suspension point, and the caller owns the cycle guard
// the whole-chain forms apply (heap::kMaxChainPages).
StatusOr<PageId> BtreeVisitLeafPage(
    storage::PageStore& store, PageId leaf, storage::PageAccess access,
    const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&, std::uint16_t)>&
        fn);

// Levels from root to leaf inclusive: 1 while the root is still a leaf.
// For DESCRIBE/SHOW and tests, not a hot path.
StatusOr<std::uint16_t> BtreeHeight(storage::PageStore& store, PageId root);

// Leaves in the tree, walked through the sibling links. Same audience as
// BtreeHeight().
StatusOr<std::uint32_t> BtreeLeafCount(storage::PageStore& store, PageId root);

}  // namespace kds::btree
