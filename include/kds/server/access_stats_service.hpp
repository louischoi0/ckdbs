#pragma once

#include <cstring>
#include <span>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/stats/access_batch.hpp"

// CR7/CR8 — the wire a peer's access statistics take to core 0.
//
// ---- The two halves ------------------------------------------------------
//
// `FlushAccessBatch` is the peer's: fold, send, clear. `RegisterAccessStats-
// BatchHandler` is core 0's: take the fold and apply it to the one
// `sys.access_stats` at page 11, which only core 0 may write (CC11).
//
// ---- The send is the one that may drop ----------------------------------
//
// Every other ring send in this engine retries (`sched/send_retry.hpp`, M7's
// yield-and-retry, which `sched.md` §5 makes the engine-wide answer to a
// full channel). This one does not, by CR8: `sys.access_stats` is invariant
// 8's advisory class - deleting it wholesale costs performance and never a
// result - so a batch that meets a full ring is dropped and counted. A retry
// here would put a yield loop behind a statistic, on a tick that runs on the
// statement path's own reactor.
//
// The counter is what keeps that honest: a drop is visible in `SHOW META`
// (`access_batches_dropped`), never silent.

namespace kds::server {

// Sends what the batch holds to `system_core` and clears it. A no-op on an
// empty batch, which is the ordinary case on an idle tick.
//
// Returns OK when the batch was sent **or** was empty; the drop is not an
// error to the caller either, since nothing above it can act on one - it is
// counted and reported, which is the whole of what CR8 asks.
inline Status FlushAccessBatch(sched::RingTransport& transport, std::uint32_t from_core,
                               std::uint32_t system_core, stats::AccessBatch& batch) {
    if (batch.empty()) return Status::OK();

    const std::span<const stats::AccessBatchEntry> entries = batch.entries();
    std::array<std::byte, sched::kCoreRingPayloadBytes> buffer{};
    stats::AccessBatchPayload head{};
    head.from_core = from_core;
    head.count = static_cast<std::uint32_t>(entries.size());
    std::memcpy(buffer.data(), &head, sizeof(head));
    std::memcpy(buffer.data() + sizeof(head), entries.data(),
                entries.size() * sizeof(stats::AccessBatchEntry));
    const std::size_t bytes = sizeof(head) + entries.size() * sizeof(stats::AccessBatchEntry);

    sched::MessageHeader header{};
    header.src_core = from_core;
    header.dst_core = system_core;
    header.session_core = from_core;
    header.kind = static_cast<std::uint16_t>(sched::RingMessageKind::kAccessStatsBatch);
    header.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);

    const Status sent = transport.TrySend(header, std::span<const std::byte>(buffer.data(), bytes));
    if (sent.ok()) {
        ++batch.counters().batches_sent;
        batch.counters().entries_sent += entries.size();
    } else {
        // CR8. The batch is dropped rather than retried or held: holding it
        // would grow one shape's count without bound and then flush a number
        // that names no interval, which is worse than the gap.
        ++batch.counters().batches_dropped;
    }
    batch.Clear();
    return Status::OK();
}

// Core 0's half: apply a peer's fold to `sys.access_stats`.
//
// `counters` may be null (a fixture that only wants the rows). It is core
// 0's own `SHOW META` block, so it counts what this core *applied*, never
// what a peer sent - the two differ by exactly the drops, which is the
// reading that makes a drop diagnosable from either end.
inline Status RegisterAccessStatsBatchHandler(sched::Scheduler& scheduler,
                                              catalog::Catalog& catalog,
                                              stats::AccessBatchCounters* counters) {
    return scheduler.RegisterMessageHandler(
        sched::RingMessageKind::kAccessStatsBatch,
        [&catalog, counters](const sched::MessageHeader&, std::span<const std::byte> payload) {
            if (payload.size() < sizeof(stats::AccessBatchPayload)) return;
            stats::AccessBatchPayload head{};
            std::memcpy(&head, payload.data(), sizeof(head));
            const std::size_t declared = static_cast<std::size_t>(head.count);
            const std::size_t available =
                (payload.size() - sizeof(head)) / sizeof(stats::AccessBatchEntry);
            // Bounded by what arrived, never by what the header claims: a
            // truncated message costs the entries it lost and nothing else.
            const std::size_t n = declared < available ? declared : available;
            for (std::size_t i = 0; i < n; ++i) {
                stats::AccessBatchEntry entry{};
                std::memcpy(&entry, payload.data() + sizeof(head) + i * sizeof(entry),
                            sizeof(entry));
                // Dropped on failure, exactly as the local path drops it
                // (`catalog.hpp`: a statistic that could fail a statement
                // would be the worse trade). The shape cap arrives here too.
                const Status applied = catalog.RecordAccess(
                    entry.kind, static_cast<catalog::Oid>(entry.rel_id), entry.column_mask,
                    entry.last_seen, entry.count);
                if (applied.ok() && counters != nullptr) ++counters->entries_applied;
            }
            if (counters != nullptr) ++counters->batches_applied;
        });
}

}  // namespace kds::server
