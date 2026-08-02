#include "kds/server/superblock.hpp"

#include <cstring>
#include <string>

#include "kds/storage/page_header.hpp"
#include "kds/storage/tagged_cell.hpp"

namespace kds::server {

SuperBlock::SuperBlock() noexcept : fields_{} {}

SuperBlock SuperBlock::CreateFresh(std::uint64_t now_unix_seconds,
                                   std::uint32_t inline_cell_width) noexcept {
    SuperBlockFields f{};
    f.magic = kSuperBlockMagic;
    f.version = kSuperBlockVersion;
    f.reserved1 = 0;
    f.create_time = now_unix_seconds;
    f.last_mount_time = now_unix_seconds;
    f.wal_anchor_count = 0;
    f.inline_cell_width = inline_cell_width;
    // The anchor table is left zeroed by the member initializer: a fresh
    // database has no checkpoint on any core, which is what an all-zero
    // anchor means (superblock.hpp).
    return SuperBlock(f);
}

StatusOr<SuperBlock> SuperBlock::Decode(std::span<const std::byte, kPageSize> page) {
    SuperBlockFields f{};
    const std::byte* base = page.data() + kSuperBlockBodyOffset;

    std::memcpy(&f.magic, base + kMagicOffset, sizeof(f.magic));
    if (f.magic != kSuperBlockMagic) {
        return Status::Corruption("superblock magic mismatch");
    }

    std::memcpy(&f.version, base + kVersionOffset, sizeof(f.version));
    if (f.version != kSuperBlockVersion) {
        // No migration path exists and none is wanted while the format is
        // still moving: an image this build did not write is refused, not
        // guessed at. Recreate the database instead.
        return Status::Corruption("superblock version " + std::to_string(f.version) +
                                  " is not this build's (" +
                                  std::to_string(kSuperBlockVersion) + ")");
    }
    std::memcpy(&f.reserved1, base + kReserved1Offset, sizeof(f.reserved1));
    std::memcpy(&f.create_time, base + kCreateTimeOffset, sizeof(f.create_time));
    std::memcpy(&f.last_mount_time, base + kLastMountTimeOffset, sizeof(f.last_mount_time));
    std::memcpy(&f.wal_anchor_count, base + kWalAnchorCountOffset, sizeof(f.wal_anchor_count));
    std::memcpy(&f.inline_cell_width, base + kInlineCellWidthOffset, sizeof(f.inline_cell_width));
    if (Status s = storage::CheckInlineCellWidth(f.inline_cell_width); !s.ok()) {
        // A width this build could never have written. Refused here rather
        // than carried forward, because every relation's row size would be
        // computed from it: a nonsense width does not produce an error
        // later, it produces rows decoded at the wrong offsets.
        return Status::Corruption("superblock: " + s.message());
    }

    SuperBlock sb(f);
    for (std::uint32_t core = 0; core < kMaxWalCores; ++core) {
        const std::byte* entry = base + kWalAnchorTableOffset + core * kWalAnchorEntrySize;
        WalAnchorFields& a = sb.wal_anchors_[core];
        std::memcpy(&a.checkpoint_lsn, entry + kWalAnchorCheckpointLsnOffset,
                    sizeof(a.checkpoint_lsn));
        std::memcpy(&a.redo_start_lsn, entry + kWalAnchorRedoStartLsnOffset,
                    sizeof(a.redo_start_lsn));
        std::memcpy(&a.durable_lsn, entry + kWalAnchorDurableLsnOffset, sizeof(a.durable_lsn));
        std::memcpy(&a.segment_no, entry + kWalAnchorSegmentNoOffset, sizeof(a.segment_no));
    }
    return sb;
}

void SuperBlock::Encode(std::span<std::byte, kPageSize> page) const {
    // Format only a page that is not already a superblock: FormatPage
    // resets page_lsn, and re-encoding an existing superblock (every mount
    // stamps last_mount_time) must not erase the LSN redo compares against
    // (wal.md section 9).
    if (storage::RawPageType(page) != static_cast<std::uint8_t>(PageType::kSuperBlock)) {
        storage::FormatPage(page, PageType::kSuperBlock);
    }

    std::byte* base = page.data() + kSuperBlockBodyOffset;

    // Zero the body first so bytes beyond kSuperBlockUsedSize (the
    // reserved-for-future-fields tail) are always well-defined, rather
    // than leaking whatever the caller's buffer previously held. The
    // common header below kSuperBlockBodyOffset is left alone.
    std::memset(base, 0, kPageSize - kSuperBlockBodyOffset);

    std::memcpy(base + kMagicOffset, &fields_.magic, sizeof(fields_.magic));
    std::memcpy(base + kVersionOffset, &fields_.version, sizeof(fields_.version));
    std::memcpy(base + kReserved1Offset, &fields_.reserved1, sizeof(fields_.reserved1));
    std::memcpy(base + kCreateTimeOffset, &fields_.create_time, sizeof(fields_.create_time));
    std::memcpy(base + kLastMountTimeOffset, &fields_.last_mount_time,
                sizeof(fields_.last_mount_time));
    std::memcpy(base + kWalAnchorCountOffset, &fields_.wal_anchor_count,
                sizeof(fields_.wal_anchor_count));
    std::memcpy(base + kInlineCellWidthOffset, &fields_.inline_cell_width,
                sizeof(fields_.inline_cell_width));

    // The whole table is written, not just the slots in use: an unpublished
    // slot's zeroes are meaningful (superblock.hpp), and writing only the
    // used prefix would leave the rest to the memset above by accident
    // rather than by contract.
    for (std::uint32_t core = 0; core < kMaxWalCores; ++core) {
        std::byte* entry = base + kWalAnchorTableOffset + core * kWalAnchorEntrySize;
        const WalAnchorFields& a = wal_anchors_[core];
        std::memcpy(entry + kWalAnchorCheckpointLsnOffset, &a.checkpoint_lsn,
                    sizeof(a.checkpoint_lsn));
        std::memcpy(entry + kWalAnchorRedoStartLsnOffset, &a.redo_start_lsn,
                    sizeof(a.redo_start_lsn));
        std::memcpy(entry + kWalAnchorDurableLsnOffset, &a.durable_lsn, sizeof(a.durable_lsn));
        std::memcpy(entry + kWalAnchorSegmentNoOffset, &a.segment_no, sizeof(a.segment_no));
    }
}

WalAnchorFields SuperBlock::wal_anchor(std::uint32_t core_id) const noexcept {
    if (core_id >= kMaxWalCores) {
        return WalAnchorFields{};
    }
    return wal_anchors_[core_id];
}

Status SuperBlock::SetWalAnchor(std::uint32_t core_id, const WalAnchorFields& anchor) noexcept {
    if (core_id >= kMaxWalCores) {
        return Status::InvalidArgument("superblock: WAL anchor core_id " +
                                       std::to_string(core_id) + " is at or above kMaxWalCores");
    }
    wal_anchors_[core_id] = anchor;
    if (core_id + 1 > fields_.wal_anchor_count) {
        fields_.wal_anchor_count = core_id + 1;
    }
    return Status::OK();
}

void SuperBlock::MarkMounted(std::uint64_t now_unix_seconds) noexcept {
    fields_.last_mount_time = now_unix_seconds;
}

}  // namespace kds::server
