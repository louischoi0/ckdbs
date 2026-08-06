#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/page_header.hpp"

// Undo page layout and the undo record codec (docs/txn.md section 3).
//
// An undo page holds the before-images that let a reader reconstruct the
// version of a tuple its snapshot is entitled to see, and that let an
// aborting transaction put the bytes back. It closes docs/wal.md section
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
    // O(1) "is this page empty", for a purge pass that does not exist yet.
    // Derivable by counting records, which is exactly why it is stored: a
    // purge sweep that had to walk every record to learn a page holds none
    // would be walking the pages it is trying to skip.
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
    // transaction, which sharing makes unanswerable. It chains the pages in
    // creation order so a future purge pass can walk them without a side
    // table; that pass will need a per-page high-water mark this header does
    // not carry yet, and reserved1 is where it goes.
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
    // Image empty. **Defined and never written** (txn.md section 3.6): a
    // tuple with undo_ptr == kNoUndoPtr whose writer is invisible already
    // means "no visible version", so an insert needs no undo record and
    // rollback of one reads the transaction's in-memory trail instead. It
    // is defined now so that persisting that trail - which recovery-driven
    // rollback will need - is a code change and not a format-version event.
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
};

inline constexpr std::size_t kUndoRecPriorTrxIdOffset = 0;
inline constexpr std::size_t kUndoRecPriorUndoPtrOffset = 8;
inline constexpr std::size_t kUndoRecTargetPageIdOffset = 16;
inline constexpr std::size_t kUndoRecTargetSlotOffset = 20;
inline constexpr std::size_t kUndoRecImageLenOffset = 22;
inline constexpr std::size_t kUndoRecTypeOffset = 24;
inline constexpr std::size_t kUndoRecFlagsOffset = 25;
inline constexpr std::size_t kUndoRecReservedOffset = 26;
// 8+8+4+2+2+1+1+2 = 28; image bytes begin here.
//
// Records are **unpadded**. Every access is a field-wise memcpy (rules.md
// section 2), so alignment buys nothing, and 8-byte padding would waste up
// to 7 bytes on a page holding ~290 records. `lower` advances by exactly
// kUndoRecordHeaderSize + image_len.
inline constexpr std::size_t kUndoRecordHeaderSize = 28;

static_assert(offsetof(UndoRecordFields, prior_trx_id) == kUndoRecPriorTrxIdOffset);
static_assert(offsetof(UndoRecordFields, prior_undo_ptr) == kUndoRecPriorUndoPtrOffset);
static_assert(offsetof(UndoRecordFields, target_page_id) == kUndoRecTargetPageIdOffset);
static_assert(offsetof(UndoRecordFields, target_slot) == kUndoRecTargetSlotOffset);
static_assert(offsetof(UndoRecordFields, image_len) == kUndoRecImageLenOffset);
static_assert(offsetof(UndoRecordFields, type) == kUndoRecTypeOffset);
static_assert(offsetof(UndoRecordFields, flags) == kUndoRecFlagsOffset);
static_assert(offsetof(UndoRecordFields, reserved) == kUndoRecReservedOffset);
// sizeof(UndoRecordFields) is 32, not 28: the u64 members give the struct
// 8-byte alignment, so its size rounds up. Deliberately not asserted
// against - those four tail bytes are never touched, because fields are
// memcpy'd one at a time through the offsets above and never as a struct.
// TupleHeaderFields carries the same note for the same reason.

// The largest before-image one record can carry: a whole empty page's
// record space, less the one record header it needs.
//
// **Known ceiling, recorded rather than hidden** (txn.md section 3.3): undo
// overhead is 32 + 24 + 28 = 84 bytes against the heap page's
// 32 + 16 + 5 + 20 + 4 = 77, so a tuple within ~7 bytes of the maximum heap
// payload cannot be updated - the undo append fails OutOfSpace naming the
// *undo* page. The deferred fix is a spilling image or a long-image record
// type; neither is invented here to answer a question nobody has hit.
inline constexpr std::size_t kMaxUndoImageLen = kUndoPageCapacity - kUndoRecordHeaderSize;

static_assert(kMaxUndoImageLen == 8108);

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
StatusOr<DecodedUndoRecord> UndoPageRead(std::span<const std::byte, kPageSize> page,
                                          std::uint16_t offset);

}  // namespace kds::txn
