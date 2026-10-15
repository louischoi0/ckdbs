#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/tagged_cell.hpp"

// The SuperBlock: one fixed logical page (kPageSize) holding whole-database
// metadata - identity (magic/version/create time), the mount stamp, and the
// WAL anchor table recovery starts from.
//
// What it deliberately does *not* hold is allocation state - no page-id
// counter, no total/free page counts, no pre-allocation range. Which page
// ids exist is answered by DevicePageStore's free-map page, the one
// structure that has to agree with the device. Two records of the same
// fact is one record too many.
//
// Ownership / concurrency: one SuperBlock for the instance, which every core
// writes - an anchor publish, a transaction-id carve - under the superblock
// latch (`Expeditor`'s, `rules.md` §3). The methods below are plain
// non-atomic reads and writes because every concurrent caller holds that
// latch (null where one thread writes page 0), not because one thread owns
// the object.
//
// Persistence: this file only does in-memory state plus encode/decode to
// a raw kPageSize buffer (same field-wise memcpy, no reinterpret_cast,
// no bitfields discipline as heap_page.hpp); DevicePageStore writes that
// buffer out. The superblock is a headered page class (page.md section 1),
// so its fields start at kSuperBlockBodyOffset and bytes 0..32 belong to
// the common header - page_type, checksum, and the page_lsn redo needs.

namespace kds::server {

// Page ids below this are reserved for fixed-position system structures
// (the superblock's own page, the free map, catalog pages). It is where
// DevicePageStore starts handing out ids, not something the superblock
// itself tracks.
inline constexpr PageId kFirstUserPageId = 128;

// Fixed page id the SuperBlock itself lives at. Reserved, same convention
// as the catalog's fixed pages (kds::catalog::kCatalogPage*, ids 4-15):
// registered via PageStore::CreateAt() with this exact value, never
// handed out by a general-purpose allocator.
inline constexpr PageId kSuperBlockPageId = 0;

inline constexpr std::uint64_t kSuperBlockMagic = 0x3153424458444B43ULL;  // "CKDXDBS1", arbitrary but stable
// Bumped whenever the layout below changes. There is no migration path and
// no compatibility window: Decode() refuses anything but this exact number,
// so a stale data file fails loudly at mount instead of being misparsed.
// **A catalog-row size change and a new bootstrap relation are each as
// breaking as a superblock field**: without a bump the file mounts cleanly
// and then fails on its first catalog read naming a size, or on the first
// statement reaching for a table it does not have - the opaque failure a
// version exists to turn into a refusal at the door. A new field past the
// anchor table is a bump too, because an old image holds zeroes there that
// read as real values. `docs/spec/page.md` section 16's "no shipped format"
// is about the page layout, not the catalog rows; this list is the
// authority on whether a catalog row may grow quietly, and it says no.
// The entries through 17 are history (full text at f3da0413); one line each:
// 4 -> 5: SysTableRow grew `varheap_page_id`.
// 5 -> 6: SysPatternRow grew `flags`/`origin`, bootstrap gained
//   `sys.pattern_defs` (withdrawn 2026-08-31 with no bump; the row bytes stay).
// 6 -> 7: bootstrap gained `sys.access_stats`.
// 7 -> 8: bootstrap gained `sys.cabins` (cabin.md §10).
// 8 -> 9: `next_trx_id` (txn.md section 4.2); a zero would reissue
//   kBootstrapXid.
// 9 -> 10: `core_count`, and `sys.tables` grew `owner_core`.
// 10 -> 11: bootstrap gained `sys.fkeys` (foreign-keys.md §1).
// 11 -> 12: SysIndexRow rewritten for multi-column and covering indexes
//   (index.md §12) - a bump that protects the other direction, an older
//   binary opening a newer file.
// 12 -> 13: bootstrap gained `sys.assertions` on fixed page 14, which was
//   the first overflow-range id a version-12 file could already hold; the
//   overflow range starts at 15 since (16 since 15 -> 16).
// 13 -> 14: `sys.tables` grew the key-mode byte. Repurposed in place with
//   no bump when the key mode went (2026-08-25); since BB-S3b (2026-10-06)
//   it stays, written 0 and read by nothing.
// 14 -> 15 (2026-08-24): SysTableRow grew `anchor_page_id` (PW2-1).
// 15 -> 16 (2026-08-27): bootstrap gained `sys.ranges` on page 15;
//   `kCatalogOverflowFirst` moved to 16. Retired at 17 -> 18.
// No bump on 2026-09-02 (AL-R3): `log_topology` consumed `reserved1`, the
//   u32 at offset 12 that every earlier image wrote as zero.
// 16 -> 17 (2026-09-05, AM-S4, on D14): a version-16 volume's `page_lsn`s
//   are offsets into per-core streams no rewrite could make one stream's,
//   so it is refused with no migration. History at cf889b63.
// 17 -> 18 (2026-09-30): **bootstrap lost `sys.ranges`**, and the split
// relation with it (`raft-marks-2026-09-30.md` §9). A relation is one
// structure headed by `desc_page_id`, always, and this build reads nothing
// else; a version-17 file that holds a split relation would be read from
// its first chain alone, so the version refuses it (D14). Page 15 and oid
// 133 are left unused rather than reissued.
// 18 -> 19 (2026-09-30, AY-S8): **an `ASSERT_SNAPSHOT` carries its run's
// chunk count** (`wal/payload.hpp`), and recovery takes a run as a base only
// once it has that many chunks. A version-18 volume's log holds snapshots
// with no count, which this build does not read; the version refuses the
// volume rather than giving it a reader (AY-Q6 (C), `raft-marks-2026-09-30.md`
// §13). The WAL segment's own format version does not move: a log is read
// only with its own volume, so the volume's word is the one that decides, and
// a count-less record that reaches a reader anyway is Corruption.
// 19 -> 20 (2026-10-07, BD-R8): **a B+ tree leaf is in key order by
// placement** (`instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`).
// A leaf places a row where its key sorts, the lookup is one binary search
// with no linear fallback, and a split is one BTREE_SPLIT record. A
// version-19 volume an engine before `1b5d252e` wrote at `cores > 1` can hold
// a leaf out of key order, which that search would read wrong, and its log
// has no BTREE_INSERT/BTREE_SPLIT to replay. Refused with no migration (the
// operator's standing order of 2026-10-07, `raft-marks-2026-10-07.md` §11),
// which retires BB-R11's mount check: no version-20 volume carries the
// `sys.tables` byte it read set.
// 20 -> 21 (2026-10-07, BF-R2, BF-Q4): **a dropped relation's tombstone
// carries the roots its reclaim walks from**
// (`instructions/v3.0.0/workorder-bf-drop-table-page-reclaim.md`). The
// `sys.objects` word at offset 24, written 0 for every table until now, is
// `pending_roots` on a tombstone: the anchor and the var-heap root, packed,
// written by the drop's retype and cleared once the reclaim is durable. A
// version-20 volume's tombstones carry 0 there, which this build would read
// as "nothing owed" and so leak their pages for good; refused with no
// migration (`raft-marks-2026-10-07.md` §11), so every tombstone on a
// mountable volume was written by a drop that recorded what it owes.
//
// 22 (BK, `instructions/v3.0.0/workorder-bk-keystone-48.md` BK-R4): the
// Keystone word is `id:48 | flags:8 | reserved:8`. A version-21 volume's
// tuple words would read under that layout - every bit above the old id
// was written zero - but its Cabin bound entries would not: their live flags
// sit in the bits that are the id's now, on their pages and inside every
// `ASSERT_RESERVE`/`ASSERT_BUILD` record. Refused with no migration, the
// standing order of 2026-10-07 applied once more.
//
// 23 (BR, `instructions/v3.0.0/br-s0-census.md` §2): the archive lineage and
// the backup a restore is owed for follow `core_count`. A version-22 image would
// read them as zero - no lineage, no restore owed - which is true of it, but
// the standing order refuses it all the same rather than mount an image
// this build did not write.
//
// 24 (BV-S1, `instructions/v3.2.0/workorder-bv-transition-constraints.md`
// BV-Q1 (a)): bootstrap gained `sys.transitions` on fixed page 16, the
// first overflow-range id a version-23 file could already hold;
// `kCatalogOverflowFirst` moved to 17. A version-23 volume has no such
// relation, so a Transition could be neither declared nor read on it;
// refused with no migration, the standing order of 2026-10-07.
//
// 25 (BW-S1, `instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`
// BW-R9): `sys.columns` rows gained a flags byte (a dropped column, a column
// in a slot) and `sys.tables` rows the relation's spare-slot count, so a
// version-24 catalog page's rows are the wrong size; refused with no
// migration, the standing order of 2026-10-07.
//
// 26 (AQ-S1, `instructions/v3.0.0/workorder-aq-expression-cabins.md`
// AQ-R1 as revised): `sys.cabins` rows gained the Cabin's shape and its
// `expr_id`, so a version-25 catalog page's rows are the wrong size, and
// bootstrap gained `sys.cabin_exprs` on fixed page 15. Refused with no
// migration, the standing order of 2026-10-07, which supersedes the order's
// read-a-28-byte-row-by-length mechanism.
//
// 27 (`docs/spec/index.md` IX4b, 2026-10-15): a secondary index orders by
// the whole entry, covered bytes included, so a separator is a whole entry
// wide and an internal node's header stores that width - a version-26
// index's internal nodes and its leaves' order are both wrong. Refused with
// no migration, the standing order of 2026-10-07.
inline constexpr std::uint32_t kSuperBlockVersion = 27;

// ---- How many WAL streams this database's log is (AR0 M0) --------------
//
// A durable fact about the volume, not a setting. `kPerCoreStreams` is 0
// because that is what every image written before AR0 M0 holds in this
// word, and it is what those images are.
//
// **Since AM-S4(d) the field's job is the refusal, not a branch.** This
// build writes `kSingleStream` and can write nothing else, and `Decode`
// refuses every other value - so no code below this line asks the
// question. `kPerCoreStreams` survives as the value that refusal names,
// which is what a field a build cannot read is for: refused by name
// rather than assumed. Nothing reads the word except `Decode`'s check and
// `Encode`'s write.
inline constexpr std::uint32_t kPerCoreStreams = 0;
inline constexpr std::uint32_t kSingleStream = 1;

// ---- On-disk field layout ----------------------------------------------

// Superblock fields begin after the common page header; every offset
// below is relative to this.
inline constexpr std::size_t kSuperBlockBodyOffset = storage::kPageBodyOffset;

struct SuperBlockFields {
    std::uint64_t magic;
    std::uint32_t version;
    // Was `reserved1`; `kPerCoreStreams` (0) or `kSingleStream`, consumed
    // in place on 2026-09-02 without a format event for the reason the
    // version list above gives.
    std::uint32_t log_topology;
    std::uint64_t create_time;       // unix seconds
    std::uint64_t last_mount_time;
    std::uint32_t wal_anchor_count;  // written, read by no branch (BP-H13); the field stays

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

    // ---- The core count ----------------------------------------------------
    //
    // The `cores` this volume was last mounted with. **Not pinned** (AT-S9):
    // a mount at another count records the running one and logs the change
    // (`bootstrap.cpp`; `core_count()` below says why nothing on the volume
    // names a core). The core-count pin retired (AT-S9); its history is at cf889b63.
    //
    // Never zero in a legal image: CreateFresh takes the count from a
    // configuration that has already refused 0, and Decode refuses a zero
    // outright - a version-9 image decodes this field out of the reserved
    // tail as 0, and "boot with no cores" is not a state to carry forward.
    std::uint32_t core_count;

    // ---- Backup and restore (BR, br-s0-census.md section 2) -----------
    //
    // `lineage_id` names the history this volume's log belongs to, and is
    // carried in every archived segment's name (BR-R10): a restore that
    // opens writes log under segment numbers the original also wrote past
    // the same point, so each restore takes a fresh id before it writes
    // anything. Drawn at creation; 0 only on an image built outside
    // `BootstrapDatabase`.
    std::uint64_t lineage_id;
    // A restore owed (BR-R2): nonzero means this volume was laid out from
    // that backup and has not yet been recovered past its stop. **The
    // window itself lives in anchor slot 0**, which the restore rewrote to
    // `{0, start, stop, segment(start)}` (BR-R4: the backup's start, never
    // the copied anchor; the stop as the durable point analysis refuses to
    // end short of) - one authority, not two copies to keep in step. The
    // first anchor published after the mount clears this in the same page
    // image, so a crash before it mounts the restore again from the start.
    std::uint64_t restore_backup_id;
};

inline constexpr std::size_t kMagicOffset = 0;
inline constexpr std::size_t kVersionOffset = 8;
inline constexpr std::size_t kLogTopologyOffset = 12;
inline constexpr std::size_t kCreateTimeOffset = 16;
inline constexpr std::size_t kLastMountTimeOffset = 24;
inline constexpr std::size_t kWalAnchorCountOffset = 32;
inline constexpr std::size_t kInlineCellWidthOffset = 36;

static_assert(offsetof(SuperBlockFields, magic) == kMagicOffset);
static_assert(offsetof(SuperBlockFields, version) == kVersionOffset);
static_assert(offsetof(SuperBlockFields, log_topology) == kLogTopologyOffset);
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
// half-applied. Under one stream (AR0 M0) only slot 0 is published, which
// is what lets a mount run at a different core count than the run that
// wrote them (AT-S9 unpinned it); `wal_anchor_count` still records the
// highest slot ever published, and this file never silently reindexes.
//
// An all-zero entry means "this core has never completed a checkpoint":
// redo_start_lsn 0 is not a legal record LSN (offset 0 of segment 0 is the
// segment header, record.hpp), so zero can never be confused with a real
// anchor and correctly reads as "replay from the start of the stream".

// The most cores an instance runs. Everything sized per core is sized by
// it: the in-flight and visibility arrays (`instance_visibility.hpp`), the
// checkpoint gate's slots and the anchor's published mask, and the page
// latch's owner field (`expeditor.cpp` asserts the fit). It also sizes the
// anchor table below, whose 64 slots stay on disk although only slot 0 is
// published: 64 * kWalAnchorEntrySize = 2048 bytes, well inside one page.
// Raising it is a format-version event for that table.
inline constexpr std::uint32_t kMaxCores = 64;

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
// The ceiling is kMaxCores rather than a preference: every per-core array
// the instance builds has exactly that many slots (`kMaxCores` above), so a
// core above it has no slot to publish into. Zero is refused because a database with no reactor is not a
// configuration, it is a typo.
Status CheckCoreCount(std::uint32_t cores);

// A fresh archive lineage id (BR-R10): random, never 0. Drawn when a volume
// is created and when a restore lays one out, so two histories that share
// segment numbers never share archive names.
std::uint64_t DrawLineageId();

inline constexpr std::size_t kWalAnchorTableOffset = 40;
static_assert(kWalAnchorTableOffset % alignof(std::uint64_t) == 0);
inline constexpr std::size_t kWalAnchorTableSize = kMaxCores * kWalAnchorEntrySize;

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

// Appended past core_count, 8-aligned (v23, BR): no existing offset moves.
inline constexpr std::size_t kLineageIdOffset = kCoreCountOffset + 8;
inline constexpr std::size_t kRestoreBackupIdOffset = kLineageIdOffset + 8;
static_assert(kLineageIdOffset % alignof(std::uint64_t) == 0);

inline constexpr std::size_t kSuperBlockUsedSize = kRestoreBackupIdOffset + sizeof(std::uint64_t);

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
    // `core_count` is validated by the same caller (CheckCoreCount), and
    // its default is 1 for the same reason the width's exists: callers that
    // are not testing the count.
    // The log topology is `kSingleStream` and takes no parameter
    // (AM-S4(d)): it is what the volume's log **is**, chosen once here and
    // never again, and this build has one answer.
    static SuperBlock CreateFresh(
        std::uint64_t now_unix_seconds,
        std::uint32_t inline_cell_width = storage::kDefaultInlineCellWidth,
        std::uint32_t core_count = 1) noexcept;

    // Reads a superblock image out of a raw page buffer (e.g. just loaded
    // off disk). Fails with Corruption if the magic doesn't match
    // kSuperBlockMagic - callers should treat that as "no database here
    // yet" and call CreateFresh() instead - and with Corruption for any
    // version but kSuperBlockVersion or a topology but kSingleStream.
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

    // **The `cores` this volume was last mounted with**, not pinned (AT-S9):
    // with one stream (AM-S4(d)) and no owners (D17) nothing on the volume
    // names a core, so a mount at another count sets it
    // (`SetCoreCount`) and everything sized by it - the anchor fold's
    // warm-up, the lock table, the page latch - is sized from the running
    // count. Validated by `CheckCoreCount`, as at creation.
    std::uint32_t core_count() const noexcept { return fields_.core_count; }
    Status SetCoreCount(std::uint32_t cores) noexcept;

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

    // `core_id + 1` of the highest slot ever published: 1 once slot 0 is,
    // 0 before, since `SetWalAnchor` admits slot 0 alone. Read by no mount
    // branch (BP-H13), so a mount at another core count reads the same
    // anchor.
    std::uint32_t wal_anchor_count() const noexcept { return fields_.wal_anchor_count; }

    // The anchor for `core_id`, or an all-zero one (no checkpoint yet) for
    // a core that never published. Out-of-range ids read as zero rather
    // than failing: "no anchor" is the right answer for a core this
    // database has no record of, and it makes recovery replay the whole
    // stream, which is safe.
    WalAnchorFields wal_anchor(std::uint32_t core_id) const noexcept;

    // Every slot, indexed by core id, for the one caller left:
    // `SuperBlockCheckpointAnchor::MountAnchorOf` seeds the fold from the
    // lowest *populated* slot on the page. Only slot 0 is ever written now
    // (`SetWalAnchor` refuses the rest), so the others read zero, which is
    // "never checkpointed" and is skipped. Sized by `core_count`, so the
    // table also bounds which cores this database has.
    std::vector<WalAnchorFields> wal_anchors() const;

    // Records a completed checkpoint's anchor. In-memory only - it is
    // durable once the superblock page has been encoded and the store
    // synced, which is the caller's job and the whole of wal.md section
    // 8-3's ordering requirement. Fails with InvalidArgument for a core_id
    // at or above kMaxCores; refusing beats wrapping, which would let
    // one core's anchor silently overwrite another's.
    //
    // **Under `kSingleStream` the only legal slot is 0** (AL-R3), and any
    // other is refused InvalidArgument: there is one stream, so there is one
    // place recovery starts. Every core's completed checkpoint reaches slot
    // 0 through `SuperBlockCheckpointAnchor`'s fold, which keeps the
    // per-core numbers in memory and writes the minimum here - the refusal
    // keeps a caller from bypassing that fold.
    Status SetWalAnchor(std::uint32_t core_id, const WalAnchorFields& anchor) noexcept;

    // The `log_topology()` and `single_stream()` predicates retired
    // (AM-S4(d)); their history is at cf889b63. The word is still encoded
    // and still validated; nothing branches on it.
    //
    // **A peer's copy is the volume's whole decoded image**, handed over on
    // `CoreRuntime::Config` and copied in `Open`'s first statement. Zero is
    // a legal value of most of these fields - a zero `next_trx_id`, a zero
    // `create_time` rendering as the epoch - so a field nobody carried
    // across would answer silently and wrongly; `Open` refuses a config
    // with no image.

    // ---- Backup and restore (BR) -----------------------------------------

    std::uint64_t lineage_id() const noexcept { return fields_.lineage_id; }
    void SetLineageId(std::uint64_t lineage) noexcept { fields_.lineage_id = lineage; }

    // The backup a restore is owed for, 0 when none is (see SuperBlockFields;
    // its window is anchor slot 0). In-memory only, durable once page 0 is
    // encoded and synced.
    std::uint64_t restore_backup_id() const noexcept { return fields_.restore_backup_id; }
    bool restore_owed() const noexcept { return fields_.restore_backup_id != 0; }
    void SetRestoreBackupId(std::uint64_t id) noexcept { fields_.restore_backup_id = id; }

    // Stamps last_mount_time to `now_unix_seconds` (call once per boot,
    // after Decode()/CreateFresh() succeeds).
    void MarkMounted(std::uint64_t now_unix_seconds) noexcept;

private:
    explicit SuperBlock(SuperBlockFields fields) noexcept : fields_(fields) {}

    SuperBlockFields fields_;
    WalAnchorFields wal_anchors_[kMaxCores]{};
};

}  // namespace kds::server
