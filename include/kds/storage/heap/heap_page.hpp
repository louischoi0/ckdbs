#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/page_header.hpp"

// Semi-sorted heap page (docs/spec/heap-and-tuple.md section 3). Layout within one
// kPageSize buffer:
//
//   [ common page header    ]  <- offset 0, 32 bytes: page_type, checksum,
//                                 page_lsn (page.md section 2). Owned by
//                                 page_header.hpp, not by this file.
//   [ HeapPageHeaderFields ]  <- offset kHeapHeaderOffset (32),
//                                kHeaderSize bytes; carries the page's
//                                immutable min_key
//   [ slot directory        ]  <- grows downward from the heap header;
//                                 header.lower tracks the offset just past
//                                 the last slot
//   [ ... free space ...    ]
//   [ tuple data            ]  <- grows upward from the tail reservation;
//                                 header.upper tracks the offset of the
//                                 start of the last tuple written
//   [ next_page_id          ]  <- last sizeof(PageId) bytes of the page,
//                                 permanently reserved (see kNextPageIdOffset)
//
// header.lower/upper are absolute byte offsets from the start of the page,
// so they already account for the common header sitting below the heap
// header. The heap page's own fields are placed *above* that header rather
// than at offset 0 because redo needs page_lsn at a fixed page-wide
// location (wal.md section 9) and the load path needs the checksum there
// (page.md section 10); a heap page is one of the headered page classes.
// The page is full when upper - lower is smaller than the next slot +
// tuple need. A PostgreSQL-style slotted page: slots are stable
// references, and physical compaction of dead slots is a separate
// operation that does not exist yet - there is no VACUUM equivalent.
//
// Concurrency: PageView has no internal synchronization. It is a thin view
// over caller-owned bytes; the caller (eventually a buffer-pool frame) is
// responsible for ensuring exclusive access to the underlying buffer
// across any mutating call. There is no locking here because there is no
// buffer pool yet to hold a lock in - that's a separate, not-yet-built
// subsystem.
//
// This first cut covers: page init, insert, read, delete-mark,
// retire-slot, in-place overwrite, free-space accounting, and the
// next-page link. This file stores trx_id/undo_ptr but does not interpret
// them - visibility is the reader's job once snapshots exist.

namespace kds::heap {

// ---- Page header -----------------------------------------------------

// Where the heap's own header begins: immediately after the common one.
// Every offset constant below is relative to *this*, not to the page.
inline constexpr std::size_t kHeapHeaderOffset = storage::kPageBodyOffset;

struct HeapPageHeaderFields {
    std::uint16_t flags;
    std::uint16_t nr_slots;
    std::uint16_t lower;
    std::uint16_t upper;
    std::uint64_t min_key;
};

inline constexpr std::size_t kHeaderFlagsOffset = 0;
inline constexpr std::size_t kHeaderNrSlotsOffset = 2;
inline constexpr std::size_t kHeaderLowerOffset = 4;
inline constexpr std::size_t kHeaderUpperOffset = 6;
inline constexpr std::size_t kHeaderMinKeyOffset = 8;
// Sum of the fields above; HeapPageHeaderFields has no tail padding at
// this size (8-byte-aligned uint64_t after two 8-byte-aligned uint16 pairs)
// so sizeof() is safe to assert directly, unlike TupleHeaderFields below.
inline constexpr std::size_t kHeaderSize = 16;

static_assert(offsetof(HeapPageHeaderFields, flags) == kHeaderFlagsOffset);
static_assert(offsetof(HeapPageHeaderFields, nr_slots) == kHeaderNrSlotsOffset);
static_assert(offsetof(HeapPageHeaderFields, lower) == kHeaderLowerOffset);
static_assert(offsetof(HeapPageHeaderFields, upper) == kHeaderUpperOffset);
static_assert(offsetof(HeapPageHeaderFields, min_key) == kHeaderMinKeyOffset);
static_assert(sizeof(HeapPageHeaderFields) == kHeaderSize);

inline constexpr std::uint16_t kHeaderFlagInitialized = 0x1;

// ---- Slot directory entry ---------------------------------------------

struct HeapSlotFields {
    std::uint16_t offset;  // absolute page offset of the tuple, 0 if dead
    std::uint16_t length;  // total tuple size (header + payload), 0 if dead
    std::uint8_t flags;
};

inline constexpr std::size_t kSlotOffsetOffset = 0;
inline constexpr std::size_t kSlotLengthOffset = 2;
inline constexpr std::size_t kSlotFlagsOffset = 4;
// 2+2+1 = 5 bytes actually read/written on disk. sizeof(HeapSlotFields)
// rounds up to 6 (uint16 alignment requires the struct's own size be a
// multiple of 2); that trailing byte is never touched, so it does not
// belong in the on-disk size constant.
inline constexpr std::size_t kSlotOnDiskSize = 5;

static_assert(offsetof(HeapSlotFields, offset) == kSlotOffsetOffset);
static_assert(offsetof(HeapSlotFields, length) == kSlotLengthOffset);
static_assert(offsetof(HeapSlotFields, flags) == kSlotFlagsOffset);

// Physically retired: the bytes are no longer reachable and the space is
// reclaimable by a future compaction. Logged as SLOT_RETIRE (wal.md 5.2).
inline constexpr std::uint8_t kSlotFlagDead = 0x1;

// Logically deleted, bytes still present: the row is gone for readers whose
// snapshot is after the deleting transaction, and still visible to older
// ones. The deleter's id is in the tuple header's trx_id - that is the
// whole of DELETE in this model (wal.md 5.1), and it is why there is no
// xmax field. Logged as HEAP_DELETE_MARK, which is a different record from
// SLOT_RETIRE above.
inline constexpr std::uint8_t kSlotFlagDeleted = 0x2;

// ---- Tuple (MVCC) header ------------------------------------------------
//
// `trx_id` + `undo_ptr`, no xmax (wal.md 5.1): a
// version's death is the next version's birth, so walking the undo chain
// already tells a reader which transaction overwrote a version. Storing
// that boundary a second time in the older version would be redundant, and
// the lock-slot role xmax plays in Postgres belongs to the Keystone lock
// byte here (heap-and-tuple.md section 4).
//
// `trx_id` is the *writer* - whichever transaction last stamped this
// version, whether by insert, overwrite, or delete-mark. It is 48-bit
// (wal.md 4.2), stored zero-extended in 8 bytes with the upper 16 bits 0,
// the same convention ids follow outside the tuple header (invariant 6).

struct TupleHeaderFields {
    std::uint64_t trx_id;
    std::uint64_t undo_ptr;
    std::uint16_t data_len;
    std::uint8_t flags;
    std::uint8_t reserved;
};

inline constexpr std::size_t kTupleTrxIdOffset = 0;
inline constexpr std::size_t kTupleUndoPtrOffset = 8;
inline constexpr std::size_t kTupleDataLenOffset = 16;
inline constexpr std::size_t kTupleFlagsOffset = 18;
inline constexpr std::size_t kTupleReservedOffset = 19;
// 8+8+2+1+1 = 20 bytes actually read/written on disk. sizeof() rounds up
// to 24 for 8-byte tail alignment; never asserted against, since that
// padding is never touched (fields are memcpy'd individually, not the
// struct as a whole).
inline constexpr std::size_t kTupleHeaderOnDiskSize = 20;

static_assert(offsetof(TupleHeaderFields, trx_id) == kTupleTrxIdOffset);
static_assert(offsetof(TupleHeaderFields, undo_ptr) == kTupleUndoPtrOffset);
static_assert(offsetof(TupleHeaderFields, data_len) == kTupleDataLenOffset);
static_assert(offsetof(TupleHeaderFields, flags) == kTupleFlagsOffset);
static_assert(offsetof(TupleHeaderFields, reserved) == kTupleReservedOffset);

// Widest transaction id the header can hold (wal.md 4.2). Writers assert
// against it rather than silently truncating.
inline constexpr std::uint64_t kMaxTrxId = (std::uint64_t{1} << 48) - 1;

// ---- Tail next_page_id reservation --------------------------------------

inline constexpr std::size_t kNextPageIdOffset = kPageSize - sizeof(PageId);

// ---- Page capacity -------------------------------------------------------

// The largest payload a *freshly created* page can take: everything between
// an empty page's lower (the first slot's own offset) and its upper (the
// next_page_id reservation), less the one slot and one tuple header that
// payload would need. Derived rather than written down, so it moves with
// the layout constants above instead of drifting from them.
//
// It exists because invariant 13 makes a row's size a schema constant: a
// relation whose constant exceeds this is one no page can ever hold a row
// of, which catalog::RowLayout::Build() refuses at CREATE TABLE rather than
// letting the first INSERT discover.
inline constexpr std::size_t kMaxTuplePayloadSize =
    kNextPageIdOffset - (kHeapHeaderOffset + kHeaderSize) - kSlotOnDiskSize -
    kTupleHeaderOnDiskSize;

static_assert(kMaxTuplePayloadSize == 8115);

// ---- PageView ------------------------------------------------------------

class PageView {
public:
    // Wraps already-initialized heap page bytes (e.g. one just loaded off
    // disk). Does not validate or modify the buffer; use CreateEmpty() to
    // format a brand-new page. Never fails (rules.md #1: constructors must
    // not fail) because it does no validation - callers that need to know
    // the buffer is actually a well-formed heap page should check
    // header flags/invariants themselves before trusting it.
    explicit PageView(std::span<std::byte, kPageSize> page) noexcept : page_(page) {}

    // Formats `page` as a brand-new, empty heap page whose min_key is
    // fixed for the rest of the page's lifetime - no method below ever
    // writes header.min_key again. Fails only if `min_key` does not fit
    // the Keystone column's 40-bit id space (docs/spec/heap-and-tuple.md invariant 7).
    // `owner_oid` (page.md §2a) is stamped with the same immutability:
    // the owning relation for a user page, 0 for system callers (catalog).
    static StatusOr<PageView> CreateEmpty(std::span<std::byte, kPageSize> page,
                                           std::uint64_t min_key,
                                           std::uint64_t owner_oid = 0);

    // CreateEmpty() for a page that stamps a different `page_type` in the
    // common header while keeping this exact body layout. The one caller
    // is the B+ tree, whose **leaves are heap pages** (btree.hpp): a leaf
    // holds tuples in the same slot directory, with the same MVCC tuple
    // header, and is walked by the same next_page_id link - the only
    // differences are the header byte that says so and what `min_key` and
    // the link *mean* (the leaf's low key, and its right sibling).
    //
    // Reusing the layout rather than defining a second slotted page is the
    // point: InsertTuple/ReadTuple/OverwriteTuple/DeleteMark, the row
    // codec and HEAP_INSERT redo all work on a leaf unchanged. `type` must be kHeap or kBtreeLeaf;
    // anything else is InvalidArgument, because nothing else has this body.
    static StatusOr<PageView> CreateEmptyAs(std::span<std::byte, kPageSize> page,
                                             std::uint64_t min_key, PageType type,
                                             std::uint64_t owner_oid = 0);

    std::uint64_t min_key() const;
    std::uint16_t slot_count() const;
    std::uint16_t lower() const;
    std::uint16_t upper() const;
    std::uint16_t free_space() const;
    bool HasSpaceFor(std::uint16_t payload_len) const;

    PageId next_page_id() const;
    void set_next_page_id(PageId next);

    // The common header's relayout epoch (docs/spec/physical-optimizer.md
    // R4), surfaced here because the callers that need it at tuple-access
    // time hold a PageView, not the raw span. Read-only on purpose: bumping
    // is the mover's act, through storage::BumpRelayoutEpoch on the span it
    // holds exclusively.
    std::uint64_t RelayoutEpoch() const noexcept { return storage::GetRelayoutEpoch(page_); }

    // A tuple read back out of the page. `payload` is a view into the
    // page buffer itself - valid only as long as the owning PageView (and
    // its underlying bytes) stay alive and untouched.
    struct Tuple {
        std::uint64_t trx_id;
        std::uint64_t undo_ptr;
        std::span<const std::byte> payload;
        bool deleted;  // slot carries kSlotFlagDeleted; trx_id is the deleter
    };

    // Inserts a new tuple with the given payload, stamping trx_id/undo_ptr
    // into its header. On success, returns the new slot index. Fails with
    // OutOfSpace if the page has no room, InvalidArgument if payload.size()
    // cannot fit a uint16_t length field or trx_id exceeds kMaxTrxId.
    StatusOr<std::uint16_t> InsertTuple(std::span<const std::byte> payload, std::uint64_t trx_id,
                                         std::uint64_t undo_ptr = 0);

    // Reads the tuple at `slot`. A delete-marked tuple is returned, with
    // `deleted` set - whether the reader may see it depends on its snapshot
    // versus `trx_id`, which is not this layer's decision. Fails with
    // NotFound only if `slot` is out of range or physically retired.
    StatusOr<Tuple> ReadTuple(std::uint16_t slot) const;

    // Just the payload bytes of the tuple at `slot` - ReadTuple() without
    // the MVCC header decode, and without re-reading the page header.
    // `nr_slots` is the caller's already-read slot_count(): a key search
    // over a page reads the header once and passes it in, where ReadTuple()
    // re-reads all five header fields on every single call. That is the
    // point of this method - a search probe touches the slot directory and
    // the payload, nothing else.
    //
    // Deliberately returns bytes, not a decoded key: what the leading bytes
    // of a payload *mean* is the row encoding's business, and this layer
    // does not know that the Keystone column exists (see the layering note
    // at the top of heap_page.cpp). Callers searching by primary key pair
    // this with KeystoneIdOfPayload() from storage/keystone.hpp.
    //
    // Fails with NotFound on exactly the slots ReadTuple() does (out of
    // range, or physically retired), so a search can treat NotFound as "no
    // key here" and keep going. A delete-marked slot still has its bytes
    // and still returns them, for the same reason ReadTuple() still returns
    // the tuple: visibility is not this layer's decision.
    StatusOr<std::span<const std::byte>> PayloadAt(std::uint16_t slot,
                                                    std::uint16_t nr_slots) const;

    // Returns the payload capacity (slot.length - kTupleHeaderOnDiskSize)
    // reserved for the tuple at `slot` - the largest new payload
    // OverwriteTuple() could write there without relocating. Fails with
    // NotFound if the slot is out of range or dead.
    StatusOr<std::uint16_t> SlotCapacity(std::uint16_t slot) const;

    // Overwrites the tuple at `slot` in place (same physical offset) with
    // new header fields and new payload bytes - a HOT-style update: no
    // slot directory or free-space change, so the row's tid (page_id +
    // slot) is preserved, unlike RetireSlot()+InsertTuple(). Fails with
    // OutOfSpace if payload.size() exceeds SlotCapacity(slot) - callers
    // needing more room must fall back to RetireSlot() + InsertTuple().
    // Fails with NotFound if `slot` is out of range or dead.
    Status OverwriteTuple(std::uint16_t slot, std::span<const std::byte> payload,
                          std::uint64_t trx_id, std::uint64_t undo_ptr);

    // DELETE: marks `slot` deleted and stamps `trx_id` as the deleter,
    // leaving the tuple bytes in place for readers whose snapshot predates
    // it. This is the whole of delete in the no-xmax model (wal.md 5.1) -
    // physical reclamation is RetireSlot(), a separate operation a purge
    // pass performs once no snapshot needs the version. Fails with NotFound
    // if `slot` is out of range or retired, InvalidArgument if trx_id
    // exceeds kMaxTrxId. Delete-marking an already-marked slot re-stamps
    // the deleter rather than failing.
    Status DeleteMark(std::uint16_t slot, std::uint64_t trx_id);

    // The inverse of DeleteMark, for rollback (docs/spec/txn.md section 6):
    // clears the mark and restores the header the tuple carried before the
    // deleter stamped it. Both halves matter - a cleared mark with the
    // deleter still in `trx_id` would leave the row attributed to a
    // transaction that aborted.
    //
    // Deliberately not "undelete": nothing above the transaction manager
    // may call this. A delete-mark is undone by the transaction that made
    // it and by nothing else.
    Status ClearDeleteMark(std::uint16_t slot, std::uint64_t trx_id, std::uint64_t undo_ptr);

    // Marks `slot` dead; its bytes are not reclaimed (no page compaction -
    // an open item until a transaction manager can determine that no
    // snapshot still needs the space). Fails with NotFound if `slot` is already out of range/dead.
    Status RetireSlot(std::uint16_t slot);

    // Writes a tuple at a **named** slot, for redo
    // (docs/workplan-wal-recovery.md RC03). The counterpart of
    // txn::UndoPageWriteAt, and it exists for the same reason: replay must
    // reproduce the exact position the original write chose, because that
    // position is *durably referenced elsewhere* - an undo record names
    // `target_page_id` + `target_slot`, so a tuple that came back at a
    // different slot would leave every undo chain pointing at the wrong
    // row. InsertTuple() cannot be used for this: it allocates the next
    // slot and returns it, which is the right contract for a writer and
    // the wrong one for a replayer.
    //
    // Three cases, and only the first two are reachable from a correct
    // stream:
    //
    //   slot == nr_slots   append, exactly as InsertTuple does - the new
    //                      slot index is `slot` by construction;
    //   slot <  nr_slots   the slot exists, so this is a re-application:
    //                      overwrite in place. **This is what makes redo
    //                      idempotent** - replaying a record twice writes
    //                      identical bytes the second time and moves no
    //                      header field;
    //   slot >  nr_slots   a gap. Slots are allocated densely and in order,
    //                      so no writer could have produced this: Corruption.
    //
    // A dead slot is also Corruption rather than a resurrection: retirement
    // follows its insert in LSN order, so redo reaching a dead slot means
    // the page is not the page the record was written against.
    //
    // Fails with OutOfSpace if the payload does not fit the existing slot's
    // capacity (an append that does not fit fails the same way InsertTuple
    // does). Under invariant 13 a relation's rows are one size, so that is
    // a corruption signal in practice rather than a resize request.
    Status RedoWriteTuple(std::uint16_t slot, std::span<const std::byte> payload,
                          std::uint64_t trx_id, std::uint64_t undo_ptr);

    // Raw slot-directory contents for `slot_idx`, dead or alive - unlike
    // ReadTuple()/SlotCapacity(), a dead slot is reported (dead=true)
    // rather than treated as NotFound. For development/inspection tooling
    // only (e.g. the server's `SHOW PAGE` command); not part of the
    // transactional read path.
    struct SlotInfo {
        std::uint16_t offset;
        std::uint16_t length;
        std::uint8_t flags;
        bool dead;
    };
    StatusOr<SlotInfo> DebugSlotInfo(std::uint16_t slot_idx) const;

private:
    HeapPageHeaderFields ReadHeader() const;
    void WriteHeader(const HeapPageHeaderFields& header);
    HeapSlotFields ReadSlot(std::uint16_t idx) const;
    void WriteSlot(std::uint16_t idx, const HeapSlotFields& slot);

    std::span<std::byte, kPageSize> page_;
};

}  // namespace kds::heap
