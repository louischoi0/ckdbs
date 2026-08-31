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

// Which var-heap slots a catalog relation's rows point at - the one walk two
// unrelated mechanisms both need, and which each used to carry its own copy
// of.
//
// The two consumers, so a change here is made against both:
//
//   - **The mount sweep** (`varheap_sweep.hpp`) compares what the pages hold
//     against what the rows point at, and collects the difference.
//   - **A peer's fault grant** (`CoreRuntime::Open`) reads the page ids its
//     rows name and grants itself read rights over exactly those, because a
//     catalog relation's var-heap sits *outside* the reserved range and is
//     therefore not covered by the system-range arm of `MayFault`
//     (`crosscore.md` CC12/CR1: the root page is reserved so bootstrap can
//     find it, the var-heap is not).
//
// **The ids the rows name, with no fetch of any of them.** That is the
// property the grant depends on: it runs where the fetch is not yet
// permitted, which is the whole point of it.

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

// The distinct var-heap pages `relations`' rows point into, in ascending id
// order.
//
// A relation with no var-heap chain - nothing has spilled into it yet -
// contributes nothing and is not an error, and neither is one this build
// knows and this instance has not materialized (`kNotFound`, skipped).
//
// **Granted page by page and never as the extent around them.** A range
// grant covers pages this core may *own*, and a page that answers
// `MayFault` from a grant never reaches `TryClaimByStamp`, so the peer would
// silently lose the write rights PW1c-7 restores to it on the fault - a
// restarted owner unable to write its own relation. That was measured, not
// reasoned: an extent-wide grant here failed
// `APeersOwnPagesSurviveARestartByTheirStamp`.
StatusOr<std::vector<PageId>> CatalogSpillPages(catalog::Catalog& catalog,
                                                storage::PageStore& store,
                                                std::span<const catalog::Oid> relations);

}  // namespace kds::exec
