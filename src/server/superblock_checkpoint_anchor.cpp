#include "kds/server/superblock_checkpoint_anchor.hpp"

namespace kds::server {

Status SuperBlockCheckpointAnchor::Publish(const wal::CheckpointAnchorRecord& anchor) {
    const WalAnchorFields fields{anchor.checkpoint_lsn, anchor.redo_start_lsn, anchor.durable_lsn,
                                 anchor.segment_no};
    if (Status s = superblock_.SetWalAnchor(anchor.core_id, fields); !s.ok()) {
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
                                      std::to_string(anchor.core_id) + ": redo_start=" +
                                      std::to_string(anchor.redo_start_lsn) + " durable_lsn=" +
                                      std::to_string(anchor.durable_lsn));
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
