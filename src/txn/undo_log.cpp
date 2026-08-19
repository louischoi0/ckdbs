#include "kds/txn/undo_log.hpp"

#include <array>
#include <cstring>
#include <string>

#include "kds/wal/log_page_init.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

namespace kds::txn {

namespace {

// The payload buffer an UNDO_WRITE needs: the fixed prefix plus the image.
std::size_t UndoWriteSize(std::size_t image_len) {
    return wal::kUndoWriteFixedSize + image_len;
}

}  // namespace

StatusOr<wal::Lsn> UndoLog::LogPageInit(std::uint64_t trx_id, PageId page_id) {
    // min_key 0 (an undo page has no key space), owner_oid 0 (a system
    // page class, unattributed by page.md §2a - and it must match what
    // UndoPage::Format() stamps on the live path, which is 0).
    return wal::LogPageInit(wal_, trx_id, page_id, PageType::kUndo, /*min_key=*/0,
                            /*owner_oid=*/0);
}

Status UndoLog::StampInit(PageId page_id, wal::Lsn lsn) {
    if (lsn == wal::kNoLsn) return Status::OK();  // the unlogged path
    return store_.StampPageLsn(page_id, lsn);
}

Status UndoLog::LogUndoWrite(std::uint64_t trx_id, PageId page_id, std::uint16_t offset,
                             const UndoRecordFields& fields,
                             std::span<const std::byte> image) {
    if (wal_ == nullptr) return Status::OK();

    // What the record carries is the undo record's **tail** - its bytes
    // from `target_page_id` onward - and not the bare before-image. The two
    // chain-link fields ride as payload *fields* instead, because
    // prior_trx_id names a different transaction from the envelope's and
    // repeating it inside the bytes would store one fact twice.
    //
    // That split is txn.md section 3.5's `payload.tail = record bytes
    // [+16, +28 + image_len)`, and until 2026-08-10 this function logged
    // the image alone - which left `target_page_id`, `target_slot` and
    // `type` on the page and nowhere in the log, so redo could rebuild a
    // chain that named no tuple (docs/workplan-wal-recovery.md RC03). The
    // spec was right and the code was not.
    //
    // Safe to change without a format version precisely because nothing has
    // ever read the log back: recovery does not exist, so no stream in
    // existence is ever interpreted under the old reading.
    std::vector<std::byte> tail(UndoRecordTailSize(image.size()));
    if (Status s = EncodeUndoRecordTail(tail, fields, image); !s.ok()) return s;

    std::vector<std::byte> buf(UndoWriteSize(tail.size()));
    const wal::UndoWritePayload payload{fields.prior_trx_id, fields.prior_undo_ptr, offset,
                                        static_cast<std::uint16_t>(tail.size())};
    if (auto n = wal::EncodeUndoWrite(buf, payload, tail); !n.ok()) return n.status();

    // The envelope's page_id is the **undo** page, not the heap page the
    // record describes. The heap page gets its own record.
    auto rec = wal_->Append(wal::RecordSpec{wal::RecordType::kUndoWrite, trx_id, page_id}, buf);
    if (!rec.ok()) return rec.status();
    return store_.StampPageLsn(page_id, rec.value());
}

void UndoLog::ReclaimSettledPages() {
    if (!horizon_) return;
    const std::uint64_t horizon = horizon_();
    // `settled + 1 < size`: the tail is never taken, whatever its bound -
    // its free space is what the caller is about to write into.
    std::size_t settled = 0;
    while (settled + 1 < pages_.size() && pages_[settled].max_trx_id < horizon) {
        recycle_.push_back(pages_[settled].page_id);
        ++settled;
    }
    if (settled > 0) pages_.erase(pages_.begin(), pages_.begin() + settled);
}

StatusOr<PageId> UndoLog::TailFor(std::uint64_t trx_id, std::size_t need) {
    // The current page, whoever last wrote to it. `trx_id` decides nothing
    // about *which* page is used - only what goes in the new page's
    // `first_trx_id` and in the PAGE_INIT envelope if one has to be created.
    if (tail_ != kInvalidPageId) {
        auto bytes = store_.Get(tail_);
        if (!bytes.ok()) return bytes.status();
        if (UndoPageFreeSpace(std::span<const std::byte, kPageSize>(bytes.value().bytes())) >= need) {
            return tail_;
        }
    }

    // No page yet, or the current one cannot hold this record. Grow - and
    // growth is the purge trigger (D3): reclaim settled pages first, then
    // prefer a reclaimed page to a new one. Work lands exactly where the
    // resource is consumed, and a workload writing no undo pays nothing.
    ReclaimSettledPages();

    if (!recycle_.empty()) {
        const PageId page_id = recycle_.back();
        auto bytes = store_.Get(page_id);
        if (!bytes.ok()) return bytes.status();
        // The same PAGE_INIT record type as the fresh path below: redo
        // replays the page's old records, then this init wipes them, then
        // the new ones land - the end state is right in every crash
        // position, and the old records were unreachable before the wipe
        // by the header's reachability fact.
        //
        // **Logged before the wipe**, which is the one place this arm must
        // differ from the fresh one. FormatUndoPage memsets the frame,
        // `page_lsn` included, and on a reclaimed page those bytes are the
        // only copy of records a crash loser's `txn_prev_undo_ptr` chain
        // may still name (its writer committed relaxed, so the horizon
        // passed it while its TXN_COMMIT sat undurable). Formatting first
        // would leave a failed append holding a wiped, dirty frame whose
        // `page_lsn` of 0 the WAL gate waves through - the wipe reaching
        // the device with no record describing it. Redo is unaffected: the
        // LSN order is the same either way.
        auto init = LogPageInit(trx_id, page_id);
        if (!init.ok()) return init.status();
        if (Status s = FormatUndoPage(bytes.value().bytes(), trx_id, tail_); !s.ok()) return s;
        if (Status s = StampInit(page_id, init.value()); !s.ok()) return s;
        recycle_.pop_back();  // popped only on success, like tail_ below
        ++pages_recycled_;
        pages_.push_back(TrackedPage{page_id, 0});
        tail_ = page_id;
        return page_id;
    }

    auto created = store_.CreateNew();
    if (!created.ok()) return created.status();
    const PageId page_id = created.value().first;

    // Format first here, unlike the reclaim arm above: a page CreateNew()
    // just handed out holds nothing a failed append could lose, and
    // leaving it unformatted would put an allocated page with no page_type
    // on the device.
    if (Status s = FormatUndoPage(created.value().second.bytes(), trx_id, tail_); !s.ok()) return s;
    auto init = LogPageInit(trx_id, page_id);
    if (!init.ok()) return init.status();
    if (Status s = StampInit(page_id, init.value()); !s.ok()) return s;

    // Published only once the page is formatted and its PAGE_INIT is
    // logged: a failure above leaves the log pointing at the page that was
    // working before, never at one recovery has not been told about.
    pages_.push_back(TrackedPage{page_id, 0});
    tail_ = page_id;
    return page_id;
}

StatusOr<std::uint64_t> UndoLog::Append(std::uint64_t trx_id, const UndoRecordFields& fields,
                                         std::span<const std::byte> image) {
    if (image.size() > kMaxUndoImageLen) {
        // Refused here rather than after a page has been allocated for it,
        // so an oversize image does not leak a page. The message names the
        // undo page for the reason UndoPageAppend's does.
        return Status::InvalidArgument("undo image of " + std::to_string(image.size()) +
                                       " bytes exceeds the " + std::to_string(kMaxUndoImageLen) +
                                       "-byte undo page capacity");
    }

    auto page_id = TailFor(trx_id, kUndoRecordHeaderSize + image.size());
    if (!page_id.ok()) return page_id.status();

    // The purge bound, raised before the record lands: a failure below
    // leaves the bound conservatively high, never a record above the
    // bound. TailFor guarantees the tail is tracked.
    if (pages_.back().max_trx_id < trx_id) pages_.back().max_trx_id = trx_id;

    auto bytes = store_.Get(page_id.value());
    if (!bytes.ok()) return bytes.status();

    auto offset = UndoPageAppend(bytes.value().bytes(), fields, image);
    if (!offset.ok()) return offset.status();

    if (Status s = LogUndoWrite(trx_id, page_id.value(), offset.value(), fields, image);
        !s.ok()) {
        return s;
    }
    return EncodeUndoPtr(page_id.value(), offset.value());
}

StatusOr<UndoVersion> UndoLog::Read(std::uint64_t ptr) {
    if (Status s = UndoPtrIsPlausible(ptr); !s.ok()) return s;

    auto bytes = store_.GetForRead(UndoPtrPageId(ptr));
    if (!bytes.ok()) return bytes.status();

    auto rec = UndoPageRead(std::span<const std::byte, kPageSize>(bytes.value().bytes()),
                            UndoPtrOffset(ptr));
    if (!rec.ok()) return rec.status();

    UndoVersion out;
    out.type = static_cast<UndoRecordType>(rec.value().fields.type);
    out.prior_trx_id = rec.value().fields.prior_trx_id;
    out.prior_undo_ptr = rec.value().fields.prior_undo_ptr;
    out.target_page_id = rec.value().fields.target_page_id;
    out.target_slot = rec.value().fields.target_slot;
    out.txn_prev_undo_ptr = rec.value().fields.txn_prev_undo_ptr;
    out.pk = rec.value().fields.pk;
    // Copied, not viewed: the next step of a walk fetches another page.
    out.image.assign(rec.value().image.begin(), rec.value().image.end());
    return out;
}

Status UndoLog::Walk(std::uint64_t ptr, const std::function<bool(const UndoVersion&)>& fn) {
    std::uint32_t steps = 0;
    while (ptr != kNoUndoPtr) {
        if (++steps > kMaxUndoChainLength) {
            return Status::Corruption("undo chain exceeds " +
                                      std::to_string(kMaxUndoChainLength) +
                                      " versions; treating it as a cycle");
        }
        auto version = Read(ptr);
        if (!version.ok()) return version.status();
        if (!fn(version.value())) return Status::OK();
        ptr = version.value().prior_undo_ptr;
    }
    return Status::OK();
}

}  // namespace kds::txn
