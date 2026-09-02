#include "kds/wal/recovery.hpp"

#include <string>

namespace kds::wal {
namespace {

// One phase's elapsed time, or 0 with no clock. A lambda-free helper because
// each phase needs the same two reads and the pair has to stay together: a
// `Now()` taken before a phase and compared against one taken after a *later*
// phase is a timing that is wrong rather than absent.
class PhaseTimer {
public:
    explicit PhaseTimer(const sched::Clock* clock) noexcept
        : clock_(clock), started_(clock != nullptr ? clock->Now() : 0) {}

    sched::MonoTimeNs Elapsed() const noexcept {
        if (clock_ == nullptr) return 0;
        const sched::MonoTimeNs now = clock_->Now();
        // A clock that went backwards is a platform fault, not a negative
        // duration to report: `MonoTimeNs` is unsigned and the subtraction
        // would wrap into an absurd number that then reads as the phase's cost.
        return now >= started_ ? now - started_ : 0;
    }

private:
    const sched::Clock* clock_;
    sched::MonoTimeNs started_;
};

}  // namespace

StatusOr<RecoveryReport> RecoverCore(LogDevice& device, std::uint32_t core_id,
                                     storage::PageStore& store, const AnalysisStart& start,
                                     UndoPhase* undo, const sched::Clock* clock,
                                     PreparedResolver* resolver) {
    RecoveryReport out;
    out.timings.timed = clock != nullptr;

    // ---- 1. Analysis: the log and nothing else --------------------------
    PhaseTimer analysis_timer(clock);
    auto analyzed = Analyze(device, core_id, start);
    out.timings.analysis_ns = analysis_timer.Elapsed();
    if (!analyzed.ok()) {
        return analyzed.status().WithContext("recovery of core " + std::to_string(core_id) +
                                             ": analysis");
    }
    out.analysis = analyzed.value();

    // ---- 1a. Resolution: what a prepared transaction's coordinator said --
    //
    // **Before the loser refusal below, and before redo**, because it is
    // what decides which set each prepared transaction joins - and it is
    // read-only, so a mount that refuses here has written nothing.
    //
    // Two independent reads and no comparison (`PreparedResolver`): this
    // stream's records say what to redo, the coordinator's say whether it
    // was committed. Guideline 3 is not approached, let alone crossed.
    out.prepared = out.analysis.prepared;
    if (out.analysis.prepared != 0) {
        if (resolver == nullptr) {
            return Status::NotImplemented(
                "recovery of core " + std::to_string(core_id) + ": " +
                std::to_string(out.analysis.prepared) +
                " transaction(s) are prepared and undecided and no resolver is installed; this "
                "core promised not to decide them, so it may neither roll them back nor "
                "publish them (instructions/v2.4.0/2pc.md D4)");
        }
        auto verdicts = resolver->ResolveAll(out.analysis.prepared_txns);
        if (!verdicts.ok()) {
            // A refusal to mount, never a default. The message the resolver
            // wrote says which core it could not ask and why.
            return verdicts.status().WithContext("recovery of core " + std::to_string(core_id) +
                                                 ": resolving prepared transaction(s)");
        }
        for (const auto& [participant_txn_id, prepared] : out.analysis.prepared_txns) {
            (void)prepared;
            auto verdict = verdicts.value().find(participant_txn_id);
            if (verdict == verdicts.value().end()) {
                // A resolver that answered some and not others. Refused
                // rather than defaulted: an unanswered prepare is exactly
                // the state this phase exists to end.
                return Status::InvalidArgument(
                    "recovery of core " + std::to_string(core_id) +
                    ": the resolver returned no verdict for prepared transaction " +
                    std::to_string(participant_txn_id));
            }
            auto state = out.analysis.transactions.find(participant_txn_id);
            if (state == out.analysis.transactions.end()) {
                // Unreachable: `prepared_txns` is a subset of
                // `transactions` by construction (analysis.cpp erases any
                // entry whose transaction is not `kPrepared`). Refused
                // rather than asserted, because the alternative is a
                // transaction resolved into nothing.
                return Status::Corruption(
                    "recovery of core " + std::to_string(core_id) + ": prepared transaction " +
                    std::to_string(participant_txn_id) + " has no analysis state");
            }
            switch (verdict->second) {
                case TxnOutcome::kWinner:
                    state->second.outcome = TxnOutcome::kWinner;
                    ++out.analysis.winners;
                    ++out.prepared_committed;
                    break;
                case TxnOutcome::kLoser:
                    state->second.outcome = TxnOutcome::kLoser;
                    ++out.analysis.losers;
                    ++out.prepared_aborted;
                    break;
                // Spelled out rather than defaulted, so that a fifth
                // outcome would be a compile warning here rather than a
                // runtime refusal: `kAborted` would say "already
                // compensated in this stream", which a prepared participant
                // never is, and `kPrepared` would be the resolver answering
                // the question with itself.
                case TxnOutcome::kAborted:
                case TxnOutcome::kPrepared:
                    return Status::InvalidArgument(
                        "recovery of core " + std::to_string(core_id) + ": the resolver of " +
                        std::to_string(participant_txn_id) + " answered " +
                        TxnOutcomeName(verdict->second) +
                        ", which is not a verdict a prepared transaction can take");
            }
            --out.analysis.prepared;
        }
    }

    // ---- The refusal, taken before a byte is written --------------------
    //
    // Checked here rather than after redo, deliberately. Redo restores
    // uncommitted writes by design, so once it has run, a mount that then
    // discovers it cannot undo them has already put a loser's rows on the
    // pages - and `txn.md` §8's gap makes those rows read as committed. The
    // only version of this refusal that leaves the database as it was found
    // is the one that happens before redo.
    if (undo == nullptr && out.analysis.losers != 0) {
        return Status::NotImplemented(
            "recovery of core " + std::to_string(core_id) + ": " +
            std::to_string(out.analysis.losers) +
            " transaction(s) have no terminal record and no undo phase is installed; replaying "
            "without rolling them back would publish their uncommitted writes (docs/spec/txn.md §8, "
            "docs/spec/wal.md)");
    }

    // ---- 2. Redo: crash-time state, uncommitted writes included ---------
    PhaseTimer redo_timer(clock);
    auto redone = Redo(device, core_id, store, out.analysis, start.single_stream);
    out.timings.redo_ns = redo_timer.Elapsed();
    if (!redone.ok()) {
        return redone.status().WithContext("recovery of core " + std::to_string(core_id) +
                                          ": redo");
    }
    out.redo = redone.value();

    // ---- 3. RV4's high-water repair, before anything can allocate -------
    PhaseTimer high_water_timer(clock);
    auto repaired = RaiseHighWater(store, out.analysis);
    out.timings.high_water_ns = high_water_timer.Elapsed();
    if (!repaired.ok()) {
        return repaired.status().WithContext("recovery of core " + std::to_string(core_id));
    }
    out.high_water = repaired.value();

    // ---- 4. Undo ---------------------------------------------------------
    //
    // Nothing to do with no losers, whoever is installed: `kAborted` was
    // already compensated by records redo replayed, and `kWinner` stands.
    if (undo != nullptr && out.analysis.losers != 0) {
        PhaseTimer undo_timer(clock);
        Status s = undo->RollBack(store, out.analysis);
        out.timings.undo_ns = undo_timer.Elapsed();
        if (!s.ok()) {
            return s.WithContext("recovery of core " + std::to_string(core_id) + ": undo");
        }
        out.undo_ran = true;
    }

    return out;
}

}  // namespace kds::wal
