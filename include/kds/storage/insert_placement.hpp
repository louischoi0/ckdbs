#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/page_store.hpp"

// What one clustered-tree insert did: where the tuple landed, and every page
// the insert created or restructured on the way.
//
// It is deliberately a value with an inline array and no allocation: an
// insert is the hot path, and the bound is small and known
// (kMaxStructuralChanges, derived below).

namespace kds::storage {

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

// **A structural refusal, noted for the statement** (BA-S14, BA-R11). The
// four clustered-tree sites that give up a stale descent - descend, parent,
// secure and lookup - mark the calling thread before they return their `TxnConflict`, so the
// dispatcher can tell that refusal from a lock's or a write conflict's,
// which carry the same code. A reactor runs one statement at a time, so a
// thread-local is per statement: the dispatcher clears it before a
// statement runs and reads it after. **A caller that recovers from the
// refusal clears it** (a scan or walk fallback, the named-key pre-check),
// or a later, unrelated `TxnConflict` of the same statement would be
// re-run as though it were this one. The index tree's three sites do not
// mark: their refusal comes during index maintenance, after the row is
// placed, and a re-run would leave earlier indexes' entries behind.
namespace detail {
inline thread_local bool g_structural_refusal = false;
}  // namespace detail
inline void NoteStructuralRefusal() noexcept { detail::g_structural_refusal = true; }
inline bool StructuralRefusalNoted() noexcept { return detail::g_structural_refusal; }
inline void ClearStructuralRefusal() noexcept { detail::g_structural_refusal = false; }

// One insert records at most: the new page, the old page whose link moved,
// **two** nodes per level it propagated a split back up (`depth <=
// kMaxBtreeDepth - 1`, since depth indexes a kMaxBtreeDepth-element path),
// and one new root.
//
// Two per level: a full internal node is *divided* - its upper half moved
// to a new node and the lower half written back - so both pages change and
// both need an image. The no-movement right split touches only one; this
// bound covers the worse of the two.
inline constexpr std::size_t kMaxStructuralChanges =
    2 + 2 * (static_cast<std::size_t>(kMaxBtreeDepth) - 1) + 1;

struct InsertPlacement {
    PageId page_id = kInvalidPageId;  // the page the tuple landed in
    std::uint16_t slot = 0;           // its slot within that page

    // Set when a B+ tree grew a level: the caller must repoint the
    // relation's anchor at it, inside the split's own record
    // (`CommandDispatcher::LogSplit`, `Catalog::StageAnchorRoot`). Returned
    // rather than written by the storage layer because that layer has no
    // catalog - and because a root published before its contents are
    // logged is a root recovery cannot reach.
    PageId new_root = kInvalidPageId;

    // Every page a split wrote, by id. Each is an image in the split's one
    // `BTREE_SPLIT` record, which carries the row too (BD-R12), so an id is
    // all the caller needs: no page of a split is described by a
    // `PAGE_INIT`.
    std::array<PageId, kMaxStructuralChanges> structural{};
    std::uint8_t n_structural = 0;

    // **The pages the insert changed, still held** (AT-S21, `wal.md` §8-1):
    // the page the tuple landed in and every page it rewrote that another
    // writer could reach - the leaf and the parents `SecureParents` took.
    // A page the insert created needs no hold of its own: it is reachable
    // only through one of these. The caller appends and stamps every record
    // naming them, then drops the holds - before a commit's durability wait,
    // which must not run under a latch. Empty on a placement that holds
    // nothing.
    std::vector<PageRef> held;
    // A split that grew a root: its anchor's `UnlatchedPin`.
    UnlatchedPin anchor;

    // The pages of the split's one `BTREE_SPLIT`. Empty for the
    // overwhelmingly common insert that just filled a slot.
    std::span<const PageId> changes() const {
        return std::span<const PageId>(structural.data(), n_structural);
    }
    bool restructured() const { return n_structural > 0; }

    void Record(PageId changed) {
        // Bounded by kMaxStructuralChanges above, which is a derivation and
        // not a hope. Dropping a change silently would be a page mutated
        // with no record describing it - the exact hole the WAL exists to
        // close - so this deliberately has no "if full, skip" arm.
        structural[n_structural++] = changed;
    }
};

}  // namespace kds::storage
