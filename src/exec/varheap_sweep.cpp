#include "kds/exec/varheap_sweep.hpp"

#include "kds/exec/catalog_spills.hpp"

#include <set>
#include <utility>

#include "kds/catalog/well_known.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/tagged_cell.hpp"
#include "kds/storage/visit.hpp"
#include "kds/storage/varheap.hpp"
#include "kds/txn/varheap_release.hpp"

namespace kds::exec {
namespace {

// Releases every slot of the relation's var-heap chain that `referenced`
// does not name.
Status SweepChain(const catalog::TableAccess& access, storage::PageStore& store,
                  wal::WalManager* wal, const std::set<SpillRef>& referenced,
                  VarHeapSweepReport& report) {
    PageId page_id = access.varheap_page_id;
    for (std::uint32_t hops = 0; page_id != kInvalidPageId; ++hops) {
        if (Status s = storage::CheckPageWalkBudget(hops, access.varheap_page_id, "var-heap chain");
            !s.ok()) {
            return s;
        }
        auto bytes = store.GetForRead(page_id);
        if (!bytes.ok()) return bytes.status();
        const std::uint16_t slots = varheap::PageSlotCount(bytes.value().bytes());
        const PageId next = varheap::PageNextPageId(bytes.value().bytes());
        ++report.pages;

        for (std::uint16_t slot = 0; slot < slots; ++slot) {
            if (referenced.count({page_id, slot}) != 0) {
                ++report.retained;
                continue;
            }
            // **`kNoTxnId`, because no transaction owns this collection** -
            // `varheap_release.hpp`'s split, purge-drain arm. The release
            // is idempotent, so a crash mid-sweep replays it harmlessly and
            // the next mount finishes the job.
            auto released =
                txn::ReleaseVarHeapSlot(store, wal, wal::kNoTxnId, page_id, slot);
            if (!released.ok()) return released.status();
            // A slot that was already a tombstone reports `kReleased` too
            // (the release is idempotent and cannot tell), so this counts
            // *collections attempted*, not bytes recovered. Stated rather
            // than over-claimed: on a relation that has never rolled back,
            // this is the count of slots already dead.
            ++report.released;
        }
        page_id = next;
    }
    return Status::OK();
}

}  // namespace

StatusOr<VarHeapSweepReport> SweepUnownedSpills(catalog::Catalog& catalog,
                                                storage::PageStore& store,
                                                wal::WalManager* wal) {
    VarHeapSweepReport report;
    // `kVarHeapCatalogRelations` (`catalog_spills.hpp`) is the shared list,
    // and **this sweep is the consumer with the stricter test**: every
    // entry's spills must be logged under `wal::kNoTxnId`, because a
    // relation whose spills a transaction owns is already released by
    // rollback and undo and sweeping it would be a second authority over
    // the same bytes. That list's comment carries the rule; adding an entry
    // there is a decision about this file too.
    for (catalog::Oid oid : kVarHeapCatalogRelations) {
        auto access = catalog.InitTableAccess(oid);
        if (!access.ok()) {
            // A catalog relation this build knows and this file does not
            // have is not a sweep failure - a fresh instance may not have
            // materialized every one. Skipped, not reported as damage.
            if (access.status().code() == StatusCode::kNotFound) continue;
            return access.status();
        }
        // Nothing has spilled into this relation yet, so there is no chain.
        if (access.value()->varheap_page_id == kInvalidPageId) continue;

        std::set<SpillRef> referenced;
        if (Status s = ReferencedSpills(*access.value(), store, referenced); !s.ok()) {
            return s.WithContext("var-heap sweep: collecting live references of relation oid " +
                                 std::to_string(oid));
        }
        if (Status s = SweepChain(*access.value(), store, wal, referenced, report); !s.ok()) {
            return s.WithContext("var-heap sweep: relation oid " + std::to_string(oid));
        }
    }
    return report;
}

}  // namespace kds::exec
