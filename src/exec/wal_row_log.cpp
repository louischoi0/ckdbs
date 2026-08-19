#include "kds/exec/wal_row_log.hpp"

#include <array>

#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/log_page_image.hpp"
#include "kds/wal/log_page_init.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

namespace kds::exec {

Status LogSpills(wal::WalManager* wal, storage::PageStore& store,
                 const std::vector<AppendedSpill>& spills, std::uint64_t env_txn,
                 std::uint64_t owner_oid) {
    if (wal == nullptr) return Status::OK();

    for (const AppendedSpill& spill : spills) {
        // ---- The page the append created ---------------------------------
        //
        // `varheap::ChainAppend` grows a chain through the store's plain
        // allocation path, and a VARHEAP_APPEND does not say its page is new -
        // so without this record redo meets an append naming a page nothing
        // creates, and refuses the mount. `wal::ApplyPageInit` already formats
        // a kVarHeap page, so this is the record nobody wrote rather than an
        // applier nobody built.
        //
        // Unstamped, for the reason the heap path gives for a new tuple page:
        // the append below lands in exactly this page and stamps it.
        if (spill.created_page_id != kInvalidPageId) {
            if (auto rec = wal::LogPageInit(wal, env_txn, spill.created_page_id,
                                            PageType::kVarHeap, /*min_key=*/0, owner_oid);
                !rec.ok()) {
                return rec.status();
            }
        }

        // ---- The link that made it reachable -----------------------------
        //
        // A full page image, because no record type describes a next-page
        // link. Losing it is the quieter half of the same defect: the value
        // page survives redo and no chain walk ever reaches it.
        if (spill.linked_page_id != kInvalidPageId) {
            if (Status s = storage::LogFullPageImage(wal, store, env_txn, spill.linked_page_id);
                !s.ok()) {
                return s;
            }
        }

        // ---- The value itself --------------------------------------------
        std::vector<std::byte> vh(wal::kVarHeapAppendFixedSize + spill.value.size());
        const wal::VarHeapAppendPayload vh_fields{
            spill.ptr.slot, 0, static_cast<std::uint32_t>(spill.value.size())};
        if (auto n = wal::EncodeVarHeapAppend(vh, vh_fields, spill.value); !n.ok()) {
            return n.status();
        }
        auto rec = wal->Append(
            wal::RecordSpec{wal::RecordType::kVarHeapAppend, env_txn, spill.ptr.page_id}, vh);
        if (!rec.ok()) return rec.status();
        if (Status s = store.StampPageLsn(spill.ptr.page_id, rec.value()); !s.ok()) return s;
    }
    return Status::OK();
}

Status LogChainInsert(wal::WalManager* wal, storage::PageStore& store,
                      const heap::ChainInsertResult& placed, std::span<const std::byte> tuple,
                      std::uint64_t hdr_trx, std::uint64_t owner_oid,
                      const std::vector<AppendedSpill>& spills) {
    if (wal == nullptr) return Status::OK();

    if (placed.grew_chain) {
        // Read back off the landed page rather than derived from the
        // tuple's Keystone id (which is what ChainInsert sets it to
        // today): a change in ChainInsert's min_key choice then cannot
        // silently diverge redo from the live format (invariant 2).
        std::uint64_t min_key = 0;
        {
            auto bytes = store.GetForRead(placed.page_id);
            if (!bytes.ok()) return bytes.status();
            min_key = heap::PageView(bytes.value().bytes()).min_key();
        }
        // Unstamped: the HEAP_INSERT below lands in this page and stamps it.
        if (auto rec = wal::LogPageInit(wal, wal::kNoTxnId, placed.page_id, PageType::kHeap,
                                        min_key, owner_oid);
            !rec.ok()) {
            return rec.status();
        }
        if (placed.linked_from != kInvalidPageId) {
            if (Status s =
                    storage::LogFullPageImage(wal, store, wal::kNoTxnId, placed.linked_from);
                !s.ok()) {
                return s;
            }
        }
    }

    // The values this row points at, before the row itself (wal.md §5.2's
    // direction: a replay must never reach a cell whose pointer resolves
    // to nothing).
    if (Status s = LogSpills(wal, store, spills, wal::kNoTxnId, owner_oid); !s.ok()) return s;

    std::vector<std::byte> payload(wal::kHeapWriteFixedSize + tuple.size());
    const wal::HeapWritePayload fields{hdr_trx, /*undo_ptr=*/0, placed.slot,
                                       static_cast<std::uint16_t>(tuple.size())};
    if (auto n = wal::EncodeHeapWrite(payload, fields, tuple); !n.ok()) return n.status();
    auto rec = wal->Append(
        wal::RecordSpec{wal::RecordType::kHeapInsert, wal::kNoTxnId, placed.page_id}, payload);
    if (!rec.ok()) return rec.status();
    return store.StampPageLsn(placed.page_id, rec.value());
}

Status LogSlotRetire(wal::WalManager* wal, storage::PageStore& store, std::uint64_t env_txn,
                     PageId page_id, std::uint16_t slot) {
    if (wal == nullptr) return Status::OK();
    std::array<std::byte, wal::kSlotRetirePayloadSize> buf{};
    const wal::SlotRetirePayload fields{slot};
    if (auto n = wal::EncodeSlotRetire(buf, fields); !n.ok()) return n.status();
    auto rec = wal->Append(wal::RecordSpec{wal::RecordType::kSlotRetire, env_txn, page_id}, buf);
    if (!rec.ok()) return rec.status();
    return store.StampPageLsn(page_id, rec.value());
}

}  // namespace kds::exec
