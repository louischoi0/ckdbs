#include "kds/server/shipped_statement_executor.hpp"

#include <utility>

#include "kds/sched/coro.hpp"

namespace kds::server {

void ShippedStatementExecutor::Execute(StatementShipServer::ShippedStatement statement,
                                       StatementShipServer::ReplyFn reply) {
    // **Before anything is allocated** (D5): the answers this core can give
    // without running the statement are given here, on a path that takes no
    // session, no transaction and no page.
    Expire();
    const DedupKey key{statement.requester, statement.session_id};
    if (auto it = answered_.find(key); it != answered_.end()) {
        if (it->second.sequence == statement.sequence) {
            ++deduped_;
            if (log_ != nullptr && log_->enabled(LogLevel::kWarn)) {
                log_->Warn("ship", "core " + std::to_string(statement.requester) +
                                       " asked again for session " +
                                       std::to_string(statement.session_id) + " sequence " +
                                       std::to_string(statement.sequence) +
                                       "; answered from the record, not run again");
            }
            reply(it->second.status, it->second.text);
            return;
        }
        if (statement.sequence < it->second.sequence) {
            // Superseded: this core answered a later statement for this
            // session, so whatever it answered for this one is gone. It may
            // have committed. Saying so is D4's whole point - a guess here
            // is a double insert there.
            ++unanswerable_;
            reply(Status::UnknownOutcome(
                      "statement shipping: core " + std::to_string(core_id_) +
                      " has answered sequence " + std::to_string(it->second.sequence) +
                      " for this session and no longer holds the outcome of sequence " +
                      std::to_string(statement.sequence) +
                      "; whether it ran cannot be established from here"),
                  {});
            return;
        }
    }

    // **A statement still running is not in the record yet**, and that is
    // the duplicate D4 is actually written for: an arrival core's deadline
    // fires *because* the owner is slow, so the retry it provokes meets the
    // original still executing here. The record alone answers only the easy
    // half of the window - it is written at `Finish` - and running the
    // statement again is precisely the double insert an engine-issued pk
    // makes of a blind retry. This core cannot say what the original will
    // answer either, so it says that: `UnknownOutcome`, never a second run
    // and never a guess.
    //
    // The same answer for any other sequence arriving while one is in
    // flight. A session runs one statement at a time - it is a connection
    // waiting on a reply - so a second is a request this core cannot
    // reconcile, and refusing it is what keeps `running_` at one entry per
    // session, which is what lets that map be keyed by the identity.
    if (running_.find(key) != running_.end()) {
        ++unanswerable_;
        reply(Status::UnknownOutcome(
                  "statement shipping: core " + std::to_string(core_id_) +
                  " is still running a statement for this session, so whether sequence " +
                  std::to_string(statement.sequence) +
                  " ran cannot be established from here"),
              {});
        return;
    }

    auto running = std::make_unique<Running>(std::move(statement.text),
                                             dispatcher_.default_isolation(), statement.role);
    // The hop limit (session.hpp): what arrived shipped does not ship on.
    running->session.mark_shipped();
    running->sequence = statement.sequence;
    running->reply = std::move(reply);

    Running* state = running.get();
    running_.emplace(key, std::move(running));

    // `kForeground`, because this is a client's statement and the only
    // thing that distinguishes it from a local one is which core its client
    // is on. It is also what puts the shipped population into the §8a
    // scheduler accounting, which is where SS-B4 goes looking for it.
    scheduler_.Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        dispatcher_.DispatchAsync(state->text, &state->session, &state->out),
        [this, key](const Status&) { Finish(key); }));
}

void ShippedStatementExecutor::Finish(const DedupKey& key) {
    auto it = running_.find(key);
    if (it == running_.end()) return;  // unreachable: one completion per statement
    std::unique_ptr<Running> state = std::move(it->second);
    running_.erase(it);
    ++executed_;

    // The rendered line back into a code and a text. An error line carries
    // its message in the status - `ErrorReply` on the arrival core puts it
    // back - so the text goes empty; a success's line **is** the answer.
    Status status = StatusFromErrorReply(state->out.response);
    std::string_view text;
    if (status.ok()) {
        text = state->out.response;
    }

    // **A shipped statement runs in autocommit and may not leave a
    // transaction open** (D1). This session dies with this statement and
    // nothing can reach it again, so a `BEGIN` that got through would leave
    // a transaction `active_` for the life of the process: it pins
    // `ReadHorizon()`, which stalls the undo purge, and answers `IsInFlight`
    // true forever, which the unfiltered catalog read consults. Rolled back
    // the way a dropped connection's is (tcp_server.cpp's close path,
    // docs/spec/txn.md section 10-8) and then refused - a caller must not be told
    // a transaction is open on a session it can never use again.
    // Unreachable from the dispatch fork, which ships only autocommit
    // shapes, and refused here for the same reason the stop flag below is.
    if (state->session.in_explicit_txn()) {
        (void)dispatcher_.Dispatch("ROLLBACK", &state->session);
        status = Status::Unsupported(
            "statement shipping: a shipped statement runs in autocommit and may not open a "
            "transaction; run it on the core the connection is on");
        text = {};
    }

    if (state->out.should_stop) {
        // A statement that ends a session cannot be shipped: the flag is
        // advisory to the *caller*, and this core is not the caller, so
        // honouring it would stop nothing while answering as though it had.
        // Unreachable from the dispatch fork, which ships only statements
        // that name a relation - and refused here rather than trusted to
        // stay unreachable, since the cost of being wrong is a client told
        // its session ended when it did not.
        status = Status::Unsupported(
            "statement shipping: a statement that ends the session may not be shipped; "
            "run it on the core the connection is on");
        text = {};
    }

    Remember(key, state->sequence, status, text);
    state->reply(status, text);
}

void ShippedStatementExecutor::Remember(const DedupKey& key, std::uint64_t sequence,
                                        const Status& status, std::string_view text) {
    // **One list node per record, never one per statement.** The order list
    // carries each key exactly once and moves it to the back when that key
    // is recorded again, so both bounds below bound the same number - a
    // session that ships a thousand statements holds one entry, not a
    // thousand. Recording order is stamp order, which is what lets `Expire`
    // stop at the first young entry.
    auto [it, inserted] = answered_.try_emplace(key);
    Answered& record = it->second;
    if (inserted) {
        record.order = answered_order_.insert(answered_order_.end(), key);
    } else {
        answered_order_.splice(answered_order_.end(), answered_order_, record.order);
    }
    record.sequence = sequence;
    record.status = status;
    record.text.assign(text);
    record.at_ns = clock_.Now();

    // The memory bound, under the time bound. Evicting here rather than at
    // the next request keeps the map's size a function of what it holds and
    // not of when it is next asked.
    while (answered_.size() > kShippedDedupMaxRecords) {
        const DedupKey oldest = answered_order_.front();
        ++early_evictions_;
        if (log_ != nullptr && log_->enabled(LogLevel::kWarn)) {
            log_->Warn("ship", "the shipped-statement dedup record is full (" +
                                   std::to_string(kShippedDedupMaxRecords) +
                                   "); core " + std::to_string(oldest.first) + "'s session " +
                                   std::to_string(oldest.second) +
                                   " was dropped before its retention expired, so a duplicate "
                                   "of it would run again");
        }
        answered_order_.pop_front();
        answered_.erase(oldest);
    }
}

void ShippedStatementExecutor::Expire() {
    const sched::MonoTimeNs now = clock_.Now();
    while (!answered_order_.empty()) {
        auto it = answered_.find(answered_order_.front());
        if (now - it->second.at_ns < kShippedDedupRetentionNs) return;  // later ones are younger
        answered_order_.pop_front();
        answered_.erase(it);
    }
}

}  // namespace kds::server
