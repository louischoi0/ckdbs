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

    // The two chain-link fields ride as payload *fields* and are not
    // repeated inside `image` - which is why payload.hpp's note that
    // UNDO_WRITE's prior writer is a different transaction from the
    // envelope's is already correct (txn.md section 3.5).
    std::vector<std::byte> buf(UndoWriteSize(image.size()));
    const wal::UndoWritePayload payload{fields.prior_trx_id, fields.prior_undo_ptr, offset,
                                        static_cast<std::uint16_t>(image.size())};
    if (auto n = wal::EncodeUndoWrite(buf, payload, image); !n.ok()) return n.status();

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
        if (UndoPageFreeSpace(std::span<const std::byte, kPageSize>(bytes.value())) >= need) {
            return tail_;
        }
    }

    // No page yet, or the current one cannot hold this record. Grow.
    auto created = store_.CreateNew();
    if (!created.ok()) return created.status();
    const PageId page_id = created.value().first;

    if (Status s = FormatUndoPage(created.value().second, trx_id, tail_); !s.ok()) return s;
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

    auto offset = UndoPageAppend(bytes.value(), fields, image);
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

    auto rec = UndoPageRead(std::span<const std::byte, kPageSize>(bytes.value()),
                            UndoPtrOffset(ptr));
    if (!rec.ok()) return rec.status();

    UndoVersion out;
    out.type = static_cast<UndoRecordType>(rec.value().fields.type);
    out.prior_trx_id = rec.value().fields.prior_trx_id;
    out.prior_undo_ptr = rec.value().fields.prior_undo_ptr;
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
        page_id = ReadUndoPageHeader(std::span<const std::byte, kPageSize>(bytes.value()))
                      .prev_page_id;
    }
    return count;
}

}  // namespace kds::txn
