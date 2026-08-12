#include "kds/wal/checkpointer.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "kds/wal/analysis.hpp"

namespace kds::wal {

Checkpointer::Checkpointer(WalManager& wal, CheckpointTarget& target, ActiveTransactions& txns,
                           CheckpointAnchor& anchor, CheckpointerConfig config)
    : wal_(wal), target_(target), txns_(txns), anchor_(anchor), config_(config) {}

Status Checkpointer::LogBegin(std::span<const CheckpointActiveTxn> active_txns,
                              std::span<const CheckpointDirtyPage> dirty_pages) {
    payload_scratch_.assign(CheckpointBeginSize(active_txns.size(), dirty_pages.size()),
                            std::byte{0});
    auto encoded = EncodeCheckpointBegin(payload_scratch_, active_txns, dirty_pages);
    if (!encoded.ok()) {
        return encoded.status();
    }

    auto lsn = wal_.Append({RecordType::kCheckpointBegin, kNoTxnId, kInvalidPageId, 0},
                           std::span(payload_scratch_).first(encoded.value()));
    if (!lsn.ok()) {
        return lsn.status();
    }
    begin_lsn_ = lsn.value();
    return Status::OK();
}

Status LogAssertionSnapshot(WalManager& wal, const AssertionCabinSnapshot& cabin) {
    // The chunk bound: what one record's payload may hold. A cabin's group count
    // is bounded by the data, so a cabin can need several records - the loader is
    // additive over them (payload.hpp), which is why no continuation flag exists.
    const std::size_t budget = wal.usable_payload_bytes();
    std::vector<std::byte> scratch;

    std::size_t at = 0;
    // A cabin with no groups still gets one record - see the header.
    do {
        std::vector<SnapshotGroupEntry> chunk;  // views into the owned keys
        std::size_t bytes = kAssertSnapshotFixedSize;
        while (at < cabin.groups.size()) {
            const std::size_t cost = AssertSnapshotGroupBytes(cabin.groups[at].key.size());
            if (!chunk.empty() && bytes + cost > budget) {
                break;
            }
            if (bytes + cost > budget) {
                // One group too large for an entire record. Refused rather than
                // dropped: a base missing a group under-counts, and an admission
                // check built on it would admit a write that violates the
                // assertion.
                return Status::OutOfSpace(
                    "assertion snapshot: assertion " + std::to_string(cabin.assertion_id) +
                    " has a group key of " + std::to_string(cabin.groups[at].key.size()) +
                    " bytes, which no ASSERT_SNAPSHOT record can carry");
            }
            bytes += cost;
            const AssertionSnapshotGroup& group = cabin.groups[at];
            SnapshotGroupEntry entry;
            entry.group_id = group.group_id;
            entry.count = group.count;
            entry.sum = group.sum;
            entry.key = std::as_bytes(std::span<const char>(group.key.data(), group.key.size()));
            chunk.push_back(entry);
            ++at;
        }

        scratch.assign(bytes, std::byte{0});
        AssertSnapshotPayload fields{};
        fields.assertion_id = cabin.assertion_id;
        auto encoded = EncodeAssertSnapshot(scratch, fields, chunk);
        if (!encoded.ok()) {
            return encoded.status();
        }
        auto lsn = wal.Append({RecordType::kAssertSnapshot, kNoTxnId, kInvalidPageId, 0},
                              std::span(scratch).first(encoded.value()));
        if (!lsn.ok()) {
            return lsn.status();
        }
    } while (at < cabin.groups.size());
    return Status::OK();
}

Status Checkpointer::LogAssertionSnapshots() {
    if (assertions_ == nullptr) {
        return Status::OK();  // no assertions on this core: no records, no cost
    }
    for (const AssertionCabinSnapshot& cabin : assertions_->SnapshotAssertions()) {
        if (Status s = LogAssertionSnapshot(wal_, cabin); !s.ok()) {
            return s;
        }
    }
    return Status::OK();
}

Status Checkpointer::Start() {
    if (in_progress_) {
        return Status::AlreadyExists("checkpointer: a checkpoint is already in progress");
    }

    // Both tables are snapshotted before the record is written, so what
    // BEGIN carries is exactly what recovery's analysis phase will see.
    const std::vector<CheckpointActiveTxn> active_txns = txns_.Snapshot();
    const std::vector<CheckpointDirtyPage> dirty = target_.DirtyTable();

    if (Status s = LogBegin(active_txns, dirty); !s.ok()) {
        return s;
    }

    // AS6a's base, immediately after BEGIN: ahead of every ASSERT_* record that
    // will be folded onto it, and inside the checkpoint the anchor names, which
    // is what lets replay find it without scanning to the cabin's birth.
    if (Status s = LogAssertionSnapshots(); !s.ok()) {
        return s;
    }

    // Redo start: the oldest LSN any page in the snapshot still needs
    // replayed. Zero recLSNs are pages that are dirty but that no record
    // describes (created, never logged) - "nothing to replay", not "replay
    // from the head of the log", so they are skipped rather than min()ed
    // in. With no logged page in the snapshot, nothing before this
    // checkpoint matters and the redo start is the checkpoint itself.
    //
    // Pages dirtied *after* this point need no special handling: their
    // recLSNs are necessarily above begin_lsn_, which is itself an upper
    // bound on what this computes.
    // §11-3's rule lives in RedoStartFrom (wal/analysis.hpp), shared with
    // recovery's analysis phase, which recomputes the same number backward
    // from the table it rebuilds. Two copies of "skip a recLSN of 0" is two
    // chances to lose the skip, and losing it drags the redo start to zero
    // and replays the whole stream.
    pending_redo_start_ = RedoStartFrom(begin_lsn_, dirty);
    pending_pages_.clear();
    pending_pages_.reserve(dirty.size());
    for (const CheckpointDirtyPage& page : dirty) {
        pending_pages_.push_back(page.page_id);
    }

    next_page_ = 0;
    in_progress_ = true;
    ++stats_.started;
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("checkpoint", "started: begin_lsn=" + std::to_string(begin_lsn_) +
                                      " dirty_pages=" + std::to_string(pending_pages_.size()) +
                                      " active_txns=" + std::to_string(active_txns.size()) +
                                      " redo_start=" + std::to_string(pending_redo_start_));
    }
    return Status::OK();
}

StatusOr<bool> Checkpointer::Step() {
    if (!in_progress_) {
        return Status::OutOfRange("checkpointer: no checkpoint in progress");
    }
    ++stats_.steps;
    if (next_page_ >= pending_pages_.size()) {
        return true;
    }

    const std::size_t batch =
        std::min(config_.pages_per_step, pending_pages_.size() - next_page_);
    const std::span<const PageId> pages(pending_pages_.data() + next_page_, batch);
    if (Status s = target_.FlushPages(pages); !s.ok()) {
        // The cursor does not move: these pages are still the
        // checkpoint's responsibility, and the next Step() retries them.
        // Logged because the retry makes this survivable and therefore
        // silent - a checkpoint that fails every step forever looks
        // exactly like one that is merely slow.
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("checkpoint", "flush of " + std::to_string(batch) +
                                          " page(s) failed at offset " +
                                          std::to_string(next_page_) + ", will retry: " +
                                          s.message());
        }
        return s;
    }
    next_page_ += batch;
    stats_.pages_flushed += batch;
    if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
        log_->Trace("checkpoint", "flushed " + std::to_string(batch) + " page(s), " +
                                      std::to_string(pending_pages_.size() - next_page_) +
                                      " remaining");
    }
    return next_page_ >= pending_pages_.size();
}

Status Checkpointer::Complete() {
    if (!in_progress_) {
        return Status::OutOfRange("checkpointer: no checkpoint in progress");
    }
    if (next_page_ < pending_pages_.size()) {
        // wal.md section 8-3: CHECKPOINT_END is written only once every
        // page in the table has been flushed. Writing it now would leave
        // an anchor recovery trusts pointing past pages that are not on
        // disk.
        return Status::OutOfRange(
            "checkpointer: " + std::to_string(pending_pages_.size() - next_page_) +
            " pages of the checkpoint's dirty table are still unflushed");
    }

    payload_scratch_.assign(kCheckpointEndPayloadSize, std::byte{0});
    const CheckpointEndPayload fields{pending_redo_start_};
    auto encoded = EncodeCheckpointEnd(payload_scratch_, fields);
    if (!encoded.ok()) {
        return encoded.status();
    }
    auto end_lsn = wal_.Append({RecordType::kCheckpointEnd, kNoTxnId, kInvalidPageId, 0},
                               std::span(payload_scratch_).first(encoded.value()));
    if (!end_lsn.ok()) {
        return end_lsn.status();
    }

    // The end record has to be on disk before anything points at it: an
    // anchor naming a redo start that a crash then erases is worse than no
    // checkpoint at all, because recovery would skip records it still
    // needs.
    if (Status s = wal_.EnsureDurable(end_lsn.value()); !s.ok()) {
        return s;
    }

    const CheckpointAnchorRecord anchor{wal_.core_id(),
                                        begin_lsn_,
                                        pending_redo_start_,
                                        wal_.durable_lsn(),
                                        pending_redo_start_ / wal_.segment_size()};
    if (Status s = anchor_.Publish(anchor); !s.ok()) {
        // The end record is durable but nothing points at it: recovery
        // will replay from the previous anchor, which is correct and
        // slower. Worth an Error - the checkpoint bought nothing.
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("checkpoint", "anchor publish failed after a durable CHECKPOINT_END: " +
                                          s.message());
        }
        return s;
    }
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("checkpoint", "anchor published: core=" + std::to_string(anchor.core_id) +
                                     " checkpoint_lsn=" + std::to_string(anchor.checkpoint_lsn) +
                                     " redo_start=" + std::to_string(anchor.redo_start_lsn) +
                                     " durable_lsn=" + std::to_string(anchor.durable_lsn) +
                                     " segment=" + std::to_string(anchor.segment_no));
    }

    redo_start_lsn_ = pending_redo_start_;
    last_checkpoint_lsn_ = begin_lsn_;
    in_progress_ = false;
    pending_pages_.clear();
    next_page_ = 0;
    ++stats_.completed;
    return Status::OK();
}

Status Checkpointer::RunToCompletion() {
    if (Status s = Start(); !s.ok()) {
        return s;
    }
    while (true) {
        auto done = Step();
        if (!done.ok()) {
            return done.status();
        }
        if (done.value()) {
            break;
        }
    }
    return Complete();
}

}  // namespace kds::wal
