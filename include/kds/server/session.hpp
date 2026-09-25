#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/server/result_sink.hpp"
#include "kds/server/role.hpp"
#include "kds/txn/manager.hpp"

// One client connection's transaction state (docs/spec/txn.md sections 1, 5, 6).
//
// ---- Why this type has to exist ------------------------------------------
//
// `CommandDispatcher::Dispatch()` was stateless and `TcpServer` shares one
// dispatcher across every connection, so there was nowhere for "this
// connection is inside a transaction" to live. Everything transactional
// needs that: which read view a statement takes, whether a write joins an
// open transaction or commits on its own, and whether a failed statement
// poisons what follows.
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
// Core-local. A session belongs to one connection on one core, and two
// sessions never touch each other's state - they interact only through the
// manager's live set, which is what makes their read views differ.

namespace kds::server {

// Where a write walk stopped, so the same walk can be resumed after a park
// (AO-S3b). `active` false is "from the head", which is what an unset
// cursor means and what every first walk carries.
//
// **Slots, not accepted rows** - the opposite of `exec::WalkMark`, and for
// a reason that only a *write* walk has. A mark counts rows the walk
// accepted, which is stable for a SELECT because nothing it does changes
// whether a row matches; an UPDATE changes exactly that (`SET v = 1 WHERE
// v = 0` unmatches every row it writes), so an accepted-row ordinal would
// resume in the wrong place the moment the statement's own writes are
// counted again. `ChainVisitOnePage` walks `0..slot_count()` in slot
// order, so the slot is the position.
//
// **Why skipping is sound across a park.** Resuming at
// `(range, page, slot)` skips every earlier range, every earlier page of
// this chain, and every lower slot of this page. A row can appear in one
// of those places while this statement is parked - another session on this
// core may insert - but such a row is invisible to this statement's
// snapshot, which was minted before the park, so skipping it changes no
// answer. Heap pages append slots and a heap chain never loses a page, so
// nothing this walk already passed moves to a position it has yet to reach.
//
// **`range` is a positional index, and that is an assumption, not a fact.**
// It indexes `ResolveRanges(access.ranges, span)`'s output, and the resume
// re-resolves that list against a freshly re-read catalog - so it names the
// same chain only while no range with a lower `lo` can appear during the
// park. Nothing splits or merges a range (spreading retired at AT-S9 and
// nothing opens one), so the index is stable and this is unreachable. It is written down
// rather than relied on silently: the day a range can open under a parked
// statement, the cursor must carry the range's `lo` and look it up on
// resume, or the walk resumes into a different chain.
//
// **A btree resumes by key instead, and `pk` is that key.** A clustered
// btree leaf splits by moving its upper half to a new right sibling, so a
// split whose midpoint fell below a `(page, slot)` cursor would carry rows
// this walk had already written into a page it had yet to visit, and it
// would write them twice. The key ordering is what removes the hazard: the
// resume **descends afresh** to whichever leaf now holds `pk` and skips
// every key below it, so wherever a split moved a row, one this walk has
// finished sorts below `pk` and one it has not sorts at or above. That
// makes the btree arm immune to concurrent structure change rather than
// merely unlikely to meet it.
//
// Lives here rather than on `CommandDispatcher` because the state it
// positions is the *session's*: two sessions on one reactor can be parked
// at once (that is what a deadlock cell needs), so a dispatcher member
// would be one cursor shared by both.
struct WalkCursor {
    std::size_t range = 0;
    PageId page = kInvalidPageId;
    std::uint16_t slot = 0;
    // The key the walk stopped **at** - the row it is waiting for, which it
    // has not written - so a resume offers that same key again rather than
    // stepping past it. Btree relations only: a heap page is unordered
    // (invariant 4), so there is no key to resume a heap walk from and the
    // position above is what it carries.
    std::uint64_t pk = 0;
    bool active = false;
    // **How many rows the statement has written so far**, carried because
    // the count a client is told is the *statement's* and the resume is a
    // fresh call with a fresh counter. Without it a ten-row UPDATE that
    // parked at row 7 answers `UPDATED 4` - the rows the resume wrote -
    // which is a wrong answer rather than a slow one. Not touched by the
    // walk itself; the caller that owns the counter seeds it and writes it
    // back.
    std::uint32_t rows_done = 0;
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

    // The level the *next* BEGIN will use. Set from the server config at
    // construction and by `SET ISOLATION LEVEL`; a `BEGIN ISOLATION LEVEL`
    // overrides it for that transaction only, which is the same three-level
    // precedence chain `durability` uses.
    txn::IsolationLevel isolation() const noexcept { return isolation_; }
    void set_isolation(txn::IsolationLevel level) noexcept { isolation_ = level; }

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

    // ---- AO-S3b: a write statement parked in the middle of its walk -----
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
    // to a statement boundary. (`AbandonWriteForShipping` was the other
    // ending - a scope closed without committing for a statement that
    // would run elsewhere - and it went with the ship at AT-S6.)
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
        // The class this transaction was begun under belongs to the
        // transaction, not the connection: `BEGIN ... DURABILITY strict`
        // binds one transaction, and a session whose next statement is
        // autocommit must fall back to
        // its own default rather than inherit a stricter class silently -
        // or, worse, a laxer one.
        txn_durability_.reset();
        return ended;
    }

    // **No home core since AT-S9.** A transaction's writes bound to the
    // core of its first write, and a write to a relation owned elsewhere was
    // refused (CC3); a transaction is one core's whole since AT-S6 and no
    // relation is owned since AT-S9, so there is nothing to bind or refuse.

    // **Statement shipping's identity and the participant list stood here
    // until AT-S6**, and went with the protocols: `ship_id_` and its
    // sequence keyed a shipped statement's dedup record and a
    // participant's context; `shipped_` kept a statement that arrived
    // shipped from shipping on; `participants_` was what a `COMMIT`
    // prepared and decided. No statement crosses, so a session is one
    // core's and holds none of it (`docs/spec/cross-owner-txn.md`).

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
    ResultSink* result_sink_ = nullptr;
    std::optional<wal::DurabilityClass> durability_;      // SET DURABILITY
    std::optional<wal::DurabilityClass> txn_durability_;  // BEGIN ... DURABILITY
    Role role_ = Role::kAdmin;
    txn::Transaction* txn_ = nullptr;
    // AO-S3b. Absent on every session that is not parked mid-walk, which
    // is every session almost all of the time.
    std::optional<ParkedWrite> parked_write_ = std::nullopt;
};

}  // namespace kds::server
