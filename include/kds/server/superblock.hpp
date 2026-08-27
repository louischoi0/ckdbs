#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/tagged_cell.hpp"

// The SuperBlock: one fixed logical page (kPageSize) holding whole-database
// metadata - identity (magic/version/create time), the mount stamp, and the
// per-core WAL anchors recovery starts from.
//
// What it deliberately does *not* hold is allocation state - no page-id
// counter, no total/free page counts, no pre-allocation range. Which page
// ids exist is answered by DevicePageStore's free-map page, the one
// structure that has to agree with the device. Two records of the same
// fact is one record too many.
//
// Ownership / concurrency: this SuperBlock is owned exclusively by the
// single master server thread (src/server). Per rules.md #3
// (thread-per-core, shared-nothing; cross-core communication uses explicit
// message/queue interfaces), other cores never touch an instance directly.
// That single-writer property is what lets every method below be a plain
// non-atomic read or write.
//
// Persistence: this file only does in-memory state plus encode/decode to
// a raw kPageSize buffer (same field-wise memcpy, no reinterpret_cast,
// no bitfields discipline as heap_page.hpp); DevicePageStore writes that
// buffer out. The superblock is a headered page class (page.md section 1),
// so its fields start at kSuperBlockBodyOffset and bytes 0..32 belong to
// the common header - page_type, checksum, and the page_lsn redo needs.

namespace kds::server {

// Page ids below this are reserved for fixed-position system structures
// (the superblock's own page, the free map, catalog pages) - ported from
// It is where DevicePageStore
// starts handing out ids, not something the superblock itself tracks.
inline constexpr PageId kFirstUserPageId = 128;

// Fixed page id the SuperBlock itself lives at. Reserved, same convention
// as the catalog's fixed pages (kds::catalog::kCatalogPage*, ids 4-8):
// registered via PageStore::CreateAt() with this exact value, never
// handed out by a general-purpose allocator.
inline constexpr PageId kSuperBlockPageId = 0;

inline constexpr std::uint64_t kSuperBlockMagic = 0x3153424458444B43ULL;  // "CKDXDBS1", arbitrary but stable
// Bumped whenever the layout below changes. There is no migration path and
// no compatibility window while the format is still moving: Decode() refuses
// anything but this exact number, so a stale data file fails loudly at mount
// instead of being misparsed. Bump it with every layout change.
// 4 -> 5: the var-heap added `varheap_page_id` to SysTableRow, which grew
// that row's on-disk size. Nothing in the *superblock* changed, but a
// catalog row format change is exactly as breaking, and without a bump a
// pre-var-heap database mounted happily and then failed on its first
// catalog read with an opaque size mismatch. A format version is what
// makes that a refusal at the door instead.
// 5 -> 6: CREATE PATTERN, twice over. `SysPatternRow` grew `flags` and
// `origin` (38 -> 41 bytes), and bootstrap gained a seventh system relation,
// `sys.pattern_defs`, on a fixed page id a pre-existing file does not have.
// Either alone is the same breakage as the 4 -> 5 case; the second is worse,
// because the missing relation is not a size mismatch anyone would recognize
// - the first CREATE PATTERN would simply fail to find a table that every
// build after this one creates at bootstrap.
// 6 -> 7: bootstrap gained `sys.access_stats` on a fixed page id a
// pre-existing file does not have. Same shape of breakage as 5 -> 6's
// sys.pattern_defs: the file would mount and then fail on the first access
// the statistics path tried to record, naming a table that every build
// after this one creates at bootstrap.
// 7 -> 8: bootstrap gained `sys.cabins` (docs/spec/cabin.md §10) on a fixed
// page id a pre-existing file does not have - the third repeat of the
// 5 -> 6 shape. Worth stating once for all three: a *new bootstrap
// relation* is as breaking as a row layout change and less obvious about
// it, because the file mounts cleanly and then fails on the first
// statement that reaches for a table which does not exist. The refusal at
// the door is the whole point.
// 8 -> 9: the transaction manager added `next_trx_id` (docs/spec/txn.md section
// 4.2). Unlike the four bumps above it this one adds a *field*, not a
// relation - and it is exactly as breaking, because the field sits past the
// WAL anchor table where a version-8 image holds zeroes. A zero there reads
// as "the next transaction is id 0", which would hand a real transaction
// the id that means "no transaction" and then reissue kBootstrapXid, the
// one id every read view trusts unconditionally. Refusing at the door is
// the only safe reading of a zero we cannot distinguish from a real value.
// 9 -> 10: multicore added `core_count` (docs/inflight/in-progress/workplan-crosscore.md M6), and
// `sys.tables` grew `owner_core` (M1). Either alone is a bump by the rules
// above - the first is a field past `next_trx_id` where a version-9 image
// holds zeroes, and a zero core count is not "unset", it is "boot with no
// cores"; the second is the fifth repeat of the catalog-row-layout break
// 4 -> 5 first recorded. The count is pinned rather than adopted because
// recovery under a changed core count is [OPEN] (wal.md section 3): a
// database whose streams were written by N cores must not be mounted by M
// until something decides what happens to the other streams.
// 10 -> 11: bootstrap gained `sys.fkeys` (docs/spec/foreign-keys.md §1) on a
// fixed page id a pre-existing file does not have - the fourth repeat of the
// 5 -> 6 shape, and the one where mounting anyway would be worst. A
// version-10 file has no page 13, so `CREATE TABLE ... REFERENCES` would
// fail to find the relation that records the constraint, and - once FK-M2
// lands - every write path would read an empty foreign-key list and enforce
// nothing. A constraint that silently does not run is not a degraded mode.
// 11 -> 12: `SysIndexRow` was rewritten for multi-column and covering
// indexes (docs/spec/index.md §12, workplan IX03) - a root page, a name, and
// two column arrays where there had been one `col_pos`.
//
// **The reason here is not the reason the four bumps above had, and the
// difference is worth stating so the next one is argued rather than copied.**
// Those protected an existing file from a build that would misparse it. This
// one cannot: nothing ever wrote a sys.indexes row, so no pre-existing file
// has one to misparse, and a version-11 file would in fact mount and run
// correctly. What the bump protects is the **other direction** - an older
// binary opening a *newer* file. It would find rows whose size its `Decode`
// rejects and fail on every SELECT compile (`HasUnindexedEqualityFilter`
// asks sys.indexes per statement), which is the opaque failure a version
// exists to turn into a refusal that names both numbers.
//
// The price is the usual one and is not reduced by any of that: every
// pre-existing data file stops mounting.
// 12 -> 13: bootstrap gained `sys.assertions` (docs/spec/assertion.md §8.2,
// workplan AST03) on fixed page 14 - the **fifth repeat of the 5 -> 6
// shape**, and it carries a second break the four before it did not.
//
// The first half is the familiar one: a version-12 file has no page 14, so
// `CREATE ASSERTION` would fail to find the relation that records the
// declaration, and it would fail *opaquely* - the file mounts cleanly and
// then names a table every build after this one creates at bootstrap.
//
// The second half is new and is the reason this one could not have been
// avoided by picking a different page id. Page 14 was the **first id of the
// catalog overflow range** (`kCatalogOverflowFirst`), which is where a
// catalog relation's chain grows when its root page fills. A version-12 file
// that ever outgrew one of its nine catalog roots put a `sys.columns` or
// `sys.tables` overflow page at id 14 - and this build would create
// sys.assertions on top of it. That is not a missing relation, it is a
// silently overwritten one, which is the worst failure any bump on this list
// has protected against. The range now starts at 15.
//
// The price is the usual one and, here, exactly what is wanted: every
// pre-existing data file stops mounting.
// 13 -> 14: `sys.tables` grew `key_mode` (docs/spec/heap-and-tuple.md section
// 4.1, workplan-key-mode.md PK01) - the **sixth repeat of the catalog-row-
// layout break** 4 -> 5 first recorded, and the second time SysTableRow
// specifically has been the row that grew (9 -> 10 was `owner_core`).
//
// One byte, and the bump is not negotiable for it. A version-13 file has
// SysTableRow rows one byte short, and `Decode` refuses anything but the
// exact size (rows.hpp's stated reason: a heap tuple's data_len is
// caller-supplied at read time, so a length mismatch is a real corruption
// signal). Without the bump the file mounts cleanly and then fails on the
// first catalog read, naming a size rather than a version - precisely the
// opaque failure 4 -> 5 exists to convert into a refusal at the door.
//
// Worth recording because it was nearly missed: `docs/spec/page.md` section 16
// says no shipped format exists, which is true of the *page* layout and not
// of the catalog rows, and the spec amendment quoted it at the wrong
// subject before this list was read. The authority on whether a catalog row
// may grow quietly is this comment block, and it has said no six times.
// 14 -> 15 (2026-08-24): SysTableRow grew `anchor_page_id` (PW2-1) - four
// bytes, and Decode refuses any size but the exact one, so without the
// bump a pre-anchor file would mount and fail on its first catalog read
// naming a size instead of a version (the key-mode byte's precedent,
// verbatim).
//
// **No bump on 2026-08-25, and the reason is worth recording** because this
// list is where a missed one would be found. The key mode was removed
// (docs/spec/heap-and-tuple.md section 4.1), and the byte the 13 -> 14 entry
// above bought was **repurposed in place** rather than dropped: same offset,
// same width, and `SysTableRow::key_order` reads the two old values as the
// facts they already implied - kAssigned's 0 as "every id here ascended",
// kExplicit's 1 as "an id landed out of order". `kOnDiskSize` did not move,
// so `Decode` accepts a version-15 file and every row in it means what it
// meant. Dropping the byte instead would have been the seventh break on
// this list, and it would have bought nothing: the flag has a live reader.
inline constexpr std::uint32_t kSuperBlockVersion = 15;

// ---- On-disk field layout ----------------------------------------------

// Superblock fields begin after the common page header; every offset
// below is relative to this.
inline constexpr std::size_t kSuperBlockBodyOffset = storage::kPageBodyOffset;

struct SuperBlockFields {
    std::uint64_t magic;
    std::uint32_t version;
    std::uint32_t reserved1;
    std::uint64_t create_time;       // unix seconds
    std::uint64_t last_mount_time;
    std::uint32_t wal_anchor_count;  // anchor slots ever published into

    // ---- The pinned tuple-cell width (heap-and-tuple.md section 3.3) ----
    //
    // `kds.inline_cell_width`: the number of bytes every variable-width
    // value occupies in a tuple. It lives here, not only in the config
    // file, because **on-disk tuple layout depends on it** - a relation's
    // row size is a schema constant computed from it, so the same database
    // read under a different width would decode every row at the wrong
    // offsets. Configuration proposes the value once, at bootstrap; the
    // superblock is what pins it.
    //
    // Every later mount validates the running configuration against this
    // and refuses to start on a disagreement, naming both numbers
    // (bootstrap.cpp). Changing it for existing data is a rebuild, which is
    // Unsupported (docs/rules/rule-fixed-length-tuple.md V5).
    //
    // It occupies what was `reserved2` through version 3 - a u32 in exactly
    // this position, which is why the anchor table below did not move. The
    // version bump is what makes that repurposing safe: a version-3 image
    // is refused outright rather than read with a zero here.
    std::uint32_t inline_cell_width;

    // ---- The transaction id ceiling (docs/spec/txn.md section 4.2) -----------
    //
    // The next transaction id **never yet issued**, which is not the same
    // as the next one to hand out: ids are allocated a block at a time and
    // this is the block's ceiling, so a crash burns the unspent remainder
    // (txn/trx_id.hpp). Ids are unique and monotonic, never gapless - the
    // same promise `sys.tables.next_id` makes for row ids, and for the same
    // reason: a durable write per id is what the alternative costs.
    //
    // Seeded to kFirstUserTrxId = 2 by CreateFresh, so kBootstrapXid (1) is
    // never reissued to a real transaction. That is what makes every
    // catalog row permanently visible to every read view.
    //
    // It lives past the WAL anchor table rather than in the header block
    // because the table's offset is fixed by four earlier format versions
    // and moving it would rewrite every anchor's position for no gain.
    std::uint64_t next_trx_id;

    // ---- The pinned core count (docs/inflight/in-progress/workplan-crosscore.md M6) ----------
    //
    // How many reactor cores this database was created for. Like
    // `inline_cell_width` above it, configuration proposes the value once -
    // at the bootstrap of a *new* database - and the superblock is what pins
    // it; every later mount validates the running `cores` against this and
    // refuses to start on a disagreement, naming both numbers
    // (bootstrap.cpp).
    //
    // The reason it is pinned rather than simply adopted is the WAL. Streams
    // are per core and LSNs are stream-local (wal.md section 3), so a
    // database written by N cores holds N streams and N anchor slots; a
    // mount at M cores would leave |N - M| streams with nothing to replay
    // them and no rule for what to do about it. Stream reassignment is
    // [OPEN], and a refusal at the door is what keeps this file from
    // deciding it by accident.
    //
    // Never zero in a legal image: CreateFresh takes the count from a
    // configuration that has already refused 0, and Decode refuses a zero
    // outright - a version-9 image decodes this field out of the reserved
    // tail as 0, and "boot with no cores" is not a state to carry forward.
    std::uint32_t core_count;
};

inline constexpr std::size_t kMagicOffset = 0;
inline constexpr std::size_t kVersionOffset = 8;
inline constexpr std::size_t kReserved1Offset = 12;
inline constexpr std::size_t kCreateTimeOffset = 16;
inline constexpr std::size_t kLastMountTimeOffset = 24;
inline constexpr std::size_t kWalAnchorCountOffset = 32;
inline constexpr std::size_t kInlineCellWidthOffset = 36;

static_assert(offsetof(SuperBlockFields, magic) == kMagicOffset);
static_assert(offsetof(SuperBlockFields, version) == kVersionOffset);
static_assert(offsetof(SuperBlockFields, reserved1) == kReserved1Offset);
static_assert(offsetof(SuperBlockFields, create_time) == kCreateTimeOffset);
static_assert(offsetof(SuperBlockFields, last_mount_time) == kLastMountTimeOffset);
static_assert(offsetof(SuperBlockFields, wal_anchor_count) == kWalAnchorCountOffset);
static_assert(offsetof(SuperBlockFields, inline_cell_width) == kInlineCellWidthOffset);

// ---- Per-core WAL anchors (wal.md section 14-3) --------------------------
//
// Where recovery starts, per core. A core's stream is only self-describing
// forward from a known point: the log carries no index, so without an
// anchor recovery has no choice but to replay from the first segment, which
// is the unbounded-RTO case checkpointing exists to prevent (wal.md section
// 1). The checkpointer publishes an entry here after - never before - its
// CHECKPOINT_END record is durable (section 8-3).
//
// Indexed directly by `core_id`, so no id is stored in the entry: a fixed
// table in the superblock page is what makes an anchor update a single
// page write, and a page write is the only kind of update that cannot land
// half-applied. Recovery under a *different* core count than the run that
// wrote these is [OPEN] (wal.md section 3), so `wal_anchor_count` records
// what the last run used and leaves the policy to whoever settles it -
// this file must not decide it by silently reindexing.
//
// An all-zero entry means "this core has never completed a checkpoint":
// redo_start_lsn 0 is not a legal record LSN (offset 0 of segment 0 is the
// segment header, record.hpp), so zero can never be confused with a real
// anchor and correctly reads as "replay from the start of the stream".

// Slots in the table. A power of two, sized so the whole table stays well
// inside one page: 64 * kWalAnchorEntrySize = 2048 bytes, which with the
// fixed fields above leaves over 3/4 of the page still reserved. Raising it
// is a format-version event, so it is deliberately generous relative to any
// core count this engine runs on today.
inline constexpr std::uint32_t kMaxWalCores = 64;

struct WalAnchorFields {
    std::uint64_t checkpoint_lsn;   // the CHECKPOINT_BEGIN record this came from
    std::uint64_t redo_start_lsn;   // where recovery's analysis phase begins
    std::uint64_t durable_lsn;      // stream's durable end when this was published
    std::uint64_t segment_no;       // segment holding redo_start_lsn
};

inline constexpr std::size_t kWalAnchorCheckpointLsnOffset = 0;
inline constexpr std::size_t kWalAnchorRedoStartLsnOffset = 8;
inline constexpr std::size_t kWalAnchorDurableLsnOffset = 16;
inline constexpr std::size_t kWalAnchorSegmentNoOffset = 24;
// 8*4 = 32 bytes per core, every field naturally aligned.
inline constexpr std::size_t kWalAnchorEntrySize = 32;

static_assert(offsetof(WalAnchorFields, checkpoint_lsn) == kWalAnchorCheckpointLsnOffset);
static_assert(offsetof(WalAnchorFields, redo_start_lsn) == kWalAnchorRedoStartLsnOffset);
static_assert(offsetof(WalAnchorFields, durable_lsn) == kWalAnchorDurableLsnOffset);
static_assert(offsetof(WalAnchorFields, segment_no) == kWalAnchorSegmentNoOffset);
static_assert(sizeof(WalAnchorFields) == kWalAnchorEntrySize);

// Whether `cores` is a count this build can run. The one test, so a config
// file, a fresh bootstrap and a mount can never come to disagree about what
// a legal core count is - the same role storage::CheckInlineCellWidth plays
// for the other pinned number.
//
// The ceiling is kMaxWalCores rather than a preference: the anchor table is
// indexed directly by `core_id` and has exactly that many slots, so a core
// above it has nowhere to publish a checkpoint from (SetWalAnchor refuses
// it). Zero is refused because a database with no reactor is not a
// configuration, it is a typo.
Status CheckCoreCount(std::uint32_t cores);

inline constexpr std::size_t kWalAnchorTableOffset = 40;
static_assert(kWalAnchorTableOffset % alignof(std::uint64_t) == 0);
inline constexpr std::size_t kWalAnchorTableSize = kMaxWalCores * kWalAnchorEntrySize;

// Bytes actually read/written; the rest of the page (up to kPageSize) is
// reserved for future fields and always encoded as zero, same intent as
// an explicit reserved tail.
// Placed after the anchor table, so no existing offset moved (see
// SuperBlockFields::next_trx_id).
inline constexpr std::size_t kNextTrxIdOffset = kWalAnchorTableOffset + kWalAnchorTableSize;
static_assert(kNextTrxIdOffset % alignof(std::uint64_t) == 0);

// Appended past next_trx_id for the same reason next_trx_id was appended
// past the anchor table: no existing offset moves.
inline constexpr std::size_t kCoreCountOffset = kNextTrxIdOffset + sizeof(std::uint64_t);
static_assert(kCoreCountOffset % alignof(std::uint32_t) == 0);

inline constexpr std::size_t kSuperBlockUsedSize = kCoreCountOffset + sizeof(std::uint32_t);

static_assert(kSuperBlockBodyOffset + kSuperBlockUsedSize <= kPageSize);

// ---- SuperBlock ----------------------------------------------------------

class SuperBlock {
public:
    // Zero-initialized, not a valid on-disk image (magic is 0) - use
    // CreateFresh() for a brand-new database or Decode() for an existing
    // one. Never fails (rules.md #1: constructors must not fail).
    SuperBlock() noexcept;

    // Formats a brand-new superblock for an empty database: sets magic/
    // version, stamps create_time/last_mount_time to `now_unix_seconds`,
    // pins `inline_cell_width`, and leaves every WAL anchor zeroed (no core
    // has checkpointed yet).
    //
    // This is the *only* moment the cell width is chosen. It is not
    // validated here - a constructor must not fail (rules.md #1), and the
    // caller has already checked it (storage::CheckInlineCellWidth) before
    // deciding to create a database at all.
    // The default exists for callers that are not testing the width
    // (anchor tests, tooling); BootstrapDatabase(), the only production
    // caller, always passes the configured value explicitly.
    // `core_count` is pinned by the same rule and validated by the same
    // caller (CheckCoreCount), and its default is 1 for the same reason the
    // width's exists: callers that are not testing the count.
    static SuperBlock CreateFresh(
        std::uint64_t now_unix_seconds,
        std::uint32_t inline_cell_width = storage::kDefaultInlineCellWidth,
        std::uint32_t core_count = 1) noexcept;

    // Reads a superblock image out of a raw page buffer (e.g. just loaded
    // off disk). Fails with Corruption if the magic doesn't match
    // kSuperBlockMagic - callers should treat that as "no database here
    // yet" and call CreateFresh() instead, following the
    // fresh-init detection (kds_meta_is_fresh_init()).
    static StatusOr<SuperBlock> Decode(std::span<const std::byte, kPageSize> page);

    // Serializes this superblock into `page`. Bytes beyond
    // kSuperBlockUsedSize are zeroed (reserved for future fields).
    void Encode(std::span<std::byte, kPageSize> page) const;

    std::uint32_t version() const noexcept { return fields_.version; }
    std::uint64_t create_time() const noexcept { return fields_.create_time; }
    std::uint64_t last_mount_time() const noexcept { return fields_.last_mount_time; }

    // The pinned kds.inline_cell_width (see SuperBlockFields). Every
    // relation's row size in this database was computed from it.
    std::uint32_t inline_cell_width() const noexcept { return fields_.inline_cell_width; }

    // The pinned `cores` (see SuperBlockFields). There is deliberately no
    // setter: the count is chosen once, at bootstrap, and a mount that
    // disagrees is refused rather than reconciled.
    std::uint32_t core_count() const noexcept { return fields_.core_count; }

    // ---- The transaction id ceiling (docs/spec/txn.md section 4.2) -----------
    //
    // The next id never yet issued. `txn::TrxIdSequence` owns the policy
    // around it - block size, the durable write, and refusing to wrap - and
    // this pair is only the storage.
    std::uint64_t next_trx_id() const noexcept { return fields_.next_trx_id; }

    // Raises the ceiling. Refuses to lower it: a ceiling that moved
    // backwards would reissue ids that are already stamped on tuples, and
    // two versions written by "the same" transaction is a wrong answer no
    // later check could detect. In-memory only - it is durable once the
    // page has been encoded and the store synced, exactly as SetWalAnchor
    // is.
    Status SetNextTrxId(std::uint64_t next) noexcept;

    // ---- Per-core WAL anchors (wal.md section 14-3) ---------------------

    // How many anchor slots the last run used - `core_id + 1` of the
    // highest core that ever published. Recovery compares it against this
    // run's core count; what to do when they differ is [OPEN] (wal.md
    // section 3) and is not decided here.
    std::uint32_t wal_anchor_count() const noexcept { return fields_.wal_anchor_count; }

    // The anchor for `core_id`, or an all-zero one (no checkpoint yet) for
    // a core that never published. Out-of-range ids read as zero rather
    // than failing: "no anchor" is the right answer for a core this
    // database has no record of, and it makes recovery replay the whole
    // stream, which is safe.
    WalAnchorFields wal_anchor(std::uint32_t core_id) const noexcept;

    // Records a completed checkpoint's anchor. In-memory only - it is
    // durable once the superblock page has been encoded and the store
    // synced, which is the caller's job and the whole of wal.md section
    // 8-3's ordering requirement. Fails with InvalidArgument for a core_id
    // at or above kMaxWalCores; refusing beats wrapping, which would let
    // one core's anchor silently overwrite another's.
    Status SetWalAnchor(std::uint32_t core_id, const WalAnchorFields& anchor) noexcept;

    // Stamps last_mount_time to `now_unix_seconds` (call once per boot,
    // after Decode()/CreateFresh() succeeds).
    void MarkMounted(std::uint64_t now_unix_seconds) noexcept;

private:
    explicit SuperBlock(SuperBlockFields fields) noexcept : fields_(fields) {}

    SuperBlockFields fields_;
    WalAnchorFields wal_anchors_[kMaxWalCores]{};
};

}  // namespace kds::server
