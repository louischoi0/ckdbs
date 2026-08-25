#include "kds/server/trx_id_lease_service.hpp"

#include <cstring>
#include <string>

#include "kds/sched/send_retry.hpp"

namespace kds::server {

Status RegisterTrxIdGrantHandler(sched::Scheduler& system_scheduler,
                                 sched::RingTransport& transport, txn::TrxIdSequence& ids,
                                 std::uint64_t ids_per_grant, Logger* log) {
    return system_scheduler.RegisterMessageHandler(
        sched::RingMessageKind::kTrxIdLease,
        [&system_scheduler, &transport, &ids, ids_per_grant, log](
            const sched::MessageHeader& header, std::span<const std::byte>) {
            // The request body is not read: the grant size is this core's,
            // fixed at registration (trx_id_lease_service.hpp). An empty
            // payload therefore cannot be malformed, and there is no size
            // check to fail closed on.
            TrxIdLeaseGrantPayload grant{};
            auto carved = ids.Carve(ids_per_grant);
            if (carved.ok()) {
                grant.first_id = carved.value().first;
                grant.count = carved.value().count;
            } else if (log != nullptr && log->enabled(LogLevel::kError)) {
                // The zero-count grant goes out regardless - the requester
                // is waiting, and "none" is an answer where silence is a
                // hang. Both an exhausted 48-bit space and a superblock
                // that could not be persisted land here; the peer's
                // statement fails with the honest error either way.
                log->Error("trxid", "cannot grant core " + std::to_string(header.src_core) +
                                        " transaction ids: " + carved.status().message());
            }

            std::byte bytes[sizeof(grant)];
            std::memcpy(bytes, &grant, sizeof(grant));

            sched::MessageHeader reply{};
            reply.src_core = header.dst_core;
            reply.dst_core = header.src_core;
            reply.session_core = header.session_core;
            reply.request_id = header.request_id;
            reply.kind = static_cast<std::uint16_t>(sched::RingMessageKind::kTrxIdLease);
            reply.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);
            system_scheduler.Submit(sched::MakeSendRetryTask(
                transport, reply, std::span<const std::byte>(bytes, sizeof(bytes))));
        });
}

Status RegisterTrxIdGrantReceiver(sched::Scheduler& scheduler, TrxIdRefill& refill,
                                  txn::TrxIdLease& lease, Logger* log) {
    return scheduler.RegisterMessageHandler(
        sched::RingMessageKind::kTrxIdLease,
        [&refill, &lease, &scheduler, log](const sched::MessageHeader& header,
                                      std::span<const std::byte> payload) {
            if (payload.size() != sizeof(TrxIdLeaseGrantPayload)) {
                if (log != nullptr && log->enabled(LogLevel::kError)) {
                    log->Error("trxid", "grant from core " + std::to_string(header.src_core) +
                                            " has " + std::to_string(payload.size()) +
                                            " bytes, not " +
                                            std::to_string(sizeof(TrxIdLeaseGrantPayload)));
                }
                // Released regardless, the extent receiver's rule: a
                // malformed grant that leaves the waiter parked hangs the
                // core. It wakes, sees count 0, and reports.
                refill.count = 0;
                refill.granted = true;
                return;
            }
            TrxIdLeaseGrantPayload fields{};
            std::memcpy(&fields, payload.data(), sizeof(fields));
            refill.first_id = fields.first_id;
            refill.count = fields.count;
            refill.stats.NoteGrant(scheduler.clock().Now(), scheduler.iterations());
            if (fields.count > 0) {
                lease.Grant(fields.first_id, fields.count);
                ++refill.stats.grants;
            }
            refill.granted = true;
        });
}

sched::Coro RequestTrxIdLease(sched::RingTransport& transport, TrxIdRefill& refill,
                              std::uint32_t core_id, std::uint32_t system_core, Logger* log,
                              const sched::Scheduler* sched) {
    refill.granted = false;
    refill.first_id = 0;
    refill.count = 0;
    refill.stats.NoteSent(sched != nullptr ? sched->clock().Now() : 0,
                          sched != nullptr ? sched->iterations() : 0);

    sched::MessageHeader header{};
    header.src_core = core_id;
    header.dst_core = system_core;
    header.session_core = core_id;
    header.kind = static_cast<std::uint16_t>(sched::RingMessageKind::kTrxIdLease);
    header.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);

    if (Status s = transport.TrySend(header, {}); !s.ok()) {
        // Not retried, the extent request's reason: nobody waits on a
        // request that never left, and the next low-water tick asks again.
        co_return s;
    }

    co_await sched::WaitFor{&refill.granted};

    if (refill.count == 0) {
        co_return Status::ResourceExhausted("core " + std::to_string(core_id) +
                                            " asked for transaction ids and was granted none");
    }
    if (log != nullptr && log->enabled(LogLevel::kDebug)) {
        log->Debug("trxid", "core " + std::to_string(core_id) + " leased " +
                                std::to_string(refill.count) + " transaction ids from " +
                                std::to_string(refill.first_id));
    }
    co_return Status::OK();
}

}  // namespace kds::server
