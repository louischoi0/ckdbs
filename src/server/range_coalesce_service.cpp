#include "kds/server/range_coalesce_service.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "kds/sched/send_retry.hpp"
#include "kds/wal/log_page_handoff.hpp"

namespace kds::server {
namespace {

// A NUL-terminated wire string, `index_build_service.cpp`'s helper for
// its reason: a reply message is fixed-width and a `std::string` over it
// must stop at the terminator, not at the field.
std::string NameOf(const char* bytes, std::size_t capacity) {
    const std::size_t n = ::strnlen(bytes, capacity);
    return std::string(bytes, n);
}

}  // namespace

// ---- The peer's half -------------------------------------------------------

void RangeCoalesceServer::Reply(sched::RingMessageKind kind, std::uint32_t requester,
                                std::uint64_t request_id, std::uint64_t table_oid,
                                std::uint32_t pages, const Status& status) {
    RangeCoalesceReplyPayload reply{};
    reply.table_oid = table_oid;
    reply.status_code = static_cast<std::uint32_t>(status.code());
    reply.pages = pages;
    const std::string& msg = status.message();
    std::memcpy(reply.message, msg.data(), std::min(msg.size(), sizeof(reply.message) - 1));
    // `session_core` is the constant 0 on every leg: core 0 owns the DDL in
    // both directions and nothing on this protocol reads the field. Named
    // rather than defaulted, `index_build_service.cpp`'s rule.
    sched::SubmitSendPod(scheduler_, transport_, core_id_, requester, /*session_core=*/0,
                         request_id, kind, reply);
}

void RangeCoalesceServer::OnQuiesce(const sched::MessageHeader& header,
                                    std::span<const std::byte> payload) {
    RangeQuiesceRequestPayload request{};
    if (payload.size() != sizeof(request)) {
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("range", "coalesce quiesce from core " +
                                     std::to_string(header.src_core) + " has " +
                                     std::to_string(payload.size()) + " bytes, not " +
                                     std::to_string(sizeof(request)) + "; dropped");
        }
        return;
    }
    std::memcpy(&request, payload.data(), sizeof(request));

    // The range's pages, walked from this core's own frames - which is the
    // point of asking the owner rather than reading the device from core
    // 0: a page this core holds dirty is in the chain and core 0 cannot
    // see it until the flush below.
    std::vector<PageId> pages;
    Status walked = CollectRangePages(store_, request.entry_page, request.hi, pages);
    if (!walked.ok()) {
        Reply(sched::RingMessageKind::kRangeQuiesceReply, header.src_core, header.request_id,
              request.table_oid, 0, walked);
        return;
    }

    // §6c step 1. Flushed **before** the departure records, PL §9 rule 1's
    // ordering and the giver's to keep: the receiver faults these bytes
    // off the device, so a record naming a page whose image is still in
    // this core's frame hands over an image the absorber cannot read.
    if (Status s = store_.FlushPages(pages); !s.ok()) {
        Reply(sched::RingMessageKind::kRangeQuiesceReply, header.src_core, header.request_id,
              request.table_oid, 0,
              s.WithContext("flushing range at lo " + std::to_string(request.lo) +
                            " before its departure"));
        return;
    }

    // §6c step 2. One record per page in **this** stream, naming the
    // absorber as the incoming core (PL §9 rule 1: the departure lives in
    // the giver's stream). Durability waited on once, for the maximum: the
    // gate is a watermark, so a per-page wait would be the same fsync
    // asked for n times.
    wal::Lsn handoff_max = wal::kNoLsn;
    for (PageId id : pages) {
        auto lsn = wal::LogPageHandoff(wal_, id, request.absorber);
        if (!lsn.ok()) {
            Reply(sched::RingMessageKind::kRangeQuiesceReply, header.src_core, header.request_id,
                  request.table_oid, 0,
                  lsn.status().WithContext("departure record for page " + std::to_string(id)));
            return;
        }
        handoff_max = std::max(handoff_max, lsn.value());
    }
    if (handoff_max != wal::kNoLsn && wal_ != nullptr) {
        if (Status s = wal_->EnsureDurable(handoff_max); !s.ok()) {
            Reply(sched::RingMessageKind::kRangeQuiesceReply, header.src_core, header.request_id,
                  request.table_oid, 0, s.WithContext("departure records not durable"));
            return;
        }
    }

    // §6c step 0's other half, and the one CC7 says the mover owes. Taken
    // **after** the records are durable, so the window in which this core
    // has given up its rights but the absorber has not been told is as
    // short as the reply - and in that window nothing writes the range
    // anyway, because the rights are gone.
    store_.RevokeWritePages(pages);
    // Best-effort, and logged rather than failed: the flush above already
    // made the device authoritative, so a frame left behind is a stale
    // *read* on a core that no longer routes to this relation, not a lost
    // write. Failing the leg here would abandon a merge over a frame.
    if (Status s = store_.EvictClean(pages); !s.ok() && log_ != nullptr &&
                                             log_->enabled(LogLevel::kError)) {
        log_->Error("range", "core " + std::to_string(core_id_) +
                                 " could not evict its departed range at lo " +
                                 std::to_string(request.lo) + ": " + s.message());
    }

    ++quiesced_ranges_;
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("range", "core " + std::to_string(core_id_) + " quiesced relation oid " +
                                std::to_string(request.table_oid) + " range at lo " +
                                std::to_string(request.lo) + ": " +
                                std::to_string(pages.size()) + " page(s) to core " +
                                std::to_string(request.absorber));
    }
    Reply(sched::RingMessageKind::kRangeQuiesceReply, header.src_core, header.request_id,
          request.table_oid, static_cast<std::uint32_t>(pages.size()), Status::OK());
}

void RangeCoalesceServer::OnAbsorb(const sched::MessageHeader& header,
                                   std::span<const std::byte> payload) {
    RangeAbsorbRequestPayload request{};
    if (payload.size() != sizeof(request)) {
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("range", "coalesce absorb from core " + std::to_string(header.src_core) +
                                     " has " + std::to_string(payload.size()) + " bytes, not " +
                                     std::to_string(sizeof(request)) + "; dropped");
        }
        return;
    }
    std::memcpy(&request, payload.data(), sizeof(request));

    const auto fail = [&](const Status& s) {
        Reply(sched::RingMessageKind::kRangeAbsorbReply, header.src_core, header.request_id,
              request.table_oid, 0, s);
    };

    // Planned here rather than sent, and that is what keeps a page list
    // off the wire: this core reads the same `sys.ranges` core 0 did, and
    // by now every departing range is flushed, so the census is exact.
    auto plan = PlanCoalesce(catalog_, store_, static_cast<catalog::Oid>(request.table_oid));
    if (!plan.ok()) return fail(plan.status());
    if (plan.value().absorber != core_id_) {
        // Core 0 and this core disagree about who absorbs, which can only
        // mean the directory moved between the two censuses. Refused
        // rather than absorbed anyway: a merge into the wrong core writes
        // `owner_core` at a core that then holds no pages.
        return fail(Status::InvalidArgument(
            "coalesce: core " + std::to_string(core_id_) +
            " was asked to absorb relation oid " + std::to_string(request.table_oid) +
            ", but its own census names core " + std::to_string(plan.value().absorber)));
    }

    // The acquisition PL §9 rule 6 requires, through the runtime's own
    // sequence rather than a second copy of it. In chunks, because the
    // sequence flushes what it is given and a relation's whole page set
    // in one call would hold every frame at once.
    std::uint32_t acquired = 0;
    for (const CoalesceSegment& segment : plan.value().segments) {
        if (segment.owner_core == core_id_) continue;  // already this core's
        for (std::size_t i = 0; i < segment.pages.size(); i += kAdmitChunkPages) {
            const std::size_t n = std::min(kAdmitChunkPages, segment.pages.size() - i);
            const std::span<const PageId> chunk(segment.pages.data() + i, n);
            if (!admit_(chunk)) {
                return fail(Status::IoError(
                    "coalesce: core " + std::to_string(core_id_) +
                    " could not acquire page " + std::to_string(segment.pages[i]) +
                    " of relation oid " + std::to_string(request.table_oid) +
                    "; the relation stays split (docs/spec/crosscore.md §6c)"));
            }
            acquired += static_cast<std::uint32_t>(n);
        }
    }

    if (Status s = LinkSegments(store_, wal_, plan.value(), core_id_, log_); !s.ok()) {
        return fail(s);
    }

    ++absorbed_relations_;
    pages_moved_ += acquired;
    Reply(sched::RingMessageKind::kRangeAbsorbReply, header.src_core, header.request_id,
          request.table_oid, acquired, Status::OK());
}

// ---- Core 0's half ---------------------------------------------------------

Status RangeCoalesceClient::RegisterReplyReceivers() {
    const auto receiver = [this](const sched::MessageHeader& header,
                                 std::span<const std::byte> payload) {
        OnReply(header, payload);
    };
    if (Status s = scheduler_.RegisterMessageHandler(sched::RingMessageKind::kRangeQuiesceReply,
                                                     receiver);
        !s.ok()) {
        return s;
    }
    return scheduler_.RegisterMessageHandler(sched::RingMessageKind::kRangeAbsorbReply, receiver);
}

void RangeCoalesceClient::OnReply(const sched::MessageHeader& header,
                                  std::span<const std::byte> payload) {
    RangeCoalesceReplyPayload reply{};
    if (payload.size() != sizeof(reply)) {
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("range", "coalesce reply from core " + std::to_string(header.src_core) +
                                     " has " + std::to_string(payload.size()) + " bytes, not " +
                                     std::to_string(sizeof(reply)) + "; dropped");
        }
        return;
    }
    std::memcpy(&reply, payload.data(), sizeof(reply));
    auto it = waiting_.find(header.request_id);
    if (it == waiting_.end()) {
        // Core 0 gave up. Nothing to undo and nothing to tell the peer:
        // every prefix of §6c's sequence is a state the engine serves, so
        // a leg that finished after the deadline leaves the relation
        // legally split and the statement re-runnable (the header's "what
        // a lost leg costs").
        if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
            log_->Debug("range", "coalesce reply for request " +
                                     std::to_string(header.request_id) + " from core " +
                                     std::to_string(header.src_core) + " matched no waiter");
        }
        return;
    }
    it->second.status = Status::FromWire(reply.status_code,
                                         NameOf(reply.message, sizeof(reply.message)));
    it->second.pages = reply.pages;
    it->second.arrived = true;
}

void RangeCoalesceClient::Open(std::uint64_t request_id) {
    RangeCoalesceOutcome& out =
        waiting_.insert_or_assign(request_id, RangeCoalesceOutcome{}).first->second;
    out.deadline_ns = clock_.Now() + kRangeCoalesceReplyDeadlineNs;
}

Status RangeCoalesceClient::Quiesce(std::uint32_t owner_core, std::uint64_t request_id,
                                    catalog::Oid rel_oid, std::uint64_t lo, std::uint64_t hi,
                                    PageId entry_page, std::uint32_t absorber) {
    RangeQuiesceRequestPayload request{};
    request.table_oid = rel_oid;
    request.lo = lo;
    request.hi = hi;
    request.entry_page = entry_page;
    request.absorber = absorber;
    Open(request_id);
    sched::SubmitSendPod(scheduler_, transport_, /*src=*/catalog::kSystemCore, owner_core,
                         /*session_core=*/0, request_id,
                         sched::RingMessageKind::kRangeQuiesceRequest, request);
    return Status::OK();
}

Status RangeCoalesceClient::Absorb(std::uint32_t absorber, std::uint64_t request_id,
                                   catalog::Oid rel_oid) {
    RangeAbsorbRequestPayload request{};
    request.table_oid = rel_oid;
    Open(request_id);
    sched::SubmitSendPod(scheduler_, transport_, /*src=*/catalog::kSystemCore, absorber,
                         /*session_core=*/0, request_id,
                         sched::RingMessageKind::kRangeAbsorbRequest, request);
    return Status::OK();
}

bool RangeCoalesceClient::Settled(std::uint64_t request_id) const {
    auto it = waiting_.find(request_id);
    if (it == waiting_.end()) return true;
    return it->second.arrived || clock_.Now() >= it->second.deadline_ns;
}

const RangeCoalesceOutcome* RangeCoalesceClient::Find(std::uint64_t request_id) const {
    auto it = waiting_.find(request_id);
    return it == waiting_.end() ? nullptr : &it->second;
}

void RangeCoalesceClient::Close(std::uint64_t request_id) { waiting_.erase(request_id); }

}  // namespace kds::server
