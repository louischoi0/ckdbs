#pragma once

#include <cstdint>
#include <span>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/lease_refill_stats.hpp"
#include "kds/storage/extent_lease.hpp"

// Refilling a core's page-id lease over the ring (docs/inflight/in-progress/workplan-crosscore.md
// M5 and P5, `RingMessageKind::kExtentLease`).
//
// P2 built the lease and left the refill out, because a refill is a request
// whose answer arrives later and there was no way to write one. This is the
// first thing in the engine built on the coroutine decision
// (`docs/spec/sched.md` §3, `sched/coro.hpp`): `co_await WaitFor{&granted}`, in
// straight-line code, on a reactor that never blocks.
//
// ---- Why the refill is a background task and not part of allocation -----
//
// `DevicePageStore::CreateNew()` cannot await anything - it is called from
// 22 sites deep inside btree splits, heap-chain growth and var-heap appends,
// none of which are coroutines and none of which should become ones. That
// is the whole reason leases exist (`storage/extent_lease.hpp`).
//
// So the refill runs *beside* allocation, not inside it: a core asks for the
// next extent when its current one crosses `low_water()`, while it can still
// hand out ids, and the grant lands before the lease is spent. A core that
// does run dry first gets `TxnConflict` from `Next()`, which is
// retryable - the statement fails, the refill is already in flight, and the
// retry succeeds. That is the same bump-ahead trade `txn::TrxIdSequence`
// makes for transaction ids.
//
// ---- The system core's half --------------------------------------------
//
// Core 0 owns the free map (M5), so it is the only core that can carve an
// extent. `RegisterGrantHandler` installs the responder; the reservation is
// synchronous there, because on core 0 it is a local call.

namespace kds::server {

// The wire form of a grant. POD, like every ring payload, and under
// `ring_message.hpp`'s exception to the on-disk layout rules: it never
// leaves the process.
struct ExtentGrantPayload {
    std::uint32_t first_page_id;
    std::uint32_t page_count;
};

static_assert(sizeof(ExtentGrantPayload) == 8);

// The wire form of PW1c-4's exact-page write grant (kRelationWriteGrant,
// workplan-peer-writer.md §8 rule 1): the pages core 0 formatted for a
// relation the receiving core owns, sent only after their PAGE_HANDOFF
// records are durable in core 0's stream (PL §9 rule 1 - the send site
// keeps the ordering). Exact pages, never an extent: the superset that is
// safe to fault is not safe to write. Fixed capacity because a ring
// payload is POD: today's population is the root plus the var-heap root
// when the schema can spill, and the headroom is for PW1c-6's index pages
// without a wire change. A relation needing more than the capacity is
// refused loudly at the send site, never truncated.
struct RelationWriteGrantPayload {
    static constexpr std::uint32_t kMaxPages = 6;
    std::uint32_t count;
    std::uint32_t page_ids[kMaxPages];  // PageIds; entries past `count` are zero
};

static_assert(sizeof(RelationWriteGrantPayload) == 28);

// Installs core 0's responder: a peer's `kExtentLease` request is answered
// with a grant carved from the free map, **made durable before it leaves**
// (`ExtentAllocator::Persist`, PW3b's finding in extent_lease.hpp): the
// peer will commit rows into the run, so the run must be allocated on the
// device before the peer can hold them, or a crash frees it for the next
// mount's allocator to hand out over them. One map write and one device
// sync per extent, on core 0's reactor.
//
// `allocator` and `transport` must outlive the scheduler. A reservation that
// fails - the map is full, or the map could not be made durable - replies
// with a **zero-page grant** rather than silently dropping: the requester is
// waiting, and a reply it can recognize as "none available" is what lets it
// stop waiting and fail a statement honestly instead of hanging.
Status RegisterExtentGrantHandler(sched::Scheduler& system_scheduler,
                                  sched::RingTransport& transport,
                                  storage::ExtentAllocator& allocator, std::uint32_t pages_per_grant,
                                  Logger* log = nullptr);

// One core's refill state: the flag the reply sets and the extent it
// carries. It has to outlive the coroutine that waits on it, which is why
// it is a named type the caller owns rather than a local.
struct ExtentRefill {
    bool granted = false;
    storage::Extent extent{};
    // Requests made, grants received, and what each cost (lease_refill_stats.hpp).
    LeaseRefillStats stats;
};

// Installs a peer's reply handler, which records the grant and releases the
// waiting coroutine. `clock`, when given, stamps the grant's arrival into
// `refill.stats` - the middle of the three legs the stats measure.
Status RegisterExtentGrantReceiver(sched::Scheduler& scheduler, ExtentRefill& refill,
                                   Logger* log = nullptr);

// The coroutine that asks. Submit it when `lease.low_water()` first turns
// true; it sends the request, waits for the grant, and applies it.
//
// Returns `ResourceExhausted` if the system core answered with no pages -
// the device is out - and the caller's lease is left exactly as it was, so
// it keeps issuing whatever it still holds.
sched::Coro RequestExtentRefill(sched::RingTransport& transport, storage::LeasedIdSource& lease,
                                ExtentRefill& refill, std::uint32_t core_id,
                                std::uint32_t system_core = 0, Logger* log = nullptr,
                                const sched::Scheduler* sched = nullptr);

}  // namespace kds::server
