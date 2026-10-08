#include "kds/exec/purge_key.hpp"

#include <string>
#include <vector>

#include "kds/exec/catalog_spills.hpp"
#include "kds/exec/wal_row_log.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/txn/varheap_release.hpp"

namespace kds::exec {

StatusOr<PurgeOutcome> PurgeKey(storage::PageStore& store, wal::WalManager* wal,
                                const catalog::TableAccess& access, std::uint64_t id,
                                std::uint64_t deleter_trx_id) {
    auto held = btree::BtreeHoldTombstone(store, access.desc_page_id, id, deleter_trx_id);
    if (!held.ok()) return held.status();
    if (!held.value().has_value()) return PurgeOutcome::kSkipped;
    btree::Location& tomb = *held.value();
    heap::PageView leaf(tomb.leaf.bytes());

    // Decoded under the hold and before anything is written, so a cell that
    // cannot be read refuses the key with the page untouched. Only the
    // references outlive the hold.
    std::vector<SpillRef> spills;
    {
        auto tuple = leaf.ReadTuple(tomb.slot);
        if (!tuple.ok()) return tuple.status();
        if (Status s = RowSpills(access, tuple.value().payload, spills); !s.ok()) return s;
    }

    // The retire and its record, under the hold (AT-S21).
    if (Status s = leaf.RetireSlot(tomb.slot); !s.ok()) return s;
    if (Status s = LogSlotRetire(wal, store, wal::kNoTxnId, tomb.page_id, tomb.slot); !s.ok()) {
        return s;
    }
    tomb.leaf.Release();  // the hold goes before any var-heap page is fetched

    for (const auto& [page_id, slot] : spills) {
        auto released = txn::ReleaseVarHeapSlot(store, wal, wal::kNoTxnId, page_id, slot);
        if (!released.ok()) return released.status();
        // In-process the slot was written by the row's own insert or
        // update, so its absence is a defect, not a crash (the outcome's
        // own rule in `varheap_release.hpp`).
        if (released.value() == txn::ReleaseOutcome::kNothingToRelease) {
            return Status::Corruption("purge of primary key " + std::to_string(id) +
                                      ": its spill at page " + std::to_string(page_id) +
                                      " slot " + std::to_string(slot) + " does not exist");
        }
    }
    return PurgeOutcome::kPurged;
}

}  // namespace kds::exec
