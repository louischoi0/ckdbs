#include "kds/server/superblock_checkpoint_anchor.hpp"

#include <vector>

namespace kds::server {

wal::CheckpointAnchorRecord SuperBlockCheckpointAnchor::MountAnchorOf(
    const SuperBlock& superblock) noexcept {
    // The lowest **populated** slot, not slot 0 alone (the header says
    // why) - and populated is the load-bearing word. A zero
    // `redo_start_lsn` is not an anchor at 0, it is a core that has never
    // completed a checkpoint (`superblock.hpp`), and under one stream
    // slots 1..63 are never written at all. Counting those zeros as
    // candidates would make the minimum 0 on every mount, which would
    // hold the warm-up at "replay the whole log" *and* write that zero
    // over the real anchor.
    const std::vector<WalAnchorFields> slots = superblock.wal_anchors();
    const WalAnchorFields* lowest = nullptr;
    for (const WalAnchorFields& slot : slots) {
        if (slot.redo_start_lsn == 0) continue;  // never published
        if (lowest == nullptr || slot.redo_start_lsn < lowest->redo_start_lsn) {
            lowest = &slot;
        }
    }

    wal::CheckpointAnchorRecord record{};
    // `core_id` is left 0 and means nothing here: the mount anchor is the
    // volume's, not a core's, and nothing reads this field off the fold.
    if (lowest != nullptr) {
        record.checkpoint_lsn = lowest->checkpoint_lsn;
        record.redo_start_lsn = lowest->redo_start_lsn;
        record.durable_lsn = lowest->durable_lsn;
        record.segment_no = lowest->segment_no;
    }
    return record;
}

wal::CheckpointAnchorRecord SuperBlockCheckpointAnchor::FoldedAnchor() const noexcept {
    // **The warm-up** (the header's second fold section): a core that has
    // not published in this process lifetime contributes nothing to a
    // minimum, so until every core has published the anchor is held where
    // the mount found it. `popcount` asks which cores, not how many
    // entries - the two differ if an id were ever out of range.
    if (folded_cores() < superblock_.core_count()) {
        return mount_anchor_;
    }

    // The minimum by redo start, carrying that core's whole set of numbers
    // rather than a field-wise minimum - four numbers from four cores would
    // describe a checkpoint that never happened, and `segment_no` is the
    // proof: it is `redo_start_lsn / segment_size` *of the same core*, so a
    // field-wise minimum could name a segment that does not hold the LSN.
    const wal::CheckpointAnchorRecord* lowest = nullptr;
    for (std::uint32_t core = 0; core < kMaxWalCores; ++core) {
        if ((published_ & (std::uint64_t{1} << core)) == 0) continue;
        const wal::CheckpointAnchorRecord& record = per_core_[core];
        if (lowest == nullptr || record.redo_start_lsn < lowest->redo_start_lsn) {
            lowest = &record;
        }
    }
    // Unreachable: `CheckCoreCount` refuses 0, so passing the gate above
    // means at least one bit is set. Answered rather than dereferenced.
    return lowest == nullptr ? mount_anchor_ : *lowest;
}

Status SuperBlockCheckpointAnchor::Publish(const wal::CheckpointAnchorRecord& anchor) {
    // Which slot, and what goes in it, is the volume's topology's answer
    // (the header's fold section).
    std::uint32_t slot = anchor.core_id;
    wal::CheckpointAnchorRecord landing = anchor;
    if (superblock_.single_stream()) {
        if (anchor.core_id >= superblock_.core_count()) {
            // Refused rather than recorded: an out-of-range id would set a
            // bit no `core_count` accounts for, releasing the warm-up early
            // and putting a phantom core's number into the minimum. The
            // ring path memcpys this id out of a peer's payload
            // (`expeditor.cpp`), so it is the one field here that does not
            // come from a caller this object can see.
            return Status::InvalidArgument(
                "superblock: checkpoint anchor names core " + std::to_string(anchor.core_id) +
                ", which this database's core count (" +
                std::to_string(superblock_.core_count()) + ") does not have");
        }
        per_core_[anchor.core_id] = anchor;
        published_ |= std::uint64_t{1} << anchor.core_id;
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
                                      std::to_string(anchor.core_id) + ": redo_start=" +
                                      std::to_string(landing.redo_start_lsn) + " durable_lsn=" +
                                      std::to_string(landing.durable_lsn) +
                                      (superblock_.single_stream()
                                           ? " (folded over " + std::to_string(folded_cores()) +
                                                 " of " + std::to_string(superblock_.core_count()) +
                                                 " cores)"
                                           : " into slot " + std::to_string(slot)));
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
