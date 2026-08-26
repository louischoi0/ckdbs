#include "kds/txn/undo_page.hpp"

#include <cstring>
#include <string>

#include "kds/storage/heap/heap_page.hpp"

// rules.md #2: all access to on-disk page bytes goes through field-wise
// memcpy helpers; reinterpret_cast of struct types onto page buffers is
// forbidden. Every read/write below copies one field at a time through an
// explicit byte offset, never the mirror structs directly - the same
// discipline heap_page.cpp and varheap.cpp follow.

namespace kds::txn {

namespace {

UndoRecordFields ReadRecordHeader(std::span<const std::byte, kPageSize> page,
                                  std::uint16_t offset) {
    UndoRecordFields r{};
    const std::byte* p = page.data() + offset;
    std::memcpy(&r.prior_trx_id, p + kUndoRecPriorTrxIdOffset, sizeof(r.prior_trx_id));
    std::memcpy(&r.prior_undo_ptr, p + kUndoRecPriorUndoPtrOffset, sizeof(r.prior_undo_ptr));
    std::memcpy(&r.target_page_id, p + kUndoRecTargetPageIdOffset, sizeof(r.target_page_id));
    std::memcpy(&r.target_slot, p + kUndoRecTargetSlotOffset, sizeof(r.target_slot));
    std::memcpy(&r.image_len, p + kUndoRecImageLenOffset, sizeof(r.image_len));
    std::memcpy(&r.type, p + kUndoRecTypeOffset, sizeof(r.type));
    std::memcpy(&r.flags, p + kUndoRecFlagsOffset, sizeof(r.flags));
    std::memcpy(&r.reserved, p + kUndoRecReservedOffset, sizeof(r.reserved));
    std::memcpy(&r.txn_prev_undo_ptr, p + kUndoRecTxnPrevUndoPtrOffset,
                sizeof(r.txn_prev_undo_ptr));
    std::memcpy(&r.pk, p + kUndoRecPkOffset, sizeof(r.pk));
    return r;
}

void WriteRecordHeader(std::span<std::byte, kPageSize> page, std::uint16_t offset,
                       const UndoRecordFields& r) {
    std::byte* p = page.data() + offset;
    std::memcpy(p + kUndoRecPriorTrxIdOffset, &r.prior_trx_id, sizeof(r.prior_trx_id));
    std::memcpy(p + kUndoRecPriorUndoPtrOffset, &r.prior_undo_ptr, sizeof(r.prior_undo_ptr));
    std::memcpy(p + kUndoRecTargetPageIdOffset, &r.target_page_id, sizeof(r.target_page_id));
    std::memcpy(p + kUndoRecTargetSlotOffset, &r.target_slot, sizeof(r.target_slot));
    std::memcpy(p + kUndoRecImageLenOffset, &r.image_len, sizeof(r.image_len));
    std::memcpy(p + kUndoRecTypeOffset, &r.type, sizeof(r.type));
    std::memcpy(p + kUndoRecFlagsOffset, &r.flags, sizeof(r.flags));
    std::memcpy(p + kUndoRecReservedOffset, &r.reserved, sizeof(r.reserved));
    std::memcpy(p + kUndoRecTxnPrevUndoPtrOffset, &r.txn_prev_undo_ptr,
                sizeof(r.txn_prev_undo_ptr));
    std::memcpy(p + kUndoRecPkOffset, &r.pk, sizeof(r.pk));
}

// Shared by UndoPageAppend and UndoPageWriteAt: what both refuse before
// touching a byte. Split out so the append path and the redo path cannot
// come to disagree about what a well-formed record is.
Status CheckRecord(const UndoRecordFields& fields, std::span<const std::byte> image) {
    if (image.size() > kMaxUndoImageLen) {
        return Status::InvalidArgument("undo image of " + std::to_string(image.size()) +
                                       " bytes exceeds the " + std::to_string(kMaxUndoImageLen) +
                                       "-byte page capacity");
    }
    if (fields.prior_trx_id > heap::kMaxTrxId) {
        return Status::InvalidArgument("prior_trx_id " + std::to_string(fields.prior_trx_id) +
                                       " exceeds the 48-bit transaction id space");
    }
    // kNoUndoPtr is the legal terminator; anything else must name a record.
    if (fields.prior_undo_ptr != kNoUndoPtr) {
        if (Status s = UndoPtrIsPlausible(fields.prior_undo_ptr); !s.ok()) return s;
    }
    const auto type = static_cast<UndoRecordType>(fields.type);
    if (type != UndoRecordType::kOverwrite && type != UndoRecordType::kDeleteMark &&
        type != UndoRecordType::kInsert) {
        return Status::InvalidArgument("undo record type " + std::to_string(fields.type) +
                                       " is not a writable type");
    }
    // A delete-mark changes no tuple bytes and an insert has no prior
    // version, so neither has an image to carry. Refused rather than
    // ignored: an image nobody will read is a disagreement between the
    // writer and txn.md section 3.3, and the reader would never notice.
    if (type != UndoRecordType::kOverwrite && !image.empty()) {
        return Status::InvalidArgument("an undo record of type " + std::to_string(fields.type) +
                                       " carries no before-image");
    }
    return Status::OK();
}

}  // namespace

Status UndoPtrIsPlausible(std::uint64_t ptr) {
    if ((ptr >> 48) != 0) {
        return Status::Corruption("undo_ptr " + std::to_string(ptr) +
                                  " has nonzero bits above 48");
    }
    const PageId page_id = UndoPtrPageId(ptr);
    if (page_id == 0) {
        return Status::Corruption("undo_ptr names page 0, which is the superblock");
    }
    const std::uint16_t offset = UndoPtrOffset(ptr);
    if (offset < kUndoRecordsOffset || offset > kPageSize - kUndoRecordHeaderSize) {
        return Status::Corruption("undo_ptr offset " + std::to_string(offset) +
                                  " lies outside the record area");
    }
    return Status::OK();
}

Status FormatUndoPage(std::span<std::byte, kPageSize> page, std::uint64_t first_trx_id,
                      PageId prev_page_id) {
    if (first_trx_id > heap::kMaxTrxId) {
        return Status::InvalidArgument("first_trx_id " + std::to_string(first_trx_id) +
                                       " exceeds the 48-bit transaction id space");
    }
    storage::FormatPage(page, PageType::kUndo);

    UndoPageHeaderFields h{};
    h.flags = kUndoPageFlagInitialized;
    h.nr_records = 0;
    h.lower = static_cast<std::uint16_t>(kUndoRecordsOffset);
    h.reserved0 = 0;
    h.first_trx_id = first_trx_id;
    h.prev_page_id = prev_page_id;
    h.reserved1 = 0;
    WriteUndoPageHeader(page, h);
    return Status::OK();
}

UndoPageHeaderFields ReadUndoPageHeader(std::span<const std::byte, kPageSize> page) {
    UndoPageHeaderFields h{};
    const std::byte* base = page.data() + kUndoHeaderOffset;
    std::memcpy(&h.flags, base + kUndoHeaderFlagsOffset, sizeof(h.flags));
    std::memcpy(&h.nr_records, base + kUndoHeaderNrRecordsOffset, sizeof(h.nr_records));
    std::memcpy(&h.lower, base + kUndoHeaderLowerOffset, sizeof(h.lower));
    std::memcpy(&h.reserved0, base + kUndoHeaderReserved0Offset, sizeof(h.reserved0));
    std::memcpy(&h.first_trx_id, base + kUndoHeaderFirstTrxIdOffset, sizeof(h.first_trx_id));
    std::memcpy(&h.prev_page_id, base + kUndoHeaderPrevPageIdOffset, sizeof(h.prev_page_id));
    std::memcpy(&h.reserved1, base + kUndoHeaderReserved1Offset, sizeof(h.reserved1));
    return h;
}

void WriteUndoPageHeader(std::span<std::byte, kPageSize> page,
                         const UndoPageHeaderFields& h) {
    std::byte* base = page.data() + kUndoHeaderOffset;
    std::memcpy(base + kUndoHeaderFlagsOffset, &h.flags, sizeof(h.flags));
    std::memcpy(base + kUndoHeaderNrRecordsOffset, &h.nr_records, sizeof(h.nr_records));
    std::memcpy(base + kUndoHeaderLowerOffset, &h.lower, sizeof(h.lower));
    std::memcpy(base + kUndoHeaderReserved0Offset, &h.reserved0, sizeof(h.reserved0));
    std::memcpy(base + kUndoHeaderFirstTrxIdOffset, &h.first_trx_id, sizeof(h.first_trx_id));
    std::memcpy(base + kUndoHeaderPrevPageIdOffset, &h.prev_page_id, sizeof(h.prev_page_id));
    std::memcpy(base + kUndoHeaderReserved1Offset, &h.reserved1, sizeof(h.reserved1));
}

std::uint16_t UndoPageFreeSpace(std::span<const std::byte, kPageSize> page) {
    const UndoPageHeaderFields h = ReadUndoPageHeader(page);
    if (h.lower >= kPageSize) return 0;
    return static_cast<std::uint16_t>(kPageSize - h.lower);
}

StatusOr<std::uint16_t> UndoPageAppend(std::span<std::byte, kPageSize> page,
                                        const UndoRecordFields& fields,
                                        std::span<const std::byte> image) {
    if (Status s = CheckRecord(fields, image); !s.ok()) return s;

    UndoPageHeaderFields h = ReadUndoPageHeader(page);
    if ((h.flags & kUndoPageFlagInitialized) == 0) {
        return Status::Corruption("undo page is not initialized");
    }
    const std::size_t need = kUndoRecordHeaderSize + image.size();
    if (h.lower < kUndoRecordsOffset || h.lower > kPageSize) {
        return Status::Corruption("undo page lower " + std::to_string(h.lower) +
                                  " is outside the page");
    }
    if (kPageSize - h.lower < need) {
        // Named as the *undo* page deliberately: the caller's heap page may
        // have had room, and a message naming that one would send the
        // reader to the wrong place (txn.md section 3.3's known ceiling).
        return Status::OutOfSpace("undo page has " + std::to_string(kPageSize - h.lower) +
                                  " bytes free, needs " + std::to_string(need));
    }

    const std::uint16_t offset = h.lower;
    UndoRecordFields written = fields;
    written.image_len = static_cast<std::uint16_t>(image.size());
    WriteRecordHeader(page, offset, written);
    if (!image.empty()) {
        std::memcpy(page.data() + offset + kUndoRecordHeaderSize, image.data(), image.size());
    }

    h.lower = static_cast<std::uint16_t>(offset + need);
    ++h.nr_records;
    WriteUndoPageHeader(page, h);
    return offset;
}

std::size_t UndoRecordTailSize(std::size_t image_len) noexcept {
    return kUndoRecordTailHeaderSize + image_len;
}

Status EncodeUndoRecordTail(std::span<std::byte> out, const UndoRecordFields& fields,
                            std::span<const std::byte> image) {
    if (image.size() > kMaxUndoImageLen) {
        return Status::InvalidArgument("undo record image of " + std::to_string(image.size()) +
                                       " bytes exceeds the " + std::to_string(kMaxUndoImageLen) +
                                       " a page can hold");
    }
    const std::size_t total = UndoRecordTailSize(image.size());
    if (out.size() < total) {
        return Status::InvalidArgument("undo record tail needs " + std::to_string(total) +
                                       " bytes, got " + std::to_string(out.size()));
    }

    // Field-wise memcpy at offsets relative to the tail's own start, which
    // is the record's +16 (rules.md §2 - never the mirror struct).
    const auto image_len = static_cast<std::uint16_t>(image.size());
    std::byte* p = out.data();
    std::memcpy(p + (kUndoRecTargetPageIdOffset - kUndoRecordTailOffset), &fields.target_page_id,
                sizeof(fields.target_page_id));
    std::memcpy(p + (kUndoRecTargetSlotOffset - kUndoRecordTailOffset), &fields.target_slot,
                sizeof(fields.target_slot));
    std::memcpy(p + (kUndoRecImageLenOffset - kUndoRecordTailOffset), &image_len,
                sizeof(image_len));
    std::memcpy(p + (kUndoRecTypeOffset - kUndoRecordTailOffset), &fields.type,
                sizeof(fields.type));
    std::memcpy(p + (kUndoRecFlagsOffset - kUndoRecordTailOffset), &fields.flags,
                sizeof(fields.flags));
    std::memcpy(p + (kUndoRecReservedOffset - kUndoRecordTailOffset), &fields.reserved,
                sizeof(fields.reserved));
    // RV10's two. They are in the tail rather than in the WAL payload's
    // fields because the tail is "the record's bytes from target_page_id
    // onward" and these are part of the record - keeping the payload's
    // field list unchanged, which is what stops UndoWritePayload from
    // becoming a second, drifting copy of the record header.
    std::memcpy(p + (kUndoRecTxnPrevUndoPtrOffset - kUndoRecordTailOffset),
                &fields.txn_prev_undo_ptr, sizeof(fields.txn_prev_undo_ptr));
    std::memcpy(p + (kUndoRecPkOffset - kUndoRecordTailOffset), &fields.pk, sizeof(fields.pk));
    if (!image.empty()) {
        std::memcpy(p + kUndoRecordTailHeaderSize, image.data(), image.size());
    }
    return Status::OK();
}

StatusOr<DecodedUndoRecord> DecodeUndoRecordTail(std::span<const std::byte> tail) {
    if (tail.size() < kUndoRecordTailHeaderSize) {
        return Status::Corruption("undo record tail of " + std::to_string(tail.size()) +
                                  " bytes is shorter than its " +
                                  std::to_string(kUndoRecordTailHeaderSize) + "-byte header");
    }

    DecodedUndoRecord out{};
    const std::byte* p = tail.data();
    // prior_trx_id / prior_undo_ptr are not in these bytes: they ride as
    // fields of the WAL payload (docs/spec/txn.md §3.5) and the caller fills
    // them.
    std::memcpy(&out.fields.target_page_id, p + (kUndoRecTargetPageIdOffset - kUndoRecordTailOffset),
                sizeof(out.fields.target_page_id));
    std::memcpy(&out.fields.target_slot, p + (kUndoRecTargetSlotOffset - kUndoRecordTailOffset),
                sizeof(out.fields.target_slot));
    std::memcpy(&out.fields.image_len, p + (kUndoRecImageLenOffset - kUndoRecordTailOffset),
                sizeof(out.fields.image_len));
    std::memcpy(&out.fields.type, p + (kUndoRecTypeOffset - kUndoRecordTailOffset),
                sizeof(out.fields.type));
    std::memcpy(&out.fields.flags, p + (kUndoRecFlagsOffset - kUndoRecordTailOffset),
                sizeof(out.fields.flags));
    std::memcpy(&out.fields.reserved, p + (kUndoRecReservedOffset - kUndoRecordTailOffset),
                sizeof(out.fields.reserved));
    std::memcpy(&out.fields.txn_prev_undo_ptr,
                p + (kUndoRecTxnPrevUndoPtrOffset - kUndoRecordTailOffset),
                sizeof(out.fields.txn_prev_undo_ptr));
    std::memcpy(&out.fields.pk, p + (kUndoRecPkOffset - kUndoRecordTailOffset),
                sizeof(out.fields.pk));

    if (UndoRecordTailSize(out.fields.image_len) != tail.size()) {
        return Status::Corruption("undo record tail says its image is " +
                                  std::to_string(out.fields.image_len) + " bytes, which does not "
                                  "match the " + std::to_string(tail.size()) + " it carries");
    }
    out.image = tail.subspan(kUndoRecordTailHeaderSize);
    return out;
}

Status UndoPageWriteAt(std::span<std::byte, kPageSize> page, std::uint16_t offset,
                       const UndoRecordFields& fields, std::span<const std::byte> image) {
    if (Status s = CheckRecord(fields, image); !s.ok()) return s;
    if (offset < kUndoRecordsOffset) {
        return Status::Corruption("undo record offset " + std::to_string(offset) +
                                  " is inside the page header");
    }
    const std::size_t need = kUndoRecordHeaderSize + image.size();
    if (kPageSize - offset < need) {
        return Status::Corruption("undo record at " + std::to_string(offset) + " needs " +
                                  std::to_string(need) + " bytes and runs past the page");
    }

    UndoRecordFields written = fields;
    written.image_len = static_cast<std::uint16_t>(image.size());
    WriteRecordHeader(page, offset, written);
    if (!image.empty()) {
        std::memcpy(page.data() + offset + kUndoRecordHeaderSize, image.data(), image.size());
    }

    // Only when the write extends the page, so replaying one record twice
    // leaves the header where it was. Redo is idempotent or it is not redo.
    UndoPageHeaderFields h = ReadUndoPageHeader(page);
    const auto end = static_cast<std::uint16_t>(offset + need);
    if (end > h.lower) {
        h.lower = end;
        ++h.nr_records;
        h.flags |= kUndoPageFlagInitialized;
        WriteUndoPageHeader(page, h);
    }
    return Status::OK();
}

StatusOr<DecodedUndoRecord> UndoPageRead(std::span<const std::byte, kPageSize> page,
                                          std::uint16_t offset) {
    if (offset < kUndoRecordsOffset || offset > kPageSize - kUndoRecordHeaderSize) {
        return Status::Corruption("undo record offset " + std::to_string(offset) +
                                  " lies outside the record area");
    }
    DecodedUndoRecord out;
    out.fields = ReadRecordHeader(page, offset);

    const auto type = static_cast<UndoRecordType>(out.fields.type);
    if (type != UndoRecordType::kOverwrite && type != UndoRecordType::kDeleteMark &&
        type != UndoRecordType::kInsert) {
        return Status::Corruption("undo record at " + std::to_string(offset) + " has type " +
                                  std::to_string(out.fields.type));
    }
    const std::size_t end =
        static_cast<std::size_t>(offset) + kUndoRecordHeaderSize + out.fields.image_len;
    if (end > kPageSize) {
        return Status::Corruption("undo record at " + std::to_string(offset) +
                                  " claims an image running past the page");
    }
    out.image = page.subspan(static_cast<std::size_t>(offset) + kUndoRecordHeaderSize,
                             out.fields.image_len);
    return out;
}

}  // namespace kds::txn
