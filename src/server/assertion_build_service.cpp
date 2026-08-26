#include "kds/server/assertion_build_service.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <variant>

#include "kds/exec/assertion_catalog.hpp"
#include "kds/parser/parser.hpp"
#include "kds/sched/send_retry.hpp"
#include "kds/txn/read_view.hpp"

namespace kds::server {

StatusOr<AssertionBuildRequestPayload> AssertionBuildRequestOf(catalog::Oid table_oid,
                                                               std::uint64_t assertion_id,
                                                               std::string_view source_text) {
    AssertionBuildRequestPayload out{};
    if (source_text.empty() || source_text.size() > kAssertionBuildTextMax) {
        return Status::Unsupported(
            "assertion declaration of " + std::to_string(source_text.size()) +
            " bytes cannot be carried to its relation's owner, which takes at most " +
            std::to_string(kAssertionBuildTextMax) +
            " (assertion_build_service.hpp: refused rather than truncated)");
    }
    out.table_oid = table_oid;
    out.assertion_id = assertion_id;
    out.text_len = static_cast<std::uint16_t>(source_text.size());
    std::memcpy(out.text, source_text.data(), source_text.size());
    return out;
}

// ---- The owner's half ------------------------------------------------------

void AssertionBuildServer::OnRequest(const sched::MessageHeader& header,
                                     std::span<const std::byte> payload) {
    AssertionBuildRequestPayload request{};
    if (payload.size() != sizeof(request)) {
        // No reply: nothing here names the assertion core 0 is waiting on.
        // A size mismatch is a build disagreeing with itself, and core 0's
        // deadline is the backstop.
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("assertion", "assertion build request from core " +
                                         std::to_string(header.src_core) + " has " +
                                         std::to_string(payload.size()) + " bytes, not " +
                                         std::to_string(sizeof(request)) + "; dropped");
        }
        return;
    }
    std::memcpy(&request, payload.data(), sizeof(request));
    const std::uint32_t requester = header.src_core;
    const std::uint64_t request_id = header.request_id;

    // The length, before the text is read: these are bytes this core did
    // not compute (`BuildIndexTree`'s rule, one layer up).
    if (request.text_len == 0 || request.text_len > kAssertionBuildTextMax) {
        Reply(requester, request_id, request.assertion_id, kInvalidPageId, 0, 0,
              Status::InvalidArgument("assertion build request carries a declaration of " +
                                      std::to_string(request.text_len) +
                                      " bytes, which is not a length this slot holds"));
        return;
    }
    // Id 0 names no assertion: a cabin built under it would log
    // `ASSERT_BUILD` records replay could attach to nothing.
    if (request.assertion_id == 0) {
        Reply(requester, request_id, request.assertion_id, kInvalidPageId, 0, 0,
              Status::InvalidArgument("assertion build request carries assertion id 0"));
        return;
    }
    // The row is the authority on who owns the relation (CC7), read rather
    // than trusted from the requester: a cabin built here for a relation
    // this core does not own is a chain its owner may not append to, which
    // is the very failure this protocol exists to prevent.
    auto row = catalog_.GetSysTableRow(request.table_oid);
    if (!row.ok()) {
        Reply(requester, request_id, request.assertion_id, kInvalidPageId, 0, 0, row.status());
        return;
    }
    if (row.value().owner_core != core_id_) {
        Reply(requester, request_id, request.assertion_id, kInvalidPageId, 0, 0,
              Status::Unsupported("relation oid " + std::to_string(request.table_oid) +
                                  " is owned by core " +
                                  std::to_string(row.value().owner_core) + ", not core " +
                                  std::to_string(core_id_) +
                                  "; an assertion is built by its relation's owner "
                                  "(workplan-peer-writer.md §7d)"));
        return;
    }

    scheduler_.Submit(std::make_unique<sched::FunctionTask>(
        sched::SchedulingGroup::kSystem, [this, requester, request_id, request] {
            Build(requester, request_id, request);
            return sched::PollResult::kDone;
        }));
}

void AssertionBuildServer::Build(std::uint32_t requester, std::uint64_t request_id,
                                 const AssertionBuildRequestPayload& request) {
    ++builds_;
    const auto fail = [&](const Status& s) {
        // Nothing to protect: no directory was adopted and no row will name
        // the chain, whatever of it reached the device.
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("assertion", "core " + std::to_string(core_id_) +
                                         ": building assertion id " +
                                         std::to_string(request.assertion_id) +
                                         " on relation oid " +
                                         std::to_string(request.table_oid) + " for core " +
                                         std::to_string(requester) + " failed: " + s.message());
        }
        Reply(requester, request_id, request.assertion_id, kInvalidPageId, 0, 0, s);
    };

    // §8.2's canon, parsed here: the owner reads the declaration the same
    // way a mount's `ReviveAssertion` reads it, so a peer-owned relation's
    // assertion means exactly what a core-0-owned one's does.
    const std::string text(request.text, request.text_len);
    parser::Parser parser(text);
    auto parsed = parser.Parse();
    if (!parsed.ok()) return fail(parsed.status());
    const auto* stmt = std::get_if<parser::AssertionStmt>(&parsed.value());
    if (stmt == nullptr || stmt->drop) {
        return fail(Status::InvalidArgument(
            "assertion build request carries text that is not a CREATE ASSERTION"));
    }

    // The relation the *text* names must be the relation the request routed
    // on. Core 0 resolved both, so a disagreement is a build disagreeing
    // with itself - but the consequence is a cabin constraining one
    // relation while another's writes maintain it, so it is checked rather
    // than assumed.
    auto named = catalog_.FindTableOidByName(stmt->table_name);
    if (!named.ok()) return fail(named.status());
    if (named.value() != request.table_oid) {
        return fail(Status::InvalidArgument(
            "assertion build request routed on relation oid " +
            std::to_string(request.table_oid) + " but its declaration names '" +
            stmt->table_name + "', which is oid " + std::to_string(named.value())));
    }

    // The visibility the build's scan reads under: latest settled state,
    // minted here exactly as `HandleAssertion` mints it - the owner's own
    // view, which is the point of building on the owner. A core with no
    // manager reads everything, which is the pre-MVCC engine.
    txn::ReadView check_view = txn::ReadView::Everything();
    if (txn_ != nullptr) {
        auto minted = txn_->MintReadView(txn::kNoTrxId);
        if (!minted.ok()) return fail(minted.status());
        check_view = minted.value();
    }

    auto build = exec::BuildAssertionCabin(catalog_, store_, *stmt, request.assertion_id,
                                          check_view, wal_);
    if (!build.ok()) return fail(build.status());

    // Durable before the reply: core 0 commits a row naming this root on
    // the strength of it, and a crash after that commit must find the cabin
    // the recovered row folds onto. The entry pages, the `ASSERT_BUILD`
    // records and the `ASSERT_SNAPSHOT` base are all in this stream, so this
    // stream syncs whole - a blocking fsync on the reactor, as core 0's own
    // `CREATE ASSERTION` durability wait is.
    if (wal_ != nullptr) {
        if (Status s = wal_->SyncAll(); !s.ok()) {
            return fail(s.WithContext("making the built Bound Cabin durable"));
        }
    }

    const PageId root = build.value().cabin_root;
    const std::uint64_t rows = build.value().rows_incorporated;
    const std::uint32_t groups = static_cast<std::uint32_t>(build.value().group_count);

    // **Adopted here, inside the build's own task** - the header's argument
    // for why this protocol needs no refusal window. From this line on the
    // assertion is enforced on this core; if core 0's publish then fails,
    // `done(aborted)` takes it back.
    enforcer_.Adopt(std::move(build.value().live));

    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("assertion", "core " + std::to_string(core_id_) + " built assertion id " +
                                    std::to_string(request.assertion_id) + " on relation oid " +
                                    std::to_string(request.table_oid) + " for core " +
                                    std::to_string(requester) + ": root " +
                                    std::to_string(root) + ", " + std::to_string(rows) +
                                    " rows in " + std::to_string(groups) +
                                    " groups; enforcing here from now");
    }
    Reply(requester, request_id, request.assertion_id, root, rows, groups, Status::OK());
}

void AssertionBuildServer::Reply(std::uint32_t requester, std::uint64_t request_id,
                                 std::uint64_t assertion_id, PageId root, std::uint64_t rows,
                                 std::uint32_t groups, const Status& status) {
    AssertionBuildReplyPayload reply{};
    reply.assertion_id = assertion_id;
    reply.cabin_root = root;
    reply.rows_incorporated = rows;
    reply.group_count = groups;
    reply.status_code = static_cast<std::uint32_t>(status.code());
    const std::string& msg = status.message();
    std::memcpy(reply.message, msg.data(), std::min(msg.size(), sizeof(reply.message) - 1));
    // `session_core` is the constant 0 on every leg of this protocol: core
    // 0 owns the statement in both directions and nothing here reads the
    // field.
    sched::SubmitSendPod(scheduler_, transport_, core_id_, requester, /*session_core=*/0,
                         request_id, sched::RingMessageKind::kAssertionBuildReply, reply);
}

void AssertionBuildServer::OnDone(const sched::MessageHeader& header,
                                  std::span<const std::byte> payload) {
    AssertionBuildDonePayload done{};
    if (payload.size() != sizeof(done)) {
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("assertion", "assertion build done from core " +
                                         std::to_string(header.src_core) + " has " +
                                         std::to_string(payload.size()) + " bytes, not " +
                                         std::to_string(sizeof(done)) + "; dropped");
        }
        return;
    }
    std::memcpy(&done, payload.data(), sizeof(done));
    if (done.committed != 0) {
        // The catalog cache, so this core's `SHOW ASSERTIONS` resolves the
        // row core 0 has just written. Nothing else moves: the directory
        // has been enforcing since the build.
        if (on_committed_) on_committed_();
        if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
            log_->Info("assertion", "assertion id " + std::to_string(done.assertion_id) +
                                        " published by core " +
                                        std::to_string(header.src_core));
        }
        return;
    }
    // Aborted: what this core adopted is a constraint no catalog row names,
    // so it is evicted. A `done(aborted)` for an id this core never adopted
    // - the refusals, and a request that never arrived - is a no-op inside
    // `Evict`.
    enforcer_.Evict(done.assertion_id);
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("assertion", "assertion id " + std::to_string(done.assertion_id) +
                                    " aborted by core " + std::to_string(header.src_core) +
                                    "; its directory is evicted and its chain orphaned");
    }
}

// ---- Core 0's half ---------------------------------------------------------

Status AssertionBuildClient::RegisterReplyReceiver() {
    return scheduler_.RegisterMessageHandler(
        sched::RingMessageKind::kAssertionBuildReply,
        [this](const sched::MessageHeader& header, std::span<const std::byte> payload) {
            AssertionBuildReplyPayload reply{};
            if (payload.size() != sizeof(reply)) {
                if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                    log_->Error("assertion", "assertion build reply from core " +
                                                 std::to_string(header.src_core) + " has " +
                                                 std::to_string(payload.size()) + " bytes, not " +
                                                 std::to_string(sizeof(reply)) + "; dropped");
                }
                return;
            }
            std::memcpy(&reply, payload.data(), sizeof(reply));
            auto it = waiting_.find(header.request_id);
            if (it == waiting_.end()) {
                // Core 0 gave up on this one. A directory the owner adopted
                // for it is enforcing a constraint no row will ever name, so
                // saying `done(aborted)` is what takes it back; a refusal
                // adopted nothing and needs nothing.
                if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
                    log_->Debug("assertion", "assertion build reply for request " +
                                                 std::to_string(header.request_id) +
                                                 " from core " +
                                                 std::to_string(header.src_core) +
                                                 " matched no waiter");
                }
                if (reply.status_code == static_cast<std::uint32_t>(StatusCode::kOk)) {
                    Done(header.src_core, reply.assertion_id, /*committed=*/false);
                }
                return;
            }
            AssertionBuildOutcome& out = it->second;
            out.status = Status::FromWire(
                reply.status_code, std::string(reply.message,
                                               ::strnlen(reply.message, sizeof(reply.message))));
            out.cabin_root = out.status.ok() ? reply.cabin_root : kInvalidPageId;
            out.rows_incorporated = reply.rows_incorporated;
            out.group_count = reply.group_count;
            out.arrived = true;
        });
}

Status AssertionBuildClient::Request(std::uint32_t owner_core, std::uint64_t request_id,
                                     catalog::Oid table_oid, std::uint64_t assertion_id,
                                     std::string_view source_text) {
    auto request = AssertionBuildRequestOf(table_oid, assertion_id, source_text);
    if (!request.ok()) return request.status();
    AssertionBuildOutcome& out =
        waiting_.insert_or_assign(request_id, AssertionBuildOutcome{}).first->second;
    out.deadline_ns = clock_.Now() + kAssertionBuildReplyDeadlineNs;
    sched::SubmitSendPod(scheduler_, transport_, /*src=*/0, owner_core, /*session_core=*/0,
                         request_id, sched::RingMessageKind::kAssertionBuildRequest,
                         request.value());
    return Status::OK();
}

bool AssertionBuildClient::Settled(std::uint64_t request_id) const {
    auto it = waiting_.find(request_id);
    if (it == waiting_.end()) return true;
    return it->second.arrived || clock_.Now() >= it->second.deadline_ns;
}

const AssertionBuildOutcome* AssertionBuildClient::Find(std::uint64_t request_id) const {
    auto it = waiting_.find(request_id);
    return it == waiting_.end() ? nullptr : &it->second;
}

void AssertionBuildClient::Close(std::uint64_t request_id) { waiting_.erase(request_id); }

void AssertionBuildClient::Done(std::uint32_t owner_core, std::uint64_t assertion_id,
                                bool committed) {
    AssertionBuildDonePayload done{};
    done.assertion_id = assertion_id;
    done.committed = committed ? 1 : 0;
    sched::SubmitSendPod(scheduler_, transport_, /*src=*/0, owner_core, /*session_core=*/0,
                         /*request_id=*/0, sched::RingMessageKind::kAssertionBuildDone, done);
}

}  // namespace kds::server
