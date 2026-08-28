#include "kds/server/prepared_resolver.hpp"

#include <string>

#include "kds/wal/file_log_device.hpp"
#include "kds/wal/log_scanner.hpp"
#include "kds/wal/record.hpp"

namespace kds::server {

StatusOr<std::map<std::uint64_t, wal::TxnOutcome>> CoordinatorStreamResolver::ResolveAll(
    const std::map<std::uint64_t, wal::PreparedTxn>& prepared) {
    // Grouped first, so one coordinator's stream is opened and scanned once
    // however many of this core's transactions it decided.
    std::map<std::uint32_t, std::map<std::uint64_t, wal::PreparedTxn>> by_coordinator;
    for (const auto& [participant_txn_id, txn] : prepared) {
        if (txn.coordinator_core == core_id_) {
            // A coordinator never writes TXN_PREPARE - only a participant
            // does - so a record naming this core as its own coordinator is
            // malformed, and resolving it would mean scanning this stream
            // for a decision this stream is the reason to be looking for.
            return Status::Corruption(
                "cross-owner recovery: the prepare of transaction " +
                std::to_string(participant_txn_id) + " at lsn " +
                std::to_string(txn.prepare_lsn) + " names core " + std::to_string(core_id_) +
                " as its own coordinator");
        }
        if (txn.coordinator_core >= anchors_.size()) {
            // Refused by name against the cores this database has, rather
            // than left to produce an absent-stream refusal by accident.
            // Unreachable for a legal image - the superblock pins the core
            // count and a mismatched mount is refused at the door - which
            // is why it is corruption rather than a condition to handle.
            return Status::Corruption(
                "cross-owner recovery: the prepare of transaction " +
                std::to_string(participant_txn_id) + " names coordinator core " +
                std::to_string(txn.coordinator_core) + ", and this database has " +
                std::to_string(anchors_.size()) + " core(s)");
        }
        by_coordinator[txn.coordinator_core].emplace(participant_txn_id, txn);
    }

    std::map<std::uint64_t, wal::TxnOutcome> out;
    for (const auto& [coordinator_core, mine] : by_coordinator) {
        if (Status s = ResolveOneStream(coordinator_core, mine, out); !s.ok()) return s;
    }
    return out;
}

Status CoordinatorStreamResolver::ResolveOneStream(
    std::uint32_t coordinator_core, const std::map<std::uint64_t, wal::PreparedTxn>& mine,
    std::map<std::uint64_t, wal::TxnOutcome>& out) {
    auto device = wal::FileLogDevice::Open(wal_dir_, coordinator_core, segment_size_);
    if (!device.ok()) {
        return device.status().WithContext("cross-owner recovery: opening core " +
                                           std::to_string(coordinator_core) +
                                           "'s stream to resolve " +
                                           std::to_string(mine.size()) + " transaction(s)");
    }
    if (device.value()->segment_count() == 0) {
        // Not "that core never wrote": every core publishes a completion
        // checkpoint at the end of every mount (RC08), so a stream with no
        // segments at all is a log directory missing a file. Answering
        // "abort" here could discard a transaction the coordinator
        // committed and acknowledged.
        return Status::Corruption(
            "cross-owner recovery: core " + std::to_string(coordinator_core) +
            " has no stream in " + wal_dir_ + ", so the outcome of " +
            std::to_string(mine.size()) + " transaction(s) prepared here cannot be read; the "
            "decision is that core's and this mount will not guess it");
    }
    ++streams_read_;

    // The whole stream, from the beginning. There is no sound lower bound:
    // a bound taken from this stream's LSNs would be exactly the
    // cross-stream comparison guideline 3 forbids, and one taken from the
    // coordinator's checkpoint would assume the decision is above it. The
    // cost is one full scan per coordinator, paid only by a mount that has
    // a prepared transaction - which is a crash mid-protocol, not an
    // ordinary boot.
    //
    // Every id at once: the ids this core prepared under this coordinator
    // are matched as the scan passes them, so the stream is read once.
    std::map<std::uint64_t, wal::TxnOutcome> verdicts;  // keyed by *coordinator* txn id
    std::uint64_t scanned = 0;
    const auto visit = [&](const wal::DecodedRecord& record) -> Status {
        ++scanned;
        const bool commit = record.type() == wal::RecordType::kTxnCommit;
        const bool abort = record.type() == wal::RecordType::kTxnAbort;
        if (!commit && !abort) return Status::OK();
        const wal::TxnOutcome verdict =
            commit ? wal::TxnOutcome::kWinner : wal::TxnOutcome::kLoser;
        auto [it, inserted] = verdicts.emplace(record.header.txn_id, verdict);
        if (!inserted && it->second != verdict) {
            // **Two terminal records disagreeing about one id.** Impossible
            // by construction - the persisted trx-id ceiling means an id is
            // never reused - and refused rather than resolved by
            // last-one-wins, because this function's whole contract is that
            // a wrong answer is worse than no answer, and a stream holding
            // both a COMMIT and an ABORT for one transaction is not
            // trustworthy about precisely what is being asked of it.
            return Status::Corruption(
                "cross-owner recovery: core " + std::to_string(coordinator_core) +
                "'s stream holds both a COMMIT and an ABORT for transaction " +
                std::to_string(record.header.txn_id) + "; its decision is not readable");
        }
        return Status::OK();
    };
    auto scan = wal::ScanLog(*device.value(), coordinator_core, /*from_lsn=*/0, visit);
    if (!scan.ok()) {
        return scan.status().WithContext("cross-owner recovery: scanning core " +
                                         std::to_string(coordinator_core) + "'s stream for " +
                                         std::to_string(mine.size()) + " transaction(s)");
    }
    records_scanned_ += scanned;

    // **The honesty check `Analyze` applies to its own stream** - and it
    // matters more here. A scan that ends before the durable point this
    // core's anchor was published with has lost records the anchor depends
    // on; for one's own stream that is a replay of a prefix, and for
    // somebody else's it is an *absent decision*, which reads as an abort
    // and durably contradicts a coordinator that committed. Taken after the
    // scan, on the scan's own end point, exactly as `Analyze` takes it.
    const wal::Lsn anchor_durable_lsn = anchors_[coordinator_core].durable_lsn;
    if (anchor_durable_lsn != 0 && scan.value().end_lsn < anchor_durable_lsn) {
        return Status::Corruption(
            "cross-owner recovery: core " + std::to_string(coordinator_core) +
            "'s stream ends at lsn " + std::to_string(scan.value().end_lsn) +
            ", before the durable point " + std::to_string(anchor_durable_lsn) +
            " its checkpoint anchor was published with - the decision for " +
            std::to_string(mine.size()) +
            " transaction(s) prepared here may be among the records that are gone");
    }

    for (const auto& [participant_txn_id, txn] : mine) {
        auto found = verdicts.find(txn.coordinator_txn_id);
        const bool decided = found != verdicts.end();
        const wal::TxnOutcome verdict = decided ? found->second : wal::TxnOutcome::kLoser;
        out.emplace(participant_txn_id, verdict);

        if (!decided && scan.value().stopped_early && log_ != nullptr &&
            log_->enabled(LogLevel::kWarn)) {
            // The weakest evidence this function ever acts on: no decision
            // *and* a torn tail. The anchor check above is what makes it
            // sound - the scan reached everything that core called durable -
            // so this is a line an operator can find, not a refusal.
            log_->Warn("recovery", "core " + std::to_string(core_id_) + ": transaction " +
                                       std::to_string(participant_txn_id) +
                                       " resolves to ABORT from core " +
                                       std::to_string(coordinator_core) +
                                       "'s stream, whose tail was torn past its durable point");
        }
        if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
            log_->Info("recovery",
                       "core " + std::to_string(core_id_) + ": transaction " +
                           std::to_string(participant_txn_id) + ", prepared for core " +
                           std::to_string(coordinator_core) + "'s session " +
                           std::to_string(txn.coordinator_session_id) + " transaction " +
                           std::to_string(txn.coordinator_txn_id) + ", resolves to " +
                           (verdict == wal::TxnOutcome::kWinner ? "COMMIT" : "ABORT") +
                           (decided ? " (its coordinator's decision)"
                                    : " (its coordinator's stream holds no decision, so "
                                      "nothing committed anywhere)"));
        }
    }
    return Status::OK();
}

}  // namespace kds::server
