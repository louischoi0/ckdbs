#include "kds/wal/manager.hpp"

#include <cctype>
#include <string>
#include <utility>

namespace kds::wal {
namespace {

// How many times an append drains a full ring before reporting it. One is
// enough on an unshared stream (nothing else can refill it); above one is
// for the shared stream, where another core can take the space this drain
// just made. Small, because past a couple of attempts the ring is not
// momentarily full - it is undersized for the write rate, which is
// AL-S8's `wal_ring_full` cell and not something to spin through.
constexpr int kRingDrainAttempts = 4;

// A checkpoint record names its core in one byte (`payload.hpp`), so a
// manager serving a core the byte cannot hold could not log an honest
// checkpoint. Refused where the id enters the WAL layer rather than at the
// two append sites: a mount that cannot work should fail at the door, not
// degrade into a core that logs an error every checkpoint interval
// forever. Unreachable through the server, which bounds `cores` at
// `kMaxWalCores` long before here.
Status CheckLoggableCoreId(std::uint32_t core_id) noexcept {
    if (core_id > 0xFF) {
        return Status::InvalidArgument(
            "WalManager: core id " + std::to_string(core_id) +
            " does not fit the byte a checkpoint record names its core in");
    }
    return Status::OK();
}

}  // namespace

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

WalManager::WalManager(WalStream* stream, std::unique_ptr<WalStream> owned_stream,
                       WalWriter* writer, const sched::Clock& clock, std::uint32_t core_id,
                       WalManagerConfig config)
    : stream_(stream),
      owned_stream_(std::move(owned_stream)),
      writer_(writer),
      clock_(clock),
      core_id_(core_id),
      config_(config),
      last_sync_ns_(clock.Now()) {}

StatusOr<std::unique_ptr<WalManager>> WalManager::Open(LogDevice* device,
                                                       const sched::Clock& clock,
                                                       std::uint32_t core_id,
                                                       WalManagerConfig config) {
    if (Status s = CheckLoggableCoreId(core_id); !s.ok()) {
        return s;
    }
    auto stream = WalStream::Open(device, core_id, config.ring_capacity, config.shared_stream);
    if (!stream.ok()) {
        return stream.status();
    }
    WalStream* borrowed = stream.value().get();
    return std::unique_ptr<WalManager>(new WalManager(borrowed, std::move(stream.value()),
                                                      /*writer=*/nullptr, clock, core_id, config));
}

StatusOr<std::unique_ptr<WalManager>> WalManager::Attach(WalStream* stream, WalWriter* writer,
                                                         const sched::Clock& clock,
                                                         std::uint32_t core_id,
                                                         WalManagerConfig config) {
    if (Status s = CheckLoggableCoreId(core_id); !s.ok()) {
        return s;
    }
    if (stream == nullptr) {
        return Status::InvalidArgument("WalManager::Attach: stream must not be null");
    }
    if (!stream->shared()) {
        // An unshared stream has no latch, so a second thread appending to
        // it would race - refused here rather than corrupting the log.
        return Status::InvalidArgument(
            "WalManager::Attach: the stream was not opened shared; core " +
            std::to_string(core_id) + " cannot append to it");
    }
    if (writer == nullptr) {
        // An attached manager never touches the device: the writer is its
        // only way to make anything durable, so one is required rather
        // than a sync path that fails later on a core with a parked
        // committer.
        return Status::InvalidArgument("WalManager::Attach: core " + std::to_string(core_id) +
                                       " needs the instance's writer to make anything durable");
    }
    if (writer->device() != stream->device()) {
        // A writer over a different device would sync bytes nobody is
        // waiting for while the wait never ends. Refused rather than
        // documented, since the caller cannot see it go wrong.
        return Status::InvalidArgument(
            "WalManager::Attach: the writer syncs a different device than the stream writes; "
            "core " +
            std::to_string(core_id) + " would wait on a durability point that never arrives");
    }
    return std::unique_ptr<WalManager>(new WalManager(stream, /*owned_stream=*/nullptr, writer,
                                                      clock, core_id, config));
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
    // syncing the device itself on an owning manager and by waiting on the
    // writer's watermark on an attached one - the two watermarks are
    // merged by durable_lsn() either way.
    return Sync();
}

void WalManager::StartWriter() {
    if (writer_ != nullptr) return;
    owned_writer_ = std::make_unique<WalWriter>(stream_->device());
    writer_ = owned_writer_.get();
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("wal", "writer thread started; syncs leave the reactor from here on");
    }
}

Status WalManager::Sync() {
    // **On the calling thread on an owning manager, always.** The writer
    // thread is not used there and that is the decision, not an omission:
    // every caller of this has someone waiting on the result - a parked
    // committer, a client's SYNC, a checkpoint's gate - and handing such a
    // sync to another thread adds a wake-up to a latency somebody is
    // measuring. Measured on a 2-core host it doubled `group`'s p99 while
    // barely moving its median, which is what an occasional slow wake-up
    // looks like (bench/results-latency-matrix.md).
    //
    // What the writer *does* take on an owning manager is the sync nobody
    // waits for - D3's loss-window tick in DrainOnce(). See there.
    //
    // **An attached manager is the other case, and it is the point of M0**
    // (AL-R1). A peer core does not own the device and must not `fdatasync`
    // on its reactor: it flushes its bytes through the latch and asks the
    // writer.
    //
    // **And then it waits**, because this function's three callers are the
    // ones whose whole meaning is "wait" (manager.hpp): a D1 commit, whose
    // contract is that the record is durable on return; `EnsureDurable`,
    // which is the WAL-before-data gate the page store calls before
    // writing a dirty page (`storage/device_page_store.cpp`, wal.md §8-1);
    // and `SyncAll`, which promises everything acknowledged has landed.
    // Returning on a sync merely *asked for* would break all three, and
    // the third of those is not a latency bug but a durability one - a
    // data page reaching the platter ahead of its log record.
    //
    // The wait is a condition variable rather than an `fdatasync` on this
    // thread, so what a peer pays here is one hand-off; the sync itself
    // left its reactor, which is the 94-98% AL-2 is about. The
    // non-blocking path is `RequestSyncNow()`, which the drain takes.
    if (attached()) {
        const bool staged = stream_->ring_used() > 0;
        if (Status s = stream_->Flush(); !s.ok()) {
            ++stats_.sync_failures;
            return s;
        }
        if (staged) ++stats_.flushes;
        const Lsn target = stream_->flushed_lsn();
        if (Status s = writer_->EnsureDurable(target); !s.ok()) {
            ++stats_.sync_failures;
            if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                log_->Error("wal", "sync failed on the instance writer: " + s.message());
            }
            return s;
        }
        last_sync_ns_ = clock_.Now();
        ResolveBatches();
        return Status::OK();
    }

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

    ResolveBatches();
    return Status::OK();
}

Status WalManager::RequestSyncNow() {
    if (writer_ == nullptr) {
        // Nobody to ask - an owning manager that never started a writer,
        // which is every in-process test and tool. Its sync is its own, on
        // this thread, exactly as it was before M0.
        return Sync();
    }
    const bool staged = stream_->ring_used() > 0;
    if (Status s = stream_->Flush(); !s.ok()) {
        ++stats_.sync_failures;
        return s;
    }
    // Counted here rather than by the caller, so every path through this
    // function reports its flush. `DrainOnce`'s D3 branch therefore does
    // not count one of its own.
    if (staged) ++stats_.flushes;
    writer_->RequestSync(stream_->flushed_lsn());
    last_sync_ns_ = clock_.Now();
    // The batch is not resolved here: nothing is durable yet. A later
    // drain tick closes it, at `DrainOnce`'s opening `ResolveBatches`.
    return Status::OK();
}

void WalManager::ResolveBatches() noexcept {
    // R6-3's parked non-committer: the request is cleared once the record
    // it named is on the platter, so an idle drain goes back to doing
    // nothing.
    if (durability_requested_ && IsDurable(requested_durable_lsn_)) {
        durability_requested_ = false;
        requested_durable_lsn_ = 0;
    }

    // Group commit: one sync past the last staged commit record resolves
    // every commit in the batch, which is the whole mechanism. **Which
    // thread performed that sync is not this bookkeeping's business** -
    // under a shared stream another core's sync, or the writer's, covers
    // this core's records too, and the batch is counted here when the
    // watermark passes it (the drain calls this every tick for exactly
    // that case).
    if (pending_group_commits_ > 0 && IsDurable(highest_group_commit_lsn_)) {
        ++stats_.group_batches;
        // The spread, recorded before the count is cleared (manager.hpp's
        // `group_batch_max` says which question it answers). `min` is
        // seeded on the first batch rather than initialised to a sentinel,
        // so "no batch yet" stays 0 and is not confusable with a batch of
        // that size.
        if (pending_group_commits_ > stats_.group_batch_max) {
            stats_.group_batch_max = pending_group_commits_;
        }
        if (stats_.group_batch_min == 0 || pending_group_commits_ < stats_.group_batch_min) {
            stats_.group_batch_min = pending_group_commits_;
        }
        pending_group_commits_ = 0;
        highest_group_commit_lsn_ = 0;
    }
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
    // wal.md section 6-4's backpressure. Synchronous I/O means the drain
    // happens right here, on the appender's own stack.
    //
    // **Bounded rather than a single retry, because the stream may be
    // shared** (AL-R1): the drain this core performs can be consumed by
    // another core's append before this one gets the latch back, so one
    // retry is no longer a proof of progress. The bound turns a pathology
    // into a reported `OutOfSpace` - the same status a caller already
    // handles - instead of a spin on the reactor.
    for (int attempt = 0; attempt < kRingDrainAttempts && !lsn.ok() &&
                          lsn.status().code() == StatusCode::kOutOfSpace;
         ++attempt) {
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
    // **First, close what somebody else's sync already made durable.**
    // Under a shared stream this core's commit records can reach the
    // platter on another core's sync or the writer's, and the batch
    // bookkeeping is this manager's alone - without this the count would
    // stay open until this core happened to sync for itself.
    ResolveBatches();

    if (appended_lsn() == durable_lsn()) {
        return Status::OK();  // nothing to do; a tick must be free
    }
    if (pending_group_commits_ > 0 ||
        (durability_requested_ && !IsDurable(requested_durable_lsn_))) {
        // Someone is parked on this. On an owning manager it is performed
        // here, on the reactor, for the reason Sync() gives: a waiter pays
        // for a hand-off. The second disjunct is R6-3's prepare - a record
        // nobody committed and somebody is waiting on (RequestDurable).
        //
        // **On an attached manager it is asked for and not waited on.**
        // The waiter is a parked task polling `IsDurable`, not this tick,
        // and a drain that blocked would hold the whole reactor - every
        // other session on this core - for another thread's `fdatasync`.
        // The batch closes on a later tick, at the `ResolveBatches` above.
        return attached() ? RequestSyncNow() : Sync();
    }

    // No commit is waiting, so the only thing forcing a sync is the D3
    // loss-window bound - and **nobody is waiting for it**. That is what
    // makes it the writer thread's: the fsync leaves this thread entirely,
    // and the statement that would have been charged for it keeps running.
    // Measured: `relaxed`'s p99 falls from 2,208 us to 194 us, because that
    // stall *was* this tick. `RequestSyncNow` is that hand-off, and its
    // no-writer arm is the inline sync a writerless manager still owes.
    if (clock_.Now() - last_sync_ns_ >= config_.relaxed_flush_interval_ns) {
        // No flush counting here: `RequestSyncNow` counts its own on both
        // arms (the writerless one through `Sync`).
        if (Status s = RequestSyncNow(); !s.ok()) {
            return s;
        }
        ++stats_.interval_syncs;
    }
    return Status::OK();
}

}  // namespace kds::wal
