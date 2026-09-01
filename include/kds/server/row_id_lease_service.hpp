#pragma once

#include <cstdint>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/catalog/row_id_lease.hpp"
#include "kds/exec/assertion_check.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/lease_refill_stats.hpp"
#include "kds/server/refusal_counters.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/wal/manager.hpp"

// The row-id lease over the ring (`RingMessageKind::kRowIdLease`): how a
// peer that may not write the catalog obtains blocks of Keystone ids for
// one relation. The extent-lease service's shape exactly
// (extent_lease_service.hpp), applied to the sequence that is per-relation
// rather than per-instance - which is why every payload carries the oid.
//
// The block size default is `kRowIdLeasePerGrant` = 4096: the measured
// floor `docs/rules/keystoneid-invariant.md` K-M2 established for bump-ahead
// allocation (below it the durable bump stops amortizing), reused rather
// than re-decided. A parameter everywhere, like every such number.

namespace kds::server {

inline constexpr std::uint64_t kRowIdLeasePerGrant = 4096;

// Wire forms. POD, under ring_message.hpp's exception to the on-disk
// layout rules: they never leave the process.
struct RowIdLeaseRequestPayload {
    std::uint64_t table_oid;
    std::uint64_t count;
    // RD5: the owner asks for the granted block to become a **range** of
    // this relation, owned by this core, at the block's first id. Set only
    // where the owner's own `RangeEligible` said yes - the one core whose
    // answer is authoritative (§9c) - and honoured only where core 0's
    // re-check agrees. A request core 0 declines is still granted its ids:
    // the relation stays one range and the insert path is unchanged, which
    // is what makes the decline a refusal rather than a wrong answer.
    std::uint64_t open_range;
};
static_assert(sizeof(RowIdLeaseRequestPayload) == 24);

struct RowIdLeaseGrantPayload {
    std::uint64_t table_oid;
    std::uint64_t first_id;
    std::uint64_t count;  // 0 = none available; the id space is exhausted
    // The head page of the range core 0 opened at `first_id`, or
    // `kInvalidPageId` where none was opened - because the request did not
    // ask, or because the re-check declined. **The grant of that page's
    // write rights is this field**: core 0 formatted it and logged the
    // handoff before replying (CC10 steps 2-4), so the receiver admits it
    // exactly as it admits a relation write grant.
    std::uint64_t entry_page;
};
static_assert(sizeof(RowIdLeaseGrantPayload) == 32);

// Installs core 0's responder: a peer's request is answered with a block
// carved by `Catalog::AllocateRowIdRange()` - the bulk-INSERT primitive,
// already exhaustion-checked against the 40-bit ceiling. A carve that
// fails replies with a zero-count grant rather than silently dropping,
// for the extent service's reason: the requester is waiting, and a reply
// it can read as "none" is what lets it fail a statement honestly.
// `store`, `wal` and `enforcer` are RD5's: a request that asks for a range needs
// the handoff record and the eligibility re-check, and **a handler given
// no store or enforcer simply grants the ids and opens nothing** - which is what every
// fixture is, and what makes the range half additive rather than a new
// precondition on the lease.
// `cabins` and `discards` are SB1's, and they are core 0's own: the
// pre-grant Cabin discard runs inside `OpenRangeOnSystemCore`, on the same
// task, so the grant this handler replies with can never precede it. Both
// nullable on the same terms as `store` and `enforcer` above — a handler
// given no store grants the ids and discards nothing.
Status RegisterRowIdGrantHandler(sched::Scheduler& system_scheduler,
                                 sched::RingTransport& transport, catalog::Catalog& catalog,
                                 Logger* log = nullptr,
                                 storage::DevicePageStore* store = nullptr,
                                 wal::WalManager* wal = nullptr,
                                 const exec::AssertionEnforcer* enforcer = nullptr,
                                 stats::CabinStore* cabins = nullptr,
                                 CabinSplitDiscardCounters* discards = nullptr);

// One core's refill state, owned by the caller for the coroutine's reason
// (extent_lease_service.hpp): it must outlive the wait.
struct RowIdRefill {
    bool granted = false;
    std::uint64_t table_oid = 0;
    std::uint64_t first_id = 0;
    std::uint64_t count = 0;
    // RD5: the head page of the range core 0 opened at `first_id`, or
    // `kInvalidPageId` where it opened none. Read by the *caller* of the
    // coroutine rather than by the receiver, because admitting the page
    // needs this core's store and WAL, which the lease table has no
    // business holding.
    PageId entry_page = kInvalidPageId;
    LeaseRefillStats stats;  // requests, grants, and what each cost
};

// Installs a peer's reply handler: records the grant into `refill`,
// applies it to `leases`, and releases the waiting coroutine. Applying
// here rather than in the coroutine means a grant is never lost to a
// caller that stopped waiting. `clock`, when given, stamps the grant's
// arrival into the stats.
Status RegisterRowIdGrantReceiver(sched::Scheduler& scheduler, RowIdRefill& refill,
                                  catalog::RowIdLeaseTable& leases, Logger* log = nullptr);

// The coroutine that asks for a block for one relation. Submit on spent or
// low lease; one in flight per core, the extent refill's rule.
//
// `open_range` asks for the block to become a range (RD5). The caller sets
// it only where **this core's own** `RangeEligible` said yes, which is the
// only core whose answer is authoritative; core 0 re-checks and may
// decline, and the ids arrive either way.
sched::Coro RequestRowIdLease(sched::RingTransport& transport, RowIdRefill& refill,
                              std::uint64_t table_oid, std::uint64_t count,
                              std::uint32_t core_id, std::uint32_t system_core = 0,
                              Logger* log = nullptr, const sched::Scheduler* sched = nullptr,
                              bool open_range = false);

}  // namespace kds::server
