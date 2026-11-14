#include "kds/txn/row_compensation.hpp"

#include <array>
#include <vector>

#include "kds/wal/payload.hpp"

namespace kds::txn {

namespace {

// The record behind a compensation, and the stamp behind the record.
Status AppendAndStamp(storage::PageStore& store, wal::WalManager& wal, wal::RecordType type,
                      std::uint64_t txn_id, PageId page_id, std::span<const std::byte> payload) {
    auto rec = wal.Append(wal::RecordSpec{type, txn_id, page_id}, payload);
    if (!rec.ok()) return rec.status();
    return store.StampPageLsn(page_id, rec.value());
}

}  // namespace

Status CompensateInsert(storage::PageStore& store, wal::WalManager* wal, std::uint64_t txn_id,
                        PageId page_id, heap::PageView& page, std::uint16_t slot) {
    if (Status s = page.RetireSlot(slot); !s.ok()) return s;
    if (wal == nullptr) return Status::OK();
    std::array<std::byte, wal::kSlotRetirePayloadSize> buf{};
    if (auto n = wal::EncodeSlotRetire(buf, wal::SlotRetirePayload{slot}); !n.ok()) {
        return n.status();
    }
    return AppendAndStamp(store, *wal, wal::RecordType::kSlotRetire, txn_id, page_id, buf);
}

Status CompensateOverwrite(storage::PageStore& store, wal::WalManager* wal, std::uint64_t txn_id,
                           PageId page_id, heap::PageView& page, std::uint16_t slot,
                           std::span<const std::byte> image, std::uint64_t prior_trx_id,
                           std::uint64_t prior_undo_ptr) {
    if (Status s = page.OverwriteTuple(slot, image, prior_trx_id, prior_undo_ptr); !s.ok()) {
        return s;
    }
    if (wal == nullptr) return Status::OK();
    std::vector<std::byte> buf(wal::kHeapWriteFixedSize + image.size());
    const wal::HeapWritePayload fields{prior_trx_id, prior_undo_ptr, slot,
                                       static_cast<std::uint16_t>(image.size())};
    if (auto n = wal::EncodeHeapWrite(buf, fields, image); !n.ok()) return n.status();
    return AppendAndStamp(store, *wal, wal::RecordType::kHeapOverwrite, txn_id, page_id, buf);
}

Status CompensateDeleteMark(storage::PageStore& store, wal::WalManager* wal, std::uint64_t txn_id,
                            PageId page_id, heap::PageView& page, std::uint16_t slot,
                            std::uint64_t prior_trx_id, std::uint64_t prior_undo_ptr) {
    if (Status s = page.ClearDeleteMark(slot, prior_trx_id, prior_undo_ptr); !s.ok()) return s;
    if (wal == nullptr) return Status::OK();
    // **HEAP_DELETE_UNMARK, not HEAP_DELETE_MARK.** The live rollback logged
    // the mark record until 2026-08-11, which redo replays by *setting* a
    // mark - so a crash after the rollback brought the row back deleted, the
    // abort undone by its own compensation. The two directions are two
    // record types (`record.hpp`).
    std::array<std::byte, wal::kDeleteUnmarkPayloadSize> buf{};
    const wal::HeapDeleteUnmarkPayload fields{prior_trx_id, prior_undo_ptr, slot};
    if (auto n = wal::EncodeHeapDeleteUnmark(buf, fields); !n.ok()) return n.status();
    return AppendAndStamp(store, *wal, wal::RecordType::kHeapDeleteUnmark, txn_id, page_id, buf);
}

}  // namespace kds::txn
