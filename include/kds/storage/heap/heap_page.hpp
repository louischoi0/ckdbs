#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"

// Semi-sorted heap page (KDS-DESIGN.md section 3). Layout within one
// kPageSize buffer:
//
//   [ HeapPageHeaderFields ]  <- offset 0, kHeaderSize bytes; carries the
//                                page's immutable min_key
//   [ slot directory        ]  <- grows downward from kHeaderSize; header
//                                 .lower tracks the offset just past the
//                                 last slot
//   [ ... free space ...    ]
//   [ tuple data            ]  <- grows upward from the tail reservation;
//                                 header.upper tracks the offset of the
//                                 start of the last tuple written
//   [ next_page_id          ]  <- last sizeof(PageId) bytes of the page,
//                                 permanently reserved (see kNextPageIdOffset)
//
// header.lower/upper are absolute byte offsets from the start of the page.
// The page is full when upper - lower is smaller than the next slot +
// tuple need. This mirrors the legacy kernel heap.c's slotted-page design
// (PostgreSQL-style): slots are stable references, and physical compaction
// of dead slots is a separate, not-yet-implemented operation (same as the
// legacy code - no VACUUM-equivalent exists yet).
//
// Concurrency: PageView has no internal synchronization. It is a thin view
// over caller-owned bytes; the caller (eventually a buffer-pool frame) is
// responsible for ensuring exclusive access to the underlying buffer
// across any mutating call. There is no locking here because there is no
// buffer pool yet to hold a lock in - that's a separate, not-yet-built
// subsystem.
//
// This first cut covers: page init, insert, read, retire-slot,
// in-place overwrite, free-space accounting, and the next-page link.
// xmax-stamping delete is follow-up work once a transaction manager
// exists to give xmin/xmax/undo_ptr meaning - this file stores and
// retrieves them but does not interpret them, same division of
// responsibility as the legacy heap.c.

namespace kds::heap {

// ---- Page header -----------------------------------------------------

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

inline constexpr std::uint8_t kSlotFlagDead = 0x1;

// ---- Tuple (MVCC) header ------------------------------------------------

struct TupleHeaderFields {
    std::uint64_t xmin;
    std::uint64_t xmax;
    std::uint64_t undo_ptr;
    std::uint16_t data_len;
    std::uint8_t flags;
    std::uint8_t reserved;
};

inline constexpr std::size_t kTupleXminOffset = 0;
inline constexpr std::size_t kTupleXmaxOffset = 8;
inline constexpr std::size_t kTupleUndoPtrOffset = 16;
inline constexpr std::size_t kTupleDataLenOffset = 24;
inline constexpr std::size_t kTupleFlagsOffset = 26;
inline constexpr std::size_t kTupleReservedOffset = 27;
// 8+8+8+2+1+1 = 28 bytes actually read/written on disk. sizeof() rounds
// up to 32 for 8-byte tail alignment; never asserted against, since that
// padding is never touched (fields are memcpy'd individually, not the
// struct as a whole).
inline constexpr std::size_t kTupleHeaderOnDiskSize = 28;

static_assert(offsetof(TupleHeaderFields, xmin) == kTupleXminOffset);
static_assert(offsetof(TupleHeaderFields, xmax) == kTupleXmaxOffset);
static_assert(offsetof(TupleHeaderFields, undo_ptr) == kTupleUndoPtrOffset);
static_assert(offsetof(TupleHeaderFields, data_len) == kTupleDataLenOffset);
static_assert(offsetof(TupleHeaderFields, flags) == kTupleFlagsOffset);
static_assert(offsetof(TupleHeaderFields, reserved) == kTupleReservedOffset);

// ---- Tail next_page_id reservation --------------------------------------

inline constexpr std::size_t kNextPageIdOffset = kPageSize - sizeof(PageId);

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
    // the Keystone column's 40-bit id space (KDS-DESIGN.md invariant 7).
    static StatusOr<PageView> CreateEmpty(std::span<std::byte, kPageSize> page,
                                           std::uint64_t min_key);

    std::uint64_t min_key() const;
    std::uint16_t slot_count() const;
    std::uint16_t lower() const;
    std::uint16_t upper() const;
    std::uint16_t free_space() const;
    bool HasSpaceFor(std::uint16_t payload_len) const;

    PageId next_page_id() const;
    void set_next_page_id(PageId next);

    // A tuple read back out of the page. `payload` is a view into the
    // page buffer itself - valid only as long as the owning PageView (and
    // its underlying bytes) stay alive and untouched.
    struct Tuple {
        std::uint64_t xmin;
        std::uint64_t xmax;
        std::uint64_t undo_ptr;
        std::span<const std::byte> payload;
    };

    // Inserts a new tuple with the given payload, stamping xmin/xmax/
    // undo_ptr into its header. On success, returns the new slot index.
    // Fails with OutOfSpace if the page has no room, InvalidArgument if
    // payload.size() cannot fit a uint16_t length field.
    StatusOr<std::uint16_t> InsertTuple(std::span<const std::byte> payload, std::uint64_t xmin,
                                         std::uint64_t xmax = 0, std::uint64_t undo_ptr = 0);

    // Reads the tuple at `slot`. Fails with NotFound if `slot` is out of
    // range or dead.
    StatusOr<Tuple> ReadTuple(std::uint16_t slot) const;

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
                          std::uint64_t xmin, std::uint64_t xmax, std::uint64_t undo_ptr);

    // Marks `slot` dead; its bytes are not reclaimed (no page compaction -
    // an open item, matches the legacy implementation's stance until a
    // transaction manager can determine no snapshot still needs the
    // space). Fails with NotFound if `slot` is already out of range/dead.
    Status RetireSlot(std::uint16_t slot);

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
