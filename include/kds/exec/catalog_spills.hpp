#pragma once

#include <cstdint>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/storage/page_store.hpp"

// Which var-heap slots a catalog relation's rows point at.
//
// **One consumer since AW-S1b**, the mount sweep (`varheap_sweep.hpp`),
// which compares what the pages hold against what the rows point at and
// collects the difference. This header opened "the one walk two unrelated
// mechanisms both need": the second was a peer's fault grant, which read
// the page ids its rows name and granted itself read rights over exactly
// those, because a catalog relation's var-heap sits *outside* the reserved
// range and was therefore not covered by the system-range arm of the fault
// predicate (`crosscore.md` CC12/CR1: the root page is reserved so
// bootstrap can find it, the var-heap is not). One frame table serves every
// core now, so there is no right to grant, and `CatalogSpillPages` - the
// walk-every-relation wrapper that existed for it - went with it.

namespace kds::exec {

// A spill pointer, as a key a referenced set can hold.
using SpillRef = std::pair<PageId, std::uint16_t>;

// The catalog relations that have a var-heap at all. Named rather than
// discovered, because both consumers are sensitive to what is on this list
// and in different ways - **check both tests before adding a second**:
//
//   - the **sweep** requires that the relation's spills are logged under
//     `wal::kNoTxnId` (`exec::LogChainInsert`), since a relation whose
//     spills a transaction owns is already released by rollback and undo,
//     and sweeping it would put a second authority over the same bytes;
//   - the **grant** requires only that a peer reads the relation.
//
// The single entry satisfies both. A relation that satisfies one and not
// the other needs its own list, not an entry here.
//
// **It is a list of one, and stays a list.** `sys.pattern_defs` was the
// other entry until the operator withdrew declared patterns on 2026-08-31;
// collapsing the list into a constant would put the two consumers back to
// naming the relation themselves, which is the duplication CB2 removed, and
// the next catalog relation to gain a var-heap joins here by CR1.
inline constexpr catalog::Oid kVarHeapCatalogRelations[] = {
    catalog::kSysAssertionsTable,
};

// Every var-heap slot the relation's **live** rows point at.
//
// Delete-marked rows count as live references, deliberately: nothing retires
// a heap slot yet (`known-gaps.md`, reclamation), so a delete-marked row is
// still readable by an older snapshot and its spilled value must still
// resolve. Treating such a reference as absent would let the sweep turn a
// leak into a wrong answer, and would let the grant miss a page a peer can
// still be asked to read.
Status ReferencedSpills(const catalog::TableAccess& access, storage::PageStore& store,
                        std::set<SpillRef>& out);

}  // namespace kds::exec
