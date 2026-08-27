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

    // Finding 1 / R6-0 (`instructions/v2.4.0/2pc.md` §2): nothing here holds
    // an outcome for this *sequence* - either the record has no entry for
    // this identity at all, or it holds a **lower** sequence, which is the
    // arm above falling through. On a first attempt that means "not seen,
    // execute". On a retry it does not - the record may have been evicted
    // early (`early_evictions()`) or may never have been written, and this
    // core cannot tell those apart from here. A lower recorded sequence is
    // not proof of non-execution either: an eviction can drop a key after a
    // high sequence finished, and a later statement then re-records the same
    // key at a lower one. Guessing either way risks a second row against an
    // engine-issued pk, so the honest answer is the one D4 already gives a
    // superseded sequence: `UnknownOutcome`, never a guess.
    if (statement.retry) {
        ++unanswerable_;
        reply(Status::UnknownOutcome(
                  "statement shipping: core " + std::to_string(core_id_) +
                  " holds no record of sequence " + std::to_string(statement.sequence) +
                  " for this session; this is a retry, so whether it ran cannot be "
                  "established from an absent record"),
              {});
        return;
    }

    // **R6-2: the fork between autocommit and a held transaction**, and the
    // only place the two differ. An enrolled statement runs on the session
    // its transaction lives on; an autocommit one gets a session of its own,
    // exactly as SS3 built it.
    std::unique_ptr<Running> running;
    if (statement.in_txn) {
        auto enrolled = EnrolFor(key, statement.role);
        if (!enrolled.ok()) {
            // No context was left half-open: `EnrolFor` refuses before it
            // records anything. A spent transaction-id lease and a full
            // enrolment table both arrive here as `TxnConflict`, which the
            // wire's `retryable` bit follows and the coordinator may act on.
            //
            // Counted apart from `unanswerable_`, which is a **duplicate**
            // count whose every member is a client told `UNKNOWN_OUTCOME`:
            // a refused enrolment is neither a duplicate nor in doubt, and
            // folding it in would make the one number SS-B4 reads mean two
            // unrelated things.
            ++enrolment_refusals_;
            reply(enrolled.status(), {});
            return;
        }
        running = std::make_unique<Running>(std::move(statement.text),
                                            enrolled.value()->session);
    } else {
        running = std::make_unique<Running>(std::move(statement.text),
                                            dispatcher_.default_isolation(), statement.role);
    }
    // The hop limit (session.hpp): what arrived shipped does not ship on.
    // Set on every path, and on the enrolled one it is already set from the
    // first statement - `mark_shipped` is idempotent.
    running->session->mark_shipped();
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
        dispatcher_.DispatchAsync(state->text, state->session, &state->out),
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
    if (state->enrolled()) {
        // **R6-2: the transaction is meant to still be open**, so the
        // refusal below does not apply and nothing is rolled back here - the
        // decide leg ends it (R6-3), or the idle ceiling does.
        //
        // What is checked instead is the opposite failure: an enrolled
        // statement that *ended* the transaction. Only a shipped `COMMIT` or
        // `ROLLBACK` could, and neither may be shipped - the coordinator
        // owns the decision (D4). If one gets through, the context is no
        // longer a transaction and must not be left standing as though it
        // were: drop it, so the next statement for this session opens a new
        // one rather than silently running outside any transaction.
        auto it_enrolled = enrolled_.find(key);
        // Stamped at the statement's **end**, not its start, so the ceiling
        // measures idleness: a statement that ran four minutes must not
        // leave its coordinator one minute of grace.
        if (it_enrolled != enrolled_.end()) it_enrolled->second->touched_at_ns = clock_.Now();
        if (it_enrolled != enrolled_.end() && !it_enrolled->second->session.in_explicit_txn()) {
            enrolled_.erase(it_enrolled);
            status = Status::Unsupported(
                "statement shipping: a statement inside a cross-owner transaction ended that "
                "transaction; the decision belongs to the coordinator, not to a shipped "
                "statement");
            text = {};
        }
    } else if (state->session->in_explicit_txn()) {
        (void)dispatcher_.Dispatch("ROLLBACK", state->session);
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

StatusOr<ShippedStatementExecutor::Enrolled*> ShippedStatementExecutor::EnrolFor(
    const DedupKey& key, Role role) {
    if (auto it = enrolled_.find(key); it != enrolled_.end()) {
        // **A context whose transaction is gone is not joinable.** `Finish`
        // drops one it finds closed, so reaching here with a closed session
        // would mean some other path ended it; refusing is the conservative
        // reading, and joining it would run the statement outside any
        // transaction while the coordinator believes otherwise.
        if (!it->second->session.in_explicit_txn()) {
            return Status::UnknownOutcome(
                "statement shipping: core " + std::to_string(core_id_) +
                " holds a cross-owner transaction context for this session whose transaction "
                "is no longer open; whether its statements applied cannot be established "
                "from here");
        }
        return it->second.get();
    }

    // **The participant's own capacity, refused before the table's.** Every
    // enrolment is one of `txn::kMaxTrackedLiveTxns`, which local clients
    // share - so without this a coordinator storm would refuse an unrelated
    // connection's `BEGIN` with `OutOfSpace` and nothing naming the cause.
    // `TxnConflict` because retrying once another cross-owner transaction
    // ends is the right response and that happens on its own.
    if (enrolled_.size() >= kShippedMaxEnrolled) {
        return Status::TxnConflict(
            "statement shipping: core " + std::to_string(core_id_) + " already holds " +
            std::to_string(enrolled_.size()) +
            " cross-owner transactions, its limit; retry once one of them is decided");
    }

    // First statement of the transaction on this core - D1's "a participant
    // is discovered as the transaction runs". The transaction is opened
    // through the **ordinary** `BEGIN`, which is the whole design: a
    // participant's transaction is a local transaction and nothing about it
    // is special (the `KwpLoadServer` argument, and SS3's for the statement
    // itself).
    auto context =
        std::make_unique<Enrolled>(dispatcher_.default_isolation(), role, clock_.Now());
    context->session.mark_shipped();
    const DispatchOutcome begun = dispatcher_.Dispatch("BEGIN", &context->session);
    if (!context->session.in_explicit_txn()) {
        // The refusal `BEGIN` rendered, recovered rather than reworded, so a
        // spent transaction-id lease reaches the coordinator as the
        // `TxnConflict` it is - retryable, and the grant that fixes it is
        // already on its way (`trx_id_lease.hpp`).
        Status why = StatusFromErrorReply(begun.response);
        if (why.ok()) {
            why = Status::UnknownOutcome(
                "statement shipping: core " + std::to_string(core_id_) +
                " could not open a transaction for this cross-owner statement");
        }
        // Nothing is recorded: a half-open context would be joined by the
        // next statement as though a transaction existed.
        return why;
    }

    ++enrolments_;
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("ship", "core " + std::to_string(core_id_) +
                                " opened a transaction for core " + std::to_string(key.first) +
                                "'s session " + std::to_string(key.second) +
                                " (cross-owner, R6-2)");
    }
    auto [it, inserted] = enrolled_.emplace(key, std::move(context));
    (void)inserted;
    return it->second.get();
}

void ShippedStatementExecutor::EndEnrolled(
    std::map<DedupKey, std::unique_ptr<Enrolled>>::iterator it, std::string_view why) {
    if (it == enrolled_.end()) return;
    if (it->second->session.in_explicit_txn()) {
        (void)dispatcher_.Dispatch("ROLLBACK", &it->second->session);
        // **Checked, because the next line makes a failure permanent.**
        // `HandleRollback` returns without finishing the session when the
        // assertion enforcer's abort fails, leaving the transaction
        // `active_` - and erasing the context below removes the last thing
        // that could reach it, so the leak would be invisible while
        // `enrolment_expiries()` counted it as cleaned up. Nothing here can
        // repair it; saying so is what a timer-driven caller owes.
        if (it->second->session.in_explicit_txn() && log_ != nullptr &&
            log_->enabled(LogLevel::kError)) {
            log_->Error("ship", "core " + std::to_string(core_id_) +
                                    " could not end an enrolled transaction; it stays active "
                                    "and pins ReadHorizon() for the life of this process");
        }
    }
    if (log_ != nullptr && log_->enabled(LogLevel::kWarn)) {
        log_->Warn("ship", "core " + std::to_string(core_id_) + " ended core " +
                               std::to_string(it->first.first) + "'s session " +
                               std::to_string(it->first.second) +
                               " cross-owner transaction: " + std::string(why));
    }
    enrolled_.erase(it);
}

void ShippedStatementExecutor::ExpireEnrolled() {
    // Before the clock read: on every live path today this map is empty,
    // and this runs on every core's drain tick.
    if (enrolled_.empty()) return;
    const sched::MonoTimeNs now = clock_.Now();
    for (auto it = enrolled_.begin(); it != enrolled_.end();) {
        // A statement still running on this context keeps it, whatever the
        // clock says: rolling back under a live statement would pull the
        // session out from a coroutine that holds a pointer into it - the
        // deferral `TcpServer::CloseClient` makes for the same reason.
        const bool busy = running_.find(it->first) != running_.end();
        if (busy || now - it->second->touched_at_ns < kShippedTxnIdleCeilingNs) {
            ++it;
            continue;
        }
        auto doomed = it++;
        ++enrolment_expiries_;
        EndEnrolled(doomed, "no decide arrived within the idle ceiling; rolled back");
    }
}

void ShippedStatementExecutor::RollbackAllEnrolled() {
    while (!enrolled_.empty()) {
        EndEnrolled(enrolled_.begin(),
                    "this core is stopping and can no longer answer for it");
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
