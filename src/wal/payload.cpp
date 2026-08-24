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
    Store<std::uint32_t>(out, kPageInitReserved2Offset, 0);
    Store<std::uint64_t>(out, kPageInitOwnerOidOffset, fields.owner_oid);
    return kPageInitPayloadSize;
}

StatusOr<PageInitPayload> DecodePageInit(std::span<const std::byte> in) {
    // Two forms (page.md §2a): the 24-byte one carrying `owner_oid`, and the
    // 12-byte pre-owner one whose owner reads as 0. **Discriminated by `>=`,
    // never by `==`**: `DecodeRecord` hands back the record's 8-byte-aligned
    // tail, not the exact payload, so a 12-byte payload arrives here as 16
    // bytes of payload-plus-zero-padding while a 24-byte one arrives as
    // exactly 24. An equality test would refuse every legacy record read
    // through the envelope - which is the one case the compatibility exists
    // for. Same `>=` rule every other codec in this file uses, and it also
    // keeps a future longer form readable at these offsets.
    if (Status s = CheckInputSize(in, kPageInitPayloadSizeLegacy, "PAGE_INIT"); !s.ok()) {
        return s;
    }

    PageInitPayload fields{};
    fields.min_key = Load<std::uint64_t>(in, kPageInitMinKeyOffset);
    fields.page_type = Load<std::uint8_t>(in, kPageInitPageTypeOffset);
    if (in.size() >= kPageInitPayloadSize) {
        fields.owner_oid = Load<std::uint64_t>(in, kPageInitOwnerOidOffset);
    }
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

// ---- PAGE_HANDOFF --------------------------------------------------------

StatusOr<std::size_t> EncodePageHandoff(std::span<std::byte> out,
                                        const PageHandoffPayload& fields) {
    if (Status s = CheckOutputSize(out, kPageHandoffPayloadSize, "PAGE_HANDOFF"); !s.ok()) {
        return s;
    }
    Store<std::uint32_t>(out, kPageHandoffIncomingCoreOffset, fields.incoming_core);
    return kPageHandoffPayloadSize;
}

StatusOr<PageHandoffPayload> DecodePageHandoff(std::span<const std::byte> in) {
    // A floor, **never an exact size** - the same `>=` rule DecodePageInit
    // spells out above. `DecodeRecord` hands back the record's 8-byte
    // aligned tail, so these four bytes arrive here as eight, and an
    // equality test refused every PAGE_HANDOFF ever read through the
    // envelope. It shipped that way in PW1c-1 and was invisible because the
    // only caller was a test handing the codec a bare 4-byte buffer.
    if (Status s = CheckInputSize(in, kPageHandoffPayloadSize, "PAGE_HANDOFF"); !s.ok()) {
        return s;
    }
    PageHandoffPayload fields{};
    fields.incoming_core = Load<std::uint32_t>(in, kPageHandoffIncomingCoreOffset);
    return fields;
}

// ---- ANCHOR_UPDATE -------------------------------------------------------

StatusOr<std::size_t> EncodeAnchorUpdate(std::span<std::byte> out,
                                         const AnchorUpdatePayload& fields) {
    if (Status s = CheckOutputSize(out, kAnchorUpdatePayloadSize, "ANCHOR_UPDATE"); !s.ok()) {
        return s;
    }
    Store<std::uint64_t>(out, kAnchorUpdateIndexOidOffset, fields.index_oid);
    Store<std::uint32_t>(out, kAnchorUpdateRootOffset, fields.root);
    return kAnchorUpdatePayloadSize;
}

StatusOr<AnchorUpdatePayload> DecodeAnchorUpdate(std::span<const std::byte> in) {
    if (Status s = CheckInputSize(in, kAnchorUpdatePayloadSize, "ANCHOR_UPDATE"); !s.ok()) {
        return s;
    }
    AnchorUpdatePayload fields{};
    fields.index_oid = Load<std::uint64_t>(in, kAnchorUpdateIndexOidOffset);
    fields.root = Load<std::uint32_t>(in, kAnchorUpdateRootOffset);
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

StatusOr<std::size_t> EncodeHeapDeleteUnmark(std::span<std::byte> out,
                                             const HeapDeleteUnmarkPayload& fields) {
    if (Status s = CheckOutputSize(out, kDeleteUnmarkPayloadSize, "HEAP_DELETE_UNMARK"); !s.ok()) {
        return s;
    }
    if (fields.trx_id > kMaxTxnId) {
        return Status::InvalidArgument("wal payload: delete-unmark trx_id exceeds 48 bits");
    }

    Store<std::uint64_t>(out, kDeleteUnmarkTrxIdOffset, fields.trx_id);
    Store<std::uint64_t>(out, kDeleteUnmarkUndoPtrOffset, fields.undo_ptr);
    Store<std::uint16_t>(out, kDeleteUnmarkSlotOffset, fields.slot);
    return kDeleteUnmarkPayloadSize;
}

StatusOr<HeapDeleteUnmarkPayload> DecodeHeapDeleteUnmark(std::span<const std::byte> in) {
    if (Status s = CheckInputSize(in, kDeleteUnmarkPayloadSize, "HEAP_DELETE_UNMARK"); !s.ok()) {
        return s;
    }

    HeapDeleteUnmarkPayload fields{};
    fields.trx_id = Load<std::uint64_t>(in, kDeleteUnmarkTrxIdOffset);
    fields.undo_ptr = Load<std::uint64_t>(in, kDeleteUnmarkUndoPtrOffset);
    fields.slot = Load<std::uint16_t>(in, kDeleteUnmarkSlotOffset);
    // **kNoTxnId is legal here and is not in the mark's payload.** Clearing
    // a delete restores whatever writer preceded it, and for a row that no
    // transaction had written since bootstrap that is the pre-existing
    // writer, not a live id. CheckTxnId would refuse it.
    if (fields.trx_id > kMaxTxnId) {
        return Status::Corruption("wal payload: delete-unmark trx_id exceeds 48 bits");
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
                                      std::span<const std::byte> tail) {
    if (tail.size() > 0xFFFFu) {
        return Status::InvalidArgument(
            "wal payload: undo record tail longer than a uint16 length field");
    }
    const std::size_t total = kUndoWriteFixedSize + tail.size();
    if (Status s = CheckOutputSize(out, total, "UNDO_WRITE"); !s.ok()) {
        return s;
    }
    if (fields.prior_trx_id > kMaxTxnId) {
        return Status::InvalidArgument("wal payload: UNDO_WRITE prior_trx_id exceeds 48 bits");
    }
    // A sanity bound, not the authority: the record also occupies the 16
    // bytes of chain links before its tail, and the exact extent check
    // belongs to txn::UndoPageWriteAt, which holds the page. Stating the
    // precise bound here would mean this layer including txn/, and wal
    // sits below it.
    if (static_cast<std::size_t>(fields.offset) + tail.size() > kPageSize) {
        return Status::InvalidArgument("wal payload: UNDO_WRITE tail runs past the undo page");
    }

    Store<std::uint64_t>(out, kUndoPriorTrxIdOffset, fields.prior_trx_id);
    Store<std::uint64_t>(out, kUndoPriorUndoPtrOffset, fields.prior_undo_ptr);
    Store<std::uint16_t>(out, kUndoOffsetOffset, fields.offset);
    Store<std::uint16_t>(out, kUndoTailLenOffset, static_cast<std::uint16_t>(tail.size()));
    if (!tail.empty()) {
        std::memcpy(out.data() + kUndoWriteFixedSize, tail.data(), tail.size());
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
    decoded.fields.tail_len = Load<std::uint16_t>(in, kUndoTailLenOffset);

    if (Status s = CheckTxnId(decoded.fields.prior_trx_id, "UNDO_WRITE prior_trx_id"); !s.ok()) {
        return s;
    }
    if (in.size() - kUndoWriteFixedSize < decoded.fields.tail_len) {
        return Status::Corruption("wal payload: UNDO_WRITE tail_len runs past the payload");
    }
    if (static_cast<std::size_t>(decoded.fields.offset) + decoded.fields.tail_len > kPageSize) {
        // Replaying this would write outside the undo page (see the note
        // in EncodeUndoWrite: a sanity bound, not the authority).
        return Status::Corruption("wal payload: UNDO_WRITE tail runs past the undo page");
    }
    decoded.tail = in.subspan(kUndoWriteFixedSize, decoded.fields.tail_len);
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
    return kCheckpointBeginFixedSize + txn_count * kActiveTxnEntrySize +
           dirty_count * kDirtyEntrySize;
}

StatusOr<std::size_t> EncodeCheckpointBegin(std::span<std::byte> out,
                                            std::span<const CheckpointActiveTxn> active_txns,
                                            std::span<const CheckpointDirtyPage> dirty_pages) {
    if (active_txns.size() > 0xFFFFFFFFull || dirty_pages.size() > 0xFFFFFFFFull) {
        return Status::InvalidArgument("wal payload: CHECKPOINT_BEGIN table longer than a uint32");
    }
    const std::size_t total = CheckpointBeginSize(active_txns.size(), dirty_pages.size());
    if (Status s = CheckOutputSize(out, total, "CHECKPOINT_BEGIN"); !s.ok()) {
        return s;
    }
    for (const CheckpointActiveTxn& entry : active_txns) {
        if (entry.txn_id > kMaxTxnId) {
            return Status::InvalidArgument("wal payload: CHECKPOINT_BEGIN txn_id exceeds 48 bits");
        }
        if (entry.txn_id == kNoTxnId) {
            return Status::InvalidArgument("wal payload: CHECKPOINT_BEGIN lists txn_id 0");
        }
    }

    Store<std::uint32_t>(out, kCheckpointTxnCountOffset,
                         static_cast<std::uint32_t>(active_txns.size()));
    Store<std::uint32_t>(out, kCheckpointDirtyCountOffset,
                         static_cast<std::uint32_t>(dirty_pages.size()));

    std::size_t at = kCheckpointBeginFixedSize;
    for (const CheckpointActiveTxn& entry : active_txns) {
        Store<std::uint64_t>(out, at + kActiveTxnIdOffset, entry.txn_id);
        // No validation of last_undo_ptr here. `UndoPtrIsPlausible` lives in
        // txn/ and this layer must not reach up for it; the value is checked
        // where it is *followed*, which is recovery's undo phase (RC05), and
        // kNoUndoPtr is legal and means "wrote nothing yet".
        Store<std::uint64_t>(out, at + kActiveTxnLastUndoPtrOffset, entry.last_undo_ptr);
        at += kActiveTxnEntrySize;
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
        CheckpointActiveTxn entry{};
        entry.txn_id = Load<std::uint64_t>(in, at + kActiveTxnIdOffset);
        entry.last_undo_ptr = Load<std::uint64_t>(in, at + kActiveTxnLastUndoPtrOffset);
        if (Status s = CheckTxnId(entry.txn_id, "CHECKPOINT_BEGIN txn_id"); !s.ok()) {
            return s;
        }
        decoded.active_txns.push_back(entry);
        at += kActiveTxnEntrySize;
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

// ---- ASSERT_RESERVE / ASSERT_BUILD ---------------------------------------

StatusOr<std::size_t> EncodeAssertEntry(std::span<std::byte> out,
                                        const AssertEntryPayload& fields,
                                        std::span<const std::byte> entry,
                                        std::span<const std::byte> key) {
    if (entry.size() > 0xFFFF) {
        return Status::InvalidArgument("wal payload: assert entry longer than a uint16 length");
    }
    if (key.size() > 0xFFFF) {
        return Status::InvalidArgument("wal payload: assert group key longer than a uint16 length");
    }
    const std::size_t total = kAssertEntryFixedSize + entry.size() + key.size();
    if (Status s = CheckOutputSize(out, total, "ASSERT_RESERVE"); !s.ok()) {
        return s;
    }

    Store<std::uint64_t>(out, kAssertEntryAssertionIdOffset, fields.assertion_id);
    Store<std::uint16_t>(out, kAssertEntryIndexOffset, fields.index);
    // From the spans, never the caller's fields, so the lengths on disk and
    // the bytes on disk cannot disagree.
    Store<std::uint16_t>(out, kAssertEntryEntryLenOffset,
                         static_cast<std::uint16_t>(entry.size()));
    Store<std::uint16_t>(out, kAssertEntryKeyLenOffset, static_cast<std::uint16_t>(key.size()));
    Store<std::uint16_t>(out, kAssertEntryReservedOffset, 0);
    Store<std::uint32_t>(out, kAssertEntryGroupIdOffset, fields.group_id);
    if (!entry.empty()) {
        std::memcpy(out.data() + kAssertEntryFixedSize, entry.data(), entry.size());
    }
    if (!key.empty()) {
        std::memcpy(out.data() + kAssertEntryFixedSize + entry.size(), key.data(), key.size());
    }
    return total;
}

StatusOr<DecodedAssertEntry> DecodeAssertEntry(std::span<const std::byte> in) {
    if (Status s = CheckInputSize(in, kAssertEntryFixedSize, "ASSERT_RESERVE"); !s.ok()) {
        return s;
    }

    DecodedAssertEntry decoded{};
    decoded.fields.assertion_id = Load<std::uint64_t>(in, kAssertEntryAssertionIdOffset);
    decoded.fields.index = Load<std::uint16_t>(in, kAssertEntryIndexOffset);
    decoded.fields.entry_len = Load<std::uint16_t>(in, kAssertEntryEntryLenOffset);
    decoded.fields.key_len = Load<std::uint16_t>(in, kAssertEntryKeyLenOffset);
    decoded.fields.reserved = Load<std::uint16_t>(in, kAssertEntryReservedOffset);
    decoded.fields.group_id = Load<std::uint32_t>(in, kAssertEntryGroupIdOffset);

    const std::size_t tail =
        std::size_t{decoded.fields.entry_len} + std::size_t{decoded.fields.key_len};
    if (Status s = CheckInputSize(in, kAssertEntryFixedSize + tail, "ASSERT_RESERVE"); !s.ok()) {
        return s;
    }
    decoded.entry = in.subspan(kAssertEntryFixedSize, decoded.fields.entry_len);
    decoded.key =
        in.subspan(kAssertEntryFixedSize + decoded.fields.entry_len, decoded.fields.key_len);
    return decoded;
}

// ---- ASSERT_COMMIT -------------------------------------------------------

std::uint16_t DecodedAssertCommit::index_at(std::uint16_t i) const noexcept {
    return Load<std::uint16_t>(indexes, std::size_t{i} * sizeof(std::uint16_t));
}

StatusOr<std::size_t> EncodeAssertCommit(std::span<std::byte> out,
                                         const AssertCommitPayload& fields,
                                         std::span<const std::uint16_t> indexes) {
    if (indexes.size() > 0xFFFF) {
        return Status::InvalidArgument("wal payload: assert commit names more entries than a "
                                       "uint16 count");
    }
    const std::size_t total = kAssertCommitFixedSize + indexes.size() * sizeof(std::uint16_t);
    if (Status s = CheckOutputSize(out, total, "ASSERT_COMMIT"); !s.ok()) {
        return s;
    }

    Store<std::uint64_t>(out, kAssertCommitAssertionIdOffset, fields.assertion_id);
    Store<std::uint16_t>(out, kAssertCommitCountOffset,
                         static_cast<std::uint16_t>(indexes.size()));
    Store<std::uint16_t>(out, kAssertCommitReservedOffset, 0);
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        Store<std::uint16_t>(out, kAssertCommitFixedSize + i * sizeof(std::uint16_t),
                             indexes[i]);
    }
    return total;
}

StatusOr<DecodedAssertCommit> DecodeAssertCommit(std::span<const std::byte> in) {
    if (Status s = CheckInputSize(in, kAssertCommitFixedSize, "ASSERT_COMMIT"); !s.ok()) {
        return s;
    }

    DecodedAssertCommit decoded{};
    decoded.fields.assertion_id = Load<std::uint64_t>(in, kAssertCommitAssertionIdOffset);
    decoded.fields.count = Load<std::uint16_t>(in, kAssertCommitCountOffset);
    decoded.fields.reserved = Load<std::uint16_t>(in, kAssertCommitReservedOffset);

    const std::size_t tail = std::size_t{decoded.fields.count} * sizeof(std::uint16_t);
    if (Status s = CheckInputSize(in, kAssertCommitFixedSize + tail, "ASSERT_COMMIT"); !s.ok()) {
        return s;
    }
    decoded.indexes = in.subspan(kAssertCommitFixedSize, tail);
    return decoded;
}

// ---- ASSERT_ROLLBACK -----------------------------------------------------

StatusOr<std::size_t> EncodeAssertRollback(std::span<std::byte> out,
                                           const AssertRollbackPayload& fields,
                                           std::span<const std::byte> key) {
    if (key.size() > 0xFFFF) {
        return Status::InvalidArgument("wal payload: assert group key longer than a uint16 length");
    }
    const std::size_t total = kAssertRollbackFixedSize + key.size();
    if (Status s = CheckOutputSize(out, total, "ASSERT_ROLLBACK"); !s.ok()) {
        return s;
    }

    Store<std::uint64_t>(out, kAssertRollbackAssertionIdOffset, fields.assertion_id);
    Store<std::int64_t>(out, kAssertRollbackDeltaOffset, fields.delta);
    Store<std::uint16_t>(out, kAssertRollbackIndexOffset, fields.index);
    Store<std::uint16_t>(out, kAssertRollbackKeyLenOffset,
                         static_cast<std::uint16_t>(key.size()));
    Store<std::uint32_t>(out, kAssertRollbackReservedOffset, 0);
    if (!key.empty()) {
        std::memcpy(out.data() + kAssertRollbackFixedSize, key.data(), key.size());
    }
    return total;
}

StatusOr<DecodedAssertRollback> DecodeAssertRollback(std::span<const std::byte> in) {
    if (Status s = CheckInputSize(in, kAssertRollbackFixedSize, "ASSERT_ROLLBACK"); !s.ok()) {
        return s;
    }

    DecodedAssertRollback decoded{};
    decoded.fields.assertion_id = Load<std::uint64_t>(in, kAssertRollbackAssertionIdOffset);
    decoded.fields.delta = Load<std::int64_t>(in, kAssertRollbackDeltaOffset);
    decoded.fields.index = Load<std::uint16_t>(in, kAssertRollbackIndexOffset);
    decoded.fields.key_len = Load<std::uint16_t>(in, kAssertRollbackKeyLenOffset);
    decoded.fields.reserved = Load<std::uint32_t>(in, kAssertRollbackReservedOffset);

    if (Status s = CheckInputSize(in, kAssertRollbackFixedSize + decoded.fields.key_len,
                                  "ASSERT_ROLLBACK");
        !s.ok()) {
        return s;
    }
    decoded.key = in.subspan(kAssertRollbackFixedSize, decoded.fields.key_len);
    return decoded;
}

// ---- ASSERT_DROP ---------------------------------------------------------

StatusOr<std::size_t> EncodeAssertDrop(std::span<std::byte> out,
                                       const AssertDropPayload& fields) {
    if (Status s = CheckOutputSize(out, kAssertDropPayloadSize, "ASSERT_DROP"); !s.ok()) {
        return s;
    }
    Store<std::uint64_t>(out, kAssertDropAssertionIdOffset, fields.assertion_id);
    return kAssertDropPayloadSize;
}

StatusOr<AssertDropPayload> DecodeAssertDrop(std::span<const std::byte> in) {
    if (Status s = CheckInputSize(in, kAssertDropPayloadSize, "ASSERT_DROP"); !s.ok()) {
        return s;
    }
    AssertDropPayload fields{};
    fields.assertion_id = Load<std::uint64_t>(in, kAssertDropAssertionIdOffset);
    return fields;
}

// ---- ASSERT_SNAPSHOT -----------------------------------------------------

StatusOr<std::size_t> EncodeAssertSnapshot(std::span<std::byte> out,
                                          const AssertSnapshotPayload& fields,
                                          std::span<const SnapshotGroupEntry> groups) {
    std::size_t total = kAssertSnapshotFixedSize;
    for (const SnapshotGroupEntry& group : groups) {
        if (group.key.size() > 0xFFFFFFFFull) {
            return Status::InvalidArgument(
                "wal payload: assert snapshot group key longer than a uint32 length");
        }
        total += AssertSnapshotGroupBytes(group.key.size());
    }
    if (Status s = CheckOutputSize(out, total, "ASSERT_SNAPSHOT"); !s.ok()) {
        return s;
    }

    Store<std::uint64_t>(out, kAssertSnapshotAssertionIdOffset, fields.assertion_id);
    // From the span, never the caller's field, so the count on disk and the
    // blocks on disk cannot disagree.
    Store<std::uint32_t>(out, kAssertSnapshotGroupCountOffset,
                         static_cast<std::uint32_t>(groups.size()));
    Store<std::uint32_t>(out, kAssertSnapshotReservedOffset, 0);

    std::size_t at = kAssertSnapshotFixedSize;
    for (const SnapshotGroupEntry& group : groups) {
        Store<std::uint32_t>(out, at + kAssertSnapshotGroupIdOffset, group.group_id);
        Store<std::uint32_t>(out, at + kAssertSnapshotGroupKeyLenOffset,
                             static_cast<std::uint32_t>(group.key.size()));
        Store<std::uint64_t>(out, at + kAssertSnapshotGroupCountValueOffset,
                             static_cast<std::uint64_t>(group.count));
        Store<std::uint64_t>(out, at + kAssertSnapshotGroupSumOffset,
                             static_cast<std::uint64_t>(group.sum));
        if (!group.key.empty()) {
            std::memcpy(out.data() + at + kAssertSnapshotGroupFixedSize, group.key.data(),
                        group.key.size());
        }
        at += AssertSnapshotGroupBytes(group.key.size());
    }
    return total;
}

StatusOr<DecodedAssertSnapshot> DecodeAssertSnapshot(std::span<const std::byte> in) {
    if (Status s = CheckInputSize(in, kAssertSnapshotFixedSize, "ASSERT_SNAPSHOT"); !s.ok()) {
        return s;
    }

    DecodedAssertSnapshot decoded{};
    decoded.fields.assertion_id = Load<std::uint64_t>(in, kAssertSnapshotAssertionIdOffset);
    decoded.fields.group_count = Load<std::uint32_t>(in, kAssertSnapshotGroupCountOffset);
    decoded.fields.reserved = Load<std::uint32_t>(in, kAssertSnapshotReservedOffset);

    // Sized from the payload before anything is reserved, the rule
    // `DecodeCheckpointBegin` above follows and for the same reason: the count
    // is bytes off a device, and a corrupt `group_count` must not ask for an
    // allocation the record does not back with bytes. Each block costs at least
    // its fixed part, so that is the bound.
    const std::size_t max_groups =
        (in.size() - kAssertSnapshotFixedSize) / kAssertSnapshotGroupFixedSize;
    if (decoded.fields.group_count > max_groups) {
        return Status::Corruption("wal payload: ASSERT_SNAPSHOT claims " +
                                  std::to_string(decoded.fields.group_count) +
                                  " groups, more than its " + std::to_string(in.size()) +
                                  " bytes can hold");
    }

    std::size_t at = kAssertSnapshotFixedSize;
    decoded.groups.reserve(decoded.fields.group_count);
    for (std::uint32_t i = 0; i < decoded.fields.group_count; ++i) {
        // Every block is bounds-checked before it is read, because
        // `group_count` is bytes off a device: a record claiming more groups
        // than it carries must be Corruption and never a read past the end.
        if (Status s = CheckInputSize(in, at + kAssertSnapshotGroupFixedSize, "ASSERT_SNAPSHOT");
            !s.ok()) {
            return s;
        }
        SnapshotGroupEntry group;
        group.group_id = Load<std::uint32_t>(in, at + kAssertSnapshotGroupIdOffset);
        const std::uint32_t key_len = Load<std::uint32_t>(in, at + kAssertSnapshotGroupKeyLenOffset);
        group.count = static_cast<std::int64_t>(
            Load<std::uint64_t>(in, at + kAssertSnapshotGroupCountValueOffset));
        group.sum = static_cast<std::int64_t>(
            Load<std::uint64_t>(in, at + kAssertSnapshotGroupSumOffset));

        const std::size_t key_at = at + kAssertSnapshotGroupFixedSize;
        if (Status s = CheckInputSize(in, key_at + key_len, "ASSERT_SNAPSHOT"); !s.ok()) {
            return s;
        }
        group.key = in.subspan(key_at, key_len);
        decoded.groups.push_back(group);
        at = key_at + key_len;
    }
    return decoded;
}

}  // namespace kds::wal
