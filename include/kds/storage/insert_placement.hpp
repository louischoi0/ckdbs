#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/base/function_ref.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/page_store.hpp"

// What one tuple insert did: where the tuple landed, and every page the
// insert created or restructured on the way.
//
// This lives in its own header, above both storage organizations, because
// both produce it and exactly one caller consumes it. A relation is either
// a chain of heap pages (heap_chain.hpp) or a clustered B+ tree
// (btree.hpp), and the statement layer has to log, and answer, an insert
// into either without a branch per concern. The general shape is the
// tree's - a set of structural changes of unbounded-ish size - and the
// chain's growth is the two-element special case of it (one new page, one
// link edit), so describing both this way costs the chain nothing and
// saves the caller a second code path.
//
// It is deliberately a value with an inline array and no allocation: an
// insert is the hot path, and the bound is small and known
// (kMaxStructuralChanges, derived below).

namespace kds::storage {

// One page an insert created or restructured, as the WAL needs it
// described.
struct StructuralChange {
    PageId page_id = kInvalidPageId;

    // A brand-new heap chain page, which `PAGE_INIT` describes completely
    // (page type + min_key) and which the following `HEAP_INSERT` then fills.
    //
    // Every other heap change - an old tail whose forward link was
    // repointed - is false and needs a `FULL_PAGE_IMAGE`. **A B+ tree never
    // sets it**: every page a split writes is an image in the split's one
    // `BTREE_SPLIT` record, which carries the row too (BD-R12). Structural
    // changes happen once per 8 KB of tuples, so the image volume is paid
    // per page of relation, never per tuple.
    bool is_new_page = false;

    // The new page's low key (`min_key`). Meaningless unless `is_new_page`.
    std::uint64_t min_key = 0;
};

// Deepest root-to-leaf path a B+ tree descent will follow before declaring
// the tree corrupt. With the internal node's 678-entry fanout, depth 4
// already addresses more leaves than there are page ids, so this is a
// cycle guard rather than a capacity limit - the same role kMaxChainPages
// plays for the heap chain. Lives here, not in btree.hpp, because the
// structural-change bound below is derived from it.
inline constexpr std::uint16_t kMaxBtreeDepth = 16;

// How many times a write descent may start over, having found its leaf no
// longer covers its key, before it gives up and asks the caller to retry
// (AT-S5c for the clustered tree, AT-S15 for the index tree). Each tree's
// `DescendTo` carries its own progress argument; the bound is here so that
// a pathological stream of splits - or a stale root, which fails the same
// way every attempt - ends in a refusal a client can read rather than in a
// loop nothing reports.
inline constexpr int kMaxDescentRestarts = 4;

// One insert records at most: the new page, the old page whose link moved,
// **two** nodes per level it propagated a split back up (`depth <=
// kMaxBtreeDepth - 1`, since depth indexes a kMaxBtreeDepth-element path),
// and one new root.
//
// Two per level, not one, since PK09: a full internal node is *divided* -
// its upper half moved to a new node and the lower half written back - so
// both pages change and both need an image. The right-split-with-no-movement
// case still touches only one, and this bound covers the worse of the two.
inline constexpr std::size_t kMaxStructuralChanges =
    2 + 2 * (static_cast<std::size_t>(kMaxBtreeDepth) - 1) + 1;

struct InsertPlacement {
    PageId page_id = kInvalidPageId;  // the page the tuple landed in
    std::uint16_t slot = 0;           // its slot within that page

    // Set when a B+ tree grew a level: the caller must repoint the
    // relation's `sys.tables.desc_page_id` at it
    // (`Catalog::UpdateRelationDescPage`, which exists for this). Returned
    // rather than written by the storage layer because that layer has no
    // catalog - and because a root published before its contents are
    // logged is a root recovery cannot reach. Always kInvalidPageId for a
    // heap chain, which has no root to move.
    PageId new_root = kInvalidPageId;

    std::array<StructuralChange, kMaxStructuralChanges> structural{};
    std::uint8_t n_structural = 0;

    // **The pages the insert changed, still held** (AT-S21, `wal.md` §8-1):
    // the page the tuple landed in and every page it rewrote that another
    // writer could reach - a B+ tree's leaf and the parents `SecureParents`
    // took, a heap chain's tail. A page the insert created needs no hold of
    // its own: it is reachable only through one of these. The caller
    // appends and stamps every record naming them, then drops the holds -
    // before a commit's durability wait, which must not run under a latch.
    // Empty on a placement that holds nothing.
    std::vector<PageRef> held;

    // A heap chain's in the order redo has to apply them, before the
    // `HEAP_INSERT` that describes the tuple itself; a B+ tree's, the pages
    // of its one `BTREE_SPLIT`. Empty for the overwhelmingly common insert
    // that just filled a slot.
    std::span<const StructuralChange> changes() const {
        return std::span<const StructuralChange>(structural.data(), n_structural);
    }
    bool restructured() const { return n_structural > 0; }

    void Record(PageId page_id_in, bool is_new_page, std::uint64_t min_key) {
        // Bounded by kMaxStructuralChanges above, which is a derivation and
        // not a hope. Dropping a change silently would be a page mutated
        // with no record describing it - the exact hole the WAL exists to
        // close - so this deliberately has no "if full, skip" arm.
        structural[n_structural++] = StructuralChange{page_id_in, is_new_page, min_key};
    }
};

// ---- The id fixed under the hold of the page it lands on (BB-R1) ---------
//
// `instructions/v3.0.0/workorder-bb-issue-under-the-leaf.md`. A user row's id
// is fixed under the exclusive hold of the page the row lands on - a btree's
// rightmost leaf, a heap chain's tail held as the tail (BB-R7) - so placement
// order is issue order and every page's slot order is its key order, at
// every core count. Until BB the
// id was fixed under catalog page 7 and the row placed later under its page,
// and a second core could fix a higher id and place it first in between
// (defect A).
//
// The storage layer has no catalog, so the two things only the caller can do
// under that hold are handed in as callables. Both run **once**, under the
// exclusive hold, and **must not park** (BB-R5): a refusal is returned, and
// the insert returns it with every hold released and nothing placed. Both are
// `FunctionRef`s - a reference, no allocation per row - so a caller binds a
// named callable, or a temporary in the call's own expression.

// An omitted pk (BB-R2): issue the id, borrow its lock, encode the row, and
// return the encoded payload - whose Keystone word carries the issued id and
// whose bytes stay valid until the insert returns.
using IssueUnderHold = FunctionRef<StatusOr<std::span<const std::byte>>()>;

// A named key (BB-R3 step 7): admit `id` against the relation's mark,
// moving it past `id`. Asked only once the structure has proved `id` absent
// from the held page and the held page the last one - below it, a key is
// refused without the mark being read.
using AdmitUnderHold = FunctionRef<Status(std::uint64_t id)>;

}  // namespace kds::storage
