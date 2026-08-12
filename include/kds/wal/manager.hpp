#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/sched/clock.hpp"
#include "kds/wal/durability.hpp"
#include "kds/wal/log_device.hpp"
#include "kds/wal/record.hpp"
#include "kds/wal/stream.hpp"
#include "kds/wal/writer.hpp"

// One core's WAL manager (wal.md sections 1, 3, 6): the layer that owns a
// WalStream and turns the durability classes into a policy about when that
// stream is synced.
//
// The stream below knows where bytes go; it has no opinion about when they
// must be on the platter. This layer holds that opinion and nothing else -
// the split is what keeps "D2 batches, D1 does not" from being smeared
// through the append path.
//
// Concurrency: core-local, like everything under it (rules.md section 3).
// One manager, one core, one stream, no synchronization.
//
// ---- The three classes (wal.md section 1) -------------------------------
//
//   D1 strict   Commit() syncs before returning. On return the commit
//               record is durable, so the caller may acknowledge
//               immediately.
//   D2 group    Commit() stages and returns. The commit becomes durable on
//               the next drain, which syncs once for every commit waiting
//               - that single sync is group commit. Same durability point
//               as D1; only the batching differs.
//   D3 relaxed  Commit() stages and returns, and nothing waits. The loss
//               window is bounded by `relaxed_flush_interval_ns`, enforced
//               by the drain.
//
// ---- Waiting, and who runs the drain ------------------------------------
//
// A D2 committer does not block here. It takes the commit LSN Commit()
// returned and waits for `IsDurable(lsn)` to go true; the scheduler has no
// future/promise primitive yet (sched.md section 3 leaves the task
// representation open), so the wait is a task that polls that predicate
// and returns kSuspended until it holds. Building that task is the
// transaction layer's job, not this one's - the manager exposes the
// predicate and stays out of the scheduler.
//
// What the manager does require is that **something calls DrainOnce()**: a
// `system`-group task, once per reactor iteration (wal.md section 6-2/6-3
// puts flushing and group-commit completion in that group). A D2 commit on
// a manager nobody drains never becomes durable, and its waiter never
// wakes.
//
// ---- What this layer does not do ----------------------------------------
//
// Checkpointing (section 11), recovery (section 12), and segment recycling
// are separate subsystems that use this one. Cross-core commit is [OPEN]
// (section 3) and deliberately not designed here: one manager owns one
// stream, and a transaction that spans cores is not representable.

namespace kds::wal {

// Engine-side spelling of wal.md section 1's D1/D2/D3. Deliberately not
// wire::DurabilityLevel: that enum has a kSessionDefault member, which is
// a session concern the server resolves before it ever reaches the engine,
// and the WAL must not depend on the wire layer. The numbering there
// mirrors these three.
enum class DurabilityClass {
    kStrict = 1,   // D1
    kGroup = 2,    // D2
    kRelaxed = 3,  // D3
};

const char* DurabilityClassName(DurabilityClass durability) noexcept;

// The inverse, over exactly the names DurabilityClassName() produces
// ("strict"/"group"/"relaxed", case-insensitively) plus the "d1"/"d2"/"d3"
// spellings wal.md section 1 uses. Anything else is InvalidArgument naming
// the accepted set - a config file that means D1 and silently gets D3 is
// the failure this refuses to have.
StatusOr<DurabilityClass> ParseDurabilityClass(std::string_view name);

struct WalManagerConfig {
    std::size_t ring_capacity = kDefaultRingCapacity;

    // Upper bound on the D3 loss window: the drain syncs once this much
    // time has passed since the last sync with bytes still unsynced. The
    // default is [OPEN] (wal.md section 15) - a parameter with a default,
    // never a constant anything may depend on. 0 means "sync on every
    // drain", which makes D3 lossless and pointless, but is a legal
    // setting and a useful one in tests.
    sched::MonoTimeNs relaxed_flush_interval_ns = 10'000'000;  // 10 ms
};

// Observability that wal.md section 13 wants shipped *with* the first
// flush path rather than added later: operators tune RPO/RTO with these,
// so they are product surface, not debug aids. The two LSNs are gauges on
// the manager itself (appended_lsn() / durable_lsn()); the gap between
// them is the current loss exposure.
struct WalStats {
    std::uint64_t records_appended = 0;
    std::uint64_t bytes_appended = 0;
    std::uint64_t flushes = 0;  // ring drains into the device, sync or not
    std::uint64_t syncs = 0;    // successful device syncs
    std::uint64_t sync_failures = 0;

    std::uint64_t strict_commits = 0;
    std::uint64_t group_commits = 0;    // D2 commits registered
    std::uint64_t group_batches = 0;    // syncs that resolved at least one
    std::uint64_t relaxed_commits = 0;
    std::uint64_t interval_syncs = 0;  // drains that fired on the D3 interval

    // Times an append found the ring full and had to drain it inline. The
    // stall metric of wal.md section 13 / test 8: nonzero means producers
    // are outrunning the device.
    std::uint64_t ring_full_drains = 0;

    // Mean records per group-commit sync, the number section 16-7 is
    // about. Zero when no batch has completed.
    double mean_group_batch_size() const noexcept {
        return group_batches == 0 ? 0.0
                                  : static_cast<double>(group_commits) /
                                        static_cast<double>(group_batches);
    }
};

class WalManager final : public WalDurability {
public:
    // Opens the manager and the stream under it on `device`, which must
    // outlive both. `clock` is the injected time source (rules.md section
    // 4) the D3 interval is measured against - never a std::chrono read.
    static StatusOr<std::unique_ptr<WalManager>> Open(LogDevice* device,
                                                      const sched::Clock& clock,
                                                      std::uint32_t core_id = 0,
                                                      WalManagerConfig config = {});

    // Optional diagnostics. Null by default so the WAL unit tests stay
    // free of one. Record-level lines are Trace: an append happens per
    // page mutation, so logging one at any lower threshold would put a
    // write() syscall on the engine's hottest path.
    void SetLogger(Logger* log) noexcept { log_ = log; }

    std::uint32_t core_id() const noexcept { return stream_->core_id(); }
    std::uint64_t segment_size() const noexcept { return stream_->segment_size(); }

    // Bytes a single record's payload may occupy, for a caller that has to chunk
    // its own data to fit one (AS6a's group snapshots are the first). The record
    // header is subtracted here rather than by every such caller, since a caller
    // that forgot would produce a record the append path refuses at the worst
    // possible moment - mid-checkpoint.
    std::size_t usable_payload_bytes() const noexcept {
        return static_cast<std::size_t>(stream_->usable_segment_bytes()) - kRecordHeaderSize;
    }
    const WalStats& stats() const noexcept { return stats_; }

    // Where the next record goes; every byte below it has been appended.
    Lsn appended_lsn() const noexcept { return stream_->append_lsn(); }
    Lsn flushed_lsn() const noexcept { return stream_->flushed_lsn(); }

    // WalDurability. The comparison against a record LSN is strict - see
    // durability.hpp.
    // **The writer thread's watermark, not the stream's** (wal/writer.hpp).
    // The stream stages bytes and writes them into the page cache; making
    // them durable belongs to the writer, so it owns the number that says
    // so. Without a writer - every in-process test, every tool - the stream
    // syncs on the caller's stack and its own watermark is the answer.
    // **The higher of the two watermarks**, because two things can make
    // bytes durable now: the reactor, for any sync a caller is waiting on,
    // and the writer thread, for D3's loss-window tick that nobody waits
    // on (DrainOnce). Both only ever move forwards, so the max is the
    // answer and neither can pull it back.
    Lsn durable_lsn() const noexcept override {
        const Lsn by_stream = stream_->durable_lsn();
        const Lsn by_writer = writer_ != nullptr ? writer_->durable_lsn() : 0;
        return by_stream > by_writer ? by_stream : by_writer;
    }

    // Starts the WAL writer thread, which from then on performs every sync.
    //
    // Off by default and started by the server, deliberately: a test or a
    // tool that drives a WalManager on one thread wants its syncs to have
    // happened by the time the call returns, and a thread would turn every
    // such assertion into a wait. The server starts one because its reactor
    // must not block on a device (bench/results-latency-matrix.md).
    void StartWriter();

    bool has_writer() const noexcept { return writer_ != nullptr; }
    Status EnsureDurable(Lsn lsn) override;

    // Appends one record and returns its LSN. A full ring is drained
    // inline rather than reported: every I/O path is still synchronous, so
    // the flush *is* the drain wal.md section 6-4 tells the appender to
    // wait for, and there is no reactor to suspend into. When the async
    // backend lands this becomes a real suspension and the signature does
    // not change. The stall is counted either way (stats().ring_full_drains).
    StatusOr<Lsn> Append(const RecordSpec& spec, std::span<const std::byte> payload = {});

    // Appends TXN_COMMIT and applies `durability`, returning the commit
    // record's LSN. Under kStrict the record is durable on return; under
    // kGroup the caller waits for IsDurable() on that LSN; under kRelaxed
    // nothing waits.
    StatusOr<Lsn> Commit(std::uint64_t txn_id, DurabilityClass durability);

    // Appends TXN_ABORT. No durability class and no wait: a transaction
    // whose abort record did not survive is a transaction with no commit
    // record, which recovery rolls back anyway (wal.md section 12-1). The
    // record exists to save recovery the work, not to make the abort true.
    StatusOr<Lsn> Abort(std::uint64_t txn_id);

    // True while at least one D2 commit is staged and unsynced - what the
    // drain looks at, and what a shutdown path must clear before it can
    // claim every acknowledged commit is safe.
    bool HasPendingGroupCommits() const noexcept { return pending_group_commits_ > 0; }

    // One tick of the `system`-group WAL housekeeping task. Syncs when
    // there are group commits waiting (one sync, whole batch) or when the
    // D3 interval has elapsed with bytes unsynced. Cheap and safe to call
    // on every reactor iteration; a tick with nothing pending does no I/O.
    Status DrainOnce();

    // Stages the ring into the device without syncing. Rarely what a
    // caller wants - durability comes from EnsureDurable()/DrainOnce() -
    // but the checkpointer needs to bound how much sits in the ring
    // without forcing a sync.
    Status Flush();

    // Makes every byte appended so far durable, whatever class put it
    // there and whatever the D3 interval says. DrainOnce() is the paced
    // version and deliberately declines to sync a relaxed commit before
    // its window is up; this is for the two moments where that pacing is
    // wrong - a clean shutdown and an explicit client SYNC, both of which
    // promise that everything acknowledged has landed.
    // **Blocking, and it has to be**: this is the one promise that
    // everything acknowledged has landed, so it may not return on a sync
    // that has merely been *asked for*. With a writer thread that means
    // waiting for the watermark; without one, Sync() already did the work
    // on this stack.
    Status SyncAll();

private:
    WalManager(std::unique_ptr<WalStream> stream, const sched::Clock& clock,
               WalManagerConfig config);

    Status Sync();

    std::unique_ptr<WalStream> stream_;

    // Null until StartWriter(). When set, it is the only thing that calls
    // the device's Sync(), and this manager never does.
    std::unique_ptr<WalWriter> writer_;
    const sched::Clock& clock_;
    WalManagerConfig config_;
    WalStats stats_;

    // The batch: how many D2 commits are staged, and the LSN of the last
    // of them. One sync past that LSN resolves all of them at once.
    std::uint64_t pending_group_commits_ = 0;
    Lsn highest_group_commit_lsn_ = 0;

    sched::MonoTimeNs last_sync_ns_ = 0;
    Logger* log_ = nullptr;
};

}  // namespace kds::wal
