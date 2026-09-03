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
// Concurrency: **one manager per core, over a stream that is either that
// core's or the instance's** (AR0 M0, `instructions/v3.0.0/workorder-al-m0-single-wal.md`
// AL-R1). Everything a manager holds itself - the statistics, the group
// batch, the parked request, the D3 clock - is its core's and is touched
// by no other thread. What crosses cores is below it: the shared stream's
// latch and watermark (stream.hpp) and the writer's atomics (writer.hpp).
// Two ways to construct one:
//
//   Open()    owns its stream (and, after StartWriter(), its writer). The
//             `cores = 1` path, every in-process test, and core 0 of a
//             shared instance, which syncs inline on its reactor as it
//             always has.
//   Attach()  borrows the instance's stream and writer. A peer core: it
//             appends and flushes through the latch, and every sync it
//             waits on is a request to the writer - it never touches the
//             device itself, which is what takes `fdatasync` off its
//             reactor (the case AL-2 makes).
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
// are separate subsystems that use this one. A cross-owner transaction is
// `docs/spec/cross-owner-txn.md`'s, not this layer's: a manager knows one
// core's commits, whichever stream they land in.

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

    // Open() arms the stream's latch so that Attach()ed managers on other
    // threads may append to it (stream.hpp). Off - the default, and every
    // `cores = 1` path - the stream is one thread's and pays nothing.
    bool shared_stream = false;
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

    // **The batch's spread, not just its mean.** `mean_group_batch_size()`
    // below answers "how much amortisation happened"; these two answer
    // "was it the same every time", which is the question a mean cannot.
    // Added 2026-09-02 to decide one specific reading: the batch measures
    // `n/2` at every session count (`bench/peer_group_batch_probe.py`), and
    // the explanation offered for it - that a session awaiting its reply
    // cannot stage, so half the population is always out of the running -
    // predicts a **tight** distribution at n/2. A mix of full and singleton
    // batches averaging the same number would mean something else entirely,
    // and the mean cannot tell those apart.
    //
    // `group_batch_min` is over **non-empty** batches, so it is never the 0
    // a sync with nothing staged would contribute; 0 means no batch has
    // completed, which is what `group_batches == 0` says too.
    std::uint64_t group_batch_max = 0;
    std::uint64_t group_batch_min = 0;
    std::uint64_t relaxed_commits = 0;
    std::uint64_t interval_syncs = 0;  // drains that fired on the D3 interval

    // Times an append found the ring full and had to drain it inline. The
    // stall metric of wal.md section 13 / test 8: nonzero means producers
    // are outrunning the device.
    //
    // **Counted per attempt, not per append**, because the ring is shared
    // and one append may drain more than once (AL-R1): a drain this core
    // performs can be consumed by another core before this one gets the
    // latch back. So the number is *stalls paid*, which is what a stall
    // metric should be, and it is above the number of appends that stalled.
    std::uint64_t ring_full_drains = 0;

    // Appends that exhausted `kRingDrainAttempts` and were refused
    // `OutOfSpace`. **A different event from the one above and a far worse
    // one**: a drain that worked cost the appender a synchronous flush, and
    // this is a statement that failed. It became reachable when AL-R1
    // replaced the single retry with a bound - under an unshared ring one
    // drain was a proof of progress - so nothing counted it before and a
    // shared ring is exactly where it can happen. Zero is the expected
    // reading at every load; nonzero is a sizing bug, not a slow device.
    std::uint64_t ring_full_refusals = 0;

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

    // A manager over a stream and a writer it does not own - a peer core's
    // view of the instance's one log (the concurrency section above).
    //
    // **The owner outlives every attached manager**, because both borrowed
    // pointers are the owner's: tear the peers down before core 0.
    // `stream` must have been opened shared, `writer` must be the one over
    // that stream's device, and both must outlive this. Neither may be
    // null: an attached manager never touches the device, so the writer is
    // its only route to durability. `config.ring_capacity` and
    // `config.shared_stream` are the stream's and are ignored here.
    static StatusOr<std::unique_ptr<WalManager>> Attach(WalStream* stream, WalWriter* writer,
                                                        const sched::Clock& clock,
                                                        std::uint32_t core_id,
                                                        WalManagerConfig config = {});

    // Optional diagnostics. Null by default so the WAL unit tests stay
    // free of one. Record-level lines are Trace: an append happens per
    // page mutation, so logging one at any lower threshold would put a
    // write() syscall on the engine's hottest path.
    void SetLogger(Logger* log) noexcept { log_ = log; }

    // The core this manager serves - not the stream's number, which is
    // 0 for the instance's shared stream whichever core appends (AL-R2).
    std::uint32_t core_id() const noexcept { return core_id_; }
    bool attached() const noexcept { return owned_stream_ == nullptr; }
    WalStream* stream() const noexcept { return stream_; }
    WalWriter* writer() const noexcept { return writer_; }
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

    // Starts the WAL writer thread, which from then on performs the syncs
    // **nobody is waiting on** - D3's loss-window tick in DrainOnce(), and
    // nothing else. Every waited-on sync stays on the reactor, by the
    // decision Sync() states: a parked committer, a client's SYNC and the
    // checkpoint gate all pay a hand-off if their sync is handed away.
    //
    // Off by default and started by the server, deliberately: a test or a
    // tool that drives a WalManager on one thread wants its syncs to have
    // happened by the time the call returns, and a thread would turn every
    // such assertion into a wait. The server starts one because the idle
    // tick must not block its reactor (bench/results-latency-matrix.md).
    // A no-op on a manager that already has a writer, attached or owned.
    void StartWriter();

    // The writer thread's own device syncs and failures; 0 where no writer
    // was started (XD0, `instructions/v2.7.1/measurement-xd.md`). The
    // other half of this core's device cost - `stats().syncs` is the rest.
    //
    // **0 on an attached manager, and that is the truthful per-core
    // answer**: the writer is the instance's, not this core's, and every
    // reader of these is per core (`SHOW META`, whose fields are summed by
    // nobody but would be wrong to sum if N cores each reported the same
    // shared count). The instance's number lives on the owner.
    std::uint64_t writer_syncs() const noexcept {
        return owned_writer_ != nullptr ? owned_writer_->syncs() : 0;
    }
    std::uint64_t writer_sync_failures() const noexcept {
        return owned_writer_ != nullptr ? owned_writer_->failures() : 0;
    }
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

    // **Someone is parked on `lsn` and no commit record covers it** (R6-3).
    // The next `DrainOnce()` syncs for it, the way it already syncs for a
    // staged group commit; this call itself does no I/O and never blocks,
    // which is what lets a reactor-side caller ask for durability and then
    // park on `IsDurable(lsn)` instead of holding the thread through an
    // `fdatasync`.
    //
    // The reason it has to exist: a record that is not a `Commit` leaves
    // the drain with nothing to sync *for*, so until D3's loss-window
    // interval elapses - 10 ms by default - nothing makes it durable. That
    // is a bounded wait rather than a hang, and it is still the wrong
    // answer for a cross-owner prepare, whose whole content is "this is on
    // the platter" and which the coordinator is parked on.
    //
    // Monotonic: a request behind one already pending is absorbed, so a
    // caller may ask without checking. `EnsureDurable`'s blocking sync
    // stays the answer for a caller with no reactor to park on.
    //
    // The flag beside the LSN is not redundant. A record's LSN is the
    // offset of its first byte and `IsDurable` is strict (durability.hpp),
    // so "not yet durable" includes `lsn == durable_lsn()` - and with no
    // flag, the initial state (0, 0) would read as a standing request and
    // sync every idle tick.
    void RequestDurable(Lsn lsn) noexcept {
        if (!durability_requested_ || lsn > requested_durable_lsn_) {
            requested_durable_lsn_ = lsn;
            durability_requested_ = true;
        }
    }

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
    WalManager(WalStream* stream, std::unique_ptr<WalStream> owned_stream, WalWriter* writer,
               const sched::Clock& clock, std::uint32_t core_id, WalManagerConfig config);

    // **Blocking**: on return every byte staged when the call began is on
    // the platter, or a failure says why. On an owning manager that is the
    // device sync, on this thread. On an attached one it is a flush, a
    // request to the writer, and a wait on the writer's watermark - the
    // same wait, one hand-off further away (AL-2 owes the measurement).
    // The three callers are the ones whose whole meaning is "wait": a D1
    // commit, `EnsureDurable`'s gate, and `SyncAll`.
    Status Sync();

    // **Non-blocking**: stages the bytes and asks for a sync, without
    // waiting for one. The drain's path on an attached manager, where a
    // tick may not hold the reactor. On an owning manager there is nobody
    // to ask, so this is `Sync()`.
    Status RequestSyncNow();

    // The batch and the parked request, closed against the durable
    // watermark. Called after every sync this manager performs and, on an
    // attached manager, on every drain tick - because there the watermark
    // moves on other threads' syncs, and a batch made durable by one of
    // them must still be counted and cleared here.
    void ResolveBatches() noexcept;

    // The stream: `owned_stream_` holds it after Open(), and is null after
    // Attach(), where `stream_` points at the instance's.
    WalStream* stream_;
    std::unique_ptr<WalStream> owned_stream_;

    // Null until StartWriter() on an owning manager; the instance's on an
    // attached one. On an owning manager it takes D3's loss-window tick
    // and nothing else - every waited-on sync stays on the reactor, in
    // Sync() (this comment claimed the opposite until XD0 read the code,
    // `instructions/v2.7.1/measurement-xd.md`). On an attached manager it
    // takes every sync, waited on or not.
    WalWriter* writer_ = nullptr;
    std::unique_ptr<WalWriter> owned_writer_;
    const sched::Clock& clock_;
    std::uint32_t core_id_;
    WalManagerConfig config_;
    WalStats stats_;

    // The batch: how many D2 commits are staged, and the LSN of the last
    // of them. One sync past that LSN resolves all of them at once.
    std::uint64_t pending_group_commits_ = 0;
    Lsn highest_group_commit_lsn_ = 0;

    // The highest LSN a caller has parked on that no *commit* record
    // covers (R6-3's prepare). Distinct from the batch above rather than
    // folded into it: `pending_group_commits_` is a count of D2 commits
    // and `stats_.group_batches` reads it as one, so incrementing it for a
    // record that commits nothing would make the batch statistics say
    // something untrue about the workload.
    Lsn requested_durable_lsn_ = 0;
    bool durability_requested_ = false;

    sched::MonoTimeNs last_sync_ns_ = 0;
    Logger* log_ = nullptr;
};

}  // namespace kds::wal
