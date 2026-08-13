#include "kds/txn/undo_log.hpp"

#include <array>
#include <cstring>
#include <string>

#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

namespace kds::txn {

namespace {

// The payload buffer an UNDO_WRITE needs: the fixed prefix plus the image.
std::size_t UndoWriteSize(std::size_t image_len) {
    return wal::kUndoWriteFixedSize + image_len;
}

}  // namespace

Status UndoLog::LogPageInit(std::uint64_t trx_id, PageId page_id) {
    if (wal_ == nullptr) return Status::OK();

    std::array<std::byte, wal::kPageInitPayloadSize> buf{};
    // min_key 0: an undo page has no key space. PageInitPayload already
    // provides for that on non-heap page types (payload.hpp).
    const wal::PageInitPayload fields{0, static_cast<std::uint8_t>(PageType::kUndo), {0, 0, 0}};
    if (auto n = wal::EncodePageInit(buf, fields); !n.ok()) return n.status();

    auto rec = wal_->Append(wal::RecordSpec{wal::RecordType::kPageInit, trx_id, page_id}, buf);
    if (!rec.ok()) return rec.status();
    return store_.StampPageLsn(page_id, rec.value());
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

    // No page yet, or the current one cannot hold this record. Grow.
    auto created = store_.CreateNew();
    if (!created.ok()) return created.status();
    const PageId page_id = created.value().first;

    if (Status s = FormatUndoPage(created.value().second.bytes(), trx_id, tail_); !s.ok()) return s;
    if (Status s = LogPageInit(trx_id, page_id); !s.ok()) return s;

    // Published only once the page is formatted and its PAGE_INIT is
    // logged: a failure above leaves the log pointing at the page that was
    // working before, never at one recovery has not been told about.
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

StatusOr<std::uint32_t> UndoLog::PageCount() {
    PageId page_id = tail_;
    std::uint32_t count = 0;
    while (page_id != kInvalidPageId) {
        if (++count > kMaxUndoChainLength) {
            return Status::Corruption("the undo page chain does not terminate");
        }
        auto bytes = store_.GetForRead(page_id);
        if (!bytes.ok()) return bytes.status();
        page_id = ReadUndoPageHeader(std::span<const std::byte, kPageSize>(bytes.value().bytes()))
                      .prev_page_id;
    }
    return count;
}

}  // namespace kds::txn
