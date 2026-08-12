#pragma once

#include <cstdint>

#include "kds/base/common.hpp"
#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/wal/log_device.hpp"
#include "kds/wal/manager.hpp"

// Recovery at mount (docs/workplan-wal-recovery.md RV1/RV2) - the caller
// `wal/recovery.hpp` declined to be, and that nothing else was.
//
// RC04a built the driver and listed three things it left "to its caller":
// reading the anchor, applying `HighWaterRepair::next_trx_id`, and looping
// the cores. No task in RC01-RC10 owned that caller, so `RecoverCore` was
// reachable only from `tests/wal_recovery_test.cpp` - analysis, eight redo
// appliers, the high-water repair and `txn::RecoveryUndo` all built, and a
// crash still recovering nothing. That is the same shape of hole RC04a
// itself was written to close, one layer up.
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
    std::uint64_t pages_healed = 0;         // checksum detected, an FPI healed

    // ---- What undo rolled back ----
    std::uint64_t transactions_rolled_back = 0;
    std::uint64_t compensations = 0;

    // ---- The caller's two obligations ----
    PageId page_floor = kInvalidPageId;
    bool page_floor_raised = false;
    std::uint64_t next_trx_id = 0;

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
StatusOr<MountRecovery> RecoverCoreAtMount(std::uint32_t core_id, const WalAnchorFields& anchor,
                                          wal::LogDevice& device, storage::PageStore& store,
                                          txn::UndoLog& undo_log, wal::WalManager* wal,
                                          Logger* log);

}  // namespace kds::server
