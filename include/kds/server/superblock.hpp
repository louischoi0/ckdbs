#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/page_header.hpp"

// The SuperBlock: one fixed logical page (kPageSize) holding whole-database
// metadata - identity (magic/version/create time), the mount stamp, and the
// per-core WAL anchors recovery starts from.
//
// What it deliberately does *not* hold is allocation state. The legacy
// kernel engine's kds_superblock_t (kds_meta.h) carried a page-id counter,
// total/free page counts, and a pre-allocation range; that half was ported
// and then never wired to anything, because which page ids exist is
// answered by DevicePageStore's free-map page instead - the one structure
// that actually has to agree with the device. Two records of the same fact
// is one record too many, so this one is gone rather than kept in sync.
//
// Ownership / concurrency: unlike the legacy kernel build (multiple
// kthreads incrementing shared atomic64_t counters), this SuperBlock is
// owned exclusively by the single DB master server process/thread
// (src/server). Per rules.md #3 (thread-per-core, shared-nothing;
// cross-core communication uses explicit message/queue interfaces), other
// cores never touch a SuperBlock instance directly. That single-writer
// property is what lets every method below be a plain (non-atomic)
// read/write instead of the legacy code's atomic64_t fields.
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
// the legacy engine's KDS_SYS_RESERVED_PAGES. It is where DevicePageStore
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
inline constexpr std::uint32_t kSuperBlockVersion = 3;

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
    std::uint32_t reserved2;         // keeps the anchor table 8-byte-aligned
};

inline constexpr std::size_t kMagicOffset = 0;
inline constexpr std::size_t kVersionOffset = 8;
inline constexpr std::size_t kReserved1Offset = 12;
inline constexpr std::size_t kCreateTimeOffset = 16;
inline constexpr std::size_t kLastMountTimeOffset = 24;
inline constexpr std::size_t kWalAnchorCountOffset = 32;
inline constexpr std::size_t kReserved2Offset = 36;

static_assert(offsetof(SuperBlockFields, magic) == kMagicOffset);
static_assert(offsetof(SuperBlockFields, version) == kVersionOffset);
static_assert(offsetof(SuperBlockFields, reserved1) == kReserved1Offset);
static_assert(offsetof(SuperBlockFields, create_time) == kCreateTimeOffset);
static_assert(offsetof(SuperBlockFields, last_mount_time) == kLastMountTimeOffset);
static_assert(offsetof(SuperBlockFields, wal_anchor_count) == kWalAnchorCountOffset);
static_assert(offsetof(SuperBlockFields, reserved2) == kReserved2Offset);

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

inline constexpr std::size_t kWalAnchorTableOffset = 40;
static_assert(kWalAnchorTableOffset % alignof(std::uint64_t) == 0);
inline constexpr std::size_t kWalAnchorTableSize = kMaxWalCores * kWalAnchorEntrySize;

// Bytes actually read/written; the rest of the page (up to kPageSize) is
// reserved for future fields and always encoded as zero, same intent as
// the legacy struct's explicit reserved2[8104] tail.
inline constexpr std::size_t kSuperBlockUsedSize = kWalAnchorTableOffset + kWalAnchorTableSize;

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
    // and leaves every WAL anchor zeroed (no core has checkpointed yet).
    static SuperBlock CreateFresh(std::uint64_t now_unix_seconds) noexcept;

    // Reads a superblock image out of a raw page buffer (e.g. just loaded
    // off disk). Fails with Corruption if the magic doesn't match
    // kSuperBlockMagic - callers should treat that as "no database here
    // yet" and call CreateFresh() instead, mirroring the legacy engine's
    // fresh-init detection (kds_meta_is_fresh_init()).
    static StatusOr<SuperBlock> Decode(std::span<const std::byte, kPageSize> page);

    // Serializes this superblock into `page`. Bytes beyond
    // kSuperBlockUsedSize are zeroed (reserved for future fields).
    void Encode(std::span<std::byte, kPageSize> page) const;

    std::uint32_t version() const noexcept { return fields_.version; }
    std::uint64_t create_time() const noexcept { return fields_.create_time; }
    std::uint64_t last_mount_time() const noexcept { return fields_.last_mount_time; }

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
