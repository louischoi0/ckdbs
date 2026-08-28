#pragma once

#include <cstdint>
#include <string>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/wal/analysis.hpp"
#include "kds/wal/recovery.hpp"

// **Reading a prepared transaction's verdict out of its coordinator's
// stream** (R6-4 of `instructions/v2.4.0/2pc.md`, D4).
//
// A participant that replied *prepared* and then stopped holds a
// transaction it may neither commit nor abort: the decision is the
// coordinator's `TXN_COMMIT`, and by D4 it lives in exactly one stream.
// Analysis marks such a transaction `kPrepared`; this is what turns that
// into a verdict, and it is deliberately the whole of what recovery does
// across a stream boundary.
//
// ---- The lookup, and why it is not a cross-stream comparison -------------
//
// `wal.md` guideline 3 forbids ordering two streams' records against each
// other, and nothing here does. The participant's own stream says what to
// redo; the coordinator's says whether the transaction committed, found by
// **scanning for a transaction id**. Two independent reads. No LSN from one
// stream is ever compared with an LSN from the other - the prepare's LSN is
// carried only so a refusal can name it.
//
// ---- Why an absent decision is an abort, and not a guess -----------------
//
// The scan starts at LSN 0 and reads the coordinator's whole stream, so
// "no COMMIT and no ABORT for this id" is a fact about every byte that core
// ever made durable, not an inference from a record that might not have
// been written. Nothing recycles a segment in this engine
// (`file_log_device.hpp` describes recycling as a whole-segment operation
// nothing performs), so the stream a mount reads is the stream from the
// beginning of the database.
//
// And the two sides reach that verdict **independently**: a coordinator
// with no COMMIT record for its own transaction rolls that transaction back
// at its own mount, because analysis calls a transaction with no terminal
// record a loser. So participant and coordinator abort the same transaction
// for the same reason, from their own streams, with no message between
// them. That is what makes this sound rather than presumed.
//
// ---- What refuses the mount ---------------------------------------------
//
// **An absent coordinator stream.** Every core publishes a completion
// checkpoint at the end of every mount (RC08), so a core of this database
// with no segment files at all is a log directory that has lost a file -
// not a core that never wrote. Answering "abort" there would discard a
// transaction that may have been committed and acknowledged to a client, so
// it refuses and names the core.
//
// **A prepare naming this core as its own coordinator**, or naming a core
// this instance does not have. The first is a malformed record - a
// coordinator never writes TXN_PREPARE, only participants do - and the
// second cannot survive the pinned core count (see the CP1 note in
// `docs/inflight/in-progress/workplan-cross-owner-txn.md`), so both are
// corruption rather than conditions to work around.

namespace kds::server {

class CoordinatorStreamResolver final : public wal::PreparedResolver {
public:
    // `wal_dir` is the directory this instance's streams live in, and
    // `segment_size` must be the one they were written with - taken from
    // the device recovery is already reading, so the two cannot disagree.
    // `core_id` is the participant's own, held only to recognise a record
    // that names it as its own coordinator.
    CoordinatorStreamResolver(std::string wal_dir, std::uint64_t segment_size,
                              std::uint32_t core_id, Logger* log = nullptr) noexcept
        : wal_dir_(std::move(wal_dir)),
          segment_size_(segment_size),
          core_id_(core_id),
          log_(log) {}

    StatusOr<wal::TxnOutcome> Resolve(std::uint64_t participant_txn_id,
                                      const wal::PreparedTxn& prepared) override;

    // Coordinator streams opened, which is one per resolution rather than
    // one per coordinator: a mount with two prepared transactions on one
    // coordinator opens it twice. Deliberate - the population is bounded by
    // `kShippedMaxEnrolled` per core and a mount is not a hot path, and a
    // cache would have to be invalidated by nothing at all, which is the
    // shape that rots.
    std::uint64_t streams_read() const noexcept { return streams_read_; }
    std::uint64_t records_scanned() const noexcept { return records_scanned_; }

private:
    std::string wal_dir_;
    std::uint64_t segment_size_;
    std::uint32_t core_id_;
    Logger* log_;
    std::uint64_t streams_read_ = 0;
    std::uint64_t records_scanned_ = 0;
};

}  // namespace kds::server
