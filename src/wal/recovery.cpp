#include "kds/wal/recovery.hpp"

#include <string>

namespace kds::wal {

StatusOr<RecoveryReport> RecoverCore(LogDevice& device, std::uint32_t core_id,
                                     storage::PageStore& store, const AnalysisStart& start,
                                     UndoPhase* undo) {
    RecoveryReport out;

    // ---- 1. Analysis: the log and nothing else --------------------------
    auto analyzed = Analyze(device, core_id, start);
    if (!analyzed.ok()) {
        return analyzed.status().WithContext("recovery of core " + std::to_string(core_id) +
                                             ": analysis");
    }
    out.analysis = analyzed.value();

    // ---- The refusal, taken before a byte is written --------------------
    //
    // Checked here rather than after redo, deliberately. Redo restores
    // uncommitted writes by design, so once it has run, a mount that then
    // discovers it cannot undo them has already put a loser's rows on the
    // pages - and `txn.md` §8's gap makes those rows read as committed. The
    // only version of this refusal that leaves the database as it was found
    // is the one that happens before redo.
    if (undo == nullptr && out.analysis.losers != 0) {
        return Status::Unsupported(
            "recovery of core " + std::to_string(core_id) + ": " +
            std::to_string(out.analysis.losers) +
            " transaction(s) have no terminal record and no undo phase is installed; replaying "
            "without rolling them back would publish their uncommitted writes (docs/txn.md §8, "
            "docs/workplan-wal-recovery.md RC05)");
    }

    // ---- 2. Redo: crash-time state, uncommitted writes included ---------
    auto redone = Redo(device, core_id, store, out.analysis);
    if (!redone.ok()) {
        return redone.status().WithContext("recovery of core " + std::to_string(core_id) +
                                           ": redo");
    }
    out.redo = redone.value();

    // ---- 3. RV4's high-water repair, before anything can allocate -------
    auto repaired = RaiseHighWater(store, out.analysis);
    if (!repaired.ok()) {
        return repaired.status().WithContext("recovery of core " + std::to_string(core_id));
    }
    out.high_water = repaired.value();

    // ---- 4. Undo ---------------------------------------------------------
    //
    // Nothing to do with no losers, whoever is installed: `kAborted` was
    // already compensated by records redo replayed, and `kWinner` stands.
    if (undo != nullptr && out.analysis.losers != 0) {
        if (Status s = undo->RollBack(store, out.analysis); !s.ok()) {
            return s.WithContext("recovery of core " + std::to_string(core_id) + ": undo");
        }
        out.undo_ran = true;
    }

    return out;
}

}  // namespace kds::wal
