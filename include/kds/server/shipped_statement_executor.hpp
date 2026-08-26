#pragma once

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/server/statement_ship_service.hpp"

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
// (`early_evictions()`), because an early eviction is the one condition
// under which a duplicate could reach an empty record and be re-executed.
// A run with `early_evictions() == 0` had no such window at all.

namespace kds::server {

// How long an answered statement's outcome is kept, for a duplicate to be
// answered from. Twice the deadline, per the argument above.
inline constexpr sched::MonoTimeNs kShippedDedupRetentionNs = 2 * kShippedStatementDeadlineNs;

// The memory bound under the time bound. 4096 records is one for every
// session that shipped a statement in the last twenty seconds; a core
// serving more concurrent shipping sessions than that evicts early and
// says so.
inline constexpr std::size_t kShippedDedupMaxRecords = 4096;

class ShippedStatementExecutor {
public:
    // `dispatcher` is this core's own - the one a local connection would
    // use. The executor must not outlive it, nor the scheduler it submits
    // to, nor the `StatementShipServer` whose `ReplyFn` its running
    // statements hold: **declare it after all three**, so it is destroyed
    // first.
    ShippedStatementExecutor(std::uint32_t core_id, CommandDispatcher& dispatcher,
                             sched::Scheduler& scheduler, const sched::Clock& clock,
                             Logger* log = nullptr) noexcept
        : core_id_(core_id),
          dispatcher_(dispatcher),
          scheduler_(scheduler),
          clock_(clock),
          log_(log) {}

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

    // Statements this core ran on another core's behalf, and finished.
    std::uint64_t executed() const noexcept { return executed_; }
    // Duplicates answered from the record instead of run again (D4).
    std::uint64_t deduped() const noexcept { return deduped_; }
    // Duplicates this core could not answer for - superseded by a later
    // sequence, or still running here so that no outcome exists yet. Each
    // one is a client told `UNKNOWN_OUTCOME`.
    std::uint64_t unanswerable() const noexcept { return unanswerable_; }
    // Records dropped by the memory bound before their retention expired -
    // the only condition under which a duplicate could be re-executed.
    std::uint64_t early_evictions() const noexcept { return early_evictions_; }
    // Statements running right now: the population the owner's reactor is
    // carrying on other cores' behalf.
    std::size_t running() const noexcept { return running_.size(); }
    // Outcomes the record holds - the number `kShippedDedupMaxRecords`
    // bounds. Read off the order list rather than the map so that the two
    // going out of step is visible from outside, since it is the list that
    // would grow with the shipping rate if a key ever took a second node.
    std::size_t records() const noexcept { return answered_order_.size(); }

private:
    // What one shipped statement holds while it runs. Heap-allocated and
    // stable: `DispatchAsync` borrows `text` as a `string_view` and writes
    // `out` when it finishes, both across every park it takes.
    struct Running {
        std::string text;
        Session session;
        DispatchOutcome out;
        StatementShipServer::ReplyFn reply;
        // The identity's third component. The first two are the map key.
        std::uint64_t sequence = 0;

        Running(std::string statement, txn::IsolationLevel isolation, Role role)
            : text(std::move(statement)), session(isolation) {
            session.set_role(role);
        }
    };

    using DedupKey = std::pair<std::uint32_t, std::uint64_t>;  // (requester, session id)

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

    std::uint32_t core_id_;
    CommandDispatcher& dispatcher_;
    sched::Scheduler& scheduler_;
    const sched::Clock& clock_;
    Logger* log_;

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

    std::uint64_t executed_ = 0;
    std::uint64_t deduped_ = 0;
    std::uint64_t unanswerable_ = 0;
    std::uint64_t early_evictions_ = 0;
};

}  // namespace kds::server
