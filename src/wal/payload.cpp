#include "kds/wal/payload.hpp"

#include <cstring>
#include <string>

#include "kds/storage/keystone.hpp"

// rules.md #2: every read/write of on-disk bytes goes field-by-field
// through a named offset. The mirror structs in the header pin those
// offsets with static_asserts; they are never memcpy'd whole.

namespace kds::wal {
namespace {

template <typename T>
T Load(std::span<const std::byte> bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

template <typename T>
void Store(std::span<std::byte> bytes, std::size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

Status CheckOutputSize(std::span<std::byte> out, std::size_t needed, const char* what) {
    if (out.size() < needed) {
        return Status::InvalidArgument(std::string("wal payload: output buffer smaller than a ") +
                                       what + " payload");
    }
    return Status::OK();
}

// A payload shorter than its own fixed part cannot be read at all. This is
// Corruption, not a torn tail: the envelope's CRC already vouched for these
// bytes, so they are intact and wrong.
Status CheckInputSize(std::span<const std::byte> in, std::size_t needed, const char* what) {
    if (in.size() < needed) {
        return Status::Corruption(std::string("wal payload: ") + what + " payload is " +
                                  std::to_string(in.size()) + " bytes, needs " +
                                  std::to_string(needed));
    }
    return Status::OK();
}

// PageType is a frozen append-only enum (common.hpp); 0 is the invalid
// sentinel and anything above the assigned range was written by a newer
// build.
bool IsKnownPageType(std::uint8_t raw) noexcept {
    return raw != static_cast<std::uint8_t>(PageType::kInvalid) && raw <= kMaxAssignedPageType;
}

Status CheckTxnId(std::uint64_t txn_id, const char* what) {
    if (txn_id > kMaxTxnId) {
        return Status::Corruption(std::string("wal payload: ") + what +
                                  " exceeds 48 bits (upper bits must be zero)");
    }
    return Status::OK();
}

}  // namespace

// ---- PAGE_INIT -----------------------------------------------------------

StatusOr<std::size_t> EncodePageInit(std::span<std::byte> out, const PageInitPayload& fields) {
    if (Status s = CheckOutputSize(out, kPageInitPayloadSize, "PAGE_INIT"); !s.ok()) {
        return s;
    }
    if (fields.min_key > kMaxKeystoneId) {
        return Status::InvalidArgument("wal payload: PAGE_INIT min_key exceeds the 40-bit id space");
    }
    if (!IsKnownPageType(fields.page_type)) {
        return Status::InvalidArgument("wal payload: PAGE_INIT page_type is unassigned");
    }

    Store<std::uint64_t>(out, kPageInitMinKeyOffset, fields.min_key);
    Store<std::uint8_t>(out, kPageInitPageTypeOffset, fields.page_type);
    std::memset(out.data() + kPageInitReservedOffset, 0, 3);
    return kPageInitPayloadSize;
}

StatusOr<PageInitPayload> DecodePageInit(std::span<const std::byte> in) {
    if (Status s = CheckInputSize(in, kPageInitPayloadSize, "PAGE_INIT"); !s.ok()) {
        return s;
    }

    PageInitPayload fields{};
    fields.min_key = Load<std::uint64_t>(in, kPageInitMinKeyOffset);
    fields.page_type = Load<std::uint8_t>(in, kPageInitPageTypeOffset);
    if (fields.min_key > kMaxKeystoneId) {
        return Status::Corruption("wal payload: PAGE_INIT min_key exceeds the 40-bit id space");
    }
    if (!IsKnownPageType(fields.page_type)) {
        // A page type this build does not know is the same hard error an
        // unknown record type is: replaying it would format a page wrong.
        return Status::Corruption("wal payload: PAGE_INIT page_type " +
                                  std::to_string(fields.page_type) +
                                  " is not known to this build");
    }
    return fields;
}

// ---- HEAP_INSERT / HEAP_OVERWRITE ---------------------------------------

StatusOr<std::size_t> EncodeHeapWrite(std::span<std::byte> out, const HeapWritePayload& fields,
                                      std::span<const std::byte> tuple) {
    if (tuple.size() > 0xFFFFu) {
        return Status::InvalidArgument("wal payload: heap tuple longer than a uint16 length field");
    }
    const std::size_t total = kHeapWriteFixedSize + tuple.size();
    if (Status s = CheckOutputSize(out, total, "heap write"); !s.ok()) {
        return s;
    }
    if (fields.trx_id > kMaxTxnId) {
        return Status::InvalidArgument("wal payload: heap write trx_id exceeds 48 bits");
    }

    Store<std::uint64_t>(out, kHeapWriteTrxIdOffset, fields.trx_id);
    Store<std::uint64_t>(out, kHeapWriteUndoPtrOffset, fields.undo_ptr);
    Store<std::uint16_t>(out, kHeapWriteSlotOffset, fields.slot);
    // Taken from the span, never from the caller's field, so the length on
    // disk and the bytes on disk cannot disagree.
    Store<std::uint16_t>(out, kHeapWriteTupleLenOffset, static_cast<std::uint16_t>(tuple.size()));
    if (!tuple.empty()) {
        std::memcpy(out.data() + kHeapWriteFixedSize, tuple.data(), tuple.size());
    }
    return total;
}

StatusOr<DecodedHeapWrite> DecodeHeapWrite(std::span<const std::byte> in) {
    if (Status s = CheckInputSize(in, kHeapWriteFixedSize, "heap write"); !s.ok()) {
        return s;
    }

    DecodedHeapWrite decoded{};
    decoded.fields.trx_id = Load<std::uint64_t>(in, kHeapWriteTrxIdOffset);
    decoded.fields.undo_ptr = Load<std::uint64_t>(in, kHeapWriteUndoPtrOffset);
    decoded.fields.slot = Load<std::uint16_t>(in, kHeapWriteSlotOffset);
    decoded.fields.tuple_len = Load<std::uint16_t>(in, kHeapWriteTupleLenOffset);

    if (Status s = CheckTxnId(decoded.fields.trx_id, "heap write trx_id"); !s.ok()) {
        return s;
    }
    // Trailing bytes are allowed - the envelope pads to 8 - but a length
    // that claims more than was written is not.
    if (in.size() - kHeapWriteFixedSize < decoded.fields.tuple_len) {
        return Status::Corruption("wal payload: heap write tuple_len runs past the payload");
    }
    decoded.tuple = in.subspan(kHeapWriteFixedSize, decoded.fields.tuple_len);
    return decoded;
}

// ---- VARHEAP_APPEND ------------------------------------------------------

StatusOr<std::size_t> EncodeVarHeapAppend(std::span<std::byte> out,
                                           const VarHeapAppendPayload& fields,
                                           std::span<const std::byte> value) {
    if (value.size() > 0xFFFFFFFFull) {
        return Status::InvalidArgument("wal payload: var-heap value longer than a uint32 length");
    }
    const std::size_t total = kVarHeapAppendFixedSize + value.size();
    if (Status s = CheckOutputSize(out, total, "VARHEAP_APPEND"); !s.ok()) {
        return s;
    }

    Store<std::uint16_t>(out, kVarHeapAppendSlotOffset, fields.slot);
    Store<std::uint16_t>(out, kVarHeapAppendReservedOffset, 0);
    // From the span, never the caller's field, so the length on disk and
    // the bytes on disk cannot disagree.
    Store<std::uint32_t>(out, kVarHeapAppendValueLenOffset,
                         static_cast<std::uint32_t>(value.size()));
    if (!value.empty()) {
        std::memcpy(out.data() + kVarHeapAppendFixedSize, value.data(), value.size());
    }
    return total;
}

StatusOr<DecodedVarHeapAppend> DecodeVarHeapAppend(std::span<const std::byte> in) {
    if (Status s = CheckInputSize(in, kVarHeapAppendFixedSize, "VARHEAP_APPEND"); !s.ok()) {
        return s;
    }

    DecodedVarHeapAppend decoded{};
    decoded.fields.slot = Load<std::uint16_t>(in, kVarHeapAppendSlotOffset);
    decoded.fields.reserved = Load<std::uint16_t>(in, kVarHeapAppendReservedOffset);
    decoded.fields.value_len = Load<std::uint32_t>(in, kVarHeapAppendValueLenOffset);

    // Trailing bytes are allowed - the envelope pads to 8 - but a length
    // claiming more than was written is not.
    if (in.size() - kVarHeapAppendFixedSize < decoded.fields.value_len) {
        return Status::Corruption("wal payload: VARHEAP_APPEND value_len runs past the payload");
    }
    decoded.value = in.subspan(kVarHeapAppendFixedSize, decoded.fields.value_len);
    return decoded;
}

// ---- INDEX_INSERT --------------------------------------------------------

StatusOr<std::size_t> EncodeIndexInsert(std::span<std::byte> out,
                                         const IndexInsertPayload& fields,
                                         std::span<const std::byte> entry) {
    if (entry.empty() || entry.size() > 0xFFFFull) {
        return Status::InvalidArgument(
            "wal payload: INDEX_INSERT entry is empty or longer than a uint16 length");
    }
    const std::size_t total = kIndexInsertFixedSize + entry.size();
    if (Status s = CheckOutputSize(out, total, "INDEX_INSERT"); !s.ok()) return s;

    Store<std::uint16_t>(out, kIndexInsertSlotOffset, fields.slot);
    // From the span, never the caller's field, so the length on disk and the
    // bytes on disk cannot disagree.
    Store<std::uint16_t>(out, kIndexInsertEntryLenOffset,
                         static_cast<std::uint16_t>(entry.size()));
    std::memcpy(out.data() + kIndexInsertFixedSize, entry.data(), entry.size());
    return total;
}

StatusOr<DecodedIndexInsert> DecodeIndexInsert(std::span<const std::byte> in) {
    if (Status s = CheckInputSize(in, kIndexInsertFixedSize, "INDEX_INSERT"); !s.ok()) return s;

    DecodedIndexInsert decoded{};
    decoded.fields.slot = Load<std::uint16_t>(in, kIndexInsertSlotOffset);
    decoded.fields.entry_len = Load<std::uint16_t>(in, kIndexInsertEntryLenOffset);

    if (in.size() - kIndexInsertFixedSize < decoded.fields.entry_len) {
        return Status::Corruption("wal payload: INDEX_INSERT entry_len runs past the payload");
    }
    decoded.entry = in.subspan(kIndexInsertFixedSize, decoded.fields.entry_len);
    return decoded;
}

// ---- HEAP_DELETE_MARK ----------------------------------------------------

StatusOr<std::size_t> EncodeHeapDeleteMark(std::span<std::byte> out,
                                           const HeapDeleteMarkPayload& fields) {
    if (Status s = CheckOutputSize(out, kDeleteMarkPayloadSize, "HEAP_DELETE_MARK"); !s.ok()) {
        return s;
    }
    if (fields.trx_id > kMaxTxnId) {
        return Status::InvalidArgument("wal payload: delete-mark trx_id exceeds 48 bits");
    }

    Store<std::uint64_t>(out, kDeleteMarkTrxIdOffset, fields.trx_id);
    Store<std::uint16_t>(out, kDeleteMarkSlotOffset, fields.slot);
    return kDeleteMarkPayloadSize;
}

StatusOr<HeapDeleteMarkPayload> DecodeHeapDeleteMark(std::span<const std::byte> in) {
    if (Status s = CheckInputSize(in, kDeleteMarkPayloadSize, "HEAP_DELETE_MARK"); !s.ok()) {
        return s;
    }

    HeapDeleteMarkPayload fields{};
    fields.trx_id = Load<std::uint64_t>(in, kDeleteMarkTrxIdOffset);
    fields.slot = Load<std::uint16_t>(in, kDeleteMarkSlotOffset);
    if (Status s = CheckTxnId(fields.trx_id, "delete-mark trx_id"); !s.ok()) {
        return s;
    }
    return fields;
}

// ---- SLOT_RETIRE ---------------------------------------------------------

StatusOr<std::size_t> EncodeSlotRetire(std::span<std::byte> out, const SlotRetirePayload& fields) {
    if (Status s = CheckOutputSize(out, kSlotRetirePayloadSize, "SLOT_RETIRE"); !s.ok()) {
        return s;
    }
    Store<std::uint16_t>(out, kSlotRetireSlotOffset, fields.slot);
    return kSlotRetirePayloadSize;
}

StatusOr<SlotRetirePayload> DecodeSlotRetire(std::span<const std::byte> in) {
    if (Status s = CheckInputSize(in, kSlotRetirePayloadSize, "SLOT_RETIRE"); !s.ok()) {
        return s;
    }
    SlotRetirePayload fields{};
    fields.slot = Load<std::uint16_t>(in, kSlotRetireSlotOffset);
    return fields;
}

// ---- UNDO_WRITE ----------------------------------------------------------

StatusOr<std::size_t> EncodeUndoWrite(std::span<std::byte> out, const UndoWritePayload& fields,
                                      std::span<const std::byte> image) {
    if (image.size() > 0xFFFFu) {
        return Status::InvalidArgument(
            "wal payload: undo before-image longer than a uint16 length field");
    }
    const std::size_t total = kUndoWriteFixedSize + image.size();
    if (Status s = CheckOutputSize(out, total, "UNDO_WRITE"); !s.ok()) {
        return s;
    }
    if (fields.prior_trx_id > kMaxTxnId) {
        return Status::InvalidArgument("wal payload: UNDO_WRITE prior_trx_id exceeds 48 bits");
    }
    if (static_cast<std::size_t>(fields.offset) + image.size() > kPageSize) {
        return Status::InvalidArgument("wal payload: UNDO_WRITE image runs past the undo page");
    }

    Store<std::uint64_t>(out, kUndoPriorTrxIdOffset, fields.prior_trx_id);
    Store<std::uint64_t>(out, kUndoPriorUndoPtrOffset, fields.prior_undo_ptr);
    Store<std::uint16_t>(out, kUndoOffsetOffset, fields.offset);
    Store<std::uint16_t>(out, kUndoImageLenOffset, static_cast<std::uint16_t>(image.size()));
    if (!image.empty()) {
        std::memcpy(out.data() + kUndoWriteFixedSize, image.data(), image.size());
    }
    return total;
}

StatusOr<DecodedUndoWrite> DecodeUndoWrite(std::span<const std::byte> in) {
    if (Status s = CheckInputSize(in, kUndoWriteFixedSize, "UNDO_WRITE"); !s.ok()) {
        return s;
    }

    DecodedUndoWrite decoded{};
    decoded.fields.prior_trx_id = Load<std::uint64_t>(in, kUndoPriorTrxIdOffset);
    decoded.fields.prior_undo_ptr = Load<std::uint64_t>(in, kUndoPriorUndoPtrOffset);
    decoded.fields.offset = Load<std::uint16_t>(in, kUndoOffsetOffset);
    decoded.fields.image_len = Load<std::uint16_t>(in, kUndoImageLenOffset);

    if (Status s = CheckTxnId(decoded.fields.prior_trx_id, "UNDO_WRITE prior_trx_id"); !s.ok()) {
        return s;
    }
    if (in.size() - kUndoWriteFixedSize < decoded.fields.image_len) {
        return Status::Corruption("wal payload: UNDO_WRITE image_len runs past the payload");
    }
    if (static_cast<std::size_t>(decoded.fields.offset) + decoded.fields.image_len > kPageSize) {
        // Replaying this would write outside the undo page.
        return Status::Corruption("wal payload: UNDO_WRITE image runs past the undo page");
    }
    decoded.image = in.subspan(kUndoWriteFixedSize, decoded.fields.image_len);
    return decoded;
}

// ---- ALLOC / FREE --------------------------------------------------------

StatusOr<std::size_t> EncodePageRun(std::span<std::byte> out, const PageRunPayload& fields) {
    if (Status s = CheckOutputSize(out, kPageRunPayloadSize, "ALLOC/FREE"); !s.ok()) {
        return s;
    }
    if (fields.nr_pages == 0) {
        return Status::InvalidArgument("wal payload: ALLOC/FREE of zero pages");
    }
    Store<std::uint32_t>(out, kPageRunNrPagesOffset, fields.nr_pages);
    return kPageRunPayloadSize;
}

StatusOr<PageRunPayload> DecodePageRun(std::span<const std::byte> in) {
    if (Status s = CheckInputSize(in, kPageRunPayloadSize, "ALLOC/FREE"); !s.ok()) {
        return s;
    }
    PageRunPayload fields{};
    fields.nr_pages = Load<std::uint32_t>(in, kPageRunNrPagesOffset);
    if (fields.nr_pages == 0) {
        return Status::Corruption("wal payload: ALLOC/FREE of zero pages");
    }
    return fields;
}

// ---- FULL_PAGE_IMAGE -----------------------------------------------------

StatusOr<std::size_t> EncodeFullPageImage(std::span<std::byte> out,
                                          std::span<const std::byte, kPageSize> page) {
    if (Status s = CheckOutputSize(out, kFullPageImagePayloadSize, "FULL_PAGE_IMAGE"); !s.ok()) {
        return s;
    }
    std::memcpy(out.data(), page.data(), kFullPageImagePayloadSize);
    return kFullPageImagePayloadSize;
}

StatusOr<std::span<const std::byte>> DecodeFullPageImage(std::span<const std::byte> in) {
    if (Status s = CheckInputSize(in, kFullPageImagePayloadSize, "FULL_PAGE_IMAGE"); !s.ok()) {
        return s;
    }
    return in.first(kFullPageImagePayloadSize);
}

// ---- CHECKPOINT_BEGIN ----------------------------------------------------

std::size_t CheckpointBeginSize(std::size_t txn_count, std::size_t dirty_count) noexcept {
    return kCheckpointBeginFixedSize + txn_count * sizeof(std::uint64_t) +
           dirty_count * kDirtyEntrySize;
}

StatusOr<std::size_t> EncodeCheckpointBegin(std::span<std::byte> out,
                                            std::span<const std::uint64_t> active_txns,
                                            std::span<const CheckpointDirtyPage> dirty_pages) {
    if (active_txns.size() > 0xFFFFFFFFull || dirty_pages.size() > 0xFFFFFFFFull) {
        return Status::InvalidArgument("wal payload: CHECKPOINT_BEGIN table longer than a uint32");
    }
    const std::size_t total = CheckpointBeginSize(active_txns.size(), dirty_pages.size());
    if (Status s = CheckOutputSize(out, total, "CHECKPOINT_BEGIN"); !s.ok()) {
        return s;
    }
    for (const std::uint64_t txn_id : active_txns) {
        if (txn_id > kMaxTxnId) {
            return Status::InvalidArgument("wal payload: CHECKPOINT_BEGIN txn_id exceeds 48 bits");
        }
        if (txn_id == kNoTxnId) {
            return Status::InvalidArgument("wal payload: CHECKPOINT_BEGIN lists txn_id 0");
        }
    }

    Store<std::uint32_t>(out, kCheckpointTxnCountOffset,
                         static_cast<std::uint32_t>(active_txns.size()));
    Store<std::uint32_t>(out, kCheckpointDirtyCountOffset,
                         static_cast<std::uint32_t>(dirty_pages.size()));

    std::size_t at = kCheckpointBeginFixedSize;
    for (const std::uint64_t txn_id : active_txns) {
        Store<std::uint64_t>(out, at, txn_id);
        at += sizeof(std::uint64_t);
    }
    for (const CheckpointDirtyPage& entry : dirty_pages) {
        Store<PageId>(out, at + kDirtyPageIdOffset, entry.page_id);
        Store<Lsn>(out, at + kDirtyRecLsnOffset, entry.rec_lsn);
        at += kDirtyEntrySize;
    }
    return total;
}

StatusOr<DecodedCheckpointBegin> DecodeCheckpointBegin(std::span<const std::byte> in) {
    if (Status s = CheckInputSize(in, kCheckpointBeginFixedSize, "CHECKPOINT_BEGIN"); !s.ok()) {
        return s;
    }

    const auto txn_count = Load<std::uint32_t>(in, kCheckpointTxnCountOffset);
    const auto dirty_count = Load<std::uint32_t>(in, kCheckpointDirtyCountOffset);
    // Sized from the payload before anything is reserved, so a corrupt count
    // cannot ask for an allocation the record does not back with bytes.
    const std::size_t needed = CheckpointBeginSize(txn_count, dirty_count);
    if (in.size() < needed) {
        return Status::Corruption("wal payload: CHECKPOINT_BEGIN counts run past the payload");
    }

    DecodedCheckpointBegin decoded{};
    decoded.active_txns.reserve(txn_count);
    decoded.dirty_pages.reserve(dirty_count);

    std::size_t at = kCheckpointBeginFixedSize;
    for (std::uint32_t i = 0; i < txn_count; ++i) {
        const auto txn_id = Load<std::uint64_t>(in, at);
        if (Status s = CheckTxnId(txn_id, "CHECKPOINT_BEGIN txn_id"); !s.ok()) {
            return s;
        }
        decoded.active_txns.push_back(txn_id);
        at += sizeof(std::uint64_t);
    }
    for (std::uint32_t i = 0; i < dirty_count; ++i) {
        CheckpointDirtyPage entry{};
        entry.page_id = Load<PageId>(in, at + kDirtyPageIdOffset);
        entry.rec_lsn = Load<Lsn>(in, at + kDirtyRecLsnOffset);
        if (entry.page_id == kInvalidPageId) {
            return Status::Corruption("wal payload: CHECKPOINT_BEGIN dirty table names the "
                                      "invalid page id");
        }
        decoded.dirty_pages.push_back(entry);
        at += kDirtyEntrySize;
    }
    return decoded;
}

// ---- CHECKPOINT_END ------------------------------------------------------

StatusOr<std::size_t> EncodeCheckpointEnd(std::span<std::byte> out,
                                          const CheckpointEndPayload& fields) {
    if (Status s = CheckOutputSize(out, kCheckpointEndPayloadSize, "CHECKPOINT_END"); !s.ok()) {
        return s;
    }
    Store<Lsn>(out, kCheckpointEndRedoStartOffset, fields.redo_start_lsn);
    return kCheckpointEndPayloadSize;
}

StatusOr<CheckpointEndPayload> DecodeCheckpointEnd(std::span<const std::byte> in) {
    if (Status s = CheckInputSize(in, kCheckpointEndPayloadSize, "CHECKPOINT_END"); !s.ok()) {
        return s;
    }
    CheckpointEndPayload fields{};
    fields.redo_start_lsn = Load<Lsn>(in, kCheckpointEndRedoStartOffset);
    return fields;
}

}  // namespace kds::wal
