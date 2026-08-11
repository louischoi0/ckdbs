#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "kds/base/common.hpp"

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

    // A brand-new tuple page, which `PAGE_INIT` describes completely (page
    // type + min_key) and which the following `HEAP_INSERT` then fills.
    //
    // Every other change - a new B+ tree internal node, a modified one, an
    // old page whose forward link was repointed - is false and needs a
    // `FULL_PAGE_IMAGE`, because no record type describes an internal
    // node's entry array or a link edit on its own (record.hpp's enum is
    // frozen and append-only; BTREE_INSERT/BTREE_SPLIT are deliberately
    // left unassigned there). Structural changes happen once per 8 KB of
    // tuples, so the FPI volume is paid per page of relation, never per
    // tuple.
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

    // In the order redo has to apply them, and always before the
    // `HEAP_INSERT` that describes the tuple itself. Empty for the
    // overwhelmingly common insert that just filled a slot.
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

}  // namespace kds::storage
