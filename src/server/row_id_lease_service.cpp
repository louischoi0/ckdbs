#include "kds/server/row_id_lease_service.hpp"

#include <cstring>
#include <string>

#include "kds/sched/send_retry.hpp"
#include "kds/server/range_alloc.hpp"

namespace kds::server {

Status RegisterRowIdGrantHandler(sched::Scheduler& system_scheduler,
                                 sched::RingTransport& transport, catalog::Catalog& catalog,
                                 Logger* log, storage::DevicePageStore* store,
                                 wal::WalManager* wal,
                                 const exec::AssertionEnforcer* enforcer) {
    return system_scheduler.RegisterMessageHandler(
        sched::RingMessageKind::kRowIdLease,
        [&system_scheduler, &transport, &catalog, log, store, wal, enforcer](
            const sched::MessageHeader& header, std::span<const std::byte> payload) {
            RowIdLeaseRequestPayload request{};
            if (payload.size() != sizeof(request)) {
                if (log != nullptr && log->enabled(LogLevel::kError)) {
                    log->Error("rowid", "lease request from core " +
                                            std::to_string(header.src_core) + " has " +
                                            std::to_string(payload.size()) + " bytes, not " +
                                            std::to_string(sizeof(request)));
                }
                return;  // nothing to reply to: the oid is unreadable
            }
            std::memcpy(&request, payload.data(), sizeof(request));

            RowIdLeaseGrantPayload grant{};
            grant.table_oid = request.table_oid;
            grant.entry_page = kInvalidPageId;
            // **`count == 0` and `open_range` are incompatible, and the
            // refusal is D6's one-key rule enforced on the wire.** The
            // substitution below is a courtesy for a requester that does
            // not care how many ids it gets; a requester asking for a
            // *range* does care, because the range's width is this carve's
            // width, and letting the substitution stand would open a range
            // of `kRowIdLeasePerGrant` for a core that believes it asked
            // for `range_size_ids`. Unreachable from `MaybeRefillRowIds`,
            // which is exactly why it is checked here rather than assumed
            // there.
            if (request.open_range != 0 && request.count == 0) {
                if (log != nullptr && log->enabled(LogLevel::kError)) {
                    log->Error("range", "core " + std::to_string(header.src_core) +
                                            " asked for a range with no block size for oid " +
                                            std::to_string(request.table_oid) +
                                            "; a range is its grant, so the two cannot differ");
                }
                request.open_range = 0;
            }
            const std::uint64_t count =
                request.count == 0 ? kRowIdLeasePerGrant : request.count;
            auto first = catalog.AllocateRowIdRange(
                static_cast<catalog::Oid>(request.table_oid), count);
            if (first.ok()) {
                grant.first_id = first.value();
                grant.count = count;
                // RD5. **After the carve and never instead of it**: the ids
                // are what the requester's statements are waiting on, and a
                // range that cannot be opened must not cost them. Every
                // failure below leaves `entry_page` invalid, which the
                // receiver reads as "no range", which is the relation
                // exactly as it was.
                //
                // `first_id == 0` cannot open a range - `OpenRange` refuses
                // `lo = 0` because that boundary *is* the relation - so a
                // relation whose very first ids are being leased opens its
                // directory at the second grant. Left as the arithmetic
                // rather than special-cased: a first block that is also the
                // whole relation has nothing to partition yet.
                //
                // `wal` is deliberately **not** required: a null one is an
                // unlogged store, `LogPageHandoff` answers `kNoLsn`, and an
                // unlogged store recovers nothing - so there is no handoff
                // to protect and nothing for the requirement to buy. The
                // store and the enforcer are required, because without
                // either the page could not be handed over or the gate not
                // asked.
                if (request.open_range != 0 && store != nullptr && enforcer != nullptr &&
                    grant.first_id != 0) {
                    auto opened = OpenRangeOnSystemCore(
                        catalog, *store, wal, *enforcer,
                        static_cast<catalog::Oid>(request.table_oid), grant.first_id,
                        header.src_core, log);
                    if (opened.ok()) {
                        grant.entry_page = opened.value();
                    } else if (log != nullptr && log->enabled(LogLevel::kError)) {
                        log->Error("range", "core 0 could not open a range for oid " +
                                                std::to_string(request.table_oid) + " at " +
                                                std::to_string(grant.first_id) + ": " +
                                                opened.status().message());
                    }
                }
            } else if (log != nullptr && log->enabled(LogLevel::kError)) {
                // The zero-count grant goes out regardless - the requester
                // is waiting, and "none" is an answer where silence is a
                // hang. OutOfRange (the 40-bit ceiling) and NotFound (no
                // such relation) both land here; the peer's statement
                // fails with the honest error either way.
                log->Error("rowid", "cannot grant core " + std::to_string(header.src_core) +
                                        " row ids for oid " + std::to_string(request.table_oid) +
                                        ": " + first.status().message());
            }

            std::byte bytes[sizeof(grant)];
            std::memcpy(bytes, &grant, sizeof(grant));

            sched::MessageHeader reply{};
            reply.src_core = header.dst_core;
            reply.dst_core = header.src_core;
            reply.session_core = header.session_core;
            reply.request_id = header.request_id;
            reply.kind = static_cast<std::uint16_t>(sched::RingMessageKind::kRowIdLease);
            reply.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);
            system_scheduler.Submit(sched::MakeSendRetryTask(
                transport, reply, std::span<const std::byte>(bytes, sizeof(bytes))));
        });
}

Status RegisterRowIdGrantReceiver(sched::Scheduler& scheduler, RowIdRefill& refill,
                                  catalog::RowIdLeaseTable& leases, Logger* log) {
    return scheduler.RegisterMessageHandler(
        sched::RingMessageKind::kRowIdLease,
        [&refill, &leases, &scheduler, log](const sched::MessageHeader& header,
                                       std::span<const std::byte> payload) {
            if (payload.size() != sizeof(RowIdLeaseGrantPayload)) {
                if (log != nullptr && log->enabled(LogLevel::kError)) {
                    log->Error("rowid", "grant from core " + std::to_string(header.src_core) +
                                            " has " + std::to_string(payload.size()) +
                                            " bytes, not " +
                                            std::to_string(sizeof(RowIdLeaseGrantPayload)));
                }
                // Released regardless, the extent receiver's rule: a
                // malformed grant that leaves the waiter parked hangs the
                // core. It wakes, sees count 0, and reports.
                refill.count = 0;
                refill.entry_page = kInvalidPageId;
                refill.granted = true;
                // No run arrived, so the relation still reads as low water
                // and the drain tick would ask again every cadence. The
                // oid is the one the request named.
                leases.Deny(static_cast<catalog::Oid>(refill.table_oid));
                return;
            }
            RowIdLeaseGrantPayload fields{};
            std::memcpy(&fields, payload.data(), sizeof(fields));
            refill.table_oid = fields.table_oid;
            refill.first_id = fields.first_id;
            refill.count = fields.count;
            // Recorded, not admitted: the page's write rights land on the
            // caller's tick, which is the one place this core's store and
            // WAL are in reach (RowIdRefill's note).
            refill.entry_page = static_cast<PageId>(fields.entry_page);
            refill.stats.NoteGrant(scheduler.clock().Now(), scheduler.iterations());
            if (fields.count > 0) {
                leases.Grant(static_cast<catalog::Oid>(fields.table_oid), fields.first_id,
                             fields.count);
                ++refill.stats.grants;
            } else {
                // "None available" is an answer, and a permanent one - the
                // carve failed because the relation is gone, names its own
                // ids, or has exhausted the space. Recorded on the entry so
                // the drain tick stops asking every cadence for a relation
                // core 0 has already refused, which would also keep it from
                // ever reaching a second needy relation on this core
                // (catalog/row_id_lease.hpp, `RowIdLease::denied`).
                leases.Deny(static_cast<catalog::Oid>(fields.table_oid));
            }
            refill.granted = true;
        });
}

sched::Coro RequestRowIdLease(sched::RingTransport& transport, RowIdRefill& refill,
                              std::uint64_t table_oid, std::uint64_t count,
                              std::uint32_t core_id, std::uint32_t system_core, Logger* log,
                              const sched::Scheduler* sched, bool open_range) {
    refill.granted = false;
    refill.table_oid = table_oid;
    refill.first_id = 0;
    refill.count = 0;
    refill.entry_page = kInvalidPageId;
    refill.stats.NoteSent(sched != nullptr ? sched->clock().Now() : 0,
                          sched != nullptr ? sched->iterations() : 0);

    RowIdLeaseRequestPayload request{table_oid, count, open_range ? 1u : 0u};
    std::byte bytes[sizeof(request)];
    std::memcpy(bytes, &request, sizeof(request));

    sched::MessageHeader header{};
    header.src_core = core_id;
    header.dst_core = system_core;
    header.session_core = core_id;
    header.kind = static_cast<std::uint16_t>(sched::RingMessageKind::kRowIdLease);
    header.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);

    if (Status s = transport.TrySend(header, std::span<const std::byte>(bytes, sizeof(bytes)));
        !s.ok()) {
        // Not retried, the extent request's reason: nobody waits on a
        // request that never left, and the next spent-lease statement asks
        // again.
        co_return s;
    }

    co_await sched::WaitFor{&refill.granted};

    if (refill.count == 0) {
        co_return Status::ResourceExhausted(
            "core " + std::to_string(core_id) + " asked for row ids for oid " +
            std::to_string(table_oid) + " and was granted none");
    }
    if (log != nullptr && log->enabled(LogLevel::kDebug)) {
        log->Debug("rowid", "core " + std::to_string(core_id) + " leased " +
                                std::to_string(refill.count) + " row ids for oid " +
                                std::to_string(table_oid) + " from " +
                                std::to_string(refill.first_id));
    }
    co_return Status::OK();
}

}  // namespace kds::server
