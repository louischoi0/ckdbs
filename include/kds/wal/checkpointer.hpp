#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "kds/sched/checkpoint_census.hpp"
#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

// Fuzzy checkpointing (wal.md section 11), run as a `system`-group task
// per core since BI-S2: the cadence submits `RunTask`, which takes one call
// per poll and parks for every wait on the log or the data file (BI-R1,
// BI-R2); `RunToCompletion` stays for the synchronous callers.
//
// What a checkpoint buys is bounded recovery: it moves the **redo start**
// forward so restart does not have to replay the whole log. It does not
// quiesce anything - "fuzzy" means transactions keep running and pages
// keep being dirtied while it works, which is why the redo start comes
// from the dirty table snapshotted at CHECKPOINT_BEGIN rather than from
// where the checkpoint finished.
//
// ---- The sequence, and why the order is the whole thing ----------------
//
//   1. CHECKPOINT_BEGIN, carrying the active-transaction table and the
//      dirty-page table `{page_id -> recLSN}` (payload.hpp). Recovery's
//      analysis phase starts from exactly these two tables.
//   2. Flush every page in that snapshot, through the pool's flush path so
//      each one goes out under the WAL gate (wal.md section 8-1). Paced:
//      Step() flushes a bounded batch, so a checkpoint spreads across
//      reactor iterations instead of blocking the foreground for its whole
//      duration.
//   3. CHECKPOINT_END carrying the redo start, and only then - after the
//      end record is durable - publish the anchor. That is wal.md section
//      8-3's checkpoint honesty: the anchor must never point past work
//      that is not on disk, because recovery trusts it and will not look
//      before it.
//
// A crash anywhere in the middle costs nothing but the work: the previous
// anchor is still valid, and recovery replays from there.
//
// ---- Redo start --------------------------------------------------------
//
// min(recLSN) over the snapshot, and the CHECKPOINT_BEGIN LSN when the
// snapshot has no logged pages in it. Both are safe for pages dirtied
// *after* BEGIN, whose recLSNs are all greater than the BEGIN LSN by
// construction. Zero recLSNs (pages created but never logged - page_mgr's
// `DirtyPage::rec_lsn` documents them) are skipped, not min()ed in: zero
// means "nothing to replay", and treating it as an LSN would pin the redo
// start to the head of the log forever.
//
// ---- What is injected, and why -----------------------------------------
//
// The live set, the pages and the anchor come from seams, so this file
// depends on neither the transaction layer nor the storage layer nor the
// superblock: `txn::TransactionManager` is the ActiveTransactions,
// `server::SuperBlockCheckpointAnchor` the anchor, and the storage
// targets (`BufferPoolCheckpointTarget`, `PageStoreCheckpointTarget`) the
// CheckpointTarget.

namespace kds::wal {

// The live transaction set at the moment of the snapshot
// (`txn::TransactionManager`).
class ActiveTransactions {
public:
    virtual ~ActiveTransactions() = default;
    // Each live transaction and the head of its undo chain (RV10,
    // `payload.hpp`'s CheckpointActiveTxn). The head is what makes a
    // checkpoint enough for recovery's undo phase: without it, a loser's
    // writes are only findable in the WAL inside the replay range, which
    // does not always contain them.
    virtual std::vector<CheckpointActiveTxn> Snapshot() const = 0;
};

// No transactions, for a core that is only replaying or only doing
// unlogged work. Not a stub for a missing implementation - an empty active
// set is a correct answer, and recovery handles it.
class NoActiveTransactions final : public ActiveTransactions {
public:
    std::vector<CheckpointActiveTxn> Snapshot() const override { return {}; }
};

// One Bound Cabin's group headers, as the checkpointer asks for them (AS6a,
// RC07). A WAL-layer shape rather than `exec::BoundCabin`'s own, for the reason
// `CheckpointTarget` is a seam: this file must not depend on the assertion
// engine, and the assertion registry is what converts.
// One group as the seam hands it over. The key is **owned** rather than a span,
// so no implementation has to keep a parallel buffer alive across the call;
// the checkpointer views into these when it encodes.
struct AssertionSnapshotGroup {
    std::uint32_t group_id = 0;
    std::int64_t count = 0;
    std::int64_t sum = 0;
    std::string key;
};

struct AssertionCabinSnapshot {
    std::uint64_t assertion_id = 0;
    std::vector<AssertionSnapshotGroup> groups;
};

// Writes one cabin's group headers as `ASSERT_SNAPSHOT` records, chunked so no
// record outgrows `wal.usable_payload_bytes()` (`payload.hpp`). A cabin no run
// can carry - a group larger than a record, or more than
// `kMaxAssertSnapshotChunks` chunks - is refused `NotImplemented` before any
// chunk is written (`AssertionSnapshotFits`).
//
// **Two callers, and the second is why this is not private to the
// checkpointer.** A checkpoint writes every live cabin's base; `CREATE
// ASSERTION` writes the new cabin's base at its publish. Without that second
// call an assertion created after the last checkpoint has no base in any
// range - the mount cannot recover it, so it stays out of the registry, so the
// completion checkpoint (which snapshots the registry) cannot give it one
// either. `enforcing=0` would be **permanent** until DROP + CREATE, and with a
// long `checkpoint_interval_ms` that is every new assertion.
//
// A cabin with no groups still gets one record: it says "this assertion existed
// and had nothing", which is what lets a loader tell an empty cabin from an
// absent base - and those must not read alike, because the second means the
// fold has nothing to fold onto.
Status LogAssertionSnapshot(WalManager& wal, const AssertionCabinSnapshot& cabin);

// Whether one run of records of `record_budget` payload bytes can carry
// `cabin`: `LogAssertionSnapshot`'s refusals, without writing anything
// (AZ-S3). The registry asks it inside a checkpoint's hold.
Status AssertionSnapshotFits(const AssertionCabinSnapshot& cabin, std::size_t record_budget);

using SnapshotVisitor = std::function<Status(const std::vector<AssertionCabinSnapshot>&)>;

// Where the checkpoint gets those snapshots. Implemented by whoever owns the
// live assertions; absent (null) on a core that has none, in which case a
// checkpoint writes no ASSERT_SNAPSHOT records at all and costs nothing.
//
// **Why the checkpoint is the right place.** The snapshot is the base assertion
// replay folds onto, so it has to sit at a point recovery can find and ahead of
// every record folded onto it. The checkpoint is already both: the anchor names
// it, and `CHECKPOINT_BEGIN` is where the active-transaction table is taken for
// the same reason.
class AssertionSnapshotSource {
public:
    virtual ~AssertionSnapshotSource() = default;

    // One entry per live assertion, handed to `visit` with the directories
    // held still until it returns. `visit` appends the `ASSERT_SNAPSHOT`
    // records, and a snapshot is a base only if no `ASSERT_*` record lands
    // between the headers it carries and its own LSN - so the instance's
    // registry, which every core reserves into, holds its latch across the
    // call (AT-S5d, `exec/assertion_check.hpp`).
    //
    // `record_budget` is the checkpoint's `usable_payload_bytes()`. A cabin
    // no run of that budget can carry is the source's to deal with before
    // `visit` (AZ-S3): the registry fails it closed rather than failing the
    // checkpoint.
    virtual Status VisitSnapshots(std::size_t record_budget, const SnapshotVisitor& visit) = 0;
};

// The pages a checkpoint has to get on disk, and the way to do it. The
// storage layer implements it (`storage/page_store_checkpoint_target.hpp`,
// `storage/page_mgr/checkpoint_target.hpp`); the seam lets the crash tests
// drive a scripted one.
class CheckpointTarget {
public:
    virtual ~CheckpointTarget() = default;

    // {page_id -> recLSN} over everything dirty right now.
    virtual std::vector<CheckpointDirtyPage> DirtyTable() const = 0;

    // Writes these pages back under the WAL gate. Ids that are no longer
    // dirty or no longer resident are not an error - something else may
    // have flushed them since the snapshot.
    virtual Status FlushPages(std::span<const PageId> page_ids) = 0;

    // ---- The run task's halves of `FlushPages` (BI-S3, BI-S4) -----------
    //
    // `WritePages` writes and does not sync, returning the pages written;
    // with `defer`, where the log is not yet durable past a page's record
    // the LSN goes to `*defer` and nothing from that page on is written
    // (BI-Q4 (a)). `RequestSync` asks for a sync of everything written so
    // far and returns a ticket, 0 for nothing to wait on; `SyncDone` and
    // `SyncResult` answer it. The defaults keep a scripted target whole: its
    // `FlushPages` is the write and the sync, it never defers, and it hands
    // out no ticket.
    virtual StatusOr<std::size_t> WritePages(std::span<const PageId> page_ids, Lsn* /*defer*/) {
        if (Status s = FlushPages(page_ids); !s.ok()) return s;
        return std::size_t{0};
    }
    virtual std::uint64_t RequestSync() { return 0; }
    virtual bool SyncDone(std::uint64_t /*ticket*/) { return true; }
    virtual Status SyncResult(std::uint64_t /*ticket*/) { return Status::OK(); }

    // The paced publish (BI-Q5 (a)): every page dirty now, for the task to
    // write in batches, and the maps after all of them, in two halves the
    // task parks between on the ticket the first answers (`page.md`
    // section 5, the map write order). A scripted target has neither.
    virtual std::vector<PageId> DirtyPages() { return {}; }
    virtual StatusOr<std::uint64_t> WriteHeaderlessMaps() { return std::uint64_t{0}; }
    virtual Status WriteFreeMaps() { return Status::OK(); }
};

// What a completed checkpoint publishes. `redo_start_lsn` is where
// recovery begins; the other two are what wal.md section 14-3 asks the
// superblock to anchor per core.
struct CheckpointAnchorRecord {
    std::uint32_t core_id = 0;
    Lsn checkpoint_lsn = 0;   // the CHECKPOINT_BEGIN record
    Lsn redo_start_lsn = 0;
    Lsn durable_lsn = 0;      // stream's durable end when the anchor was published
    // Segment holding redo_start_lsn. Derivable from the LSN and the
    // segment size (a format constant, wal.md section 4.1), and recorded so
    // the anchor names the file recovery must open before it has a device.
    std::uint64_t segment_no = 0;
};

class CheckpointAnchor {
public:
    virtual ~CheckpointAnchor() = default;
    // Called only after CHECKPOINT_END is durable (wal.md section 8-3).
    virtual Status Publish(const CheckpointAnchorRecord& anchor) = 0;

    // **The publish in halves, for the run task** (BI-Q5 (a)). `Encode`
    // records `anchor` and dirties the page that carries it, with no I/O,
    // and names that page (`kInvalidPageId`: none to write); the task writes
    // and syncs it, parked, and then `Commit`s what follows a synced
    // publish. The default is `Publish` whole, for an anchor with no page.
    virtual StatusOr<PageId> Encode(const CheckpointAnchorRecord& anchor) {
        if (Status s = Publish(anchor); !s.ok()) return s;
        return kInvalidPageId;
    }
    virtual void Commit() {}
};

// Holds the last anchor in memory. Correct only for a process that never
// restarts - the tests' anchor; the server's is `SuperBlockCheckpointAnchor`.
class InMemoryCheckpointAnchor final : public CheckpointAnchor {
public:
    Status Publish(const CheckpointAnchorRecord& anchor) override {
        anchor_ = anchor;
        ++publishes_;
        return Status::OK();
    }

    const CheckpointAnchorRecord& anchor() const noexcept { return anchor_; }
    std::uint64_t publishes() const noexcept { return publishes_; }

private:
    CheckpointAnchorRecord anchor_{};
    std::uint64_t publishes_ = 0;
};

// **At most one checkpoint runs in the instance** (AT-S8). Every core keeps
// its own checkpointer, because each core's active-transaction table is its
// own and unlatched and recovery needs every live transaction's undo head in
// some `CHECKPOINT_BEGIN` inside the replay range; what the instance shares
// is the pool they all walk and the anchor they all publish into. So the run
// is the instance's and the table is the core's: a cadence tick that finds
// another core's checkpoint running skips, and its next tick tries again.
// A skip costs a later redo start for that core's contribution to the fold,
// never an answer. The instance owns one; a fixture that builds none runs
// ungated.
//
// **The gate rotates since BI-S2** (BI-Q7 (a)). A first-come gate let the
// same cores win every period once a run outlasted `period / cores`: the
// stagger puts one tick per slot, a long run swallows the next core's tick,
// and those cores skipped for good while the fold held their old anchors and
// `D` stopped. A cadence attempt names its core and the time; the gate admits
// it only if, among the cores that asked within the last period, it is the
// one whose last *entry* is oldest. Keyed on entry rather than completion,
// so a core whose runs fail does not hold the rest back; and on recent asks,
// so a core that stopped asking does not either. An attempt that names no
// core - shutdown's, mount's, a test's - is first-come, as before.
class CheckpointGate {
public:
    static constexpr std::uint32_t kAnyCore = ~std::uint32_t{0};
    static constexpr std::uint32_t kCores = CheckpointCensus::kSlots;

    // One attempt at the run, released at scope end if it was taken. A null
    // gate always enters. Acquire on entry, so a run that follows another
    // sees every write-back the previous one made.
    class Hold {
    public:
        explicit Hold(CheckpointGate* gate) noexcept : Hold(gate, kAnyCore, 0) {}
        // A cadence attempt by `core` at `now`, rotated when the gate is.
        Hold(CheckpointGate* gate, std::uint32_t core, std::uint64_t now) noexcept
            : gate_(gate), entered_(gate == nullptr || gate->TryEnter(core, now)) {}
        ~Hold() {
            if (entered_ && gate_ != nullptr) gate_->running_.store(false, std::memory_order_release);
        }
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;

        bool entered() const noexcept { return entered_; }

    private:
        CheckpointGate* gate_;
        bool entered_;
    };

    // Rotates the cadence's attempts over a window of `period_ns`; 0, the
    // default, leaves the gate first-come. Set once, before any reactor runs.
    void SetRotation(std::uint64_t period_ns) noexcept { period_ns_ = period_ns; }

    // Attempts that found the run held, or another core's turn, over the
    // process. A test's evidence that two cores met the gate; nothing else
    // reads it.
    std::uint64_t skipped() const noexcept { return skipped_.load(std::memory_order_relaxed); }

private:
    bool TryEnter(std::uint32_t core, std::uint64_t now) noexcept {
        const bool rotated = core < kCores && period_ns_ != 0;
        // `now + 1`: an injected clock may read 0, and 0 is "never".
        if (rotated) last_ask_[core].store(now + 1, std::memory_order_relaxed);
        if ((rotated && !Turn(core, now)) || running_.exchange(true, std::memory_order_acquire)) {
            skipped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (rotated) last_entry_[core].store(now + 1, std::memory_order_relaxed);
        return true;
    }

    // Whether `core` is the asking core with the oldest entry. Advisory and
    // relaxed: two cores reading at once can both think it their turn, and
    // `running_` still lets one in.
    bool Turn(std::uint32_t core, std::uint64_t now) const noexcept {
        std::uint32_t chosen = core;
        std::uint64_t oldest = last_entry_[core].load(std::memory_order_relaxed);
        for (std::uint32_t c = 0; c < kCores; ++c) {
            const std::uint64_t asked = last_ask_[c].load(std::memory_order_relaxed);
            // An ask stamped after `now` - another core read the clock later
            // - is recent, not one that wrapped to "stale".
            if (asked == 0 || (asked <= now + 1 && now + 1 - asked > period_ns_)) continue;
            const std::uint64_t entry = last_entry_[c].load(std::memory_order_relaxed);
            if (entry < oldest || (entry == oldest && c < chosen)) {
                chosen = c;
                oldest = entry;
            }
        }
        return chosen == core;
    }

    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> skipped_{0};
    std::uint64_t period_ns_ = 0;
    std::array<std::atomic<std::uint64_t>, kCores> last_ask_{};
    std::array<std::atomic<std::uint64_t>, kCores> last_entry_{};
};

struct CheckpointerConfig {
    // Pages flushed per Step(), and per batch of the paced publish. The
    // pacing knob of wal.md section 11-2: one step is one poll of the run
    // task (BI-R1), so small enough that a poll does not monopolize a
    // reactor iteration, large enough that the per-batch barrier amortizes.
    // BI-S1 measured a step's page writes at 0.2-4 ms at p90 (BI-Q9-R). An
    // SLO-feedback controller would replace it; none exists, so this is a
    // fixed batch size, deliberately a parameter and not a constant.
    std::size_t pages_per_step = 64;
};

struct CheckpointStats {
    std::uint64_t started = 0;
    std::uint64_t completed = 0;
    std::uint64_t pages_flushed = 0;
    std::uint64_t steps = 0;
};

class Checkpointer {
public:
    // All four references must outlive the checkpointer.
    Checkpointer(WalManager& wal, CheckpointTarget& target, ActiveTransactions& txns,
                 CheckpointAnchor& anchor, CheckpointerConfig config = {});

    // Diagnostic log, null (discard) by default; `log` must outlive the
    // checkpointer. This is the subsystem log.hpp names as the reason it
    // exists: a checkpoint runs on a timer with no caller to return a
    // Status to, so without a line here a failing one is invisible until
    // recovery needs the anchor it never published.
    void SetLogger(Logger* log) noexcept { log_ = log; }

    // AS6a's snapshot source (RC07), null by default: a core with no assertions
    // writes no ASSERT_SNAPSHOT records. `source` must outlive this.
    void SetAssertionSource(AssertionSnapshotSource* source) noexcept {
        assertions_ = source;
    }

    bool in_progress() const noexcept { return in_progress_; }
    const CheckpointStats& stats() const noexcept { return stats_; }

    // The redo start the last completed checkpoint established, and the
    // BEGIN record it came from. Both 0 before the first one completes.
    Lsn redo_start_lsn() const noexcept { return redo_start_lsn_; }
    Lsn last_checkpoint_lsn() const noexcept { return last_checkpoint_lsn_; }

    // Logs CHECKPOINT_BEGIN and takes the snapshot. Fails with
    // AlreadyExists if one is already running - checkpoints do not nest,
    // and a second one would publish an anchor built from the first one's
    // half-flushed state.
    Status Start();

    // Flushes up to `pages_per_step` of the snapshot. Returns true once
    // every page in it has been written back, at which point Complete()
    // is the next call. A failed step leaves the checkpoint in progress
    // and its remaining pages unflushed, so the next Step() retries them.
    // The run task's step writes and does not sync (`write_only`): the
    // publish's sync covers it (BI-Q13). With `defer`, a step that needs the
    // log further stores the LSN there and returns false with its cursor
    // unmoved.
    StatusOr<bool> Step(Lsn* defer = nullptr, bool write_only = false);

    // Logs CHECKPOINT_END, makes it durable, and publishes the anchor -
    // in that order. Fails with OutOfRange if the flush phase has not
    // finished, since an anchor published over unflushed pages is exactly
    // the lie wal.md section 8-3 forbids. `AppendEnd` and `PublishAnchor`
    // are its halves, so the task can park between them (BI-R2).
    Status Complete();

    // Start() + Step() until done + Complete(), for callers that do not
    // need the checkpoint spread across reactor iterations: the mount's
    // completion checkpoint, shutdown's, and tests. The cadence's is
    // `SubmitRun`'s task.
    Status RunToCompletion();

    // **One synchronous attempt at `gate`** (null runs ungated), then
    // `RunToCompletion`, with the run record and log lines a cadence run
    // has. First-come at the gate. Shutdown's and the tests' caller since
    // BI-S2; the cadence submits `RunTask` instead. A skip returns OK.
    // `core_id` names the core in the log lines. Not fatal and it disarms
    // nothing: pages a failed run did not flush stay dirty and the next run
    // retries them.
    Status RunGated(CheckpointGate* gate, std::uint32_t core_id);

    // ---- The cadence's run, as a task (BI-S2, BI-R1) --------------------
    //
    // **The cadence callback only submits.** A timer callback runs inline in
    // the reactor's phase 2 (`scheduler.hpp`), so a run there held its core
    // for its whole length, charged to no group. `SubmitRun` puts one
    // `system`-group task on `scheduler` unless one is already in flight on
    // this checkpointer, and returns. The task takes the gate, runs `Start`,
    // then one `Step` per poll with a yield between, then `Complete`.
    //
    // **The gate's hold lives here, never in the task's frame** (BI-R4): a
    // frame parked on a stopped reactor is not resumed and is destroyed only
    // with its scheduler, after the shutdown checkpoints - so a hold in it
    // would make every core's shutdown checkpoint skip. `Abandon` releases
    // it. Across a yield the task holds nothing else (BI-R3): every page,
    // latch and claim is taken and given back inside one synchronous call.
    void SubmitRun(sched::Scheduler& scheduler, CheckpointGate* gate, std::uint32_t core_id);

    // A run task is queued or parked on this checkpointer.
    bool run_in_flight() const noexcept { return task_in_flight_; }

    // **Shutdown's first act, for every core, before any shutdown checkpoint**
    // (BI-R4): releases a parked run's gate hold and drops its snapshot and
    // cursor, logging nothing - what a crash mid-run leaves. The shutdown
    // checkpoint then starts fresh, so its redo start is its own
    // `CHECKPOINT_BEGIN`; resuming the parked snapshot would publish an old
    // recLSN under a new `checkpoint_lsn`, the record shape the assertion
    // floor's defect needs (`known-gaps.md`). The parked frame, if any, is
    // never resumed. Called with the core's reactor stopped and joined.
    void Abandon() noexcept;

private:
    Status LogBegin(std::span<const CheckpointActiveTxn> active_txns,
                    std::span<const CheckpointDirtyPage> dirty_pages);

    // AS6a: every live cabin's group headers, chunked so no record outgrows
    // `usable_payload_bytes()`. Emitted immediately after CHECKPOINT_BEGIN, which is what puts
    // the base ahead of every record folded onto it.
    Status LogAssertionSnapshots();

    // Complete's halves: the end record appended (its LSN), and the anchor
    // published once that record is durable - the caller's to ensure.
    StatusOr<Lsn> AppendEnd();
    Status PublishAnchor();
    // The record this run's publish carries, and the bookkeeping and log
    // line of a publish that landed.
    CheckpointAnchorRecord AnchorRecord() const;
    void Published(const CheckpointAnchorRecord& anchor);

    // BI-S1's run record: `BeginRun` points `run_report_` at `report` and
    // marks the run's edge; `EndRun` publishes it to the census and BA's
    // tallies and logs its Info line.
    void BeginRun(std::uint32_t core_id, CheckpointRunReport& report);
    void EndRun(std::uint32_t core_id, const Status& ran);
    void LogRun(std::uint32_t core_id, const CheckpointRunReport& report) const;

    // The run `RunGated` is timing, or null: `Start`, `Step` and `Complete`
    // install it as their thread's phase sink for their own length (BI-S1,
    // `sched/checkpoint_census.hpp`), so a direct caller - mount, a test -
    // records nothing.
    CheckpointRunReport* run_report_ = nullptr;

    // The run task's body, and what it keeps outside its frame (BI-R4).
    sched::Coro RunTask(CheckpointGate* gate, std::uint32_t core_id, std::uint64_t generation);
    Status FinishTask(std::uint32_t core_id, const Status& ran);
    // Parks on the log for one step, at most, before the step's own inline
    // gate takes what is left (BI-R2's bound). After the snapshot's one park
    // (BI-Q13), each is a page of the batch changed since `CHECKPOINT_BEGIN`. A bound, not a tuned number: the
    // remainder is counted (`log_gate_batch_n`, `log_gate_run_n`) and BI-S5
    // reports it.
    static constexpr int kGateParks = 16;
    // Parks until the log is durable past `lsn`, registered with the
    // writer's kick; the log's fail-stop answers it.
    sched::Coro ParkOnLog(Lsn lsn, std::uint32_t core_id);
    // Parks until the data file is synced past everything written so far
    // (BI-R2's second half). No kick: the data-sync thread has none, and the
    // drain timer ends an idle block (BI-Q6 (b)). `ParkOnDataSyncTicket`
    // parks on a ticket the target already issued.
    sched::Coro ParkOnDataSync();
    sched::Coro ParkOnDataSyncTicket(std::uint64_t ticket);
    // Writes `pages` for the publish, deferring on the log and parking as a
    // step does (BI-Q5 (a)).
    sched::Coro WriteForPublish(std::span<const PageId> pages, std::uint32_t core_id,
                                std::uint64_t generation);
    void LogSkip(std::uint32_t core_id) const;
    bool task_in_flight_ = false;
    // Moved by `Abandon`, so a frame of an abandoned run that is ever polled
    // returns without touching this checkpointer.
    std::uint64_t task_generation_ = 0;
    std::uint32_t task_core_ = 0;
    std::optional<CheckpointGate::Hold> task_hold_;
    CheckpointRunReport task_report_;

    WalManager& wal_;
    CheckpointTarget& target_;
    ActiveTransactions& txns_;
    CheckpointAnchor& anchor_;
    CheckpointerConfig config_;
    Logger* log_ = nullptr;
    AssertionSnapshotSource* assertions_ = nullptr;  // AS6a, may be null
    CheckpointStats stats_;

    bool in_progress_ = false;
    Lsn begin_lsn_ = 0;
    Lsn pending_redo_start_ = 0;
    std::vector<PageId> pending_pages_;  // snapshot pages not yet flushed
    std::size_t next_page_ = 0;

    Lsn redo_start_lsn_ = 0;
    Lsn last_checkpoint_lsn_ = 0;
    std::vector<std::byte> payload_scratch_;
};

}  // namespace kds::wal
