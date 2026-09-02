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

        // **A row this core is about to delete is answered busy, ahead of
        // reading whether it exists** (AJ-T1, AJ-R3(a)). It does exist —
        // that is the point — and vouching for it is what would dangle the
        // reference: the DELETE's own fan-out has already been told "no
        // children" by this child's owner, so an intent granted now would
        // be released by the child's commit before the DELETE's per-row
        // check ever looks for it.
        //
        // **Busy and not violation**, which is F3 and the same distinction
        // the intent table's mirror makes: the delete has not committed, so
        // the answer depends on how it ends, and the caller retries rather
        // than being told it is wrong. This is also exactly the verdict an
        // uncommitted delete-mark already produces through
        // `CheckParentPresent`, so the crossing gains no new answer — the
        // registration only makes that answer available before the mark is
        // written.
        //
        // Ahead of the existence read *and* of the grant below, because
        // either one alone would be a window: reading first would let a
        // pass be computed for a row already registered, and granting first
        // would hand out the reliance this check exists to refuse.
        if (pending_deletes_.Pending(parent_oid, parent_pk)) {
            pending_deletes_.NoteRefusal();
            verdicts.push_back(exec::FkVerdict::kBusy);
            continue;
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

// ---- The child owner's half (AJ-T2) --------------------------------------

void FkProbeServer::OnReverseRequest(const sched::MessageHeader& header,
                                     std::span<const std::byte> payload) {
    ++reverse_probes_;
    FkReverseProbeRequestPayload request{};
    if (payload.size() != sizeof(request)) {
        // `OnRequest`'s reasoning: the session id is inside the payload
        // this could not read, so there is nothing to reply *to*
        // coherently and the requester's deadline is what ends its park.
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("fk", "foreign-key reverse probe from core " +
                                  std::to_string(header.src_core) + " has " +
                                  std::to_string(payload.size()) + " payload bytes, not " +
                                  std::to_string(sizeof(request)));
        }
        return;
    }
    std::memcpy(&request, payload.data(), sizeof(request));

    std::vector<exec::FkVerdict> verdicts;
    if (request.count > kFkReverseProbeMaxEntries) {
        ReverseReply(header.src_core, header.request_id, request.session_id, verdicts,
                     Status::InvalidArgument(
                         "a foreign-key reverse probe named " + std::to_string(request.count) +
                         " children, past the " + std::to_string(kFkReverseProbeMaxEntries) +
                         " one request carries"));
        return;
    }

    // §4's one-MVCC rule, unchanged and for `OnRequest`'s reason: **this
    // core's own latest state**, minted here rather than carried on the
    // wire. A read view sent from the deleting core would be that core's
    // idea of who is live, applied to rows it cannot see.
    txn::ReadView check_view = txn::ReadView::Everything();
    if (txn_ != nullptr) {
        auto minted = txn_->MintReadView(/*writer=*/0);
        if (!minted.ok()) {
            ReverseReply(header.src_core, header.request_id, request.session_id, verdicts,
                         minted.status());
            return;
        }
        check_view = minted.value();
    }

    exec::Budget budget;
    verdicts.reserve(request.count);
    for (std::uint32_t i = 0; i < request.count; ++i) {
        const auto child_oid = static_cast<catalog::Oid>(request.child_oid[i]);

        auto child = catalog_.InitTableAccess(child_oid);
        if (!child.ok()) {
            ReverseReply(header.src_core, header.request_id, request.session_id, verdicts,
                         child.status());
            return;
        }
        // **Fail-closed on a child that is not this core's**, the mirror of
        // the forward's ownership re-check and the same race: the deleting
        // core resolved this owner from its own catalog, which can be
        // stale. Answering "no children" from a relation this core does not
        // own is precisely the dangling reference the fan-out exists to
        // prevent, one hop further along.
        if (child.value()->owner_core != core_id_) {
            ReverseReply(header.src_core, header.request_id, request.session_id, verdicts,
                         Status::TxnConflict("relation oid " + std::to_string(child_oid) +
                                             " is not owned by core " + std::to_string(core_id_) +
                                             " any more; re-resolve and retry"));
            return;
        }

        // The check this core already runs for its own parents, with its
        // own `core_id` as the scope the answer is good for. A child whose
        // ranges this core does not wholly own is refused inside
        // `CheckNoChildReferences` itself (AE-5.1 keeps that refusal), so
        // the split case needs nothing here.
        exec::FkReverseOptions options;
        options.core_id = core_id_;
        const catalog::TableAccess::CabinRef cabin =
            child.value()->CabinOn(request.child_column_no[i]);
        if (cabins_ != nullptr && cabin.id != 0) {
            options.cabins = cabins_;
            options.cabin_id = cabin.id;
        }

        auto outcome =
            exec::CheckNoChildReferences(store_, *child.value(), request.child_column_no[i],
                                         request.parent_pk[i], check_view, options, &budget);
        if (!outcome.ok()) {
            ReverseReply(header.src_core, header.request_id, request.session_id, verdicts,
                         outcome.status());
            return;
        }
        // **Nothing is recorded and nothing is granted** (AJ-R5). The
        // forward's equivalent line grants a reference intent; this core
        // answers and forgets, which is what keeps it out of the decide and
        // off the 720x release leg an intent-holding participant pays.
        verdicts.push_back(outcome.value().verdict);
    }

    ReverseReply(header.src_core, header.request_id, request.session_id, verdicts, Status::OK());
}

void FkProbeServer::ReverseReply(std::uint32_t requester, std::uint64_t request_id,
                                 std::uint64_t session_id,
                                 const std::vector<exec::FkVerdict>& verdicts,
                                 const Status& status) {
    FkReverseProbeReplyPayload reply{};
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
            log_->Info("fk", "foreign-key reverse probe from core " + std::to_string(requester) +
                                 " refused: " + status.message());
        }
    }
    sched::SubmitSendPod(scheduler_, transport_, core_id_, requester,
                         /*session_core=*/requester, request_id,
                         sched::RingMessageKind::kFkReverseProbeReply, reply);
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

void FkProbeClient::Land(std::uint64_t request_id, std::uint32_t status_code, const char* message,
                         const std::uint8_t* verdict, std::uint32_t count, std::uint32_t cap) {
    auto it = waiting_.find(request_id);
    // **A reply matching no waiter is this core having given up** - the
    // deadline passed, or the statement had no reactor to park on. Nothing
    // to undo here: the intents the owner granted are released by the
    // transaction's decide either way, which is the property that makes an
    // abandoned probe cost memory on the owner for the transaction's life
    // and never a leak. A reverse reply has nothing to undo at all - the
    // answering core recorded nothing (AJ-R5).
    if (it == waiting_.end()) return;

    it->second.arrived = true;
    if (status_code != 0) {
        it->second.status = Status::FromWire(status_code, message);
        return;
    }
    it->second.status = Status::OK();
    it->second.verdicts.clear();
    // Clamped to the payload's own array, because `count` came off the
    // wire: a forged one would read past the end of a fixed array that the
    // sender's own cap can no longer vouch for.
    const std::uint32_t n = count <= cap ? count : cap;
    it->second.verdicts.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        it->second.verdicts.push_back(static_cast<exec::FkVerdict>(verdict[i]));
    }
}

Status FkProbeClient::RegisterReplyReceiver() {
    if (Status s = scheduler_.RegisterMessageHandler(
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
                Land(header.request_id, reply.status_code, reply.message, reply.verdict,
                     reply.count, kFkProbeMaxParents);
            });
        !s.ok()) {
        return s;
    }

    // **The reverse direction's replies land in the same waiter map**
    // (AJ-T2), which is what lets one park cover a mixed set of request
    // ids: `FkProbeOutcome` holds an arrival, a status and positional
    // verdicts, none of it direction-specific. Two kinds, one landing.
    return scheduler_.RegisterMessageHandler(
        sched::RingMessageKind::kFkReverseProbeReply,
        [this](const sched::MessageHeader& header, std::span<const std::byte> payload) {
            FkReverseProbeReplyPayload reply{};
            if (payload.size() != sizeof(reply)) {
                if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                    log_->Error("fk", "foreign-key reverse probe reply from core " +
                                          std::to_string(header.src_core) + " has " +
                                          std::to_string(payload.size()) + " payload bytes");
                }
                return;
            }
            std::memcpy(&reply, payload.data(), sizeof(reply));
            Land(header.request_id, reply.status_code, reply.message, reply.verdict, reply.count,
                 kFkReverseProbeMaxEntries);
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

    // Counted where the round actually leaves, not on entry: a request
    // past the cap opens no waiter and sends nothing, and counting it would
    // report a crossing that never happened.
    ++requests_;
    sched::SubmitSendPod(scheduler_, transport_, core_id_, owner_core,
                         /*session_core=*/core_id_, request_id,
                         sched::RingMessageKind::kFkProbeRequest, request);
    return Status::OK();
}

Status FkProbeClient::RequestReverse(std::uint32_t owner_core, std::uint64_t request_id,
                                     std::uint64_t session_id, std::uint64_t transaction_id,
                                     const FkReverseProbeGroup& group) {
    if (group.entries.size() > kFkReverseProbeMaxEntries) {
        // Fail-closed and no waiter opened, `Request`'s rule: a request this
        // core cannot phrase is a statement it cannot run, not one it runs
        // partially. Reachable only for a parent with more foreign children
        // on one core than a message carries, which is a schema nobody has
        // written - and the refusal is what keeps that honest.
        return Status::NotImplemented(
            "this statement names " + std::to_string(group.entries.size()) +
            " child relations on core " + std::to_string(owner_core) + ", past the " +
            std::to_string(kFkReverseProbeMaxEntries) +
            " one reverse probe carries; chunking is not built "
            "(instructions/v2.8.0/workorder-aj.md AJ-T2)");
    }

    FkReverseProbeRequestPayload request{};
    request.session_id = session_id;
    request.transaction_id = transaction_id;
    request.count = static_cast<std::uint32_t>(group.entries.size());
    for (std::size_t i = 0; i < group.entries.size(); ++i) {
        request.child_oid[i] = group.entries[i].child_oid;
        request.parent_pk[i] = group.entries[i].parent_pk;
        request.child_column_no[i] = group.entries[i].child_column_no;
    }

    FkProbeOutcome& outcome = waiting_[request_id];
    outcome.arrived = false;
    outcome.status = Status::OK();
    outcome.verdicts.clear();
    outcome.deadline_ns = clock_.Now() + kFkProbeReplyDeadlineNs;

    // Counted where the round actually leaves, `Request`'s rule.
    ++reverse_requests_;
    sched::SubmitSendPod(scheduler_, transport_, core_id_, owner_core,
                         /*session_core=*/core_id_, request_id,
                         sched::RingMessageKind::kFkReverseProbeRequest, request);
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
