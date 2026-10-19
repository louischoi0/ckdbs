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
// **One consumer**, the mount sweep (`varheap_sweep.hpp`), which compares
// what the pages hold against what the rows point at and collects the
// difference. The fault-grant consumer and its `CatalogSpillPages` wrapper
// retired (AW-S1b); their history is at cf889b63.

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
// Every entry - the three declaration relations (`declaration_relation.hpp`)
// - satisfies both: each logs its spills through `LogChainInsert` at
// `kBootstrapXid`, which is `kNoTxnId`. A relation that satisfies one test
// and not the other needs its own list, not an entry here.
//
// **A list, not a constant**: a constant would put the two consumers back
// to naming the relation themselves (the duplication CB2 removed), and the
// next catalog relation to gain a var-heap joins here.
inline constexpr catalog::Oid kVarHeapCatalogRelations[] = {
    catalog::kSysAssertionsTable,
    catalog::kSysTransitionsTable,
    catalog::kSysCabinExprsTable,
};

// The var-heap slots one row's payload points at, appended to `out` in
// column order. Fails on a cell it cannot decode. **Not catalog-only**:
// `PURGE` reads a user row's spills through it (BH-R7), so the sweep and
// the purge share one decoder.
Status RowSpills(const catalog::TableAccess& access, std::span<const std::byte> payload,
                 std::vector<SpillRef>& out);

// The var-heap slot one column's cell points at, if any, appended to `out`:
// `RowSpills`' body, for a caller that releases one column's values alone
// (CD-S2's clean of the dropped cells).
Status ColumnSpills(const catalog::TableAccess& access, std::span<const std::byte> payload,
                    std::size_t col, std::vector<SpillRef>& out);

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
