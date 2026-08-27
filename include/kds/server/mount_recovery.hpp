#pragma once

#include <cstdint>

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

// Recovery at mount (docs/workplan-wal-recovery.md RV1/RV2) - the caller
// `wal/recovery.hpp` declined to be, and that nothing else was.
//
// RC04a built the driver and left three things "to its caller": reading the
// anchor, applying `HighWaterRepair::next_trx_id`, and looping the cores. No
// task owned that caller, so nothing ran a phase at mount. The workplan's RC11
// entry carries the history; what belongs here is the contract below.
//
// ---- One core, and why the loop is not here ------------------------------
//
// RV2 makes each stream independent, and the engine already opens one WAL
// stack per core in two places - `Expeditor` for core 0, `CoreRuntime` for
// every peer. Each calls this for the stream it owns. Writing the loop here
// instead would mean one core reaching into another's store and log, and
// would put an order between streams that `workplan-crosscore.md`
// guideline 3 forbids.
//
// ---- What this adds that the driver could not ----------------------------
//
// Two `server`-layer facts, and `wal/` sits below `server/`:
//
//   1. **The anchor becomes an `AnalysisStart`.** A zeroed slot is not an
//      error - it means no checkpoint was ever published, so the scan
//      starts at the head of the stream and the durable-point check is
//      disabled (`analysis.hpp`). That is the state of every database this
//      engine has ever written before RC08.
//   2. **The undo phase is installed.** `txn::RecoveryUndo` over the
//      caller's undo log and WAL manager. Not optional: `RecoverCore`
//      refuses a stream that has losers and no phase, so passing null
//      would be choosing that refusal over recovering.
//
// The anchor is passed **in** rather than read from a `SuperBlock&` here,
// because a peer's superblock is not the live one: `CoreRuntime` holds a
// default-constructed copy whose anchor slots are all zero, while the
// anchor a peer's checkpointer published lives in core 0's page 0
// (`server/remote_checkpoint_anchor.hpp`). A function reading the
// superblock in front of it would therefore recover core 0 from its anchor
// and every peer from the head of its stream, silently.
//
// ---- The two obligations this leaves the caller, by name -----------------
//
//   1. **Persist the transaction ceiling.** `next_trx_id` is the id the
//      sequence must not sit below, and `txn::TrxIdSequence` **caches it at
//      construction** (`txn/trx_id.hpp`) - so the caller owes
//      `SuperBlock::SetNextTrxId` plus a persist *before* it builds the
//      sequence, not after. Raising it afterwards changes a field nothing
//      reads again.
//   2. **Seed the extent allocator above `page_floor`.** RC04's named
//      obligation 1: `storage::ExtentAllocator` searches from a hint, and
//      an extent covering a page the log names is RV4's hazard wearing the
//      multicore shape.
//
// Neither is done here, because both write structures a peer may not touch
// (page 0 is core 0's, M5) and a function that did them would be right on
// one mount path and wrong on the other.

namespace kds::server {

// What one core's recovery did, for the caller's obligations above and for
// the mount log line. Counted rather than inferred: a phase that silently
// did nothing is the failure mode this whole plan is written against.
struct MountRecovery {
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
    // The PW1c-2 not-dirty filter's count - zero on any stream with no
    // PAGE_HANDOFF, and lifted here so that claim is checkable at a real
    // mount, not only in gtest (the f19ead1 review's deferred item).
    std::uint64_t redo_skipped_not_dirty = 0;
    std::uint64_t pages_healed = 0;         // checksum detected, an FPI healed

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

    // ---- Assertion enforcement, resumed (RC07) ----
    //
    // `assertions_enforcing` is what `SHOW ASSERTIONS` will report `enforcing=1`
    // for; `assertions_unrecovered` is the honest remainder - a surviving
    // declaration whose directory could not be rebuilt, which stays out of the
    // registry rather than enforcing against zero.
    std::uint32_t assertions_enforcing = 0;
    std::uint32_t assertions_unrecovered = 0;

    // Declarations this core read and did **not** take on, because the
    // relation belongs to another core (PW1c-6c): an assertion is enforced
    // by the core whose writes maintain its Bound Cabin, so on every other
    // core it is somebody else's, not a failure. Counted separately from
    // `assertions_unrecovered` for exactly that reason - folding the two
    // would make a correctly-partitioned instance look half-broken.
    std::uint32_t assertions_foreign = 0;

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
// at all**: resolving a page to its relation needs a page->relation index,
// `page.md` has none, and its absence is already the named blocker on page reuse
// (`docs/spec/physical-optimizer.md` §6 gate 3). Building the set instead means
// walking every page of every relation at every mount. So `SHOW META` reports
// this number and states the other case in words; RC09's task entry carries the
// full argument.
//
// Never fails the mount: an unreadable relation is what it is *reporting*, not
// an error it hit. Returns the two counts, writes one log line per finding.
MountRecovery AuditCatalogAfterRecovery(catalog::Catalog& catalog, storage::PageStore& store,
                                        MountRecovery report, Logger* log);

// Assertion enforcement, resumed (RC07's mount wiring, AS6a).
//
// `assertion.md` §7 promises enforcement "immediately at restart - no
// rebuild scan, no enforcement gap", and until this ran the engine contradicted
// it: `SHOW ASSERTIONS` reported `enforcing=0` for every surviving assertion,
// honestly, because the registry is memory-resident and nothing refilled it.
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
// ---- Whose assertions these are (PW1c-6c) --------------------------------
//
// **Only the relations `core_id` owns.** An assertion's Bound Cabin is
// appended to by every write to its relation, so it is maintained by that
// relation's owner and by nobody else; a second core holding the same
// directory would hold a stale copy of a structure it may not write, and
// would report `enforcing=1` for counts that stopped moving at this mount.
// Assertions on another core's relations are counted (`assertions_foreign`)
// and skipped.
//
// A relation this core owns whose cabin this core may **not write** is the
// pre-PW1c-6c file: core 0 built the cabin from core 0's lease. It is not
// adopted - appending would be refused at the first write, and enforcing
// from a directory nothing maintains is worse than not enforcing - and it
// is recorded through `AssertionEnforcer::NoteUnenforceable`, which is what
// makes the owner's write path refuse by name instead of admitting an
// unchecked write.
//
// Called after `RecoverCoreAtMount` and before the listener binds. `from_lsn` is
// the anchor's `checkpoint_lsn`, which is what makes the scan AS6a's "from the
// last checkpoint".
MountRecovery ResumeAssertionsAfterRecovery(catalog::Catalog& catalog,
                                           storage::PageStore& store, wal::LogDevice& device,
                                           std::uint32_t core_id, wal::Lsn from_lsn,
                                           exec::AssertionEnforcer& enforcer,
                                           MountRecovery report, Logger* log);

// Recovery's completion checkpoint (RC08, `docs/spec/wal.md` §12-4): the last step
// of a mount, and what bounds the *next* crash's work.
//
// ---- Why a mount without one is a mount that gets slower ------------------
//
// Recovery starts where the anchor says, and until this ran nothing published
// an anchor after replaying - so every mount scanned from wherever the last
// periodic checkpoint left off, and a database that had never completed one
// scanned its whole stream, every time, forever. The cost of this call is one
// flush of the dirty table plus two records; what it buys is that the next
// recovery starts here instead. (Numbers live in the workplan and in `bench/`,
// which name the commit they were measured at; a header cannot.)
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
Status CheckpointAfterRecovery(std::uint32_t core_id, wal::WalManager& wal,
                               wal::CheckpointTarget& target, wal::CheckpointAnchor& anchor,
                               Logger* log, const sched::Clock* clock = nullptr,
                               sched::MonoTimeNs* elapsed_ns = nullptr,
                               const wal::AssertionSnapshotSource* assertions = nullptr);

}  // namespace kds::server
