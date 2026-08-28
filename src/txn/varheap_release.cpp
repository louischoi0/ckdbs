#include "kds/txn/varheap_release.hpp"

#include <array>
#include <string>

#include "kds/storage/varheap.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

namespace kds::txn {

StatusOr<ReleaseOutcome> ReleaseVarHeapSlot(storage::PageStore& store, wal::WalManager* wal,
                                            std::uint64_t txn_id, PageId page_id,
                                            std::uint16_t slot) {
    auto page = store.Get(page_id);
    if (!page.ok()) {
        // The page the append landed in does not exist. Under recovery that
        // means its PAGE_INIT was never redone, so neither was the append -
        // see ReleaseOutcome::kNothingToRelease. Any other failure of the
        // store is reported as itself.
        if (page.status().code() == StatusCode::kNotFound) {
            return ReleaseOutcome::kNothingToRelease;
        }
        return page.status().WithContext("var-heap page " + std::to_string(page_id) +
                                         " named by a release");
    }

    // The slot's absence is distinguished from a damaged page here, and only
    // here: `PageRelease` answers Corruption for both "not a kVarHeap page"
    // and "slot past the directory", and only the second is a legitimate
    // recovery state. So the count is read first and the guard is left to
    // do the rest.
    if (slot >= varheap::PageSlotCount(page.value().bytes())) {
        return ReleaseOutcome::kNothingToRelease;
    }

    if (Status s = varheap::PageRelease(page.value().bytes(), slot); !s.ok()) return s;
    if (wal == nullptr) return ReleaseOutcome::kReleased;

    std::array<std::byte, wal::kVarHeapReleasePayloadSize> buf{};
    if (auto n = wal::EncodeVarHeapRelease(buf, wal::VarHeapReleasePayload{slot}); !n.ok()) {
        return n.status();
    }
    auto record = wal->Append(wal::RecordSpec{wal::RecordType::kVarHeapRelease, txn_id, page_id},
                              buf);
    if (!record.ok()) return record.status();
    if (Status s = store.StampPageLsn(page_id, record.value()); !s.ok()) return s;
    return ReleaseOutcome::kReleased;
}

}  // namespace kds::txn
