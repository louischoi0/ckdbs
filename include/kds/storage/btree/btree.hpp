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
// delete-mark, `exec::EncodeRow`/`DecodeRow` and the `HEAP_INSERT` redo
// record all apply to a leaf verbatim.
// A clustered-btree relation is therefore not a second storage engine,
// it is the heap with a directory over it.
//
// ---- Splits move nothing, and that is on purpose ------------------------
//
// Invariant 10 makes every pk system-issued, monotonically increasing.
// A descent for a new id therefore always ends at the **rightmost** leaf,
// and when that leaf is full the split is:
//
//     new leaf, low key = the id that caused the split, tuple goes there;
//     old leaf's next_page_id repointed at it; the same id copied up as
//     the parent's separator.
//
// No key is ever moved between pages. That is the identical bargain
// heap_chain.hpp strikes, and it is struck for the identical reason: **the
// heap page split policy is an open decision in CLAUDE.md** (how a full
// page's contents get divided, and where the new boundary goes), and a
// tree that divides page contents would decide it as a side effect. So it
// does not. A split of a leaf whose contents would have to be divided -
// which requires an id below the leaf's existing maximum, i.e. a
// non-monotonic sequence - is refused with OutOfSpace naming the open
// decision, not silently guessed at.
//
// Two consequences worth stating because they are easy to assume away:
//
//   1. **Nothing here moves a tuple**, so a leaf's `min_key` is immutable
//      exactly as a heap page's is (invariant 2). That is a property of
//      what is unimplemented - no relayout, no compaction - not a
//      guarantee to design against.
//   2. Leaves fill left-to-right and are never merged, so the tree's space
//      utilisation matches the heap chain's - no 50% worst case, and no
//      reuse of space freed by DELETE either, for the same missing page
//      compaction.
//
// ---- Structural changes are reported, not logged here -------------------
//
// This file mutates pages; it does not know about the WAL. An insert
// reports every page it created or restructured (`BtreeInsertResult::
// changes()`), in the order redo has to apply them, and the caller emits
// the records. Same division as heap_chain.hpp, where `linked_from` and
// `grew_chain` exist for exactly that.
//
// Concurrency: none of its own, like heap_chain.hpp. Descent takes and
// releases each page through the PageStore; the caller holds the pin/latch
// discipline (CLAUDE.md's page-latch consistency model). There is no
// latch coupling and no B-link right-link protocol, because there is no
// concurrent mutation to protect against yet - the server is one
// cooperative thread and nothing here suspends.

namespace kds::btree {

// The descent's depth bound and the shape an insert reports back are
// shared with the heap chain and live in insert_placement.hpp:
// `kMaxBtreeDepth`, `StructuralChange`, `InsertPlacement`.

// Where a tuple lives: what a descent hands back to a reader.
struct Location {
    PageId page_id = kInvalidPageId;
    std::uint16_t slot = 0;
    // There is deliberately no bytes field. One rode here until 2026-08-13
    // - "the leaf's bytes, still resident from the descent" - and under the
    // pin model (workplan-pageref.md) that sentence stopped being true the
    // moment the descent returned: the span outlived its pin, which is a
    // use-after-free the day anything faults in between. Its one
    // production consumer already re-fetched by page_id (a hash hit on a
    // resident frame, and the fetch is what marks the write path dirty),
    // so the field's saving was zero and its hazard was real. A caller
    // reads the tuple by fetching page_id, holding the ref it gets back.
};

// Formats `page` as a brand-new relation's root: an empty leaf with
// min_key 0, so a relation that never outgrows one page is exactly one
// page, the same as a heap-clustered one. The tree gains its first
// internal level only when this leaf splits. `owner_oid` (page.md §2a) is
// the relation's oid, stamped into this and every page the tree ever
// creates — not defaulted, because a clustered tree always has a relation.
Status FormatRoot(std::span<std::byte, kPageSize> page, std::uint64_t owner_oid);

// Inserts `payload` (whose leading Keystone word must carry `id`) into the
// tree rooted at `root`, splitting and growing as needed.
//
// Fails with:
//   AlreadyExists  a live tuple in the target leaf already carries `id`
//   OutOfRange     `id` is below the target leaf's min_key (invariant 3)
//   OutOfSpace     the leaf is full and `id` does not sort above its
//                  contents, so making room would need the undecided split
//                  policy; or a tuple no empty leaf could hold
//   Corruption     the payload is too short for a Keystone word, its id
//                  disagrees with `id`, or the descent exceeded
//                  kMaxBtreeDepth / hit a page of the wrong type
//   ...            whatever the store reports when a page cannot be
//                  allocated
//
// On failure nothing is reported as structural, but pages *may* already
// have been allocated (an allocation with no free-page path to undo it,
// same as ChainInsert). They are never linked in, so nothing reaches them.
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
// over it.
StatusOr<Location> BtreeLookup(storage::PageStore& store, PageId root, std::uint64_t id);

// Calls `fn` once per slot of every leaf, left to right - which is pk
// order page by page, tuples within a leaf staying unordered exactly as in
// a heap page. Signature matches heap::ChainVisit deliberately, so a
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
