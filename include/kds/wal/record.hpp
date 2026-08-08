#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"

// WAL record and segment codec (docs/wal.md sections 4.1 and 4.2). This
// file is format only: it turns records into bytes and back, and knows
// nothing about rings, devices, or when anything is flushed.
//
// Encoding rules are page_header.hpp's, for the same reason (rules.md
// sections 2 and 5): mirror struct, offsetof static_asserts, field-wise
// memcpy through named offsets, fixed-width little-endian, no bitfields.
//
// Two things make a stream self-delimiting, and both live here:
//
//   - Every record carries its own `total_len` and a CRC32C over
//     everything after the CRC field. Recovery walks forward and stops at
//     the first record whose length is impossible or whose CRC fails -
//     that is the durable end of the stream, not an error (section 4.2).
//     No commit marker is needed to find the tail.
//   - Records never span segments. A record that does not fit pads the
//     tail with PAD and seals the segment.
//
// An LSN is a stream-local byte offset (section 3), so it is also the
// position to read a record back from. Offset 0 is the segment header,
// which is why a valid record LSN is never 0 - and why page_lsn 0 keeps
// meaning "never logged" (page_header.hpp).

namespace kds::wal {

using Lsn = std::uint64_t;

// A page that has never been touched by the log carries this, matching
// storage::kNoPageLsn.
inline constexpr Lsn kNoLsn = 0;

// ---- Record types --------------------------------------------------------

// Frozen and append-only, like PageType: values are persisted, so an
// existing value's meaning never changes and a retired number is never
// reused. Adding one is a format-version event; an unknown type during
// replay is a hard error, never skipped (wal.md section 5.2).
enum class RecordType : std::uint8_t {
    kInvalid = 0,
    kTxnBegin = 1,
    kTxnCommit = 2,
    kTxnAbort = 3,
    kPageInit = 4,         // new heap page: common header + min_key
    kHeapInsert = 5,       // slot, tuple bytes, writer trx_id, undo_ptr
    kHeapOverwrite = 6,    // in-place new version
    kHeapDeleteMark = 7,   // the DELETE of wal.md section 5.1
    kSlotRetire = 8,       // physical retirement, distinct from delete-mark
    kAlloc = 9,            // SpaceManager allocation, precedes file extension
    kFullPageImage = 10,   // torn-page healing (section 10)
    kCheckpointBegin = 11,
    kCheckpointEnd = 12,
    kPad = 13,             // segment tail filler; carries no payload meaning
    kUndoWrite = 14,       // undo-page append: before-image + the chain link
    kFree = 15,            // SpaceManager release, the counterpart of kAlloc
    // var-heap append: a spilled value's bytes and the slot they landed in
    // (docs/rule-fixed-length-tuple.md section 5). Logged because a
    // var-heap value is *authoritative data* - losing one loses a committed
    // value, not a hint - which is what separates this page class from the
    // advisory waystone family.
    kVarHeapAppend = 16,
    // A secondary-index entry appended to a leaf: the slot it landed in and
    // its bytes (docs/feat-index.md §12.1). Logged because an index is
    // maintained on every write and a missing entry is a **lost row**, not a
    // lost hint - the same argument that makes the var-heap authoritative.
    //
    // Emitted **before** the HEAP_INSERT or HEAP_OVERWRITE it points at, and
    // the direction is forced rather than stylistic: if the index record is
    // durable and the row's is not, redo produces a dangling entry, which
    // verification drops on sight; the reverse produces a row no probe can
    // find. Same reasoning that puts VARHEAP_APPEND first, reached from the
    // opposite pointer direction.
    //
    // **Only for an append that split nothing.** A split's pages take full
    // page images, and those images are taken after the entry is in - so
    // emitting this as well would apply it twice.
    kIndexInsert = 17,
    // BTREE_INSERT/BTREE_SPLIT (wal.md section 5.2) are not assigned yet:
    // there is no B+ tree page format to describe, and a number reserved
    // for a payload nobody can encode is a number that gets used wrong.
    // Appending them later is exactly what this enum's append-only rule is
    // for.
    //
    // **INDEX_PAGE_INIT is not assigned either, and spec §12.1 proposed it.**
    // The proposal assumed a new index page could be described by its header
    // the way a new heap page is, with the following record filling it. A
    // dividing split does not work that way: the new sibling leaves the
    // operation holding half the entries, so only a full page image
    // describes it - which is exactly what the clustered tree's internal
    // nodes already do. A record type nothing can write is worse than none.
};

inline constexpr std::uint8_t kMaxAssignedRecordType = 17;

bool IsAssignedRecordType(std::uint8_t raw) noexcept;
const char* RecordTypeName(RecordType type) noexcept;

// ---- Record header -------------------------------------------------------

struct RecordHeaderFields {
    std::uint32_t total_len;  // header + payload + padding to 8 bytes
    std::uint32_t crc32c;     // over bytes [8, total_len)
    Lsn lsn;                  // this record's stream offset
    std::uint64_t txn_id;     // 48-bit, zero-extended; 0 = non-transactional
    std::uint8_t type;        // RecordType
    std::uint8_t flags;       // per-type
    std::uint16_t reserved;   // 0
    PageId page_id;           // target page, kInvalidPageId where N/A
};

inline constexpr std::size_t kRecordTotalLenOffset = 0;
inline constexpr std::size_t kRecordCrcOffset = 4;
inline constexpr std::size_t kRecordLsnOffset = 8;
inline constexpr std::size_t kRecordTxnIdOffset = 16;
inline constexpr std::size_t kRecordTypeOffset = 24;
inline constexpr std::size_t kRecordFlagsOffset = 25;
inline constexpr std::size_t kRecordReservedOffset = 26;
inline constexpr std::size_t kRecordPageIdOffset = 28;
// 4+4+8+8+1+1+2+4 = 32, every field naturally aligned, no tail padding.
inline constexpr std::size_t kRecordHeaderSize = 32;

// Where the CRC starts covering: everything from the LSN onward, so the
// length and the checksum itself are outside their own protection. A
// wrong `total_len` is caught by the length checks instead.
inline constexpr std::size_t kRecordCrcCoverageOffset = 8;

static_assert(offsetof(RecordHeaderFields, total_len) == kRecordTotalLenOffset);
static_assert(offsetof(RecordHeaderFields, crc32c) == kRecordCrcOffset);
static_assert(offsetof(RecordHeaderFields, lsn) == kRecordLsnOffset);
static_assert(offsetof(RecordHeaderFields, txn_id) == kRecordTxnIdOffset);
static_assert(offsetof(RecordHeaderFields, type) == kRecordTypeOffset);
static_assert(offsetof(RecordHeaderFields, flags) == kRecordFlagsOffset);
static_assert(offsetof(RecordHeaderFields, reserved) == kRecordReservedOffset);
static_assert(offsetof(RecordHeaderFields, page_id) == kRecordPageIdOffset);
static_assert(sizeof(RecordHeaderFields) == kRecordHeaderSize);

// Records are 8-byte aligned, so every LSN is too.
inline constexpr std::size_t kRecordAlignment = 8;

// Widest transaction id a record can name (wal.md section 4.2 / 5.1),
// matching heap::kMaxTrxId.
inline constexpr std::uint64_t kMaxTxnId = (std::uint64_t{1} << 48) - 1;

// Non-transactional records (checkpoint, pad) use this in `txn_id`.
inline constexpr std::uint64_t kNoTxnId = 0;

// ---- Encoding ------------------------------------------------------------

// What a caller supplies; `lsn`, `total_len`, and `crc32c` are the codec's
// to compute or take as a parameter.
struct RecordSpec {
    RecordType type = RecordType::kInvalid;
    std::uint64_t txn_id = kNoTxnId;
    PageId page_id = kInvalidPageId;
    std::uint8_t flags = 0;
};

// Bytes a record with this payload occupies, including padding. Callers
// size buffers and check segment fit with this.
std::size_t EncodedRecordSize(std::size_t payload_size) noexcept;

// Writes one record at the front of `out`, stamped with `lsn`, and returns
// how many bytes it used. Padding bytes are zeroed so an encoded record is
// a pure function of its inputs (the deterministic simulator compares
// streams byte for byte). Fails with InvalidArgument for an unassigned
// type, a txn_id above kMaxTxnId, or an `out` too small.
StatusOr<std::size_t> EncodeRecord(std::span<std::byte> out, const RecordSpec& spec, Lsn lsn,
                                   std::span<const std::byte> payload);

// ---- Decoding ------------------------------------------------------------

struct DecodedRecord {
    RecordHeaderFields header;
    std::span<const std::byte> payload;  // view into the caller's buffer

    RecordType type() const noexcept { return static_cast<RecordType>(header.type); }
};

// Reads the record at the front of `in`. Fails with Corruption for a
// length that cannot be right (too small, unaligned, past the buffer) or a
// CRC mismatch - both of which recovery reads as "the stream ends here",
// and neither of which is distinguishable from the other in a torn tail.
StatusOr<DecodedRecord> DecodeRecord(std::span<const std::byte> in);

// Walks the records in a buffer that starts at `base_lsn`. Next() returns
// nullopt at the durable end: no more bytes, or a record that does not
// decode. `stopped_early()` distinguishes a clean end from a torn tail for
// callers that want to meter it; neither is an error.
class RecordReader {
public:
    RecordReader(std::span<const std::byte> buffer, Lsn base_lsn) noexcept
        : buffer_(buffer), base_lsn_(base_lsn) {}

    std::optional<DecodedRecord> Next();

    // Stream offset just past the last successfully decoded record - the
    // durable end of the stream once Next() has returned nullopt.
    Lsn end_lsn() const noexcept { return base_lsn_ + cursor_; }
    bool stopped_early() const noexcept { return stopped_early_; }

private:
    std::span<const std::byte> buffer_;
    Lsn base_lsn_;
    std::size_t cursor_ = 0;
    bool stopped_early_ = false;
};

// ---- Segment header ------------------------------------------------------

struct SegmentHeaderFields {
    std::uint64_t magic;
    std::uint32_t format_version;
    std::uint32_t core_id;
    std::uint64_t segment_no;
    Lsn start_lsn;         // stream offset of this segment's first byte
    std::uint32_t crc32c;  // over bytes [0, kSegmentCrcOffset)
    std::uint32_t reserved;
};

inline constexpr std::size_t kSegmentMagicOffset = 0;
inline constexpr std::size_t kSegmentFormatVersionOffset = 8;
inline constexpr std::size_t kSegmentCoreIdOffset = 12;
inline constexpr std::size_t kSegmentNoOffset = 16;
inline constexpr std::size_t kSegmentStartLsnOffset = 24;
inline constexpr std::size_t kSegmentCrcOffset = 32;
inline constexpr std::size_t kSegmentReservedOffset = 36;
inline constexpr std::size_t kSegmentUsedSize = 40;

static_assert(offsetof(SegmentHeaderFields, magic) == kSegmentMagicOffset);
static_assert(offsetof(SegmentHeaderFields, format_version) == kSegmentFormatVersionOffset);
static_assert(offsetof(SegmentHeaderFields, core_id) == kSegmentCoreIdOffset);
static_assert(offsetof(SegmentHeaderFields, segment_no) == kSegmentNoOffset);
static_assert(offsetof(SegmentHeaderFields, start_lsn) == kSegmentStartLsnOffset);
static_assert(offsetof(SegmentHeaderFields, crc32c) == kSegmentCrcOffset);

// One 4 KiB block, so the first record starts at a device-friendly
// boundary and no record ever shares a block with the header.
inline constexpr std::size_t kSegmentHeaderSize = 4096;
static_assert(kSegmentUsedSize <= kSegmentHeaderSize);

inline constexpr std::uint64_t kSegmentMagic = 0x314C41575344584BULL;  // "KXDSWAL1"
inline constexpr std::uint32_t kSegmentFormatVersion = 1;

// Zeroes the block and writes the header into it. `out` must be at least
// kSegmentHeaderSize bytes.
Status EncodeSegmentHeader(std::span<std::byte> out, const SegmentHeaderFields& fields);

// Fails with Corruption on a bad magic, a CRC mismatch, or a
// format_version this build does not know - a newer segment is refused
// rather than misparsed.
StatusOr<SegmentHeaderFields> DecodeSegmentHeader(std::span<const std::byte> in);

}  // namespace kds::wal
