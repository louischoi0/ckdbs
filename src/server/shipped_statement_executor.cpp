#include "kds/server/shipped_statement_executor.hpp"

#include <functional>
#include <utility>

#include "kds/sched/coro.hpp"
#include "kds/wal/log_txn_prepare.hpp"

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
        // **A prepared transaction takes no more statements** (R6-3, D4).
        // Prepare is a promise that everything this transaction wrote is
        // durable; a statement admitted after it would write rows the
        // PREPARE record does not cover, so a commit decided on that
        // promise would make a transaction durable in part. Refused
        // retryably - the coordinator is between its own two phases, and a
        // client that gets this can run the statement in a new
        // transaction.
        //
        // Unreachable while a coordinator waits for its own COMMIT before
        // sending another statement, which is what a client does; refused
        // here rather than trusted to stay unreachable, because the cost of
        // being wrong is a half-durable transaction.
        if (it->second->prepared || it->second->phase_running) {
            return Status::TxnConflict(
                "statement shipping: core " + std::to_string(core_id_) +
                " has prepared this session's cross-owner transaction and can take no further "
                "statement in it; the decision is the coordinator's to make first");
        }
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

// ---- R6-3: the participant's two phases -------------------------------------

void ShippedStatementExecutor::Prepare(const Txn2pcServer::PrepareAsk& ask,
                                       Txn2pcServer::ReplyFn reply) {
    const DedupKey key{ask.coordinator, ask.session_id};
    auto it = enrolled_.find(key);
    if (it == enrolled_.end()) {
        // Nothing here to prepare. The reachable causes are all the same
        // outcome: the idle ceiling rolled the context back, the enrolment
        // was refused, or this core restarted. None of them committed
        // anything, so the coordinator's abort is the correct end and the
        // refusal is **retryable** - the transaction can be run again from
        // the top with nothing to undo.
        ++prepare_refusals_;
        reply(Status::TxnConflict(
            "cross-owner transaction: core " + std::to_string(core_id_) +
            " holds no transaction for core " + std::to_string(ask.coordinator) +
            "'s session " + std::to_string(ask.session_id) +
            "; it was rolled back or never opened, so this transaction cannot commit"));
        return;
    }
    Enrolled& context = *it->second;

    if (context.prepared) {
        // A resend of a prepare this core already answered. Idempotent by
        // construction - the record is durable and the promise stands - so
        // it is re-answered rather than re-made, and `prepared_` is not
        // counted twice. The identity is checked first: a *different*
        // transaction id on the same context is not a resend, it is two
        // transactions confused for one.
        if (context.coordinator_txn_id != ask.transaction_id) {
            ++prepare_refusals_;
            reply(Status::InvalidArgument(
                "cross-owner transaction: core " + std::to_string(core_id_) +
                " has prepared transaction " + std::to_string(context.coordinator_txn_id) +
                " for this session and is asked to prepare " +
                std::to_string(ask.transaction_id) + "; one session, one transaction"));
            return;
        }
        reply(Status::OK());
        return;
    }
    if (context.phase_running || running_.find(key) != running_.end()) {
        // A statement or a phase is still running on this context. A
        // client is parked on one statement at a time, so a prepare
        // arriving mid-statement is a coordinator that did not wait for its
        // own answer - refused rather than raced, and retryable because
        // nothing here has moved.
        ++prepare_refusals_;
        reply(Status::TxnConflict("cross-owner transaction: core " + std::to_string(core_id_) +
                                  " is still running this session's previous work; prepare "
                                  "cannot be answered until it finishes"));
        return;
    }
    if (!context.session.in_explicit_txn()) {
        // The context outlived its transaction. `Finish` drops one it finds
        // closed, so this is defence rather than an expected arm - and the
        // context is dropped here too, so the next statement opens a new
        // transaction instead of joining a dead one.
        ++prepare_refusals_;
        enrolled_.erase(it);
        reply(Status::TxnConflict("cross-owner transaction: core " + std::to_string(core_id_) +
                                  " holds a context for this session whose transaction is no "
                                  "longer open; this transaction cannot commit"));
        return;
    }
    if (context.session.failed()) {
        // **R6-2's named gap, answered.** A poisoned session is still
        // "in a transaction" (`state_ != kIdle`), so every later statement
        // met the failed-txn gate and returned "current transaction is
        // aborted" - which told the coordinator nothing. Prepare is where
        // it becomes legible: this participant is doomed, so it refuses,
        // and the coordinator's answer is ABORT. Rolled back here rather
        // than left standing, since nothing can commit it.
        ++prepare_refusals_;
        EndEnrolled(it, "its transaction was aborted, so it could not prepare");
        reply(Status::TxnConflict("cross-owner transaction: core " + std::to_string(core_id_) +
                                  " has an aborted transaction for this session; it cannot "
                                  "prepare and the transaction must roll back"));
        return;
    }

    // D2's pairing, completed: this core's own transaction id is what the
    // record's envelope carries, and the coordinator's is what its payload
    // names. Recorded on the context as well, because a decide is checked
    // against it.
    context.coordinator_txn_id = ask.transaction_id;
    const std::uint64_t participant_txn_id = context.session.transaction()->id();
    auto lsn = wal::LogTxnPrepare(wal_, participant_txn_id, ask.coordinator, ask.session_id,
                                  ask.transaction_id);
    if (!lsn.ok()) {
        // Nothing is prepared and nothing is claimed. The context stays
        // enrolled and abortable, which is what makes this refusal safe.
        ++prepare_refusals_;
        reply(lsn.status());
        return;
    }

    if (wal_ == nullptr) {
        // The unlogged fixture. There is no record and therefore no sync to
        // wait for; the promise this reply makes is as durable as anything
        // else on an unlogged instance, which is to say not at all - and
        // that is the property of the instance, not of this row. Stated
        // rather than silently taken, because "prepared" is a durability
        // claim everywhere else in this file.
        // **The LSN is 0 on this arm and the flag is still raised** (R6-5):
        // there is no record for a checkpoint's redo start to be held
        // below, because there is no record - but a writer of this
        // transaction's rows must block on it here exactly as it would on a
        // logged core, since what it is waiting for is a *decision* and
        // that arrives over the ring either way. `MarkPrepared(0)` is the
        // one call that says both, which is why the flag is not derived
        // from the LSN.
        context.prepared = true;
        context.asked_at_ns = clock_.Now();
        ++in_doubt_;
        ++prepared_;
        if (txn::Transaction* txn = context.session.transaction(); txn != nullptr) {
            txn->MarkPrepared(wal::kNoLsn);
        }
        reply(Status::OK());
        return;
    }
    // The drain has nothing to sync *for* until it is told (`RequestDurable`):
    // a prepare is not a commit, so without this the record waits out D3's
    // loss-window interval while a coordinator is parked on it.
    wal_->RequestDurable(lsn.value());
    context.phase_running = true;
    // `kForeground`: this is a client's transaction, and the only thing
    // that distinguishes it from a local one is which core its client is
    // on - `Execute`'s argument, one level up.
    scheduler_.Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        AwaitPrepared(key, lsn.value(), clock_.Now() + kTxnPhaseDeadlineNs, std::move(reply))));
}

sched::Coro ShippedStatementExecutor::AwaitPrepared(DedupKey key, wal::Lsn lsn,
                                                    sched::MonoTimeNs deadline_ns,
                                                    Txn2pcServer::ReplyFn reply) {
    // **Bounded, and the bound is not decoration.** A device that never
    // answers would otherwise park this coroutine for the life of the
    // process while the context sat un-expirable behind `phase_running` -
    // the silent failure HP3 predicts is unreachable. On expiry nothing is
    // claimed: the context stays unprepared and abortable, and the record
    // that may yet become durable is resolved the way any prepared-then-
    // aborted transaction is, by the TXN_ABORT that follows it.
    const std::function<bool()> durable = [this, lsn, deadline_ns] {
        return wal_->IsDurable(lsn) || clock_.Now() >= deadline_ns;
    };
    co_await sched::WaitUntil{&durable};

    auto it = enrolled_.find(key);
    if (it == enrolled_.end()) {
        // Re-found rather than held across the park, the way every other
        // parked path in this engine re-finds its state. Unreachable while
        // `phase_running` holds the sweep off, and answered rather than
        // asserted because the cost of being wrong is a coordinator parked
        // to its deadline.
        ++prepare_refusals_;
        reply(Status::TxnConflict("cross-owner transaction: core " + std::to_string(core_id_) +
                                  " lost this session's transaction while its prepare was "
                                  "reaching the device"));
        co_return Status::OK();
    }
    Enrolled& context = *it->second;
    context.phase_running = false;
    if (!wal_->IsDurable(lsn)) {
        ++prepare_refusals_;
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("2pc", "core " + std::to_string(core_id_) +
                                   " could not make a prepare record durable within the phase "
                                   "deadline; the transaction is refused rather than promised");
        }
        reply(Status::TxnConflict("cross-owner transaction: core " + std::to_string(core_id_) +
                                  " could not make its prepare durable within the phase "
                                  "deadline; nothing is prepared here"));
        ApplyHeldDecision(it);
        co_return Status::OK();
    }

    // **The promise is made here and nowhere earlier**: from this line the
    // context may not be aborted by this core (D4), which is what
    // `ExpireEnrolled` and `RollbackAllEnrolled` both read.
    context.prepared = true;
    // R6-5's ceiling runs from the promise: the first ask goes out one
    // ceiling from here, not one ceiling from whenever a statement last
    // finished on this context.
    context.asked_at_ns = clock_.Now();
    ++in_doubt_;
    ++prepared_;
    // **The checkpoint must not outrun this record** (R6-4): a prepared
    // participant is live, so it lands in every `CHECKPOINT_BEGIN`'s active
    // table as an ordinary transaction, and a redo start that advanced past
    // the prepare would leave the next mount reading it as a loser. The
    // transaction carries the LSN and `OldestPreparedLsn` is what the
    // checkpointer floors at. The same call raises the flag a **writer** of
    // this transaction's rows reads (R6-5, `Transaction::prepared`), which
    // is why one function sets both.
    if (txn::Transaction* txn = context.session.transaction(); txn != nullptr) {
        txn->MarkPrepared(lsn);
    }
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("2pc", "core " + std::to_string(core_id_) + " prepared core " +
                               std::to_string(key.first) + "'s session " +
                               std::to_string(key.second) + " at lsn " + std::to_string(lsn));
    }
    reply(Status::OK());
    // **The decision may already be here**, held by `Decide` because this
    // phase was running when it arrived - a coordinator that another
    // participant's refusal made decide while this core was still reaching
    // the device. Applied now, in the order it arrived, on the arm above as
    // well as this one: an ABORT for a prepare that failed is exactly what
    // that context needs.
    ApplyHeldDecision(it);
    co_return Status::OK();
}

void ShippedStatementExecutor::ApplyHeldDecision(
    std::map<DedupKey, std::unique_ptr<Enrolled>>::iterator it) {
    Enrolled& context = *it->second;
    if (!context.decision_pending) return;
    context.decision_pending = false;
    StartDecision(it, context.decision_commits, std::move(context.decision_reply));
}

void ShippedStatementExecutor::Decide(const Txn2pcServer::DecideAsk& ask,
                                      Txn2pcServer::ReplyFn reply) {
    const DedupKey key{ask.coordinator, ask.session_id};
    const bool commit = ask.decision == TxnDecision::kCommit;
    auto it = enrolled_.find(key);

    if (it == enrolled_.end()) {
        if (!commit) {
            // **Benign, and acknowledged as such.** An abort for a context
            // that is already gone has nothing left to do: the sweep rolled
            // it back, or a refused prepare did. Counted as an abort
            // applied, because from the coordinator's side that is exactly
            // what happened.
            ++decides_aborted_;
            reply(Status::OK());
            return;
        }
        if (ask.retry) {
            // **R6-0's bit, read where R6-1 said it would be**: on this leg
            // it separates a benign resend from a decide for a transaction
            // this core never prepared. A marked resend meeting no context
            // is the first of those - the participant applied the decision
            // and dropped the context, and the acknowledgement was lost -
            // so it is acknowledged again rather than counted as the
            // anomaly below. No counter of its own: nothing resends a
            // decide until R6-5, so a field for it would read structurally
            // 0, against the "absent rather than zeroed" rule.
            if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
                log_->Debug("2pc", "core " + std::to_string(core_id_) +
                                       " acknowledged a resent decision for core " +
                                       std::to_string(ask.coordinator) + "'s session " +
                                       std::to_string(ask.session_id) +
                                       ", which it has already applied and released");
            }
            reply(Status::OK());
            return;
        }
        // A commit for a transaction this core does not hold. The
        // coordinator's decision is durable, so this is the shape of a lost
        // participant - and there is nothing here to apply it to. Refused
        // and logged at Error rather than acknowledged, because an ack
        // would tell the coordinator the transaction is whole when part of
        // it is missing.
        ++decide_refusals_;
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("2pc", "core " + std::to_string(core_id_) +
                                   " was told to commit core " + std::to_string(ask.coordinator) +
                                   "'s session " + std::to_string(ask.session_id) +
                                   ", which it holds no transaction for; the decision stands "
                                   "and this core's part of it is lost");
        }
        reply(Status::InvalidArgument(
            "cross-owner transaction: core " + std::to_string(core_id_) +
            " holds no transaction for this session and cannot commit it"));
        return;
    }
    Enrolled& context = *it->second;

    if (context.coordinator_txn_id != 0 &&
        context.coordinator_txn_id != ask.transaction_id) {
        // The identity check, in the direction that matters: this core
        // prepared *a* transaction for this session and is being told about
        // a different one. Applying it would end the wrong transaction.
        ++decide_refusals_;
        reply(Status::InvalidArgument(
            "cross-owner transaction: core " + std::to_string(core_id_) + " holds transaction " +
            std::to_string(context.coordinator_txn_id) +
            " for this session and was told to decide " + std::to_string(ask.transaction_id)));
        return;
    }

    // **A decide that meets a phase still running is held, not refused.**
    // The reachable case is not a race at the ceiling: one participant
    // refuses instantly, the coordinator decides ABORT and sends it while
    // *this* core's prepare is still reaching the device. Refusing that
    // decide would leave a participant that goes on to become prepared with
    // no decision ever coming - the sweep skips it, shutdown skips it, and
    // this row has no resolution ask - so the answer is to remember it and
    // apply it the moment the prepare wakes.
    //
    // A statement running on the context is the other overlap and is not
    // held: it is `Finish` that would tear the session down under a
    // dispatch, and a decide arriving mid-statement means a coordinator
    // that did not wait for its own answer.
    if (running_.find(key) != running_.end()) {
        ++decide_refusals_;
        reply(Status::TxnConflict("cross-owner transaction: core " + std::to_string(core_id_) +
                                  " is still running this session's statement; the decision "
                                  "cannot be applied under it"));
        return;
    }
    if (context.phase_running) {
        if (context.decision_pending) {
            // Two decisions for one transaction. The first is the one this
            // core was told to keep, and D4 gives a transaction exactly one.
            ++decide_refusals_;
            reply(Status::InvalidArgument(
                "cross-owner transaction: core " + std::to_string(core_id_) +
                " already holds an undelivered decision for this session"));
            return;
        }
        context.decision_pending = true;
        context.decision_commits = commit;
        context.decision_reply = std::move(reply);
        if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
            log_->Debug("2pc", "core " + std::to_string(core_id_) + " holds a " +
                                   (commit ? "COMMIT" : "ABORT") + " for core " +
                                   std::to_string(key.first) + "'s session " +
                                   std::to_string(key.second) +
                                   " until its prepare reaches the device");
        }
        return;
    }

    StartDecision(it, commit, std::move(reply));
}

void ShippedStatementExecutor::StartDecision(
    std::map<DedupKey, std::unique_ptr<Enrolled>>::iterator it, bool commit,
    Txn2pcServer::ReplyFn reply) {
    Enrolled& context = *it->second;
    if (commit && !context.prepared) {
        // **Never commit unprepared work.** By D4 a coordinator commits
        // only once every participant replied prepared, so this can only be
        // a coordinator that did not wait, or a prepare that failed after
        // the decision was made - and committing here would make this
        // core's half durable on the strength of a promise it never gave.
        ++decide_refusals_;
        if (reply) {
            reply(Status::InvalidArgument(
                "cross-owner transaction: core " + std::to_string(core_id_) +
                " was told to commit a transaction it has not prepared"));
        }
        return;
    }

    context.phase_running = true;
    context.decision_commits = commit;
    context.decision_reply = std::move(reply);
    context.decision_out = DispatchOutcome{};
    // A literal, so the text outlives every park `DispatchAsync` takes -
    // and the *ordinary* verbs, because a participant's transaction is an
    // ordinary local transaction (R6-2's whole argument). The async entry
    // point rather than `Dispatch`, so the commit joins this core's group
    // commit instead of taking an `fdatasync` on its reactor.
    static constexpr std::string_view kCommit = "COMMIT";
    static constexpr std::string_view kRollback = "ROLLBACK";
    const DedupKey key = it->first;
    scheduler_.Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        dispatcher_.DispatchAsync(commit ? kCommit : kRollback, &context.session,
                                  &context.decision_out),
        [this, key](const Status&) { FinishDecision(key); }));
}

void ShippedStatementExecutor::FinishDecision(const DedupKey& key) {
    auto it = enrolled_.find(key);
    if (it == enrolled_.end()) return;  // unreachable: one completion per decide
    Enrolled& context = *it->second;
    context.phase_running = false;
    const bool was_prepared = context.prepared;
    const bool commit = context.decision_commits;
    Txn2pcServer::ReplyFn reply = std::move(context.decision_reply);

    // The rendered line back into a code, `Finish`'s rule: an error line
    // carries its message in the status, and a success's line is the
    // transaction's own `COMMIT trx_id=...`, which the coordinator has no
    // use for - what it needs is the code.
    Status status = StatusFromErrorReply(context.decision_out.response);

    if (status.ok()) {
        if (commit) {
            ++decides_committed_;
        } else {
            ++decides_aborted_;
        }
        if (was_prepared && in_doubt_ > 0) --in_doubt_;
        enrolled_.erase(it);
        if (reply) reply(Status::OK());
        return;
    }

    // The decided end refused, and what that leaves behind depends on
    // whether this core had prepared.
    //
    // **Unprepared** (an abort of a context that never promised anything):
    // bad and bounded - `RollbackLocal`'s only arm that leaves the
    // transaction open is the assertion enforcer's, and `ExpireEnrolled`
    // sweeps an unprepared context, so the ceiling retries it.
    //
    // **Prepared**: the sweep passes over it (D4), so nothing here retries
    // it and the context stands until the process stops - one live
    // transaction, one enrolment slot, and the read horizon it pins. On the
    // *commit* arm that is correct and is the worst outcome this protocol
    // has: the coordinator's decision is durable and this core has not
    // applied it, which is exactly the in-doubt population R6-5 resolves.
    // On the *abort* arm the standing context is residue rather than doubt -
    // the decision is known and it is ABORT - and retrying it is R6-5's to
    // build, because D4's prohibition is on a **unilateral** abort and this
    // one would not be. Either way all this path can do is say so, in a code
    // the coordinator counts and a line an operator can find.
    ++decide_refusals_;
    if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
        log_->Error("2pc", "core " + std::to_string(core_id_) + " could not " +
                               (commit ? "commit" : "roll back") + " core " +
                               std::to_string(key.first) + "'s session " +
                               std::to_string(key.second) +
                               " cross-owner transaction: " + status.message());
    }
    if (!context.session.in_explicit_txn()) {
        // Whatever it answered, the transaction is over: the context must
        // not be left standing as one, for `Finish`'s reason - the next
        // statement would run outside any transaction while the coordinator
        // believed otherwise.
        if (was_prepared && in_doubt_ > 0) --in_doubt_;
        enrolled_.erase(it);
    }
    if (reply) reply(status);
}

void ShippedStatementExecutor::Resolve(const Txn2pcServer::ResolveAnswer& answer) {
    const DedupKey key{answer.coordinator, answer.session_id};
    auto it = enrolled_.find(key);
    if (it == enrolled_.end()) {
        // The decide arrived while the ask was in flight and released the
        // context. Nothing to do and nothing wrong: this is what a
        // coordinator answering both looks like, and the ask cost one round
        // trip.
        return;
    }
    Enrolled& context = *it->second;
    if (!context.prepared || context.phase_running ||
        running_.find(key) != running_.end()) {
        // Not this core's to act on right now. A context that is no longer
        // prepared was decided between the ask and the answer; one with a
        // phase or a statement running is mid-decide already, and applying
        // a second decision under it is what `Decide`'s own guards refuse.
        return;
    }
    if (context.coordinator_txn_id != answer.transaction_id) {
        // The answer names a different transaction than the one this
        // context prepared. `Decide`'s identity check, on the leg that
        // carries no waiter to check it for us.
        ++decide_refusals_;
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("2pc", "core " + std::to_string(core_id_) + " asked about transaction " +
                                   std::to_string(context.coordinator_txn_id) +
                                   " and was answered about " +
                                   std::to_string(answer.transaction_id) + "; ignored");
        }
        return;
    }

    if (!answer.status.ok()) {
        if (answer.status.code() == StatusCode::kUnknownOutcome) {
            // **The coordinator no longer holds the record** (D5). Nothing
            // is applied: committing would make this core's half durable on
            // no authority, and aborting would contradict a coordinator
            // that may have committed. The context stays prepared, stays in
            // doubt, goes on holding its locks - and the next mount reads
            // the coordinator's *stream*, which is the durable record its
            // memory is not (R6-4). Counted, because this is the one
            // outcome that is not resolved by the time this returns.
            ++in_doubt_resolved_unknown_;
            if (log_ != nullptr && log_->enabled(LogLevel::kWarn)) {
                log_->Warn("2pc", "core " + std::to_string(core_id_) + " asked core " +
                                      std::to_string(answer.coordinator) +
                                      " about transaction " +
                                      std::to_string(answer.transaction_id) +
                                      " and was told the outcome cannot be established there; "
                                      "this core stays in doubt until the next mount resolves "
                                      "it against that core's stream");
            }
            // Not asked again: the answer is terminal. The stamp still
            // moves so the sweep does not re-ask every tick.
            context.asked_at_ns = clock_.Now();
            return;
        }
        // "Not decided yet" and anything else: keep waiting, ask again on
        // the next ceiling. The stamp moved when the ask went out, so the
        // cadence is already one ask per ceiling and this needs no action.
        return;
    }

    const bool commit = answer.decision == TxnDecision::kCommit;
    if (commit) {
        ++in_doubt_resolved_committed_;
    } else {
        ++in_doubt_resolved_aborted_;
    }
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("2pc", "core " + std::to_string(core_id_) + " resolved transaction " +
                              std::to_string(answer.transaction_id) + " as " +
                              (commit ? "COMMIT" : "ABORT") + " by asking core " +
                              std::to_string(answer.coordinator));
    }
    // No reply: nobody is parked on this, and the coordinator already knows
    // what it decided. `StartDecision` is the same entry point a decide
    // message takes, so "never commit unprepared work" is asked once.
    StartDecision(it, commit, {});
}

void ShippedStatementExecutor::ExpireEnrolled() {
    // Before the clock read: on every live path today this map is empty,
    // and this runs on every core's drain tick.
    if (enrolled_.empty()) return;
    const sched::MonoTimeNs now = clock_.Now();
    for (auto it = enrolled_.begin(); it != enrolled_.end();) {
        // **A prepared context is not the idle ceiling's** (R6-3, D4): this
        // core promised not to abort it unilaterally, and the ceiling that
        // applies to it is D5's, which asks rather than ends. The number of
        // contexts this passes over is `in_doubt()`.
        if (it->second->prepared) {
            // **D5's bounded wait, from the participant's side** (R6-5).
            // One ask per ceiling, for as long as it takes: the alternative
            // - a cap on asks - would leave a participant holding locks
            // with nothing left that could free it before the next mount,
            // and asking costs two ring messages against a transaction that
            // is already blocking writers. The stamp moves before the send,
            // so a ring that refuses the send still costs one ceiling
            // rather than one tick.
            Enrolled& context = *it->second;
            const bool busy =
                context.phase_running || running_.find(it->first) != running_.end();
            if (!busy && txn_2pc_server_ != nullptr &&
                now - context.asked_at_ns >= kTxnInDoubtCeilingNs) {
                context.asked_at_ns = now;
                ++in_doubt_asks_;
                if (log_ != nullptr && log_->enabled(LogLevel::kWarn)) {
                    log_->Warn("2pc", "core " + std::to_string(core_id_) +
                                          " has been in doubt about core " +
                                          std::to_string(it->first.first) + "'s transaction " +
                                          std::to_string(context.coordinator_txn_id) +
                                          " for its ceiling; asking what was decided");
                }
                txn_2pc_server_->Ask(it->first.first, it->first.second,
                                     context.coordinator_txn_id);
            }
            ++it;
            continue;
        }
        // A statement still running on this context keeps it, whatever the
        // clock says: rolling back under a live statement would pull the
        // session out from a coroutine that holds a pointer into it - the
        // deferral `TcpServer::CloseClient` makes for the same reason. A
        // phase in flight (a prepare reaching the device) holds it for the
        // same reason and is the same hazard.
        const bool busy =
            it->second->phase_running || running_.find(it->first) != running_.end();
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
    for (auto it = enrolled_.begin(); it != enrolled_.end();) {
        if (it->second->prepared) {
            // **D4 outranks the shutdown.** A rollback here would append
            // TXN_ABORT for a transaction the coordinator may already have
            // committed in its own stream, and that disagreement is
            // durable - the one outcome two-phase commit exists to prevent.
            // Left instead: the PREPARE record stands, the next mount finds
            // it undecided (R6-4), and the horizon this pins dies with the
            // process anyway.
            ++left_in_doubt_at_stop_;
            if (log_ != nullptr && log_->enabled(LogLevel::kWarn)) {
                log_->Warn("2pc", "core " + std::to_string(core_id_) +
                                      " is stopping with core " + std::to_string(it->first.first) +
                                      "'s session " + std::to_string(it->first.second) +
                                      " prepared and undecided; it is left in doubt rather "
                                      "than rolled back, and resolves at the next mount");
            }
            ++it;
            continue;
        }
        auto doomed = it++;
        EndEnrolled(doomed, "this core is stopping and can no longer answer for it");
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
