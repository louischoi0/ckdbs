#include "kds/server/statement_ship_service.hpp"

#include <cstring>
#include <utility>

#include "kds/sched/send_retry.hpp"
// `Utf8PrefixLen` lived here until R6-3 gave it its second caller (the
// participant reply's message), which is the condition R6-1's header set
// for the hoist.
#include "kds/server/utf8_prefix.hpp"

namespace kds::server {
namespace {

// The one refusal that means "the statement ran and its answer cannot be
// carried" - written once because it is produced twice, from the encode and
// from the caller that pre-empts the encode, and two copies of a message
// that must mean exactly one thing is how they stop meaning it.
Status OverLongReply(std::size_t bytes) {
    return Status::UnknownOutcome(
        "statement shipping: the statement executed on its owner but its reply is " +
        std::to_string(bytes) + " bytes, past the " +
        std::to_string(kShippedStatementReplyTextMax) +
        " a reply carries; the statement's effect stands and its answer is lost");
}

}  // namespace

StatusOr<ShippedStatementRequestPayload> ShippedStatementRequestOf(std::uint64_t session_id,
                                                                   std::uint64_t sequence,
                                                                   std::uint64_t target_oid,
                                                                   Role role,
                                                                   std::string_view text,
                                                                   bool retry, bool in_txn,
                                                                   std::optional<txn::IsolationLevel> isolation,
                                                                   bool join, bool typed_answer,
                                                                   PipelineTag answer_tag) {
    if (text.empty()) {
        return Status::InvalidArgument("statement shipping: an empty statement is not a statement");
    }
    // Refuses, never truncates: a shortened statement is a different
    // statement, and the caller cannot be expected to know the ring's
    // bound - so the refusal names it.
    if (text.size() > kShippedStatementTextMax) {
        return Status::Unsupported(
            "statement shipping: the statement is " + std::to_string(text.size()) +
            " bytes and a shipped statement carries at most " +
            std::to_string(kShippedStatementTextMax) +
            "; run it on the core that owns the relation, or raise the ring payload "
            "(docs/spec/crosscore.md §9's sizing decision)");
    }
    ShippedStatementRequestPayload out{};
    out.session_id = session_id;
    out.sequence = sequence;
    out.target_oid = target_oid;
    out.role = static_cast<std::uint8_t>(role);
    out.retry = retry ? 1 : 0;
    out.in_txn = in_txn ? 1 : 0;
    // 0 stays 0 where nothing was stated, which is every autocommit
    // statement: the level is a promise about a transaction that spans
    // statements, and an autocommit one is not that.
    out.isolation = isolation.has_value() ? static_cast<std::uint8_t>(*isolation) : 0;
    // RR0: "join, do not open". Meaningful only with `in_txn`, and a
    // caller that sets it without one is asking for nothing - the owner
    // reads it under `in_txn` alone.
    out.join = join ? 1 : 0;
    // XG1. `form` and the tag are one fact: `kShippedAnswerText` leaves the
    // tag zeroed and unread, which is every text-arm statement and every
    // write, and is byte-identical on the wire to what every pre-XG1 sender
    // produced in those bytes.
    out.form = typed_answer ? kShippedAnswerTyped : kShippedAnswerText;
    out.answer_tag = typed_answer ? answer_tag : PipelineTag{};
    out.text_len = static_cast<std::uint16_t>(text.size());
    std::memcpy(out.text, text.data(), text.size());
    return out;
}

StatusOr<ShippedStatementReplyPayload> ShippedStatementReplyOf(std::uint64_t session_id,
                                                               std::uint64_t sequence,
                                                               const Status& status,
                                                               std::string_view text,
                                                               std::uint64_t read_watermark) {
    ShippedStatementReplyPayload out{};
    out.session_id = session_id;
    out.sequence = sequence;
    out.status_code = static_cast<std::uint32_t>(status.code());
    out.read_watermark = read_watermark;

    // **The cap is asymmetric, and deliberately so.**
    //
    // A refusal's message is *diagnostic*: what a client acts on is the
    // code, which crosses whole, so an over-long message is shortened and
    // the owner's log keeps all of it - `IndexBuildReplyPayload`'s rule,
    // and the reason it is not a lie is that nothing downstream parses it.
    if (!status.ok()) {
        const std::string& message = status.message();
        const std::size_t len = Utf8PrefixLen(message, kShippedStatementReplyTextMax);
        out.text_len = static_cast<std::uint16_t>(len);
        if (len > 0) std::memcpy(out.text, message.data(), len);
        return out;
    }

    // A success's text **is** the answer, so the same cap refuses. Handing
    // back a shortened answer as though it were the answer is the one
    // thing this must never do.
    //
    // And the statement has already run by the time this is reached, so
    // what is being reported is a delivered-but-unreportable outcome -
    // the same class as a lost reply, wearing the same code, because a
    // client that retried it would run the statement twice.
    if (text.size() > kShippedStatementReplyTextMax) return OverLongReply(text.size());
    out.text_len = static_cast<std::uint16_t>(text.size());
    if (!text.empty()) std::memcpy(out.text, text.data(), text.size());
    return out;
}

StatusOr<std::string_view> ShippedStatementTextOf(const ShippedStatementRequestPayload& request) {
    // Bounded against the array rather than trusted: these are bytes this
    // core did not compute, and `text_len` is the only thing standing
    // between a forged length and a read past the payload.
    if (request.text_len == 0 || request.text_len > kShippedStatementTextMax) {
        return Status::InvalidArgument("statement shipping: request names a statement of " +
                                       std::to_string(request.text_len) +
                                       " bytes, which is not a length this payload can hold");
    }
    return std::string_view(request.text, request.text_len);
}

StatusOr<std::optional<txn::IsolationLevel>> ShippedStatementIsolationOf(
    const ShippedStatementRequestPayload& request) {
    // 0 is "not stated", which is legal and is what every autocommit
    // request carries - the zero-collision rule, so a zeroed buffer decodes
    // as an absence rather than as a level.
    if (request.isolation == 0) return std::optional<txn::IsolationLevel>{};
    switch (static_cast<txn::IsolationLevel>(request.isolation)) {
        case txn::IsolationLevel::kReadCommitted:
            return std::optional{txn::IsolationLevel::kReadCommitted};
        case txn::IsolationLevel::kRepeatableRead:
            return std::optional{txn::IsolationLevel::kRepeatableRead};
    }
    // `ShippedStatementRoleOf`'s rule, for the same reason: a byte outside
    // the enum means the two ends disagree about what a level is, and a
    // transaction opened at a guessed level is one whose visibility promise
    // nobody made.
    return Status::InvalidArgument("statement shipping: request names isolation level " +
                                   std::to_string(request.isolation) +
                                   ", which is not a level this build knows");
}

StatusOr<Role> ShippedStatementRoleOf(const ShippedStatementRequestPayload& request) {
    switch (static_cast<Role>(request.role)) {
        case Role::kReadOnly: return Role::kReadOnly;
        case Role::kReadWrite: return Role::kReadWrite;
        case Role::kAdmin: return Role::kAdmin;
    }
    // Not `kReadOnly` as a lenient default: a byte outside the enum means
    // the two ends disagree about what a rank is, and a statement run under
    // a guessed rank is a statement run under no authorization at all.
    return Status::InvalidArgument("statement shipping: request names role " +
                                   std::to_string(request.role) +
                                   ", which is not a role this build knows");
}

StatusOr<bool> ShippedAnswerTypedOf(const ShippedStatementRequestPayload& request) {
    if (request.form == kShippedAnswerText) return false;
    if (request.form == kShippedAnswerTyped) return true;
    // The `role` byte's rule, applied to the one field whose misreading is
    // a *shape* rather than a permission: an owner that guessed here would
    // answer a request for typed rows with a rendered line, or the
    // reverse, and the client has no way to tell which it got. Refused by
    // name, before anything runs.
    return Status::Unsupported("statement shipping: request asks for answer form " +
                               std::to_string(request.form) +
                               ", which is not a form this build serves");
}

// ---- The owner's half ------------------------------------------------------

void StatementShipServer::OnRequest(const sched::MessageHeader& header,
                                    std::span<const std::byte> payload) {
    ++requests_;
    ShippedStatementRequestPayload request{};
    if (payload.size() != sizeof(request)) {
        // No reply, and it is the one case that gets none: nothing here
        // names the waiter parked on the other side, so there is no
        // address to answer. The arrival core's deadline is the backstop,
        // and it answers `UnknownOutcome` - which is correct, because a
        // request this core could not read may still have been a statement
        // some other core executed.
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("ship", "shipped statement from core " +
                                    std::to_string(header.src_core) + " has " +
                                    std::to_string(payload.size()) + " bytes, not " +
                                    std::to_string(sizeof(request)) + "; dropped");
        }
        return;
    }
    std::memcpy(&request, payload.data(), sizeof(request));

    // Everything the answer needs, copied out before the payload can die:
    // the executor may park, and `request` lives only as long as this call.
    const std::uint32_t requester = header.src_core;
    const std::uint64_t request_id = header.request_id;
    const std::uint64_t session_id = request.session_id;
    const std::uint64_t sequence = request.sequence;

    auto text = ShippedStatementTextOf(request);
    if (!text.ok()) {
        Reply(requester, request_id, session_id, sequence, text.status(), {}, 0);
        return;
    }
    // Before the executor, because a rank this core cannot read is a
    // refusal and not an execution - and a refusal must cost nothing.
    auto role = ShippedStatementRoleOf(request);
    if (!role.ok()) {
        Reply(requester, request_id, session_id, sequence, role.status(), {}, 0);
        return;
    }
    // R6-8's level, on the same terms and for the same reason: a byte this
    // core cannot read is a refusal before anything runs, never a level
    // guessed for a transaction a client was promised something about.
    auto isolation = ShippedStatementIsolationOf(request);
    if (!isolation.ok()) {
        Reply(requester, request_id, session_id, sequence, isolation.status(), {}, 0);
        return;
    }
    // XG1's form, on the same terms as the two above and for a reason of
    // its own: this is the field whose misreading is a *shape*. An owner
    // that guessed would answer a request for typed rows with a rendered
    // line and the client could not tell. Refused before anything runs -
    // which is also how an owner too old to serve the form answers, since
    // the byte it does not know is exactly the byte it refuses.
    auto typed_answer = ShippedAnswerTypedOf(request);
    if (!typed_answer.ok()) {
        Reply(requester, request_id, session_id, sequence, typed_answer.status(), {}, 0);
        return;
    }
    if (!execute_) {
        // SS1 ships the wire and nothing else. Stated as a refusal rather
        // than left to a null call, because a handler that crashes and a
        // handler that is not built yet must not look alike.
        Reply(requester, request_id, session_id, sequence,
              Status::NotImplemented("statement shipping: this core has no executor installed; "
                                  "the wire is built and the owner-side execution is not (SS3)"),
              {}, 0);
        return;
    }

    // The seam may answer now or many reactor turns from now - joining the
    // owner's group commit means parking (the header's D3 argument), so
    // the reply closure captures by value and nothing it needs outlives
    // this frame.
    ShippedStatement statement;
    statement.requester = requester;
    statement.session_id = session_id;
    statement.sequence = sequence;
    statement.target_oid = request.target_oid;
    statement.role = role.value();
    statement.retry = request.retry != 0;
    statement.in_txn = request.in_txn != 0;
    statement.isolation = isolation.value();
    statement.typed_answer = typed_answer.value();
    statement.answer_tag = request.answer_tag;
    statement.join = request.join != 0;
    statement.text.assign(text.value());
    execute_(std::move(statement),
             [this, requester, request_id, session_id, sequence](
                 const Status& status, std::string_view reply_text, std::uint64_t watermark) {
                 Reply(requester, request_id, session_id, sequence, status, reply_text,
                       watermark);
             });
}

void StatementShipServer::Reply(std::uint32_t requester, std::uint64_t request_id,
                                std::uint64_t session_id, std::uint64_t sequence,
                                const Status& status, std::string_view text,
                                std::uint64_t read_watermark) {
    // The pair is decided **before** it is encoded, so there is one encode
    // and no arm that can fail. The earlier shape re-encoded on failure and
    // its own failure arm returned without sending - dropping the one thing
    // this class promises always to send.
    //
    // An over-long *answer* is the only case the encode would refuse, and
    // it is a refusal in its own right: the statement ran, and its answer
    // cannot be carried. `ShippedStatementReplyOf` states it; taking it
    // here turns the second encode into a refusal encode, which cannot
    // refuse (a refusal's message is truncated to fit, never rejected).
    Status answer = status;
    std::string_view answer_text = text;
    if (answer.ok() && text.size() > kShippedStatementReplyTextMax) {
        answer = OverLongReply(text.size());
        answer_text = {};
    }

    auto reply = ShippedStatementReplyOf(session_id, sequence, answer, answer_text,
                                         // **Dropped on the refusal arm**, including the
                                         // over-long-answer one above: a watermark is a
                                         // promise about a view the client's transaction is
                                         // reading through, and a statement whose answer this
                                         // core could not carry has no such promise to make.
                                         answer.ok() ? read_watermark : 0);
    if (!reply.ok()) return;  // unreachable: neither arm above can refuse
    ++replies_;
    //  is the *requester's*, both ways: a shipped statement's
    // session lives on the arrival core, not on core 0, so a reader of a
    // captured header can see whose statement is parked on this.
    sched::SubmitSendPod(scheduler_, transport_, core_id_, requester, /*session_core=*/requester,
                         request_id, sched::RingMessageKind::kShippedStatementReply,
                         reply.value());
}

// ---- The arrival core's half -----------------------------------------------

Status StatementShipClient::RegisterReplyReceiver() {
    return scheduler_.RegisterMessageHandler(
        sched::RingMessageKind::kShippedStatementReply,
        [this](const sched::MessageHeader& header, std::span<const std::byte> payload) {
            ShippedStatementReplyPayload reply{};
            if (payload.size() != sizeof(reply)) {
                if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                    log_->Error("ship", "shipped statement reply from core " +
                                            std::to_string(header.src_core) + " has " +
                                            std::to_string(payload.size()) + " bytes, not " +
                                            std::to_string(sizeof(reply)) + "; dropped");
                }
                return;
            }
            std::memcpy(&reply, payload.data(), sizeof(reply));

            auto it = waiting_.find(header.request_id);
            if (it == waiting_.end()) {
                // The deadline fired first and the statement was already
                // answered `UnknownOutcome`. Nothing can be undone here -
                // unlike an index build's tree, a committed statement is
                // committed.
                //
                // **Split, because the two halves mean opposite things.** A
                // late *success* is a statement that ran and whose client
                // was told nobody knows: that is the population D4's dedup
                // record exists for, and counting a late refusal beside it
                // would overstate it. A late refusal owes nothing - nothing
                // ran - and is only evidence the deadline is tight.
                const bool executed =
                    reply.status_code == static_cast<std::uint32_t>(StatusCode::kOk);
                if (executed) {
                    ++late_executed_replies_;
                } else {
                    ++late_refused_replies_;
                }
                if (log_ != nullptr && log_->enabled(LogLevel::kWarn)) {
                    log_->Warn("ship", std::string("a shipped statement's ") +
                                           (executed ? "result" : "refusal") +
                                           " arrived after its deadline (session " +
                                           std::to_string(reply.session_id) + ", sequence " +
                                           std::to_string(reply.sequence) +
                                           "); the client was told the outcome is unknown");
                }
                return;
            }
            if (reply.session_id != it->second.session_id ||
                reply.sequence != it->second.sequence) {
                // The ring matched a waiter the identity does not. Refused
                // rather than delivered: answering one statement with
                // another's result is the one failure this protocol must
                // not have, and the waiter is left to its deadline, which
                // says `UnknownOutcome` - the truthful answer, since this
                // core now knows nothing about its statement.
                //
                // Counted as well as logged: it is a strictly worse anomaly
                // than a late reply, and a number that stays 0 is the only
                // cheap evidence that it does.
                ++identity_mismatches_;
                if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                    log_->Error("ship", "a shipped statement's reply names session " +
                                            std::to_string(reply.session_id) + " sequence " +
                                            std::to_string(reply.sequence) +
                                            ", but its waiter holds session " +
                                            std::to_string(it->second.session_id) +
                                            " sequence " +
                                            std::to_string(it->second.sequence) + "; dropped");
                }
                return;
            }

            // **Counted here**, past the two arms that answer no waiter
            // (a late reply, a mismatched identity) and before the two that
            // do - the length refusal below included, because a forged
            // length is a reply that arrived and refused, not a statement
            // whose answer never came. `shipped() - replies()` is then
            // exactly "still parked, or lost".
            ++replies_;
            const sched::MonoTimeNs waited = clock_.Now() - it->second.sent_ns;
            if (waited > wait_ns_max_) wait_ns_max_ = waited;

            if (reply.text_len > kShippedStatementReplyTextMax) {
                ++refusals_;
                // Bytes this core did not compute, bounded here as the
                // request side bounds them - but **not** by reading an
                // empty text instead. On the success arm that would hand
                // the client a blank answer wearing an OK status, which is
                // the one thing the asymmetric cap exists to prevent; and
                // nothing in a payload whose length is wrong is
                // trustworthy, `status_code` included. So the reading is
                // the deadline's, arrived at early: the statement may have
                // run and its answer cannot be read.
                if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                    log_->Error("ship", "a shipped statement's reply names a text of " +
                                            std::to_string(reply.text_len) +
                                            " bytes, which is not a length this payload can "
                                            "hold; the outcome is reported unknown");
                }
                it->second.text.clear();
                it->second.status = Status::UnknownOutcome(
                    "statement shipping: the owner's reply names a text of " +
                    std::to_string(reply.text_len) +
                    " bytes, which is not a length a reply can hold; the statement may have "
                    "run and its answer cannot be read");
                it->second.arrived = true;
                return;
            }
            it->second.text.assign(reply.text, reply.text_len);
            // RR0 / D3: taken before the status is rebuilt, because the
            // watermark is the owner's and this side neither interprets it
            // nor compares it with anything of its own - the coordinator's
            // session does that, once, against what it already held for
            // this core (`CommandDispatcher::FinishShippedStatement`).
            it->second.read_watermark = reply.read_watermark;
            it->second.status = Status::FromWire(reply.status_code, it->second.text);
            if (!it->second.status.ok()) ++refusals_;
            // A success carries the reply line in `text`; a refusal carries
            // its message, which FromWire has just taken - so `text` is
            // meaningful only on the success arm and is cleared on the
            // other, rather than left as a copy of the message.
            if (!it->second.status.ok()) it->second.text.clear();
            it->second.arrived = true;
        });
}

Status StatementShipClient::Ship(std::uint32_t owner_core, std::uint64_t request_id,
                                 std::uint64_t session_id, std::uint64_t sequence,
                                 std::uint64_t target_oid, Role role, std::string_view text,
                                 bool retry, bool in_txn,
                                 std::optional<txn::IsolationLevel> isolation, bool join,
                                 bool typed_answer, PipelineTag answer_tag) {
    // **One live waiter per request id.** Reusing an id that still has a
    // statement parked on it would replace that statement's waiter with
    // this one's, and the identity check on the reply path cannot catch it -
    // the identity it checks against would by then be *this* statement's,
    // so the parked one would be woken with another statement's answer.
    // That is the failure this protocol must not have, and it is refused
    // here rather than made unreachable by convention.
    if (waiting_.find(request_id) != waiting_.end()) {
        return Status::InvalidArgument(
            "statement shipping: request id " + std::to_string(request_id) +
            " already has a statement parked on it; ids are allocated per core and per "
            "statement");
    }
    // The owner core, bounded before the send rather than left to it: an
    // out-of-range core is the one send failure `MakeSendRetryTask` does
    // not retry, and it reports through an `on_done` this protocol does not
    // pass - so the message would be dropped, the statement would park out
    // the whole deadline, and it would be answered `UnknownOutcome` for a
    // request that provably never left this core.
    if (owner_core >= transport_.core_count()) {
        return Status::InvalidArgument(
            "statement shipping: core " + std::to_string(owner_core) +
            " is not a core of this instance, which has " +
            std::to_string(transport_.core_count()));
    }
    auto request =
        ShippedStatementRequestOf(session_id, sequence, target_oid, role, text, retry, in_txn,
                                  isolation, join, typed_answer, answer_tag);
    // A statement the wire refuses opens no waiter: nothing was sent, so
    // nothing will answer, and a waiter would only cost the statement a
    // deadline before saying what is already known.
    if (!request.ok()) return request.status();

    // Fresh by the duplicate-id refusal above, so the defaults stand and
    // only the identity and the deadline are written.
    ShippedStatementOutcome& outcome = waiting_[request_id];
    outcome.session_id = session_id;
    outcome.sequence = sequence;
    outcome.sent_ns = clock_.Now();
    outcome.deadline_ns = outcome.sent_ns + kShippedStatementDeadlineNs;
    ++shipped_;

    sched::SubmitSendPod(scheduler_, transport_, core_id_, owner_core,
                         /*session_core=*/core_id_, request_id,
                         sched::RingMessageKind::kShippedStatementRequest, request.value());
    return Status::OK();
}

bool StatementShipClient::Settled(std::uint64_t request_id) const {
    auto it = waiting_.find(request_id);
    if (it == waiting_.end()) return true;
    if (it->second.arrived) return true;
    return clock_.Now() >= it->second.deadline_ns;
}

const ShippedStatementOutcome* StatementShipClient::Find(std::uint64_t request_id) const {
    auto it = waiting_.find(request_id);
    return it == waiting_.end() ? nullptr : &it->second;
}

void StatementShipClient::Close(std::uint64_t request_id) { waiting_.erase(request_id); }

}  // namespace kds::server
