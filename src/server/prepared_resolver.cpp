#include "kds/server/prepared_resolver.hpp"

#include <string>

#include "kds/wal/file_log_device.hpp"
#include "kds/wal/log_scanner.hpp"
#include "kds/wal/record.hpp"

namespace kds::server {

StatusOr<wal::TxnOutcome> CoordinatorStreamResolver::Resolve(
    std::uint64_t participant_txn_id, const wal::PreparedTxn& prepared) {
    if (prepared.coordinator_core == core_id_) {
        // A coordinator never writes TXN_PREPARE - only a participant does -
        // so a record naming this core as its own coordinator is malformed,
        // and resolving it would mean scanning this stream for a decision
        // this stream is the reason to be looking for.
        return Status::Corruption(
            "cross-owner recovery: the prepare of transaction " +
            std::to_string(participant_txn_id) + " at lsn " +
            std::to_string(prepared.prepare_lsn) + " names core " + std::to_string(core_id_) +
            " as its own coordinator");
    }

    auto device = wal::FileLogDevice::Open(wal_dir_, prepared.coordinator_core, segment_size_);
    if (!device.ok()) {
        return device.status().WithContext("cross-owner recovery: opening core " +
                                           std::to_string(prepared.coordinator_core) +
                                           "'s stream to resolve transaction " +
                                           std::to_string(participant_txn_id));
    }
    if (device.value()->segment_count() == 0) {
        // Not "that core never wrote": every core publishes a completion
        // checkpoint at the end of every mount (RC08), so a stream with no
        // segments at all is a log directory missing a file. Answering
        // "abort" here could discard a transaction the coordinator
        // committed and acknowledged.
        return Status::Corruption(
            "cross-owner recovery: core " + std::to_string(prepared.coordinator_core) +
            " has no stream in " + wal_dir_ + ", so the outcome of transaction " +
            std::to_string(participant_txn_id) + " - prepared here at lsn " +
            std::to_string(prepared.prepare_lsn) +
            " - cannot be read; the decision is that core's and this mount will not guess it");
    }
    ++streams_read_;

    // The whole stream, from the beginning. There is no sound lower bound:
    // a bound taken from this stream's LSNs would be exactly the
    // cross-stream comparison guideline 3 forbids, and one taken from the
    // coordinator's checkpoint would assume the decision is above it. The
    // cost is one full scan per prepared transaction, paid only by a mount
    // that has one - which is a crash mid-protocol, not an ordinary boot.
    bool decided = false;
    wal::TxnOutcome verdict = wal::TxnOutcome::kLoser;
    std::uint64_t scanned = 0;
    const auto visit = [&](const wal::DecodedRecord& record) -> Status {
        ++scanned;
        if (record.header.txn_id != prepared.coordinator_txn_id) return Status::OK();
        if (record.type() == wal::RecordType::kTxnCommit) {
            verdict = wal::TxnOutcome::kWinner;
            decided = true;
        } else if (record.type() == wal::RecordType::kTxnAbort) {
            verdict = wal::TxnOutcome::kLoser;
            decided = true;
        }
        // The scan runs on to the end rather than stopping at the first
        // match. One transaction id has at most one terminal record - the
        // persisted trx-id ceiling means an id is never reused - so this
        // costs the tail of a scan and buys the guarantee that a *second*
        // terminal record would be seen rather than hidden behind the
        // first. If one ever is, the last wins and the stream is a
        // different problem than this function's.
        return Status::OK();
    };
    auto scan = wal::ScanLog(*device.value(), prepared.coordinator_core, /*from_lsn=*/0, visit);
    if (!scan.ok()) {
        return scan.status().WithContext("cross-owner recovery: scanning core " +
                                         std::to_string(prepared.coordinator_core) +
                                         "'s stream for transaction " +
                                         std::to_string(prepared.coordinator_txn_id));
    }
    records_scanned_ += scanned;

    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("recovery",
                   "core " + std::to_string(core_id_) + ": transaction " +
                       std::to_string(participant_txn_id) + ", prepared for core " +
                       std::to_string(prepared.coordinator_core) + "'s session " +
                       std::to_string(prepared.coordinator_session_id) + " transaction " +
                       std::to_string(prepared.coordinator_txn_id) + ", resolves to " +
                       (verdict == wal::TxnOutcome::kWinner ? "COMMIT" : "ABORT") +
                       (decided ? " (its coordinator's decision)"
                                : " (its coordinator's stream holds no decision, so nothing "
                                  "committed anywhere)") +
                       " after " + std::to_string(scanned) + " record(s)" +
                       (scan.value().stopped_early ? "; that stream's tail was torn" : ""));
    }
    return verdict;
}

}  // namespace kds::server
