#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/assertion_check.hpp"
#include "kds/sched/clock.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/wal/checkpointer.hpp"
#include "kds/wal/log_device.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/recovery.hpp"

// Recovery at mount (RV1/RV2, `docs/spec/wal.md`): the caller
// `wal/recovery.hpp`'s driver leaves to the server layer (RC04a, RC11).
//
// ---- Once, for the instance's one stream ----------------------------------
//
// `Expeditor::Open` calls `RecoverCoreAtMount` once, as core 0, before any
// peer is built; one stream holds every core's records (AL-R5), so a peer
// runs no pass and reports `MountRecovery::ran == false`.
//
// ---- What this adds that the driver could not ----------------------------
//
// Two `server`-layer facts, and `wal/` sits below `server/`:
//
//   1. **The anchor becomes an `AnalysisStart`.** A zeroed slot is not an
//      error - no checkpoint was ever published, so the scan starts at the
//      head of the stream and the durable-point check is disabled
//      (`analysis.hpp`).
//   2. **The undo phase is installed.** `txn::RecoveryUndo` over the
//      caller's undo log and WAL manager. Not optional: `RecoverCore`
//      refuses a stream that has losers and no phase.
//
// The anchor is passed **in** rather than read from a `SuperBlock&` here,
// because the caller selects it: slot 0 of the mounted image, which a
// restore has already rewritten to the backup's start (BR-R2).
//
// ---- The two obligations this leaves the caller, by name -----------------
//
//   1. **Persist the transaction ceiling.** `txn::TrxIdSequence` **caches
//      `next_trx_id` at construction** (`txn/trx_id.hpp`), so the caller
//      owes `SuperBlock::SetNextTrxId` plus a persist *before* it builds the
//      sequence. Page 0's image and its persist are the caller's
//      (`Expeditor::Open`), so this function only reports the ceiling.
//   2. Every core allocates through the store's allocation floor, which
//      this pass raises past `page_floor` (RC04's obligation 1).

namespace kds::server {

// What one core's recovery did, for the caller's obligations above and for
// the mount log line. Counted rather than inferred: a phase that silently
// did nothing is the failure mode this whole plan is written against.
struct MountRecovery {
    // **Whether this core ran a pass at all** (AL-R5). False on a peer,
    // where core 0's pass covered every core's records - so the zeroes below
    // are not misread as "this mount found an empty log".
    bool ran = true;

    // ---- What the log held ----
    std::uint64_t records = 0;  // records analysis read
    // A torn tail is the *expected* shape of a crash, not a failure
    // (`log_scanner.hpp`); it is reported so the mount line can say so.
    bool torn_tail = false;
    std::uint64_t winners = 0;
    std::uint64_t aborted = 0;
    std::uint64_t losers = 0;

    // ---- What redo wrote ----
    std::uint64_t redo_applied = 0;
    std::uint64_t redo_skipped_by_lsn = 0;  // already on the page (RV5)
    // The PW1c-2 not-dirty filter's count, checkable at a real mount.
    std::uint64_t redo_skipped_not_dirty = 0;
    std::uint64_t pages_healed = 0;         // checksum detected, an FPI healed
    // Redo's start and, when its floor raised it (BC-R2), what analysis had
    // recomputed - `SHOW META` prints the pair only when they differ.
    wal::Lsn redo_start = 0;
    wal::Lsn redo_start_floored_from = 0;
    // Where the scan ended - the stream's end, before undo appended its
    // compensations. A dropped relation's tombstone this mount finds pending
    // is reclaimed once the durable redo start reaches it (BF-R4): every
    // record that names the relation's pages precedes the drop's commit,
    // and no compensation names them, since the drop's `X` waited out every
    // writer of the relation.
    wal::Lsn scan_end = 0;
    // A restore this mount completed (BR-R2): the backup it came from and
    // the stop its scan passed. Zero when the mount owed none. Set by the
    // mount, which refuses to open short of the stop, so a nonzero stop
    // here is a stop reached.
    std::uint64_t restore_backup_id = 0;
    wal::Lsn restore_redo_start = 0;
    wal::Lsn restore_stop_lsn = 0;

    // ---- What undo rolled back ----
    std::uint64_t transactions_rolled_back = 0;
    std::uint64_t compensations = 0;

    // ---- The caller's two obligations ----
    PageId page_floor = kInvalidPageId;
    bool page_floor_raised = false;
    std::uint64_t next_trx_id = 0;

    // ---- What it cost (RC09, `docs/spec/wal.md` §13) ----
    //
    // `timings.timed` is false when no clock was supplied, which is what keeps
    // four zeroes from reading as "instant" (`wal/recovery.hpp`).
    wal::RecoveryTimings timings;
    sched::MonoTimeNs checkpoint_ns = 0;  // RC08's completion checkpoint

    // ---- What it could not promise (RV3, RC09) ----
    //
    // Relations the catalog still describes whose pages the crash took with it
    // - the *detectable* half of the unlogged-DDL gap, filled by
    // `AuditCatalogAfterRecovery` and left at zero until it runs. The other
    // half is not detectable at all; the audit's own comment says why.
    //
    // **User relations only.** A bootstrap catalog relation has no `sys.tables`
    // row and lives on a fixed page, so it is neither checkable this way nor at
    // risk in the way a user relation is.
    std::uint32_t relations_checked = 0;
    std::uint32_t relations_missing_pages = 0;

    // Delete-marked catalog rows a previous mount left behind, retired
    // before the listener bound (DT10, `ddl-transactional.md` §5c).
    // Non-zero says this mount followed a crash or a shutdown that left a
    // transactional DROP's rows half-resolved; zero is the ordinary case
    // and the one a clean shutdown always produces.
    std::uint64_t catalog_marks_finalized = 0;

    // Var-heap slots this mount collected because no row pointed at them
    // (H7, `exec/varheap_sweep.hpp`). The leak it exists for is a
    // rolled-back `CREATE ASSERTION`'s spilled body text: `LogChainInsert`
    // logs its spills under `kNoTxnId`, so there is no transaction to chain
    // an undo record to and no compensation runs.
    //
    // **Not a leak count.** `varheap::PageRelease` is idempotent and cannot
    // tell a live slot it is killing from a tombstone it is rewriting, so
    // this counts *slots the sweep did not find a reference for*, which on
    // a relation that has rolled back before includes the ones an earlier
    // mount already collected. Non-zero is not a defect report.
    std::uint64_t varheap_slots_swept = 0;

    // ---- Assertion enforcement, resumed (RC07) ----
    //
    // `assertions_enforcing` is what `SHOW ASSERTIONS` will report `enforcing=1`
    // for; `assertions_unrecovered` is the honest remainder - a surviving
    // declaration whose directory could not be rebuilt, which stays out of the
    // registry rather than enforcing against zero.
    std::uint32_t assertions_enforcing = 0;
    std::uint32_t assertions_unrecovered = 0;

    // `assertions_foreign` retired (AT-S5d); its history is at cf889b63.

    // Nothing to recover: an unwritten log, or one whose whole range the
    // last clean shutdown's checkpoint already covers. The common mount,
    // and the one that must cost nothing.
    bool empty() const noexcept { return records == 0; }
};

// Runs analysis, redo, the high-water repair and undo against one core's
// stream, and returns what they did.
//
// Every failure is a refused mount (RV1), never a partial success: an
// anchor the stream cannot honestly reach, a record that will not apply to
// the page it names, a store that cannot raise its floor, a loser whose row
// has moved. `txn.md` §8's instruction is that half a recovery is worse
// than none, and the caller's contract is to propagate rather than log and
// continue.
//
// `wal` may be null only where the caller can promise no further crash -
// the shape socket-free tests use. A real mount installs one, because an
// unlogged compensation is a rollback the next recovery cannot see happened
// (`txn/recovery_undo.hpp`).
// `clock`, when given, times the phases into `MountRecovery::timings` (RC09).
//
StatusOr<MountRecovery> RecoverCoreAtMount(std::uint32_t core_id, const WalAnchorFields& anchor,
                                          wal::LogDevice& device, storage::PageStore& store,
                                          txn::UndoLog& undo_log, wal::WalManager* wal,
                                          Logger* log, const sched::Clock* clock = nullptr);

// RV3's honest counter - **the half of it that can be computed**.
//
// Cheap and exact: ask the catalog for each relation and try to open it, one page
// read per relation, no chain walks. That is what this does.
//
// The other half - rows whose relation the catalog lost - **cannot be computed
// cheaply**: `page.md` §2a's `owner_oid` resolves a page to its relation, but
// only by reading every allocated page, and a dropped relation's reclaim walks
// only what its tombstone's roots reach, which is not this count's question.
// Building the set means reading every page at every mount. So `SHOW META`
// reports this number and states the other case in words; RC09's task entry
// carries the full argument.
//
// Never fails the mount: an unreadable relation is what it is *reporting*, not
// an error it hit. Returns the two counts, writes one log line per finding.
MountRecovery AuditCatalogAfterRecovery(catalog::Catalog& catalog, storage::PageStore& store,
                                        MountRecovery report, Logger* log);

// Assertion enforcement, resumed (RC07's mount wiring, AS6a).
//
// `assertion.md` §7 promises enforcement "immediately at restart - no
// rebuild scan, no enforcement gap"; the registry is memory-resident, so
// this is what refills it.
//
// Three steps per surviving assertion, and each can fail on its own without
// costing the mount:
//
//   1. **Revive the shell** from the stored declaration
//      (`exec::ReviveAssertion`): §8.2 keeps `source_text` as the canon exactly
//      so the group columns can be recovered by re-parsing it.
//   2. **Refill the directory** through `exec::RecoverAssertions`: the group
//      headers from the last checkpoint's snapshot, the entry linkage from the
//      cabin's own pages, then the `ASSERT_*` records after the snapshot.
//   3. **Adopt only what came back whole.** An assertion the pass could not
//      base is left out of the registry, so it reports `enforcing=0` - which is
//      the honest answer and the one that was already true.
//
// **Adopting an unrecovered cabin would be the worst outcome available**, worse
// than not enforcing: a directory at zero admits every write, so the constraint
// would appear enforcing and enforce nothing. That is why the pass reports per
// assertion and why this refuses to adopt on anything less than success.
//
// ---- Whose assertions these are (AT-S5d) ----------------------------------
//
// **Every one the catalog declares, into the one registry `enforcer` is.**
// The registry is the instance's and a write on any core checks it, so the
// resume runs once - at core 0's mount, before any peer is built - and a
// peer handed the instance's registry does not call this (`CoreRuntime::Open`).
//
// A declaration whose cabin cannot be revived is recorded through
// `AssertionEnforcer::NoteUnenforceable`, which is what makes the write path
// refuse the relation's writes by name instead of admitting them unchecked.
//
// Called after `RecoverCoreAtMount` and before the listener binds. `from_lsn`
// is where the scan begins. `stream_core` is whose *log* is scanned, which the
// scanner validates every segment header against: 0, the one stream's.
MountRecovery ResumeAssertionsAfterRecovery(catalog::Catalog& catalog,
                                           storage::PageStore& store, wal::LogDevice& device,
                                           std::uint32_t stream_core, wal::Lsn from_lsn,
                                           exec::AssertionEnforcer& enforcer,
                                           MountRecovery report, Logger* log);

// Recovery's completion checkpoint (RC08, `docs/spec/wal.md` §12-4): the last step
// of a mount, and what bounds the *next* crash's work.
//
// ---- Why a mount without one is a mount that gets slower ------------------
//
// Recovery starts where the anchor says; without an anchor published after
// replaying, every mount would scan from the last periodic checkpoint, and a
// database that never completed one would scan its whole stream every time.
// The cost is one flush of the dirty table plus two records; the next
// recovery starts here instead.
//
// ---- Why the active-transaction table is empty, and why that is a fact ----
//
// `CHECKPOINT_BEGIN` carries the live transactions so recovery's undo phase
// can find its losers (RV10). Immediately after recovery there are none: every
// loser was rolled back and given its `TXN_ABORT`, every winner had committed
// before the crash, and no statement has been accepted yet because the listener
// is not bound (RV1). So `wal::NoActiveTransactions` is the *correct* table
// here and not a stand-in for one - which is why this function takes no
// transaction source and cannot be handed the wrong one.
//
// The caller supplies the target and the anchor because both are its layer's:
// a `DevicePageStore` knows its dirty table, and where an anchor becomes
// durable is the superblock owner's business (`superblock_checkpoint_anchor.hpp`).
//
// Fails with whatever the checkpoint reports, and a failure is the mount's
// (RV1): an anchor that was not published leaves the next recovery replaying
// more than it should, which is a promise silently withdrawn rather than a
// wrong answer - but it happens at mount, where there is still a caller to
// tell.
// `elapsed_ns`, when given, receives how long the checkpoint took (RC09).
//
// `assertions`, when given, makes this checkpoint carry AS6a's group snapshots -
// and a mount that has resumed enforcement **must** pass it. The completion
// checkpoint becomes the anchor, so the next mount folds from *this* record: a
// completion checkpoint written without the snapshots leaves that mount with no
// base, and enforcement would survive exactly one restart. That is why this
// call is sequenced after `ResumeAssertionsAfterRecovery` rather than beside the
// recovery it completes.
//
// `at_mount` (the default, and every server caller's): every dirty page is
// written before the run begins, so its CHECKPOINT_BEGIN carries an empty
// dirty table - a replay can dirty more pages than one record holds in the
// log's ring (BR-S10). Nothing else writes at a mount. The simulation's
// mid-run and shutdown checkpoints pass false, so its crash corpus still
// recovers from anchors whose redo start lies below their checkpoint.
Status CheckpointAfterRecovery(std::uint32_t core_id, wal::WalManager& wal,
                               wal::CheckpointTarget& target, wal::CheckpointAnchor& anchor,
                               Logger* log, const sched::Clock* clock = nullptr,
                               sched::MonoTimeNs* elapsed_ns = nullptr,
                               wal::AssertionSnapshotSource* assertions = nullptr,
                               bool at_mount = true);

}  // namespace kds::server
