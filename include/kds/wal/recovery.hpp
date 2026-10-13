#pragma once

#include <cstdint>
#include <map>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/sched/clock.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/analysis.hpp"
#include "kds/wal/high_water.hpp"
#include "kds/wal/log_device.hpp"
#include "kds/wal/redo.hpp"

// Recovery of the log, end to end (docs/spec/wal.md §12,
// docs/workplan-wal-recovery.md RV1/RV2): the separately-tested phases
// `Analyze`, `Redo`, `RaiseHighWater` and the injected undo, in order.
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
// **Redo without undo is worse than no recovery at all**: redo restores
// uncommitted writes, and a surviving one reads as *committed* on the next
// boot (its writer id is below the new high-water mark and in no live set).
// So with an `UndoPhase` installed (the mount's is `txn::RecoveryUndo`)
// losers are rolled back; without one, a stream with losers **fails the
// mount** - `txn.md` §8's "do not attempt a partial recovery". A stream with
// no losers needs no undo.
//
// ---- What this deliberately leaves to its caller -------------------------
//
// **The superblock.** Where the anchor is read from and where
// `HighWaterRepair::next_trx_id` is applied are both `server/`'s, which sits
// above `wal/` - the boundary `analysis.hpp` drew and this file keeps. The
// caller passes an `AnalysisStart` in and applies the report's ceiling out.
//
// **When it runs.** Once per log, at mount (`server/mount_recovery.cpp`);
// `core_id` names the stream's segment files and the log lines.
//
// **The completion checkpoint** (RC08) and the operator report (RC09).

namespace kds::wal {

// Recovery's fourth phase (RC05; `txn::RecoveryUndo`).
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

    // Analysis's recomputed redo start, when redo's floor raised it to the
    // scan start (BC-R2); 0 when the floor changed nothing. The case is the
    // one `workorder-bc-wal-recycling.md` §1.4 describes, and BC-S1's cells
    // show the records below the floor change no page.
    Lsn redo_start_floored_from = 0;

    // Records whose page the store could not resolve to a live relation
    // (RV3's counter). **Always 0: nothing populates it** (RC09's, unbuilt).
    std::uint64_t orphaned_records = 0;
};

// Runs the phases above against the stream on `device`.
//
// Fails, and so refuses the mount (RV1), when:
//   - analysis cannot honestly reach the anchor's durable point, or the
//     scanner rejects the file set (`analysis.hpp`);
//   - a record cannot be applied to the page it names, or a checksum-failed
//     page reaches the end of the scan unhealed (`redo.hpp`);
//   - the store cannot raise its allocation floor (`high_water.hpp`);
//   - **analysis found losers and `undo` is null** - see above;
//   - the log carries a retired record kind (`analysis.cpp`, BP-R4);
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
