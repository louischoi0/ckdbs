#pragma once

#include <array>
#include <cstdint>

#include "kds/base/status.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

// The one TXN_PREPARE emitter (R6-3; `log_page_handoff.hpp`'s shape, for
// its reason - hand-copied appends of the same record are how six PAGE_INIT
// emitters happened).
//
// **The durability is the caller's, and it is the whole point of the
// record.** This function appends; it does not sync. A prepare is a promise
// that everything this transaction wrote is on the platter, so the caller
// must not reply *prepared* until `IsDurable(lsn)` answers true for the LSN
// returned here - which, because one stream's LSNs are ordered, covers
// every record the transaction wrote before it. `WalManager::RequestDurable`
// is how a parked caller gets the drain to do that without blocking the
// reactor.
//
// The envelope's txn_id is the **participant's own** local transaction id
// (D2), never the coordinator's: no foreign id enters this stream, which is
// the invariant `CoreRuntime::Open`'s mount check enforces from the other
// side. The coordinator's identity travels in the payload instead.
//
// A null `wal` answers kNoLsn, matching every sibling emitter. What that
// means here is narrower than for a page record and is stated rather than
// left to be discovered: an unlogged instance recovers nothing, so a
// participant on one cannot be resolved after a crash - it is the fixture
// case, not a durability class.

namespace kds::wal {

inline StatusOr<Lsn> LogTxnPrepare(WalManager* wal, std::uint64_t participant_txn_id,
                                   std::uint32_t coordinator_core,
                                   std::uint64_t coordinator_session_id,
                                   std::uint64_t coordinator_txn_id) {
    if (wal == nullptr) return kNoLsn;
    std::array<std::byte, kTxnPreparePayloadSize> buf{};
    const TxnPreparePayload fields{coordinator_session_id, coordinator_txn_id,
                                   coordinator_core};
    if (auto n = EncodeTxnPrepare(buf, fields); !n.ok()) return n.status();
    return wal->Append(RecordSpec{RecordType::kTxnPrepare, participant_txn_id, kInvalidPageId},
                       buf);
}

}  // namespace kds::wal
