#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/exec/functions.hpp"
#include "kds/exec/set_expr.hpp"
#include "kds/server/priority.hpp"
#include "kds/server/result_sink.hpp"
#include "kds/server/role.hpp"
#include "kds/txn/manager.hpp"

// One client connection's transaction state (docs/spec/txn.md sections 1, 5, 6).
//
// ---- Why this type has to exist ------------------------------------------
//
// One dispatcher serves every connection on a core, so "this connection is
// inside a transaction" lives here: which read view a statement takes,
// whether a write joins an open transaction or commits on its own, and
// whether a failed statement poisons what follows.
//
// A Session is owned by the connection and outlives no statement boundary
// it should not. It holds a `txn::Transaction*` borrowed from the manager,
// which is why `Finish()` exists: the manager keeps an ended transaction
// standing until the holder drops it, so the reply can still name its id.
//
// ---- The state machine (section 10-8) ------------------------------------
//
//   kIdle       autocommit. Every statement is its own transaction.
//   kInTxn      an explicit transaction is open (BEGIN was accepted).
//   kFailedTxn  a statement inside an explicit transaction failed. Only
//               ROLLBACK / ABORT / SYNC / STOP / PING are admitted until
//               the client rolls back.
//
// **kFailedTxn has two shapes.** Poisoned by a failed statement, it still
// holds its transaction, and the client's ROLLBACK is what undoes it.
// Expired by the lifetime sweep (`Expire`, AN-R14, docs/spec/txn.md §1),
// the server has already rolled it back and holds nothing; the state stays
// failed so the client learns it at its next statement and its ROLLBACK
// ends nothing but the state. One wire state for both (`S_READY.txn_state`
// is this enum's numbering), because the client's next move is the same.
//
// **Failure atomicity is per transaction, not per statement** (section 6).
// An UPDATE that fails on row 7 of 10 inside an explicit transaction leaves
// rows 1-6 written and the session in kFailedTxn; the client must ROLLBACK,
// which undoes all six. In autocommit the abort is automatic, so behaviour
// is statement-atomic there. That deviates from SQL, which needs savepoints
// or a statement-level trail high-water mark - a non-goal the trail's shape
// supports additively.
//
// ---- Concurrency ----------------------------------------------------------
//
// One connection's, touched by one core at a time: a session with no
// transaction open may move whole to another core at a transaction's head
// (BL-S3, `load_placer.hpp`), and a transaction never crosses. Two sessions
// never touch each other's state - they interact only through the
// manager's live set, which is what makes their read views differ.

namespace kds::server {

// Where a write walk stopped, so the same walk can be resumed after a park
// (AO-S3b). `active` false is "from the head", which is what an unset
// cursor means and what every first walk carries.
//
// **A walk resumes by key, and `tuple_key` is that key.** A clustered btree
// leaf splits by moving its upper half to a new right sibling, so a split
// whose midpoint fell below a remembered `(page, slot)` would carry rows this
// walk had already written into a page it had yet to visit, and it would
// write them twice. The resume **descends afresh** to whichever leaf now
// holds `tuple_key` and skips every key below it, so wherever a split moved
// a row, one this walk has finished sorts below `tuple_key` and one it has
// not sorts at or above. A row another session inserted while this one was
// parked is invisible to its snapshot, minted before the park, so skipping
// it changes no answer.
//
// Lives here rather than on `CommandDispatcher` because the state it
// positions is the *session's*: two sessions on one reactor can be parked
// at once (that is what a deadlock cell needs), so a dispatcher member
// would be one cursor shared by both.
struct WalkCursor {
    // The key the walk stopped **at** - the row it is waiting for, which it
    // has not written - so a resume offers that same key again rather than
    // stepping past it.
    std::uint64_t tuple_key = 0;
    bool active = false;
    // **How many rows the statement has written so far**, carried because
    // the count a client is told is the *statement's* and the resume is a
    // fresh call with a fresh counter. Without it a ten-row UPDATE that
    // parked at row 7 answers `UPDATED 4` - the rows the resume wrote -
    // which is a wrong answer rather than a slow one. Not touched by the
    // walk itself; the caller that owns the counter seeds it and writes it
    // back.
    std::uint32_t rows_done = 0;
    // **The rows a write collected before its first write**, by tuple key:
    // set when its WHERE holds a subquery, so the WHERE is evaluated once,
    // under no write hold, over the relation as it stood and never over the
    // statement's own writes. Carried so a resume writes exactly
    // these rows rather than re-evaluating over what it already wrote.
    std::shared_ptr<const std::unordered_set<std::uint64_t>> targets;
};

class Session {
public:
    enum class State : std::uint8_t {
        kIdle = 0,
        kInTxn = 1,
        kFailedTxn = 2,
    };

    explicit Session(txn::IsolationLevel default_isolation =
                         txn::IsolationLevel::kReadCommitted) noexcept
        : isolation_(default_isolation) {}

    State state() const noexcept { return state_; }
    bool in_explicit_txn() const noexcept { return state_ != State::kIdle; }
    bool failed() const noexcept { return state_ == State::kFailedTxn; }
    // The id of the transaction the lifetime sweep rolled back, or 0 when
    // the session was not expired. Non-zero only in kFailedTxn, with no
    // transaction held.
    std::uint64_t expired_trx_id() const noexcept { return expired_trx_id_; }

    // The level the *next* BEGIN will use. Set from the server config at
    // construction and by `SET ISOLATION LEVEL`; a `BEGIN ISOLATION LEVEL`
    // overrides it for that transaction only, which is the same three-level
    // precedence chain `durability` uses.
    txn::IsolationLevel isolation() const noexcept { return isolation_; }
    void set_isolation(txn::IsolationLevel level) noexcept { isolation_ = level; }

    // ---- The namespace an unqualified relation name means (`USE`) -------
    //
    // namespace.md NS8: `public` until a `USE <namespace>`, which sets it
    // for every later statement of the connection. Session state, not
    // transactional: a `ROLLBACK` does not undo a `USE`. A namespace
    // dropped after the `USE` leaves the session in an oid that resolves
    // nothing, and every unqualified name answers NotFound until the next.
    catalog::Oid default_namespace() const noexcept { return default_namespace_; }
    void set_default_namespace(catalog::Oid oid) noexcept { default_namespace_ = oid; }

    // ---- Durability, the same three rungs (docs/spec/protocol.md §9) ----
    //
    // **The chain `isolation`'s comment above already names**, now with the
    // rung it was describing: server config (the dispatcher's own
    // `durability_`), then `SET DURABILITY` for this connection, then
    // `BEGIN ... DURABILITY <class>` - or KWP's `C_TXN_BEGIN{durability}`,
    // which is the same rung reached over a frame - for one transaction.
    //
    // Two optionals rather than two values, because absence is what "take
    // the rung below" means: a session that never issued `SET DURABILITY`
    // must follow the server's setting as it changes, not a copy of it
    // taken when the connection opened.
    //
    // `txn_durability_` is cleared by `Finish()` with everything else the
    // transaction owned: a class chosen for one transaction is not the
    // next one's.
    std::optional<wal::DurabilityClass> durability() const noexcept { return durability_; }
    void set_durability(wal::DurabilityClass durability) noexcept { durability_ = durability; }

    std::optional<wal::DurabilityClass> txn_durability() const noexcept {
        return txn_durability_;
    }
    void set_txn_durability(wal::DurabilityClass durability) noexcept {
        txn_durability_ = durability;
    }

    // What this session's *next* commit is owed, given a server default.
    // One function so no call site re-derives the precedence - the failure
    // that would produce is a commit acked under a class the client did
    // not ask for, which no test of either rung alone would catch.
    wal::DurabilityClass EffectiveDurability(wal::DurabilityClass server_default) const noexcept {
        if (state_ != State::kIdle && txn_durability_.has_value()) return *txn_durability_;
        if (durability_.has_value()) return *durability_;
        return server_default;
    }

    // **The acknowledged-commit bound** (BA-R1c, `instance_visibility.hpp`'s
    // note): this session's last commit LSN until a snapshot covers it, then
    // `wal::kNoLsn` again. Survives `Finish()`: the next statement reads it.
    wal::Lsn acknowledged_commit_lsn() const noexcept { return acknowledged_commit_lsn_; }
    void set_acknowledged_commit_lsn(wal::Lsn lsn) noexcept { acknowledged_commit_lsn_ = lsn; }

    // ---- Where this connection's result rows go (result_sink.hpp) ------
    //
    // Null - the default, and every caller that predates KWP - means the
    // newline protocol's rendering, written into the reply the statement
    // returns. `KwpSession` installs one for the statement it is running
    // and clears it afterwards.
    //
    // **On the session and not on the dispatcher**, which was the first
    // shape and was wrong for a reason the dispatcher's own hoisted
    // aggregator already documents (AG3): a statement can *park* - a lock
    // wait, a group commit's wait - and
    // while it is parked the reactor runs another connection's statement on
    // the same core and the same dispatcher. A per-dispatcher pointer would
    // then be the other connection's by the time the parked one resumed,
    // and its rows would be encoded into a stranger's session. One sink per
    // connection cannot be clobbered by another connection.
    ResultSink* result_sink() const noexcept { return result_sink_; }
    void set_result_sink(ResultSink* sink) noexcept { result_sink_ = sink; }

    // **§10's cancel flag** (docs/spec/protocol.md, BA-S15). Set from any
    // core - a cancel connection's, through `CancelRegistry`, or this
    // connection's own teardown - and taken by whoever observes it first:
    // a `SELECT`'s walk at its slice boundary, or the KWP session at its
    // next frame. Taken, so one cancel cancels one statement. Shared so a
    // cancel racing this session's close sets a flag that is still there.
    const std::shared_ptr<std::atomic<bool>>& cancel_flag() const noexcept { return cancel_; }
    void RequestCancel() noexcept { cancel_->store(true, std::memory_order_release); }
    bool TakeCancel() noexcept { return cancel_->exchange(false, std::memory_order_acq_rel); }

    // What this connection may do (role.hpp), checked once per statement
    // by the dispatcher. **kAdmin by default, and that is the auth-off
    // contract**: an unauthenticated instance is the operator's own
    // process, and every in-process construction (tests, tools) predates
    // roles. The auth gate stamps the real role at the moment its
    // exchange succeeds (tcp_server.cpp), which is the only code that
    // ever learns one.
    Role role() const noexcept { return role_; }
    void set_role(Role role) noexcept { role_ = role; }

    // The open transaction, or null in autocommit. Borrowed from the
    // manager, never owned.
    txn::Transaction* transaction() const noexcept { return txn_; }

    // Called by BEGIN once the manager has started one.
    void Adopt(txn::Transaction* txn) noexcept {
        txn_ = txn;
        state_ = State::kInTxn;
    }

    // ---- A write statement parked in the middle of its walk (AO-S3b) ----
    //
    // Everything the resume needs that the coroutine above it does not
    // have: the scope it must **keep open** (the rows already written live
    // in that transaction and nothing may unwind them), the snapshot the
    // first run minted, and where the walk stopped.
    //
    // **The scope is the reason this exists.** Every other park in the
    // dispatcher re-runs the statement whole; this one cannot, because a
    // statement that has written rows is not re-runnable - there are no
    // savepoints, so an explicit transaction's rows cannot be rolled back
    // to a statement boundary.
    struct ParkedWrite {
        txn::Transaction* txn = nullptr;
        // The scope owned the transaction (autocommit). Carried because
        // `session.transaction()` is null on that arm and the resume's
        // `EndWrite` must still commit what it opened.
        bool owned = false;
        txn::Snapshot snapshot{};
        WalkCursor cursor{};
        // R6-5's `statement_trail_mark_`, which is a property of the
        // statement and so must survive its park: another statement runs
        // on this core while this one waits, and the mark is a dispatcher
        // member.
        std::size_t trail_mark = 0;
        // Which handler to re-enter. The statement text is the coroutine's
        // and is re-parsed on the resume, so this is only the fork.
        bool is_delete = false;
        // The statement's constants, for the reason the snapshot is carried:
        // `NOW()` is one instant for the whole statement, and the resume
        // compiles it again (AP-S4).
        exec::StatementContext context{};
        // The answers of the statement's scalar subqueries (BJ-S7), in the order
        // they were run. A subquery is run once, before the walk; the resume
        // compiles the statement again and must read what the first run did,
        // or the rows after the park would be set from another state of the
        // database than the rows before it.
        std::vector<exec::SubqueryResult> subqueries{};
    };

    const std::optional<ParkedWrite>& parked_write() const noexcept { return parked_write_; }
    void set_parked_write(ParkedWrite parked) noexcept { parked_write_ = std::move(parked); }
    void clear_parked_write() noexcept { parked_write_.reset(); }

    // A statement inside an explicit transaction failed. In autocommit this
    // is not called: there is no transaction to poison, and the statement's
    // own abort already happened.
    void Poison() noexcept {
        if (state_ == State::kInTxn) state_ = State::kFailedTxn;
    }

    // The transaction ended (committed or aborted). Returns the handle so
    // the caller can Release() it against the manager after replying.
    txn::Transaction* Finish() noexcept {
        txn::Transaction* ended = txn_;
        txn_ = nullptr;
        state_ = State::kIdle;
        expired_trx_id_ = 0;
        // The class this transaction was begun under belongs to the
        // transaction, not the connection: `BEGIN ... DURABILITY strict`
        // binds one transaction, and a session whose next statement is
        // autocommit must fall back to
        // its own default rather than inherit a stricter class silently -
        // or, worse, a laxer one.
        txn_durability_.reset();
        return ended;
    }

    // The lifetime sweep rolled the transaction back
    // (`TcpServer::ExpireOverdueTransactions`). Failed, not idle: an idle
    // session would run the client's next statement in autocommit, and the
    // writes it believes pending would be silently missing from its COMMIT.
    void Expire(std::uint64_t trx_id) noexcept {
        txn_ = nullptr;
        state_ = State::kFailedTxn;
        expired_trx_id_ = trx_id;
        txn_durability_.reset();
    }

    // The home core, the ship identity and the participant list retired
    // (AT-S6, AT-S9); their history is at cf889b63. A transaction is one
    // core's, whole.

    // **When BL's balancer last moved this session** (BL-R4's dwell,
    // `load_placer.hpp`), on the instance's clock; 0 for never. Kept on the
    // session because the session is what travels and the placer is every
    // core's.
    std::uint64_t moved_at_ns() const noexcept { return moved_at_ns_; }
    void set_moved_at_ns(std::uint64_t ns) noexcept { moved_at_ns_ = ns; }

    // **The level this session declared** (`SET PRIORITY`, `priority.hpp`),
    // `normal` until it declares one; read at its next head by the placer
    // and by each walk for its slice.
    Priority priority() const noexcept { return priority_.level(); }
    void set_priority(Priority p) noexcept { priority_.set(p); }
    // The load window this session was last counted active in (its core's
    // `window_end_ns` then, `sched/core_load.hpp`), so a session is counted
    // once a window however many statements it runs.
    std::uint64_t counted_in_window() const noexcept { return counted_in_window_; }
    void set_counted_in_window(std::uint64_t w) noexcept { counted_in_window_ = w; }

    // Which commands a poisoned session still answers (section 10-8).
    // Deliberately a whitelist rather than a blacklist: a new statement
    // must be refused inside a failed transaction by default, and admitting
    // it should be a decision someone made.
    static bool AdmittedWhileFailed(std::string_view cmd) noexcept {
        return IEqualsAscii(cmd, "ROLLBACK") || IEqualsAscii(cmd, "ABORT") ||
               IEqualsAscii(cmd, "SYNC") || IEqualsAscii(cmd, "STOP") ||
               IEqualsAscii(cmd, "PING");
    }

private:
    // A local fold rather than the dispatcher's: this header must not
    // depend on the dispatcher, which depends on it.
    static bool IEqualsAscii(std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            char x = a[i];
            char y = b[i];
            if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 'a' + 'A');
            if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 'a' + 'A');
            if (x != y) return false;
        }
        return true;
    }

    State state_ = State::kIdle;
    txn::IsolationLevel isolation_ = txn::IsolationLevel::kReadCommitted;
    catalog::Oid default_namespace_ = catalog::kNamespacePublic;
    ResultSink* result_sink_ = nullptr;
    std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
    std::optional<wal::DurabilityClass> durability_;      // SET DURABILITY
    std::optional<wal::DurabilityClass> txn_durability_;  // BEGIN ... DURABILITY
    wal::Lsn acknowledged_commit_lsn_ = wal::kNoLsn;      // BA-R1c
    Role role_ = Role::kAdmin;
    txn::Transaction* txn_ = nullptr;
    std::uint64_t expired_trx_id_ = 0;  // AN-R14's sweep; see `Expire`
    // AO-S3b. Absent on every session that is not parked mid-walk, which
    // is every session almost all of the time.
    std::optional<ParkedWrite> parked_write_ = std::nullopt;
    std::uint64_t moved_at_ns_ = 0;
    std::uint64_t counted_in_window_ = UINT64_MAX;
    PrioritySeat priority_;
};

}  // namespace kds::server
