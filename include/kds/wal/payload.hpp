#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/wal/record.hpp"

// Per-type WAL record payloads (wal.md section 5.2). record.hpp owns the
// envelope - length, CRC, LSN, txn_id, page_id, type - and this file owns
// what rides inside it, one codec per record type.
//
// Encoding rules are record.hpp's, which are page_header.hpp's (rules.md
// sections 2 and 5): mirror struct, offsetof static_asserts, field-wise
// memcpy through named offsets, fixed-width little-endian, no bitfields.
//
// Three conventions hold across every payload here, and they are what make
// redo possible:
//
//   - Nothing is repeated from the envelope. The target page is the
//     record header's `page_id` and the writing transaction is its
//     `txn_id`, so no payload carries either. The one exception is
//     UNDO_WRITE's *prior* writer, which is a different transaction from
//     the one that wrote the record.
//   - Payloads are fixed-size or self-describing. DecodeRecord hands back
//     the whole aligned tail including up to 7 padding bytes (record.cpp),
//     so a variable-length payload that did not carry its own length could
//     not tell its last byte from padding. Every one of them carries a
//     length field for exactly that reason, and every decoder below
//     tolerates trailing bytes rather than requiring an exact size.
//   - Decoders reject what they cannot represent (a length that runs past
//     the payload, a txn_id above 48 bits) with Corruption. During replay a
//     malformed payload inside a CRC-valid record is not a torn tail - the
//     bytes are intact and wrong - so it is a hard recovery error, never
//     something to skip (wal.md section 5.2).
//
// TXN_BEGIN, TXN_COMMIT, TXN_ABORT and PAD have no payload: the envelope's
// txn_id already says everything they carry. TXN_BEGIN's durability class
// (wal.md section 1) is not encoded here yet - it belongs in the record
// header's per-type `flags` byte, and the class enum lives in the
// not-yet-written transaction layer.

namespace kds::wal {

// ---- PAGE_INIT -----------------------------------------------------------
//
// The sole logger of a heap page's min_key (wal.md section 14-2): min_key is
// immutable after creation, so it appears in this record and nowhere else.

struct PageInitPayload {
    std::uint64_t min_key;      // heap pages only; 0 for other page types
    std::uint8_t page_type;     // kds::PageType
    std::uint8_t reserved[3];   // 0
};

inline constexpr std::size_t kPageInitMinKeyOffset = 0;
inline constexpr std::size_t kPageInitPageTypeOffset = 8;
inline constexpr std::size_t kPageInitReservedOffset = 9;
// 8+1+3 = 12 bytes on disk.
inline constexpr std::size_t kPageInitPayloadSize = 12;

static_assert(offsetof(PageInitPayload, min_key) == kPageInitMinKeyOffset);
static_assert(offsetof(PageInitPayload, page_type) == kPageInitPageTypeOffset);
static_assert(offsetof(PageInitPayload, reserved) == kPageInitReservedOffset);

StatusOr<std::size_t> EncodePageInit(std::span<std::byte> out, const PageInitPayload& fields);
StatusOr<PageInitPayload> DecodePageInit(std::span<const std::byte> in);

// ---- HEAP_INSERT / HEAP_OVERWRITE ---------------------------------------
//
// One shape for both: an insert names a new slot, an overwrite names an
// existing one, and the bytes and stamps written are identical either way
// (wal.md section 5.2). `tuple` is the tuple payload as PageView sees it -
// starting at the Keystone word, without the 20-byte MVCC tuple header,
// whose two meaningful fields are `trx_id` and `undo_ptr` right here.
//
// `trx_id` is the writer, and it is *not* taken from the envelope: a redo
// of this record must stamp the tuple even when replayed outside any
// transaction context, so the value is explicit rather than inferred.

struct HeapWritePayload {
    std::uint64_t trx_id;     // writer, 48-bit zero-extended
    std::uint64_t undo_ptr;   // previous version, 0 if none
    std::uint16_t slot;
    std::uint16_t tuple_len;  // bytes of tuple payload that follow
};

inline constexpr std::size_t kHeapWriteTrxIdOffset = 0;
inline constexpr std::size_t kHeapWriteUndoPtrOffset = 8;
inline constexpr std::size_t kHeapWriteSlotOffset = 16;
inline constexpr std::size_t kHeapWriteTupleLenOffset = 18;
// 8+8+2+2 = 20; tuple bytes begin here.
inline constexpr std::size_t kHeapWriteFixedSize = 20;

static_assert(offsetof(HeapWritePayload, trx_id) == kHeapWriteTrxIdOffset);
static_assert(offsetof(HeapWritePayload, undo_ptr) == kHeapWriteUndoPtrOffset);
static_assert(offsetof(HeapWritePayload, slot) == kHeapWriteSlotOffset);
static_assert(offsetof(HeapWritePayload, tuple_len) == kHeapWriteTupleLenOffset);

struct DecodedHeapWrite {
    HeapWritePayload fields;
    std::span<const std::byte> tuple;  // view into the caller's buffer
};

// `fields.tuple_len` is ignored on encode - it is set from `tuple.size()`,
// so the two can never disagree on disk.
StatusOr<std::size_t> EncodeHeapWrite(std::span<std::byte> out, const HeapWritePayload& fields,
                                      std::span<const std::byte> tuple);
StatusOr<DecodedHeapWrite> DecodeHeapWrite(std::span<const std::byte> in);

// ---- VARHEAP_APPEND ------------------------------------------------------
//
// One spilled value landing in a var-heap page (docs/rule-fixed-length-
// tuple.md section 5). The target page is the envelope's `page_id`.
//
// Redo is an append at a *named* slot rather than "append wherever": the
// slot is recorded so replay reproduces the exact pointer the tuple's cell
// already carries. A pointer that resolved to a different slot after
// recovery would be a value silently swapped for another.
//
// Write ordering, which is the whole of the var-heap's recovery story:
// VARHEAP_APPEND precedes the HEAP_INSERT/HEAP_OVERWRITE whose cell points
// at it, in the same transaction, replayed by the ordinary winner/loser
// machinery. A crash between the two leaves an unreferenced value for
// purge's sweep to collect. **There is deliberately no var-heap-specific
// recovery logic**, and none may be added.

struct VarHeapAppendPayload {
    std::uint16_t slot;
    std::uint16_t reserved;   // 0
    std::uint32_t value_len;  // bytes of value that follow
};

inline constexpr std::size_t kVarHeapAppendSlotOffset = 0;
inline constexpr std::size_t kVarHeapAppendReservedOffset = 2;
inline constexpr std::size_t kVarHeapAppendValueLenOffset = 4;
// 2+2+4 = 8; value bytes begin here.
inline constexpr std::size_t kVarHeapAppendFixedSize = 8;

static_assert(offsetof(VarHeapAppendPayload, slot) == kVarHeapAppendSlotOffset);
static_assert(offsetof(VarHeapAppendPayload, reserved) == kVarHeapAppendReservedOffset);
static_assert(offsetof(VarHeapAppendPayload, value_len) == kVarHeapAppendValueLenOffset);
static_assert(sizeof(VarHeapAppendPayload) == kVarHeapAppendFixedSize);

struct DecodedVarHeapAppend {
    VarHeapAppendPayload fields;
    std::span<const std::byte> value;  // view into the caller's buffer
};

// `fields.value_len` is ignored on encode - it is set from `value.size()`,
// so the two can never disagree on disk.
StatusOr<std::size_t> EncodeVarHeapAppend(std::span<std::byte> out,
                                           const VarHeapAppendPayload& fields,
                                           std::span<const std::byte> value);
StatusOr<DecodedVarHeapAppend> DecodeVarHeapAppend(std::span<const std::byte> in);

// ---- INDEX_INSERT --------------------------------------------------------
//
// One entry appended to one secondary-index leaf (docs/feat-index.md §12.1).
// The record's `page_id` names the leaf, and the leaf's own header carries
// the widths - so redo needs neither the index's oid nor its layout, and
// there is no second place for either to be wrong.

struct IndexInsertPayload {
    std::uint16_t slot;       // the sorted position the entry took
    std::uint16_t entry_len;  // bytes of entry that follow
};

inline constexpr std::size_t kIndexInsertSlotOffset = 0;
inline constexpr std::size_t kIndexInsertEntryLenOffset = 2;
// 2+2 = 4; entry bytes begin here.
inline constexpr std::size_t kIndexInsertFixedSize = 4;

static_assert(offsetof(IndexInsertPayload, slot) == kIndexInsertSlotOffset);
static_assert(offsetof(IndexInsertPayload, entry_len) == kIndexInsertEntryLenOffset);
static_assert(sizeof(IndexInsertPayload) == kIndexInsertFixedSize);

struct DecodedIndexInsert {
    IndexInsertPayload fields;
    std::span<const std::byte> entry;  // view into the caller's buffer
};

// `fields.entry_len` is ignored on encode - it is set from `entry.size()`,
// so the two can never disagree on disk.
StatusOr<std::size_t> EncodeIndexInsert(std::span<std::byte> out,
                                         const IndexInsertPayload& fields,
                                         std::span<const std::byte> entry);
StatusOr<DecodedIndexInsert> DecodeIndexInsert(std::span<const std::byte> in);

// ---- HEAP_DELETE_MARK ----------------------------------------------------
//
// The whole of DELETE in the no-xmax model (wal.md section 5.1): a slot flag
// plus the deleter's id. No tuple bytes move, so none are logged.

struct HeapDeleteMarkPayload {
    std::uint64_t trx_id;  // the deleter
    std::uint16_t slot;
};

inline constexpr std::size_t kDeleteMarkTrxIdOffset = 0;
inline constexpr std::size_t kDeleteMarkSlotOffset = 8;
inline constexpr std::size_t kDeleteMarkPayloadSize = 10;

static_assert(offsetof(HeapDeleteMarkPayload, trx_id) == kDeleteMarkTrxIdOffset);
static_assert(offsetof(HeapDeleteMarkPayload, slot) == kDeleteMarkSlotOffset);

StatusOr<std::size_t> EncodeHeapDeleteMark(std::span<std::byte> out,
                                           const HeapDeleteMarkPayload& fields);
StatusOr<HeapDeleteMarkPayload> DecodeHeapDeleteMark(std::span<const std::byte> in);

// ---- SLOT_RETIRE ---------------------------------------------------------
//
// Physical retirement, deliberately a different record from the delete-mark
// above.
//
// **Who owns it depends on who emitted it** (docs/txn.md section 6's
// amendment). This comment used to say no transaction owns a SLOT_RETIRE
// and its envelope therefore carries kNoTxnId. That is true of a purge
// pass and false of a rollback compensation, which *is* owned by the
// aborting transaction - stamping kNoTxnId there would hide the rollback
// from recovery's analysis phase. So:
//
//   emitted by rollback     the aborting transaction's id
//   emitted by a purge pass kNoTxnId
//
// Nothing purges yet, so today every SLOT_RETIRE in a stream is a rollback
// compensation (txn/manager.cpp).

struct SlotRetirePayload {
    std::uint16_t slot;
};

inline constexpr std::size_t kSlotRetireSlotOffset = 0;
inline constexpr std::size_t kSlotRetirePayloadSize = 2;

static_assert(offsetof(SlotRetirePayload, slot) == kSlotRetireSlotOffset);

StatusOr<std::size_t> EncodeSlotRetire(std::span<std::byte> out, const SlotRetirePayload& fields);
StatusOr<SlotRetirePayload> DecodeSlotRetire(std::span<const std::byte> in);

// ---- UNDO_WRITE ----------------------------------------------------------
//
// An append to an undo page (the envelope's page_id). The payload is the
// before-image plus the chain link - the *prior* writer and the prior
// undo_ptr - which is what lets a reader reconstruct validity intervals with
// no xmax stored anywhere (wal.md sections 5.1, 5.2).
//
// The undo page's own layout is [OPEN] (wal.md section 15), so `offset` is
// deliberately just a byte offset within the page: this codec describes the
// bytes written, not where the undo allocator chose to put them.

struct UndoWritePayload {
    std::uint64_t prior_trx_id;    // writer of the version being superseded
    std::uint64_t prior_undo_ptr;  // its own predecessor; 0 ends the chain
    std::uint16_t offset;          // byte offset within the undo page
    std::uint16_t image_len;       // bytes of before-image that follow
};

inline constexpr std::size_t kUndoPriorTrxIdOffset = 0;
inline constexpr std::size_t kUndoPriorUndoPtrOffset = 8;
inline constexpr std::size_t kUndoOffsetOffset = 16;
inline constexpr std::size_t kUndoImageLenOffset = 18;
inline constexpr std::size_t kUndoWriteFixedSize = 20;

static_assert(offsetof(UndoWritePayload, prior_trx_id) == kUndoPriorTrxIdOffset);
static_assert(offsetof(UndoWritePayload, prior_undo_ptr) == kUndoPriorUndoPtrOffset);
static_assert(offsetof(UndoWritePayload, offset) == kUndoOffsetOffset);
static_assert(offsetof(UndoWritePayload, image_len) == kUndoImageLenOffset);

struct DecodedUndoWrite {
    UndoWritePayload fields;
    std::span<const std::byte> image;
};

StatusOr<std::size_t> EncodeUndoWrite(std::span<std::byte> out, const UndoWritePayload& fields,
                                      std::span<const std::byte> image);
StatusOr<DecodedUndoWrite> DecodeUndoWrite(std::span<const std::byte> in);

// ---- ALLOC / FREE --------------------------------------------------------
//
// A run of pages starting at the envelope's page_id. ALLOC is logged before
// the file is extended (wal.md section 5.2), which is what makes replay of a
// crash mid-growth safe.

struct PageRunPayload {
    std::uint32_t nr_pages;
};

inline constexpr std::size_t kPageRunNrPagesOffset = 0;
inline constexpr std::size_t kPageRunPayloadSize = 4;

static_assert(offsetof(PageRunPayload, nr_pages) == kPageRunNrPagesOffset);

StatusOr<std::size_t> EncodePageRun(std::span<std::byte> out, const PageRunPayload& fields);
StatusOr<PageRunPayload> DecodePageRun(std::span<const std::byte> in);

// ---- FULL_PAGE_IMAGE -----------------------------------------------------
//
// The whole page, verbatim, for the first modification of a headered page
// after each checkpoint (wal.md section 10). No fixed prefix: the page id is
// the envelope's, and the length is kPageSize by definition. kPageSize is a
// multiple of the record alignment, so an FPI record never carries padding.

inline constexpr std::size_t kFullPageImagePayloadSize = kPageSize;
static_assert(kFullPageImagePayloadSize % kRecordAlignment == 0);

StatusOr<std::size_t> EncodeFullPageImage(std::span<std::byte> out,
                                          std::span<const std::byte, kPageSize> page);
StatusOr<std::span<const std::byte>> DecodeFullPageImage(std::span<const std::byte> in);

// ---- CHECKPOINT_BEGIN ----------------------------------------------------
//
// The two tables recovery's analysis phase starts from (wal.md sections 11,
// 12): which transactions were live, and which pages were dirty with the LSN
// each first became dirty at. Both are variable-length, so both are counted.

struct CheckpointDirtyPage {
    PageId page_id;
    Lsn rec_lsn;  // oldest LSN that must be replayed to make the page whole
};

inline constexpr std::size_t kCheckpointTxnCountOffset = 0;
inline constexpr std::size_t kCheckpointDirtyCountOffset = 4;
inline constexpr std::size_t kCheckpointBeginFixedSize = 8;
// Then `txn_count` 8-byte ids, then `dirty_count` entries of:
inline constexpr std::size_t kDirtyPageIdOffset = 0;
inline constexpr std::size_t kDirtyRecLsnOffset = 4;
inline constexpr std::size_t kDirtyEntrySize = 12;

struct DecodedCheckpointBegin {
    std::vector<std::uint64_t> active_txns;
    std::vector<CheckpointDirtyPage> dirty_pages;
};

// Bytes this table pair needs, so a caller can size its buffer before
// deciding whether the checkpoint record fits the segment.
std::size_t CheckpointBeginSize(std::size_t txn_count, std::size_t dirty_count) noexcept;

StatusOr<std::size_t> EncodeCheckpointBegin(std::span<std::byte> out,
                                            std::span<const std::uint64_t> active_txns,
                                            std::span<const CheckpointDirtyPage> dirty_pages);
StatusOr<DecodedCheckpointBegin> DecodeCheckpointBegin(std::span<const std::byte> in);

// ---- CHECKPOINT_END ------------------------------------------------------
//
// The redo start this checkpoint established - min(rec_lsn) over the dirty
// table, or the checkpoint's own LSN when nothing was dirty. Recovery reads
// it from the superblock anchor; it is logged too so a stream is
// self-describing without one (wal.md section 11-3).

struct CheckpointEndPayload {
    Lsn redo_start_lsn;
};

inline constexpr std::size_t kCheckpointEndRedoStartOffset = 0;
inline constexpr std::size_t kCheckpointEndPayloadSize = 8;

static_assert(offsetof(CheckpointEndPayload, redo_start_lsn) == kCheckpointEndRedoStartOffset);

StatusOr<std::size_t> EncodeCheckpointEnd(std::span<std::byte> out,
                                          const CheckpointEndPayload& fields);
StatusOr<CheckpointEndPayload> DecodeCheckpointEnd(std::span<const std::byte> in);

// ---- ASSERT_RESERVE / ASSERT_BUILD ---------------------------------------
//
// One Bound Cabin entry landing in the page the envelope names
// (docs/feat-assertion.md §7, workplan AST05). One shape for both record
// types, HEAP_INSERT/HEAP_OVERWRITE's precedent: a reservation and a build
// row write identical bytes and differ in ownership - RESERVE is txn-owned
// with kEntryReserved set in the entry, BUILD is DDL-owned (kNoTxnId) with
// it clear - and the replay handler (exec/assertion_replay.hpp) checks the
// flag agrees with the type, so the two cannot be confused on disk.
//
// The entry rides as opaque bytes through storage/cabin_bound_page.hpp's
// codec - INDEX_INSERT's rule: the one place that knows the entry layout
// stays the one place. The **group key rides beside it** because an entry
// does not carry its group: the key is the canonical encoding of the GROUP
// BY values (exec::EncodeGroupKey), and carrying it is what lets replay
// rebuild the memory-resident group directory without re-reading any
// relation row.
//
// There is deliberately no delta field: the entry's inline aggregate value
// **is** the group delta (a COUNT assertion writes 1, §5.1) - one number,
// one place to be wrong.

struct AssertEntryPayload {
    std::uint64_t assertion_id;  // the sys.assertions Keystone id
    std::uint16_t index;         // the slot the entry took in the envelope's page
    std::uint16_t entry_len;     // bytes of entry that follow
    std::uint16_t key_len;       // bytes of group key that follow the entry
    std::uint16_t reserved;      // 0
};

inline constexpr std::size_t kAssertEntryAssertionIdOffset = 0;
inline constexpr std::size_t kAssertEntryIndexOffset = 8;
inline constexpr std::size_t kAssertEntryEntryLenOffset = 10;
inline constexpr std::size_t kAssertEntryKeyLenOffset = 12;
inline constexpr std::size_t kAssertEntryReservedOffset = 14;
// 8+2+2+2+2 = 16; entry bytes begin here, key bytes after them.
inline constexpr std::size_t kAssertEntryFixedSize = 16;

static_assert(offsetof(AssertEntryPayload, assertion_id) == kAssertEntryAssertionIdOffset);
static_assert(offsetof(AssertEntryPayload, index) == kAssertEntryIndexOffset);
static_assert(offsetof(AssertEntryPayload, entry_len) == kAssertEntryEntryLenOffset);
static_assert(offsetof(AssertEntryPayload, key_len) == kAssertEntryKeyLenOffset);
static_assert(offsetof(AssertEntryPayload, reserved) == kAssertEntryReservedOffset);
static_assert(sizeof(AssertEntryPayload) == kAssertEntryFixedSize);

struct DecodedAssertEntry {
    AssertEntryPayload fields;
    std::span<const std::byte> entry;  // view into the caller's buffer
    std::span<const std::byte> key;    // view into the caller's buffer
};

// `fields.entry_len` and `fields.key_len` are ignored on encode - both are
// set from their spans, so the lengths and the bytes cannot disagree on
// disk.
StatusOr<std::size_t> EncodeAssertEntry(std::span<std::byte> out,
                                        const AssertEntryPayload& fields,
                                        std::span<const std::byte> entry,
                                        std::span<const std::byte> key);
StatusOr<DecodedAssertEntry> DecodeAssertEntry(std::span<const std::byte> in);

// ---- ASSERT_COMMIT -------------------------------------------------------
//
// The reserved→committed flag transition for one transaction's entries **on
// one page** - the envelope's. Batched per page rather than per transaction
// because a physiological record describes one page's mutation; the spec's
// "batched per txn" (§7) is met one page at a time, and a transaction whose
// reservations span N pages commits them with N of these. The group
// directory is untouched by replay of one: a reservation counts in the
// aggregate from the moment of admission (§6.2), so commit moves flags and
// never sums.

struct AssertCommitPayload {
    std::uint64_t assertion_id;
    std::uint16_t count;     // how many little-endian u16 indexes follow
    std::uint16_t reserved;  // 0
};

inline constexpr std::size_t kAssertCommitAssertionIdOffset = 0;
inline constexpr std::size_t kAssertCommitCountOffset = 8;
inline constexpr std::size_t kAssertCommitReservedOffset = 10;
// 8+2+2 = 12; the index array begins here.
inline constexpr std::size_t kAssertCommitFixedSize = 12;

static_assert(offsetof(AssertCommitPayload, assertion_id) == kAssertCommitAssertionIdOffset);
static_assert(offsetof(AssertCommitPayload, count) == kAssertCommitCountOffset);
static_assert(offsetof(AssertCommitPayload, reserved) == kAssertCommitReservedOffset);
// No sizeof assert: the struct pads to 16 for its u64's alignment while the
// wire form is 12 - HEAP_DELETE_MARK's situation, handled the same way.

struct DecodedAssertCommit {
    AssertCommitPayload fields;
    std::span<const std::byte> indexes;  // fields.count little-endian u16s

    // The i-th cleared entry's page slot; `i < fields.count` is the
    // caller's to honour, as a span index would be.
    std::uint16_t index_at(std::uint16_t i) const noexcept;
};

// `fields.count` is ignored on encode - it is set from `indexes.size()`.
StatusOr<std::size_t> EncodeAssertCommit(std::span<std::byte> out,
                                         const AssertCommitPayload& fields,
                                         std::span<const std::uint16_t> indexes);
StatusOr<DecodedAssertCommit> DecodeAssertCommit(std::span<const std::byte> in);

// ---- ASSERT_ROLLBACK -----------------------------------------------------
//
// One reserved entry compensated (§6.2 step 5, or recovery closing a
// crashed in-flight reservation): the delta leaves the group's aggregate
// and the directory forgets the entry at (envelope page_id, index). The
// page entry itself is **not** rewritten - the slot is orphaned, see the
// record-type comment - so the payload carries the delta rather than
// pointing replay at page bytes whose flags no longer distinguish an
// aborted entry from a live reservation.

struct AssertRollbackPayload {
    std::uint64_t assertion_id;
    std::int64_t delta;      // the reserved delta as applied; replay subtracts it
    std::uint16_t index;     // the orphaned entry's slot in the envelope's page
    std::uint16_t key_len;   // bytes of group key that follow
    std::uint32_t reserved;  // 0
};

inline constexpr std::size_t kAssertRollbackAssertionIdOffset = 0;
inline constexpr std::size_t kAssertRollbackDeltaOffset = 8;
inline constexpr std::size_t kAssertRollbackIndexOffset = 16;
inline constexpr std::size_t kAssertRollbackKeyLenOffset = 18;
inline constexpr std::size_t kAssertRollbackReservedOffset = 20;
// 8+8+2+2+4 = 24; key bytes begin here.
inline constexpr std::size_t kAssertRollbackFixedSize = 24;

static_assert(offsetof(AssertRollbackPayload, assertion_id) == kAssertRollbackAssertionIdOffset);
static_assert(offsetof(AssertRollbackPayload, delta) == kAssertRollbackDeltaOffset);
static_assert(offsetof(AssertRollbackPayload, index) == kAssertRollbackIndexOffset);
static_assert(offsetof(AssertRollbackPayload, key_len) == kAssertRollbackKeyLenOffset);
static_assert(offsetof(AssertRollbackPayload, reserved) == kAssertRollbackReservedOffset);
static_assert(sizeof(AssertRollbackPayload) == kAssertRollbackFixedSize);

struct DecodedAssertRollback {
    AssertRollbackPayload fields;
    std::span<const std::byte> key;  // view into the caller's buffer
};

// `fields.key_len` is ignored on encode - it is set from `key.size()`.
StatusOr<std::size_t> EncodeAssertRollback(std::span<std::byte> out,
                                           const AssertRollbackPayload& fields,
                                           std::span<const std::byte> key);
StatusOr<DecodedAssertRollback> DecodeAssertRollback(std::span<const std::byte> in);

// ---- ASSERT_DROP ---------------------------------------------------------
//
// Teardown: replay forgets everything held for the assertion. The pages
// return through ordinary FREE records; the envelope's page_id names the
// cabin root as a diagnostic and replay does not touch it.

struct AssertDropPayload {
    std::uint64_t assertion_id;
};

inline constexpr std::size_t kAssertDropAssertionIdOffset = 0;
inline constexpr std::size_t kAssertDropPayloadSize = 8;

static_assert(offsetof(AssertDropPayload, assertion_id) == kAssertDropAssertionIdOffset);

StatusOr<std::size_t> EncodeAssertDrop(std::span<std::byte> out,
                                       const AssertDropPayload& fields);
StatusOr<AssertDropPayload> DecodeAssertDrop(std::span<const std::byte> in);

}  // namespace kds::wal
