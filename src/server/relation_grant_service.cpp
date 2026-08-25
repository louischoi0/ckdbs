#include "kds/server/relation_grant_service.hpp"

#include <cstring>
#include <string>

#include "kds/sched/send_retry.hpp"

namespace kds::server {

Status RegisterRelationGrantHandler(sched::Scheduler& system_scheduler,
                                    catalog::Catalog& catalog,
                                    catalog::Catalog::RelationPublishHook publish,
                                    Logger* log) {
    return system_scheduler.RegisterMessageHandler(
        sched::RingMessageKind::kRelationGrantRequest,
        [&catalog, publish = std::move(publish), log](const sched::MessageHeader& header,
                                                       std::span<const std::byte> payload) {
            RelationGrantRequestPayload request{};
            if (payload.size() != sizeof(request)) {
                if (log != nullptr && log->enabled(LogLevel::kError)) {
                    log->Error("grant", "relation grant request from core " +
                                            std::to_string(header.src_core) + " has " +
                                            std::to_string(payload.size()) + " bytes, not " +
                                            std::to_string(sizeof(request)));
                }
                return;
            }
            std::memcpy(&request, payload.data(), sizeof(request));
            const auto oid = static_cast<catalog::Oid>(request.table_oid);

            // The row is the authority on who owns the relation (CC7), and
            // it is read here rather than trusted from the requester: a
            // grant to a core the catalog does not name would be the
            // two-writer route. A dropped relation still carries its row
            // and its owner; re-publishing its orphaned pages to that owner
            // is harmless (nothing resolves them) and not worth a second
            // lookup on this path.
            auto row = catalog.GetSysTableRow(oid);
            if (!row.ok() || row.value().owner_core != header.src_core) {
                if (log != nullptr && log->enabled(LogLevel::kError)) {
                    log->Error("grant",
                               "core " + std::to_string(header.src_core) +
                                   " asked for grants on oid " + std::to_string(oid) + ": " +
                                   (row.ok() ? "owned by core " +
                                                   std::to_string(row.value().owner_core)
                                             : row.status().message()) +
                                   "; dropped");
                }
                return;
            }
            if (log != nullptr && log->enabled(LogLevel::kInfo)) {
                log->Info("grant", "re-delivering relation oid=" + std::to_string(oid) +
                                       " to core " + std::to_string(header.src_core) +
                                       " on request (workplan-peer-writer.md PW1c-7)");
            }
            publish(oid, row.value().owner_core, row.value().desc_page_id,
                    row.value().varheap_page_id, row.value().anchor_page_id);
        });
}

void RequestRelationGrant(sched::Scheduler& scheduler, sched::RingTransport& transport,
                          catalog::Oid table_oid, std::uint32_t core_id,
                          std::uint32_t system_core) {
    const RelationGrantRequestPayload request{table_oid};
    std::byte bytes[sizeof(request)];
    std::memcpy(bytes, &request, sizeof(request));

    sched::MessageHeader header{};
    header.src_core = core_id;
    header.dst_core = system_core;
    header.session_core = core_id;
    header.kind = static_cast<std::uint16_t>(sched::RingMessageKind::kRelationGrantRequest);
    header.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);
    // A send-retry task, not TrySend: nobody waits on this request, but a
    // full ring dropping it silently would cost a whole client retry
    // cycle for nothing. The task copies the payload (send_retry.hpp).
    scheduler.Submit(sched::MakeSendRetryTask(transport, header,
                                              std::span<const std::byte>(bytes, sizeof(bytes))));
}

}  // namespace kds::server
