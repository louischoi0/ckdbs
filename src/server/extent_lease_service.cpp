#include "kds/server/extent_lease_service.hpp"

#include <cstring>
#include <string>

#include "kds/sched/send_retry.hpp"

namespace kds::server {

Status RegisterExtentGrantHandler(sched::Scheduler& system_scheduler,
                                  sched::RingTransport& transport,
                                  storage::ExtentAllocator& allocator,
                                  std::uint32_t pages_per_grant, Logger* log) {
    return system_scheduler.RegisterMessageHandler(
        sched::RingMessageKind::kExtentLease,
        [&system_scheduler, &transport, &allocator, pages_per_grant, log](
            const sched::MessageHeader& header, std::span<const std::byte>) {
            ExtentGrantPayload grant{};
            auto reserved = allocator.Reserve(pages_per_grant);
            if (reserved.ok()) {
                grant.first_page_id = reserved.value().first;
                grant.page_count = reserved.value().count;
            } else if (log != nullptr && log->enabled(LogLevel::kError)) {
                // A zero-page grant goes out regardless (see the header):
                // the requester is waiting, and a reply it can read as
                // "none available" is what lets it fail honestly instead of
                // waiting forever.
                log->Error("extent", "cannot grant core " + std::to_string(header.src_core) +
                                         " an extent: " + reserved.status().message());
            }

            std::byte bytes[sizeof(ExtentGrantPayload)];
            std::memcpy(bytes, &grant, sizeof(grant));

            sched::MessageHeader reply{};
            reply.src_core = header.dst_core;
            reply.dst_core = header.src_core;
            reply.session_core = header.session_core;
            reply.request_id = header.request_id;
            reply.kind = static_cast<std::uint16_t>(sched::RingMessageKind::kExtentLease);
            reply.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);

            // Through the retry task: a momentarily full ring must not lose
            // a grant somebody is waiting on (sched.md §5 forbids the drop).
            system_scheduler.Submit(sched::MakeSendRetryTask(
                transport, reply, std::span<const std::byte>(bytes, sizeof(bytes))));
        });
}

Status RegisterExtentGrantReceiver(sched::Scheduler& scheduler, ExtentRefill& refill, Logger* log,
                                   const sched::Clock* clock) {
    return scheduler.RegisterMessageHandler(
        sched::RingMessageKind::kExtentLease,
        [&refill, &scheduler, log, clock](const sched::MessageHeader& header,
                              std::span<const std::byte> payload) {
            if (clock != nullptr) refill.stats.granted_at_ns = clock->Now();
            refill.stats.granted_iter = scheduler.iterations();
            if (payload.size() != sizeof(ExtentGrantPayload)) {
                if (log != nullptr && log->enabled(LogLevel::kError)) {
                    log->Error("extent", "grant from core " + std::to_string(header.src_core) +
                                             " has " + std::to_string(payload.size()) +
                                             " bytes, not " +
                                             std::to_string(sizeof(ExtentGrantPayload)));
                }
                // Still released: a malformed grant that left the waiter
                // parked would hang the core that asked. It wakes, sees a
                // zero extent, and reports.
                refill.extent = storage::Extent{};
                refill.granted = true;
                return;
            }
            ExtentGrantPayload fields{};
            std::memcpy(&fields, payload.data(), sizeof(fields));
            refill.extent = storage::Extent{fields.first_page_id, fields.page_count};
            ++refill.stats.grants;
            refill.granted = true;
        });
}

sched::Coro RequestExtentRefill(sched::RingTransport& transport, storage::LeasedIdSource& lease,
                                ExtentRefill& refill, std::uint32_t core_id,
                                std::uint32_t system_core, Logger* log,
                                const sched::Clock* clock, const sched::Scheduler* sched) {
    refill.granted = false;
    refill.extent = storage::Extent{};
    ++refill.stats.requests;
    if (clock != nullptr) refill.stats.sent_at_ns = clock->Now();
    if (sched != nullptr) refill.stats.sent_iter = sched->iterations();

    sched::MessageHeader header{};
    header.src_core = core_id;
    header.dst_core = system_core;
    header.session_core = core_id;
    header.kind = static_cast<std::uint16_t>(sched::RingMessageKind::kExtentLease);
    header.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);

    if (Status s = transport.TrySend(header, {}); !s.ok()) {
        // Not retried here: a full ring on a *request* is different from a
        // full ring on a grant. Nobody is waiting on this yet, the lease
        // still has ids, and the next low-water check will ask again -
        // which is cheaper and simpler than parking a coroutine on a send.
        co_return s;
    }

    // The line this whole decision was made for.
    co_await sched::WaitFor{&refill.granted};

    if (refill.extent.empty()) {
        co_return Status::ResourceExhausted(
            "core " + std::to_string(core_id) +
            " asked the system core for an extent and was granted none; the free map is full");
    }

    lease.Grant(refill.extent);
    if (log != nullptr && log->enabled(LogLevel::kDebug)) {
        log->Debug("extent", "core " + std::to_string(core_id) + " leased " +
                                 std::to_string(refill.extent.count) + " pages from " +
                                 std::to_string(refill.extent.first));
    }
    co_return Status::OK();
}

}  // namespace kds::server
