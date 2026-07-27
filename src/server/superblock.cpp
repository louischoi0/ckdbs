#include "kds/server/superblock.hpp"

#include <cstring>

namespace kds::server {

SuperBlock::SuperBlock() noexcept : fields_{} {}

SuperBlock SuperBlock::CreateFresh(std::uint64_t now_unix_seconds) noexcept {
    SuperBlockFields f{};
    f.magic = kSuperBlockMagic;
    f.version = kSuperBlockVersion;
    f.reserved1 = 0;
    f.max_page_id = kFirstUserPageId;
    f.last_commit_page_id = 0;
    f.create_time = now_unix_seconds;
    f.last_mount_time = now_unix_seconds;
    f.last_fsync_time = 0;
    f.total_pages = 0;
    f.free_pages = 0;
    f.alloc_point = 0;
    f.reserved3 = 0;
    f.alloc_remaining = 0;
    return SuperBlock(f);
}

StatusOr<SuperBlock> SuperBlock::Decode(std::span<const std::byte, kPageSize> page) {
    SuperBlockFields f{};
    const std::byte* base = page.data();

    std::memcpy(&f.magic, base + kMagicOffset, sizeof(f.magic));
    if (f.magic != kSuperBlockMagic) {
        return Status::Corruption("superblock magic mismatch");
    }

    std::memcpy(&f.version, base + kVersionOffset, sizeof(f.version));
    std::memcpy(&f.reserved1, base + kReserved1Offset, sizeof(f.reserved1));
    std::memcpy(&f.max_page_id, base + kMaxPageIdOffset, sizeof(f.max_page_id));
    std::memcpy(&f.last_commit_page_id, base + kLastCommitPageIdOffset,
                sizeof(f.last_commit_page_id));
    std::memcpy(&f.create_time, base + kCreateTimeOffset, sizeof(f.create_time));
    std::memcpy(&f.last_mount_time, base + kLastMountTimeOffset, sizeof(f.last_mount_time));
    std::memcpy(&f.last_fsync_time, base + kLastFsyncTimeOffset, sizeof(f.last_fsync_time));
    std::memcpy(&f.total_pages, base + kTotalPagesOffset, sizeof(f.total_pages));
    std::memcpy(&f.free_pages, base + kFreePagesOffset, sizeof(f.free_pages));
    std::memcpy(&f.alloc_point, base + kAllocPointOffset, sizeof(f.alloc_point));
    std::memcpy(&f.reserved3, base + kReserved3Offset, sizeof(f.reserved3));
    std::memcpy(&f.alloc_remaining, base + kAllocRemainingOffset, sizeof(f.alloc_remaining));

    return SuperBlock(f);
}

void SuperBlock::Encode(std::span<std::byte, kPageSize> page) const {
    std::byte* base = page.data();

    // Zero the whole page first so bytes beyond kSuperBlockUsedSize (the
    // reserved-for-future-fields tail) are always well-defined, rather
    // than leaking whatever the caller's buffer previously held.
    std::memset(base, 0, kPageSize);

    std::memcpy(base + kMagicOffset, &fields_.magic, sizeof(fields_.magic));
    std::memcpy(base + kVersionOffset, &fields_.version, sizeof(fields_.version));
    std::memcpy(base + kReserved1Offset, &fields_.reserved1, sizeof(fields_.reserved1));
    std::memcpy(base + kMaxPageIdOffset, &fields_.max_page_id, sizeof(fields_.max_page_id));
    std::memcpy(base + kLastCommitPageIdOffset, &fields_.last_commit_page_id,
                sizeof(fields_.last_commit_page_id));
    std::memcpy(base + kCreateTimeOffset, &fields_.create_time, sizeof(fields_.create_time));
    std::memcpy(base + kLastMountTimeOffset, &fields_.last_mount_time,
                sizeof(fields_.last_mount_time));
    std::memcpy(base + kLastFsyncTimeOffset, &fields_.last_fsync_time,
                sizeof(fields_.last_fsync_time));
    std::memcpy(base + kTotalPagesOffset, &fields_.total_pages, sizeof(fields_.total_pages));
    std::memcpy(base + kFreePagesOffset, &fields_.free_pages, sizeof(fields_.free_pages));
    std::memcpy(base + kAllocPointOffset, &fields_.alloc_point, sizeof(fields_.alloc_point));
    std::memcpy(base + kReserved3Offset, &fields_.reserved3, sizeof(fields_.reserved3));
    std::memcpy(base + kAllocRemainingOffset, &fields_.alloc_remaining,
                sizeof(fields_.alloc_remaining));
}

PageId SuperBlock::AllocatePageId() noexcept {
    return static_cast<PageId>(fields_.max_page_id++);
}

PageId SuperBlock::AllocatePageIdBatch(std::uint64_t count) noexcept {
    auto first = static_cast<PageId>(fields_.max_page_id);
    fields_.max_page_id += count;
    return first;
}

void SuperBlock::SetAllocRange(PageId alloc_point, std::uint64_t remaining) noexcept {
    fields_.alloc_point = alloc_point;
    fields_.alloc_remaining = remaining;
}

std::pair<PageId, std::uint64_t> SuperBlock::alloc_range() const noexcept {
    return {fields_.alloc_point, fields_.alloc_remaining};
}

void SuperBlock::SetPageCounts(std::uint64_t total_pages, std::uint64_t free_pages) noexcept {
    fields_.total_pages = total_pages;
    fields_.free_pages = free_pages;
}

void SuperBlock::MarkMounted(std::uint64_t now_unix_seconds) noexcept {
    fields_.last_mount_time = now_unix_seconds;
}

void SuperBlock::MarkFsynced(std::uint64_t now_unix_seconds) noexcept {
    fields_.last_fsync_time = now_unix_seconds;
    fields_.last_commit_page_id = fields_.max_page_id;
}

}  // namespace kds::server
