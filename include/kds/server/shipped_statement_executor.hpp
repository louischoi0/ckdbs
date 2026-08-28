#pragma once

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/server/statement_ship_service.hpp"
#include "kds/server/txn_2pc_service.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/read_view.hpp"
#include "kds/wal/manager.hpp"

// **The owner's half of statement shipping** (SS3 of the statement-shipping
// work order): what `StatementShipServer`'s seam does with a statement that
// arrived from another core.
//
// It is deliberately thin. The statement runs through **the owner's
// ordinary dispatcher**, on a session of its own, in autocommit - which is
// D3's whole content: a shipped statement is not a special execution mode,
// it is a local statement whose text came over a ring instead of a socket.
// Everything that governs a local statement therefore governs this one: the
// affinity gate, the shape gate, the row-id and transaction-id leases,
// index maintenance, the assertion enforcer.
//
// ---- Why it parks, and what that buys -----------------------------------
//
// `DispatchAsync` is the entry point, not `Dispatch`, and the difference is
// the entire performance thesis. `Dispatch` finishes a `group` commit by
// calling `DrainOnce()` + `EnsureDurable()` on the calling stack: one
// `fdatasync` per statement, taken on the owner's reactor, blocking every
// other connection on that core behind the device. `DispatchAsync` stages
// the commit and parks on `IsDurable(lsn)`, so the next statement - shipped
// or local - runs and stages its own commit into the *same* device sync.
// The pretasks measured that batching at **79x** on one core
// (`bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md` §4), and
// re-concentrating commits onto owners is the reason shipping exists at
// all (`docs/inflight/in-progress/memo-shipping-and-group-commit.md` §3).
//
// ---- The answer, and how a code survives the round trip ------------------
//
// A dispatcher answers in a *rendered line*: a success is the client's
// reply, a failure is `ErrorReply`'s spelling. What crosses back is a
// status code and a text, because the arrival core re-renders through the
// same `ErrorReply` and the `retryable` bit a client's retry loop reads
// must be the bit the owner meant. `StatusFromErrorReply` recovers the
// code from the line (command_dispatcher.hpp states what that recovery is
// exact about and what it is lossy about); the pair round-trips
// byte-identically, which is the property the client actually depends on.
//
// ---- The dedup record (D4) ----------------------------------------------
//
// A lost reply must never become a silent double-execute. Engine-issued
// primary keys make a blind retry a second row, not an idempotent replay,
// so the owner keeps what it answered:
//
//   - keyed by **(requester core, session id)**, because a session id is
//     minted per core and two cores mint the same one;
//   - holding the last `sequence` and the outcome it produced;
//   - answered from, never re-executed, when the same (session, sequence)
//     arrives again;
//   - **refused `UnknownOutcome`** when a *lower* sequence arrives, since
//     that statement's outcome has been superseded and this core can no
//     longer say whether it ran. Guessing is the one thing D4 forbids.
//
// **The record covers the statement while it is still running, not only
// after it has answered**, and that half is the one that matters: an
// arrival core's deadline fires *because* the owner is slow, so the retry
// it provokes is precisely the request that meets the original mid-flight.
// A record written only at `Finish` would let that retry through to a
// second execution - a second row, against an engine-issued pk. `running_`
// is therefore keyed by the same identity and consulted in the same place,
// and a statement for a session already running one is `UnknownOutcome`:
// true, non-retryable, and never a second run.
//
// **What that costs, stated because it is not free**: the in-flight
// refusal is keyed on the session and not on the sequence, so once the
// arrival core's deadline has fired and freed its client, a genuinely
// *new* statement on that session meets the original still running here
// and is answered `UnknownOutcome` about a statement that never started.
// Conservative rather than unsafe - and the premise above ("a session runs
// one statement at a time") is exactly what stops being true at the
// deadline. Keying the in-flight refusal on the sequence would narrow it;
// it is not narrowed today because nothing retries yet, and a refusal that
// is too broad is the safe direction to be wrong in.
//
// Nothing re-sends a landed request today (`sched::SubmitSendPod` retries
// only a send the ring refused, which by definition never arrived), so the
// record is the guard for the retry paths a routing layer will bring, and
// its tests drive it directly rather than through a race that cannot happen
// yet. That is stated because a record nothing exercises is otherwise
// indistinguishable from a record that does not work.
//
// **Bounded, and the bound is derived rather than picked**: a record is
// kept for `kShippedDedupRetentionNs`, twice the arrival core's deadline. A
// duplicate can only matter while the original is still parked somewhere,
// and a waiter past its deadline has already been answered
// `UnknownOutcome` - so a record older than two deadlines cannot be the
// answer to anything still asking. `kShippedDedupMaxRecords` is a second
// bound under it, for memory rather than for correctness - and it is a
// bound on *records*, so the order list carries one node per key and moves
// it rather than appending a second: a session shipping a thousand
// statements inside one retention window holds one entry, not a thousand.
// When the cap bites it evicts the oldest record early and **counts** it
// (`early_evictions()`). Before R6-0 an early eviction was the one
// condition under which a duplicate could reach an empty record and be
// re-executed; the retry bit (`instructions/v2.4.0/2pc.md` §2,
// `ShippedStatement::retry`) closes it - a resend marked `retry` that meets
// an absent record is `UnknownOutcome`, never a second execution. What
// `early_evictions() > 0` now measures is a different cost: a resend that
// arrives *after* its record was dropped pays `UnknownOutcome` where it
// would otherwise have been answered from the record. Nothing sets the bit
// on a live path yet - it is the wire and the semantics only, and the
// routing layer that actually resends a lost request is R6-3's.

namespace kds::server {

// How long an answered statement's outcome is kept, for a duplicate to be
// answered from. Twice the deadline, per the argument above.
inline constexpr sched::MonoTimeNs kShippedDedupRetentionNs = 2 * kShippedStatementDeadlineNs;

// The memory bound under the time bound. 4096 records is one for every
// session that shipped a statement in the last twenty seconds; a core
// serving more concurrent shipping sessions than that evicts early and
// says so.
inline constexpr std::size_t kShippedDedupMaxRecords = 4096;

// ---- R6-2: how long an enrolled transaction may go untouched -------------
//
// **A backstop for a lost abort, not a normal-path bound**, and the
// distinction decides the number. A cross-owner transaction is a client's
// `BEGIN … COMMIT`, so the gap between two of its statements is client think
// time and has no engine-side bound - the same is true of a local
// transaction, which this engine lets a connection hold open indefinitely
// and unwinds only when the socket dies (`tcp_server.cpp`'s close path,
// `docs/spec/txn.md` §10-8). A participant has **no socket to notice that
// death**, which is the whole reason a ceiling has to exist here at all.
//
// So it is set well above any coordinator-side deadline in the series rather
// than tuned to a workload: five minutes against the shipped statement's ten
// seconds. Being wrong in the tight direction rolls back a transaction a
// client is still using, which reaches that client as an abort it did not
// ask for - a wrong answer. Being wrong in the generous direction costs an
// abandoned transaction pinning `ReadHorizon()` for five minutes **and one
// of this core's `txn::kMaxTrackedLiveTxns` slots for the same five
// minutes**, which is the half a first draft of this paragraph left out:
// the table is 64 entries and it is shared with every *local* client, so
// enough abandoned enrolments would refuse an unrelated connection's
// `BEGIN` and nothing would say why. `kShippedMaxEnrolled` below is what
// keeps that from being local clients' problem.
//
// **`Expire` is only sound while nothing here has prepared.** After a
// participant replies prepared it may not unilaterally abort (D4), so R6-3
// must exclude prepared contexts from this sweep - the ceiling then belongs
// to the in-doubt resolution D5 states, not to this constant. Written here
// because R6-2 is where the sweep is introduced and R6-3 is where it would
// silently become wrong.
inline constexpr sched::MonoTimeNs kShippedTxnIdleCeilingNs = 300ull * 1'000'000'000ull;
static_assert(kShippedTxnIdleCeilingNs > kShippedStatementDeadlineNs,
              "a participant must outwait the coordinator's per-statement deadline, or a "
              "transaction is torn down under a statement that is still on its way");

// How many cross-owner transactions one core will hold as a participant.
//
// **A bound on a shared resource, not on memory** - which is what separates
// it from `kShippedDedupMaxRecords` above. Every enrolment is a live local
// transaction, and `txn::kMaxTrackedLiveTxns` (64) is the whole core's
// supply of those, shared with every ordinary client on it. Without a cap
// here, enough coordinators would take all 64 and a local `BEGIN` would be
// refused `OutOfSpace` with nothing pointing at the cause.
//
// A quarter of the table, so three quarters stay local. Past it a
// participant refuses **`TxnConflict`** - the one code the wire's
// `retryable` bit follows (`status.hpp`, and PW6's finding (2)) - because
// the right response is to retry once another cross-owner transaction ends,
// which is a thing that happens on its own.
inline constexpr std::size_t kShippedMaxEnrolled = 16;
static_assert(kShippedMaxEnrolled < txn::kMaxTrackedLiveTxns,
              "a participant may not take the whole core's live-transaction table; local "
              "clients share it and would be refused BEGIN with nothing naming the cause");

class ShippedStatementExecutor {
public:
    // `dispatcher` is this core's own - the one a local connection would
    // use. The executor must not outlive it, nor the scheduler it submits
    // to, nor the `StatementShipServer` whose `ReplyFn` its running
    // statements hold: **declare it after all three**, so it is destroyed
    // first.
    // `wal` is this core's own stream, and it is what R6-3's prepare
    // writes into. A null one is the fixture case and behaves the way
    // every other unlogged path in this engine behaves - the record is not
    // written and nothing pretends it was; what it costs is stated at
    // `Prepare` rather than here, because that is the one caller for which
    // "unlogged" changes a promise rather than a durability class.
    ShippedStatementExecutor(std::uint32_t core_id, CommandDispatcher& dispatcher,
                             sched::Scheduler& scheduler, const sched::Clock& clock,
                             Logger* log = nullptr, wal::WalManager* wal = nullptr) noexcept
        : core_id_(core_id),
          dispatcher_(dispatcher),
          scheduler_(scheduler),
          clock_(clock),
          log_(log),
          wal_(wal) {}

    ShippedStatementExecutor(const ShippedStatementExecutor&) = delete;
    ShippedStatementExecutor& operator=(const ShippedStatementExecutor&) = delete;

    // The seam `StatementShipServer` takes. Captures `this`, so the
    // executor must outlive the server it is installed in.
    StatementShipServer::ExecuteFn Seam() {
        return [this](StatementShipServer::ShippedStatement statement,
                      StatementShipServer::ReplyFn reply) {
            Execute(std::move(statement), std::move(reply));
        };
    }

    // ---- R6-3: the two seams `Txn2pcServer` takes ------------------------
    //
    // Here rather than in that class because **this** object owns the
    // enrolment: a participant finds its transaction by
    // `(coordinator core, session id)`, which is the key this map is built
    // on, and the transport looks nothing up.
    Txn2pcServer::PrepareFn PrepareSeam() {
        return [this](Txn2pcServer::PrepareAsk ask, Txn2pcServer::ReplyFn reply) {
            Prepare(ask, std::move(reply));
        };
    }
    Txn2pcServer::DecideFn DecideSeam() {
        return [this](Txn2pcServer::DecideAsk ask, Txn2pcServer::ReplyFn reply) {
            Decide(ask, std::move(reply));
        };
    }
    // R6-5's third seam: the coordinator's answer to an ask this core sent.
    Txn2pcServer::ResolveFn ResolveSeam() {
        return [this](Txn2pcServer::ResolveAnswer answer) { Resolve(answer); };
    }
    // The transport this core asks its coordinators through (R6-5). Set by
    // the wiring after both objects exist - the server holds this
    // executor's seams, so the executor cannot hold the server by
    // construction - and null on every fixture that drives the seams
    // directly, where an in-doubt context simply waits with nothing to ask.
    void SetTxn2pcServer(Txn2pcServer* server) noexcept { txn_2pc_server_ = server; }

    // Statements this core ran on another core's behalf, and finished.
    std::uint64_t executed() const noexcept { return executed_; }
    // Duplicates answered from the record instead of run again (D4).
    std::uint64_t deduped() const noexcept { return deduped_; }
    // Duplicates this core could not answer for - superseded by a later
    // sequence, still running here so that no outcome exists yet, or (R6-0)
    // a marked retry that met no record of its sequence. Each one is a
    // client told `UNKNOWN_OUTCOME`.
    std::uint64_t unanswerable() const noexcept { return unanswerable_; }
    // Records dropped by the memory bound before their retention expired.
    // **No longer a correctness signal since R6-0**: a marked retry meeting
    // the empty record is refused rather than re-executed. What it counts
    // now is the availability cost - outcomes that can only be answered
    // `UnknownOutcome` because the record for them is gone.
    std::uint64_t early_evictions() const noexcept { return early_evictions_; }
    // Statements running right now: the population the owner's reactor is
    // carrying on other cores' behalf.
    std::size_t running() const noexcept { return running_.size(); }
    // Outcomes the record holds - the number `kShippedDedupMaxRecords`
    // bounds. Read off the order list rather than the map so that the two
    // going out of step is visible from outside, since it is the list that
    // would grow with the shipping rate if a key ever took a second node.
    std::size_t records() const noexcept { return answered_order_.size(); }

    // ---- R6-2 ------------------------------------------------------------

    // Cross-owner transactions this core is a participant in right now.
    // Each one is a live local transaction, so each one pins
    // `ReadHorizon()` - which is why the number is worth reading and why
    // the ceiling below exists.
    std::size_t enrolled() const noexcept { return enrolled_.size(); }
    // Transactions opened on this core's behalf of a coordinator, ever.
    std::uint64_t enrolments() const noexcept { return enrolments_; }
    // Enrolments this core refused: its own limit, or a transaction-id
    // lease it could not draw from. Apart from `unanswerable()` because
    // neither is a duplicate and neither is in doubt - both are retryable,
    // and a coordinator may act on them.
    std::uint64_t enrolment_refusals() const noexcept { return enrolment_refusals_; }

    // **RR0: statements refused because their context was gone.** A
    // statement that may only join a transaction and found none - the idle
    // ceiling having rolled it back, or this core having stopped and come
    // back. A rising number means coordinators are holding cross-owner
    // transactions open past `kShippedTxnIdleCeilingNs`.
    //
    // **A subset of `enrolment_refusals_`, not a count beside it.** Every
    // refusal `EnrolFor` returns is counted there by its one caller, so the
    // two must not be summed - this is *which* of those refusals were a
    // context that existed and does not any more, against the rest, which
    // are a context that could not be opened at all.
    std::uint64_t join_refusals() const noexcept { return join_refusals_; }
    // Transactions the idle ceiling rolled back because no decide came.
    // **Non-zero means a coordinator abandoned one**, which is a defect
    // somewhere else - nothing on a healthy path reaches the ceiling.
    std::uint64_t enrolment_expiries() const noexcept { return enrolment_expiries_; }

    // ---- R6-3 ------------------------------------------------------------

    // Transactions this core has replied **prepared** for and not yet been
    // told the outcome of. Each one holds its locks and its transaction and
    // may not be aborted here, so this is the population D5's in-doubt
    // handling is about - and while it is non-zero, this core's shutdown
    // cannot claim it left nothing undecided.
    std::size_t in_doubt() const noexcept { return in_doubt_; }
    // Prepares this core answered *prepared*, ever.
    std::uint64_t prepared() const noexcept { return prepared_; }
    // Prepares this core refused - the population that turns a coordinator's
    // commit into an abort, with a reason the client reads.
    std::uint64_t prepare_refusals() const noexcept { return prepare_refusals_; }
    // Decides applied here, split by what they said. `decides_committed()`
    // is the number that must equal the coordinator's committed cross-owner
    // transactions once every ack is in.
    std::uint64_t decides_committed() const noexcept { return decides_committed_; }
    std::uint64_t decides_aborted() const noexcept { return decides_aborted_; }
    // Decides this core could not apply, or would not: a decide for a
    // transaction it never prepared, a commit whose local commit failed, an
    // identity that is not the one it prepared under, and (R6-5) an
    // in-doubt answer naming a different transaction than the context
    // asked about. **Non-zero is a protocol anomaly**, not a workload
    // property - with one benign exception R6-5 introduces and cannot
    // distinguish here: a decide that outran D5's ceiling, so that the
    // participant resolved by asking and the original decide arrived behind
    // its own answer. That case is counted here and logged, and its outcome
    // is already correct; separating it needs a decided-window on this side
    // or a bit the coordinator has no way to set, which is R6-8's to weigh
    // when the path goes live.
    std::uint64_t decide_refusals() const noexcept { return decide_refusals_; }
    // Prepared transactions this core stopped with, rather than rolling
    // them back. D4 forbids the rollback; R6-6 owns what the next mount
    // does with them, and this is the number that says whether that path
    // was reached.
    //
    // **The protection this counts is not yet end to end** (R6-3, stated
    // here because the counter would otherwise read as one that is):
    // `wal/analysis.cpp` has no `TXN_PREPARE` arm, so the transaction this
    // path carefully declined to abort is still a loser at the next mount
    // and undo unwinds it. R6-4 is what closes that, and the series ships
    // as one at RP7's gate.
    std::uint64_t left_in_doubt_at_stop() const noexcept { return left_in_doubt_at_stop_; }

    // The isolation level a cross-owner transaction was opened at here
    // (R6-8), or empty where this core holds no context for that
    // coordinator session.
    //
    // **A reader for a promise, and it has no other one.** The level is the
    // client's choice and it crosses on the request; that it *arrived* is
    // otherwise checkable only by inference - two statements and a
    // concurrent commit, to see whether the participant's view moved - and
    // an inference is a poor test of a contract. `SHOW META` deliberately
    // does not print it: it is a property of one transaction, not a rate.
    std::optional<txn::IsolationLevel> enrolled_isolation(std::uint32_t coordinator,
                                                          std::uint64_t session_id) const {
        auto it = enrolled_.find(DedupKey{coordinator, session_id});
        if (it == enrolled_.end()) return std::nullopt;
        return it->second->session.isolation();
    }

    // Prepares this core has asked a coordinator about, ever (R6-5). One
    // per ceiling per in-doubt transaction, so a rising number against a
    // flat `in_doubt()` is a coordinator that is not answering.
    std::uint64_t in_doubt_asks() const noexcept { return in_doubt_asks_; }
    // In-doubt transactions a coordinator's answer resolved, split by what
    // it said. **`in_doubt_resolved_unknown()` is the one that matters**:
    // it counts transactions whose coordinator no longer holds the record,
    // which stay prepared here until the next mount reads that
    // coordinator's stream (R6-4) - and which go on holding their locks and
    // blocking writers of their rows until then.
    std::uint64_t in_doubt_resolved_committed() const noexcept {
        return in_doubt_resolved_committed_;
    }
    std::uint64_t in_doubt_resolved_aborted() const noexcept {
        return in_doubt_resolved_aborted_;
    }
    std::uint64_t in_doubt_resolved_unknown() const noexcept {
        return in_doubt_resolved_unknown_;
    }

    // Rolls back every enrolled transaction idle past
    // `kShippedTxnIdleCeilingNs`, and (R6-5) asks about every prepared one
    // that has been in doubt for `kTxnInDoubtCeilingNs`. Driven from the
    // reactor's periodic tick, the way `PendingIndexBuilds::Expire` is - a
    // lazy sweep would never run for an abandoned context, since nothing
    // arrives for it by definition.
    //
    // **A prepared context is not the ceiling's** (R6-3, D4): a participant
    // that has replied prepared may not unilaterally abort, so
    // `kShippedTxnIdleCeilingNs` stops applying to it and D5's ask applies
    // instead. That is the whole difference between the two halves of this
    // sweep - one ends transactions, the other asks about them and ends
    // nothing.
    void ExpireEnrolled();

    // Rolls back every enrolled transaction, whatever its age - **except a
    // prepared one** (R6-3). The shutdown path: an open transaction that
    // outlives its executor pins the horizon for the life of the process,
    // and this core is about to stop being able to answer for it either
    // way.
    //
    // The exception is not tidiness, it is D4. A prepared participant may
    // not abort unilaterally, and a rollback here would append TXN_ABORT to
    // this stream for a transaction the coordinator may already have
    // committed in its own - which is the one durable disagreement this
    // protocol exists to prevent. So a prepared context is *left*, counted
    // in `left_in_doubt_at_stop()`, and its resolution belongs to the next
    // mount (R6-4) or to D5's ask (R6-5). What it costs in the meantime is
    // nothing: the process is stopping, so the horizon it pins dies with
    // it.
    //
    // **Precondition: the reactor has stopped.** Unlike `ExpireEnrolled`
    // this does not skip a context a statement is running on, and it is
    // sound only because its callers run after the worker joined - a
    // suspended `CoroTask` destroyed rather than completed never invokes its
    // completion (`sched/coro.hpp`), so the `Session*` a parked statement
    // holds is never dereferenced. Called from a live reactor with a
    // statement in flight, it is a use-after-free.
    void RollbackAllEnrolled();

private:
    // What one shipped statement holds while it runs. Heap-allocated and
    // stable: `DispatchAsync` borrows `text` as a `string_view` and writes
    // `out` when it finishes, both across every park it takes.
    struct Running {
        std::string text;
        // **Engaged only on the autocommit path.** An enrolled statement
        // runs on the session its transaction is held by (`Enrolled`), which
        // outlives this struct - so the session cannot live here, and a
        // pointer says which one is in play without a second code path.
        std::optional<Session> own_session;
        Session* session = nullptr;
        DispatchOutcome out;
        StatementShipServer::ReplyFn reply;
        // The identity's third component. The first two are the map key.
        std::uint64_t sequence = 0;

        // Derived, not stored: the two constructors already say which case
        // this is, and a third could forget to set a flag. Deliberately not
        // `session->in_explicit_txn()`, which is a different question - that
        // is also true of the failure `Finish` must still catch, an
        // autocommit statement that opened a transaction.
        bool enrolled() const noexcept { return !own_session.has_value(); }

        // Autocommit: this statement owns its session and dies with it.
        Running(std::string statement, txn::IsolationLevel isolation, Role role)
            : text(std::move(statement)), own_session(std::in_place, isolation) {
            own_session->set_role(role);
            session = &*own_session;
        }
        // Enrolled: the transaction's session, owned by `Enrolled`.
        Running(std::string statement, Session& enrolled_session)
            : text(std::move(statement)), session(&enrolled_session) {}
    };

    using DedupKey = std::pair<std::uint32_t, std::uint64_t>;  // (requester, session id)

    // ---- R6-2: a cross-owner transaction's participant half --------------
    //
    // One per `(coordinator core, session id)` that has shipped an enrolled
    // statement. It holds the **local** transaction (D2: this core's own id,
    // from this core's own lease) on a session that outlives the statement
    // that opened it - the `KwpLoadServer` shape, which holds one session's
    // transaction across many messages through the ordinary
    // `Dispatch("BEGIN"/"COMMIT"/"ROLLBACK")` seam and adds no transaction
    // code of its own.
    //
    // `coordinator_txn_id` is 0 until prepare brings it (R6-3). D2 asks the
    // participant to record the coordinator's `(session_id, transaction_id)`
    // beside its own, and only the first half of that pair is knowable here:
    // the statement path carries no transaction id, by the sizing argument
    // in `statement_ship_service.hpp`.
    struct Enrolled {
        Session session;
        // The coordinator's, recorded when prepare brings it (R6-3). D2 asks
        // for it by name, which is why it is here before its writer is.
        std::uint64_t coordinator_txn_id = 0;
        // Moved when a statement *finishes*, so the ceiling measures
        // idleness: a long transaction that is still being used is not the
        // thing the sweep is looking for.
        sched::MonoTimeNs touched_at_ns = 0;
        // **This core has replied prepared and may no longer abort** (D4).
        // Set only once the PREPARE record is durable, never at the append:
        // a reply sent before the sync would promise a durability the
        // device has not given, which is the one thing prepare means.
        bool prepared = false;
        // A phase is running on this context right now - the prepare
        // awaiting its sync, or the decided `COMMIT`/`ROLLBACK` running.
        // Held for the reason `running_` is held for a statement: both park,
        // and a sweep that tore the session down under one would pull the
        // ground from a live coroutine. `running_` cannot serve here
        // because a phase is not a statement and takes no entry in it.
        bool phase_running = false;
        // The decide leg's state, held here because this context is what
        // owns the transaction being ended and there is at most one
        // decision per context. `DispatchAsync` writes `decision_out`
        // across every park it takes, so it must not move - which is why
        // the map's value is a `unique_ptr` (the same reason a running
        // statement's `Session*` needs).
        DispatchOutcome decision_out;
        Txn2pcServer::ReplyFn decision_reply;
        bool decision_commits = false;
        // **A decision that arrived while the prepare was still reaching
        // the device**, held rather than refused. Its reachable cause is
        // another participant refusing, which lets the coordinator decide
        // before this core has answered at all; refusing it would strand a
        // participant that goes on to become prepared with no decision
        // coming, since nothing in this row resolves one. `AwaitPrepared`
        // applies it on wake.
        bool decision_pending = false;
        // R6-5. When the last in-doubt ask went out, or when the promise
        // was made if none has - the ceiling runs from whichever, so the
        // first ask waits one ceiling after prepare and each later one a
        // ceiling after the last. Separate from `touched_at_ns` because
        // that one moves when a *statement* finishes, and a prepared
        // context takes no statements: folding them would make the ask
        // cadence depend on work that can no longer happen.
        sched::MonoTimeNs asked_at_ns = 0;
        // **The ask is over** (R6-5, D5). Raised by an `UnknownOutcome`
        // answer, which is the one terminal answer the leg has: the
        // coordinator holds no record, may not re-decide, and the only
        // thing left that can resolve this transaction is the next mount
        // reading that coordinator's stream (R6-4). Without it the sweep
        // re-asks every ceiling for the life of the process - a question
        // whose answer cannot change, a Warn line per ceiling per stuck
        // transaction, and an `in_doubt_resolved_unknown_` that counts asks
        // instead of the transactions its accessor documents.
        bool resolve_terminal = false;

        Enrolled(txn::IsolationLevel isolation, Role role, sched::MonoTimeNs now)
            : session(isolation), touched_at_ns(now) {
            session.set_role(role);
        }
    };

    // The outcome kept for a duplicate to be answered from.
    struct Answered {
        std::uint64_t sequence = 0;
        Status status;
        std::string text;
        sched::MonoTimeNs at_ns = 0;
        // This key's one node in `answered_order_`. A list, so the node is
        // stable while every other entry comes and goes, and re-recording a
        // key splices it to the back instead of appending a second one.
        std::list<DedupKey>::iterator order;
    };

    void Execute(StatementShipServer::ShippedStatement statement,
                 StatementShipServer::ReplyFn reply);
    void Finish(const DedupKey& key);
    void Remember(const DedupKey& key, std::uint64_t sequence, const Status& status,
                  std::string_view text);
    void Expire();
    // Opens the transaction for `key` if it has none, and answers the
    // session to run on. Refuses without leaving a half-open context - a
    // spent transaction-id lease refuses `TxnConflict` here, and that is a
    // retryable refusal the coordinator may act on.
    //
    // `isolation` is the **coordinator's** (R6-8): the level is the
    // client's choice, and this core's own default is what it fell back to
    // before the level crossed - which under D3 handed a transaction the
    // wrong promise. Used only when the context is opened; a statement
    // joining an existing one runs at the level that one was opened with,
    // since a transaction has one level for its life.
    // `join` is RR0's wire bit: true means this statement may join an
    // existing context and may not open one, which is what the coordinator
    // says on every statement after the first it sent this core in this
    // transaction.
    StatusOr<Enrolled*> EnrolFor(const DedupKey& key, Role role, txn::IsolationLevel isolation,
                                 bool join);

    // Rolls `it`'s transaction back and drops the context. **Rollback
    // only**, still: R6-3's commit arm is not here but in `Decide`, and for
    // the reason this comment already gave - `Dispatch("COMMIT")` finishes
    // a group commit inline, so a commit from a timer-driven path would
    // take a blocking `fdatasync` on the reactor. The decide leg has a
    // coroutine to park in and uses `DispatchAsync` instead.
    void EndEnrolled(std::map<DedupKey, std::unique_ptr<Enrolled>>::iterator it,
                     std::string_view why);

    // ---- R6-3: the two phases, as this core sees them --------------------
    void Prepare(const Txn2pcServer::PrepareAsk& ask, Txn2pcServer::ReplyFn reply);
    void Decide(const Txn2pcServer::DecideAsk& ask, Txn2pcServer::ReplyFn reply);
    // ---- R6-5: the coordinator's answer to an ask this core sent ---------
    //
    // Applied through the same `StartDecision` a decide message takes, with
    // no reply to send: nobody is parked on this, and the coordinator
    // already knows what it decided. An `UnknownOutcome` answer applies
    // nothing - the context stays prepared and in doubt, which is the only
    // honest state, and the next mount resolves it (R6-4).
    void Resolve(const Txn2pcServer::ResolveAnswer& answer);
    // Parks until the prepare record is durable, then answers. A coroutine
    // rather than an inline `EnsureDurable` because this runs on the
    // participant's reactor, which serves every other connection on this
    // core: a blocking sync here is the cost statement shipping exists to
    // remove, paid once per cross-owner commit instead of once per
    // statement.
    sched::Coro AwaitPrepared(DedupKey key, wal::Lsn lsn, sched::MonoTimeNs deadline_ns,
                              Txn2pcServer::ReplyFn reply);
    // Runs the decided end - `COMMIT` or `ROLLBACK` - on the context's own
    // session. One entry point for both callers, `Decide` and the prepare
    // that woke holding a decision, so the "never commit unprepared work"
    // rule is asked once rather than at each.
    void StartDecision(std::map<DedupKey, std::unique_ptr<Enrolled>>::iterator it, bool commit,
                       Txn2pcServer::ReplyFn reply);
    // The decision `Decide` held because a phase was running, applied now
    // that it is not. A no-op where none is held, which is every ordinary
    // prepare.
    void ApplyHeldDecision(std::map<DedupKey, std::unique_ptr<Enrolled>>::iterator it);
    // The decided end of the transaction, once the dispatcher's `COMMIT` or
    // `ROLLBACK` has finished: read what it answered, drop the context, and
    // acknowledge. The completion of a `DispatchAsync` task rather than a
    // coroutine of its own - `Execute`/`Finish`'s shape, and for its
    // reason: the dispatcher's coroutine is the thing that parks, and
    // wrapping it in a second one would add a frame that waits on nothing.
    void FinishDecision(const DedupKey& key);

    std::uint32_t core_id_;
    CommandDispatcher& dispatcher_;
    sched::Scheduler& scheduler_;
    const sched::Clock& clock_;
    Logger* log_;
    // This core's own stream, R6-3's prepare record's destination. Null on
    // an unlogged instance, which is the fixture case.
    wal::WalManager* wal_;
    // R6-5's ask transport, borrowed. Null where nothing can ask.
    Txn2pcServer* txn_2pc_server_ = nullptr;

    // **Keyed by the shipping identity**, which is what makes the record
    // reach the in-flight half of the window: a duplicate is recognised
    // while its original is still running, not only after it has answered.
    // At most one entry per key, because `Execute` refuses a second
    // statement for a session that already has one here.
    std::map<DedupKey, std::unique_ptr<Running>> running_;

    std::map<DedupKey, Answered> answered_;
    // Recording order, oldest at the front, for the two bounds. Exactly one
    // node per record - `Answered::order` is it - so `kShippedDedupMaxRecords`
    // bounds this list and not merely the map above it.
    std::list<DedupKey> answered_order_;

    // R6-2's contexts. `unique_ptr` for the same reason `running_` uses one:
    // a running statement holds a `Session*` into the entry across every
    // park it takes, and a map node's value must not move under it.
    std::map<DedupKey, std::unique_ptr<Enrolled>> enrolled_;

    std::uint64_t executed_ = 0;
    std::uint64_t deduped_ = 0;
    std::uint64_t unanswerable_ = 0;
    std::uint64_t early_evictions_ = 0;
    std::uint64_t enrolments_ = 0;
    std::uint64_t enrolment_refusals_ = 0;
    std::uint64_t join_refusals_ = 0;
    std::uint64_t enrolment_expiries_ = 0;

    // R6-3. `in_doubt_` is derived from `enrolled_`'s `prepared` flags and
    // kept beside them rather than counted on demand, because the sweep and
    // the shutdown path both consult it every tick.
    std::size_t in_doubt_ = 0;
    std::uint64_t prepared_ = 0;
    std::uint64_t prepare_refusals_ = 0;
    std::uint64_t decides_committed_ = 0;
    std::uint64_t decides_aborted_ = 0;
    std::uint64_t decide_refusals_ = 0;
    std::uint64_t left_in_doubt_at_stop_ = 0;

    // R6-5.
    std::uint64_t in_doubt_asks_ = 0;
    std::uint64_t in_doubt_resolved_committed_ = 0;
    std::uint64_t in_doubt_resolved_aborted_ = 0;
    std::uint64_t in_doubt_resolved_unknown_ = 0;
};

}  // namespace kds::server
