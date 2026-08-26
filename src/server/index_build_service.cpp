#include "kds/server/index_build_service.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

#include "kds/exec/index_ddl.hpp"
#include "kds/sched/send_retry.hpp"
#include "kds/wal/record.hpp"

namespace kds::server {
namespace {

std::string NameOf(const char* bytes, std::size_t capacity) {
    return std::string(bytes, ::strnlen(bytes, capacity));
}

}  // namespace

StatusOr<IndexBuildRequestPayload> IndexBuildRequestOf(const catalog::Catalog::IndexDef& def) {
    IndexBuildRequestPayload out{};
    if (def.key_cols.empty() || def.key_cols.size() > catalog::kMaxIndexKeyColumns ||
        def.covered_cols.size() > catalog::kMaxIndexCoveredColumns) {
        return Status::InvalidArgument("index definition names " +
                                       std::to_string(def.key_cols.size()) + " key and " +
                                       std::to_string(def.covered_cols.size()) +
                                       " covered columns, not a shape the build request "
                                       "carries");
    }
    if (def.name.size() >= sizeof(out.name)) {
        return Status::InvalidArgument("index name '" + def.name + "' is " +
                                       std::to_string(def.name.size()) +
                                       " bytes; the build request carries at most " +
                                       std::to_string(sizeof(out.name) - 1));
    }
    out.table_oid = def.table_oid;
    out.index_oid = def.index_oid;
    out.key_width = def.key_width;
    out.entry_width = def.entry_width;
    out.flags = def.flags;
    out.nkeys = static_cast<std::uint8_t>(def.key_cols.size());
    out.ncovered = static_cast<std::uint8_t>(def.covered_cols.size());
    for (std::size_t i = 0; i < def.key_cols.size(); ++i) out.key_cols[i] = def.key_cols[i];
    for (std::size_t i = 0; i < def.covered_cols.size(); ++i) {
        out.covered_cols[i] = def.covered_cols[i];
    }
    std::memcpy(out.name, def.name.data(), def.name.size());
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
    // Oid 0 is the *clustered* root's discriminator in `WriteAnchorRoot`,
    // so a request carrying it would move the relation's own root where it
    // meant to seed an index slot. Refused here rather than in
    // `CheckIndexDef`, which legitimately sees a zero oid on the local arm:
    // `PrepareIndexDef` checks before it issues one. On the wire it is
    // bytes this core did not compute, and this is where those are bounded.
    if (request.index_oid == 0) {
        Reply(requester, request_id, request.index_oid, kInvalidPageId,
              Status::InvalidArgument("index build request carries index oid 0, which names the "
                                      "clustered root rather than an index slot"));
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
    pending_.Open(request.table_oid, request.index_oid, scheduler_.clock().Now());
    scheduler_.Submit(std::make_unique<sched::FunctionTask>(
        sched::SchedulingGroup::kSystem, [this, requester, request_id, request] {
            Build(requester, request_id, request);
            return sched::PollResult::kDone;
        }));
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
    // `kHere`, so this asks the anchor's entry table too - and asks it
    // before a page is allocated, which is what makes the refusal free
    // (D5, anchor_page.hpp's CheckAnchorRoomForIndex). This core owns the
    // relation, so the anchor it reads is its own.
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
    // `session_core` is the constant 0 on every leg of this protocol: core
    // 0 owns the statement in both directions and nothing here reads the
    // field. Written rather than left to a default so a reader of a
    // captured header sees it was decided.
    sched::SubmitSendPod(scheduler_, transport_, core_id_, requester, /*session_core=*/0,
                         request_id, sched::RingMessageKind::kIndexBuildReply, reply);
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

Status IndexBuildClient::RegisterReplyReceiver() {
    return scheduler_.RegisterMessageHandler(
        sched::RingMessageKind::kIndexBuildReply,
        [this](const sched::MessageHeader& header, std::span<const std::byte> payload) {
            IndexBuildReplyPayload reply{};
            if (payload.size() != sizeof(reply)) {
                if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                    log_->Error("index", "index build reply from core " +
                                             std::to_string(header.src_core) + " has " +
                                             std::to_string(payload.size()) + " bytes, not " +
                                             std::to_string(sizeof(reply)) + "; dropped");
                }
                return;
            }
            std::memcpy(&reply, payload.data(), sizeof(reply));
            auto it = waiting_.find(header.request_id);
            if (it == waiting_.end()) {
                // Core 0 gave up on this one. A tree the owner built for it
                // is orphaned by saying so, which also closes the window
                // the late request opened (the header's argument); a
                // refusal closed its own.
                if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
                    log_->Debug("index", "index build reply for request " +
                                             std::to_string(header.request_id) +
                                             " from core " + std::to_string(header.src_core) +
                                             " matched no waiter; " +
                                             (reply.status_code == 0
                                                  ? "its tree is orphaned"
                                                  : "discarded"));
                }
                if (reply.status_code == static_cast<std::uint32_t>(StatusCode::kOk)) {
                    Done(header.src_core, reply.index_oid, /*committed=*/false);
                }
                return;
            }
            IndexBuildOutcome& out = it->second;
            out.status = Status::FromWire(reply.status_code,
                                          NameOf(reply.message, sizeof(reply.message)));
            out.root_page_id = out.status.ok() ? reply.root_page_id : kInvalidPageId;
            out.arrived = true;
        });
}

Status IndexBuildClient::Request(std::uint32_t owner_core, std::uint64_t request_id,
                                 const catalog::Catalog::IndexDef& def) {
    auto request = IndexBuildRequestOf(def);
    if (!request.ok()) return request.status();
    IndexBuildOutcome& out = waiting_.insert_or_assign(request_id, IndexBuildOutcome{}).first->second;
    out.deadline_ns = clock_.Now() + kIndexBuildReplyDeadlineNs;
    sched::SubmitSendPod(scheduler_, transport_, /*src=*/0, owner_core, /*session_core=*/0,
                         request_id, sched::RingMessageKind::kIndexBuildRequest,
                         request.value());
    return Status::OK();
}

bool IndexBuildClient::Settled(std::uint64_t request_id) const {
    auto it = waiting_.find(request_id);
    if (it == waiting_.end()) return true;
    return it->second.arrived || clock_.Now() >= it->second.deadline_ns;
}

const IndexBuildOutcome* IndexBuildClient::Find(std::uint64_t request_id) const {
    auto it = waiting_.find(request_id);
    return it == waiting_.end() ? nullptr : &it->second;
}

void IndexBuildClient::Close(std::uint64_t request_id) { waiting_.erase(request_id); }

void IndexBuildClient::Done(std::uint32_t owner_core, std::uint64_t index_oid, bool committed) {
    IndexBuildDonePayload done{};
    done.index_oid = index_oid;
    done.committed = committed ? 1 : 0;
    sched::SubmitSendPod(scheduler_, transport_, /*src=*/0, owner_core, /*session_core=*/0,
                         /*request_id=*/0, sched::RingMessageKind::kIndexBuildDone, done);
}

}  // namespace kds::server
