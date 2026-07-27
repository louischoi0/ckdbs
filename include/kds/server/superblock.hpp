#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"

// The SuperBlock: one fixed logical page (kPageSize) holding whole-database
// metadata - magic/version, the page-id counter, and mount/fsync
// bookkeeping. Ported from the legacy kernel engine's kds_superblock_t
// (kds_meta.h), redesigned for this project's ownership model instead of
// that one's atomics.
//
// Ownership / concurrency: unlike the legacy kernel build (multiple
// kthreads incrementing shared atomic64_t counters), this SuperBlock is
// owned exclusively by the single DB master server process/thread
// (src/server) - the one place page ids are minted and mount/fsync
// timestamps are updated. Per rules.md #3 (thread-per-core, shared-nothing;
// cross-core communication uses explicit message/queue interfaces), other
// cores never touch a SuperBlock instance directly - they request page ids
// etc. from the server thread via its message interface (not yet built).
// That single-writer property is what lets every method below be a plain
// (non-atomic) read/increment instead of the legacy code's atomic64_t
// fields.
//
// Persistence: this file only does in-memory state plus encode/decode to
// a raw kPageSize buffer (same field-wise memcpy, no reinterpret_cast,
// no bitfields discipline as heap_page.hpp). Actually reading/writing that
// buffer to a block device is a separate, not-yet-built concern (I/O
// backend abstraction is an open decision - see CLAUDE.md).

namespace kds::server {

// Page ids below this are reserved for fixed-position system structures
// (the superblock's own page, catalog pages, etc.) - ported from the
// legacy engine's KDS_SYS_RESERVED_PAGES. A fresh SuperBlock starts minting
// user page ids at this value.
inline constexpr std::uint64_t kFirstUserPageId = 128;

// Fixed page id the SuperBlock itself lives at. Reserved, same convention
// as the catalog's fixed pages (kds::catalog::kCatalogPage*, ids 4-8):
// registered via PageStore::CreateAt() with this exact value, never
// handed out by a general-purpose allocator.
inline constexpr PageId kSuperBlockPageId = 0;

inline constexpr std::uint64_t kSuperBlockMagic = 0x3153424458444B43ULL;  // "CKDXDBS1", arbitrary but stable
inline constexpr std::uint32_t kSuperBlockVersion = 1;

// ---- On-disk field layout ----------------------------------------------

struct SuperBlockFields {
    std::uint64_t magic;
    std::uint32_t version;
    std::uint32_t reserved1;
    std::uint64_t max_page_id;
    std::uint64_t last_commit_page_id;
    std::uint64_t create_time;       // unix seconds
    std::uint64_t last_mount_time;
    std::uint64_t last_fsync_time;
    std::uint64_t total_pages;
    std::uint64_t free_pages;
    PageId alloc_point;
    std::uint32_t reserved3;  // keeps alloc_remaining 8-byte-offset-aligned
    std::uint64_t alloc_remaining;
};

inline constexpr std::size_t kMagicOffset = 0;
inline constexpr std::size_t kVersionOffset = 8;
inline constexpr std::size_t kReserved1Offset = 12;
inline constexpr std::size_t kMaxPageIdOffset = 16;
inline constexpr std::size_t kLastCommitPageIdOffset = 24;
inline constexpr std::size_t kCreateTimeOffset = 32;
inline constexpr std::size_t kLastMountTimeOffset = 40;
inline constexpr std::size_t kLastFsyncTimeOffset = 48;
inline constexpr std::size_t kTotalPagesOffset = 56;
inline constexpr std::size_t kFreePagesOffset = 64;
inline constexpr std::size_t kAllocPointOffset = 72;
inline constexpr std::size_t kReserved3Offset = 76;
inline constexpr std::size_t kAllocRemainingOffset = 80;
// Bytes actually read/written; the rest of the page (up to kPageSize) is
// reserved for future fields and always encoded as zero, same intent as
// the legacy struct's explicit reserved2[8104] tail.
inline constexpr std::size_t kSuperBlockUsedSize = 88;

static_assert(offsetof(SuperBlockFields, magic) == kMagicOffset);
static_assert(offsetof(SuperBlockFields, version) == kVersionOffset);
static_assert(offsetof(SuperBlockFields, reserved1) == kReserved1Offset);
static_assert(offsetof(SuperBlockFields, max_page_id) == kMaxPageIdOffset);
static_assert(offsetof(SuperBlockFields, last_commit_page_id) == kLastCommitPageIdOffset);
static_assert(offsetof(SuperBlockFields, create_time) == kCreateTimeOffset);
static_assert(offsetof(SuperBlockFields, last_mount_time) == kLastMountTimeOffset);
static_assert(offsetof(SuperBlockFields, last_fsync_time) == kLastFsyncTimeOffset);
static_assert(offsetof(SuperBlockFields, total_pages) == kTotalPagesOffset);
static_assert(offsetof(SuperBlockFields, free_pages) == kFreePagesOffset);
static_assert(offsetof(SuperBlockFields, alloc_point) == kAllocPointOffset);
static_assert(offsetof(SuperBlockFields, reserved3) == kReserved3Offset);
static_assert(offsetof(SuperBlockFields, alloc_remaining) == kAllocRemainingOffset);
static_assert(kSuperBlockUsedSize <= kPageSize);

// ---- SuperBlock ----------------------------------------------------------

class SuperBlock {
public:
    // Zero-initialized, not a valid on-disk image (magic is 0) - use
    // CreateFresh() for a brand-new database or Decode() for an existing
    // one. Never fails (rules.md #1: constructors must not fail).
    SuperBlock() noexcept;

    // Formats a brand-new superblock for an empty database: sets magic/
    // version, zeroes the counters, stamps create_time/last_mount_time to
    // `now_unix_seconds`, and starts page-id minting at kFirstUserPageId.
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
    std::uint64_t max_page_id() const noexcept { return fields_.max_page_id; }
    std::uint64_t last_commit_page_id() const noexcept { return fields_.last_commit_page_id; }
    std::uint64_t create_time() const noexcept { return fields_.create_time; }
    std::uint64_t last_mount_time() const noexcept { return fields_.last_mount_time; }
    std::uint64_t last_fsync_time() const noexcept { return fields_.last_fsync_time; }
    std::uint64_t total_pages() const noexcept { return fields_.total_pages; }
    std::uint64_t free_pages() const noexcept { return fields_.free_pages; }

    // Mints one fresh page id, bumping max_page_id. Single-writer only -
    // see the file-level ownership note above.
    PageId AllocatePageId() noexcept;

    // Mints `count` consecutive fresh page ids at once and returns the
    // first one (ids [first, first + count) are now reserved), the batch
    // equivalent of the legacy alloc_page_id_batch().
    PageId AllocatePageIdBatch(std::uint64_t count) noexcept;

    // Persists the page-id pre-allocation range (see
    // kds::page_alloc, not yet built) so it can resume across a clean
    // restart instead of always starting a fresh range. Ported from the
    // legacy kds_meta_set_alloc_range()/kds_meta_get_alloc_range() - same
    // crash-safety caveat applies: only call this at clean shutdown, since
    // trusting a persisted in-flight range after an unclean shutdown could
    // hand out a page id twice.
    void SetAllocRange(PageId alloc_point, std::uint64_t remaining) noexcept;
    std::pair<PageId, std::uint64_t> alloc_range() const noexcept;

    // Updates total/free page counts as reported by the page allocator.
    void SetPageCounts(std::uint64_t total_pages, std::uint64_t free_pages) noexcept;

    // Stamps last_mount_time to `now_unix_seconds` (call once per boot,
    // after Decode()/CreateFresh() succeeds).
    void MarkMounted(std::uint64_t now_unix_seconds) noexcept;

    // Stamps last_fsync_time and advances last_commit_page_id to the
    // current max_page_id. Call this right after actually persisting the
    // encoded page to disk, not before - it records that the fsync
    // happened, matching the legacy kds_superblock_fsync() ordering.
    void MarkFsynced(std::uint64_t now_unix_seconds) noexcept;

private:
    explicit SuperBlock(SuperBlockFields fields) noexcept : fields_(fields) {}

    SuperBlockFields fields_;
};

}  // namespace kds::server
