#include "kds/wal/high_water.hpp"

#include <string>

namespace kds::wal {

StatusOr<HighWaterRepair> RaiseHighWater(storage::PageStore& store,
                                         const AnalysisResult& analysis) {
    HighWaterRepair out;

    // The transaction half is arithmetic and never fails: ids are 48-bit
    // (invariant 12) and `heap::kMaxTrxId` is far below the point a +1
    // could wrap a uint64. Exhaustion is TrxIdSequence's to report, at the
    // moment it would issue the id, not this function's to pre-empt.
    if (analysis.max_txn_id != 0) {
        out.next_trx_id = analysis.max_txn_id + 1;
    }

    if (analysis.max_page_id == kInvalidPageId) {
        // No record in the range named a page. Nothing to raise, and
        // nothing wrong: a stream of transaction and checkpoint records is
        // exactly what a clean shutdown leaves behind.
        return out;
    }
    if (analysis.max_page_id == kInvalidPageId - 1) {
        // The floor would be the reserved invalid id, so no legal floor
        // exists. Reported here rather than let through as a floor of
        // 0xFFFFFFFF, which every range check downstream would read as
        // "unset" rather than as "one past the end".
        return Status::Corruption(
            "wal high-water: the log names page id " + std::to_string(analysis.max_page_id) +
            ", whose successor is the reserved invalid page id (invariant 1)");
    }

    out.page_floor = analysis.max_page_id + 1;
    if (Status s = store.RaiseAllocationFloor(out.page_floor); !s.ok()) {
        return s.WithContext("wal high-water: raising the allocation floor past page " +
                             std::to_string(analysis.max_page_id) + ", the highest the log names");
    }
    out.page_floor_raised = true;
    return out;
}

}  // namespace kds::wal
