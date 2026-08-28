#include "kds/server/txn_2pc_service.hpp"

#include <cstring>
#include <string>
#include <utility>

#include "kds/sched/send_retry.hpp"
#include "kds/server/utf8_prefix.hpp"

namespace kds::server {

StatusOr<TxnDecision> TxnDecisionOf(const TxnDecideRequestPayload& decide) {
    switch (static_cast<TxnDecision>(decide.decision)) {
        case TxnDecision::kCommit: return TxnDecision::kCommit;
        case TxnDecision::kAbort: return TxnDecision::kAbort;
        case TxnDecision::kUnset: break;
    }
    // Neither defaulted nor guessed. `kUnset` is the zeroed buffer and any
    // other byte is two ends disagreeing about what a decision is; reading
    // either as commit would apply a transaction nobody decided, and as
    // abort would discard one that may already be committed in the
    // coordinator's stream - which is the decision, by D4.
    return Status::InvalidArgument("cross-owner transaction: decide names decision " +
                                   std::to_string(decide.decision) +
                                   ", which is not a decision this build knows; the "
                                   "participant stays in doubt rather than guessing");
}

StatusOr<std::string_view> TxnParticipantReplyMessageOf(const TxnParticipantReplyPayload& reply) {
    if (reply.message_len > kTxnParticipantReplyMessageMax) {
        return Status::InvalidArgument("cross-owner transaction: participant reply names a "
                                       "message of " +
                                       std::to_string(reply.message_len) +
                                       " bytes, which is not a length this payload can hold");
    }
    return std::string_view(reply.message, reply.message_len);
}

TxnParticipantReplyPayload TxnParticipantReplyOf(std::uint64_t session_id,
                                                 std::uint64_t transaction_id,
                                                 const Status& status) {
    TxnParticipantReplyPayload out{};
    out.session_id = session_id;
    out.transaction_id = transaction_id;
    out.status_code = static_cast<std::uint32_t>(status.code());
    // A success carries no message, so the length stays 0 and the array
    // stays zeroed - the reader's `message_len` bound then reads an empty
    // view, which is what `TxnParticipantReplyMessageOf` calls legal.
    if (status.ok()) return out;
    const std::string& message = status.message();
    const std::size_t len = Utf8PrefixLen(message, kTxnParticipantReplyMessageMax);
    out.message_len = static_cast<std::uint16_t>(len);
    if (len > 0) std::memcpy(out.message, message.data(), len);
    return out;
}

// ---- The participant's half -----------------------------------------------

namespace {

// The refusal a core with no seam gives, written once because it is given
// twice and two copies of a message that must mean one thing are how they
// stop meaning it. The wire working and nothing executing on it must not
// look alike (SS1's rule): without this a mis-wired core would cost the
// coordinator a whole deadline per phase, which on the prepare leg reads as
// an abort and says nothing about why.
Status NoParticipantSeam() {
    return Status::Unsupported(
        "cross-owner transaction: this core has no participant seam installed; the wire is "
        "built and the participant is not");
}

// The bytes into the POD, bounded rather than trusted. **The one case that
// gets no reply**: nothing in a mis-sized payload names the waiter parked on
// the other side, so there is no address to answer, and the coordinator's
// deadline is the backstop.
template <class Payload>
bool CopyRequest(const sched::MessageHeader& header, std::span<const std::byte> payload,
                 const char* leg, Logger* log, Payload& out) {
    if (payload.size() != sizeof(out)) {
        if (log != nullptr && log->enabled(LogLevel::kError)) {
            log->Error("2pc", std::string(leg) + " from core " +
                                  std::to_string(header.src_core) + " has " +
                                  std::to_string(payload.size()) + " bytes, not " +
                                  std::to_string(sizeof(out)) + "; dropped");
        }
        return false;
    }
    std::memcpy(&out, payload.data(), sizeof(out));
    return true;
}

}  // namespace

void Txn2pcServer::OnPrepare(const sched::MessageHeader& header,
                             std::span<const std::byte> payload) {
    ++prepares_;
    TxnPrepareRequestPayload request{};
    // A dropped prepare costs the coordinator a deadline, which on this leg
    // is an abort - safe, because a request this core could not read
    // prepared nothing.
    if (!CopyRequest(header, payload, "prepare", log_, request)) return;

    // Copied out before the payload can die: the seam parks on a device
    // sync, and `request` lives only as long as this call.
    PrepareAsk ask;
    ask.coordinator = header.src_core;
    ask.session_id = request.session_id;
    ask.transaction_id = request.transaction_id;
    ask.retry = request.retry != 0;
    const std::uint64_t request_id = header.request_id;

    if (ask.transaction_id == 0) {
        // The payload's own invariant, checked where the bytes arrive: a
        // transaction id of 0 is the id no transaction has, so it cannot be
        // recorded in a PREPARE record that recovery could resolve. Refused
        // rather than prepared against, which would write a record no
        // coordinator answers for.
        Reply(ask.coordinator, request_id, sched::RingMessageKind::kTxnPrepareReply,
              ask.session_id, ask.transaction_id,
              Status::InvalidArgument(
                  "cross-owner transaction: prepare names coordinator transaction 0, which no "
                  "transaction has"));
        return;
    }
    if (!prepare_) {
        Reply(ask.coordinator, request_id, sched::RingMessageKind::kTxnPrepareReply,
              ask.session_id, ask.transaction_id, NoParticipantSeam());
        return;
    }
    prepare_(ask, [this, coordinator = ask.coordinator, request_id, session_id = ask.session_id,
                   transaction_id = ask.transaction_id](const Status& status) {
        Reply(coordinator, request_id, sched::RingMessageKind::kTxnPrepareReply, session_id,
              transaction_id, status);
    });
}

void Txn2pcServer::OnDecide(const sched::MessageHeader& header,
                            std::span<const std::byte> payload) {
    ++decides_;
    TxnDecideRequestPayload request{};
    // A dropped decide is worse than a dropped prepare and the same code
    // handles both: this core stays in doubt until the decision is resent.
    if (!CopyRequest(header, payload, "decide", log_, request)) return;

    DecideAsk ask;
    ask.coordinator = header.src_core;
    ask.session_id = request.session_id;
    ask.transaction_id = request.transaction_id;
    ask.retry = request.retry != 0;
    const std::uint64_t request_id = header.request_id;

    // Refused rather than guessed, and the refusal leaves this core in
    // doubt on purpose (the header's fail-closed paragraph): neither commit
    // nor abort is the safe reading of a byte that names no decision.
    auto decision = TxnDecisionOf(request);
    if (!decision.ok()) {
        Reply(ask.coordinator, request_id, sched::RingMessageKind::kTxnDecideReply,
              ask.session_id, ask.transaction_id, decision.status());
        return;
    }
    ask.decision = decision.value();

    if (!decide_) {
        Reply(ask.coordinator, request_id, sched::RingMessageKind::kTxnDecideReply,
              ask.session_id, ask.transaction_id, NoParticipantSeam());
        return;
    }
    decide_(ask, [this, coordinator = ask.coordinator, request_id, session_id = ask.session_id,
                  transaction_id = ask.transaction_id](const Status& status) {
        Reply(coordinator, request_id, sched::RingMessageKind::kTxnDecideReply, session_id,
              transaction_id, status);
    });
}

// ---- R6-5: the in-doubt ask, from this core as a participant ----------------

Status Txn2pcServer::RegisterResolveReplyReceiver() {
    return scheduler_.RegisterMessageHandler(
        sched::RingMessageKind::kTxnResolveReply,
        [this](const sched::MessageHeader& header, std::span<const std::byte> payload) {
            OnResolveReply(header, payload);
        });
}

void Txn2pcServer::Ask(std::uint32_t coordinator, std::uint64_t session_id,
                       std::uint64_t transaction_id) {
    TxnResolveRequestPayload request{};
    request.session_id = session_id;
    request.transaction_id = transaction_id;
    // **Always set, and that is the leg's contract** (R6-0): the decide this
    // asks about was the first attempt, so an ask is a resend by
    // construction and a coordinator that no longer holds the record must
    // answer `UnknownOutcome` rather than re-decide.
    request.retry = 1;
    sched::SubmitSendPod(scheduler_, transport_, core_id_, coordinator,
                         /*session_core=*/coordinator, next_ask_id_++,
                         sched::RingMessageKind::kTxnResolveRequest, request);
}

void Txn2pcServer::OnResolveReply(const sched::MessageHeader& header,
                                  std::span<const std::byte> payload) {
    TxnResolveReplyPayload reply{};
    // Dropped rather than answered: this is a reply, so there is nobody to
    // answer, and the participant's own ceiling asks again.
    if (!CopyRequest(header, payload, "resolve reply", log_, reply)) return;
    if (!resolve_) {
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("2pc", "core " + std::to_string(core_id_) +
                                   " received an in-doubt answer from core " +
                                   std::to_string(header.src_core) +
                                   " with no resolve seam installed; dropped");
        }
        return;
    }

    ResolveAnswer answer;
    answer.coordinator = header.src_core;
    answer.session_id = reply.session_id;
    answer.transaction_id = reply.transaction_id;
    // No message crosses this leg (the header's "a participant is not a
    // client"), so the code is rebuilt with the words this core would use.
    answer.status = Status::FromWire(
        reply.status_code, "cross-owner transaction: core " + std::to_string(header.src_core) +
                               " answered this core's in-doubt ask for transaction " +
                               std::to_string(reply.transaction_id));
    if (answer.status.ok()) {
        auto decision = static_cast<TxnDecision>(reply.decision);
        if (decision != TxnDecision::kCommit && decision != TxnDecision::kAbort) {
            // A decided answer whose decision byte names nothing. Refused
            // into doubt rather than guessed, `TxnDecisionOf`'s rule: the
            // participant keeps waiting and asks again, which is the state
            // it was already in.
            if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                log_->Error("2pc", "core " + std::to_string(header.src_core) +
                                       " answered an in-doubt ask with decision byte " +
                                       std::to_string(reply.decision) +
                                       ", which names no decision; this core stays in doubt");
            }
            return;
        }
        answer.decision = decision;
    }
    resolve_(answer);
}

void Txn2pcServer::Reply(std::uint32_t coordinator, std::uint64_t request_id,
                         sched::RingMessageKind kind, std::uint64_t session_id,
                         std::uint64_t transaction_id, const Status& status) {
    ++replies_;
    // The identity rides back so the coordinator can check an answer
    // against the waiter the ring matched it to rather than trust it.
    const TxnParticipantReplyPayload reply =
        TxnParticipantReplyOf(session_id, transaction_id, status);
    // `session_core` is the *coordinator's*: the client's session lives
    // there, so a reader of a captured header can see whose transaction is
    // parked on this.
    sched::SubmitSendPod(scheduler_, transport_, core_id_, coordinator,
                         /*session_core=*/coordinator, request_id, kind, reply);
}

// ---- The coordinator's half -----------------------------------------------

Status Txn2pcClient::RegisterReplyReceivers() {
    if (Status s = scheduler_.RegisterMessageHandler(
            sched::RingMessageKind::kTxnPrepareReply,
            [this](const sched::MessageHeader& header, std::span<const std::byte> payload) {
                OnReply(TxnPhase::kPrepare, header, payload);
            });
        !s.ok()) {
        return s;
    }
    if (Status s = scheduler_.RegisterMessageHandler(
            sched::RingMessageKind::kTxnDecideReply,
            [this](const sched::MessageHeader& header, std::span<const std::byte> payload) {
                OnReply(TxnPhase::kDecide, header, payload);
            });
        !s.ok()) {
        return s;
    }
    // R6-5: the one *request* this half takes. Registered with the replies
    // rather than in a call of its own, because a coordinator that answers
    // prepares but not asks is a coordinator whose participants wait out
    // their ceilings for ever - the wiring must not be able to install one
    // half of that.
    return scheduler_.RegisterMessageHandler(
        sched::RingMessageKind::kTxnResolveRequest,
        [this](const sched::MessageHeader& header, std::span<const std::byte> payload) {
            OnResolveAsk(header, payload);
        });
}

Status Txn2pcClient::OpenPhase(std::uint64_t request_id, TxnPhase phase,
                               std::uint64_t session_id, std::uint64_t transaction_id,
                               std::span<const std::uint32_t> participants) {
    // **One live waiter per request id**, for `StatementShipClient::Ship`'s
    // reason: reusing an id that still has a phase parked on it would
    // replace that phase's waiter, and the identity check could not catch
    // it - the identity it compares against would by then be this phase's.
    if (waiting_.find(request_id) != waiting_.end()) {
        return Status::InvalidArgument(
            "cross-owner transaction: request id " + std::to_string(request_id) +
            " already has a phase parked on it; ids are allocated per core and per phase");
    }
    if (participants.empty()) {
        // D1's fast path is the *caller's* to take, and taking it here
        // instead would hide a caller that thinks it has participants when
        // it has none. A phase over nobody is a programming error, not a
        // transaction shape.
        return Status::InvalidArgument(
            "cross-owner transaction: a phase over no participants is not a phase; a "
            "one-owner transaction takes the single-core path and enters no protocol");
    }
    if (transaction_id == 0) {
        return Status::InvalidArgument(
            "cross-owner transaction: a phase must name the coordinator's transaction id, and "
            "0 is the id no transaction has");
    }
    for (std::size_t i = 0; i < participants.size(); ++i) {
        // Bounded before the send: an out-of-range core is the one send
        // failure `MakeSendRetryTask` does not retry, so the message would
        // be dropped and the phase would cost a whole deadline before
        // saying what is already known here.
        if (participants[i] >= transport_.core_count()) {
            return Status::InvalidArgument(
                "cross-owner transaction: core " + std::to_string(participants[i]) +
                " is not a core of this instance, which has " +
                std::to_string(transport_.core_count()));
        }
        if (participants[i] == core_id_) {
            // The coordinator is not one of its own participants: its half
            // of the transaction is the local one, which commits through
            // its own stream and needs no message. Admitting it would have
            // this core prepare a context it never enrolled.
            return Status::InvalidArgument(
                "cross-owner transaction: core " + std::to_string(core_id_) +
                " is the coordinator and cannot be one of its own participants");
        }
        for (std::size_t j = 0; j < i; ++j) {
            // A repeated participant would take two waiter slots and one
            // reply, so the phase could only end on its deadline.
            if (participants[i] == participants[j]) {
                return Status::InvalidArgument(
                    "cross-owner transaction: core " + std::to_string(participants[i]) +
                    " is named twice in one phase's participants");
            }
        }
    }

    TxnPhaseOutcome& outcome = waiting_[request_id];
    outcome.phase = phase;
    outcome.session_id = session_id;
    outcome.transaction_id = transaction_id;
    outcome.participants.reserve(participants.size());
    for (std::uint32_t core : participants) {
        outcome.participants.push_back(TxnParticipantOutcome{core, false, Status::OK()});
    }
    outcome.outstanding = participants.size();
    outcome.deadline_ns = clock_.Now() + kTxnPhaseDeadlineNs;
    return Status::OK();
}

Status Txn2pcClient::Prepare(std::uint64_t request_id, std::uint64_t session_id,
                             std::uint64_t transaction_id,
                             std::span<const std::uint32_t> participants) {
    if (Status s = OpenPhase(request_id, TxnPhase::kPrepare, session_id, transaction_id,
                             participants);
        !s.ok()) {
        return s;
    }
    // **The record opens undecided, here** (R6-5), before the first prepare
    // leaves. A participant that prepares and then hears nothing asks about
    // a transaction whose decision may be a few milliseconds away, and the
    // honest answer to that is "not yet, ask again" - which needs a record
    // to distinguish it from "no such transaction". Opening it after the
    // decision instead would answer `UnknownOutcome` for the whole width of
    // the prepare phase, and D5 makes that answer terminal: the participant
    // stops asking and waits for the next mount.
    OpenDecisionRecord(session_id, transaction_id, clock_.Now());

    TxnPrepareRequestPayload request{};
    request.session_id = session_id;
    request.transaction_id = transaction_id;
    // R6-0's bit, and the prepare leg sends only first attempts: nothing
    // resends a prepare. The resend path is D5's resolution ask, which
    // `Txn2pcServer::Ask` sends from the participant's side with the bit
    // set - the sender the R6-0 contract was waiting for.
    request.retry = 0;
    for (std::uint32_t core : participants) {
        ++prepare_messages_;
        sched::SubmitSendPod(scheduler_, transport_, core_id_, core, /*session_core=*/core_id_,
                             request_id, sched::RingMessageKind::kTxnPrepareRequest, request);
    }
    return Status::OK();
}

Status Txn2pcClient::Decide(std::uint64_t request_id, std::uint64_t session_id,
                            std::uint64_t transaction_id, TxnDecision decision,
                            std::span<const std::uint32_t> participants) {
    if (decision == TxnDecision::kUnset) {
        return Status::InvalidArgument(
            "cross-owner transaction: a decide must name commit or abort; kUnset is the zeroed "
            "buffer and a participant refuses it into doubt");
    }
    if (Status s =
            OpenPhase(request_id, TxnPhase::kDecide, session_id, transaction_id, participants);
        !s.ok()) {
        return s;
    }

    // **Recorded before the sends, and unconditionally** (R6-5). The caller
    // has already made the decision durable in this core's own stream -
    // that is the order `DispatchAsync` keeps, and D4's reason for it - so
    // from here an in-doubt participant is answered with the real decision
    // even if every decide message the ring takes is dropped. Recording it
    // after the loop would leave the one window where an ask provoked by a
    // *lost* decide could arrive before the record existed.
    const sched::MonoTimeNs now = clock_.Now();
    DecisionRecord& record = OpenDecisionRecord(session_id, transaction_id, now);
    record.decision = decision;
    record.decided_at_ns = now;

    TxnDecideRequestPayload request{};
    request.session_id = session_id;
    request.transaction_id = transaction_id;
    request.decision = static_cast<std::uint8_t>(decision);
    request.retry = 0;
    for (std::uint32_t core : participants) {
        ++decide_messages_;
        sched::SubmitSendPod(scheduler_, transport_, core_id_, core, /*session_core=*/core_id_,
                             request_id, sched::RingMessageKind::kTxnDecideRequest, request);
    }
    return Status::OK();
}

void Txn2pcClient::OnReply(TxnPhase phase, const sched::MessageHeader& header,
                           std::span<const std::byte> payload) {
    TxnParticipantReplyPayload reply{};
    if (payload.size() != sizeof(reply)) {
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("2pc", "a participant reply from core " +
                                   std::to_string(header.src_core) + " has " +
                                   std::to_string(payload.size()) + " bytes, not " +
                                   std::to_string(sizeof(reply)) + "; dropped");
        }
        return;
    }
    std::memcpy(&reply, payload.data(), sizeof(reply));

    auto it = waiting_.find(header.request_id);
    if (it == waiting_.end()) {
        // The phase settled on its deadline and the coordinator has already
        // acted: on prepare that means the transaction was aborted, on
        // decide that the participant was left to D5's resolution. Nothing
        // can be undone here; counting it is what makes a tight deadline
        // legible.
        ++late_replies_;
        if (log_ != nullptr && log_->enabled(LogLevel::kWarn)) {
            log_->Warn("2pc", "a participant's answer from core " +
                                  std::to_string(header.src_core) +
                                  " arrived after its phase settled (session " +
                                  std::to_string(reply.session_id) + ", transaction " +
                                  std::to_string(reply.transaction_id) + ")");
        }
        return;
    }
    // The leg, then the identity. **The leg first, and it is not
    // redundant**: both legs of one transaction carry the same session and
    // transaction id, so a prepare answer that arrives after the prepare
    // phase timed out and the decide phase opened on this id would pass
    // every identity test and be delivered as a decide acknowledgement.
    if (it->second.phase != phase || reply.session_id != it->second.session_id ||
        reply.transaction_id != it->second.transaction_id) {
        ++identity_mismatches_;
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("2pc", "a participant's answer from core " +
                                   std::to_string(header.src_core) + " names session " +
                                   std::to_string(reply.session_id) + " transaction " +
                                   std::to_string(reply.transaction_id) +
                                   ", but its waiter holds session " +
                                   std::to_string(it->second.session_id) + " transaction " +
                                   std::to_string(it->second.transaction_id) + "; dropped");
        }
        return;
    }

    TxnParticipantOutcome* participant = nullptr;
    for (TxnParticipantOutcome& p : it->second.participants) {
        if (p.core == header.src_core) {
            participant = &p;
            break;
        }
    }
    if (participant == nullptr) {
        // A core that is not in this phase answered for it. Strictly worse
        // than a late reply and counted with the mismatches for that
        // reason: it is the shape of one transaction's answer reaching
        // another's waiter.
        ++identity_mismatches_;
        return;
    }
    if (participant->replied) {
        // A second answer from one participant. Absorbed rather than
        // re-counted: `outstanding` has already been decremented, and
        // decrementing it twice would settle a phase a participant is still
        // owed.
        ++late_replies_;
        return;
    }

    auto message = TxnParticipantReplyMessageOf(reply);
    if (!message.ok()) {
        // Bytes this core did not compute, and a payload whose length is
        // wrong is untrustworthy whole - `status_code` included. So the
        // answer is not read as a success with an empty message: it is the
        // refusal the length makes it, which on prepare aborts and on
        // decide is counted as a participant that did not confirm.
        participant->replied = true;
        participant->status = message.status();
    } else {
        participant->replied = true;
        participant->status = Status::FromWire(reply.status_code, std::string(message.value()));
    }
    --it->second.outstanding;
    if (!participant->status.ok()) {
        if (phase == TxnPhase::kPrepare) {
            ++prepare_refusals_;
        } else {
            ++decide_refusals_;
        }
    }
}

bool Txn2pcClient::Settled(std::uint64_t request_id) const {
    auto it = waiting_.find(request_id);
    if (it == waiting_.end()) return true;
    if (it->second.outstanding == 0) return true;
    return clock_.Now() >= it->second.deadline_ns;
}

const TxnPhaseOutcome* Txn2pcClient::Find(std::uint64_t request_id) const {
    auto it = waiting_.find(request_id);
    return it == waiting_.end() ? nullptr : &it->second;
}

void Txn2pcClient::Close(std::uint64_t request_id) {
    auto it = waiting_.find(request_id);
    if (it == waiting_.end()) return;
    // Counted at the close rather than at the settle, because that is where
    // the phase's whole story is known: a participant still outstanding
    // here never answered, whatever the caller decided to do about it.
    if (it->second.outstanding != 0) ++phase_timeouts_;
    // **A decide phase every participant acknowledged needs no record**
    // (R6-5): every one of them applied the decision and released, so
    // nobody is left to ask, and keeping it would hold a map node for the
    // retention over the healthy path - which is every cross-owner
    // transaction. A phase that timed out or was refused keeps its record,
    // because the participant that did not acknowledge is precisely the one
    // that will ask.
    if (it->second.phase == TxnPhase::kDecide && it->second.AllPrepared()) {
        decisions_.erase(DecisionKey{it->second.session_id, it->second.transaction_id});
    }
    waiting_.erase(it);
}

// ---- R6-5: answering an in-doubt participant --------------------------------

namespace {

// When a record's retention starts: from its decision where it has one,
// from its opening where it does not - a record that never reached a
// decision is a coordinator that died between its phases, and the only time
// it has is the one it was opened at.
sched::MonoTimeNs DecisionAge(const auto& record) noexcept {
    return record.decided_at_ns != 0 ? record.decided_at_ns : record.opened_at_ns;
}

}  // namespace

void Txn2pcClient::ExpireDecisions(sched::MonoTimeNs now) {
    for (auto it = decisions_.begin(); it != decisions_.end();) {
        if (now - DecisionAge(it->second) < kTxnDecisionRetentionNs) {
            ++it;
            continue;
        }
        ++decisions_forgotten_;
        it = decisions_.erase(it);
    }

    // The memory bound under the time bound, on the **same ordering**. A
    // record only reaches this while something is wrong - a decide phase
    // whose participant never acknowledged holds its record for the whole
    // retention - so the cap is a ceiling on a storm rather than on a
    // workload, and the record it drops is the one whose participant has
    // had longest to ask. An ask that then finds nothing is answered
    // `UnknownOutcome`, which is D5's own honest answer and resolves at the
    // next mount.
    while (decisions_.size() > kTxnDecisionMaxRecords) {
        auto oldest = decisions_.begin();
        for (auto it = std::next(decisions_.begin()); it != decisions_.end(); ++it) {
            if (DecisionAge(it->second) < DecisionAge(oldest->second)) oldest = it;
        }
        ++decisions_evicted_;
        if (log_ != nullptr && log_->enabled(LogLevel::kWarn)) {
            log_->Warn("2pc", "core " + std::to_string(core_id_) + " holds the maximum " +
                                  std::to_string(kTxnDecisionMaxRecords) +
                                  " cross-owner decision records; session " +
                                  std::to_string(oldest->first.first) + " transaction " +
                                  std::to_string(oldest->first.second) +
                                  " was dropped, and a participant asking about it will be "
                                  "told UNKNOWN_OUTCOME");
        }
        decisions_.erase(oldest);
    }
}

Txn2pcClient::DecisionRecord& Txn2pcClient::OpenDecisionRecord(std::uint64_t session_id,
                                                               std::uint64_t transaction_id,
                                                               sched::MonoTimeNs now) {
    // Expire **before** the insert, so the new record is never the one the
    // cap drops - and so a caller cannot open a record into a map that is
    // already over its bound.
    ExpireDecisions(now);
    DecisionRecord& record = decisions_[DecisionKey{session_id, transaction_id}];
    if (record.opened_at_ns == 0) record.opened_at_ns = now;
    return record;
}

void Txn2pcClient::OnResolveAsk(const sched::MessageHeader& header,
                                std::span<const std::byte> payload) {
    TxnResolveRequestPayload ask{};
    if (payload.size() != sizeof(ask)) {
        // The one ask that gets no answer: nothing in a mis-sized payload
        // names the transaction to answer about. The participant's ceiling
        // asks again, which is the backstop.
        ++resolve_refusals_;
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("2pc", "an in-doubt ask from core " + std::to_string(header.src_core) +
                                   " has " + std::to_string(payload.size()) + " bytes, not " +
                                   std::to_string(sizeof(ask)) + "; dropped");
        }
        return;
    }
    std::memcpy(&ask, payload.data(), sizeof(ask));

    const std::uint32_t participant = header.src_core;
    auto answer = [&](const Status& status, TxnDecision decision) {
        TxnResolveReplyPayload reply{};
        reply.session_id = ask.session_id;
        reply.transaction_id = ask.transaction_id;
        reply.status_code = static_cast<std::uint32_t>(status.code());
        reply.decision = static_cast<std::uint8_t>(decision);
        sched::SubmitSendPod(scheduler_, transport_, core_id_, participant,
                             /*session_core=*/core_id_, header.request_id,
                             sched::RingMessageKind::kTxnResolveReply, reply);
    };

    if (ask.retry == 0) {
        // **R6-0's contract, enforced on the one leg where it has a
        // sender.** An ask is a resend by construction - the decide it asks
        // about was the first attempt - so a clear bit means a sender that
        // does not know that, and answering it as though it did is how the
        // guarantee known-gaps states ("every retry path built from R6-3 on
        // has to set it") would be quietly lost. Refused, not guessed.
        ++resolve_refusals_;
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("2pc", "an in-doubt ask from core " + std::to_string(participant) +
                                   " for transaction " + std::to_string(ask.transaction_id) +
                                   " left R6-0's retry bit clear; an ask is a resend by "
                                   "construction and is refused rather than answered");
        }
        answer(Status::InvalidArgument({}), TxnDecision::kUnset);
        return;
    }

    ExpireDecisions(clock_.Now());
    auto it = decisions_.find(DecisionKey{ask.session_id, ask.transaction_id});
    if (it == decisions_.end()) {
        // **D5's terminal answer.** No record: this coordinator either
        // never had this transaction, or held its decision long enough for
        // the retention to drop it. Neither can be told from the other
        // here, and re-deciding is the one thing D5 forbids - so the
        // participant is told the outcome cannot be established from this
        // core, and the next mount resolves it against this core's *stream*
        // (R6-4), which is the durable record the retention does not touch.
        ++resolutions_unknown_;
        if (log_ != nullptr && log_->enabled(LogLevel::kWarn)) {
            log_->Warn("2pc", "core " + std::to_string(core_id_) +
                                  " holds no record of transaction " +
                                  std::to_string(ask.transaction_id) + " for session " +
                                  std::to_string(ask.session_id) + "; core " +
                                  std::to_string(participant) +
                                  " is told the outcome is unknown from here");
        }
        answer(Status::UnknownOutcome({}), TxnDecision::kUnset);
        return;
    }
    if (it->second.decision == TxnDecision::kUnset) {
        // Opened and not yet decided: the prepare phase is still open, or
        // this core is between the decision and the send. Retryable in the
        // engine's one spelling, and what the participant retries is the
        // ask - after another ceiling, holding its locks, which is exactly
        // D5's "may neither abort nor commit, holds its locks and waits".
        ++resolutions_undecided_;
        answer(Status::TxnConflict({}), TxnDecision::kUnset);
        return;
    }
    ++resolutions_answered_;
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("2pc", "core " + std::to_string(core_id_) + " answered core " +
                              std::to_string(participant) + "'s in-doubt ask for transaction " +
                              std::to_string(ask.transaction_id) + ": " +
                              (it->second.decision == TxnDecision::kCommit ? "COMMIT" : "ABORT"));
    }
    answer(Status::OK(), it->second.decision);
}

}  // namespace kds::server
