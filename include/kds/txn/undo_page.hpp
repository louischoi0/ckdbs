#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/page_header.hpp"

// Undo page layout and the undo record codec (docs/spec/txn.md section 3).
//
// An undo page holds the before-images that let a reader reconstruct the
// version of a tuple its snapshot is entitled to see, and that let an
// aborting transaction put the bytes back. It closes docs/spec/wal.md section
// 15's "Undo-page layout details (UNDO_WRITE targets)".
//
// ---- Headered, and not by preference -------------------------------------
//
// wal.md section 9 already lists undo among the classes carrying the common
// 32-byte page header, and page.md section 1 names the waystone directory's
// interior pages as the *only* headerless class. The page needs the
// checksum, and more importantly the `page_lsn` the WAL-before-data gate
// reads (device_page_store.hpp), because undo writes are themselves
// WAL-logged. `DevicePageStore::CreateNewHeaderless()` must never be used
// for one.
//
// ---- Layout ---------------------------------------------------------------
//
//   byte 0     common page header (32 B)   kUndo, checksum @4, page_lsn @8
//   byte 32    UndoPageHeaderFields (24 B)
//   byte 56    UndoRecord 0, 1, 2, ...     append-only, grows upward to `lower`
//   byte 8192  end
//
// Records grow *upward* and there is no slot directory, which is the whole
// difference from a heap page: an undo record is never addressed by index,
// only by the byte offset a tuple's `undo_ptr` already carries. Nothing
// searches an undo page, so nothing needs a directory to search.
//
// ---- Concurrency ----------------------------------------------------------
//
// Pure functions over a caller-owned page buffer, like page_header.hpp. The
// caller holds whatever pin/latch the page needs; this file takes none.

namespace kds::txn {

// ---- Page header ---------------------------------------------------------

// Where the undo page's own header begins: immediately after the common
// one. Every offset constant below is relative to *this*, not to the page.
inline constexpr std::size_t kUndoHeaderOffset = storage::kPageBodyOffset;

struct UndoPageHeaderFields {
    std::uint16_t flags;
    // O(1) "is this page empty". The live purge keeps its bound in memory
    // and never reads this; what still wants it is UP4's mount-time
    // reclaim of a previous run's pages (docs/inflight/in-progress/workplan-undo-purge.md),
    // whose sweep would otherwise walk every record to learn a page holds
    // nothing.
    std::uint16_t nr_records;
    // **Absolute** page offset of the next free byte, for the reason
    // HeapPageHeaderFields::lower is: it is compared against kPageSize and
    // used directly as a memcpy destination, and a body-relative value
    // would invite one missing `+ kPageBodyOffset`.
    std::uint16_t lower;
    std::uint16_t reserved0;  // 0
    // The transaction whose append created this page; 0 = unknown. It is a
    // **diagnostic, not an owner**: an undo page is shared by every
    // transaction that appends to it while it has room, because a page per
    // transaction costs 8 KB per autocommitted UPDATE and autocommit is one
    // transaction per statement (bench/results-txn-layer-budget.md §3).
    // Nothing reads this field to decide anything, and nothing may - a
    // record's writer is in the UNDO_WRITE envelope, and a record's
    // *reachability* is a property of the tuples pointing at it.
    std::uint64_t first_trx_id;
    // The log's previous undo page - **not** the previous page of any one
    // transaction, which sharing makes unanswerable. It chains the pages
    // in creation order. **Historical once page reuse starts**: the purge
    // (docs/inflight/in-progress/workplan-undo-purge.md) recycles a settled page without
    // rewriting the link that points at it, so a device walk can revisit
    // a reused page - which is why UndoLog::PageCount() counts the
    // in-memory chain instead. The purge keeps its per-page bound in
    // memory too: the bound is a 48-bit writer id, this field's neighbour
    // reserved1 is 32 bits, and this run's chain is the only one the log
    // appends to, so the side table needs no on-disk home. reserved1
    // stays reserved.
    PageId prev_page_id;
    std::uint32_t reserved1;  // 0
};

inline constexpr std::size_t kUndoHeaderFlagsOffset = 0;
inline constexpr std::size_t kUndoHeaderNrRecordsOffset = 2;
inline constexpr std::size_t kUndoHeaderLowerOffset = 4;
inline constexpr std::size_t kUndoHeaderReserved0Offset = 6;
inline constexpr std::size_t kUndoHeaderFirstTrxIdOffset = 8;
inline constexpr std::size_t kUndoHeaderPrevPageIdOffset = 16;
inline constexpr std::size_t kUndoHeaderReserved1Offset = 20;
// 2+2+2+2+8+4+4 = 24, every field naturally aligned and no tail padding at
// this size, so sizeof() is safe to assert directly.
inline constexpr std::size_t kUndoPageHeaderSize = 24;

static_assert(offsetof(UndoPageHeaderFields, flags) == kUndoHeaderFlagsOffset);
static_assert(offsetof(UndoPageHeaderFields, nr_records) == kUndoHeaderNrRecordsOffset);
static_assert(offsetof(UndoPageHeaderFields, lower) == kUndoHeaderLowerOffset);
static_assert(offsetof(UndoPageHeaderFields, reserved0) == kUndoHeaderReserved0Offset);
static_assert(offsetof(UndoPageHeaderFields, first_trx_id) == kUndoHeaderFirstTrxIdOffset);
static_assert(offsetof(UndoPageHeaderFields, prev_page_id) == kUndoHeaderPrevPageIdOffset);
static_assert(offsetof(UndoPageHeaderFields, reserved1) == kUndoHeaderReserved1Offset);
static_assert(sizeof(UndoPageHeaderFields) == kUndoPageHeaderSize);

inline constexpr std::uint16_t kUndoPageFlagInitialized = 0x1;

// Where the first record sits, and how much record space a page holds.
inline constexpr std::size_t kUndoRecordsOffset = kUndoHeaderOffset + kUndoPageHeaderSize;
inline constexpr std::size_t kUndoPageCapacity = kPageSize - kUndoRecordsOffset;

static_assert(kUndoRecordsOffset == 56);
static_assert(kUndoPageCapacity == 8136);

// ---- Undo record ---------------------------------------------------------

enum class UndoRecordType : std::uint8_t {
    kInvalid = 0,
    // The full prior tuple payload, as PageView sees it - starting at the
    // Keystone word, without the MVCC tuple header.
    kOverwrite = 1,
    // Image empty: a delete-mark changes no tuple bytes, so there are none
    // to restore.
    kDeleteMark = 2,
    // Image empty. **Written since 2026-08-11** (RV10,
    // docs/workplan-wal-recovery.md §4b), reversing txn.md section 3.6.
    //
    // Nothing about *visibility* changed and section 3.6 was right about
    // it: a tuple with undo_ptr == kNoUndoPtr whose writer is invisible
    // already means "no visible version", so reading an insert needs no
    // record. What needs one is **recovery**, and for a reason wider than
    // the insert - an undo record is now a link in the writing
    // transaction's chain (`txn_prev_undo_ptr` below), and an insert that
    // wrote none would break that chain and orphan everything the
    // transaction did before it.
    kInsert = 3,
};

struct UndoRecordFields {
    std::uint64_t prior_trx_id;    // writer of the version being superseded
    std::uint64_t prior_undo_ptr;  // its own predecessor; kNoUndoPtr ends the chain
    PageId target_page_id;         // the heap page holding the tuple
    std::uint16_t target_slot;
    std::uint16_t image_len;
    std::uint8_t type;      // UndoRecordType
    std::uint8_t flags;     // 0
    std::uint16_t reserved; // 0

    // ---- RV10's two additions -------------------------------------------

    // **The third chain.** The writing transaction's previous undo record,
    // kNoUndoPtr for its first. `undo_log.hpp` names the other two and says
    // they are not the same chain: `prev_page_id` is page->page in creation
    // order and `prior_undo_ptr` is record->record over *one tuple's
    // versions*. Neither answers "what did this transaction do", which is
    // the question recovery's undo phase asks and could not previously ask
    // of anything durable - its only other source is the WAL inside the
    // replay range, and a page written back before a checkpoint puts a
    // still-uncommitted write outside it (workplan-wal-recovery.md §4b).
    //
    // Walked from the head `CHECKPOINT_BEGIN` records per active
    // transaction, so its reach does not depend on the redo start.
    std::uint64_t txn_prev_undo_ptr;

    // The Keystone id of the row this record is about, zero-extended
    // (invariant 7 - the upper 24 bits are always 0).
    //
    // Here because compensation must prove it is writing the row it means
    // to before it writes: a btree leaf division moves tuples and renumbers
    // slots, so `(target_page_id, target_slot)` is where the row *was*
    // (`TransactionManager::Compensate`, and workplan-wal-recovery.md §4a).
    // The live path reads the pk from its in-memory trail; recovery has
    // only this record, and **two of the three types carry no image to
    // recover a pk from** - kDeleteMark's is empty by design and kInsert's
    // is empty too. Storing it once, for every type, is what makes the
    // check uniform rather than available for kOverwrite alone.
    std::uint64_t pk;
};

inline constexpr std::size_t kUndoRecPriorTrxIdOffset = 0;
inline constexpr std::size_t kUndoRecPriorUndoPtrOffset = 8;
inline constexpr std::size_t kUndoRecTargetPageIdOffset = 16;
inline constexpr std::size_t kUndoRecTargetSlotOffset = 20;
inline constexpr std::size_t kUndoRecImageLenOffset = 22;
inline constexpr std::size_t kUndoRecTypeOffset = 24;
inline constexpr std::size_t kUndoRecFlagsOffset = 25;
inline constexpr std::size_t kUndoRecReservedOffset = 26;
inline constexpr std::size_t kUndoRecTxnPrevUndoPtrOffset = 28;
inline constexpr std::size_t kUndoRecPkOffset = 36;
// 8+8+4+2+2+1+1+2+8+8 = 44; image bytes begin here.
//
// **Grew 28 -> 44 at RV10**, and the cost is stated rather than absorbed:
// 16 bytes per undo record, so a kOverwrite carrying a 64-byte image goes
// from 92 to 108 bytes and a page holds ~17 % fewer of them. That is the
// price of a chain that can be walked without the log. It used to compound
// with the reclamation gap, undo pages being unpurgeable; since
// `docs/inflight/in-progress/workplan-undo-purge.md` it costs a higher steady-state page count
// instead, ~17 % more growths per unit of undo written.
//
// The two new fields are appended rather than fitted into `reserved`, which
// is 2 bytes and holds neither. Appending also keeps every existing offset
// where it was, so the *tail* mapping below extends rather than moves.
//
// Records are **unpadded**. Every access is a field-wise memcpy (rules.md
// section 2), so alignment buys nothing, and 8-byte padding would waste up
// to 7 bytes on a page holding ~290 records. `lower` advances by exactly
// kUndoRecordHeaderSize + image_len.
inline constexpr std::size_t kUndoRecordHeaderSize = 44;

static_assert(offsetof(UndoRecordFields, prior_trx_id) == kUndoRecPriorTrxIdOffset);
static_assert(offsetof(UndoRecordFields, prior_undo_ptr) == kUndoRecPriorUndoPtrOffset);
static_assert(offsetof(UndoRecordFields, target_page_id) == kUndoRecTargetPageIdOffset);
static_assert(offsetof(UndoRecordFields, target_slot) == kUndoRecTargetSlotOffset);
static_assert(offsetof(UndoRecordFields, image_len) == kUndoRecImageLenOffset);
static_assert(offsetof(UndoRecordFields, type) == kUndoRecTypeOffset);
static_assert(offsetof(UndoRecordFields, flags) == kUndoRecFlagsOffset);
static_assert(offsetof(UndoRecordFields, reserved) == kUndoRecReservedOffset);
// **RV10's two fields get no offsetof assert, and cannot have one.** The
// record is deliberately unpadded (see kUndoRecordHeaderSize below), so
// `txn_prev_undo_ptr` sits at on-disk offset 28 and `pk` at 36 - neither
// 8-byte aligned. The compiler cannot place a `uint64_t` member there, so
// it pads the *struct* to 32 and 40, and asserting the two against each
// other compares an in-memory offset with an on-disk one.
//
// The asserts above survive only because every field before these happens
// to be naturally aligned; that was luck, not a property, and it ran out
// here. Nothing is lost by dropping them: the codec never lays the struct
// over page bytes, it memcpy's each field through the constant above
// (`undo_page.cpp`, and the tail encoder at kUndoRecordTailOffset), which
// is exactly what makes the unpadded layout legal. `catalog/rows.hpp`
// carries the same note for the same reason - everything from `next_id` on
// is unasserted there.
// sizeof(UndoRecordFields) is 48, not 44: the u64 members give the struct
// 8-byte alignment, so its size rounds up. Deliberately not asserted
// against - those four tail bytes are never touched, because fields are
// memcpy'd one at a time through the offsets above and never as a struct.
// TupleHeaderFields carries the same note for the same reason.

// The largest before-image one record can carry: a whole empty page's
// record space, less the one record header it needs.
//
// **Known ceiling, recorded rather than hidden** (txn.md section 3.3): undo
// overhead is 32 + 24 + 44 = 100 bytes against the heap page's
// 32 + 16 + 5 + 20 + 4 = 77, so a tuple within 23 bytes of the maximum heap
// payload cannot be updated - the undo append fails OutOfSpace naming the
// *undo* page. **The band was ~7 bytes before RV10** and widened with the
// record header; `UndoPageTest.TheWidestHeapTupleCannotBeUndone` pins the
// number so it cannot drift again unnoticed. The deferred fix is a spilling image or a long-image record
// type; neither is invented here to answer a question nobody has hit.
inline constexpr std::size_t kMaxUndoImageLen = kUndoPageCapacity - kUndoRecordHeaderSize;

// 8136 - 44. **Was 8108**, which is 8136 - 28: the literal was left behind
// when RV10 grew the header by its two fields. Derived-then-asserted only
// works if the literal moves with the derivation.
static_assert(kMaxUndoImageLen == 8092);

// ---- undo_ptr ------------------------------------------------------------
//
//     undo_ptr = (uint64(page_id) << 16) | offset
//
// The page id occupies bits 16..47, so bits 48..63 are always zero - the
// same zero-extension convention invariant 6 imposes on ids and trx_id.
// Explicit shift/mask, never a bitfield: this is a persisted encoding and
// bitfield layout is implementation-defined (rules.md section 5).

inline constexpr int kUndoPtrPageIdShift = 16;
inline constexpr std::uint64_t kUndoPtrOffsetMask = 0xFFFFu;

// **kNoUndoPtr means "no predecessor", and it is unambiguous structurally**
// rather than by convention: page 0 is the superblock, and offset 0 is
// inside the common page header, below kUndoRecordsOffset. Neither can ever
// name a real undo record, so the sentinel costs no encodable value.
inline constexpr std::uint64_t kNoUndoPtr = 0;

constexpr std::uint64_t EncodeUndoPtr(PageId page_id, std::uint16_t offset) noexcept {
    return (static_cast<std::uint64_t>(page_id) << kUndoPtrPageIdShift) |
           static_cast<std::uint64_t>(offset);
}

constexpr PageId UndoPtrPageId(std::uint64_t ptr) noexcept {
    return static_cast<PageId>(ptr >> kUndoPtrPageIdShift);
}

constexpr std::uint16_t UndoPtrOffset(std::uint64_t ptr) noexcept {
    return static_cast<std::uint16_t>(ptr & kUndoPtrOffsetMask);
}

static_assert(UndoPtrPageId(EncodeUndoPtr(0x0A0B0C0Du, 0x1234)) == 0x0A0B0C0Du);
static_assert(UndoPtrOffset(EncodeUndoPtr(0x0A0B0C0Du, 0x1234)) == 0x1234);
static_assert(EncodeUndoPtr(0, kUndoRecordsOffset) != kNoUndoPtr);

// Rejects a pointer this build must not follow: page 0, an offset outside
// [kUndoRecordsOffset, kPageSize - kUndoRecordHeaderSize], or nonzero upper
// 16 bits. Reports Corruption rather than a bool, because a stored pointer
// that fails this is a damaged chain and not a miss - unlike a Waystone
// entry, undo is authoritative data (invariant 8 does not apply to it).
//
// kNoUndoPtr is **not** plausible: it is the chain terminator, and a caller
// that reached here holding one has already failed to check for it.
Status UndoPtrIsPlausible(std::uint64_t ptr);

// ---- The record tail, as the WAL carries it ------------------------------
//
// An UNDO_WRITE record logs the undo record's bytes from `target_page_id`
// onward - `docs/spec/txn.md` §3.5's `payload.image = record bytes
// [+16, +28 + image_len)`. The two chain-link fields before that offset
// ride as payload *fields* instead, because prior_trx_id names a different
// transaction from the record's envelope and repeating it inside the bytes
// would store one fact twice.
//
// The tail therefore carries exactly what the links do not:
// `target_page_id`, `target_slot`, `image_len`, `type`, `flags`,
// `reserved`, then the before-image. **Those are the fields that say which
// tuple a before-image belongs to**, which is why redo cannot rebuild a
// chain without them (docs/workplan-wal-recovery.md RC03).
//
// One encoder and one decoder, used by the writer and by redo, so the two
// cannot disagree about the shape - the rule exec/tuple_verify.hpp states
// for its own verifier.

inline constexpr std::size_t kUndoRecordTailOffset = kUndoRecTargetPageIdOffset;  // 16
inline constexpr std::size_t kUndoRecordTailHeaderSize =
    kUndoRecordHeaderSize - kUndoRecordTailOffset;  // 28 since RV10, was 12
// Derived, then asserted against the literal, so the two new fields cannot
// be added to the record and forgotten in the tail redo reads back.
static_assert(kUndoRecordTailHeaderSize == 28);

// Bytes a tail occupies for an image of `image_len`.
std::size_t UndoRecordTailSize(std::size_t image_len) noexcept;

// Writes the tail into `out`. `fields.image_len` is ignored and set from
// `image.size()`, exactly as UndoPageAppend does, so the two can never
// disagree on disk.
Status EncodeUndoRecordTail(std::span<std::byte> out, const UndoRecordFields& fields,
                            std::span<const std::byte> image);

// ---- Page operations -----------------------------------------------------

// Formats `page` as a brand-new, empty undo page, recording `first_trx_id`
// as the transaction whose append created it and chaining it behind
// `prev_page_id` (kInvalidPageId for the log's first page). Stamps the
// common header as kUndo. Neither field grants anyone exclusive use of the
// page: see the header struct.
Status FormatUndoPage(std::span<std::byte, kPageSize> page, std::uint64_t first_trx_id,
                      PageId prev_page_id);

UndoPageHeaderFields ReadUndoPageHeader(std::span<const std::byte, kPageSize> page);
void WriteUndoPageHeader(std::span<std::byte, kPageSize> page,
                         const UndoPageHeaderFields& header);

// Bytes still available for a record header plus its image.
std::uint16_t UndoPageFreeSpace(std::span<const std::byte, kPageSize> page);

// Appends one record to this page alone and returns the **absolute page
// offset** it landed at, which is what EncodeUndoPtr() takes. Fails with
// OutOfSpace if the page has no room - the signal the log uses to chain a
// new page, exactly as PageView::InsertTuple's is - and InvalidArgument for
// an image longer than kMaxUndoImageLen or a trx_id above 48 bits.
//
// `fields.image_len` is ignored: it is set from `image.size()`, so the two
// can never disagree on disk.
StatusOr<std::uint16_t> UndoPageAppend(std::span<std::byte, kPageSize> page,
                                        const UndoRecordFields& fields,
                                        std::span<const std::byte> image);

// Writes a record at a **named** offset, for redo. Redo must reproduce the
// exact offset the tuple's undo_ptr already carries, so replay names the
// position rather than appending wherever the page happens to end - the
// same reason VARHEAP_APPEND records their slot. Advances `lower` and
// `nr_records` only when the write extends the page, so replaying a record
// twice is idempotent.
Status UndoPageWriteAt(std::span<std::byte, kPageSize> page, std::uint16_t offset,
                       const UndoRecordFields& fields, std::span<const std::byte> image);

struct DecodedUndoRecord {
    UndoRecordFields fields;
    // A view into the page buffer itself - valid only while those bytes
    // stay alive and untouched. A caller stepping back a version copies it,
    // because the next step fetches another page.
    std::span<const std::byte> image;
};

// Reads the record at `offset`. Fails with Corruption for an offset outside
// the record area, a record whose extent runs past the page, or a type
// beyond kInsert - all of which mean a damaged chain rather than a miss.
// Reads one back. `prior_trx_id` and `prior_undo_ptr` come from the WAL
// payload's own fields and are left zero here - the caller fills them,
// because they are not in these bytes. Fails with Corruption for a tail
// shorter than the header or one whose image_len runs past it.
StatusOr<DecodedUndoRecord> DecodeUndoRecordTail(std::span<const std::byte> tail);

StatusOr<DecodedUndoRecord> UndoPageRead(std::span<const std::byte, kPageSize> page,
                                          std::uint16_t offset);

}  // namespace kds::txn
