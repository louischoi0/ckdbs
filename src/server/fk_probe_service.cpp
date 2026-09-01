#include "kds/server/fk_probe_service.hpp"

#include <cstring>
#include <string>

#include "kds/base/crash_point.hpp"
#include "kds/sched/send_retry.hpp"

namespace kds::server {
namespace {

// The reply's message field, filled without ever running off the end. The
// owner's log holds the whole; the child's core sees this much.
void SetMessage(char (&dst)[kFkProbeReplyMessageBytes], std::string_view text) {
    const std::size_t n = text.size() < kFkProbeReplyMessageBytes - 1
                              ? text.size()
                              : kFkProbeReplyMessageBytes - 1;
    std::memcpy(dst, text.data(), n);
    dst[n] = '\0';
}

}  // namespace

// ---- The parent owner's half ---------------------------------------------

void FkProbeServer::OnRequest(const sched::MessageHeader& header,
                              std::span<const std::byte> payload) {
    ++probes_;
    FkProbeRequestPayload request{};
    if (payload.size() != sizeof(request)) {
        // Nothing to reply *to* coherently - the session id is inside the
        // payload this could not read - so the requester's deadline is what
        // ends its park. Logged rather than guessed at.
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("fk", "foreign-key probe from core " + std::to_string(header.src_core) +
                                  " has " + std::to_string(payload.size()) + " payload bytes, not " +
                                  std::to_string(sizeof(request)));
        }
        return;
    }
    std::memcpy(&request, payload.data(), sizeof(request));

    std::vector<exec::FkVerdict> verdicts;
    if (request.count > kFkProbeMaxParents) {
        Reply(header.src_core, header.request_id, request.session_id, verdicts,
              Status::InvalidArgument("a foreign-key probe named " +
                                      std::to_string(request.count) +
                                      " parents, past the " +
                                      std::to_string(kFkProbeMaxParents) + " one request carries"));
        return;
    }

    // The view a constraint check reads under (§4): **latest state**, minted
    // here rather than carried on the wire. Carrying it would be carrying
    // one core's idea of who is live to a core with its own; what §4 asks
    // for is the parent owner's own now, which is what this is.
    txn::ReadView check_view = txn::ReadView::Everything();
    if (txn_ != nullptr) {
        auto minted = txn_->MintReadView(/*writer=*/0);
        if (!minted.ok()) {
            Reply(header.src_core, header.request_id, request.session_id, verdicts,
                  minted.status());
            return;
        }
        check_view = minted.value();
    }

    const FkIntentHolder holder{header.src_core, request.session_id};
    exec::Budget budget;

    verdicts.reserve(request.count);
    for (std::uint32_t i = 0; i < request.count; ++i) {
        const auto parent_oid = static_cast<catalog::Oid>(request.parent_oid[i]);
        const std::uint64_t parent_pk = request.parent_pk[i];

        auto parent = catalog_.InitTableAccess(parent_oid);
        if (!parent.ok()) {
            Reply(header.src_core, header.request_id, request.session_id, verdicts,
                  parent.status());
            return;
        }
        // **Fail-closed on a parent that is not this core's.** The child's
        // core resolved the owner from its own catalog, which can be stale
        // - a migration between its read and this handler is exactly the
        // race - and answering from a relation this core does not own would
        // be the same wrong answer the whole order exists to prevent, just
        // one hop further along.
        if (parent.value()->owner_core != core_id_) {
            Reply(header.src_core, header.request_id, request.session_id, verdicts,
                  Status::TxnConflict("relation oid " + std::to_string(parent_oid) +
                                      " is not owned by core " + std::to_string(core_id_) +
                                      " any more; re-resolve and retry"));
            return;
        }

        auto verdict = exec::CheckParentPresent(store_, *parent.value(), parent_pk, check_view,
                                                &budget);
        if (!verdict.ok()) {
            Reply(header.src_core, header.request_id, request.session_id, verdicts,
                  verdict.status());
            return;
        }

        // **The intent is granted on a pass and only on a pass.** A
        // violation promises nothing - there is no row to hold still - and
        // a busy answer is already the statement being told to retry, which
        // it will do by probing again.
        if (verdict.value() == exec::FkVerdict::kPass) {
            intents_.Add(parent_oid, parent_pk, holder);
            // AH-T5: the intent is granted and this participant has not
            // prepared. **The window the whole memory-residency argument
            // rests on** (AH-R5): the intent dies with the process, and
            // what makes that safe is that a participant which restarts
            // here cannot answer the prepare, so the coordinator's
            // transaction fails rather than committing a child row whose
            // parent nobody is holding still. A window in which the
            // coordinator can still commit is a defect of AH, full stop -
            // so this point exists to be killed at rather than to be
            // reasoned about.
            base::CrashPointHit("participant.fk_intent_granted_preprepare");
        }
        verdicts.push_back(verdict.value());
    }

    Reply(header.src_core, header.request_id, request.session_id, verdicts, Status::OK());
}

void FkProbeServer::Reply(std::uint32_t requester, std::uint64_t request_id,
                          std::uint64_t session_id, const std::vector<exec::FkVerdict>& verdicts,
                          const Status& status) {
    FkProbeReplyPayload reply{};
    reply.session_id = session_id;
    reply.status_code = static_cast<std::uint32_t>(status.code());
    if (status.ok()) {
        reply.count = static_cast<std::uint32_t>(verdicts.size());
        for (std::size_t i = 0; i < verdicts.size(); ++i) {
            reply.verdict[i] = static_cast<std::uint8_t>(verdicts[i]);
        }
    } else {
        SetMessage(reply.message, status.message());
        if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
            log_->Info("fk", "foreign-key probe from core " + std::to_string(requester) +
                                 " refused: " + status.message());
        }
    }
    sched::SubmitSendPod(scheduler_, transport_, core_id_, requester,
                         /*session_core=*/requester, request_id,
                         sched::RingMessageKind::kFkProbeReply, reply);
}

// ---- The child core's half -----------------------------------------------

Status FkProbeClient::RegisterReplyReceiver() {
    return scheduler_.RegisterMessageHandler(
        sched::RingMessageKind::kFkProbeReply,
        [this](const sched::MessageHeader& header, std::span<const std::byte> payload) {
            FkProbeReplyPayload reply{};
            if (payload.size() != sizeof(reply)) {
                if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                    log_->Error("fk", "foreign-key probe reply from core " +
                                          std::to_string(header.src_core) + " has " +
                                          std::to_string(payload.size()) + " payload bytes");
                }
                return;
            }
            std::memcpy(&reply, payload.data(), sizeof(reply));

            auto it = waiting_.find(header.request_id);
            // **A reply matching no waiter is this core having given up** -
            // the deadline passed, or the statement had no reactor to park
            // on. Nothing to undo here: the intents the owner granted are
            // released by the transaction's decide either way, which is the
            // property that makes an abandoned probe cost memory on the
            // owner for the transaction's life and never a leak.
            if (it == waiting_.end()) return;

            it->second.arrived = true;
            if (reply.status_code != 0) {
                it->second.status = Status::FromWire(reply.status_code, reply.message);
                return;
            }
            it->second.status = Status::OK();
            it->second.verdicts.clear();
            const std::uint32_t n =
                reply.count <= kFkProbeMaxParents ? reply.count : kFkProbeMaxParents;
            it->second.verdicts.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                it->second.verdicts.push_back(static_cast<exec::FkVerdict>(reply.verdict[i]));
            }
        });
}

Status FkProbeClient::Request(std::uint32_t owner_core, std::uint64_t request_id,
                              std::uint64_t session_id, std::uint64_t transaction_id,
                              const exec::FkParentVerdicts::ForeignGroup& group) {
    if (group.parents.size() > kFkProbeMaxParents) {
        // Fail-closed, and no waiter opened: a request this core cannot
        // phrase is a statement it cannot run, not one it runs partially.
        return Status::NotImplemented(
            "this statement names " + std::to_string(group.parents.size()) +
            " distinct parent rows on core " + std::to_string(owner_core) + ", past the " +
            std::to_string(kFkProbeMaxParents) +
            " one probe carries; chunking is not built "
            "(instructions/v2.8.0/workorder-ah.md AH-T2)");
    }

    FkProbeRequestPayload request{};
    request.session_id = session_id;
    request.transaction_id = transaction_id;
    request.count = static_cast<std::uint32_t>(group.parents.size());
    for (std::size_t i = 0; i < group.parents.size(); ++i) {
        request.parent_oid[i] = group.parents[i].first;
        request.parent_pk[i] = group.parents[i].second;
    }

    FkProbeOutcome& outcome = waiting_[request_id];
    outcome.arrived = false;
    outcome.status = Status::OK();
    outcome.verdicts.clear();
    outcome.deadline_ns = clock_.Now() + kFkProbeReplyDeadlineNs;

    sched::SubmitSendPod(scheduler_, transport_, core_id_, owner_core,
                         /*session_core=*/core_id_, request_id,
                         sched::RingMessageKind::kFkProbeRequest, request);
    return Status::OK();
}

bool FkProbeClient::Settled(std::uint64_t request_id) const {
    auto it = waiting_.find(request_id);
    if (it == waiting_.end()) return true;  // closed, or never opened
    if (it->second.arrived) return true;
    return clock_.Now() >= it->second.deadline_ns;
}

const FkProbeOutcome* FkProbeClient::Find(std::uint64_t request_id) const {
    auto it = waiting_.find(request_id);
    return it == waiting_.end() ? nullptr : &it->second;
}

void FkProbeClient::Close(std::uint64_t request_id) { waiting_.erase(request_id); }

}  // namespace kds::server
