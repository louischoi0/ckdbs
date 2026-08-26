#pragma once

#include <cstdint>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/sched/clock.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/analysis.hpp"
#include "kds/wal/high_water.hpp"
#include "kds/wal/log_device.hpp"
#include "kds/wal/redo.hpp"

// One core's recovery, end to end (docs/spec/wal.md §12,
// docs/workplan-wal-recovery.md RV1/RV2).
//
// The phases exist as separate, separately-tested functions - `Analyze`,
// `Redo`, `RaiseHighWater` - and until this file nothing put them in order.
// `workplan-wal-recovery.md` §2 named the entry point and attributed it to
// "RC02-RC05", but no task's *Done when* mentioned it, so it was written by
// nobody. This is it.
//
// ---- Order, and why each step is where it is -----------------------------
//
//   1. **Analysis** - one forward scan; no page is touched.
//   2. **Redo** - crash-time state, uncommitted changes and undo pages
//      included, because that state is what undo then rolls back from.
//   3. **The high-water repair** (RV4) - *before* undo, not after. Undo
//      writes compensations, and RV4's rule is "before the store serves an
//      allocation"; putting the repair after the phase that writes would be
//      taking the rule's word for its intent.
//   4. **Undo** - injected, see below.
//
// ---- Undo is injected, and its absence refuses the mount -----------------
//
// RC05 is unbuilt. That is not a reason for this function to skip the phase
// and return success, and the reason is sharper than tidiness: **redo
// without undo is worse than no recovery at all.** Redo deliberately
// restores uncommitted writes, and `txn.md` §8's accepted gap is that a
// surviving uncommitted row reads as *committed* on the next boot - its
// writer id is below the new high-water mark and in no live set. So a
// recovery that replays and stops has not left the database as it found it;
// it has published every loser's writes.
//
// Hence `UndoPhase`. With one installed, losers are rolled back. Without
// one, a stream that analysis found losers in **fails the mount**, naming
// RC05 - which is `txn.md` §8's "do not attempt a partial recovery" applied
// to the one partial state this code could otherwise produce. A stream with
// no losers needs no undo and recovers completely today.
//
// ---- What this deliberately leaves to its caller -------------------------
//
// **The superblock.** Where the anchor is read from and where
// `HighWaterRepair::next_trx_id` is applied are both `server/`'s, which sits
// above `wal/` - the boundary `analysis.hpp` drew and this file keeps. The
// caller passes an `AnalysisStart` in and applies the report's ceiling out.
//
// **The core loop.** RV2 makes each stream independent, so recovery of a
// database is this function once per core, ordered by nothing. Introducing
// an ordering between streams is the thing `workplan-crosscore.md`
// guideline 3 forbids.
//
// **The completion checkpoint** (RC08) and the operator report (RC09).

namespace kds::wal {

// Recovery's fourth phase, supplied by whoever implements it (RC05).
//
// An interface rather than a function pointer because undo needs state a
// callback would have to capture - the undo log, the transaction manager's
// compensation path, the row-identity check §4a added - and because a
// null one has to be a distinguishable, refusable condition rather than a
// silently absent step.
class UndoPhase {
public:
    virtual ~UndoPhase() = default;

    // Rolls back every `TxnOutcome::kLoser` in `analysis`, emitting each
    // compensation as an ordinary logged mutation and a `TXN_ABORT` after
    // it - so the work is crash-restartable through RV5's page_lsn gate
    // alone, exactly as a live rollback is.
    //
    // **Not `kAborted`.** A durable `TXN_ABORT` means the compensations
    // preceding it are durable too, so redo has already replayed them and
    // undo owes that transaction nothing (`analysis.hpp`).
    virtual Status RollBack(storage::PageStore& store, const AnalysisResult& analysis) = 0;
};

// How long each phase took (`docs/spec/wal.md` §13's "recovery phase timings",
// RC09). Zero throughout when no clock was supplied, which `timed` says
// explicitly - an operator reading four zeroes must be able to tell "instant"
// from "never measured", and a duration is the one number where those two look
// identical.
struct RecoveryTimings {
    bool timed = false;
    sched::MonoTimeNs analysis_ns = 0;
    sched::MonoTimeNs redo_ns = 0;
    sched::MonoTimeNs high_water_ns = 0;
    sched::MonoTimeNs undo_ns = 0;

    sched::MonoTimeNs total_ns() const noexcept {
        return analysis_ns + redo_ns + high_water_ns + undo_ns;
    }
};

struct RecoveryReport {
    AnalysisResult analysis;
    RedoStats redo;
    HighWaterRepair high_water;
    RecoveryTimings timings;

    // Whether an UndoPhase ran. False with no losers (nothing to do) and
    // false with none installed (in which case recovery failed and this
    // report was not returned) - so on a returned report it means
    // "compensations were emitted".
    bool undo_ran = false;

    // Records whose page the store could not resolve to a live relation -
    // RV3's honest counter, so a catalog the crash lost reads as a number
    // rather than as silence. **Always 0 today**: RC09 owns populating it,
    // and a field that is present and zero is what keeps its absence from
    // being mistaken for its being fine.
    std::uint64_t orphaned_records = 0;
};

// Runs the phases above against one core's stream.
//
// Fails, and so refuses the mount (RV1), when:
//   - analysis cannot honestly reach the anchor's durable point, or the
//     scanner rejects the file set (`analysis.hpp`);
//   - a record cannot be applied to the page it names, or a checksum-failed
//     page reaches the end of the scan unhealed (`redo.hpp`);
//   - the store cannot raise its allocation floor (`high_water.hpp`);
//   - **analysis found losers and `undo` is null** - see above;
//   - the undo phase itself fails.
//
// Every one of those is a refusal rather than a partial success, which is
// the whole of `txn.md` §8's instruction.
// `clock`, when given, times each phase into `RecoveryReport::timings`. Passed
// rather than read from the platform, because `sched::Clock` is the engine's
// only clock (`sched/clock.hpp`) and a deterministic caller - the simulator, a
// test - must be able to supply its own. A null clock is not an error: the
// report simply says it was not timed.
StatusOr<RecoveryReport> RecoverCore(LogDevice& device, std::uint32_t core_id,
                                     storage::PageStore& store, const AnalysisStart& start,
                                     UndoPhase* undo = nullptr,
                                     const sched::Clock* clock = nullptr);

}  // namespace kds::wal
