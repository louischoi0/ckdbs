#include "kds/server/mount_recovery.hpp"

#include <string>
#include <utility>
#include <vector>

#include "kds/exec/assertion_catalog.hpp"
#include "kds/exec/assertion_recover.hpp"
#include "kds/txn/recovery_undo.hpp"
#include "kds/wal/recovery.hpp"

namespace kds::server {

StatusOr<MountRecovery> RecoverCoreAtMount(std::uint32_t core_id, const WalAnchorFields& anchor,
                                          wal::LogDevice& device, storage::PageStore& store,
                                          txn::UndoLog& undo_log, wal::WalManager* wal,
                                          Logger* log, const sched::Clock* clock) {
    // A zeroed slot means no checkpoint was ever published: scan from the
    // head of the stream, and disable the durable-point check because there
    // is no published point to hold the scan to (`analysis.hpp`).
    wal::AnalysisStart start;
    start.redo_start_lsn = anchor.redo_start_lsn;
    start.anchor_durable_lsn = anchor.durable_lsn;

    txn::RecoveryUndo undo(undo_log, wal);
    auto report = wal::RecoverCore(device, core_id, store, start, &undo, clock);
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
    out.redo_skipped_not_dirty = report.value().redo.skipped_not_dirty;
    out.pages_healed = report.value().redo.pages_healed;
    out.transactions_rolled_back = undo.transactions();
    out.compensations = undo.compensations();
    out.page_floor = report.value().high_water.page_floor;
    out.page_floor_raised = report.value().high_water.page_floor_raised;
    out.next_trx_id = report.value().high_water.next_trx_id;
    out.timings = report.value().timings;

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
                          ", skipped-not-dirty " + std::to_string(out.redo_skipped_not_dirty) +
                          ", healed " + std::to_string(out.pages_healed) + " page(s); undo wrote " +
                          std::to_string(out.compensations) + " compensation(s)" +
                          (out.torn_tail ? "; tail was torn" : "") +
                          (out.timings.timed
                               ? "; analysis " + std::to_string(out.timings.analysis_ns / 1000) +
                                     "us, redo " + std::to_string(out.timings.redo_ns / 1000) +
                                     "us, high-water " +
                                     std::to_string(out.timings.high_water_ns / 1000) +
                                     "us, undo " + std::to_string(out.timings.undo_ns / 1000) + "us"
                               : ""));
        }
    }
    return out;
}

MountRecovery AuditCatalogAfterRecovery(catalog::Catalog& catalog, storage::PageStore& store,
                                        MountRecovery report, Logger* log) {
    auto tables = catalog.ListTables();
    if (!tables.ok()) {
        // The catalog itself is unreadable, which is a bigger fact than any
        // count this function could return - and not one it may swallow. Left
        // at zero, with the reason on the log, because a zero here would
        // otherwise read as "every relation checked out".
        if (log != nullptr) {
            log->Error("recovery", "post-recovery catalog audit could not list relations: " +
                                       tables.status().message());
        }
        return report;
    }

    for (const catalog::SysObjectRow& object : tables.value()) {
        // **User relations only.** A bootstrap object is listed in
        // `sys.objects` and has no `sys.tables` row at all - it lives on a
        // fixed catalog page instead (`catalog/well_known.hpp`) - so opening
        // one fails for a reason that has nothing to do with a crash, and
        // counting those nine as "missing pages" is a false alarm on every
        // healthy mount. `kUserOidStart` is the documented boundary below which
        // every oid is a bootstrap oid.
        if (object.oid < catalog::kUserOidStart) {
            continue;
        }
        ++report.relations_checked;
        const std::string name(catalog::NameView(object.name));

        // `GetSysTableRow`, not `InitTableAccess`: the row read is never cached
        // (`catalog.hpp` - `next_id` forbids it), while an access **is**, so an
        // audit built on the cached path answers from memory for any relation
        // something already opened and reports nothing. That is not a
        // theoretical difference: the first version of this function missed one
        // of two dead relations for exactly that reason, and at a real mount the
        // cache is cold only by accident of ordering.
        auto row = catalog.GetSysTableRow(object.oid);
        if (!row.ok()) {
            ++report.relations_missing_pages;
            if (log != nullptr) {
                log->Error("recovery", "relation '" + name +
                                           "' is listed in sys.objects and has no sys.tables row "
                                           "after recovery: " +
                                           row.status().message() +
                                           " (docs/inflight/known-gaps.md: DDL is unlogged, RV3)");
            }
            continue;
        }

        // The two pages a relation cannot live without, both allocated by an
        // unlogged `CREATE TABLE`: its descriptor, and its var-heap root when
        // it has spillable columns. A relation whose spilled values are gone
        // answers reads with Corruption rather than with rows, which is worth
        // one more page read on the relations that have one at all.
        struct Owned {
            const char* what;
            PageId page_id;
        };
        for (const Owned& owned : {Owned{"descriptor", row.value().desc_page_id},
                                   Owned{"var-heap root", row.value().varheap_page_id},
                                   Owned{"anchor", row.value().anchor_page_id}}) {
            if (owned.page_id == kInvalidPageId) {
                continue;
            }
            if (auto page = store.GetForRead(owned.page_id); !page.ok()) {
                ++report.relations_missing_pages;
                if (log != nullptr) {
                    log->Error("recovery", "relation '" + name + "' has no " + owned.what +
                                               " page " + std::to_string(owned.page_id) +
                                               " after recovery: " + page.status().message() +
                                               " (docs/inflight/known-gaps.md: DDL is unlogged, RV3)");
                }
                break;  // One finding per relation; it is already unusable.
            }
        }
    }

    if (log != nullptr && report.relations_missing_pages == 0) {
        log->Info("recovery", "post-recovery catalog audit: " +
                                  std::to_string(report.relations_checked) +
                                  " relation(s) checked, all openable");
    }
    return report;
}

MountRecovery ResumeAssertionsAfterRecovery(catalog::Catalog& catalog,
                                           storage::PageStore& store, wal::LogDevice& device,
                                           std::uint32_t core_id, wal::Lsn from_lsn,
                                           exec::AssertionEnforcer& enforcer,
                                           MountRecovery report, Logger* log) {
    auto defs = exec::ListAssertions(catalog, store);
    if (!defs.ok()) {
        if (log != nullptr) {
            log->Error("recovery", "could not list assertions after recovery, so none can be "
                                   "resumed: " +
                                       defs.status().message());
        }
        // **Fail closed on this core's own relations** (PW1c-6c). The scan
        // that just failed reads the declarations, and a declaration lives
        // partly in a var-heap page a peer may not fault
        // (`exec::ListAssertionTargets` says why). Without the text nothing
        // can be enforced - but *that* an assertion exists is readable, and
        // it is enough to refuse the relation's writes instead of admitting
        // them unchecked, which is the failure this whole task exists to
        // close. A second failure here leaves the report as it was: there
        // is nothing further to try.
        auto targets = exec::ListAssertionTargets(catalog, store);
        if (!targets.ok()) return report;
        for (const exec::AssertionDef& target : targets.value()) {
            auto row = catalog.GetSysTableRow(target.target_oid);
            if (!row.ok() || row.value().owner_core != core_id) {
                ++report.assertions_foreign;
                continue;
            }
            enforcer.NoteUnenforceable(target.target_oid, target.id);
            ++report.assertions_unrecovered;
        }
        return report;
    }
    if (defs.value().empty()) {
        return report;  // nothing declared: no scan, no cost
    }

    // The shells first, so the pass has somewhere to put the headers. Each is
    // held in a stable slot for the life of the call, because the pass takes
    // pointers into the cabins.
    std::vector<exec::LiveAssertion> revived;
    revived.reserve(defs.value().size());
    for (const exec::AssertionDef& def : defs.value()) {
        // **Whose relation is it** (PW1c-6c). The row rather than the
        // declaration answers it, and an unreadable row is treated as
        // another core's: not adopting costs enforcement on this core,
        // where adopting a directory this core may not maintain costs a
        // constraint that reports enforcing and does not.
        auto row = catalog.GetSysTableRow(def.target_oid);
        if (!row.ok() || row.value().owner_core != core_id) {
            ++report.assertions_foreign;
            continue;
        }
        auto live = exec::ReviveAssertion(catalog, store, def);
        if (!live.ok()) {
            // Fail closed, as above: this core owns the relation, so this
            // core is the only one that could have enforced the constraint,
            // and a write admitted here would be checked by nobody.
            enforcer.NoteUnenforceable(def.target_oid, def.id);
            ++report.assertions_unrecovered;
            if (log != nullptr) {
                log->Error("recovery", "assertion \"" + def.name +
                                           "\" cannot be revived, so it will not enforce: " +
                                           live.status().message());
            }
            continue;
        }
        // The cabin this core would have to append to, tested **after** the
        // revive, which walked the chain: a leased store claims an
        // own-stamped page as it reads it (PW1c-7), so asking before the
        // walk would answer no for a cabin that is in fact this core's.
        if (!store.MayWrite(live.value().chain.root())) {
            enforcer.NoteUnenforceable(def.target_oid, def.id);
            ++report.assertions_unrecovered;
            if (log != nullptr) {
                log->Error("recovery",
                           "assertion \"" + def.name + "\" on a relation core " +
                               std::to_string(core_id) +
                               " owns has a Bound Cabin this core may not write (root page " +
                               std::to_string(live.value().chain.root()) +
                               "), so it cannot be enforced here and the relation's writes are "
                               "refused instead; re-create it so its owner builds the cabin "
                               "(workplan-peer-writer.md PW1c-6c)");
            }
            continue;
        }
        revived.push_back(std::move(live.value()));
    }
    if (revived.empty()) {
        return report;
    }

    std::vector<exec::RecoverableAssertion> targets;
    targets.reserve(revived.size());
    for (std::size_t i = 0; i < revived.size(); ++i) {
        exec::RecoverableAssertion target;
        target.assertion_id = revived[i].assertion_id;
        target.root_page_id = revived[i].chain.root();
        target.cabin = &revived[i].cabin;
        targets.push_back(target);
    }

    auto recovered = exec::RecoverAssertions(device, core_id, from_lsn, store, targets, log);
    if (!recovered.ok()) {
        // Reported, not fatal: a mount that refused here would refuse to open a
        // database whose *data* is intact over a constraint that can be rebuilt
        // by re-creating the assertion. What must not happen is adopting the
        // half-built directories, and returning early is what prevents it.
        report.assertions_unrecovered += static_cast<std::uint32_t>(revived.size());
        // Every one of them is on a relation this core owns - the loop above
        // kept nothing else - so every one of them has to refuse that
        // relation's writes rather than leave them unchecked (PW1c-6c).
        for (const exec::LiveAssertion& one : revived) {
            enforcer.NoteUnenforceable(one.target_oid, one.assertion_id);
        }
        if (log != nullptr) {
            log->Error("recovery", "assertion recovery failed, so no assertion will enforce: " +
                                       recovered.status().message());
        }
        return report;
    }

    for (std::size_t i = 0; i < revived.size(); ++i) {
        const auto& results = recovered.value().assertions;
        const bool whole =
            i < results.size() && results[i].assertion_id == revived[i].assertion_id &&
            results[i].recovered;
        if (!whole) {
            // **Not adopted, deliberately.** A directory the pass could not base
            // is a directory at zero, and a cabin at zero admits every write - so
            // adopting it would report `enforcing=1` for a constraint enforcing
            // nothing, which is worse than the honest `enforcing=0`.
            //
            // And on this core's own relation, `enforcing=0` is not the end
            // of it: the relation's writes are refused too, because nothing
            // else can check them (PW1c-6c).
            enforcer.NoteUnenforceable(revived[i].target_oid, revived[i].assertion_id);
            ++report.assertions_unrecovered;
            continue;
        }
        enforcer.Adopt(std::move(revived[i]));
        ++report.assertions_enforcing;
    }

    if (log != nullptr) {
        log->Info("recovery", "assertions resumed on core " + std::to_string(core_id) + ": " +
                                  std::to_string(report.assertions_enforcing) + " enforcing, " +
                                  std::to_string(report.assertions_foreign) +
                                  " on another core's relations, " +
                                  std::to_string(report.assertions_unrecovered) +
                                  " unrecovered");
    }
    return report;
}

Status CheckpointAfterRecovery(std::uint32_t core_id, wal::WalManager& wal,
                               wal::CheckpointTarget& target, wal::CheckpointAnchor& anchor,
                               Logger* log, const sched::Clock* clock,
                               sched::MonoTimeNs* elapsed_ns,
                               const wal::AssertionSnapshotSource* assertions) {
    const sched::MonoTimeNs started = clock != nullptr ? clock->Now() : 0;
    // Empty by fact, not by omission - see the header. A checkpoint written
    // here with a *stale* active list would be worse than none: recovery would
    // walk the undo chain of a transaction that no longer exists.
    wal::NoActiveTransactions none;
    wal::Checkpointer checkpointer(wal, target, none, anchor);
    checkpointer.SetLogger(log);
    // AS6a: this checkpoint becomes the anchor, so it has to carry the base the
    // *next* mount folds from. Without it enforcement would survive one restart
    // and not the one after (see the header).
    checkpointer.SetAssertionSource(assertions);

    // Run to completion rather than paced: there is no reactor to spread this
    // across yet, and the whole point is that the mount does not finish until
    // the anchor is durable (wal.md §8-3's ordering is inside Complete()).
    if (Status s = checkpointer.RunToCompletion(); !s.ok()) {
        return s.WithContext("recovery of core " + std::to_string(core_id) +
                             ": completion checkpoint");
    }

    if (elapsed_ns != nullptr && clock != nullptr) {
        const sched::MonoTimeNs now = clock->Now();
        *elapsed_ns = now >= started ? now - started : 0;
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
