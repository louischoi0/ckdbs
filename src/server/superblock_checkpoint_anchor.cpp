#include "kds/server/superblock_checkpoint_anchor.hpp"

namespace kds::server {

wal::CheckpointAnchorRecord SuperBlockCheckpointAnchor::MountAnchorOf(
    const SuperBlock& superblock) noexcept {
    const WalAnchorFields at_mount = superblock.wal_anchor(0);
    wal::CheckpointAnchorRecord record{};
    record.core_id = 0;
    record.checkpoint_lsn = at_mount.checkpoint_lsn;
    record.redo_start_lsn = at_mount.redo_start_lsn;
    record.durable_lsn = at_mount.durable_lsn;
    record.segment_no = at_mount.segment_no;
    return record;
}

wal::CheckpointAnchorRecord SuperBlockCheckpointAnchor::FoldedAnchor() const noexcept {
    // **The warm-up** (the header's second fold section): a core that has
    // not published in this process lifetime is absent from the map and
    // would contribute nothing to a minimum, so until every core has
    // published the anchor is held where the mount found it.
    if (per_core_.size() < superblock_.core_count()) {
        return mount_anchor_;
    }

    // The minimum by redo start, carrying that core's whole set of numbers
    // rather than a field-wise minimum - four numbers from four cores would
    // describe a checkpoint that never happened.
    const wal::CheckpointAnchorRecord* lowest = nullptr;
    for (const auto& [core_id, record] : per_core_) {
        if (lowest == nullptr || record.redo_start_lsn < lowest->redo_start_lsn) {
            lowest = &record;
        }
    }
    return lowest == nullptr ? mount_anchor_ : *lowest;
}

Status SuperBlockCheckpointAnchor::Publish(const wal::CheckpointAnchorRecord& anchor) {
    // Which slot, and what goes in it, is the volume's topology's answer
    // (the header's fold section).
    std::uint32_t slot = anchor.core_id;
    wal::CheckpointAnchorRecord landing = anchor;
    if (superblock_.single_stream()) {
        per_core_[anchor.core_id] = anchor;
        slot = 0;
        landing = FoldedAnchor();
    }

    const WalAnchorFields fields{landing.checkpoint_lsn, landing.redo_start_lsn,
                                 landing.durable_lsn, landing.segment_no};
    if (Status s = superblock_.SetWalAnchor(slot, fields); !s.ok()) {
        return s;
    }

    // Fetched rather than cached: the superblock page is the store's to
    // move, and holding a span across calls would outlive whatever frame it
    // came from.
    auto page = store_.Get(kSuperBlockPageId);
    if (!page.ok()) {
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("superblock", "anchor publish could not read the superblock page: " +
                                          page.status().message());
        }
        return page.status();
    }
    superblock_.Encode(page.value().bytes());
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        // The superblock is the one page whose every rewrite matters -
        // it is what a restart reads first.
        log_->Debug("superblock", "wal anchor written for core " +
                                      std::to_string(anchor.core_id) + " into slot " +
                                      std::to_string(slot) + ": redo_start=" +
                                      std::to_string(landing.redo_start_lsn) + " durable_lsn=" +
                                      std::to_string(landing.durable_lsn) +
                                      (superblock_.single_stream()
                                           ? " (folded over " + std::to_string(per_core_.size()) +
                                                 " cores)"
                                           : ""));
    }

    if (Status s = store_.Sync(); !s.ok()) {
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("superblock", "anchor sync failed; on-disk anchor is still the "
                                      "previous one: " + s.message());
        }
        // The in-memory anchor now claims more than the platter does. That
        // is the safe direction - the *previous* anchor is what is on disk,
        // and recovery replaying from an older redo start costs time, never
        // correctness (wal.md section 11) - but the caller is told, because
        // a checkpoint whose anchor did not land bought nothing.
        return s;
    }

    ++publishes_;
    return Status::OK();
}

}  // namespace kds::server
