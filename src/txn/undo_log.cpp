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
    Tail* tail = nullptr;
    for (Tail& t : tails_) {
        if (t.trx_id == trx_id) {
            tail = &t;
            break;
        }
    }

    if (tail != nullptr) {
        auto bytes = store_.Get(tail->page_id);
        if (!bytes.ok()) return bytes.status();
        if (UndoPageFreeSpace(std::span<const std::byte, kPageSize>(bytes.value())) >= need) {
            return tail->page_id;
        }
    }

    // Either the transaction has no page yet or its tail is full. Grow.
    const PageId prev = tail == nullptr ? kInvalidPageId : tail->page_id;
    auto created = store_.CreateNew();
    if (!created.ok()) return created.status();
    const PageId page_id = created.value().first;

    if (Status s = FormatUndoPage(created.value().second, trx_id, prev); !s.ok()) return s;
    if (Status s = LogPageInit(trx_id, page_id); !s.ok()) return s;

    if (tail == nullptr) {
        tails_.push_back(Tail{trx_id, page_id});
    } else {
        tail->page_id = page_id;
    }
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

StatusOr<std::uint32_t> UndoLog::PageCountFor(std::uint64_t trx_id) {
    PageId page_id = kInvalidPageId;
    for (const Tail& t : tails_) {
        if (t.trx_id == trx_id) {
            page_id = t.page_id;
            break;
        }
    }

    std::uint32_t count = 0;
    while (page_id != kInvalidPageId) {
        if (++count > kMaxUndoChainLength) {
            return Status::Corruption("undo page chain for transaction " +
                                      std::to_string(trx_id) + " does not terminate");
        }
        auto bytes = store_.GetForRead(page_id);
        if (!bytes.ok()) return bytes.status();
        page_id = ReadUndoPageHeader(std::span<const std::byte, kPageSize>(bytes.value()))
                      .prev_page_id;
    }
    return count;
}

void UndoLog::Forget(std::uint64_t trx_id) {
    for (std::size_t i = 0; i < tails_.size(); ++i) {
        if (tails_[i].trx_id == trx_id) {
            tails_[i] = tails_.back();
            tails_.pop_back();
            return;
        }
    }
}

}  // namespace kds::txn
