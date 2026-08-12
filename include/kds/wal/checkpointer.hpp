#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

// Fuzzy checkpointing (wal.md section 11), run as a `system`-group task
// per core.
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
// Both tables come from seams rather than concrete types. The transaction
// manager does not exist yet, so ActiveTransactions is how the checkpoint
// asks for the live set; and the superblock's per-core WAL anchor is a
// *required but unlanded* spec amendment (wal.md section 14-3), so the
// anchor is a seam with an in-memory implementation instead of a
// superblock write this file would have to invent a format for. When the
// amendment lands, a superblock-backed CheckpointAnchor drops in with no
// change here.

namespace kds::wal {

// The live transaction set at the moment of the snapshot. Implemented by
// the transaction manager when it exists.
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
// One group as the seam hands it over. The key is **owned** rather than a span:
// a directory's keys live in its own headers, and every implementation of the
// seam would otherwise have to keep a parallel buffer alive across the call -
// which the first one written did, with a `mutable` vector, before this changed.
// The checkpointer views into these when it encodes.
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
// record outgrows a segment (`payload.hpp`).
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

    // One entry per live assertion. Called on the checkpointer's thread while
    // nothing else touches the directories (core-local, §6.1).
    virtual std::vector<AssertionCabinSnapshot> SnapshotAssertions() const = 0;
};

// The pages a checkpoint has to get on disk, and the way to do it. The
// per-core BufferPool is the real implementation (BufferPoolCheckpointTarget
// below); the seam keeps this file from depending on the storage layer and
// lets the crash tests drive a scripted one.
class CheckpointTarget {
public:
    virtual ~CheckpointTarget() = default;

    // {page_id -> recLSN} over everything dirty right now.
    virtual std::vector<CheckpointDirtyPage> DirtyTable() const = 0;

    // Writes these pages back under the WAL gate. Ids that are no longer
    // dirty or no longer resident are not an error - something else may
    // have flushed them since the snapshot.
    virtual Status FlushPages(std::span<const PageId> page_ids) = 0;
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
    // device's segment size, and recorded anyway so the anchor names the
    // file recovery must open before it has a device to ask - the segment
    // size is [OPEN] (wal.md section 15) and may differ from this build's
    // default in a database written by another one.
    std::uint64_t segment_no = 0;
};

class CheckpointAnchor {
public:
    virtual ~CheckpointAnchor() = default;
    // Called only after CHECKPOINT_END is durable (wal.md section 8-3).
    virtual Status Publish(const CheckpointAnchorRecord& anchor) = 0;
};

// Holds the last anchor in memory. Correct only for a process that never
// restarts - it is what the tests use, and what runs until the superblock
// amendment (wal.md section 14-3) lands.
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

struct CheckpointerConfig {
    // Pages flushed per Step(). The pacing knob of wal.md section 11-2:
    // small enough that a checkpoint does not monopolize a reactor
    // iteration, large enough that the per-batch barrier amortizes. The
    // real thing is SLO-feedback-driven (page.md section 13's checkpoint
    // spreading), which needs the controller that does not exist yet - so
    // this is a fixed batch size, deliberately a parameter and not a
    // constant.
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
    void SetAssertionSource(const AssertionSnapshotSource* source) noexcept {
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
    StatusOr<bool> Step();

    // Logs CHECKPOINT_END, makes it durable, and publishes the anchor -
    // in that order. Fails with OutOfRange if the flush phase has not
    // finished, since an anchor published over unflushed pages is exactly
    // the lie wal.md section 8-3 forbids.
    Status Complete();

    // Start() + Step() until done + Complete(), for callers that do not
    // need the checkpoint spread across reactor iterations (tests, and
    // shutdown).
    Status RunToCompletion();

private:
    Status LogBegin(std::span<const CheckpointActiveTxn> active_txns,
                    std::span<const CheckpointDirtyPage> dirty_pages);

    // AS6a: every live cabin's group headers, chunked so no record outgrows a
    // segment. Emitted immediately after CHECKPOINT_BEGIN, which is what puts
    // the base ahead of every record folded onto it.
    Status LogAssertionSnapshots();

    WalManager& wal_;
    CheckpointTarget& target_;
    ActiveTransactions& txns_;
    CheckpointAnchor& anchor_;
    CheckpointerConfig config_;
    Logger* log_ = nullptr;
    const AssertionSnapshotSource* assertions_ = nullptr;  // AS6a, may be null
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
