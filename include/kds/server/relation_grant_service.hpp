#pragma once

#include <cstdint>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"

// Re-delivery of a relation's grants over the ring
// (`RingMessageKind::kRelationGrantRequest`; docs/inflight/in-progress/workplan-peer-writer.md
// PW1c-7).
//
// Every fault grant and write grant a peer holds is memory-resident, and
// PW1c-4's publish sends them exactly once, at CREATE TABLE. Three things
// therefore leave a relation with an owner and no writer: a crash before
// the owner's acquisition restamp (PL §9 rule 6) made the creation pages
// its own, a restart, and a message lost to a full ring. Pages the owner
// wrote itself need nothing - their stream stamp claims them at the next
// fault (device_page_store.hpp, MayWrite) - so what is missing is only ever
// the creation pages core 0 formatted, and only core 0 can hand those off:
// PL §9 rule 1 puts the handoff record in the *giver's* stream.
//
// So the owner asks. The demand is recorded where it is found - the
// dispatcher's rights probe, on a write the shape gate admitted - and the
// drain tick sends one request per relation. Core 0 answers by running the
// publish sequence a CREATE TABLE runs (flush, durable PAGE_HANDOFF
// records, fault grant, write grant): a second handoff record for a page
// is idempotent at analysis (PW1c-4's one-liner), and the grants land
// through the receivers PW1c-4 already installed. No reply on this kind
// and no waiter; one request in flight per core (CoreRuntime's latch, the
// PW1c-7 review's C4 - each request costs core 0 a catalog scan, an extent
// flush and an fsync), released when a write grant is admitted or after a
// bounded number of ticks, so a request core 0 dropped can be asked again
// by the next refused statement.

namespace kds::server {

// Wire form. POD, under ring_message.hpp's exception to the on-disk layout
// rules: it never leaves the process.
struct RelationGrantRequestPayload {
    std::uint64_t table_oid;
};
static_assert(sizeof(RelationGrantRequestPayload) == 8);

// The demand sink the dispatcher writes into is `RelationGrantDemand`
// (core_affinity.hpp - the dispatcher's only dependency on this path, kept
// out of this header so the scheduler and transport stay out of
// command_dispatcher.hpp). This file is the ring half.

// Installs core 0's responder. `publish` is the relation publish hook
// itself - the one sequence that hands a relation to its owner, shared
// with CREATE TABLE rather than copied - and it is called only for a
// relation whose `sys.tables.owner_core` is the requesting core: a request
// for another core's relation, or for no relation, is logged and dropped,
// since re-delivering rights to the wrong core would be exactly the
// two-writer route PW1c exists to close. `catalog` and `publish` must
// outlive the scheduler.
Status RegisterRelationGrantHandler(sched::Scheduler& system_scheduler,
                                    catalog::Catalog& catalog,
                                    catalog::Catalog::RelationPublishHook publish,
                                    Logger* log = nullptr);

// The owner's ask: one send-retry task naming `table_oid`, submitted on
// `scheduler`. Fire-and-forget by design (see the header comment).
void RequestRelationGrant(sched::Scheduler& scheduler, sched::RingTransport& transport,
                          catalog::Oid table_oid, std::uint32_t core_id,
                          std::uint32_t system_core = 0);

}  // namespace kds::server
