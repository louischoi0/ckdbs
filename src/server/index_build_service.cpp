#include "kds/server/index_build_service.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include "kds/exec/index_ddl.hpp"
#include "kds/sched/send_retry.hpp"
#include "kds/wal/record.hpp"

namespace kds::server {
namespace {

template <typename Pod>
void SendPod(sched::Scheduler& scheduler, sched::RingTransport& transport, std::uint32_t src,
             std::uint32_t dst, std::uint64_t request_id, sched::RingMessageKind kind,
             const Pod& pod) {
    std::byte bytes[sizeof(Pod)];
    std::memcpy(bytes, &pod, sizeof(Pod));
    sched::MessageHeader header{};
    header.request_id = request_id;
    header.src_core = src;
    header.dst_core = dst;
    header.session_core = src;
    header.kind = static_cast<std::uint16_t>(kind);
    header.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);
    // The task copies the payload (send_retry.hpp), so a stack buffer is
    // enough.
    scheduler.Submit(sched::MakeSendRetryTask(transport, header,
                                              std::span<const std::byte>(bytes, sizeof(bytes))));
}

// session_step_client.cpp's switch, for its reason: Status's constructor
// is private, and an unknown code degrades to IoError rather than being
// trusted.
Status StatusOfWire(std::uint32_t code, std::string msg) {
    switch (static_cast<StatusCode>(code)) {
        case StatusCode::kOk: return Status::OK();
        case StatusCode::kTxnConflict: return Status::TxnConflict(std::move(msg));
        case StatusCode::kNotFound: return Status::NotFound(std::move(msg));
        case StatusCode::kUnsupported: return Status::Unsupported(std::move(msg));
        case StatusCode::kInvalidArgument: return Status::InvalidArgument(std::move(msg));
        case StatusCode::kResourceExhausted: return Status::ResourceExhausted(std::move(msg));
        case StatusCode::kAlreadyExists: return Status::AlreadyExists(std::move(msg));
        case StatusCode::kCorruption: return Status::Corruption(std::move(msg));
        case StatusCode::kOutOfSpace: return Status::OutOfSpace(std::move(msg));
        case StatusCode::kOutOfRange: return Status::OutOfRange(std::move(msg));
        default: return Status::IoError(std::move(msg));
    }
}

std::string NameOf(const char* bytes, std::size_t capacity) {
    return std::string(bytes, ::strnlen(bytes, capacity));
}

}  // namespace

IndexBuildRequestPayload IndexBuildRequestOf(const catalog::Catalog::IndexDef& def) {
    IndexBuildRequestPayload out{};
    out.table_oid = def.table_oid;
    out.index_oid = def.index_oid;
    out.key_width = def.key_width;
    out.entry_width = def.entry_width;
    out.flags = def.flags;
    const std::size_t nkeys = std::min(def.key_cols.size(), catalog::kMaxIndexKeyColumns);
    const std::size_t ncovered =
        std::min(def.covered_cols.size(), catalog::kMaxIndexCoveredColumns);
    out.nkeys = static_cast<std::uint8_t>(nkeys);
    out.ncovered = static_cast<std::uint8_t>(ncovered);
    for (std::size_t i = 0; i < nkeys; ++i) out.key_cols[i] = def.key_cols[i];
    for (std::size_t i = 0; i < ncovered; ++i) out.covered_cols[i] = def.covered_cols[i];
    std::memcpy(out.name, def.name.data(), std::min(def.name.size(), sizeof(out.name) - 1));
    return out;
}

catalog::Catalog::IndexDef IndexDefOf(const IndexBuildRequestPayload& request) {
    catalog::Catalog::IndexDef def;
    def.table_oid = request.table_oid;
    def.index_oid = request.index_oid;
    def.name = NameOf(request.name, sizeof(request.name));
    def.key_width = request.key_width;
    def.entry_width = request.entry_width;
    def.flags = request.flags;
    for (std::size_t i = 0; i < request.nkeys; ++i) def.key_cols.push_back(request.key_cols[i]);
    for (std::size_t i = 0; i < request.ncovered; ++i) {
        def.covered_cols.push_back(request.covered_cols[i]);
    }
    return def;
}

// ---- The owner's half ------------------------------------------------------

void IndexBuildServer::OnRequest(const sched::MessageHeader& header,
                                 std::span<const std::byte> payload) {
    IndexBuildRequestPayload request{};
    if (payload.size() != sizeof(request)) {
        // No reply: nothing here names the index core 0 is waiting on. A
        // size mismatch is a build disagreeing with itself, and core 0's
        // deadline is the backstop.
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("index", "index build request from core " +
                                     std::to_string(header.src_core) + " has " +
                                     std::to_string(payload.size()) + " bytes, not " +
                                     std::to_string(sizeof(request)) + "; dropped");
        }
        return;
    }
    std::memcpy(&request, payload.data(), sizeof(request));
    const std::uint32_t requester = header.src_core;
    const std::uint64_t request_id = header.request_id;

    // The caps, before either array is read (BuildIndexTree's rule, one
    // layer up): these are bytes this core did not compute.
    if (request.nkeys == 0 || request.nkeys > catalog::kMaxIndexKeyColumns ||
        request.ncovered > catalog::kMaxIndexCoveredColumns) {
        Reply(requester, request_id, request.index_oid, kInvalidPageId,
              Status::InvalidArgument("index build request names " +
                                      std::to_string(request.nkeys) + " key and " +
                                      std::to_string(request.ncovered) +
                                      " covered columns, not a shape an index entry has"));
        return;
    }
    // The row is the authority on who owns the relation (CC7), read rather
    // than trusted from the requester: a tree built here for a relation
    // this core does not own is the two-writer route from the other side.
    auto row = catalog_.GetSysTableRow(request.table_oid);
    if (!row.ok()) {
        Reply(requester, request_id, request.index_oid, kInvalidPageId, row.status());
        return;
    }
    if (row.value().owner_core != core_id_) {
        Reply(requester, request_id, request.index_oid, kInvalidPageId,
              Status::Unsupported("relation oid " + std::to_string(request.table_oid) +
                                  " is owned by core " +
                                  std::to_string(row.value().owner_core) + ", not core " +
                                  std::to_string(core_id_) +
                                  "; an index is built by its relation's owner "
                                  "(workplan-peer-writer.md §7c)"));
        return;
    }
    // One window per relation at a time: a second build would only extend
    // the first's refusal. Retryable, since the first ends.
    if (pending_.Covers(request.table_oid)) {
        Reply(requester, request_id, request.index_oid, kInvalidPageId,
              Status::TxnConflict("relation oid " + std::to_string(request.table_oid) +
                                  " already has an index build pending on core " +
                                  std::to_string(core_id_) + "; retry when it ends"));
        return;
    }

    // The window opens *here*, before the build and before the task's
    // turn: a write admitted between this message and the build's first
    // page would be the missing row the header describes.
    pending_.Open(request.table_oid, request.index_oid, clock_.Now());
    if (submit_) {
        submit_(std::make_unique<sched::FunctionTask>(
            sched::SchedulingGroup::kSystem, [this, requester, request_id, request] {
                Build(requester, request_id, request);
                return sched::PollResult::kDone;
            }));
    } else {
        Build(requester, request_id, request);
    }
}

void IndexBuildServer::Build(std::uint32_t requester, std::uint64_t request_id,
                             const IndexBuildRequestPayload& request) {
    ++builds_;
    const auto fail = [&](const Status& s) {
        // Nothing to protect: no tree is published and none will be.
        pending_.Close(request.index_oid);
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("index", "core " + std::to_string(core_id_) + ": building index oid " +
                                     std::to_string(request.index_oid) + " on relation oid " +
                                     std::to_string(request.table_oid) + " for core " +
                                     std::to_string(requester) + " failed: " + s.message());
        }
        Reply(requester, request_id, request.index_oid, kInvalidPageId, s);
    };

    const catalog::Catalog::IndexDef def = IndexDefOf(request);
    // Core 0 ran this already; it runs again against *this* core's view of
    // the relation, which is the one the tree is built from. `kHere`: this
    // core owns the relation and seeds its own anchor.
    if (Status s = catalog_.CheckIndexDef(def); !s.ok()) return fail(s);
    auto access = catalog_.InitTableAccess(def.table_oid);
    if (!access.ok()) return fail(access.status());

    // `CREATE INDEX`'s own page half, from this core's lease and pool,
    // logged under kNoTxnId into this stream (the header's argument).
    auto root = exec::BuildIndexTree(store_, *access.value(), def, wal::kNoTxnId, wal_);
    if (!root.ok()) return fail(root.status());

    // The anchor slot, seeded by its owner: what Catalog::CreateIndex does
    // for a relation core 0 owns, and what core 0's row write skips for
    // this one (AnchorSeed::kByOwner).
    if (access.value()->anchor_page_id != kInvalidPageId) {
        if (Status s = catalog_.WriteAnchorRoot(access.value()->anchor_page_id, def.table_oid,
                                                def.index_oid, root.value(), wal::kNoTxnId);
            !s.ok()) {
            return fail(s.WithContext("seeding the anchor slot"));
        }
    }
    // Durable before the reply: core 0 commits a row naming this root on
    // the strength of it, and a crash after that commit must find the
    // tree the recovered row probes. The images and the anchor record are
    // in this stream, so this stream syncs whole - SyncAll's promise, on a
    // DDL's cadence; a blocking fsync on the reactor, as core 0's own
    // CREATE INDEX commit is.
    if (wal_ != nullptr) {
        if (Status s = wal_->SyncAll(); !s.ok()) {
            return fail(s.WithContext("making the built tree durable"));
        }
    }
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("index", "core " + std::to_string(core_id_) + " built index oid " +
                                std::to_string(def.index_oid) + " on relation oid " +
                                std::to_string(def.table_oid) + " for core " +
                                std::to_string(requester) + ": root " +
                                std::to_string(root.value()) +
                                "; writes refused until done (workplan-peer-writer.md §7c)");
    }
    Reply(requester, request_id, def.index_oid, root.value(), Status::OK());
}

void IndexBuildServer::Reply(std::uint32_t requester, std::uint64_t request_id,
                             std::uint64_t index_oid, PageId root, const Status& status) {
    IndexBuildReplyPayload reply{};
    reply.index_oid = index_oid;
    reply.root_page_id = root;
    reply.status_code = static_cast<std::uint32_t>(status.code());
    const std::string& msg = status.message();
    std::memcpy(reply.message, msg.data(), std::min(msg.size(), sizeof(reply.message) - 1));
    std::byte bytes[sizeof(reply)];
    std::memcpy(bytes, &reply, sizeof(reply));
    send_(requester, request_id, sched::RingMessageKind::kIndexBuildReply,
          std::span<const std::byte>(bytes, sizeof(bytes)));
}

void IndexBuildServer::OnDone(const sched::MessageHeader& header,
                              std::span<const std::byte> payload) {
    IndexBuildDonePayload done{};
    if (payload.size() != sizeof(done)) {
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("index", "index build done from core " +
                                     std::to_string(header.src_core) + " has " +
                                     std::to_string(payload.size()) + " bytes, not " +
                                     std::to_string(sizeof(done)) + "; dropped");
        }
        return;
    }
    std::memcpy(&done, payload.data(), sizeof(done));
    if (!pending_.Close(done.index_oid)) {
        // Core 0 gave up before the request arrived (the two sends re-queue
        // independently on a full ring), or the window expired. The
        // ceiling covers the first; nothing is owed for the second.
        if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
            log_->Debug("index", "done for index oid " + std::to_string(done.index_oid) +
                                     " matched no open window; ignored");
        }
        return;
    }
    if (done.committed != 0) {
        if (on_committed_) on_committed_();
        if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
            log_->Info("index", "index oid " + std::to_string(done.index_oid) +
                                    " published by core " + std::to_string(header.src_core) +
                                    "; the window is closed");
        }
    } else if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("index", "index oid " + std::to_string(done.index_oid) +
                                " aborted by core " + std::to_string(header.src_core) +
                                "; its tree is orphaned and its anchor slot stays");
    }
}

void IndexBuildServer::Expire(sched::MonoTimeNs now) {
    const auto expired = pending_.Expire(now, kIndexBuildPendingCeilingNs);
    if (expired.empty()) return;
    for (const PendingIndexBuilds::Entry& entry : expired) {
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("index", "core " + std::to_string(core_id_) + ": index oid " +
                                     std::to_string(entry.index_oid) + " on relation oid " +
                                     std::to_string(entry.table_oid) +
                                     " heard no done within the ceiling; the window is "
                                     "released (workplan-peer-writer.md §7c)");
        }
    }
    // In case one of them was a commit whose `done` was lost: the
    // published index must be seen by the writes this release admits.
    if (on_committed_) on_committed_();
}

// ---- Core 0's half ---------------------------------------------------------

IndexBuildOutcome& IndexBuildWaiters::Open(std::uint64_t request_id) {
    return waiting_.insert_or_assign(request_id, IndexBuildOutcome{}).first->second;
}

IndexBuildOutcome* IndexBuildWaiters::Find(std::uint64_t request_id) {
    auto it = waiting_.find(request_id);
    return it == waiting_.end() ? nullptr : &it->second;
}

void IndexBuildWaiters::Close(std::uint64_t request_id) { waiting_.erase(request_id); }

Status RegisterIndexBuildReplyReceiver(sched::Scheduler& system_scheduler,
                                       IndexBuildWaiters& waiters, Logger* log) {
    return system_scheduler.RegisterMessageHandler(
        sched::RingMessageKind::kIndexBuildReply,
        [&waiters, log](const sched::MessageHeader& header, std::span<const std::byte> payload) {
            IndexBuildReplyPayload reply{};
            if (payload.size() != sizeof(reply)) {
                if (log != nullptr && log->enabled(LogLevel::kError)) {
                    log->Error("index", "index build reply from core " +
                                            std::to_string(header.src_core) + " has " +
                                            std::to_string(payload.size()) + " bytes, not " +
                                            std::to_string(sizeof(reply)) + "; dropped");
                }
                return;
            }
            std::memcpy(&reply, payload.data(), sizeof(reply));
            IndexBuildOutcome* out = waiters.Find(header.request_id);
            if (out == nullptr) {
                if (log != nullptr && log->enabled(LogLevel::kDebug)) {
                    log->Debug("index", "index build reply for request " +
                                            std::to_string(header.request_id) + " from core " +
                                            std::to_string(header.src_core) +
                                            " matched no waiter; discarded");
                }
                return;
            }
            out->status = StatusOfWire(reply.status_code,
                                       NameOf(reply.message, sizeof(reply.message)));
            out->root_page_id = out->status.ok() ? reply.root_page_id : kInvalidPageId;
            out->arrived = true;
        });
}

void SendIndexBuildRequest(sched::Scheduler& scheduler, sched::RingTransport& transport,
                           std::uint32_t owner_core, std::uint64_t request_id,
                           const IndexBuildRequestPayload& request, std::uint32_t system_core) {
    SendPod(scheduler, transport, system_core, owner_core, request_id,
            sched::RingMessageKind::kIndexBuildRequest, request);
}

void SendIndexBuildDone(sched::Scheduler& scheduler, sched::RingTransport& transport,
                        std::uint32_t owner_core, std::uint64_t index_oid, bool committed,
                        std::uint32_t system_core) {
    IndexBuildDonePayload done{};
    done.index_oid = index_oid;
    done.committed = committed ? 1 : 0;
    SendPod(scheduler, transport, system_core, owner_core, /*request_id=*/0,
            sched::RingMessageKind::kIndexBuildDone, done);
}

}  // namespace kds::server
