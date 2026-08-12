#include "kds/server/mount_recovery.hpp"

#include <string>

#include "kds/txn/recovery_undo.hpp"
#include "kds/wal/recovery.hpp"

namespace kds::server {

StatusOr<MountRecovery> RecoverCoreAtMount(std::uint32_t core_id, const WalAnchorFields& anchor,
                                          wal::LogDevice& device, storage::PageStore& store,
                                          txn::UndoLog& undo_log, wal::WalManager* wal,
                                          Logger* log) {
    // A zeroed slot means no checkpoint was ever published: scan from the
    // head of the stream, and disable the durable-point check because there
    // is no published point to hold the scan to (`analysis.hpp`).
    wal::AnalysisStart start;
    start.redo_start_lsn = anchor.redo_start_lsn;
    start.anchor_durable_lsn = anchor.durable_lsn;

    txn::RecoveryUndo undo(undo_log, wal);
    auto report = wal::RecoverCore(device, core_id, store, start, &undo);
    if (!report.ok()) {
        // Propagated, not logged-and-continued. The status already carries
        // the phase and the core (`recovery.cpp`), and RV1 makes it the
        // mount's answer rather than a warning on a database that is then
        // served anyway.
        return report.status();
    }

    MountRecovery out;
    out.records = report.value().analysis.records;
    out.torn_tail = report.value().analysis.stopped_early;
    out.winners = report.value().analysis.winners;
    out.aborted = report.value().analysis.aborted;
    out.losers = report.value().analysis.losers;
    out.redo_applied = report.value().redo.applied;
    out.redo_skipped_by_lsn = report.value().redo.skipped_by_lsn;
    out.pages_healed = report.value().redo.pages_healed;
    out.transactions_rolled_back = undo.transactions();
    out.compensations = undo.compensations();
    out.page_floor = report.value().high_water.page_floor;
    out.page_floor_raised = report.value().high_water.page_floor_raised;
    out.next_trx_id = report.value().high_water.next_trx_id;

    if (log != nullptr) {
        if (out.empty()) {
            // Said out loud rather than left silent, because "no records"
            // and "recovery never ran" look identical in a log that only
            // reports work (`analysis.hpp`'s own argument for the
            // durable-point check).
            log->Info("recovery", "core " + std::to_string(core_id) +
                                      ": stream holds no records, nothing to recover");
        } else {
            log->Info("recovery",
                      "core " + std::to_string(core_id) + ": " + std::to_string(out.records) +
                          " records scanned, " + std::to_string(out.winners) + " committed, " +
                          std::to_string(out.aborted) + " aborted, " + std::to_string(out.losers) +
                          " rolled back; redo applied " + std::to_string(out.redo_applied) +
                          ", skipped " + std::to_string(out.redo_skipped_by_lsn) +
                          ", healed " + std::to_string(out.pages_healed) + " page(s); undo wrote " +
                          std::to_string(out.compensations) + " compensation(s)" +
                          (out.torn_tail ? "; tail was torn" : ""));
        }
    }
    return out;
}

Status CheckpointAfterRecovery(std::uint32_t core_id, wal::WalManager& wal,
                               wal::CheckpointTarget& target, wal::CheckpointAnchor& anchor,
                               Logger* log) {
    // Empty by fact, not by omission - see the header. A checkpoint written
    // here with a *stale* active list would be worse than none: recovery would
    // walk the undo chain of a transaction that no longer exists.
    wal::NoActiveTransactions none;
    wal::Checkpointer checkpointer(wal, target, none, anchor);
    checkpointer.SetLogger(log);

    // Run to completion rather than paced: there is no reactor to spread this
    // across yet, and the whole point is that the mount does not finish until
    // the anchor is durable (wal.md §8-3's ordering is inside Complete()).
    if (Status s = checkpointer.RunToCompletion(); !s.ok()) {
        return s.WithContext("recovery of core " + std::to_string(core_id) +
                             ": completion checkpoint");
    }

    if (log != nullptr) {
        log->Info("recovery", "core " + std::to_string(core_id) +
                                 ": completion checkpoint at lsn " +
                                 std::to_string(checkpointer.last_checkpoint_lsn()) +
                                 ", the next recovery starts at " +
                                 std::to_string(checkpointer.redo_start_lsn()) + " (" +
                                 std::to_string(checkpointer.stats().pages_flushed) +
                                 " page(s) flushed)");
    }
    return Status::OK();
}

}  // namespace kds::server
