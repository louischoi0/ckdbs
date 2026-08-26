#pragma once

#include <cstdint>
#include <string_view>

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
        // binding forward would restrict it for no reason.
        home_core_ = kUnbound;
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
    // by the dispatcher on this session's first ship and is stable for its
    // life; the sequence counts the statements this session has shipped.
    // Zero means "never shipped", which is why the dispatcher mints from 1.
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
    Role role_ = Role::kAdmin;
    txn::Transaction* txn_ = nullptr;
    std::uint32_t home_core_ = kUnbound;
    std::uint64_t ship_id_ = 0;
    std::uint64_t ship_sequence_ = 0;
    bool shipped_ = false;
};

}  // namespace kds::server
