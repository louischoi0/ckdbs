#pragma once

#include <cstdint>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/catalog/row_id_lease.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/lease_refill_stats.hpp"

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
};
static_assert(sizeof(RowIdLeaseRequestPayload) == 16);

struct RowIdLeaseGrantPayload {
    std::uint64_t table_oid;
    std::uint64_t first_id;
    std::uint64_t count;  // 0 = none available; the id space is exhausted
};
static_assert(sizeof(RowIdLeaseGrantPayload) == 24);

// Installs core 0's responder: a peer's request is answered with a block
// carved by `Catalog::AllocateRowIdRange()` - the bulk-INSERT primitive,
// already exhaustion-checked against the 40-bit ceiling. A carve that
// fails replies with a zero-count grant rather than silently dropping,
// for the extent service's reason: the requester is waiting, and a reply
// it can read as "none" is what lets it fail a statement honestly.
Status RegisterRowIdGrantHandler(sched::Scheduler& system_scheduler,
                                 sched::RingTransport& transport, catalog::Catalog& catalog,
                                 Logger* log = nullptr);

// One core's refill state, owned by the caller for the coroutine's reason
// (extent_lease_service.hpp): it must outlive the wait.
struct RowIdRefill {
    bool granted = false;
    std::uint64_t table_oid = 0;
    std::uint64_t first_id = 0;
    std::uint64_t count = 0;
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
sched::Coro RequestRowIdLease(sched::RingTransport& transport, RowIdRefill& refill,
                              std::uint64_t table_oid, std::uint64_t count,
                              std::uint32_t core_id, std::uint32_t system_core = 0,
                              Logger* log = nullptr,
                              const sched::Scheduler* sched = nullptr);

}  // namespace kds::server
