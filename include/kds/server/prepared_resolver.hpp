#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/server/superblock.hpp"
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
// **Provided it is all still there**, which is a separate claim and needs
// its own check. `Analyze` refuses a scan of its *own* stream that ends
// before the durable point its anchor was published with, because a log
// that lost the records the anchor depends on yields no records and looks
// exactly like a clean one (`analysis.hpp`). A scan of somebody else's
// stream owes the same check for a sharper reason: a coordinator stream
// whose last segment is gone, or whose tail the crash tore, would read as
// "no decision" - an abort - and durably contradict a coordinator that
// committed and told a client so. So the resolution takes the
// coordinator's anchor too, and refuses when the scan falls short of it.
//
// ---- Why the verdict does not depend on who recovers first ---------------
//
// Core 0 recovers before its peers, so a participant may read a
// coordinator's stream before or after that coordinator's own recovery has
// run. It does not matter: a durable `TXN_COMMIT` is stable, and both "no
// decision yet" and "the coordinator's recovery rolled it back and wrote
// `TXN_ABORT`" resolve to the same verdict here. There is no ordering
// between the two mounts to get wrong, which is guideline 3's shape one
// level up.
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
// second is refused against the anchor table's size rather than left to
// produce an absent-stream refusal by accident.
//
// ---- One precondition, stated rather than relied on ----------------------
//
// This opens **another core's** log device, which `file_log_device.hpp`
// calls core-local. It is sound because every mount and completion
// checkpoint runs on the startup thread, in sequence, before any reactor
// worker exists (`expeditor.cpp` spawns them after every core is built) -
// so no other thread is appending to the stream being scanned. If that ever
// stops being true, a segment roll racing this scan is observed as a
// short segment file and refuses the mount, which is a spurious refusal
// rather than a wrong answer, but a refusal all the same.

namespace kds::server {

class CoordinatorStreamResolver final : public wal::PreparedResolver {
public:
    // `wal_dir` is the directory this instance's streams live in, and
    // `segment_size` must be the one they were written with - taken from
    // the device recovery is already reading, so the two cannot disagree.
    // `core_id` is the participant's own, held only to recognise a record
    // that names it as its own coordinator. `anchors` is every core's
    // checkpoint anchor, indexed by core id: it bounds which cores exist
    // and carries the durable point each coordinator's scan must reach.
    CoordinatorStreamResolver(std::string wal_dir, std::uint64_t segment_size,
                              std::uint32_t core_id, std::vector<WalAnchorFields> anchors,
                              Logger* log = nullptr) noexcept
        : wal_dir_(std::move(wal_dir)),
          segment_size_(segment_size),
          core_id_(core_id),
          anchors_(std::move(anchors)),
          log_(log) {}

    // **Every prepared transaction at once**, grouped by coordinator, so a
    // coordinator's stream is scanned **once** however many of this core's
    // transactions it decided. The alternative - one call, one scan - is up
    // to `kShippedMaxEnrolled` full scans of a stream that may be tens of
    // gigabytes, at a mount that is already the worst this engine has.
    //
    // The answer holds one verdict per input id. A missing entry is not
    // possible: a resolution either produces a verdict for every id or
    // fails, and a failure is the mount's answer.
    StatusOr<std::map<std::uint64_t, wal::TxnOutcome>> ResolveAll(
        const std::map<std::uint64_t, wal::PreparedTxn>& prepared) override;

    // Coordinator streams opened - **one per coordinator**, not one per
    // transaction, which is what `ResolveAll` exists to make true and what
    // a test asserts. Nothing is cached between mounts: there would be
    // nothing to invalidate it, which is the shape that rots.
    std::uint64_t streams_read() const noexcept { return streams_read_; }
    // Records read across all of them, so the cost of a resolution is
    // visible rather than inferred from the log line.
    std::uint64_t records_scanned() const noexcept { return records_scanned_; }

private:
    // One coordinator's stream, and every transaction of this core's that it
    // decided. `ResolveAll` groups into these first so each stream is
    // opened and scanned once.
    Status ResolveOneStream(std::uint32_t coordinator_core,
                            const std::map<std::uint64_t, wal::PreparedTxn>& mine,
                            std::map<std::uint64_t, wal::TxnOutcome>& out);

    std::string wal_dir_;
    std::uint64_t segment_size_;
    std::uint32_t core_id_;
    std::vector<WalAnchorFields> anchors_;
    Logger* log_;
    std::uint64_t streams_read_ = 0;
    std::uint64_t records_scanned_ = 0;
};

}  // namespace kds::server
