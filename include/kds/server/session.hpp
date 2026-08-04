#pragma once

#include <cstdint>
#include <string_view>

#include "kds/txn/manager.hpp"

// One client connection's transaction state (docs/txn.md sections 1, 5, 6).
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
        return ended;
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
    txn::Transaction* txn_ = nullptr;
};

}  // namespace kds::server
