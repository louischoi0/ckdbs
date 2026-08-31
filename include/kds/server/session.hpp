#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

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
    // next one's, exactly as `home_core_` and the participant list are not.
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
    // aggregator already documents (AG3): a statement can *park* - a
    // cross-core read, a shipped statement, a group commit's wait - and
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
        // The home core belongs to the transaction, not the connection: the
        // next one is free to write wherever it likes, and carrying the
        // binding forward would restrict it for no reason. The participant
        // list is the same fact one level out (R6-3): the cores this
        // transaction enrolled are not the next one's.
        home_core_ = kUnbound;
        // **And the shipping identity, where this transaction enrolled
        // anyone** (R6-8 review). A participant's transaction context is
        // keyed on `(coordinator core, session_id)` alone
        // (`ShippedStatementExecutor::DedupKey`) - the statement leg carries
        // no transaction id, so two consecutive transactions of one session
        // are indistinguishable there. With the id stable for the
        // connection's life, a *second* transaction's first shipped
        // statement joined the *first* transaction's context whenever that
        // context outlived its coordinator - which every `ROLLBACK` leaves
        // it doing, since nothing tells a participant about a transaction
        // that never reached prepare. The rolled-back writes then committed
        // with the next transaction's: a wrong answer, and the one this
        // clearing removes.
        //
        // Conditioned on `participants_`, so it costs exactly the sessions
        // that ran a cross-owner transaction: an autocommit session's id is
        // still minted once and kept for its life (SS2's dedup record is
        // per `(core, session)`, and the measured autocommit path is
        // untouched), and a one-owner transaction never enrolled anyone and
        // never had a participant context to leave behind. What it costs
        // the population it does apply to is one more dedup record per
        // cross-owner transaction on each participant - retention-bounded
        // at `kShippedDedupRetentionNs` and capped by
        // `kShippedDedupMaxRecords`, whose early eviction is counted.
        if (!participants_.empty()) ship_id_ = 0;
        participants_.clear();
        watermarks_.clear();
        // The class this transaction was begun under, for `home_core_`'s
        // reason: `BEGIN ... DURABILITY strict` binds one transaction, and
        // a session whose next statement is autocommit must fall back to
        // its own default rather than inherit a stricter class silently -
        // or, worse, a laxer one.
        txn_durability_.reset();
        return ended;
    }

    // ---- The transaction's home core (crosscore.md §6, CC3) ------------
    //
    // **A transaction's writes bind to one core.** The first write picks it;
    // any later write to a relation owned by a different core is refused,
    // retryably. That restriction is what keeps commit single-stream and the
    // 2PC door closed - LSNs are stream-local and are never compared across
    // cores (workplan guideline 3), so a transaction whose writes landed in
    // two streams could not be recovered as one.
    //
    // `kUnbound` until the first write, so a read-only transaction never
    // acquires a home and never restricts anything - reads pipeline freely
    // under §5.
    static constexpr std::uint32_t kUnbound = 0xFFFFFFFFu;

    std::uint32_t home_core() const noexcept { return home_core_; }
    bool home_bound() const noexcept { return home_core_ != kUnbound; }

    // Binds the home core on the first write. Idempotent for the core
    // already bound; a caller must check `MayWriteOn()` first rather than
    // rely on this to refuse, because the refusal is a client-visible error
    // with a message, not a silent no-op.
    void BindHomeCore(std::uint32_t core_id) noexcept {
        if (home_core_ == kUnbound) home_core_ = core_id;
    }

    // Whether a write to a relation owned by `core_id` is admissible.
    // Always true before the first write and for the bound core itself.
    bool MayWriteOn(std::uint32_t core_id) const noexcept {
        return home_core_ == kUnbound || home_core_ == core_id;
    }

    // ---- Statement shipping (SS2, server/statement_ship_service.hpp) ---
    //
    // **The identity a shipped statement carries.** The owner keeps the
    // last outcome per `(arrival core, session)` so a duplicate is answered
    // rather than run twice, which against engine-issued primary keys is
    // the difference between an answer and a second row. The id is minted
    // by the dispatcher on this session's first ship; the sequence counts
    // the statements this session has shipped.
    // Zero means "never shipped", which is why the dispatcher mints from 1.
    //
    // **Stable for the session's life, with one exception**: a transaction
    // that enrolled a participant drops it at `Finish()`, so the next
    // transaction ships under a fresh id. The reason is at that clearing -
    // the participant's context is keyed on this id and nothing else, so a
    // reused id lets one transaction inherit another's half.
    std::uint64_t ship_id() const noexcept { return ship_id_; }
    void set_ship_id(std::uint64_t id) noexcept { ship_id_ = id; }
    std::uint64_t NextShipSequence() noexcept { return ++ship_sequence_; }

    // **This session is running a statement that arrived shipped**, set by
    // the owner's executor on the session it mints (SS3). A shipped
    // statement never ships again: two cores whose catalogs disagree about
    // an owner would otherwise pass one statement back and forth, each hop
    // a fresh identity the dedup record cannot recognise, until a deadline
    // fired on every one of them. One hop, and the second core refuses as
    // it always did.
    bool shipped() const noexcept { return shipped_; }
    void mark_shipped() noexcept { shipped_ = true; }

    // ---- Cross-owner participants (R6-3, D1) ---------------------------
    //
    // **The cores this transaction has enrolled as participants**, in the
    // order it discovered them - D1's "participants are relation owners,
    // discovered as the transaction runs rather than declared up front". A
    // transaction becomes cross-owner at the moment its second owner is
    // touched, and this vector is empty until then, which is what makes the
    // fast path a test on `empty()` rather than a lookup.
    //
    // Not the home core, and not a replacement for it: `home_core_` is
    // where *this* core's half of the transaction lives, and these are the
    // other cores' halves. A one-owner transaction has a home and no
    // participants, and takes the single-core path unchanged.
    //
    // Cleared by `Finish()` with the transaction, for `home_core_`'s
    // reason: an enrolment belongs to the transaction that opened it, and a
    // list carried into the next one would prepare cores that transaction
    // never touched.
    const std::vector<std::uint32_t>& participants() const noexcept { return participants_; }
    bool has_participants() const noexcept { return !participants_.empty(); }

    // Idempotent: a transaction that ships four statements to one owner has
    // one participant, and prepares it once.
    void EnrolParticipant(std::uint32_t core_id) {
        if (!HasParticipant(core_id)) participants_.push_back(core_id);
    }

    // **Whether this transaction has already shipped a statement to
    // `core_id`** (RR0). What the wire's `join` bit is: true means the
    // participant must already hold a context and may not open a second
    // one. Read before `EnrolParticipant` records the ship, so the
    // enrolling statement itself answers false.
    bool HasParticipant(std::uint32_t core_id) const noexcept {
        for (std::uint32_t core : participants_) {
            if (core == core_id) return true;
        }
        return false;
    }

    // ---- RR0 / D3: the per-participant watermark ------------------------
    //
    // **What the coordinator carries.** One entry per participant that has
    // reported a watermark, which under D3's ratified `[OPEN]` is the
    // REPEATABLE READ participants and no others: an RC cross-owner
    // transaction carries none, so this vector stays empty and the whole
    // mechanism costs the default level nothing.
    //
    // The value is that core's `ReadView::up_to_trx_id`, and it is never
    // compared with this core's or with another participant's. **The reason
    // is not that the id spaces differ** - there is one instance-wide
    // sequence, leased per core (`txn/trx_id_lease.hpp`) - it is that the
    // quantity is a high-water mark over the ids *that core* has issued,
    // plus its own in-flight set. Two of them are two cores' answers to
    // "what had I handed out", so ordering them numerically orders nothing,
    // and that is why D3's promise is a consistent snapshot *per core*
    // rather than a global instant. The only comparison it takes part in is
    // with itself, one reply later.
    //
    // Cleared with the transaction by `Finish()` above, for
    // `participants_`'s reason: a watermark belongs to the transaction that
    // observed it.
    //
    // Answers `true` when `watermark` is the value already held for
    // `core_id`, or when nothing was held and it is now recorded. `false`
    // means this participant's snapshot **moved under a transaction that
    // was promised it would not** - which the caller turns into a refusal,
    // because the two halves of the transaction have then read different
    // states of one core and RR was not delivered.
    bool NoteParticipantWatermark(std::uint32_t core_id, std::uint64_t watermark) {
        for (auto& [core, held] : watermarks_) {
            if (core != core_id) continue;
            return held == watermark;
        }
        watermarks_.emplace_back(core_id, watermark);
        return true;
    }

    // The watermark held for `core_id`, or 0 where none is - for tests and
    // for anything that reports the transaction's shape.
    std::uint64_t ParticipantWatermark(std::uint32_t core_id) const noexcept {
        for (const auto& [core, held] : watermarks_) {
            if (core == core_id) return held;
        }
        return 0;
    }

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
    std::uint32_t home_core_ = kUnbound;
    std::uint64_t ship_id_ = 0;
    std::uint64_t ship_sequence_ = 0;
    bool shipped_ = false;
    // R6-3. A vector rather than a set: the count is bounded by the core
    // count, which is small, and the discovery order is worth keeping - it
    // is the order the prepare messages go out in and the order a log line
    // names them in.
    std::vector<std::uint32_t> participants_;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> watermarks_;
};

}  // namespace kds::server
