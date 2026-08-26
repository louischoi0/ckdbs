#pragma once

#include <cstdint>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/lease_refill_stats.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/trx_id_lease.hpp"

// The transaction-id lease over the ring (`RingMessageKind::kTrxIdLease`,
// reserved by workplan P1 and unsent until now): how a peer that may not
// write the superblock obtains blocks of transaction ids
// (`docs/inflight/in-progress/workplan-peer-writer.md` PW1).
//
// `row_id_lease_service.hpp`'s shape exactly, applied to the sequence that
// is per-instance rather than per-relation - which is why no payload here
// carries an oid.
//
// ---- Why this is the first thing PW1 builds -----------------------------
//
// `TrxIdSequence` constructs spent (`next_ == ceiling_`), so a peer's very
// first `Next()` reserves a block, and before this service a peer's persist
// callback refused it. Every peer write died at its first id, ahead of any
// page. Reads never noticed: a read view mints from `peek()`, which issues
// nothing.
//
// ---- The block size, and why nobody re-decides it -----------------------
//
// `kTrxIdBlockSize` = 4096, the floor `docs/rules/keystoneid-invariant.md` K-M2
// established by measurement (below it the durable bump stops amortizing).
// The grant reuses it rather than introducing a second number for the same
// question. A parameter everywhere, like every such constant here.
//
// ---- Asking early is not an optimization --------------------------------
//
// `TrxIdSequence::Next()` runs inside a statement and **cannot await**, so a
// peer must request its next block while the current one still has ids -
// `storage/extent_lease.hpp`'s rule, and the reason `low_water()` exists. By
// the time `Next()` reports exhaustion it is already too late for that
// statement, which is why exhaustion is retryable rather than fatal.

namespace kds::server {

// One grant's worth of ids. `txn::kTrxIdBlockSize` rather than a constant of
// this service's own, for the reason in the header.
inline constexpr std::uint64_t kTrxIdLeasePerGrant = txn::kTrxIdBlockSize;

// Wire form. POD, under ring_message.hpp's exception to the on-disk layout
// rules: it never leaves the process.
//
// **The request carries nothing, and that is a decision.** How many ids a
// grant is worth belongs to the core that owns the sequence, not to the one
// asking - `RegisterExtentGrantHandler` fixes `pages_per_grant` at
// registration for the same reason. Here it is also a bound: `Carve` clamps
// at `kMaxTrxId + 1` and grants whatever it clamped to, so a requested count
// would let one malformed message consume the instance's whole 48-bit space,
// which invariant 12 forbids ever reclaiming. The row-id service takes a
// count on the wire; that is the sibling this one deliberately does not copy.
struct TrxIdLeaseGrantPayload {
    std::uint64_t first_id;
    std::uint64_t count;  // 0 = none available; the id space is exhausted
};
static_assert(sizeof(TrxIdLeaseGrantPayload) == 16);

// Installs core 0's responder: a peer's request is answered with a block of
// `ids_per_grant` from `ids.Carve()`, which raises the superblock's ceiling
// and makes it durable **before** replying. A carve that fails replies with a
// zero-count grant rather than dropping silently - the requester is waiting,
// and a reply it can read as "none" is what lets it fail a statement honestly.
//
// `ids` is core 0's own sequence, deliberately: sharing the carve is what
// keeps the two consumers of one ceiling from colliding (trx_id.hpp).
Status RegisterTrxIdGrantHandler(sched::Scheduler& system_scheduler,
                                 sched::RingTransport& transport, txn::TrxIdSequence& ids,
                                 std::uint64_t ids_per_grant = kTrxIdLeasePerGrant,
                                 Logger* log = nullptr);

// One core's refill state, owned by the caller for the coroutine's reason
// (extent_lease_service.hpp): it must outlive the wait.
struct TrxIdRefill {
    bool granted = false;
    std::uint64_t first_id = 0;
    std::uint64_t count = 0;
    LeaseRefillStats stats;  // requests, grants, and what each cost
};

// Installs a peer's reply handler: records the grant into `refill`, applies
// it to `lease`, and releases the waiting coroutine. Applying here rather
// than in the coroutine means a grant is never lost to a caller that stopped
// waiting. `clock`, when given, stamps the grant's arrival into the stats.
Status RegisterTrxIdGrantReceiver(sched::Scheduler& scheduler, TrxIdRefill& refill,
                                  txn::TrxIdLease& lease, Logger* log = nullptr);

// The coroutine that asks for a block. Submit at `low_water()`; one in
// flight per core, the extent refill's rule. It names no size - see the
// payload above.
sched::Coro RequestTrxIdLease(sched::RingTransport& transport, TrxIdRefill& refill,
                              std::uint32_t core_id, std::uint32_t system_core = 0,
                              Logger* log = nullptr,
                              const sched::Scheduler* sched = nullptr);

}  // namespace kds::server
