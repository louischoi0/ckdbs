#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"

// WAL record and segment codec (docs/spec/wal.md sections 4.1 and 4.2). This
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
    // (docs/rules/rule-fixed-length-tuple.md section 5). Logged because a
    // var-heap value is *authoritative data* - losing one loses a committed
    // value, not a hint - which is what separates this page class from the
    // advisory waystone family.
    kVarHeapAppend = 16,
    // A secondary-index entry appended to a leaf: the slot it landed in and
    // its bytes (docs/spec/index.md §12.1). Logged because an index is
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
    // ---- The assertion records (docs/spec/assertion.md §7, AST05) --------
    //
    // A Bound Cabin is an authoritative constraint substrate, so its
    // maintenance is logged on the var-heap's and the index's argument:
    // losing an entry loses part of an admission check's answer, never a
    // hint. All five payloads carry their assertion's id, because one
    // relation may carry several assertions and the envelope's page_id
    // names a page, not an owner.
    //
    // One reservation admitted on the home core (§6.2 step 3): the entry
    // written into the envelope's page, plus the group key that attributes
    // it - which is what lets replay rebuild the memory-resident group
    // directory without re-reading any relation row. txn_id is the writer,
    // and the entry bytes carry kEntryReserved set.
    kAssertReserve = 18,
    // The reserved→committed flag transition, batched **per page** rather
    // than per transaction: a physiological record describes one page, so a
    // transaction holding reservations on N pages commits them with N of
    // these. Replay touches flags only - a reservation counts in the
    // aggregate from the moment of admission, so commit moves no sum.
    kAssertCommit = 19,
    // The compensation (§6.2 step 5, and recovery's answer to a crashed
    // in-flight reservation): the reserved delta subtracted and the entry
    // forgotten by the group directory. **The page entry is not rewritten**
    // - abort is a removal the directory performs (cabin_bound_page.hpp) -
    // so the slot is orphaned and its 32 bytes ride on purge like every
    // superseded value in this engine. txn_id is the aborting transaction,
    // SLOT_RETIRE's rollback rule.
    kAssertRollback = 20,
    // One entry materialized by the CREATE-time builder (AST06): the same
    // payload shape as ASSERT_RESERVE - HEAP_INSERT/HEAP_OVERWRITE's
    // precedent - but owned by no transaction and with kEntryReserved
    // clear, because §8.1a's build applies its deltas at commit time and
    // what the builder writes is committed by construction. Per entry, not
    // per batch: a batched sibling is an append-only addition the day the
    // build's log volume asks for one, and guessing its shape before a
    // builder exists is how a record nobody can write gets assigned.
    kAssertBuild = 21,
    // Teardown (DROP ASSERTION): the directory forgets the assertion. The
    // pages return through ordinary FREE records, not through this one.
    kAssertDrop = 22,
    // ---- Clearing a delete-mark (RC05, 2026-08-11) ----------------------
    //
    // Rollback's compensation for a DELETE, and it needs a type of its own
    // because **HEAP_DELETE_MARK cannot say which direction it meant**.
    //
    // This is a bug that shipped, found writing RC05. `redo.cpp`'s applier
    // asserted the opposite in a comment - "rollback's ClearDeleteMark is a
    // *different* record, so redo never has to guess which direction a mark
    // record meant" - and it was not: `TransactionManager::Compensate`
    // logged `kHeapDeleteMark` to *clear* a mark, with a payload
    // `{trx_id, slot}` identical in shape to the one that *sets* it. Redo
    // replays both by calling `DeleteMark`, so a transaction that
    // delete-marked a row and then aborted came back from recovery with the
    // row still deleted - the abort silently undone by its own
    // compensation.
    //
    // A distinct type rather than a flag in the payload: the two operations
    // are different operations, and a reader that has to consult a bit to
    // know whether a record adds or removes state is one substitution away
    // from the same defect.
    kHeapDeleteUnmark = 23,
    // One Bound Cabin's group headers as of a checkpoint (AS6a): the base
    // assertion replay folds onto, so the fold starts at the last checkpoint
    // rather than at the cabin's birth. Chunked, because a cabin's group count is
    // bounded by the data and a record must fit a segment; `payload.hpp` carries
    // the format and the reason no continuation flag is needed.
    kAssertSnapshot = 24,
    // BTREE_INSERT/BTREE_SPLIT (wal.md section 5.2) are not assigned yet:
    // there is no B+ tree page format to describe, and a number reserved
    // for a payload nobody can encode is a number that gets used wrong.
    // Appending them later is exactly what this enum's append-only rule is
    // for.
    //
    // The PL handoff (docs/spec/page-lsn-cross-stream.md §9 rule 1,
    // workplan-peer-writer.md PW1c-1): *this page left this stream at this
    // LSN*, appended by the outgoing owner after the page is flushed
    // durable and before the incoming owner is granted write rights. The
    // envelope's page_id names the page; the payload names the incoming
    // core; the handoff LSN is the record's own. It moves a **fact**,
    // never an ordering - no LSN is ever compared across streams.
    //
    // Redo applies nothing for it and must not even load the page (the
    // page belongs to another stream from this LSN on); analysis is its
    // real consumer. PW1c-2 (built): analysis *removes* the page from
    // this stream's dirty page table at the handoff, and redo's
    // not-dirty filter skips the page's earlier records without faulting
    // it. Nothing emits it until PW1c-4 wires the DDL-publish grant.
    kPageHandoff = 25,
    // One anchor-page slot update (storage/anchor_page.hpp; PW2,
    // workplan-peer-writer.md §7a): the clustered root (index_oid 0) or
    // one index's root moved. The record every growth-path catalog write
    // becomes - a root move writes the mover's own anchor page in its own
    // stream, never sys.tables/sys.indexes - and the second half of a
    // fresh anchor's durable story, since PAGE_INIT rebuilds only the
    // common header and the roots are body content. Physiological like
    // every page record: the envelope names the anchor page, the payload
    // the slot.
    kAnchorUpdate = 26,
    // **INDEX_PAGE_INIT is not assigned either, and spec §12.1 proposed it.**
    // The proposal assumed a new index page could be described by its header
    // the way a new heap page is, with the following record filling it. A
    // dividing split does not work that way: the new sibling leaves the
    // operation holding half the entries, so only a full page image
    // describes it - which is exactly what the clustered tree's internal
    // nodes already do. A record type nothing can write is worse than none.
};

// The highest assigned type, **derived from the enum rather than typed as a
// number** - and that is a bug fix, not tidiness.
//
// `EncodeRecord` refuses any type above this bound, so a hand-maintained value
// silently un-assigns whatever was appended after it. That is exactly what
// happened: RC05 added `kHeapDeleteUnmark = 23` and left the constant at 22,
// so **the record could not be written at all** - `TransactionManager::Compensate`
// and `txn::RecoveryUndo` both failed with "unassigned record type" when
// rolling back a DELETE, and every test that covers that path runs unlogged
// (`wal = nullptr`), which is what hid it until 2026-08-12.
//
// Keep this pinned to the last enumerator when appending a type; the test that
// every named type encodes is what proves it stayed pinned.
inline constexpr std::uint8_t kMaxAssignedRecordType =
    static_cast<std::uint8_t>(RecordType::kAnchorUpdate);

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
// **2 since 2026-08-12**, and the bump is the D2 decision `docs/inflight/known-gaps.md`
// carried: two record payloads moved under the licence "free today, no WAL
// stream has ever been read back", and `6d7b91b` - inside the same handful of
// commits - is what makes streams get read back.
//
// What moved: `AssertEntryPayload` gained `group_id` (AS6a), taking
// `kAssertEntryFixedSize` from 16 to 20 so every byte after offset 16 shifted;
// and RC03's `UNDO_WRITE` correction moved under the same argument. Version 1
// therefore names a stream this build cannot decode.
inline constexpr std::uint32_t kSegmentFormatVersion = 2;

// **The oldest stream this build will read**, and it is deliberately equal to
// the current version rather than 1.
//
// A version field alone does not refuse anything: `DecodeSegmentHeader` rejects
// only what is *newer* than this build, so bumping the version without this
// floor would leave a v1 segment accepted and its `ASSERT_*` records decoded at
// the wrong offsets - a short read at best, four bytes of someone else's field
// at worst. Refusing outright is what the bump is for.
//
// Raised with `kSegmentFormatVersion` while no compatibility promise exists
// (pre-1.0, and no v1 database is known). The day one does exist, this stops
// tracking the current version and a decoder per supported version is what
// takes its place - which is a decision to make then, with a migration story,
// not a default to inherit now.
inline constexpr std::uint32_t kMinReadableSegmentFormatVersion = 2;
static_assert(kMinReadableSegmentFormatVersion <= kSegmentFormatVersion);

// Zeroes the block and writes the header into it. `out` must be at least
// kSegmentHeaderSize bytes.
Status EncodeSegmentHeader(std::span<std::byte> out, const SegmentHeaderFields& fields);

// Fails with Corruption on a bad magic, a CRC mismatch, or a
// format_version this build does not know - a newer segment is refused
// rather than misparsed.
StatusOr<SegmentHeaderFields> DecodeSegmentHeader(std::span<const std::byte> in);

}  // namespace kds::wal
