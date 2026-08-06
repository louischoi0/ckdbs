#include "kds/wal/manager.hpp"

#include <cctype>
#include <string>
#include <utility>

namespace kds::wal {

const char* DurabilityClassName(DurabilityClass durability) noexcept {
    switch (durability) {
        case DurabilityClass::kStrict:
            return "strict";
        case DurabilityClass::kGroup:
            return "group";
        case DurabilityClass::kRelaxed:
            return "relaxed";
    }
    return "unknown";
}

StatusOr<DurabilityClass> ParseDurabilityClass(std::string_view name) {
    std::string lowered;
    lowered.reserve(name.size());
    for (const char c : name) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (lowered == "strict" || lowered == "d1") return DurabilityClass::kStrict;
    if (lowered == "group" || lowered == "d2") return DurabilityClass::kGroup;
    if (lowered == "relaxed" || lowered == "d3") return DurabilityClass::kRelaxed;
    return Status::InvalidArgument("unknown durability class '" + std::string(name) +
                                   "'; expected strict|d1, group|d2 or relaxed|d3");
}

WalManager::WalManager(std::unique_ptr<WalStream> stream, const sched::Clock& clock,
                       WalManagerConfig config)
    : stream_(std::move(stream)),
      clock_(clock),
      config_(config),
      last_sync_ns_(clock.Now()) {}

StatusOr<std::unique_ptr<WalManager>> WalManager::Open(LogDevice* device,
                                                       const sched::Clock& clock,
                                                       std::uint32_t core_id,
                                                       WalManagerConfig config) {
    auto stream = WalStream::Open(device, core_id, config.ring_capacity);
    if (!stream.ok()) {
        return stream.status();
    }
    return std::unique_ptr<WalManager>(
        new WalManager(std::move(stream.value()), clock, config));
}

Status WalManager::Flush() {
    // The stream no-ops an empty flush; only count the ones that moved
    // bytes, or the metric stops meaning anything.
    if (stream_->ring_used() == 0) {
        return Status::OK();
    }
    if (Status s = stream_->Flush(); !s.ok()) {
        return s;
    }
    ++stats_.flushes;
    return Status::OK();
}

Status WalManager::SyncAll() {
    // The reactor's, like every other waited-on sync. It also has to cover
    // whatever the writer thread has in flight, which Sync() does by
    // syncing the device itself - the two watermarks are merged by
    // durable_lsn().
    return Sync();
}

void WalManager::StartWriter() {
    if (writer_ != nullptr) return;
    writer_ = std::make_unique<WalWriter>(stream_->device());
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("wal", "writer thread started; syncs leave the reactor from here on");
    }
}

Status WalManager::Sync() {
    // **On the calling thread, always.** The writer thread is not used here
    // and that is the decision, not an omission: every caller of this has
    // someone waiting on the result - a parked committer, a client's SYNC,
    // a checkpoint's gate - and handing such a sync to another thread adds
    // a wake-up to a latency somebody is measuring. Measured on a 2-core
    // host it doubled `group`'s p99 while barely moving its median, which is
    // what an occasional slow wake-up looks like
    // (bench/results-latency-matrix.md).
    //
    // What the writer *does* take is the sync nobody waits for - D3's
    // loss-window tick in DrainOnce(). See there.
    const bool had_staged_bytes = stream_->ring_used() > 0;
    if (Status s = stream_->Sync(); !s.ok()) {
        // Sync() flushes first, so the failure could be either half. The
        // stream leaves the bytes staged and the durable point where it
        // was; nothing here may pretend otherwise, least of all the batch
        // bookkeeping - those commits are still waiting.
        ++stats_.sync_failures;
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("wal", "sync failed: " + s.message());
        }
        return s;
    }
    if (had_staged_bytes) {
        ++stats_.flushes;
    }
    ++stats_.syncs;
    last_sync_ns_ = clock_.Now();

    // Debug rather than Trace: a sync is a durability point, and there is
    // one per group-commit batch rather than one per record.
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("wal", "sync durable_lsn=" + std::to_string(durable_lsn()) +
                               " appended_lsn=" + std::to_string(appended_lsn()) +
                               " pending_group_commits=" +
                               std::to_string(pending_group_commits_));
    }

    // Group commit: one sync past the last staged commit record resolves
    // every commit in the batch, which is the whole mechanism.
    if (pending_group_commits_ > 0 && IsDurable(highest_group_commit_lsn_)) {
        ++stats_.group_batches;
        pending_group_commits_ = 0;
        highest_group_commit_lsn_ = 0;
    }
    return Status::OK();
}

Status WalManager::EnsureDurable(Lsn lsn) {
    if (IsDurable(lsn)) {
        return Status::OK();
    }
    if (lsn >= appended_lsn()) {
        // Nothing was ever appended at this LSN, so no amount of syncing
        // will make it durable. A page claiming a page_lsn the log never
        // issued is a corrupt page, not a slow one.
        return Status::InvalidArgument("WalManager: lsn " + std::to_string(lsn) +
                                       " is at or past the append point " +
                                       std::to_string(appended_lsn()) +
                                       "; no such record was logged");
    }
    return Sync();
}

StatusOr<Lsn> WalManager::Append(const RecordSpec& spec, std::span<const std::byte> payload) {
    auto lsn = stream_->Append(spec, payload);
    if (!lsn.ok() && lsn.status().code() == StatusCode::kOutOfSpace) {
        // wal.md section 6-4's backpressure. Synchronous I/O means the
        // drain happens right here, on the appender's own stack.
        ++stats_.ring_full_drains;
        if (Status s = Flush(); !s.ok()) {
            return s;
        }
        lsn = stream_->Append(spec, payload);
    }
    if (!lsn.ok()) {
        return lsn.status();
    }
    ++stats_.records_appended;
    stats_.bytes_appended += EncodedRecordSize(payload.size());

    // One line per WAL record. Trace only: an append accompanies every
    // logged page mutation, so this is a per-mutation syscall when enabled.
    if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
        log_->Trace("wal", std::string("append ") + RecordTypeName(spec.type) + " lsn=" +
                               std::to_string(lsn.value()) + " txn=" +
                               std::to_string(spec.txn_id) + " page=" +
                               std::to_string(spec.page_id) + " bytes=" +
                               std::to_string(EncodedRecordSize(payload.size())));
    }
    return lsn.value();
}

StatusOr<Lsn> WalManager::Commit(std::uint64_t txn_id, DurabilityClass durability) {
    auto lsn = Append({RecordType::kTxnCommit, txn_id, kInvalidPageId, 0});
    if (!lsn.ok()) {
        return lsn.status();
    }

    switch (durability) {
        case DurabilityClass::kStrict:
            // D1 and D2 share a durability point; only the batching
            // differs, so this is the same sync the drain would do - just
            // now, on this task's stack, instead of at the next tick.
            if (Status s = Sync(); !s.ok()) {
                return s;
            }
            ++stats_.strict_commits;
            break;

        case DurabilityClass::kGroup:
            ++pending_group_commits_;
            highest_group_commit_lsn_ = lsn.value();
            ++stats_.group_commits;
            break;

        case DurabilityClass::kRelaxed:
            // Acknowledged as soon as it is in the ring; the drain bounds
            // how long it can stay there.
            ++stats_.relaxed_commits;
            break;
    }
    return lsn.value();
}

StatusOr<Lsn> WalManager::Abort(std::uint64_t txn_id) {
    return Append({RecordType::kTxnAbort, txn_id, kInvalidPageId, 0});
}

Status WalManager::DrainOnce() {
    if (appended_lsn() == durable_lsn()) {
        return Status::OK();  // nothing to do; a tick must be free
    }
    if (pending_group_commits_ > 0) {
        // Someone is parked on this. Performed here, on the reactor, for
        // the reason Sync() gives: a waiter pays for a hand-off.
        return Sync();
    }

    // No commit is waiting, so the only thing forcing a sync is the D3
    // loss-window bound - and **nobody is waiting for it**. That is what
    // makes it the writer thread's: the fsync leaves this thread entirely,
    // and the statement that would have been charged for it keeps running.
    // Measured: `relaxed`'s p99 falls from 2,208 us to 194 us, because that
    // stall *was* this tick.
    if (clock_.Now() - last_sync_ns_ >= config_.relaxed_flush_interval_ns) {
        if (writer_ != nullptr) {
            const bool staged = stream_->ring_used() > 0;
            if (Status s = stream_->Flush(); !s.ok()) {
                ++stats_.sync_failures;
                return s;
            }
            if (staged) ++stats_.flushes;
            last_sync_ns_ = clock_.Now();
            writer_->RequestSync(stream_->flushed_lsn());
            ++stats_.interval_syncs;
            return Status::OK();
        }
        if (Status s = Sync(); !s.ok()) {
            return s;
        }
        ++stats_.interval_syncs;
    }
    return Status::OK();
}

}  // namespace kds::wal
