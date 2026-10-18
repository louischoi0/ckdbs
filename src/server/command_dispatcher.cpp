#include "kds/server/command_dispatcher.hpp"
#include "kds/server/backup.hpp"
#include "kds/server/wal_archive.hpp"
#include "kds/server/page_reclaim.hpp"
#include "kds/server/statement_epoch.hpp"
#include "kds/server/read_borrow.hpp"

#include "kds/txn/lock_table.hpp"

#include "kds/sched/checkpoint_census.hpp"
#include "kds/sched/core_load.hpp"
#include "kds/base/contention.hpp"
#include "kds/base/current_core.hpp"

#include "kds/exec/type_literals.hpp"
#include "kds/storage/anchor_page.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/tagged_cell.hpp"  // varchar(N)'s bounds are the cell width's
#include "kds/server/mount_recovery.hpp"  // SHOW META's recovery block (RC09)
#include "kds/sched/scheduler.hpp"       // SHOW META's group accounting (sched.md 4)

#include "kds/stats/optimizer_signals.hpp"
#include "kds/stats/trail_store.hpp"
#include "kds/stats/statement_stats.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>
#include <charconv>
#include <ctime>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <variant>

#include <vector>

#include "kds/catalog/foreign_key.hpp"
#include "kds/catalog/keystone_budget.hpp"
#include "kds/exec/query_layer.hpp"
#include "kds/exec/catalog_view.hpp"
#include "kds/exec/chain_frame.hpp"
#include "kds/exec/fk_check.hpp"
#include "kds/exec/assertion_catalog.hpp"
#include "kds/exec/transition.hpp"
#include "kds/exec/index_ddl.hpp"
#include "kds/exec/index_maintain.hpp"
#include "kds/exec/pagination.hpp"
#include "kds/exec/purge_key.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/step_compiler.hpp"
#include "kds/exec/step_vm.hpp"
#include "kds/exec/tuple_verify.hpp"
#include "kds/wal/log_page_init.hpp"
#include "kds/exec/wal_row_log.hpp"
#include "kds/storage/log_page_image.hpp"
#include "kds/storage/index/index_tree.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/parser/lexer.hpp"
#include "kds/parser/parser.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/wal/payload.hpp"

namespace kds::server {

// `SelectRun`'s, defined beside it below: `DispatchAndStage` stamps the head's
// start on a walk it hands on, so the walk's log line times the statement.
void SetSelectRunStart(SelectRun& run, sched::MonoTimeNs started_ns) noexcept;

namespace {

// The statement's constants (`exec/functions.hpp`) for a statement the
// session runs: `NOW()`'s instant and the namespace an unqualified relation
// name means (`USE`, namespace.md NS8).
exec::StatementContext StatementContextFor(const Session& session) {
    exec::StatementContext context = exec::StatementContextNow();
    context.default_namespace = session.default_namespace();
    return context;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// Who a waiter is held by, in words (AO-S6e-b). A read borrow holds under
// an id from a space of its own (`txn::kReadHolderBit`) and is no
// transaction.
std::string HolderName(std::uint64_t holder) {
    return txn::IsReadHolder(holder) ? std::string("a positioned reader")
                                     : "transaction " + std::to_string(holder);
}

// A relation refused at the relation unit, in the one spelling every such
// refusal takes - a DDL's `X` meeting a writer or a reader, a writer's `IX`
// meeting a DDL.
Status RelationHeld(catalog::Oid oid, std::uint64_t holder) {
    return Status::TxnConflict("relation oid " + std::to_string(oid) + " is held by " +
                               HolderName(holder));
}

bool IEquals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// True when a written column name resolves to the tuple key column (position
// 0) and to nothing else, under the compiler's rule
// (`catalog::Schema::FindColumn`). A name that also matches another live
// column ignoring case - possible only on a relation created before names
// were unique ignoring case - is not the tuple key's: the fast paths decline
// it and the compiler - or, for PURGE, which has none, `PurgeConjunctRefusal`
// - refuses it as ambiguous, rather than probing the key
// for a name the statement may have meant as another column.
bool NamesTupleKey(const catalog::Schema& schema, std::string_view name) {
    auto found = schema.FindColumn(name, 0);
    return found.ok() && found.value() != nullptr && found.value()->pos == 0;
}

// Splits `line` into the first whitespace-delimited token and "the rest"
// (trimmed), so multi-word commands like "SHOW META"/"FIND TABLE x" can
// be matched one token at a time without pulling in a real tokenizer.
std::pair<std::string_view, std::string_view> SplitFirstToken(std::string_view line) {
    line = Trim(line);
    std::size_t sp = line.find_first_of(" \t");
    if (sp == std::string_view::npos) return {line, std::string_view{}};
    return {line.substr(0, sp), Trim(line.substr(sp + 1))};
}

// `COMMIT`, `ROLLBACK` and `ABORT` end the whole transaction and take one
// optional noise word, `TRANSACTION` or `WORK`, and nothing else
// (docs/spec/txn.md §9); `SAVEPOINT` and `RELEASE` name nothing that exists.
// `word` is the refused word, a view into `line`, and `what` the refusal.
Status RefuseWhole(std::string_view line, std::string_view word, const std::string& what) {
    return Status::Unsupported(what + " (byte " + std::to_string(word.data() - line.data()) +
                               "): there are no savepoints or chained transactions, and a "
                               "transaction commits or rolls back whole (docs/spec/txn.md §9)");
}

// The refusal of any word after an ending's noise word, or OK.
//
// **A refused rollback form poisons an open transaction.** The client asked
// to undo part of its work; left open, a following `COMMIT` would make that
// work durable. Poisoned, only `ROLLBACK` ends it. A refused `COMMIT` form
// leaves the transaction as it was: nothing the client meant to undo is at
// stake, and a plain `COMMIT` is still the client's to send.
Status RefuseTxnEndTail(std::string_view line, std::string_view cmd, std::string_view rest,
                        Session& session) {
    std::string_view word = SplitFirstToken(rest).first;
    if (IEquals(word, "TRANSACTION") || IEquals(word, "WORK")) {
        word = SplitFirstToken(SplitFirstToken(rest).second).first;
    }
    if (word.empty()) return Status::OK();
    const Status s = RefuseWhole(line, word, std::string(cmd) + " takes no '" +
                                                 std::string(word) + "'");
    if (IEquals(cmd, "COMMIT") || !session.in_explicit_txn()) return s;
    if (session.expired_trx_id() != 0) {
        // The lifetime sweep already rolled it back (`Session::Expire`).
        return s.WithMessage(s.message() +
                             "; the server already rolled this transaction back at "
                             "txn_lifetime_ceiling_ms, and ROLLBACK ends it");
    }
    session.Poison();
    return s.WithMessage(s.message() +
                         "; nothing was rolled back, and the transaction is failed until ROLLBACK");
}

// Splits `line` at the first token equal to `keyword`, answering
// `{before, from-the-keyword-on}`. `{line, ""}` where the keyword does not
// appear.
//
// What `SplitFirstToken` cannot do: a clause whose value spans several
// words (`ISOLATION LEVEL repeatable read`) ends where the next clause
// begins, and nowhere else. Whole-token comparison, so a relation or a
// level named with the keyword as a substring is not a boundary.
std::pair<std::string_view, std::string_view> SplitBeforeClause(std::string_view line,
                                                                std::string_view keyword) {
    std::string_view rest = Trim(line);
    std::size_t consumed = 0;
    while (!rest.empty()) {
        auto [word, tail] = SplitFirstToken(rest);
        if (IEquals(word, keyword)) return {Trim(line.substr(0, consumed)), rest};
        // Where the next token begins, measured against the original line
        // so the answer's first half keeps the caller's own bytes.
        consumed = static_cast<std::size_t>(tail.empty() ? line.size()
                                                         : tail.data() - line.data());
        rest = tail;
    }
    return {Trim(line), std::string_view{}};
}

}  // namespace

// ---- The error surface (docs/spec/txn.md section 5, protocol.md section 11) ---
//
// A conflict is not an ordinary error: financial client libraries build
// retry loops on the `retryable` bit, so its spelling is part of the
// compatibility surface rather than a diagnostic. Every path that can
// report one goes through here, so the shape cannot drift between them.
//
//     ERR TXN_CONFLICT retryable=1 row id=42 was written by transaction 118
//
// The `ERR ` prefix stays, because it is what drives the dispatcher's
// Warn-vs-Debug logging one level up.
namespace {

// **The spellings, written once and read both ways.** `ErrorReply` renders
// from this table and `StatusFromErrorReply` recovers through it, so a code
// added to one is a code added to both - which is the whole point, because a
// spelling that reached only the renderer would come back through the bare
// arm and lose its `retryable` bit on the way (SS3's round trip,
// command_dispatcher.hpp). The rationale for each entry is on the entry.
struct ErrorSpelling {
    StatusCode code;
    std::string_view token;  // including the trailing space
};
inline constexpr ErrorSpelling kErrorSpellings[] = {
    // The only retryable code (status.hpp): a race the statement lost.
    {StatusCode::kTxnConflict, "TXN_CONFLICT retryable=1 "},
    // A constraint the statement *broke*, as opposed to a race it lost.
    // Given a spelling of its own for the same reason TXN_CONFLICT has one -
    // a client library switches on it - and carrying `retryable=0`
    // explicitly rather than by omission, so the two look alike where they
    // are read.
    {StatusCode::kFkViolation, "FK_VIOLATION retryable=0 "},
    // The third constraint spelling (docs/spec/assertion.md §4.4, AS9),
    // shaped exactly like FK_VIOLATION and for its reason.
    {StatusCode::kAssertionViolation, "ASSERTION_VIOLATION retryable=0 "},
    // The fourth (BV-Q3), shaped exactly like the two above.
    {StatusCode::kTransitionViolation, "TRANSITION_VIOLATION retryable=0 "},
    // Statement shipping, this spelling's producer, retired (AT-S6); its
    // history is at cf889b63. The spelling stays because the wire pins it. Its own spelling
    // because it is the one refusal here that does **not** mean "nothing
    // happened": the statement may have committed. A client must be able to
    // tell it from the bare `ERR` it would otherwise wear, because the
    // correct response is to read the data back - not to retry, which
    // against engine-issued tuple keys would insert twice.
    {StatusCode::kUnknownOutcome, "UNKNOWN_OUTCOME retryable=0 "},
    // The refusal pair (status.hpp carries the test that separates them),
    // spelled because the distinction pays only at the client: "can I
    // rewrite this?" versus "should I ask a newer server?".
    {StatusCode::kUnsupported, "UNSUPPORTED retryable=0 "},
    {StatusCode::kNotImplemented, "NOT_IMPLEMENTED retryable=0 "},
};

}  // namespace

std::string ErrorReply(const Status& status) {
    for (const ErrorSpelling& spelling : kErrorSpellings) {
        if (status.code() == spelling.code) {
            return "ERR " + std::string(spelling.token) + status.message();
        }
    }
    return "ERR " + status.message();
}

Status DeadlockVictim(const std::string& waited_for) {
    return Status::TxnConflict(
        "deadlock: this transaction waited for " + waited_for +
        ", which is itself waiting on a row this transaction holds. This transaction closed "
        "the cycle, so its statement is the one refused; ROLLBACK to release its rows and let "
        "the other proceed");
}

Status StatusFromErrorReply(std::string_view reply) {
    constexpr std::string_view kPrefix = "ERR ";
    if (reply.rfind(kPrefix, 0) != 0) return Status::OK();
    reply.remove_prefix(kPrefix.size());

    for (const ErrorSpelling& spelling : kErrorSpellings) {
        if (reply.rfind(spelling.token, 0) != 0) continue;
        reply.remove_prefix(spelling.token.size());
        return Status::FromWire(static_cast<std::uint32_t>(spelling.code), std::string(reply));
    }
    // The bare arm. kInvalidArgument stands for every code that renders
    // bare - which is what makes this lossy, and harmless: `ErrorReply`
    // renders all of them identically, so the line the client is handed is
    // the line the dispatcher wrote whichever of them it was.
    return Status::InvalidArgument(std::string(reply));
}

// Whether a write is checked against a Bound Cabin - true since AST07,
// whose enforcer runs in the three write paths. Still a conjunct rather
// than a constant `1` in the replies, because enforcement also needs the
// *registry* to hold the assertion: one the registry cannot enforce is not
// enforced however true this constant is (`assertion.md` §6.1, §8.1).
constexpr bool kWritePathEnforcesAssertions = true;

stats::LockWaitRecord CommandDispatcher::LockWaitRow(const txn::LockKey& key,
                                                    txn::LockMode mode, std::uint64_t waiter,
                                                    std::uint64_t holder) const {
    stats::LockWaitRecord row;
    row.core = core_id_;
    // The wall instant `NOW()` reads, so a row compares against it.
    row.started_us = exec::StatementContextNow().now_us;
    row.unit = key.unit;
    row.mode = mode;
    row.rel_oid = key.rel_oid;
    row.key = key.unit == txn::LockUnit::kRelation ? 0 : key.lo;
    row.waiter_trx = waiter;
    // A read borrow's id names no transaction (`IsReadHolder`).
    row.holder_trx = txn::IsReadHolder(holder) ? 0 : holder;
    return row;
}

sched::Coro CommandDispatcher::AwaitWriteBlock(std::string_view line, Session* session,
                                               DispatchOutcome* out,
                                               sched::MonoTimeNs* statement_deadline_ns) {
    // ---- R6-5: D5's bounded wait on an undecided row --------------------
    //
    // A writer of a row held by an undecided transaction **blocks**, with a
    // bounded ceiling ending in a named refusal, rather than being refused
    // up front with an internal state it can only spin on (D5; any
    // undecided holder since AO-S3, on any core since AX-S1).
    //
    // **The wait is on the statement, not on the row**, because there is
    // nowhere inside a write path to park: a conflict is found under a page
    // span in a row callback (`CheckWriteConflictBlocking` says so at the
    // site). So the statement is refused, nothing having been written -
    // which `EndWrite` is what establishes - this coroutine parks until the
    // doubt clears, and the statement runs again from the top. What the
    // client sees is one statement that took a while, which is what
    // "blocks" means to it.
    //
    // **Bounded once, not once per blocker.** The deadline is taken before
    // the first wait and every later one shares it, so a statement that is
    // blocked, freed, and blocked again by a *second* undecided transaction
    // still ends within one ceiling. That is what makes HP3's "no hang is
    // reachable" true of a shape that would otherwise be a loop with a
    // bounded body.
    //
    // **The discriminator is at the recording site**: `CheckWriteConflictBlocking`
    // notes a blocker only under `may_park_` and a live clock. The tests
    // below repeat it because they guard a dereference and a deadline
    // (`NowNs()` answers 0 with no clock, which would make the wait
    // unbounded), and so the deadline never starts for a caller with
    // nothing to wait on.
    //
    // **What the waiter is held by, in words** (AO-S6e-c). A tuple key of 0 is not
    // a row (`kFirstRowId` is 1): it is what an assertion's admission
    // records, whose contended resource is a *group*.
    const auto held_name = [](std::uint64_t tuple_key) {
        return tuple_key != 0 ? "row id=" + std::to_string(tuple_key)
                       : std::string("a bound assertion's group");
    };
    if (out->write_block.has_value() && txn_ != nullptr && clock_ != nullptr) {
        // **The bound is a fault net, not a ceiling** (AO-R8, AR2-R10): the
        // wait ends when the holder decides, and this bound fires only when
        // something is broken - reported as a fault below, never as busy.
        //
        // **Taken once for the statement, not once per entry** (AO-S6d,
        // item 16): a foreign-key probe's resume can open a wait of its own,
        // and a re-taken deadline would multiply the net by the rounds.
        if (*statement_deadline_ns == 0) {
            *statement_deadline_ns =
                NowNs() + lock_wait_fault_net_ns_;
        }
        const sched::MonoTimeNs deadline_ns = *statement_deadline_ns;
        // **The waiter's identity for the wait-for graph** (AO-S4a). Zero
        // means "no edge": a statement holding nothing can be in no cycle.
        // An explicit transaction holds rows, and so does an autocommit
        // statement parked mid-walk (AO-S3b), whose open scope can be the
        // other half of a cycle - a waiter the graph cannot name would be a
        // deadlock only the fault net ends (AR2-R10).
        Session& waiting_session = session != nullptr ? *session : autocommit_session_;
        // **Re-read every turn**: an autocommit statement refused before
        // writing, re-run, can write rows and park mid-walk, so its identity
        // appears mid-loop.
        const auto waiter_now = [&waiting_session]() -> std::uint64_t {
            if (const std::optional<Session::ParkedWrite>& parked =
                    waiting_session.parked_write();
                parked.has_value() && parked->txn != nullptr) {
                return parked->txn->id();
            }
            return waiting_session.transaction() != nullptr
                       ? waiting_session.transaction()->id()
                       : 0;
        };
        std::uint64_t waiter_id = waiter_now();
        bool deadlocked = false;
        while (out->write_block.has_value()) {
            const DispatchOutcome::WriteBlock block = *out->write_block;
            // An identity that changed leaves the old one's edge behind,
            // and a stale edge is a false cycle for the next waiter to
            // close against - so it goes with the identity it named.
            const std::uint64_t was = waiter_id;
            waiter_id = waiter_now();
            if (was != waiter_id && was != 0 && locks_ != nullptr) locks_->ClearWaitFor(was);
            if (NowNs() >= deadline_ns) break;
            // **The edge, and the cycle test in the same breath.** A cycle
            // can only close at the instant a waiter adds the edge that
            // closes it, so the waiter that would close one is the victim
            // and is refused rather than parked - AO-R7's rule, applied
            // where it is exactly true instead of up to a cadence later.
            if (locks_ != nullptr && waiter_id != 0 &&
                locks_->NoteWaitFor(waiter_id, block.trx_id)) {
                deadlocked = true;
                // A victim's row by the park's rule: a slot, not yet flipped.
                if (block.slot != nullptr && !txn::LockWaitReady(block.slot)) {
                    stats::LockWaitRecord row =
                        LockWaitRow(block.key, block.mode, waiter_id, block.trx_id);
                    row.outcome = stats::LockWaitOutcome::kDeadlock;
                    stats::LockWaitLog::Record(row);
                }
                break;
            }
            // **`sys.lock_waits`' row** (BO-S3): a wait on the unit's slot,
            // from the park to its end; a poll with no slot is not one, and
            // neither is a slot already flipped - the re-run is immediate.
            std::optional<stats::PendingLockWait> row;
            if (block.slot != nullptr && !txn::LockWaitReady(block.slot)) {
                row.emplace(clock_, LockWaitRow(block.key, block.mode, waiter_id, block.trx_id));
            }
            // **On the unit's slot where the refused ask handed one back**
            // (AX-S2b): the release flips it and kicks this core, whichever
            // core released, so the re-run comes after the release - not
            // in the window between the holder's decide and its release,
            // where it would be refused. Where there is no slot the wait
            // settles the moment the transaction is decided, whichever way
            // - a committed one releases the row to a re-read, an aborted
            // one puts the old version back - on a poll that nothing kicks
            // (`known-gaps.md`, Locks).
            const auto freed = [this, &block] {
                return block.slot != nullptr ? txn::LockWaitReady(block.slot)
                                             : !txn_->IsInFlight(block.trx_id);
            };
            const std::function<bool()> released = [this, &freed, deadline_ns] {
                return freed() || NowNs() >= deadline_ns;
            };
            co_await sched::WaitUntil{&released};
            const bool settled = freed();
            if (row.has_value()) {
                row->End(settled ? stats::LockWaitOutcome::kGranted
                                 : stats::LockWaitOutcome::kFaultNet);
            }
            // Dropped however the wait ended, as `AwaitRelationLock` drops
            // its own - and cleared on the outcome, so an exit below that
            // refuses the statement has nothing left to drop.
            if (block.slot != nullptr) {
                locks_->DropWake(block.key, block.slot);
                out->write_block->slot.reset();
            }
            if (!settled) break;  // the net, not the decision
            if (logging(LogLevel::kDebug)) {
                log_->Debug("lock", "core " + std::to_string(core_id_) + " held a write of " +
                                        held_name(block.tuple_key) + " until transaction " +
                                        std::to_string(block.trx_id) +
                                        " decided, and is running it again");
            }
            const MayParkScope parking(*this, /*allowed=*/true, out->statement);
            *out = DispatchAndStage(line, session);
        }
        // However the wait ended - granted, victim, or the net - this
        // transaction is no longer waiting for anything, and an edge left
        // behind would be a false cycle for the next waiter to close
        // against.
        //
        // **The one exit this line does not cover is the decide's**, and
        // that is deliberate rather than missed. A coroutine destroyed *at*
        // the suspend point - a session dropped mid-wait - never runs this,
        // so the edge outlives its statement. It cannot outlive its
        // *transaction*: `TransactionManager::Commit` and `Abort` clear it
        // with the borrows (AO-R6), and a stale edge matters only while the
        // transaction it names is still in flight: a waiter is recorded
        // against a holder `IsInFlight` admits, or since AY-S3 against a
        // decided one its unit's wake names - which waits for nothing and
        // whose release clears its edges, so it can close no cycle. An RAII guard
        // here would be the wrong shape: the table is instance-scoped and a
        // destructor running during teardown would reach it after its
        // owner is gone.
        if (locks_ != nullptr && waiter_id != 0) locks_->ClearWaitFor(waiter_id);

        if (deadlocked) {
            // **The victim** (AO-R7). `TxnConflict` because that is the one
            // retryable code and a deadlock genuinely is retryable - the
            // transaction that survives will have released by the time this
            // one comes back - and the message names deadlock, because an
            // operator meeting a conflict needs to know whether to look for
            // contention or for a lock-order bug in the application.
            //
            // The waiter is the victim and the holder is untouched (R1: a
            // detector aborts the waiter, never revokes a holder), which is
            // what makes the outcome deterministic rather than a race
            // between two transactions to notice.
            const DispatchOutcome::WriteBlock block = *out->write_block;
            const std::string held = held_name(block.tuple_key);
            // **What is aborted is the statement; the transaction is
            // poisoned and still holds its rows.** Saying "the other
            // proceeds" without saying when would be the convenient
            // sentence rather than the true one: the survivor's wait ends
            // when this client rolls back, which is the same contract every
            // other failed statement inside a transaction has (PostgreSQL
            // holds locks to transaction end too).
            RefuseParkedWrite(*out, waiting_session,
                              DeadlockVictim(held + ", held by transaction " +
                                             std::to_string(block.trx_id)));
        }
        if (out->write_block.has_value()) {
            // **The fault net fired: a defect report, not an outcome** (AO-R8,
            // AR2-R10 as AR2-A amends it) - a detector that missed a cycle, or
            // a holder that is stuck. `TxnConflict` because `IsRetryable`
            // admits exactly that code, and **not** `UnknownOutcome`: this
            // statement plainly did nothing. The message names the net.
            const DispatchOutcome::WriteBlock block = *out->write_block;
            const std::string held = held_name(block.tuple_key);
            // **For a bound assertion's group it means contention, not a
            // defect** (AO-S6e-c): every writer touching one group serialises
            // on it, so the message says so. Whether a group wait should have
            // a shorter bound of its own is undecided (AO-0 item 22).
            const std::string why =
                block.tuple_key != 0
                    ? std::string(
                          "A wait ends when the holder decides, so reaching the net means a "
                          "deadlock went undetected or a holder is stuck - not that the row "
                          "was busy")
                    : std::string(
                          "A bound assertion's group is held for a transaction's length, so "
                          "reaching the net here means the group is contended rather than that "
                          "anything is stuck - retry, or narrow the assertion's grouping");
            RefuseParkedWrite(
                *out, waiting_session,
                Status::TxnConflict(
                    held + " is held by transaction " +
                    std::to_string(block.trx_id) + ", which has not decided after " +
                    std::to_string(lock_wait_fault_net_ns_ / 1'000'000) +
                    " ms; the write hit the lock-wait fault net and was refused. " + why));
        }
    }
    co_return Status::OK();
}

sched::Coro CommandDispatcher::AwaitStatementWaits(std::string_view line, Session* session,
                                                   DispatchOutcome* out,
                                                   sched::MonoTimeNs* statement_deadline_ns) {
    // **One loop over both waits** (AO-S6e-a): each re-run is a fresh
    // statement and can meet what the other waits for, so two sequential
    // arms would refuse what a wait could get past (item 16). Both waits
    // are deadline-bounded and clear their own field on every exit. One
    // function, so the first dispatch and a foreign-key probe's resume
    // cannot diverge.
    for (;;) {
        if (out->write_block.has_value()) {
            co_await AwaitWriteBlock(line, session, out, statement_deadline_ns);
            continue;
        }
        if (out->lock_wait.has_value()) {
            co_await AwaitRelationLock(line, session, out, statement_deadline_ns);
            continue;
        }
        break;
    }
    // ---- BA-S15: a `SELECT`'s walk runs here, sliced --------------------
    //
    // After the waits, which re-run a statement whole and so may replace a
    // head's reply - a reply carrying a walk carries no wait - and before
    // anything after a dispatch reads the outcome: every dispatch
    // `DispatchAsync` makes is followed by this function.
    if (out->select_run != nullptr) co_await CompleteSelect(line, out);
    co_return Status::OK();
}

sched::Coro CommandDispatcher::AwaitRelationLock(std::string_view line, Session* session,
                                                 DispatchOutcome* out,
                                                 sched::MonoTimeNs* statement_deadline_ns) {
    // ---- AO-S6e-b: DDL waits for a positioned reader ---------------------
    //
    // The ask was made inside the DDL body, before its first catalog write,
    // and the body returned its refusal - so nothing of this statement is
    // on a page and the re-run below is a whole fresh statement.
    //
    // **What it still holds depends on whose transaction it is**, and the
    // edge below turns on exactly that: an autocommit drop's transaction
    // was unwound by `InDdlStatement` before this ran, so it holds nothing
    // and can be no half of a cycle; inside an explicit transaction it
    // holds everything that transaction has written, and `EndWrite`
    // deliberately withheld the poison so this wait could happen at all.
    //
    // **What is waited on is the slot, not the holder's decide.** The
    // holder may be a read borrow, which no in-flight table holds, and for
    // a transaction the slot flips at the release rather than the retire.
    // The release flips it from whichever core releases; the kick is
    // AU-S2's. `locks_` and the slot are non-null by construction: the
    // field is set only where a `TryAcquire` asked for a wake and got one.
    const DispatchOutcome::LockWait wait = *out->lock_wait;
    out->lock_wait.reset();

    // Bounded once for the statement, by the lock family's own fault net -
    // the same bound and the same variable as the write-block wait, because
    // a statement that waits for a row and then for a relation has waited
    // once (AO-S6d, item 16).
    if (*statement_deadline_ns == 0) {
        *statement_deadline_ns = NowNs() + lock_wait_fault_net_ns_;
    }
    const sched::MonoTimeNs deadline_ns = *statement_deadline_ns;

    // **The edge, for the one waiter here that can be half of a cycle.**
    // An autocommit DDL holds nothing while it waits - `InDdlStatement`
    // unwound its transaction before this ran - so it registers none, which
    // is the write-block loop's own rule for the same reason. **Inside an
    // explicit transaction it holds everything the transaction has
    // written**, and the holder it waits for need not be a reader: a
    // relation `X` is refused by the `IX` of any writer of that relation,
    // and that writer can be waiting for a row this transaction holds. So
    // the edge is drawn, and a registration that would close a cycle makes
    // this statement the victim (AO-R7) rather than a wait the fault net
    // ends.
    Session& waiting_session = session != nullptr ? *session : autocommit_session_;
    const std::uint64_t waiter_id = waiting_session.transaction() != nullptr
                                        ? waiting_session.transaction()->id()
                                        : 0;
    const std::shared_ptr<txn::LockWaitSlot> slot = wait.slot;
    // **Only against a holder that is a transaction**: a read borrow's id
    // is not one, it never decides, and it never waits - so an edge to it
    // is an entry the walk can only ever pass through on its way to
    // nothing. The graph holds transactions, which is what its contract
    // says.
    // (Holder 0 is `BorrowChain`'s stale-memo re-run, which waits on nobody.)
    if (waiter_id != 0 && wait.holder != 0 && !txn::IsReadHolder(wait.holder) &&
        locks_->NoteWaitFor(waiter_id, wait.holder)) {
        // A victim's row by the park's rule: a slot not yet flipped.
        if (!txn::LockWaitReady(slot)) {
            stats::LockWaitRecord row = LockWaitRow(wait.key, wait.mode, waiter_id, wait.holder);
            row.outcome = stats::LockWaitOutcome::kDeadlock;
            stats::LockWaitLog::Record(row);
        }
        locks_->DropWake(wait.key, slot);
        locks_->ClearWaitFor(waiter_id);
        RefuseParkedWrite(*out, waiting_session,
                          DeadlockVictim(std::string(txn::LockUnitName(wait.key.unit)) + " oid " +
                                         std::to_string(wait.key.rel_oid) + ", held by " +
                                         HolderName(wait.holder)),
                          wait.poisons);
        co_return Status::OK();
    }

    // `sys.lock_waits`' row (BO-S3), unless the slot was flipped before the
    // park: a stale memo's re-run or an ask granted after its refusal, which
    // wait for nothing.
    std::optional<stats::PendingLockWait> row;
    if (!txn::LockWaitReady(slot)) {
        row.emplace(clock_, LockWaitRow(wait.key, wait.mode, waiter_id, wait.holder));
    }
    const std::function<bool()> freed = [this, slot, deadline_ns] {
        return txn::LockWaitReady(slot) || NowNs() >= deadline_ns;
    };
    co_await sched::WaitUntil{&freed};
    if (row.has_value()) {
        row->End(txn::LockWaitReady(slot) ? stats::LockWaitOutcome::kGranted
                                          : stats::LockWaitOutcome::kFaultNet);
    }
    if (waiter_id != 0) locks_->ClearWaitFor(waiter_id);
    // Dropped however the wait ended. A registration left behind outlives
    // the statement that made it and would keep its entry alive with a
    // waiter nobody flips a second time - and the re-run cannot reclaim it,
    // because it asks under a new id and this record is addressable only by
    // its slot.
    locks_->DropWake(wait.key, slot);

    if (!txn::LockWaitReady(slot)) {
        // **The net fired, and that is a defect report** (AO-R8): the
        // refusal already in `out` is the one the ask produced, rendered
        // once, at the site that knew what it asked for.
        if (logging(LogLevel::kWarn)) {
            log_->Warn("lock", "core " + std::to_string(core_id_) + " refused a statement after " +
                                   std::to_string(lock_wait_fault_net_ns_ / 1'000'000) +
                                   " ms waiting for " + txn::LockUnitName(wait.key.unit) + " oid " +
                                   std::to_string(wait.key.rel_oid) + ", held by " +
                                   HolderName(wait.holder));
        }
        // **The poison `EndWrite` withheld for this wait**, restored at the
        // one exit nothing else restores it on (the victim arm has
        // `RefuseParkedWrite`, a re-run `EndWrite`); without it the failed
        // transaction would commit (§6's failure atomicity). Not for a
        // statement that poisons nothing - a `CREATE ASSERTION`'s build.
        if (waiting_session.in_explicit_txn() && wait.poisons) waiting_session.Poison();
        co_return Status::OK();
    }
    if (logging(LogLevel::kDebug)) {
        log_->Debug("lock", "core " + std::to_string(core_id_) + " held a statement until " +
                                HolderName(wait.holder) + " released " +
                                txn::LockUnitName(wait.key.unit) + " oid " +
                                std::to_string(wait.key.rel_oid) + ", and is running it again");
    }
    // **A wake is not a grant** (`lock_table.hpp`): the unit may have been
    // taken again between the flip and this line - by another reader, which
    // never waits and so never queues behind this asker. The re-run asks
    // again and parks again if it must, and the statement's one deadline is
    // what bounds the sequence rather than each turn of it.
    const MayParkScope parking(*this, /*allowed=*/true, out->statement);
    *out = DispatchAndStage(line, session);
    co_return Status::OK();
}

sched::Coro CommandDispatcher::DispatchAsync(std::string_view line, Session* session,
                                             DispatchOutcome* out) {
    // A deferred `strict` commit this statement leaves behind (BA-S7 part 2)
    // is parked on below even if a re-run's outcome no longer names it: an
    // entry nobody parks on would hold its marker, and so cap every
    // snapshot on the instance, until some other statement's park ran it.
    // Found by this call's number, not by the queue's length: another
    // statement of this core stages its own inside this one's parks and
    // walk (BA-S15), and this statement must not wait out that one's sync.
    const std::uint64_t statement = ++last_statement_;
    // `sys.statement_stats` times the statement from here to its reply
    // (BO-S5); a frame destroyed at a park never replies and is no call.
    const sched::MonoTimeNs began_ns = NowNs();
    out->statement = statement;
    // Every statement runs on the core its session is on (AT-S9). What
    // suspends here is a statement's own wait - a lock, a blocking writer,
    // the group commit - never a stage on another core.

    // ---- BA-R1c: the session's own last commit, before any mint ----------
    //
    // Another core's commit marker can hold the snapshot ceiling below a
    // commit this session was already acknowledged - for a whole `strict`
    // sync - and a statement minted under it misses its own session's row.
    // So the statement waits here, at the boundary, holding nothing, until
    // the markers below its bound lift; the last lift kicks this core
    // (`instance_visibility.hpp`'s "acknowledged-commit bound" note). Not a
    // mint at the bound instead: that covers a commit whose entry is not in.
    if (const wal::Lsn bound = UncoveredCommit(session); bound != wal::kNoLsn) {
        const txn::TransactionManager::CeilingWait waiting(*txn_);
        const std::function<bool()> covered = [this, bound] { return txn_->CeilingCovers(bound); };
        co_await sched::WaitUntil{&covered};
    }

    // **The statement may park from here**, which is the whole difference
    // between this entry point and `Dispatch()` - and the condition every
    // wait below is admitted under. Set and cleared around
    // the synchronous half, which takes no suspension point, so it never
    // spans a park and never describes another statement.
    {
        // **RAII** (AO-S6d): a leaked `may_park_` grants a parking
        // allowance, and the guard keeps that structural should a
        // `co_return` or `co_await` ever appear inside a window.
        const MayParkScope parking(*this, /*allowed=*/true, statement);
        *out = DispatchAndStage(line, session);
    }
    // Kept from the first run: every re-run is the same text, and a reply
    // rebuilt below (a refused commit, a walk's end) need not carry it.
    const std::uint64_t fetch_id = out->fetch_id;

    // ---- R6-5: D5's bounded wait on an undecided row --------------------
    //
    // `AwaitWriteBlock` is the whole of it, reached through
    // `AwaitStatementWaits` over what the statement's own dispatch
    // produced. The frame is allocated only where there is something to
    // wait for.
    //
    // **The deadline is the statement's** and is threaded through both
    // callers for the reason the function states.
    sched::MonoTimeNs statement_deadline_ns = 0;
    co_await AwaitStatementWaits(line, session, out, &statement_deadline_ns);

    // ---- BA-S14: a structural refusal waits a turn and runs again --------
    //
    // **A stale descent's `TxnConflict` stops reaching the client** (BA-R11,
    // BA-Q10). Seven sites give up a descent whose path another core's split
    // or root growth made stale; until BA-S14 that refusal went to the client
    // as `retryable=1`, and BA-S4's census counted 20 of them. A statement
    // that wrote nothing before the refusal is re-run here from the top,
    // after one yield so the core that split runs on: the re-run re-reads
    // the root, which a growth moved through the schema word. Bounded by the
    // statement's one deadline, the fault net, like every other wait in
    // this function; the refusal that outlasts it is answered, and poisons
    // an explicit transaction where a write's refusal would have (a read's
    // never does). A statement that wrote rows first keeps today's refusal
    // (`txn.md` §9: no statement-level rollback) - `EndWrite` clears the
    // note for it. No B-link move-right (BA-Q10).
    while (StructuralRerunDue(*out, statement_deadline_ns)) {
        co_await sched::Yield{};
        {
            const MayParkScope parking(*this, /*allowed=*/true, statement);
            *out = DispatchAndStage(line, session);
        }
        co_await AwaitStatementWaits(line, session, out, &statement_deadline_ns);
    }
    // The poison `EndWrite` withheld for a re-run is owed by whatever ends
    // the re-runs as a refusal - the deadline, or a last run whose outcome
    // is no longer the structural refusal. Read off the outcome, not the
    // member: another statement of this core may have run since (BA-S15).
    if (out->poison_withheld) (session != nullptr ? *session : autocommit_session_).Poison();

    // ---- BH: a `PURGE` waits out a key it may not free yet (PU4, PU5) ----
    //
    // **Polled, and bounded by `kPurgeHorizonWaitNs`, not the fault net**
    // (BH-Q3): an open older snapshot is the ordinary case, and nothing
    // kicks this core when the horizon advances (`known-gaps.md`, "what
    // still polls"). Each re-run is a whole attempt with a fresh owned
    // transaction, so the attempt that finally writes judged under a view
    // minted after the wait (BH-R5). At the bound the last attempt's
    // refusal stands - the `TxnConflict` naming the key.
    if (out->purge_wait.has_value() && txn_ != nullptr && clock_ != nullptr) {
        const sched::MonoTimeNs deadline_ns = NowNs() + exec::kPurgeHorizonWaitNs;
        while (out->purge_wait.has_value() && NowNs() < deadline_ns) {
            const DispatchOutcome::PurgeWait wait = *out->purge_wait;
            const std::function<bool()> cleared = [this, wait, deadline_ns] {
                const bool settled = wait.horizon ? txn_->ResolvedForEveryReader(wait.trx_id)
                                                  : !txn_->IsInFlight(wait.trx_id);
                return settled || NowNs() >= deadline_ns;
            };
            co_await sched::WaitUntil{&cleared};
            {
                const MayParkScope parking(*this, /*allowed=*/true, statement);
                *out = DispatchAndStage(line, session);
            }
            // A re-run is a whole statement and can meet what any statement
            // meets - a DDL that took the relation's `X` during the wait -
            // so its own waits are waited out here, and their registrations
            // dropped, before the purge's is asked again (BH-S3's review, C1).
            co_await AwaitStatementWaits(line, session, out, &statement_deadline_ns);
        }
        out->purge_wait.reset();
    }

    // The foreign-key probe park (AT-S5f), the shipped-statement park and
    // cross-owner commit (AT-S6) and the remote step park (AT-S9) retired;
    // their history is at cf889b63. A foreign-key statement parks the way
    // every other statement does: on a row it cannot have yet, through
    // `AwaitStatementWaits` above, and it is re-run rather than resumed. A
    // transaction is this core's, whole, and its `COMMIT` is the local one.

    if (out->pending_lsn == wal::kNoLsn) {
        for (auto it = deferred_commits_.rbegin(); it != deferred_commits_.rend(); ++it) {
            if (it->statement == statement) {
                out->pending_lsn = it->lsn;
                break;
            }
        }
    }
    if (out->pending_lsn != wal::kNoLsn) {
        // **The group commit.** Parking here rather than syncing inside the
        // statement is the whole change: every other runnable connection
        // gets to stage its own commit before the reactor's post-task hook
        // syncs once for all of them (Scheduler::SetPostTaskHook). Nothing
        // is held across this - the statement finished above, its page
        // spans with it.
        const wal::Lsn lsn = out->pending_lsn;
        // A fail-stopped log (`wal/stream.hpp`) never syncs again, so the
        // wait ends there too and the commit is answered with the refusal
        // rather than parked until the restart.
        const std::function<bool()> durable = [this, lsn] {
            return wal_->IsDurable(lsn) || wal_->stopped();
        };
        // Counted in before the wait's first read, so the writer kicks this
        // core when the watermark passes (BA-R4 part 1).
        const wal::WalManager::DurableWait waiting(*wal_, core_id_);
        co_await sched::WaitUntil{&durable};
        // A deferred `strict` commit publishes here, after its record is
        // durable (BA-S7 part 2) - this statement's and any other this
        // core parked that the same sync covered.
        FinishDeferredCommits();
        out->pending_lsn = wal::kNoLsn;
        if (Status refused = wal_->EnsureDurable(lsn); !refused.ok()) {
            *out = {ErrorReply(refused), out->should_stop, 0, refused};
            // A refused commit is no bound: a deferred one was aborted, so
            // no ceiling would ever cover it. The session's earlier bound
            // was covered before this statement ran (BA-R1c).
            (session != nullptr ? *session : autocommit_session_)
                .set_acknowledged_commit_lsn(wal::kNoLsn);
        }
    }
    out->fetch_id = fetch_id;
    RecordStatement(line, *out, began_ns);
    co_return Status::OK();
}

bool CommandDispatcher::StructuralRerunDue(const DispatchOutcome& out,
                                           sched::MonoTimeNs& deadline_ns) {
    if (out.status.code() != StatusCode::kTxnConflict || !storage::StructuralRefusalNoted() ||
        txn_ == nullptr || clock_ == nullptr) {
        return false;
    }
    if (deadline_ns == 0) deadline_ns = NowNs() + lock_wait_fault_net_ns_;
    if (NowNs() >= deadline_ns) return false;
    Contention::Add(Tally::kStructuralReruns);
    return true;
}

CommandDispatcher::StatementEpochScope::StatementEpochScope(CommandDispatcher& dispatcher)
    : d_(dispatcher) {
    const bool outermost = d_.epoch_depth_++ == 0;
    // Published before the word is read (BF-R9): `kEntering` and a fence,
    // so a reclaimer either sees this statement or the statement sees the
    // drop's word.
    const auto seam = [this](EpochSeam at) {
        if (d_.epoch_seam_for_test_ && d_.epoch_seam_at_ == at) d_.epoch_seam_for_test_();
    };
    if (outermost && d_.epochs_ != nullptr) d_.epochs_->Enter(d_.core_id_);
    // **The count before the word** (`ViewFor`): a closing DDL moves the word
    // and then uncounts, so a head that read the word before the move and the
    // count after the uncount would keep a cache filled while the DDL was
    // open and still take the fast path. Read first, a count of zero means
    // the word read below is past every move a closed DDL made.
    d_.ddl_open_at_head_ = d_.txn_ != nullptr && d_.txn_->visibility().AnyDdlOpen();
    d_.catalog_.Revalidate(d_.epoch_seam_for_test_
                               ? std::function<void()>([&] { seam(EpochSeam::kAfterRead); })
                               : std::function<void()>{});
    seam(EpochSeam::kBeforePublish);
    if (outermost && d_.epochs_ != nullptr) {
        d_.epochs_->Publish(d_.core_id_, d_.catalog_.cache_built_at());
    }
    seam(EpochSeam::kAfterPublish);
}

CommandDispatcher::StatementEpochScope::~StatementEpochScope() {
    if (--d_.epoch_depth_ == 0 && d_.epochs_ != nullptr) d_.epochs_->Leave(d_.core_id_);
}

DispatchOutcome CommandDispatcher::DispatchAndStage(std::string_view line, Session* session) {
    // BA-S14's two per-statement facts, cleared before the statement runs.
    structural_poison_withheld_ = false;
    storage::ClearStructuralRefusal();
    // The statement boundary is where the catalog asks whether the schema
    // moved (AT-S2; `Catalog::Revalidate`): before this statement resolves
    // anything and after the previous one released everything it held. And
    // where this core publishes its statement epoch (BF-R9), around that
    // revalidation, for as long as the statement runs.
    const StatementEpochScope epoch(*this);
    // **This statement runs as this dispatcher's core** (AM-S2 step 3,
    // `base/current_core.hpp`). On a reactor thread it repeats what
    // `Scheduler::RunOnce` declared; off one - a fixture driving several
    // cores' dispatchers from the test thread - it is what makes the
    // identity true for its readers (the page store's per-core WAL gate,
    // the lock table's waiter core, the WAL manager's owner check). One
    // statement is one core's work, so this is the seam.
    const CurrentCoreGuard as_this_core(core_id_);

    // Read only when something might report it; a dispatcher with no
    // logger does no clock reads at all.
    const sched::MonoTimeNs started_ns = log_ == nullptr ? 0 : NowNs();

    // A caller with no session of its own gets this dispatcher's, which is
    // permanently in autocommit unless that caller opens a transaction on
    // it. That is what makes `Dispatch(line)` mean exactly what it meant
    // before transactions existed.
    Session& active = session != nullptr ? *session : autocommit_session_;

    pending_commit_lsn_ = wal::kNoLsn;
    blocking_writer_ = 0;
    blocked_tuple_key_ = 0;
    lock_wait_.reset();
    last_refusal_ = Status::OK();
    last_refusal_detail_ = wire::kNoDetail;
    statement_trail_mark_ = 0;
    statement_fetch_id_ = 0;

    DispatchOutcome outcome = DispatchInner(line, active);
    outcome.fetch_id = std::exchange(statement_fetch_id_, 0);
    // Read back out of the member the write paths set: threading it through
    // InsertInner/UpdateInner/EndWrite and every handler between would be a
    // parameter on a dozen signatures for one number, and one statement runs
    // at a time on a core (sched.md section 3), so there is no second value
    // to confuse it with.
    outcome.pending_lsn = pending_commit_lsn_;
    pending_commit_lsn_ = wal::kNoLsn;
    // R6-5's, on the same terms. `EndWrite` has already dropped the blocker
    // where the statement is not re-runnable, so a value here means "this
    // statement wrote nothing and a wait could get it past".
    //
    // **Never beside a relation wait** (AT-S5e): a
    // statement can record a busy parent's writer before it asks its
    // relation `IX`, and a write-block re-run would overwrite the outcome
    // and drop the relation's wake without a `DropWake`. The relation
    // re-run re-resolves the parent anyway.
    if (blocking_writer_ != 0 && !lock_wait_.has_value()) {
        outcome.write_block = DispatchOutcome::WriteBlock{blocking_writer_, blocked_tuple_key_};
        // The unit's wake travels with the block (AX-S2b). Its holder is
        // the blocker recorded: every `NoteBlockingWriter` that can follow
        // a refused `BorrowChain` is handed that ask's own blocker, and
        // every refused `BorrowChain` ends the statement.
        if (blocking_wake_.has_value()) {
            outcome.write_block->key = blocking_wake_->key;
            outcome.write_block->slot = std::move(blocking_wake_->slot);
            outcome.write_block->mode = blocking_wake_->mode;
        }
    } else if (blocking_wake_.has_value()) {
        // Handed to no wait - `NoteBlockingWriter` declined (a futile
        // repeatable-read wait, a relation wait already carried), `EndWrite`
        // dropped the blocker, or a relation wait stands beside it - so
        // it leaves the table here.
        locks_->DropWake(blocking_wake_->key, blocking_wake_->slot);
    }
    blocking_wake_.reset();
    // **The refusal, installed where the path that raised it could not
    // carry one.** A path that renders its refusal and leaves `status` OK
    // would otherwise have the cap's category recovered by parsing the line
    // - and `kErrorSpellings` cannot recover `ResourceExhausted`, so it
    // becomes `InvalidArgument` with the cap's detail attached to it.
    // `InsertOneRow` carried its refusal as a rendered string until BB-S3
    // and carries the `Status` now. Only installed over an OK status: a path
    // that carried its own has already said something more specific.
    if (!last_refusal_.ok() && outcome.status.ok()) outcome.status = last_refusal_;
    outcome.resource_detail = std::exchange(last_refusal_detail_, wire::kNoDetail);
    last_refusal_ = Status::OK();
    // AO-S6e-b's, on the same terms, and exclusive with the blocker above:
    // `NoteBlockingWriter` records none once a relation wait is set, and
    // the blocker is not installed when one recorded earlier meets it.
    if (lock_wait_.has_value()) outcome.lock_wait = std::move(lock_wait_);
    lock_wait_.reset();
    blocking_writer_ = 0;
    blocked_tuple_key_ = 0;
    outcome.poison_withheld = structural_poison_withheld_;
    outcome.statement = parking_statement_;

    // A `SELECT` whose walk has not run yet is logged when it ends
    // (`CompleteSelect`), with this head's start.
    if (outcome.select_run != nullptr) {
        SetSelectRunStart(*outcome.select_run, started_ns);
        return outcome;
    }
    LogStatement(line, outcome, started_ns);
    return outcome;
}

void CommandDispatcher::LogStatement(std::string_view line, const DispatchOutcome& outcome,
                                     sched::MonoTimeNs started_ns) const {
    if (log_ == nullptr) return;

    // A failed command reports at Warn and a successful one at Debug, so
    // the level has to be decided *before* the enabled() test - gating the
    // whole block on Debug would silently drop every error at any threshold
    // above it, which is exactly the threshold an operator runs at.
    const bool failed = outcome.response.rfind("ERR ", 0) == 0;
    const LogLevel level = failed ? LogLevel::kWarn : LogLevel::kDebug;
    if (!log_->enabled(level)) return;

    // The reply is summarized, not echoed: a SELECT response carries every
    // matching row, and a log that reproduces result sets is a log that
    // cannot be kept. An error is the exception - its whole content is the
    // reason, which is the thing worth having.
    std::string msg =
        "\"" + std::string(line) + "\" -> " +
        (failed ? outcome.response : std::to_string(outcome.response.size()) + "B reply");
    if (clock_ != nullptr) {
        msg += " in " + std::to_string((NowNs() - started_ns) / 1000) + "us";
    }
    log_->Log(level, "query", msg);
}

// ---- The durability wait (docs/spec/wal.md D2) --------------------------------
//
// Two entry points, one statement path, and the difference is only *how the
// wait is taken*. `Dispatch()` blocks on this thread, because its callers -
// tests, tools, anything without a reactor - have nowhere to park.
// `DispatchAsync()` parks, which is what lets the next connection's
// statement run and stage its commit into the same device sync. The
// statement itself is finished either way before this runs, so nothing is
// held across the wait.

bool CommandDispatcher::DefersCommit(std::uint64_t txn_id) const {
    return effective_durability_ == wal::DurabilityClass::kStrict && may_park_ &&
           wal_ != nullptr && wal_->KicksDurableWaiters() &&
           std::find(ddl_txns_.begin(), ddl_txns_.end(), txn_id) == ddl_txns_.end();
}

void CommandDispatcher::DeferCommit(txn::Transaction* txn, wal::Lsn lsn) {
    pending_commit_lsn_ = lsn;
    deferred_commits_.push_back({lsn, txn, parking_statement_});
}

void CommandDispatcher::FinishDeferredCommits() {
    std::size_t done = 0;
    for (; done < deferred_commits_.size(); ++done) {
        const DeferredCommit& d = deferred_commits_[done];
        if (wal_->IsDurable(d.lsn)) {
            txn_->FinishDeferredCommit(*d.txn);
        } else if (wal_->stopped()) {
            (void)txn_->Abort(*d.txn, RowLocatorForRollback());
        } else {
            break;
        }
        txn_->Release(*d.txn);
    }
    deferred_commits_.erase(deferred_commits_.begin(),
                            deferred_commits_.begin() + static_cast<std::ptrdiff_t>(done));
}

wal::Lsn CommandDispatcher::UncoveredCommit(Session* session) {
    Session& bound_session = session != nullptr ? *session : autocommit_session_;
    const wal::Lsn bound = bound_session.acknowledged_commit_lsn();
    if (bound == wal::kNoLsn || txn_->CeilingCovers(bound)) {
        bound_session.set_acknowledged_commit_lsn(wal::kNoLsn);
        return wal::kNoLsn;
    }
    return bound;
}

DispatchOutcome CommandDispatcher::Dispatch(std::string_view line, Session* session) {
    const sched::MonoTimeNs began_ns = NowNs();
    DispatchOutcome outcome = DispatchInternal(line, session);
    RecordStatement(line, outcome, began_ns);
    return outcome;
}

void CommandDispatcher::RecordStatement(std::string_view line, const DispatchOutcome& out,
                                        sched::MonoTimeNs began_ns) const {
    // Once per statement, at its end, the statement whole: a re-run, a wait
    // and the group commit are inside it (§3.1). A refusal is a call too.
    const sched::MonoTimeNs now_ns = NowNs();
    const bool refused = !out.status.ok() || out.response.rfind("ERR", 0) == 0;
    stats::StatementStats::Record(core_id_, out.fetch_id, line, refused,
                                  now_ns > began_ns ? (now_ns - began_ns) / 1000 : 0,
                                  exec::StatementContextNow().now_us);
}

DispatchOutcome CommandDispatcher::DispatchInternal(std::string_view line, Session* session) {
    // `DispatchAsync`'s BA-R1c wait, blocking, as this path's group commit
    // is below: no other task on this core can hold a marker meanwhile,
    // because a commit holds one only inside its synchronous body. **That
    // premise ends with BA-Q3 (b)**, which holds a `strict` commit's marker
    // across a park: this loop must become a park before it lands, or it
    // spins on its own core's marker for good.
    if (const wal::Lsn bound = UncoveredCommit(session); bound != wal::kNoLsn) {
        const txn::TransactionManager::CeilingWait waiting(*txn_);
        while (!txn_->CeilingCovers(bound)) {
            // **This core's own parked `strict` commits hold markers too**
            // (BA-S7 part 2), and their statements cannot run while this
            // wait holds the reactor: make their records durable and
            // publish them here, or this loop waits for itself.
            if (!deferred_commits_.empty()) {
                (void)wal_->EnsureDurable(deferred_commits_.back().lsn);
                FinishDeferredCommits();
                continue;
            }
            std::this_thread::yield();
        }
    }
    // `DispatchAsync`'s BA-S14 re-run, blocking: a structural refusal of a
    // statement that wrote nothing is run again after a yield, under the
    // fault net, and the poison withheld for it is owed by whatever ends the
    // re-runs. Without it this path answered a refusal no client of the
    // server sees.
    DispatchOutcome outcome;
    {
        const RerunScope rerunning(*this);
        outcome = DispatchAndStage(line, session);
        sched::MonoTimeNs deadline_ns = 0;
        while (StructuralRerunDue(outcome, deadline_ns)) {
            std::this_thread::yield();
            outcome = DispatchAndStage(line, session);
        }
    }
    if (outcome.poison_withheld) (session != nullptr ? *session : autocommit_session_).Poison();
    if (outcome.pending_lsn == wal::kNoLsn) return outcome;

    // Inline, on this thread: the batch is whatever happened to be staged
    // already, which with no scheduler is this commit alone.
    Status s = wal_->DrainOnce();
    if (s.ok()) s = wal_->EnsureDurable(outcome.pending_lsn);
    if (!s.ok()) {
        DispatchOutcome refused{ErrorReply(s), outcome.should_stop};
        refused.fetch_id = outcome.fetch_id;  // still this shape's call (BO-S5)
        return refused;
    }
    outcome.pending_lsn = wal::kNoLsn;
    return outcome;
}

namespace {

// The statement-class → role table (role.hpp's model, docs/spec/protocol.md
// §14). Keyed on exactly the tokens DispatchInner routes on, and
// *total*: a command absent from every readonly/readwrite line below is
// admin's - including commands that do not exist, so a statement added
// later is refused by default rather than admitted by omission, the
// posture Session::AdmittedWhileFailed set.
Role RequiredRole(std::string_view cmd, std::string_view rest) {
    // The readonly floor: reads, liveness, and transaction control -
    // BEGIN/COMMIT are admissible because a REPEATABLE READ transaction
    // is how a readonly session gets one view across statements; any
    // write *inside* it is judged as itself.
    if (IEquals(cmd, "PING") || IEquals(cmd, "SELECT") || IEquals(cmd, "WITH") ||
        IEquals(cmd, "ANALYZE") || IEquals(cmd, "SHOW") || IEquals(cmd, "DESCRIBE") ||
        IEquals(cmd, "DESC") || IEquals(cmd, "BEGIN") ||
        IEquals(cmd, "START") || IEquals(cmd, "USE") ||
        IEquals(cmd, "COMMIT") || IEquals(cmd, "ROLLBACK") || IEquals(cmd, "ABORT") ||
        IEquals(cmd, "SAVEPOINT") || IEquals(cmd, "RELEASE")) {
        // SAVEPOINT and RELEASE are refused for every role; at this floor a
        // readonly session is told why rather than that it lacks a role.
        return Role::kReadOnly;
    }
    if (IEquals(cmd, "INSERT") || IEquals(cmd, "UPDATE") || IEquals(cmd, "DELETE")) {
        return Role::kReadWrite;
    }
    if (IEquals(cmd, "SET")) {
        // A session may steer its own reads and its own commits; the
        // server is the admin's. `DURABILITY` joins `ISOLATION` at the
        // readonly floor because it binds **this connection's** commits
        // and no one else's - the same reasoning, and deliberately not the
        // reasoning that keeps `SYNC` admin's (§14): SYNC forces
        // device-wide I/O for every session, and a durability class asks
        // only for the acknowledgement this session is owed. A class is
        // not a way to weaken anyone else's guarantee.
        const std::string_view target = SplitFirstToken(rest).first;
        return (IEquals(target, "ISOLATION") || IEquals(target, "DURABILITY")) ? Role::kReadOnly
                                                                              : Role::kAdmin;
    }
    // DDL in every spelling, STOP, SYNC - and everything unclassified.
    return Role::kAdmin;
}

}  // namespace

DispatchOutcome CommandDispatcher::DispatchInner(std::string_view line, Session& session) {
    // A new statement: the next reader to need a view takes the boundary
    // (see `EnsureStatementBoundary`). One statement runs at a time on a
    // core (sched.md §3), so a plain member is the right scope.
    statement_boundary_taken_ = false;

    // And the class this statement's commit will be acked under - §9's
    // three rungs resolved once, here, rather than at each of the four
    // sites below that time an acknowledgement. Stamped **before** the
    // routing, so `SET DURABILITY` itself runs under the class that was in
    // force when it arrived and takes effect from the next statement, the
    // same "applies to the next one" rule `SET ISOLATION LEVEL` states.
    effective_durability_ = session.EffectiveDurability(durability_);

    auto [cmd, rest] = SplitFirstToken(line);

    if (cmd.empty()) {
        return {"ERR empty command", false};
    }

    // ---- Authorization (role.hpp; docs/spec/protocol.md §14) -----------------
    //
    // One check, on the same tokens the routing below reads, before
    // anything else interprets the line. An admin never fails it, so a
    // typo still answers "unknown command" to the one role for which a
    // permission message would be a misdiagnosis.
    if (Role required = RequiredRole(cmd, rest); !RoleCovers(session.role(), required)) {
        return {"ERR permission: " + std::string(cmd) + " needs " +
                    std::string(RoleName(required)) + "; this connection is " +
                    std::string(RoleName(session.role())),
                false};
    }

    // ---- The archive's fail-closed gate (BR-Q2 (a), `wal_archive.hpp`) -------
    //
    // Reads, transaction control and the admin verbs that end the condition
    // (BACKUP, SYNC, STOP) still run; a statement that writes the log does not.
    if (IEquals(cmd, "INSERT") || IEquals(cmd, "UPDATE") || IEquals(cmd, "DELETE") ||
        IEquals(cmd, "CREATE") || IEquals(cmd, "DROP") || IEquals(cmd, "ALTER") ||
        IEquals(cmd, "PURGE") || IEquals(cmd, "REINDEX")) {
        if (auto refused = ArchiveRefusal(); refused.has_value()) return *refused;
    }

    // ---- The failed-txn gate (docs/spec/txn.md section 10-8) -----------------
    //
    // A statement inside an explicit transaction failed, so the transaction
    // can no longer be committed. Everything but the ways out is refused -
    // a whitelist, so a statement added later is refused by default rather
    // than admitted by omission.
    if (session.failed() && !Session::AdmittedWhileFailed(cmd)) {
        if (const std::uint64_t expired = session.expired_trx_id(); expired != 0) {
            // **Already rolled back, and the client has to be told so**
            // (AN-R14, docs/spec/txn.md §1): the generic text below says
            // "aborted" and leaves a reader to assume ROLLBACK will undo
            // something. Its own detail, because the fix is a shorter
            // transaction - COMMIT included, which must never quietly
            // succeed over writes that are already gone.
            Status s = Status::ResourceExhausted(
                "transaction trx_id=" + std::to_string(expired) +
                " exceeded the transaction lifetime ceiling (txn_lifetime_ceiling_ms) and was "
                "rolled back by the server; its writes are undone. ROLLBACK to end it");
            last_refusal_detail_ =
                static_cast<std::uint16_t>(wire::ResourceDetail::kTxnLifetimeCeiling);
            return {ErrorReply(s), false, 0, s};
        }
        return {"ERR current transaction is aborted; commands are ignored until ROLLBACK",
                false};
    }

    // ---- Transaction control --------------------------------------------
    if (IEquals(cmd, "BEGIN") || IEquals(cmd, "START")) {
        // `START TRANSACTION` is the SQL spelling; both reach one handler,
        // which strips the noise word.
        return HandleBegin(rest, session);
    }
    if (IEquals(cmd, "COMMIT") || IEquals(cmd, "ROLLBACK") || IEquals(cmd, "ABORT")) {
        if (Status s = RefuseTxnEndTail(line, cmd, rest, session); !s.ok()) {
            return {ErrorReply(s), false, 0, s};
        }
        return IEquals(cmd, "COMMIT") ? HandleCommit(session) : HandleRollback(session);
    }
    if (IEquals(cmd, "SAVEPOINT") || IEquals(cmd, "RELEASE")) {
        const Status s = RefuseWhole(line, cmd, std::string(cmd) + " is not supported");
        return {ErrorReply(s), false, 0, s};
    }
    if (IEquals(cmd, "USE")) return HandleUse(line, rest, session);
    if (IEquals(cmd, "SET")) {
        auto [sub, sub_rest] = SplitFirstToken(rest);
        if (IEquals(sub, "ISOLATION")) return HandleSetIsolation(sub_rest, session);
        if (IEquals(sub, "DURABILITY")) return HandleSetDurability(sub_rest, session);
        if (IEquals(sub, "CABIN_OPTIMIZER")) return HandleSetCabinOptimizer(sub_rest);
        return {"ERR unknown SET target; SET ISOLATION LEVEL, SET DURABILITY and "
                "SET CABIN_OPTIMIZER are supported",
                false};
    }
    if (IEquals(cmd, "PING")) {
        return {"PONG", false};
    }
    if (IEquals(cmd, "STOP")) {
        return {"OK bye", true};
    }
    // Every sub-target under SHOW (and DESCRIBE below) is readonly by
    // RequiredRole's whole-command classification. That is only true
    // while every one of them *reads*: a mutating sub-target added here
    // would be admitted to every rank silently - the one way the role
    // model fails open. Such a command must take the SET branch's shape
    // in RequiredRole (sub-token classified, admin by default).
    if (IEquals(cmd, "SHOW")) {
        auto [sub, sub_rest] = SplitFirstToken(rest);
        return HandleShow(line, sub, sub_rest, session);
    }
    if (IEquals(cmd, "DESCRIBE") || IEquals(cmd, "DESC")) {
        return HandleShow(line, cmd, rest, session);
    }
    // **A DDL runs where the session is** (AT-S5, AT-R5): every core writes
    // the catalog's pages under the page latch, the DDL's own relation `X`
    // (AO) keeps a statement from being overtaken, and the schema word
    // (AT-S2) carries the change to every other core's memo. §5d's purge
    // runs on core 0's tick (BP-S4), gated on `ResolvedForEveryReader`
    // (`ddl-transactional.md` §5d, `catalog.md` CT6). A write's admission
    // asks `CheckWriteAdmission`'s one assertion question, and what keeps
    // two cores off one page's bytes is the page latch.
    // The peer DDL refusal, DDL shipping and the write-affinity gate retired
    // (AT-S5, AT-S9, AT-S18); their history is at cf889b63.
    if (IEquals(cmd, "CREATE")) {
        auto [sub, sub_rest] = SplitFirstToken(rest);
        if (IEquals(sub, "CABIN")) {
            return HandleCabin(Trim(line), session);
        }
        if (IEquals(sub, "ASSERTION")) {
            return HandleAssertion(Trim(line), session);
        }
        if (IEquals(sub, "TRANSITION")) {
            return HandleTransition(Trim(line), session);
        }
        // `CREATE UNIQUE INDEX` (BX); `UNIQUE` before anything else is
        // refused by the parser at that word's byte.
        if (IEquals(sub, "INDEX") || IEquals(sub, "UNIQUE")) {
            return HandleIndex(Trim(line), session);
        }
        if (IEquals(sub, "NAMESPACE")) {
            return HandleNamespace(Trim(line), session);
        }
        if (IEquals(sub, "TABLE")) {
            // Disambiguate the bare-name form ("CREATE TABLE foo")
            // from the SQL form ("CREATE TABLE foo (col type, ...)"): the
            // bare form's argument is just a name, so it never contains
            // '(' - the SQL grammar always does (ast.hpp: a column list is
            // mandatory). Route on that rather than trying to parse both
            // ways and see which succeeds.
            if (sub_rest.find('(') != std::string_view::npos) {
                return HandleCreateTableSql(Trim(line), session);
            }
            return HandleCreateTable(sub_rest, session);
        }
        return {"ERR unknown CREATE target", false};
    }
    if (IEquals(cmd, "ALTER")) {
        return HandleAlter(Trim(line), session);
    }
    if (IEquals(cmd, "DROP")) {
        auto [sub, sub_rest] = SplitFirstToken(rest);
        // Cabins, indexes, assertions and tables can be dropped, and the
        // catalog is append-only apart from those paths. Routed by name so
        // an unroutable object gets a truthful refusal rather than a syntax
        // error pointing at its name. `PATTERN` was one of these until
        // 2026-08-31, when the operator withdrew declared patterns.
        if (IEquals(sub, "CABIN")) {
            return HandleCabin(Trim(line), session);
        }
        if (IEquals(sub, "INDEX")) {
            return HandleIndex(Trim(line), session);
        }
        if (IEquals(sub, "ASSERTION")) {
            return HandleAssertion(Trim(line), session);
        }
        if (IEquals(sub, "TRANSITION")) {
            return HandleTransition(Trim(line), session);
        }
        if (IEquals(sub, "NAMESPACE")) {
            return HandleNamespace(Trim(line), session);
        }
        if (IEquals(sub, "TABLE")) {
            return HandleDropTable(Trim(line), session);
        }
        return {"ERR only DROP TABLE, DROP NAMESPACE, DROP CABIN, DROP INDEX, DROP "
                "ASSERTION and DROP TRANSITION are supported",
                false};
    }
    if (IEquals(cmd, "INSERT")) {
        return HandleInsert(Trim(line), session);
    }
    if (IEquals(cmd, "SELECT")) {
        return HandleSelect(Trim(line), session);
    }
    // ANALYZE is a dispatcher prefix, not a parser keyword: it is stripped
    // here and the remainder goes down the ordinary SELECT path with stats
    // collection on. Two consequences are the reason it is done this way.
    // The run being described is the run that actually happened - same
    // parse, same compile, same executor - and the statement text every
    // layer below sees is the *stripped* text, so a fingerprint (and the
    // sys.patterns row and Waystone trail keyed on it) is identical
    // whether or not a client typed ANALYZE.
    if (IEquals(cmd, "ANALYZE")) {
        if (rest.empty()) {
            return {"ERR ANALYZE needs a statement to analyze", false};
        }
        return HandleSelect(rest, session, /*analyze=*/true);
    }
    if (IEquals(cmd, "UPDATE")) {
        return HandleUpdate(Trim(line), session);
    }
    if (IEquals(cmd, "DELETE")) {
        return HandleDelete(Trim(line), session);
    }
    // Unclassified in `RequiredRole`, so admin's (BH-Q10): it lifts an
    // identity guarantee, which is not a read-write session's decision.
    if (IEquals(cmd, "PURGE")) {
        return HandlePurge(Trim(line), session);
    }
    // Unclassified in `RequiredRole`, so admin's, as every DDL is (BY).
    if (IEquals(cmd, "REINDEX")) {
        return HandleIndex(Trim(line), session);
    }
    if (IEquals(cmd, "SYNC")) {
        return HandleSync();
    }
    // Unclassified in `RequiredRole`, so admin's: a backup pins the log and
    // writes the server's files.
    if (IEquals(cmd, "BACKUP")) {
        return HandleBackup(Trim(line), session);
    }
    // WITH is routed to the SELECT path rather than falling through to
    // "unknown command": the parser answers it with the truthful "CTEs
    // are not supported, subqueries are allowed in predicate position
    // only" and an exact position (spec §2). Dispatching on the first
    // word alone would hide that behind a generic refusal, and a client
    // would have no idea whether the word was unrecognized or declined.
    if (IEquals(cmd, "WITH")) {
        return HandleSelect(Trim(line), session);
    }
    return {"ERR unknown command", false};
}

namespace {

// BACKUP's words, read by hand: the statement never reaches the SQL parser,
// as SET, SHOW and SYNC do not. Each word carries its byte offset so a
// refusal can name it.
struct BackupWord {
    std::string text;
    std::size_t at = 0;
    bool quoted = false;
};

StatusOr<std::vector<BackupWord>> SplitBackupWords(std::string_view line) {
    std::vector<BackupWord> words;
    std::size_t i = 0;
    while (i < line.size()) {
        if (line[i] == ' ' || line[i] == '\t') {
            ++i;
            continue;
        }
        BackupWord word;
        word.at = i;
        if (line[i] == '\'') {
            word.quoted = true;
            ++i;
            bool closed = false;
            while (i < line.size()) {
                if (line[i] == '\'') {
                    if (i + 1 < line.size() && line[i + 1] == '\'') {
                        word.text.push_back('\'');
                        i += 2;
                        continue;
                    }
                    ++i;
                    closed = true;
                    break;
                }
                word.text.push_back(line[i++]);
            }
            if (!closed) {
                return Status::InvalidArgument("BACKUP: unterminated string (byte " +
                                               std::to_string(word.at) + ")");
            }
        } else {
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != ';') {
                word.text.push_back(line[i++]);
            }
            if (word.text.empty()) {  // a bare ';' ends the statement
                ++i;
                continue;
            }
        }
        words.push_back(std::move(word));
    }
    return words;
}

}  // namespace

DispatchOutcome CommandDispatcher::HandleBackup(std::string_view line, Session& session) {
    const auto refuse = [](const Status& s) { return DispatchOutcome{ErrorReply(s), false, 0, s}; };
    const auto answer = [&](StatusOr<std::string> r) {
        return r.ok() ? DispatchOutcome{r.value(), false} : refuse(r.status());
    };
    auto split = SplitBackupWords(line);
    if (!split.ok()) return refuse(split.status());
    const std::vector<BackupWord>& w = split.value();
    const std::size_t verb_at = w.size() > 1 ? w[1].at : line.size();
    const std::string verb = w.size() > 1 ? w[1].text : std::string();
    if (backup_ == nullptr || wal_ == nullptr) {
        return refuse(Status::Unsupported(
            "BACKUP needs a mounted volume with a log, and this dispatcher has none (byte 0)"));
    }
    if (IEquals(verb, "STATUS") && w.size() == 2) {
        return {backup_->StatusLine(), false};
    }
    if (session.in_explicit_txn()) {
        return refuse(Status::NotImplemented(
            "BACKUP inside a transaction is not implemented; run it in autocommit (byte 0)"));
    }
    // `<verb> '<arg>'` or `<verb> TO '<arg>'`: the one argument, quoted.
    const auto quoted_arg = [&](bool to) -> StatusOr<std::string> {
        const std::size_t at = to ? 3 : 2;
        if (to && (w.size() < 3 || !IEquals(w[2].text, "TO") || w[2].quoted)) {
            return Status::InvalidArgument("BACKUP " + verb + " expects TO '<directory>' (byte " +
                                           std::to_string(w.size() > 2 ? w[2].at : line.size()) +
                                           ")");
        }
        if (w.size() <= at || !w[at].quoted) {
            return Status::InvalidArgument(
                "BACKUP " + verb + " expects a quoted " + (to ? "directory" : "label") +
                " (byte " + std::to_string(w.size() > at ? w[at].at : line.size()) + ")");
        }
        if (w.size() > at + 1) {
            return Status::InvalidArgument("BACKUP " + verb + ": unexpected `" + w[at + 1].text +
                                           "` (byte " + std::to_string(w[at + 1].at) + ")");
        }
        if (to && w[at].text.empty()) {
            return Status::InvalidArgument("BACKUP " + verb + ": an empty directory (byte " +
                                           std::to_string(w[at].at) + ")");
        }
        return w[at].text;
    };
    const auto now_unix = static_cast<std::uint64_t>(std::time(nullptr));
    if (IEquals(verb, "START")) {
        auto label = quoted_arg(/*to=*/false);
        if (!label.ok()) return refuse(label.status());
        return answer(backup_->Start(label.value(), *wal_));
    }
    if (IEquals(verb, "COPY")) {
        auto dir = quoted_arg(/*to=*/true);
        if (!dir.ok()) return refuse(dir.status());
        return answer(backup_->Copy(dir.value()));
    }
    if (IEquals(verb, "STOP")) {
        auto dir = quoted_arg(/*to=*/true);
        if (!dir.ok()) return refuse(dir.status());
        return answer(backup_->Stop(dir.value(), *wal_, now_unix));
    }
    if (IEquals(verb, "ABORT") && w.size() == 2) {
        return answer(backup_->Abort());
    }
    return refuse(Status::InvalidArgument(
        "BACKUP expects START '<label>', COPY TO '<directory>', STOP TO '<directory>', ABORT "
        "or STATUS (byte " + std::to_string(w.size() > 2 ? w[2].at : verb_at) + ")"));
}

DispatchOutcome CommandDispatcher::HandleSync() {
    // The log first, and unconditionally. The store's gate syncs it only
    // as far as the pages it is about to write need, so a relaxed commit
    // whose pages are already clean would survive a SYNC unsynced - and
    // SYNC's whole promise is that it does not.
    if (wal_ != nullptr) {
        if (Status s = wal_->SyncAll(); !s.ok()) {
            if (logging(LogLevel::kError)) {
                log_->Error("wal", "client SYNC failed to sync the log: " + s.message());
            }
            return {ErrorReply(s), false, 0, s};
        }
    }
    if (Status s = page_store_.Sync(); !s.ok()) {
        if (logging(LogLevel::kError)) {
            log_->Error("storage", "client SYNC failed: " + s.message());
        }
        return {ErrorReply(s), false, 0, s};
    }
    // Info, not Debug: a client-forced sync is rare and it is a durability
    // point, so it is one of the few things worth having in a default log.
    if (logging(LogLevel::kInfo)) log_->Info("storage", "client SYNC: store persisted");
    return {"OK synced", false};
}

// The meta view's fields (`sys.meta`, BO-Q4): each section's `key=value`
// tokens, space-separated, in print order. A section whose source is absent
// - the absent-rather-than-zeroed rule - has empty text and so no row.
std::vector<std::pair<std::string_view, std::string>> CommandDispatcher::MetaSections() {
    std::vector<std::pair<std::string_view, std::string>> sections;
    std::ostringstream os;
    const auto section = [&](std::string_view name) {
        if (!sections.empty()) sections.back().second = os.str();
        os.str("");
        sections.emplace_back(name, std::string());
    };
    section("volume");
    os << "version=" << superblock_.version() << " create_time=" << superblock_.create_time()
       << " last_mount_time=" << superblock_.last_mount_time()
       // The archive lineage (BR-R10): the history this volume's log is,
       // fresh at creation and at every restore.
       << " wal_lineage=" << superblock_.lineage_id()
       // **What the volume's log is** (AR0 M0). Always `single` -
       // `SuperBlock::Decode` refuses every other topology - and kept
       // because it is a durable *format* fact a client reads off the
       // volume, not a setting that varies. A literal, because there is no
       // field to read it from.
       << " wal_topology=single"
       // The core serving this session (PW6, docs/inflight/in-progress/workplan-peer-writer.md).
       // Every core listens (AT-S8), so the kernel picks the accepting core and
       // a client cannot choose it (PW5), so a client that needs to know -
       // the per-core writer benchmark, an operator reading a refusal - must
       // be able to ask. Constant per transaction; per session too unless a
       // transaction placer moves it between transactions (BL-S2).
       << " core=" << core_id_
       // §5d: delete-marked catalog rows the horizon-gated purge retired
       // *this* mount. Its sibling `catalog_marks_finalized` below counts a
       // previous mount's leftovers, and is part of the recovery report;
       // this one is live dispatcher state and prints unconditionally.
       << " catalog_marks_purged=" << catalog_marks_purged_
       << " catalog_purge_sweeps=" << catalog_purge_sweeps_;

    // **The Cabin controller's own numbers** (PHY06), which `SHOW
    // CABIN_OPTIMIZER`'s header printed; its managed entries are
    // `sys.cabin_optimizer`'s rows. The switch is always printed; the rest
    // only where an instance runs a controller, under the view latch the
    // entries are read under (AT-S8). The executor's counters are what it
    // applied, beside what the controller decided.
    section("cabin_optimizer");
    os << " cabin_optimizer=" << (cabin_optimizer_enabled() ? "on" : "off");
    {
        const LatchGuard latched(cabin_optimizer_view_latch_);
        if (cabin_controller_ != nullptr) {
            os << " cabin_optimizer_managed=" << cabin_controller_->managed_count()
               << " cabin_optimizer_pages_committed=" << cabin_controller_->pages_committed()
               << " cabin_optimizer_page_budget=" << cabin_controller_->config().page_budget;
        }
        if (cabin_executor_ != nullptr) {
            const exec::CabinOptimizerExecutor::Counters& c = cabin_executor_->counters();
            os << " cabin_optimizer_ticks=" << c.ticks << " cabin_optimizer_creates=" << c.creates
               << " cabin_optimizer_extends=" << c.extends << " cabin_optimizer_heals=" << c.heals
               << " cabin_optimizer_drops=" << c.drops
               << " cabin_optimizer_deferred=" << c.builds_deferred
               << " cabin_optimizer_failures=" << c.build_failures;
        }
    }

    // **The join planner's statistics** (plan-stats.md §6): what the
    // published snapshot holds and what the steward has done. Absent where
    // the dispatcher has no store (a hand-built one).
    if (plan_stats_ != nullptr) {
        section("plan_stats");
        const std::shared_ptr<const stats::PlanStatsSnapshot> snapshot = plan_source_.Pin();
        const stats::PlanStats::Counters c = plan_stats_->counters();
        os << " plan_stats_generation=" << (snapshot != nullptr ? snapshot->generation() : 0)
           << " plan_stats_relations=" << (snapshot != nullptr ? snapshot->relations() : 0)
           << " plan_stats_posteriors=" << (snapshot != nullptr ? snapshot->posteriors() : 0)
           << " plan_stats_part_drops=" << c.part_drops << " plan_stats_seeds=" << c.seeds
           << " plan_stats_seed_pages=" << c.seed_pages
           << " plan_stats_publishes=" << c.publishes
           << " plan_stats_steward_us_max=" << c.steward_ns_max / 1000;
    }
    // This core's order memo (plan-stats.md PS6): the answering core's
    // counts, not the instance's.
    section("plan_memo");
    os << " plan_memo_hits=" << plan_memo_.hits() << " plan_memo_misses=" << plan_memo_.misses();

    // **The Cabin store's instance-wide half** (cabin.md §6a), which `SHOW
    // CABINS`' header printed and no `sys.cabins` row can carry: a walk
    // declines to bank a set while its view could still be contradicted,
    // and that is the transaction's property, not a Cabin's. With the cap's
    // refusals they separate "nobody probed this column" from "every probe
    // that would have recorded was refused". Absent where no store runs.
    section("cabins");
    if (cabins_ != nullptr) {
        os << " cabins_unbankable_views=" << cabins_->stats().unbankable_views
           << " cabins_cap_refusals=" << cabins_->stats().cap_refusals;
    }

    // FM10 (docs/inflight/in-progress/workplan-multi-free-map.md): what the allocation map
    // costs, printed unconditionally because a one-region database's `1`
    // is the answer that says the multi-page map is costing nothing here.
    // `map_pages_resident` below twice `map_regions` is FM6's saving made
    // visible: a database with no Waystone directory builds no headerless
    // bitmap at all, and `headerless_pages=0` is the fact that lets
    // IsHeaderless answer with no lookup on the fault, write-back and
    // WAL-gate paths.
    section("map");
    const auto map = page_store_.map_residency();
    os << " map_regions=" << map.regions << " map_pages_resident=" << map.resident_pages
       << " map_coverage_ids=" << map.coverage_ids
       << " headerless_pages=" << (map.has_headerless ? 1 : 0);

    // **DROP TABLE page reclamation** (BF-R11): the store's counts, beside
    // the map's, and the reclaim's own where an instance runs one. A
    // plateau in `pages_allocated` with `pages_reused` rising is what says
    // a create-drop workload reuses rather than grows - the undo pages'
    // precedent below.
    section("allocation");
    const auto allocation = page_store_.allocation_counters();
    os << " pages_allocated=" << allocation.allocated << " pages_freed=" << allocation.freed
       << " pages_reused=" << allocation.reused;
    if (reclaim_counters_ != nullptr) {
        os << " reclaim_pending=" << reclaim_counters_->pending.load(std::memory_order_relaxed)
           << " reclaim_deferred=" << reclaim_counters_->deferred.load(std::memory_order_relaxed)
           << " reclaim_skipped=" << reclaim_counters_->skipped.load(std::memory_order_relaxed)
           << " reclaim_refused=" << reclaim_counters_->refused.load(std::memory_order_relaxed);
    }
    // The buffer pool (BE-R2, `eviction.md` EV9): its budget and what it
    // holds, then what reclaim has cost. `pool_batch_steps` over
    // `pool_batches_inline` is the inline walk's mean, which BE-R2 bounds;
    // it and `pool_batch_steps_background` over `pool_slots` are the
    // hand's laps (BG-S1).
    section("pool");
    const auto pool = page_store_.pool_counters();
    os << " pool_budget=" << pool.budget << " pool_resident=" << pool.resident
       << " pool_slots=" << pool.slots << " pool_hits=" << pool.hits
       << " pool_misses=" << pool.misses << " pool_reclaimed_inline=" << pool.reclaimed_inline
       << " pool_reclaimed_background=" << pool.reclaimed_background
       << " pool_batches_inline=" << pool.batches_inline
       << " pool_batches_background=" << pool.batches_background
       << " pool_batches_partial=" << pool.batches_partial
       << " pool_batch_steps=" << pool.batch_steps
       << " pool_batch_steps_background=" << pool.batch_steps_background
       << " pool_dirty_queued=" << pool.dirty_queued
       << " pool_dirty_drained=" << pool.dirty_drained << " pool_refused=" << pool.refused;

    // The undo purge's two numbers (docs/inflight/in-progress/workplan-undo-purge.md UP3):
    // live pages plateauing under a write-heavy loop is the feature, and
    // the recycle count is what proves the plateau came from reuse rather
    // than idleness. Absent without a manager, like the transaction rows.
    section("undo");
    if (txn_ != nullptr) {
        os << " undo_pages_live=" << txn_->undo().LivePages()
           << " undo_pages_recycled=" << txn_->undo().PagesRecycled();
    }

    // **The device syncs, read off counters that already increment** (XD0,
    // `instructions/v2.7.1/measurement-xd.md`; no atomic and no path added).
    //
    // `wal_syncs` is the total: the manager's own syncs plus the writer
    // thread's. Under one stream a peer performs none - it flushes through
    // core 0's stream and waits on core 0's writer - so its `wal_syncs` and
    // `wal_interval_syncs` are structurally 0 (AR0 M0) and the instance's
    // sync cost reads on core 0. `wal_sync_failures` is not: a peer's flush
    // of the shared ring and its wait on the writer can fail, and a failed
    // wait counts on core 0 too.
    //
    // `wal_interval_syncs` counts D3's loss-window ticks on both arms
    // (handed to the writer, or inline where there is none), so one rule
    // holds on every core: `wal_syncs - wal_interval_syncs` is the part
    // somebody was parked on. On core 0 the interval count is *requests*
    // and the writer's *completions*, so they differ by the sync in flight
    // and any pair the writer coalesced - bounded and small.
    //
    // Absent where the dispatcher has no WAL - the absent-rather-than-
    // zeroed rule - which is an in-process dispatcher, never a served core.
    section("wal");
    if (wal_ != nullptr) {
        const wal::WalStats& wal_stats = wal_->stats();
        // The log's durable end - the instance's one stream, so every core
        // prints the same number. What a point-in-time restore's target is
        // written in (BR-S7): an LSN noted before an operation to undo.
        os << " wal_durable_lsn=" << wal_->durable_lsn()
           << " wal_syncs=" << (wal_stats.syncs + wal_->writer_syncs())
           << " wal_interval_syncs=" << wal_stats.interval_syncs
           << " wal_sync_failures="
           << (wal_stats.sync_failures + wal_->writer_sync_failures());
        // **Fail-stop** (`wal.md` §6-5), on every core - the stream is the
        // instance's - and only once it has happened: an absent field is the
        // running log, a present one is why every write is refused.
        if (wal_->stopped()) os << " wal_stopped=1";
        // **Recycling** (BC-R6), on core 0 alone, the `wal_syncs` rule: the
        // removal is core 0's writer's, so a peer has no number to give.
        if (!wal_->attached()) {
            os << " wal_first_segment=" << wal_->stream()->device()->first_segment()
               << " wal_segments_removed=" << wal_->stream()->device()->segments_removed()
               << " wal_remove_failures=" << wal_->recycle_failures();
        }

        // **How many commits this core's fsyncs are amortised over** (AF-T5):
        // one sync resolves a batch, and a core with one committing session
        // pays a sync per commit (`kds.conf.sample`: "a batch of one is a
        // batch"). The batch is the fact; a session count is only a proxy.
        //
        // Printed as the two counters plus their quotient rather than the
        // quotient alone: an operator reading a live server needs to see
        // whether the batches are few and large or many and small, and a
        // mean over a whole uptime hides a phase change. `1.000` is the
        // cliff - every D2 commit paid its own device sync.
        //
        // Per core, like every other row in this reply: `SHOW META` answers
        // from the dispatcher the session is on, so reading a peer's batch
        // needs a session there (the kernel picks which core) - said out loud
        // because the same limitation is what stopped `wal_syncs` from
        // answering this question, and it has not gone away.
        os << " wal_group_commits=" << wal_stats.group_commits
           << " wal_group_batches=" << wal_stats.group_batches
           << " wal_mean_group_batch=" << std::fixed << std::setprecision(3)
           << wal_stats.mean_group_batch_size() << std::defaultfloat
           // The spread beside the mean, because the mean cannot say
           // whether the amortisation was the same every time - and that
           // is the whole question about D2's measured `n/2`
           // (`manager.hpp`'s `group_batch_max`). `min` is over non-empty
           // batches, so `min == max` is a batch that never varied.
           << " wal_group_batch_max=" << wal_stats.group_batch_max
           << " wal_group_batch_min=" << wal_stats.group_batch_min;

        // **The ring's backpressure, and it is per core even though the
        // ring is not** (AR0 M0). Under one stream every core appends into
        // the same ring, so a stall here was caused by the instance's
        // producers together - but it was *paid* by this core's appender,
        // on its own reactor thread, and that is the quantity an operator
        // is reading for. Summing the cores gives the instance's stalls;
        // the spread across them says whether one core is starving.
        //
        // Two counters rather than one, because they are two events (see
        // `manager.hpp`): `wal_ring_full` is a stall the appender paid a
        // flush for and got through, `wal_ring_full_refusals` an append
        // that ran out of attempts and was refused. The first is a load
        // reading, the second a sizing bug.
        os << " wal_ring_full=" << wal_stats.ring_full_drains
           << " wal_ring_full_refusals=" << wal_stats.ring_full_refusals;

        // **The segment roll** (BQ-S1, `workorder-bq-wal-segment-roll.md`):
        // the instance's totals, printed on every core like `wal_stopped`,
        // since the stream is one object - so they are never summed across
        // cores. `wal_append_lsn` is the stream's byte position; its delta
        // over a window is the log volume written in it.
        const wal::WalStream& stream = *wal_->stream();
        os << " wal_append_lsn=" << stream.append_lsn();
        for (std::size_t i = 0; i < wal::kRollStatCount; ++i) {
            const std::string_view name = wal::kRollStatNames[i];
            const std::uint64_t v = stream.roll_stat(static_cast<wal::RollStat>(i));
            os << ' ' << name << '=' << (name.ends_with("_us") ? v / 1000 : v);
        }
    }

    // The id-lease, cross-owner commit, shipping, 2PC, access-batch,
    // ownership, range-split and Cabin-scope counter blocks retired with
    // their events (AT-S6, AT-S7, AT-S9, AT-S10, AT-S10b); their history is at cf889b63.

    // Group accounting against wall time (`docs/spec/sched.md` §4's last bullet;
    // `sched/scheduler.hpp`'s accessors carry the argument for why there are
    // two counters per group). `sched_wall_us - sum(sched_*_polled_us)` is
    // the reactor time charged to no group: the idle block, the WAL drain's
    // `fdatasync`, timer callbacks, the io drain. Absent rather than zeroed
    // where no reactor is attached, the rule the recovery block follows.
    section("sched");
    if (scheduler_view_ != nullptr) {
        os << " sched_wall_us=" << scheduler_view_->run_wall_ns() / 1000
           << " sched_iterations=" << scheduler_view_->iterations()
           // The wake path and the park rule, from this reactor's side
           // (§7). `sched_idle_blocks` is how often it slept with the flag
           // raised, and `sched_parked_idle_blocks` the blocks taken with
           // tasks still queued, every one of which was a spin before
           // "parked is not ready".
           << " sched_idle_blocks=" << scheduler_view_->idle_blocks()
           << " sched_parked_idle_blocks=" << scheduler_view_->parked_idle_blocks()
           // The block's *duration*, and the wake traffic around it (D7 of
           // `instructions/v2.3.0-reactor-wake.md`). With
           // `sched_idle_block_us` present,
           // `sched_wall_us - sum(sched_*_polled_us) - sched_idle_block_us`
           // is the time charged to nobody that was not sleep - which is
           // the reading the group-accounting gap actually needs, and the
           // one the field above could not give on its own.
           //
           // `sched_wakes_sent` is the whole instance's, so it repeats on
           // every core and equals the sum of their `sched_wakes_received`.
           << " sched_idle_block_us=" << scheduler_view_->idle_block_ns() / 1000
           << " sched_wakes_sent=" << scheduler_view_->wakes_sent()
           << " sched_wakes_received=" << scheduler_view_->wakes_received();
        // Indexed by the enum, not paired with it: a fourth group would
        // then fail to compile here rather than print as two.
        static constexpr const char* kGroupNames[sched::kNumSchedulingGroups] = {
            "foreground", "maintenance", "system"};
        for (int i = 0; i < sched::kNumSchedulingGroups; ++i) {
            const auto group = static_cast<sched::SchedulingGroup>(i);
            os << " sched_" << kGroupNames[i]
               << "_polled_us=" << scheduler_view_->polled_ns_total(group) / 1000
               << " sched_" << kGroupNames[i]
               << "_polls=" << scheduler_view_->polls_total(group)
               << " sched_" << kGroupNames[i]
               << "_consumed_us=" << scheduler_view_->consumed_ns(group) / 1000
               << " sched_" << kGroupNames[i]
               << "_longest_poll_us=" << scheduler_view_->longest_poll_ns(group) / 1000
               << " sched_" << kGroupNames[i]
               << "_polls_over_10ms=" << scheduler_view_->polls_over_10ms(group)
               << " sched_" << kGroupNames[i]
               << "_polls_over_100ms=" << scheduler_view_->polls_over_100ms(group);
        }
        // BI-S1: how long one iteration held this reactor, apart for the
        // iterations a checkpoint run of this core overlapped.
        const auto print_holds = [&os](const char* where, const sched::Scheduler::IterationHolds& h) {
            os << " sched_hold_" << where << "_longest_us=" << h.longest_ns / 1000
               << " sched_hold_" << where << "_over_10ms=" << h.over_10ms
               << " sched_hold_" << where << "_over_100ms=" << h.over_100ms
               << " sched_hold_" << where << "_over_1s=" << h.over_1s;
        };
        print_holds("in_run", scheduler_view_->holds_inside_run());
        print_holds("out_run", scheduler_view_->holds_outside_run());
        os << " sched_longest_unpolled_us=" << scheduler_view_->longest_unpolled_ns() / 1000
           << " sched_unpolled_over_100ms=" << scheduler_view_->unpolled_over_100ms();
    }

    // BI-S1's run record for this core (`sched/checkpoint_census.hpp`): the
    // runs it took and its last run whole, per phase.
    section("checkpoint");
    {
        const CheckpointCensus::Slot& ckpt = CheckpointCensus::Of(core_id_);
        const auto load = [](const std::atomic<std::uint64_t>& v) {
            return v.load(std::memory_order_relaxed);
        };
        os << " checkpoint_runs=" << load(ckpt.run_edges) / 2
           << " checkpoint_start_refused=" << load(ckpt.start_refused)
           << " checkpoint_last_completed=" << (ckpt.last_completed.load(std::memory_order_relaxed) ? 1 : 0)
           << " checkpoint_last_wall_us=" << load(ckpt.last_wall_ns) / 1000
           << " checkpoint_last_step_max_us=" << load(ckpt.last_step_max_ns) / 1000
           << " checkpoint_last_step_page_write_max_us="
           << load(ckpt.last_step_page_write_max_ns) / 1000
           << " checkpoint_last_steps=" << load(ckpt.last_steps)
           << " checkpoint_last_pages=" << load(ckpt.last_pages)
           << " checkpoint_last_dirty_at_start=" << load(ckpt.last_dirty_at_start)
           << " checkpoint_last_gate_parks=" << load(ckpt.last_gate_parks)
           << " checkpoint_last_end_parks=" << load(ckpt.last_end_parks);
        for (std::size_t i = 0; i < kCheckpointPhaseCount; ++i) {
            os << " checkpoint_last_" << kCheckpointPhaseNames[i]
               << "_sum_us=" << load(ckpt.last_sum_ns[i]) / 1000 << " checkpoint_last_"
               << kCheckpointPhaseNames[i] << "_max_us=" << load(ckpt.last_max_ns[i]) / 1000;
        }
    }

    // **Every core's load, from any core** (BL-S1, `sched/core_load.hpp`):
    // the one block here that does not describe only the asking session's
    // core. `load_c<k>_*` for each of the instance's cores; none at one core,
    // where nothing writes them (BL-R9).
    section("load");
    for (std::uint32_t k = 0; k < sched::CoreLoad::Cores(); ++k) {
        const sched::CoreLoad::Slot& s = sched::CoreLoad::Of(k);
        const auto r = [](const auto& v) { return v.load(std::memory_order_relaxed); };
        os << " load_c" << k << "_busy_ppm=" << r(s.busy_ppm) << " load_c" << k
           << "_foreground_ppm=" << r(s.foreground_ppm) << " load_c" << k
           << "_queued_foreground=" << r(s.queued_foreground) << " load_c" << k
           << "_sessions=" << r(s.sessions) << " load_c" << k << "_active_sessions="
           << r(s.active_sessions) << " load_c" << k << "_heads=" << r(s.heads)
           << " load_c" << k << "_inside=" << r(s.inside) << " load_c" << k
           << "_moves_out=" << r(s.moves_out);
        // **What the core waited on** (BL-S1's holder split): the contention
        // block's per-core slot (`base/contention.hpp`), which that block
        // prints summed. Only the latches it waited on; slot 0 also carries
        // the threads that declared no core (the WAL writer among them).
        const detail::ContentionSlot& c = detail::g_contention[k % detail::kContentionSlots];
        for (std::size_t i = 0; i < kLatchKindCount; ++i) {
            const std::uint64_t ns = r(c.wait_ns[i]);
            if (ns == 0) continue;
            os << " load_c" << k << "_waited_" << kLatchKindNames[i] << "_us=" << ns / 1000;
        }
        // BQ-S1's per-core share of the stream latch: the drain's part of
        // `_waited_wal_stream_us`, and the waits a segment roll covered;
        // BM-S1's window lookups (`LookupCommit`, a scan beside a writer's
        // per-row question among them).
        for (const Tally t : {Tally::kWalDrainLatchWaitNs, Tally::kWalRollBlockedWaits,
                              Tally::kWalRollBlockedWaitNs, Tally::kWindowLookups}) {
            const std::uint64_t v = r(c.tallies[static_cast<std::size_t>(t)]);
            if (v == 0) continue;
            os << " load_c" << k << '_' << kTallyNames[static_cast<std::size_t>(t)] << '='
               << (IsNsTally(t) ? v / 1000 : v);
        }
    }

    // The last recovery, for the operator who has to answer "what did the
    // restart do" (RC09, `docs/spec/wal.md` §13). Absent rather than zeroed when no
    // report is installed - a dispatcher built without one (every socket-free
    // test) has not "recovered nothing", it has no answer, and printing zeroes
    // would be an answer.
    section("recovery");
    if (recovery_ != nullptr) {
        // **Where the pass ran** (AR0 M0). Printed only where it was not
        // here: on core 0 its absence means "these numbers are mine".
        if (!recovery_->ran) {
            os << " recovery_by=core0";
        }
        os << " recovery_records=" << recovery_->records
           << " recovery_committed=" << recovery_->winners
           << " recovery_rolled_back=" << recovery_->transactions_rolled_back
           << " recovery_compensations=" << recovery_->compensations
           << " recovery_redo_applied=" << recovery_->redo_applied
           << " recovery_pages_healed=" << recovery_->pages_healed
           << " recovery_torn_tail=" << (recovery_->torn_tail ? 1 : 0);
        // **Redo's floor** (BC-R2), printed only when it raised the start:
        // the two LSNs say how far below the anchor analysis had reached,
        // and that redo did not go there.
        if (recovery_->redo_start_floored_from != 0) {
            os << " recovery_redo_start=" << recovery_->redo_start
               << " recovery_redo_start_recomputed=" << recovery_->redo_start_floored_from;
        }
        // **A restore this mount completed** (BR-R2), absent when none was
        // owed: which backup, where its replay started, and the stop it
        // passed - the mount refuses to open short of it, so a printed stop
        // is a stop reached. Core 0's: a peer recovered nothing.
        if (recovery_->restore_backup_id != 0) {
            os << " recovery_restore_backup=" << recovery_->restore_backup_id
               << " recovery_restore_start=" << recovery_->restore_redo_start
               << " recovery_restore_stop=" << recovery_->restore_stop_lsn;
        }
        if (recovery_->timings.timed) {
            os << " recovery_analysis_us=" << recovery_->timings.analysis_ns / 1000
               << " recovery_redo_us=" << recovery_->timings.redo_ns / 1000
               << " recovery_high_water_us=" << recovery_->timings.high_water_ns / 1000
               << " recovery_undo_us=" << recovery_->timings.undo_ns / 1000
               << " recovery_checkpoint_us=" << recovery_->checkpoint_ns / 1000;
        }
        // Catalog mutations log the ordinary record types and DDL runs under
        // a real transaction (RV3), so `catalog_recovered` is 1;
        // `relations_missing_pages` stays as the audit, and reads 0.
        os << " recovery_relations_checked=" << recovery_->relations_checked
           << " recovery_relations_missing_pages=" << recovery_->relations_missing_pages
           << " catalog_recovered=1"
           // DT10: delete-marked catalog rows a previous mount left
           // behind, retired before the listener bound. Since D2 every
           // DROP is transactional, so a non-zero here means the previous
           // mount ended - cleanly or not - with marks some reader or a
           // crash kept the §5d purge from retiring.
           << " catalog_marks_finalized=" << recovery_->catalog_marks_finalized
           // Transactional (DT7) and durable (RV3): a committed DDL survives
           // a crash by redo, an uncommitted one is rolled back by the
           // catalog hook's undo records. A list of statement names rather
           // than `=1`, so a statement that leaves it can be seen to.
           << " ddl_transactional=create-table,drop-table,create-index,drop-index"
           << " ddl_durable=1";
        // RC07: what the mount could resume enforcing, and the honest
        // remainder. A surviving declaration whose directory could not be
        // rebuilt is counted here and left *out* of the registry, so
        // SHOW ASSERTIONS reports `enforcing=0` for it rather than a
        // constraint that would admit every write. Core 0's mount resumes
        // the instance's registry (AT-S5d), so on a peer both are 0 for
        // `recovery_by=core0`'s reason.
        os << " recovery_assertions_enforcing=" << recovery_->assertions_enforcing
           << " recovery_assertions_unrecovered=" << recovery_->assertions_unrecovered;
    }
    // **The backup block** (BR): the instance's one control, so every core
    // prints the same state; absent where nothing mounted a volume.
    section("backup");
    if (backup_ != nullptr) backup_->AppendMeta(os);
    if (archiver_ != nullptr) archiver_->AppendMeta(os);
    // **`sys.lock_waits`' store** (BO-S3): the instance's, summed over every
    // core's part, as the contention block below is. `overwritten` is the
    // rows a full ring gave up for newer ones and `dropped` the rows no ring
    // could take - what the view no longer holds, and why.
    section("lock_waits");
    {
        const stats::LockWaitLog::Totals waits = stats::LockWaitLog::Read();
        os << " lock_waits_recorded=" << waits.recorded
           << " lock_waits_overwritten=" << waits.overwritten
           << " lock_waits_dropped=" << waits.dropped;
    }
    // **`sys.statement_stats`' store** (BO-S5), the instance's: new shapes
    // a full part could not take, which the view therefore never shows.
    section("statement_stats");
    os << " statement_stats_dropped=" << stats::StatementStats::Dropped();
    // **BA's census block** (BA-R0, BA-S2; `base/contention.hpp`): the
    // instance's, summed over every core when read - unlike the WAL block
    // above, which is this core's. Every latch kind is printed, `other`
    // included, so a latch no row describes still shows its waits; each
    // count is the contended acquisitions only.
    section("contention");
    const Contention::Snapshot contention = Contention::Read();
    for (std::size_t i = 0; i < kLatchKindCount; ++i) {
        os << " contention_" << kLatchKindNames[i] << "_waits=" << contention.waits[i]
           << " contention_" << kLatchKindNames[i] << "_wait_us=" << contention.wait_ns[i] / 1000;
    }
    for (std::size_t i = 0; i < kTallyCount; ++i) {
        const bool ns = IsNsTally(static_cast<Tally>(i));
        os << " contention_" << kTallyNames[i] << "="
           << (ns ? contention.tallies[i] / 1000 : contention.tallies[i]);
    }
    for (std::size_t i = 0; i < kLongestCount; ++i) {
        os << " contention_" << kLongestNames[i] << "=" << contention.longest_ns[i] / 1000;
    }
    sections.back().second = os.str();
    return sections;
}

DispatchOutcome CommandDispatcher::HandleSetCabinOptimizer(std::string_view rest) {
    // PO8's runtime kill switch (workplan PHY05). Non-destructive in both
    // directions by construction: PHY04's cadence task reads this flag at
    // every batch boundary - before the tick's snapshot, between actions,
    // and between a build's pages - so OFF lands mid-build and the build
    // discards cleanly, nothing having been committed. An optional '=' is
    // accepted because both spellings read naturally on a terminal.
    auto [value, extra] = SplitFirstToken(Trim(rest));
    if (IEquals(value, "=")) std::tie(value, extra) = SplitFirstToken(Trim(extra));
    if (!Trim(extra).empty()) {
        return {"ERR SET CABIN_OPTIMIZER takes exactly one value, on or off", false};
    }
    if (IEquals(value, "ON")) {
        set_cabin_optimizer_enabled(true);
    } else if (IEquals(value, "OFF")) {
        set_cabin_optimizer_enabled(false);
    } else {
        return {"ERR SET CABIN_OPTIMIZER takes on or off, not '" + std::string(value) + "'",
                false};
    }
    return {std::string("OK cabin_optimizer=") + (cabin_optimizer_enabled() ? "on" : "off"),
            false};
}


// `t` or `ns.t` in a **raw argument** (AF-T3). The debug surface's commands
// take a bare token rather than a parse - `DESCRIBE`, the
// bare `CREATE TABLE <name>` form - and a qualifier has to mean the same
// thing there as in the grammar, or the two surfaces disagree about what a
// name is. One dot deep, exactly as `Parser::ParseQualifiedName`.
//
// A trailing or leading dot is left alone: the halves come back as written
// and the catalog refuses the empty one by name, which beats this helper
// inventing a syntax error for a surface that has no byte offsets.
std::pair<std::string_view, std::string_view> SplitQualifiedArg(std::string_view arg) {
    const std::size_t dot = arg.find('.');
    if (dot == std::string_view::npos) return {std::string_view{}, arg};
    return {arg.substr(0, dot), arg.substr(dot + 1)};
}

DispatchOutcome CommandDispatcher::HandleCreateTable(std::string_view args,
                                                     Session& session) {
    // D2 (workplan-rv3-catalog-recovery.md): every DDL statement runs
    // under a real transaction - the session's, or an implicit one this
    // scope opens and FinishDdlStatement resolves - so a crash mid-DDL
    // has a loser recovery can roll back.
    return InDdlStatement(session, [&](WriteScope& scope) -> DispatchOutcome {
        if (args.empty()) {
            return {"ERR CREATE TABLE requires a name", false};
        }
        // AF-T3: the same qualifier the SQL form takes, so `CREATE TABLE
        // orders.t` means one thing on both surfaces.
        const auto [qualifier, name] = SplitQualifiedArg(args);

        // Before the collision check, for `HandleCreateTableSql`'s reason:
        // the `EXISTS` arm below would otherwise answer a statement whose
        // namespace does not exist, absorbing the qualifier instead of
        // verifying it.
        auto create_ns = ResolveNames(session, [&](const txn::ReadView* v) {
            return ResolveCreateNamespace(qualifier, /*byte_offset=*/0, session, v);
        });
        if (!create_ns.ok()) {
            return {ErrorReply(create_ns.status()), false, 0, create_ns.status()};
        }

        if (auto exists = ExistingRelationReply(create_ns.value(), name);
            exists.has_value()) {
            return *exists;
        }

        catalog::Schema schema;
        DdlScope ddl = DdlScopeFor(scope);
        auto oid = catalog_.CreateTable(create_ns.value(), name, schema, ddl.trx_id, ddl.sink());
        NoteDdlRows(ddl);  // before the status, for the partial-write reason above
        if (!oid.ok()) {
            return {ErrorReply(oid.status()), false, 0, oid.status()};
        }
        return {"CREATED oid=" + std::to_string(oid.value()), false};
    });
}

namespace {

// The one spelling of a successful CREATE INDEX (docs/spec/client-manual.md).
// `entries=0` is a literal the manual documents, and the backfill over a
// populated relation makes it false; it stays until the field is dropped or
// counted - either is client-visible, so neither is done in passing.
std::string CreatedIndexReply(std::string_view name, std::string_view table,
                              catalog::Oid index_oid, PageId root, std::uint16_t key_width,
                              std::uint16_t entry_width, const std::vector<std::string>& warnings) {
    std::ostringstream os;
    os << "CREATED INDEX name=" << name << " on=" << table << " index_oid=" << index_oid
       << " root_page=" << root << " key_width=" << key_width << " entry_width=" << entry_width
       << " entries=0";
    for (const std::string& warning : warnings) os << "\\n" << "WARN " << warning;
    return os.str();
}

}  // namespace

DispatchOutcome CommandDispatcher::HandleIndex(std::string_view line,
                                               Session& session) {
    // Parsed before the DDL scope exists: a statement that does not parse
    // costs no transaction id (PW4's philosophy - refuse before resources),
    // and the parse is pure.
    parser::Parser parser(line);
    auto parsed = parser.Parse();
    if (!parsed.ok()) {
        return {ErrorReply(parsed.status()), false, 0, parsed.status()};
    }
    if (!std::holds_alternative<parser::IndexStmt>(parsed.value())) {
        return {"ERR expected a CREATE INDEX or DROP INDEX statement", false};
    }
    const parser::IndexStmt stmt = std::get<parser::IndexStmt>(std::move(parsed.value()));
    // BY-Q4 (a): autocommit only. Refused before a scope opens, so an
    // explicit transaction is not poisoned by asking - `PURGE`'s shape.
    if (stmt.reindex && session.in_explicit_txn()) {
        const Status s = Status::NotImplemented(
            "REINDEX inside a transaction is not implemented; run it in autocommit (byte 0)");
        return {ErrorReply(s), false, 0, s};
    }

    return InDdlStatement(session, [&](WriteScope& scope) -> DispatchOutcome {
        // ---- AT-S5e: the relation `X`, for both statements --------------
        //
        // **Taken before the first catalog write, on whichever core the
        // session is**, as `DROP TABLE` has taken it since AO-S6e-b. A
        // writer of the relation holds its `IX` from before it decides
        // anything from the relation's indexes (`InsertParsed`,
        // `DeclaredWriteBorrow`) until its transaction decides, so the grant
        // is the moment no writer is mid-statement, and every writer that
        // arrives afterwards parks on the table's slot until this decides -
        // then re-runs, asks the schema word at its task boundary, and
        // resolves the index list this statement left. Without the hold, a row
        // written on another core during a `CREATE INDEX` backfill would be
        // missing from the finished index.
        //
        // A name that does not resolve takes no borrow: a create is refused
        // below, a drop falls through to the refusal `exec::DropIndex` owns,
        // with its byte position. An index name is a resolution route like a
        // relation's: an index another transaction's open `CREATE INDEX`
        // wrote is not found, so no drop delete-marks a row nobody decided.
        std::optional<catalog::Oid> relation;
        Status miss;  // why a create's relation did not resolve
        if (stmt.drop || stmt.reindex) {
            relation = ResolveNames(session, [&](const txn::ReadView* v) {
                auto ix = catalog_.FindIndexByName(stmt.index_name, v);
                return ix.ok() ? std::optional<catalog::Oid>(ix.value().table_oid)
                               : std::nullopt;
            });
        } else {
            auto oid = ResolveNames(session, [&](const txn::ReadView* v) {
                return catalog_.ResolveRelation(stmt.schema, stmt.table_name,
                                                session.default_namespace(),
                                                stmt.table_byte_offset, v);
            });
            if (oid.ok()) {
                relation = oid.value();
            } else {
                miss = oid.status();
            }
        }
        if (relation.has_value()) {
            if (std::optional<Status> held = BorrowRelationForDdl(scope.txn, *relation);
                held.has_value()) {
                return {ErrorReply(*held), false, 0, *held};
            }
        }

        // The name asked again under the `X`, at latest state with this
        // DDL's own id: another transaction's open create is still not found,
        // this transaction's own is, and a drop that committed during the
        // wait reads as gone. The statement's view would be older than the
        // wait and answer "being dropped" about a drop already committed.
        const auto latest_name_view = [&](const DdlScope& ddl) -> std::optional<txn::ReadView> {
            if (txn_ == nullptr) return std::nullopt;
            return txn_->MintCheckView(ddl.txn != nullptr ? ddl.trx_id : txn::kNoTrxId);
        };

        if (stmt.reindex) {
            // **A drop and a create in one transaction, under the `X` above**
            // (BY-R6): the old row delete-marked, the new one written under
            // the same name (BY-R8), and both trees settled at the decide -
            // the old one queued at commit, the new one at any other ending
            // (BY-R2). Only an index on the relation held above is rebuilt
            // (`exec::DropIndex`'s contract).
            DdlScope ddl = DdlScopeFor(scope);
            catalog::CatalogRowChange change;
            catalog::CatalogRowRef created_row;
            const std::optional<txn::ReadView> name_view = latest_name_view(ddl);
            auto result = exec::ReindexIndex(
                catalog_, page_store_, stmt, relation,
                name_view.has_value() ? &*name_view : nullptr, ddl.trx_id,
                ddl.txn != nullptr ? &change : nullptr,
                ddl.txn != nullptr ? &created_row : nullptr, wal_, txn_);
            NoteIndexRowDropped(ddl, change);
            NoteRowWritten(ddl, created_row);
            if (!result.ok()) return {ErrorReply(result.status()), false, 0, result.status()};
            const catalog::Oid old_oid = result.value().replaced_oid;
            NoteOwedIndex(scope, *relation, old_oid, /*created=*/false);
            NoteOwedIndex(scope, *relation, result.value().index_oid, /*created=*/true);
            if (logging(LogLevel::kInfo)) {
                log_->Info("ddl", "rebuilt index '" + stmt.index_name + "'");
            }
            std::ostringstream os;
            os << "REINDEXED INDEX name=" << stmt.index_name
               << " index_oid=" << result.value().index_oid << " replaced=" << old_oid
               << " root=" << result.value().root_page_id;
            return {os.str(), false};
        }

        if (stmt.drop) {
            // **Isolated on every core since AT-S5e.** DT5 shipped this as
            // atomic *and isolated* on the strength of `SHOW INDEXES`
            // filtering, which was wrong - index maintenance reads the list
            // with a null view, so another session's INSERT wrote no entry
            // for an index whose drop had not committed - and DT9 closed it
            // at the read: an unfiltered catalog read counts a delete-mark
            // only once its deleter is no longer in flight (`catalog.cpp`'s
            // `ScanAll`). A writer on any core
            // resolves the index while the drop is open since AX-S2; the
            // relation `X` above keeps it from writing until the drop
            // decides, and the decide moving the schema word before it
            // releases (`Transaction::NoteWroteCatalog`) makes it re-resolve
            // against whichever way the drop went.
            DdlScope ddl = DdlScopeFor(scope);
            catalog::CatalogRowChange change;
            const std::optional<txn::ReadView> name_view = latest_name_view(ddl);
            // Only an index on the relation held above (`exec::DropIndex`'s
            // contract): a name that took no hold, or that another
            // transaction moved since, writes nothing.
            auto index_oid = exec::DropIndex(catalog_, stmt, relation,
                                             name_view.has_value() ? &*name_view : nullptr,
                                             ddl.trx_id, ddl.txn != nullptr ? &change : nullptr);
            NoteIndexRowDropped(ddl, change);
            if (!index_oid.ok()) {
                return {ErrorReply(index_oid.status()), false, 0, index_oid.status()};
            }
            NoteOwedIndex(scope, *relation, index_oid.value(), /*created=*/false);
            std::ostringstream os;
            os << "DROPPED INDEX name=" << stmt.index_name << " index_oid=" << index_oid.value();
            if (logging(LogLevel::kInfo)) {
                log_->Info("ddl", "dropped index '" + stmt.index_name + "'");
            }
            return {os.str(), false};
        }

        // **Refused here, not by `PrepareIndexDef`'s own lookup**: an index
        // is built only over the relation borrowed above. A later lookup
        // could find one created since, which holds no `X` of this
        // statement's.
        if (!relation.has_value()) return {ErrorReply(miss), false, 0, miss};
        DdlScope create_ddl = DdlScopeFor(scope);
        catalog::CatalogRowRef created_row;
        // Resolved under the session's view (spec §5's rule: an index is a
        // schema object and CREATE INDEX is a resolution route). An index
        // must not be built against a relation the caller cannot see. The
        // view is taken after the scope opened, which counted this DDL, so
        // it is a minted one wherever a manager runs.
        const std::optional<txn::ReadView> build_view = ViewFor(session);
        auto result = exec::CreateIndex(catalog_, page_store_, stmt, session.default_namespace(),
                                        create_ddl.trx_id,
                                        create_ddl.txn != nullptr ? &created_row : nullptr,
                                        build_view.has_value() ? &*build_view : nullptr, wal_,
                                        txn_);
        // Before the status is read: a create that failed after the catalog
        // row went down still left it there.
        NoteRowWritten(create_ddl, created_row);
        if (!result.ok()) return {ErrorReply(result.status()), false, 0, result.status()};
        NoteOwedIndex(scope, *relation, result.value().index_oid, /*created=*/true);
        if (logging(LogLevel::kInfo)) {
            log_->Info("ddl", "created index '" + stmt.index_name + "' on " + stmt.table_name);
        }
        return {CreatedIndexReply(stmt.index_name, stmt.table_name, result.value().index_oid,
                                  result.value().root_page_id, result.value().key_width,
                                  result.value().entry_width,
                                  result.value().warnings),
                false};
    });
}

DispatchOutcome CommandDispatcher::HandleCabin(std::string_view line, Session& session) {
    auto parsed = parser::Parse(line);
    if (!parsed.ok()) {
        return {ErrorReply(parsed.status()), false, 0, parsed.status()};
    }
    if (!std::holds_alternative<parser::CabinStmt>(parsed.value())) {
        return {"ERR expected a CREATE CABIN or DROP CABIN statement", false};
    }
    const auto& stmt = std::get<parser::CabinStmt>(parsed.value());
    if (stmt.shape == catalog::kCabinShapeColumn) return RunCabinDdl(stmt, session.default_namespace());

    // ---- AQ-S1: an expression or predicate Cabin takes the relation `X` --
    //
    // `CREATE INDEX`'s hold (AT-S5e), for two reasons of its own. `DROP
    // COLUMN` decides under the `X` whether a Cabin's key reads the column,
    // and a create compiled against the column on another core must not land
    // its text after that answer - the text would name a dropped column and
    // fail every fill of the relation. And a writer holds its `IX` from before
    // it reads the relation's Cabins until it decides, so a Cabin appears
    // only between writers, never under one that compiled without it.
    return InDdlStatement(session, [&](WriteScope& scope) -> DispatchOutcome {
        const std::optional<catalog::Oid> relation =
            ResolveNames(session, [&](const txn::ReadView* v) {
                auto oid = catalog_.ResolveRelation(stmt.schema, stmt.table_name,
                                                    session.default_namespace(), 0, v);
                return oid.ok() ? std::optional<catalog::Oid>(oid.value()) : std::nullopt;
            });
        if (relation.has_value()) {
            if (std::optional<Status> held = BorrowRelationForDdl(scope.txn, *relation);
                held.has_value()) {
                return {ErrorReply(*held), false, 0, *held};
            }
        }
        return RunCabinDdl(stmt, session.default_namespace());
    });
}

DispatchOutcome CommandDispatcher::RunCabinDdl(const parser::CabinStmt& stmt,
                                             catalog::Oid default_namespace) {
    // One handler for both, because the two statements share a parse and a
    // reply shape and differ only in which catalog call they reach - the same
    // reason CabinStmt is one struct (ast.hpp).
    if (stmt.drop) {
        auto cabin_id =
            exec::DropCabin(catalog_, page_store_, wal_, stmt, default_namespace);
        if (!cabin_id.ok()) {
            return {ErrorReply(cabin_id.status()), false, 0, cabin_id.status()};
        }
        // The runtime half. The catalog cannot see the observed sets, and
        // they are unreachable the moment its row is gone - the compiler
        // stops emitting cabin probes for the column - so this frees memory
        // rather than protecting an answer.
        if (cabins_ != nullptr) cabins_->Forget(cabin_id.value());
        // Transactionless like the pattern and assertion routes: the
        // sys.cabins row is logged but no commit record follows it.
        if (Status s = AwaitDdlDurability(); !s.ok()) return {ErrorReply(s), false, 0, s};
        std::ostringstream os;
        os << "DROPPED CABIN on=" << stmt.table_name;
        if (stmt.shape == catalog::kCabinShapeColumn) os << '.' << stmt.column_name;
        os << " cabin_id=" << cabin_id.value();
        if (logging(LogLevel::kInfo)) {
            log_->Info("ddl", "dropped cabin " + std::to_string(cabin_id.value()) + " on " +
                                  stmt.table_name);
        }
        return {os.str(), false};
    }

    // Committed now, not the session's view: the row is written with no
    // transaction and outlives a rollback, so even the session's own
    // uncommitted relation is one it must not name.
    std::optional<txn::ReadView> committed;
    if (txn_ != nullptr) committed = txn_->MintCheckView(txn::kNoTrxId);
    auto result = exec::CreateCabin(catalog_, page_store_, wal_, stmt, default_namespace,
                                    committed.has_value() ? &*committed : nullptr);
    if (!result.ok()) {
        return {ErrorReply(result.status()), false, 0, result.status()};
    }
    if (Status s = AwaitDdlDurability(); !s.ok()) return {ErrorReply(s), false, 0, s};

    std::ostringstream os;
    // `observed=0` is printed rather than left out, because it is the thing
    // most likely to be misunderstood: creating a Cabin observes nothing and
    // accelerates nothing until traffic fills it (spec §4's miss path).
    if (stmt.shape == catalog::kCabinShapeColumn) {
        os << "CREATED CABIN on=" << stmt.table_name << '.' << result.value().key
           << " cabin_id=" << result.value().cabin_id << " column=" << result.value().col_pos
           << " observed=0";
    } else {
        // The key as the engine canonicalised it, so an operator sees what
        // a statement must spell to be served (AQ-R5, R6).
        os << "CREATED CABIN on=" << stmt.table_name << " (" << result.value().key << ")"
           << " cabin_id=" << result.value().cabin_id << " observed=0";
    }
    for (const std::string& warning : result.value().warnings) {
        os << "\\n" << "WARN " << warning;
    }
    if (logging(LogLevel::kInfo)) {
        log_->Info("ddl", "created cabin on " + stmt.table_name + " (" + result.value().key + ")");
    }
    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleAlter(std::string_view line,
                                               Session& session) {
    auto parsed = parser::Parse(line);
    if (!parsed.ok()) {
        return {ErrorReply(parsed.status()), false, 0, parsed.status()};
    }
    if (!std::holds_alternative<parser::AlterStmt>(parsed.value())) {
        return {"ERR expected an ALTER TABLE statement", false};
    }
    const auto& stmt = std::get<parser::AlterStmt>(parsed.value());
    if (stmt.action == parser::AlterAction::kDropColumn) return HandleDropColumn(stmt, session);
    if (stmt.action == parser::AlterAction::kAddColumn) return HandleAddColumn(stmt, session);
    const bool rename_column = stmt.action == parser::AlterAction::kRenameColumn;

    auto oid = ResolveAlterTarget(stmt, session);
    if (!oid.ok()) return {ErrorReply(oid.status()), false, 0, oid.status()};

    // AL4: assertions RESTRICT both forms. The stored canon is the
    // declaration's verbatim text (AS10), and the recovery-side registry
    // rebuild will re-parse it - a rename would leave an *enforcing*
    // constraint whose canon names a vanished table or column. Unlike a
    // pattern, an assertion is not allowed to die quietly. This is
    // AssertionsOnRelation()'s first live call site.
    auto restricting = exec::AssertionsOnRelation(catalog_, page_store_, oid.value());
    if (!restricting.ok()) {
        return {ErrorReply(restricting.status()), false, 0, restricting.status()};
    }
    if (!restricting.value().empty()) {
        return {"ERR assertion '" + restricting.value().front().name +
                    "' stores its declaration against this relation's current names; DROP "
                    "ASSERTION, rename, then re-declare it",
                false};
    }

    const Status renamed =
        rename_column
            ? [&] {
                  // A collision or an unstorable name is the new name's
                  // fault, so it carries that name's byte; a missing old
                  // column carries the old name's, and so does an old name
                  // matching two live columns ignoring case - resolved here
                  // first (`Schema::FindColumn`), the catalog door asking
                  // again under its hold. The door's own ambiguity refusal
                  // would take the new name's byte below; it is unreachable,
                  // since no twin can be created after this check.
                  auto access = catalog_.InitTableAccess(oid.value());
                  if (!access.ok()) return access.status();
                  if (auto old = access.value()->schema.FindColumn(
                          stmt.old_column, stmt.old_column_byte_offset);
                      !old.ok()) {
                      return old.status();
                  }
                  const Status r =
                      catalog_.RenameColumn(oid.value(), stmt.old_column, stmt.new_name);
                  const auto at = [&](std::uint32_t byte) {
                      return r.WithMessage(r.message() + " (byte " + std::to_string(byte) + ")");
                  };
                  switch (r.code()) {
                      case StatusCode::kAlreadyExists:
                      case StatusCode::kInvalidArgument:
                          return at(stmt.new_name_byte_offset);
                      case StatusCode::kNotFound:
                          return at(stmt.old_column_byte_offset);
                      default:
                          return r;
                  }
              }()
            : catalog_.RenameTable(oid.value(), stmt.new_name,
                                   session.in_explicit_txn() && session.transaction() != nullptr
                                       ? session.transaction()->id()
                                       : txn::kNoTrxId);
    if (!renamed.ok()) {
        return {ErrorReply(renamed), false, 0, renamed};
    }
    // Transactionless like the pattern and assertion routes: the renamed
    // catalog rows are logged but no commit record follows them.
    if (Status s = AwaitDdlDurability(); !s.ok()) return {ErrorReply(s), false, 0, s};

    std::ostringstream os;
    if (rename_column) {
        os << "RENAMED COLUMN " << stmt.table_name << '.' << stmt.old_column << " TO "
           << stmt.new_name;
    } else {
        os << "RENAMED TABLE " << stmt.table_name << " TO " << stmt.new_name;
    }
    if (logging(LogLevel::kInfo)) {
        log_->Info("ddl", "renamed " + stmt.table_name +
                              (rename_column ? "." + stmt.old_column : std::string()) +
                              " to " + stmt.new_name);
    }
    return {os.str(), false};
}

StatusOr<std::optional<std::string>> CommandDispatcher::DropColumnRestriction(
    catalog::Oid oid, std::uint16_t pos) {
    // BW-R8: every object that names the column by its position. A dropped
    // column keeps its position, so any of these would go on reading bytes
    // no statement can see or write - an index entry, a reference, a state
    // check, an observed set - and each is refused rather than dropped with
    // it (AL4's argument: a constraint is not allowed to die quietly).
    auto indexes = catalog_.ListIndexes();
    if (!indexes.ok()) return indexes.status();
    for (const catalog::SysIndexRow& ix : indexes.value()) {
        if (ix.table_oid != oid) continue;
        const auto names = [&](auto first, std::uint8_t n) {
            return std::find(first, first + n, pos) != first + n;
        };
        if (names(ix.key_cols.begin(), ix.nkeys) || names(ix.covered_cols.begin(), ix.ncovered)) {
            return std::optional<std::string>("index '" +
                                              std::string(catalog::NameView(ix.name)) +
                                              "' keys or covers it; DROP INDEX first");
        }
    }
    auto fkeys = catalog_.ListForeignKeys();
    if (!fkeys.ok()) return fkeys.status();
    for (const catalog::SysFkeyRow& fk : fkeys.value()) {
        // The parent end of a reference is always its tuple key, which no drop
        // reaches (invariant 11), so only the child end names a column.
        if (fk.child_rel_oid == oid && fk.child_column_no == pos) {
            return std::optional<std::string>("it references '" +
                                              RelationNameOf(fk.parent_rel_oid) +
                                              "' (a foreign key, declared at CREATE TABLE)");
        }
    }
    auto transitions = exec::TransitionsOnRelation(catalog_, page_store_, oid);
    if (!transitions.ok()) return transitions.status();
    for (const exec::TransitionDef& t : transitions.value()) {
        if (t.column_no == pos) {
            return std::optional<std::string>("transition '" + t.name +
                                              "' governs it; DROP TRANSITION first");
        }
    }
    auto cabins = catalog_.ListCabins();
    if (!cabins.ok()) return cabins.status();
    for (const catalog::SysCabinRow& cabin : cabins.value()) {
        if (cabin.rel_oid == oid && cabin.shape == catalog::kCabinShapeColumn &&
            cabin.column_no == pos) {
            return std::optional<std::string>("a Cabin is on it; DROP CABIN first");
        }
    }
    // An expression or predicate Cabin reads it by position too (AQ-S1).
    auto access = catalog_.InitTableAccess(oid);
    if (!access.ok()) return access.status();
    auto read = exec::AnyCabinKeyReads(catalog_, page_store_, *access.value(), pos);
    if (!read.ok()) return read.status();
    if (read.value()) {
        return std::optional<std::string>("a Cabin's key reads it; DROP CABIN first");
    }
    // AL4's predicate, by relation: an assertion's canon is its source
    // text, and a coarser refusal is never wrong.
    auto assertions = exec::AssertionsOnRelation(catalog_, page_store_, oid);
    if (!assertions.ok()) return assertions.status();
    if (!assertions.value().empty()) {
        return std::optional<std::string>("assertion '" + assertions.value().front().name +
                                          "' is declared on this relation; DROP ASSERTION "
                                          "first");
    }
    return std::optional<std::string>();
}

StatusOr<catalog::Oid> CommandDispatcher::ResolveAlterTarget(const parser::AlterStmt& stmt,
                                                            Session& session) {
    // You cannot alter a relation you cannot see (DT3c).
    auto oid = ResolveNames(session, [&](const txn::ReadView* v) {
        return catalog_.ResolveRelation(stmt.schema, stmt.table_name,
                                        session.default_namespace(), stmt.byte_offset, v);
    });
    // AL7: the catalog's own names are load-bearing for bootstrap and are
    // nobody's to change - a system relation never resolves
    // (`Catalog::ResolveRelation` refuses `sys`), and `RenameTable`'s own
    // guard is defense rather than the door.
    return oid;
}

StatusOr<catalog::Oid> CommandDispatcher::AlterColumnRelation(const parser::AlterStmt& stmt,
                                                             Session& session,
                                                             WriteScope& scope) {
    auto oid = ResolveAlterTarget(stmt, session);
    if (!oid.ok()) return oid.status();
    // ---- BW-R7: the relation `X`, `CREATE INDEX`'s shape (AT-S5e) ---------
    //
    // Before the first catalog write. Every writer holds the relation's `IX`
    // and every positioned reader its `IS` until its transaction decides, so
    // the grant is a moment none is mid-statement, and one arriving later
    // parks until this decides and then re-resolves the schema the decide
    // published (the schema word moves first).
    if (std::optional<Status> held = BorrowRelationForDdl(scope.txn, oid.value());
        held.has_value()) {
        return *held;
    }
    return oid.value();
}

DispatchOutcome CommandDispatcher::HandleAddColumn(const parser::AlterStmt& stmt,
                                                   Session& session) {
    const parser::ColumnDef& col = stmt.column;
    const auto refuse = [](const std::string& what, std::uint32_t byte) {
        const Status s =
            Status::NotImplemented(what + " (byte " + std::to_string(byte) + ")");
        return DispatchOutcome{ErrorReply(s), false, 0, s};
    };
    // BW-R5: the rows that predate the column have no value for it, so
    // it is nullable and says so - this engine's columns are NOT NULL
    // unless declared NULL (null.md §2.3), and reading an unmarked
    // declaration as nullable would enforce something other than what
    // was written.
    if (col.notnull) {
        return refuse("an added column must be declared NULL: the rows that predate it "
                      "have no value for it, and there is no DEFAULT",
                      stmt.column_byte_offset);
    }
    if (!col.references_table.empty()) {
        return refuse("a foreign key is declared at CREATE TABLE only",
                      col.references_byte_offset);
    }
    if (col.cabin_policy != catalog::kCabinPolicyUnset) {
        return refuse("a column's cabin policy is declared at CREATE TABLE only; CREATE "
                      "CABIN after the ADD",
                      col.cabin_byte_offset);
    }
    // Before the `X`: a declaration that cannot be admitted waits for
    // nobody. `pos` 1 because an added column is never the tuple key; the catalog
    // places it.
    catalog::SysColumnRow row{};
    if (auto refused = ColumnRowOf(col, /*pos=*/1, row); refused.has_value()) return *refused;

    return InDdlStatement(session, [&](WriteScope& scope) -> DispatchOutcome {
        auto oid = AlterColumnRelation(stmt, session, scope);
        if (!oid.ok()) return {ErrorReply(oid.status()), false, 0, oid.status()};


        DdlScope ddl = DdlScopeFor(scope);
        catalog::CatalogRowRef created;
        const Status added = catalog_.AddColumn(oid.value(), row, ddl.trx_id, &created);
        // Before the status, CREATE INDEX's reason: a failed add that wrote
        // its row still left it there.
        NoteRowWritten(ddl, created);
        if (!added.ok()) {
            const Status s = added.WithContext("byte " + std::to_string(stmt.column_byte_offset));
            return {ErrorReply(s), false, 0, s};
        }
        if (logging(LogLevel::kInfo)) {
            log_->Info("ddl", "added column " + stmt.table_name + "." + col.name);
        }
        return {"ADDED COLUMN " + stmt.table_name + "." + col.name, false};
    });
}

DispatchOutcome CommandDispatcher::HandleDropColumn(const parser::AlterStmt& stmt,
                                                    Session& session) {
    return InDdlStatement(session, [&](WriteScope& scope) -> DispatchOutcome {
        auto oid = AlterColumnRelation(stmt, session, scope);
        if (!oid.ok()) return {ErrorReply(oid.status()), false, 0, oid.status()};

        // Resolved under the `X`, so no dependant can arrive between the
        // restrict and the write.
        auto access = catalog_.InitTableAccess(oid.value());
        if (!access.ok()) return {ErrorReply(access.status()), false, 0, access.status()};
        auto found =
            access.value()->schema.FindColumn(stmt.old_column, stmt.old_column_byte_offset);
        if (!found.ok()) return {ErrorReply(found.status()), false, 0, found.status()};
        const catalog::SysColumnRow* column = found.value();
        const auto at = " (byte " + std::to_string(stmt.old_column_byte_offset) + ")";
        if (column == nullptr) {
            const Status s = Status::NotFound("relation '" + stmt.table_name +
                                              "' has no column named '" + stmt.old_column +
                                              "'" + at);
            return {ErrorReply(s), false, 0, s};
        }
        // Identity is the Keystone word at position 0 (invariant 11).
        if (column->pos == 0) {
            const Status s = Status::InvalidArgument("column '" + stmt.old_column +
                                                      "' is the tuple key and cannot be "
                                                      "dropped (invariant 11)" + at);
            return {ErrorReply(s), false, 0, s};
        }
        // A heap's last declared column (BZ-Q3): without it the relation has
        // no column a statement can write or read, only its hidden key.
        if (access.value()->schema.heap() && access.value()->layout.live_columns == 2) {
            const Status s = Status::InvalidArgument(
                "column '" + stmt.old_column + "' is the heap's last declared column and cannot "
                "be dropped; a heap keeps at least one" + at);
            return {ErrorReply(s), false, 0, s};
        }
        auto restricted =
            DropColumnRestriction(oid.value(), static_cast<std::uint16_t>(column->pos));
        if (!restricted.ok()) {
            return {ErrorReply(restricted.status()), false, 0, restricted.status()};
        }
        if (restricted.value().has_value()) {
            return {"ERR column '" + stmt.old_column + "' cannot be dropped" + at + ": " +
                        *restricted.value(),
                    false};
        }

        if (before_drop_column_write_for_test_) before_drop_column_write_for_test_();
        DdlScope ddl = DdlScopeFor(scope);
        std::vector<catalog::CatalogRowChange> changed;
        // By the position the RESTRICT was asked of: a RENAME COLUMN takes
        // no relation lock, so a second resolution by name could reach
        // another column (the door refuses a renamed one).
        const Status dropped =
            catalog_.DropColumn(oid.value(), static_cast<std::uint16_t>(column->pos),
                                stmt.old_column, ddl.trx_id, &changed);
        NoteCatalogRowChanges(ddl, changed);
        if (!dropped.ok()) {
            const Status s = dropped.WithContext("byte " + std::to_string(stmt.old_column_byte_offset));
            return {ErrorReply(s), false, 0, s};
        }
        if (logging(LogLevel::kInfo)) {
            log_->Info("ddl", "dropped column " + stmt.table_name + "." + stmt.old_column);
        }
        return {"DROPPED COLUMN " + stmt.table_name + "." + stmt.old_column, false};
    });
}

DispatchOutcome CommandDispatcher::HandleDropTable(std::string_view line,
                                                   Session& session) {
    // Set by a drop that succeeded, so its Transitions go once it commits -
    // which in autocommit is the end of `InDdlStatement`, read here before
    // it, and asked of the status the commit leaves (a failed commit
    // carries its own, `FinishDdlStatement`).
    std::optional<catalog::Oid> dropped_oid;
    const bool autocommit = !session.in_explicit_txn();
    DispatchOutcome out = InDdlStatement(session, [&](WriteScope& scope) -> DispatchOutcome {
        auto parsed = parser::Parse(line);
        if (!parsed.ok()) {
            return {ErrorReply(parsed.status()), false, 0, parsed.status()};
        }
        if (!std::holds_alternative<parser::DropTableStmt>(parsed.value())) {
            return {"ERR expected a DROP TABLE statement", false};
        }
        const auto& stmt = std::get<parser::DropTableStmt>(parsed.value());

        // Nor drop one (DT3c).
        auto oid = ResolveNames(session, [&](const txn::ReadView* v) {
            return catalog_.ResolveRelation(stmt.schema, stmt.table_name,
                                            session.default_namespace(), stmt.byte_offset, v);
        });
        if (!oid.ok()) {
            return {ErrorReply(oid.status()), false, 0, oid.status()};
        }

        // DT3's RESTRICT, both blockers named. A referencing foreign key
        // blocks at the *declared* level - the constraint exists whether or
        // not rows do - which is the check known-gaps.md said was waiting for
        // exactly this caller. An enforcing constraint is not allowed to die
        // quietly - ALTER's AL4 argument, same predicate, third caller.
        //
        // **Asked twice since BF-Q11 (a)**: before the `X`, so a refused drop
        // waits for nobody, and again under it, so a foreign key or an
        // assertion created between the first ask and the grant still
        // refuses. A child's `CREATE TABLE` takes the parent's `IS` for the
        // same window from its side. Before BF a dangle there pointed at
        // allocated pages; once a drop's pages are reclaimed within the run,
        // a cached parent could be descended into reused ones.
        const auto restricted = [&]() -> std::optional<DispatchOutcome> {
            auto fkeys = catalog_.ListForeignKeys();
            if (!fkeys.ok()) {
                return DispatchOutcome{ErrorReply(fkeys.status()), false, 0, fkeys.status()};
            }
            for (const catalog::SysFkeyRow& fk : fkeys.value()) {
                if (fk.parent_rel_oid != oid.value()) continue;
                return DispatchOutcome{"ERR relation '" + stmt.table_name +
                                           "' is referenced by a foreign key on '" +
                                           RelationNameOf(fk.child_rel_oid) +
                                           "'; drop the referencing relation first",
                                       false};
            }
            auto restricting = exec::AssertionsOnRelation(catalog_, page_store_, oid.value());
            if (!restricting.ok()) {
                return DispatchOutcome{ErrorReply(restricting.status()), false, 0,
                                       restricting.status()};
            }
            if (!restricting.value().empty()) {
                return DispatchOutcome{"ERR assertion '" + restricting.value().front().name +
                                           "' is declared on this relation; DROP ASSERTION first",
                                       false};
            }
            // CN-4 T7 as revised in BV-S2: a Transition goes with its
            // relation - but its row commits at its own statement and a drop
            // inside an explicit transaction can still roll back, so there
            // the drop is refused rather than taking a constraint the
            // rollback could not give back.
            if (session.in_explicit_txn()) {
                auto governing = exec::TransitionsOnRelation(catalog_, page_store_, oid.value());
                if (!governing.ok()) {
                    return DispatchOutcome{ErrorReply(governing.status()), false, 0,
                                           governing.status()};
                }
                if (!governing.value().empty()) {
                    const Status s = Status::NotImplemented(
                        "transition '" + governing.value().front().name +
                        "' is declared on this relation; inside an explicit transaction DROP "
                        "TRANSITION first, or drop the relation in autocommit (byte " +
                        std::to_string(stmt.byte_offset) + ")");
                    return DispatchOutcome{ErrorReply(s), false, 0, s};
                }
            }
            return std::nullopt;
        };
        if (auto refused = restricted(); refused.has_value()) return *refused;
        if (before_drop_exclusive_for_test_) before_drop_exclusive_for_test_();

        // ---- AO-S6e-b: the relation `X`, and what waits for what --------
        //
        // Taken **after** the RESTRICT checks, so a refusal the catalog
        // knows at once is not waited for, and asked again under the `X`
        // (BF). What it buys is `drop-table.md` DT7's: a reader already
        // positioned finishes against a live schema. It does not make the
        // drop isolated (`ddl-transactional.md` §5a): a read refused its
        // borrow reads on, safe because no page is freed while a statement
        // that could have bound the relation runs (the statement epoch).
        if (std::optional<Status> held = BorrowRelationForDdl(scope.txn, oid.value());
            held.has_value()) {
            return {ErrorReply(*held), false, 0, *held};
        }
        if (auto refused = restricted(); refused.has_value()) return *refused;

        std::vector<std::uint64_t> dropped_cabins;
        // Transactional when inside an explicit transaction (DT5): the
        // dependents are delete-marked instead of retired, and every change -
        // the marks and the `sys.objects` retype - goes on the trail so
        // `Abort` undoes it. Registered before the status is read, for the
        // same reason CREATE's rows are: a drop that failed partway still
        // changed rows.
        DdlScope ddl = DdlScopeFor(scope);
        std::vector<catalog::CatalogRowChange> changed;
        catalog::PendingRoots owed;
        Status dropped =
            catalog_.DropTable(oid.value(), dropped_cabins, ddl.trx_id,
                               ddl.txn != nullptr ? &changed : nullptr, &owed);
        NoteCatalogRowChanges(ddl, changed);
        if (Status s = dropped; !s.ok()) {
            return {ErrorReply(s), false, 0, s};
        }
        // What the drop owes (BF-R9), queued at its transaction's commit by
        // `EndDdlScopeById`: the roots its retype wrote into the tombstone.
        if (reclaim_queue_ != nullptr && scope.txn != nullptr) {
            owed_drops_.push_back(OwedDrop{scope.txn->id(), oid.value(), owed});
        }
        // The catalog rows are gone and the compiler stops emitting probes;
        // the in-memory sets would only leak, so they are forgotten, not
        // protected (cabin.md - un-observing is always legal).
        if (cabins_ != nullptr) {
            for (const std::uint64_t cabin_id : dropped_cabins) cabins_->Forget(cabin_id);
        }

        if (logging(LogLevel::kInfo)) {
            log_->Info("ddl", "dropped table " + stmt.table_name + " (oid " +
                                  std::to_string(oid.value()) +
                                  "); its pages are reclaimed once no reader or replay "
                                  "can reach them");
        }
        dropped_oid = oid.value();
        return {"DROPPED TABLE " + stmt.table_name + " oid=" + std::to_string(oid.value()), false};
    });
    // CN-4 T7: the relation's Transitions go with it, once the drop has
    // committed - in autocommit, at the end of `InDdlStatement`; inside an
    // explicit transaction the drop was refused while one stood. A crash
    // between the commit and this leaves an inert row naming a relation
    // that is gone - an oid is never reissued - which `DROP TRANSITION`
    // removes (`docs/inflight/known-gaps.md`).
    if (dropped_oid.has_value() && out.status.ok() && autocommit) {
        auto governing = exec::TransitionsOnRelation(catalog_, page_store_, *dropped_oid);
        Status cleared = governing.status();
        if (governing.ok()) {
            for (const exec::TransitionDef& def : governing.value()) {
                auto gone = exec::DeleteTransition(catalog_, page_store_, wal_, def.name);
                if (!gone.ok()) cleared = gone.status();
            }
        }
        if (!cleared.ok() && logging(LogLevel::kWarn)) {
            log_->Warn("ddl", "a dropped relation's transitions were not all removed: " +
                                  cleared.message());
        }
        // Its expression Cabins' texts, on the same terms (AQ-S1): inside an
        // explicit transaction the drop can still roll back its Cabins'
        // rows, so their texts stay - inert once the drop commits, since
        // nothing reads a text no sys.cabins row names.
        if (Status s = exec::ForgetCabinTexts(catalog_, page_store_, wal_, *dropped_oid);
            !s.ok() && logging(LogLevel::kWarn)) {
            log_->Warn("ddl", "a dropped relation's cabin key functions were not all removed: " +
                                  s.message());
        }
    }
    return out;
}

// `CREATE NAMESPACE <name>` / `DROP NAMESPACE <name>` (AF-T3, AF-6's
// operator-taken shape (a); `docs/spec/namespace.md` NS7).
//
// One handler for both, as the parser has one production: they differ only
// in which catalog call they reach. Under `InDdlStatement` like every other
// DDL route, so the one `sys.objects` row this writes is on a real
// transaction's trail and a crash mid-statement has a loser to roll back.
// The namespace's declared affinity is read by nothing (D18, NS10).
DispatchOutcome CommandDispatcher::HandleNamespace(std::string_view line, Session& session) {
    parser::Parser parser(line);
    auto parsed = parser.Parse();
    if (!parsed.ok()) {
        return {ErrorReply(parsed.status()), false, 0, parsed.status()};
    }
    if (!std::holds_alternative<parser::NamespaceStmt>(parsed.value())) {
        return {"ERR expected a CREATE NAMESPACE or DROP NAMESPACE statement", false};
    }
    const auto stmt = std::get<parser::NamespaceStmt>(std::move(parsed.value()));

    return InDdlStatement(session, [&](WriteScope& scope) -> DispatchOutcome {
        DdlScope ddl = DdlScopeFor(scope);

        if (!stmt.drop) {
            auto oid = catalog_.CreateNamespace(stmt.name, ddl.trx_id, ddl.sink());
            NoteDdlRows(ddl);  // before the status: a partial write is still a write
            if (!oid.ok()) {
                Status s = oid.status().WithContext("byte " +
                                                    std::to_string(stmt.byte_offset));
                return {ErrorReply(s), false, 0, s};
            }
            if (logging(LogLevel::kInfo)) {
                log_->Info("ddl", "created namespace '" + stmt.name + "' oid=" +
                                      std::to_string(oid.value()));
            }
            return {"CREATED NAMESPACE " + stmt.name + " oid=" + std::to_string(oid.value()),
                    false};
        }

        auto oid = ResolveNames(session, [&](const txn::ReadView* v) {
            return catalog_.FindNamespaceOidByName(stmt.name, v);
        });
        if (!oid.ok()) {
            Status s = oid.status().code() == StatusCode::kNotFound
                           ? Status::NotFound("no namespace named '" + stmt.name + "' (byte " +
                                              std::to_string(stmt.byte_offset) + ")")
                           : oid.status();
            return {ErrorReply(s), false, 0, s};
        }

        // RESTRICT, and the catalog names the relation that blocked it.
        std::vector<catalog::CatalogRowChange> changed;
        Status dropped = catalog_.DropNamespace(oid.value(), ddl.trx_id,
                                                ddl.txn != nullptr ? &changed : nullptr);
        NoteCatalogRowChanges(ddl, changed);
        if (!dropped.ok()) {
            Status s = dropped.WithContext("byte " + std::to_string(stmt.byte_offset));
            return {ErrorReply(s), false, 0, s};
        }
        if (logging(LogLevel::kInfo)) {
            log_->Info("ddl", "dropped namespace '" + stmt.name + "' oid=" +
                                  std::to_string(oid.value()));
        }
        return {"DROPPED NAMESPACE " + stmt.name + " oid=" + std::to_string(oid.value()), false};
    });
}

CommandDispatcher::DdlBuildLock::~DdlBuildLock() {
    if (txn == nullptr) return;
    (void)manager->Abort(*txn);
    manager->Release(*txn);  // `Abort` keeps it in `live_` by design
}

std::optional<Status> CommandDispatcher::TakeDdlBuildLock(Session& session, catalog::Oid relation,
                                                         std::string_view relation_name,
                                                         std::string_view verb,
                                                         DdlBuildLock& out) {
    if (locks_ == nullptr || txn_ == nullptr) return std::nullopt;
    // **The session's own transaction is tested first**, by what it holds
    // rather than by who the table reports: with it and another writer both
    // holding the relation, the table may name the other, and waiting for it
    // would end in the same refusal.
    const txn::Transaction* own = session.transaction();
    if (own != nullptr && own->borrows().Holds(txn::LockKey::Relation(relation))) {
        return Status::TxnConflict("relation '" + std::string(relation_name) +
                                   "' is held by this session's own transaction; " +
                                   std::string(verb) +
                                   " reads settled state - retry when it has ended");
    }
    auto begun = txn_->Begin(txn::IsolationLevel::kReadCommitted);
    if (!begun.ok()) return begun.status();
    out.manager = txn_;
    out.txn = begun.value();
    return BorrowRelationForDdl(out.txn, relation, /*poisons=*/false);
}

DispatchOutcome CommandDispatcher::HandleAssertion(std::string_view line, Session& session) {
    parser::Parser parser(line);
    auto parsed = parser.Parse();
    if (!parsed.ok()) {
        return {ErrorReply(parsed.status()), false, 0, parsed.status()};
    }
    if (!std::holds_alternative<parser::AssertionStmt>(parsed.value())) {
        return {"ERR expected a CREATE ASSERTION or DROP ASSERTION statement", false};
    }
    const auto& stmt = std::get<parser::AssertionStmt>(parsed.value());

    if (stmt.drop) {
        auto id = exec::DropAssertion(catalog_, page_store_, stmt, wal_);
        if (!id.ok()) {
            return {ErrorReply(id.status()), false, 0, id.status()};
        }
        if (Status s = AwaitDdlDurability(); !s.ok()) return {ErrorReply(s), false, 0, s};
        // The instance's registry, the one place the directory is (AT-S5d).
        enforcer_->Evict(id.value());
        std::ostringstream os;
        os << "DROPPED ASSERTION name=" << stmt.name << " assertion_id=" << id.value();
        if (logging(LogLevel::kInfo)) {
            log_->Info("ddl", "dropped assertion '" + stmt.name + "'");
        }
        return {os.str(), false};
    }

    // ---- AT-S5e, AT-0 item 13: the relation `X`, for the build ----------
    //
    // Every writer of the relation holds its `IX` from its first intention
    // until its transaction decides, so the grant is the moment no writer
    // is mid-statement, and a writer arriving during the build parks until
    // the directory is adopted. **The holder is a transaction of the
    // statement's own** (the operator's mark on item 13): it writes nothing
    // and is rolled back when the statement ends, so the relation is
    // released there and a failed `CREATE ASSERTION` poisons nothing - a
    // transaction rather than a minted id because a waiter names its holder.
    // **Resolved under a settled view, and a name it does not resolve is
    // refused here**, once: the oid it names is the one locked and the one
    // built. Settled and not the session's, because the statement reads
    // settled state - a relation an open transaction created, the session's
    // own included, is not there yet - and because a repeatable-read view
    // minted at `BEGIN` would keep naming a relation dropped and re-created
    // since, so the re-check below would refuse every retry. A dispatcher
    // with no manager reads everything, which is the pre-MVCC engine and what
    // every socket-free test runs on.
    const auto settled_view = [this] {
        return txn_ != nullptr ? txn_->MintCheckView(txn::kNoTrxId) : txn::ReadView::Everything();
    };
    const txn::ReadView view = settled_view();
    auto target = exec::AssertionTargetOid(catalog_, stmt, session.default_namespace(), &view);
    if (!target.ok()) return {ErrorReply(target.status()), false, 0, target.status()};

    DdlBuildLock build_lock;
    if (std::optional<Status> refused =
            TakeDdlBuildLock(session, target.value(), stmt.table_name, "CREATE ASSERTION",
                             build_lock);
        refused.has_value()) {
        return {ErrorReply(*refused), false, 0, *refused};
    }

    // The visibility the build's scan reads under: settled state, minted
    // after the grant exactly as a FK check's view is.
    const txn::ReadView check_view = settled_view();

    // **The lock was waited for under the view the name resolved by**, and a
    // drop-and-recreate that committed meanwhile leaves that name on another
    // relation. The grant is on the old one, so what is built must be too, or
    // nothing: the settled name is read again and must still be the locked oid.
    // A name that is gone is the `NotFound` a retry would give.
    if (build_lock.txn != nullptr) {
        auto settled =
            exec::AssertionTargetOid(catalog_, stmt, session.default_namespace(), &check_view);
        if (!settled.ok()) return {ErrorReply(settled.status()), false, 0, settled.status()};
        if (settled.value() != target.value()) {
            const Status moved = Status::TxnConflict(
                "relation '" + stmt.table_name +
                "' changed while CREATE ASSERTION waited for it - retry");
            return {ErrorReply(moved), false, 0, moved};
        }
    }

    // Through ErrorReply, not a bare "ERR ": the build can refuse with the
    // two coded spellings - ASSERTION_VIOLATION for data already past the
    // bound, TXN_CONFLICT for an unsettled relation - and both are
    // compatibility surfaces a client switches on.
    auto created = exec::CreateAssertion(catalog_, page_store_, stmt, target.value(), check_view,
                                         wal_, *enforcer_, after_assertion_publish_run_for_test_);
    if (!created.ok()) {
        return {ErrorReply(created.status()), false, 0, created.status()};
    }
    const exec::AssertionDdlResult& result = created.value();
    // After the adoption, deliberately: a
    // sync failure then answers ERR with the live registry still enforcing
    // what the log already holds - over-enforcing until the operator
    // retries, where the other order left a durably created constraint
    // unenforced on the running instance.
    if (Status s = AwaitDdlDurability(); !s.ok()) return {ErrorReply(s), false, 0, s};

    // A freshly created assertion **is** enforcing (AST07): a create that
    // returns was adopted inside `CreateAssertion` (AZ-S2). SHOW derives
    // its answer from the registry, because after a restart the row can
    // outlive enforcement - a mount that finds no whole snapshot run
    // brings the assertion up unenforcing (AY-S8, `assertion.md` §8.1).
    std::ostringstream os;
    os << "CREATED ASSERTION name=" << stmt.name
       << " assertion_id=" << result.assertion_id << " on=" << stmt.table_name
       << " cabin_root=" << result.cabin_root << " rows=" << result.rows_incorporated
       << " groups=" << result.group_count
       << " enforcing=" << (kWritePathEnforcesAssertions ? 1 : 0);
    if (logging(LogLevel::kInfo)) {
        log_->Info("ddl", "created assertion '" + stmt.name + "' on '" + stmt.table_name +
                              "' (built, enforcing)");
    }
    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleTransition(std::string_view line, Session& session) {
    parser::Parser parser(line);
    auto parsed = parser.Parse();
    if (!parsed.ok()) return {ErrorReply(parsed.status()), false, 0, parsed.status()};
    if (!std::holds_alternative<parser::TransitionStmt>(parsed.value())) {
        return {"ERR expected a CREATE TRANSITION or DROP TRANSITION statement", false};
    }
    const auto& stmt = std::get<parser::TransitionStmt>(parsed.value());
    const char* verb = stmt.drop ? "DROP TRANSITION" : "CREATE TRANSITION";

    // ---- The relation `X` (BV-R4), `HandleAssertion`'s shape -------------
    //
    // Held by a transaction of the statement's own, which writes nothing
    // and is rolled back when the statement ends: the grant is the moment
    // no writer of the relation is mid-statement, and a writer arriving
    // during the build waits for the publish. The row and the schema word
    // move before the release, so a writer that resolved the relation
    // before re-runs at its first intention (`MemoIsCurrent`) and meets the
    // change. Resolved under a settled view: a DROP names the relation its
    // row names; a CREATE resolves its table name.
    const auto settled_view = [this] {
        return txn_ != nullptr ? txn_->MintCheckView(txn::kNoTrxId) : txn::ReadView::Everything();
    };
    const txn::ReadView view = settled_view();
    StatusOr<catalog::Oid> target = Status::NotFound("");
    if (stmt.drop) {
        auto def = exec::FindTransitionByName(catalog_, page_store_, stmt.name);
        if (!def.ok()) return {ErrorReply(def.status()), false, 0, def.status()};
        if (!def.value().has_value()) {
            const Status s = Status::NotFound("no transition named \"" + stmt.name +
                                              "\" (byte " + std::to_string(stmt.byte_offset) +
                                              ")");
            return {ErrorReply(s), false, 0, s};
        }
        target = def.value()->target_oid;
    } else {
        target = exec::TransitionTargetOid(catalog_, stmt, session.default_namespace(), &view);
    }
    if (!target.ok()) return {ErrorReply(target.status()), false, 0, target.status()};

    DdlBuildLock build_lock;
    if (std::optional<Status> refused =
            TakeDdlBuildLock(session, target.value(), RelationNameOf(target.value()), verb,
                             build_lock);
        refused.has_value()) {
        return {ErrorReply(*refused), false, 0, *refused};
    }
    const txn::ReadView check_view = settled_view();
    // **The memo is made current under the `X`** (the S2 review's B2): the
    // statement's boundary settled it before the grant, and another core's
    // `CREATE TRANSITION` or `RENAME COLUMN` on this relation may have
    // published and released in between - the one-per-column check (CN-4
    // T6) and the column's resolution would then read an entry without it.
    // Nothing of this statement holds a `TableAccess` yet, so dropping the
    // memo here strands nothing; from the grant on, no DDL can change the
    // relation until this statement ends.
    catalog_.Revalidate();

    if (stmt.drop) {
        // The name was resolved before the grant; a drop and a re-create on
        // another relation in between would leave it naming a Transition
        // whose relation this statement does not hold. Asked again under the
        // `X`, as a CREATE re-resolves its table (the S3 review's C1).
        auto still = exec::FindTransitionByName(catalog_, page_store_, stmt.name);
        if (!still.ok()) return {ErrorReply(still.status()), false, 0, still.status()};
        if (still.value().has_value() && still.value()->target_oid != target.value()) {
            const Status moved = Status::TxnConflict(
                "transition \"" + stmt.name +
                "\" moved to another relation while DROP TRANSITION waited for it - retry");
            return {ErrorReply(moved), false, 0, moved};
        }
        auto dropped = exec::DropTransition(catalog_, page_store_, stmt, wal_);
        if (!dropped.ok()) return {ErrorReply(dropped.status()), false, 0, dropped.status()};
        if (Status s = AwaitDdlDurability(); !s.ok()) return {ErrorReply(s), false, 0, s};
        if (logging(LogLevel::kInfo)) log_->Info("ddl", "dropped transition '" + stmt.name + "'");
        return {"DROPPED TRANSITION name=" + stmt.name +
                    " transition_id=" + std::to_string(dropped.value().id),
                false};
    }

    // A drop-and-recreate that committed while this waited leaves the name
    // on another relation; the grant is on the old one, so what is built
    // must be too, or nothing.
    if (build_lock.txn != nullptr) {
        auto settled =
            exec::TransitionTargetOid(catalog_, stmt, session.default_namespace(), &check_view);
        if (!settled.ok()) return {ErrorReply(settled.status()), false, 0, settled.status()};
        if (settled.value() != target.value()) {
            const Status moved = Status::TxnConflict(
                "relation '" + stmt.table_name +
                "' changed while CREATE TRANSITION waited for it - retry");
            return {ErrorReply(moved), false, 0, moved};
        }
    }
    auto created =
        exec::CreateTransition(catalog_, page_store_, stmt, target.value(), check_view, wal_);
    if (!created.ok()) return {ErrorReply(created.status()), false, 0, created.status()};
    if (Status s = AwaitDdlDurability(); !s.ok()) return {ErrorReply(s), false, 0, s};
    if (logging(LogLevel::kInfo)) {
        log_->Info("ddl", "created transition '" + stmt.name + "' on '" + stmt.table_name + "'");
    }
    return {"CREATED TRANSITION name=" + stmt.name +
                " transition_id=" + std::to_string(created.value().transition_id) +
                " on=" + stmt.table_name + " column=" + stmt.column +
                " rows=" + std::to_string(created.value().rows_checked),
            false};
}

// ---- H6 step 3: the three inspection commands ---------------------------
//
// The one-line `\n`-escaped convention `SHOW PAGE` established
// (`client-manual.md`), so a text client reads them the same way.

// ---- Foreign-key checks (docs/spec/foreign-keys.md §§2-4) ----------------

txn::ReadView CommandDispatcher::CheckView(const WriteScope& scope) {
    // **Minted here, not taken from the statement.** A constraint check reads
    // latest state, so it needs a view of *now*: a parent committed-deleted
    // after this statement's snapshot must still fail the check, and an
    // in-flight writer must be seen rather than looked through (§4).
    //
    // The writer's own id goes in, so a transaction's own uncommitted rows
    // satisfy its own constraints - the table's fourth row, with no special
    // case anywhere.
    if (txn_ == nullptr) return txn::ReadView::Everything();
    return txn_->MintCheckView(WriterId(scope));
}

void CommandDispatcher::RecordFkAccess(exec::AccessKind kind, catalog::Oid rel_oid,
                                       std::uint64_t column_mask) {
    // FK-M4. The checks are not steps (fk_check.hpp says why), so the shape
    // they touch is recorded by hand where a step would have been counted by
    // being one. Same relation and the same call every other access goes
    // through, so `SHOW ACCESS` compares constraint cost against query cost
    // without anyone having to know which is which.
    if (!access_stats_enabled_) return;
    Status recorded = catalog_.RecordAccess(exec::StoredAccessKind(kind), rel_oid, column_mask,
                                            static_cast<std::uint64_t>(NowNs()));
    // Dropped deliberately: a statistic that could fail a write would be a
    // worse trade than no statistic (catalog.hpp says so at the source).
    (void)recorded;
}

namespace {

// A row of neither arity, refused counting `columns` - the live ones, which
// is every one on a relation with no dropped column (BW-R6). Both accepted
// counts are named, because with two of them a single number reads as an
// off-by-one against the wrong one.
Status InsertArityRefusal(const catalog::TableAccess& ta, std::size_t columns,
                          const std::vector<parser::AstValue>& row) {
    // The row's first value is where it was written; a KWP load row has no
    // text, so no byte.
    const std::string at = !row.empty() && row.front().byte_offset != 0
                               ? " (byte " + std::to_string(row.front().byte_offset) + ")"
                               : "";
    return Status::InvalidArgument(
        "expected " + std::to_string(columns) + " value(s) including tuple key column '" +
        std::string(catalog::NameView(ta.schema.columns.front().name)) + "', or " +
        std::to_string(columns - 1) + " to have it issued; got " + std::to_string(row.size()) +
        at);
}

// Whether `text` holds `word`, ASCII case ignored - RETURNING's cheap gate,
// so a write without one is never parsed twice.
bool ContainsWordIgnoringCase(std::string_view text, std::string_view word) {
    if (word.size() > text.size()) return false;
    for (std::size_t i = 0; i + word.size() <= text.size(); ++i) {
        if (IEquals(text.substr(i, word.size()), word)) return true;
    }
    return false;
}

// A statement's header - relation, namespace, byte and described shape - with
// no column list and no rows: what both normalizers fill with physical rows.
parser::InsertStmt InsertShellOf(const parser::InsertStmt& written) {
    parser::InsertStmt out;
    out.table_name = written.table_name;
    out.schema = written.schema;
    out.table_byte_offset = written.table_byte_offset;
    out.described = written.described;
    return out;
}

// The column each value of a `width`-long row is for: the listed column,
// or the live column at its position, one later when the row leaves its
// tuple key out. Empty when the length reaches no column - the arity
// refusal of the normalization below answers that row.
StatusOr<std::vector<const catalog::SysColumnRow*>> InsertTargetColumns(
    const catalog::TableAccess& ta, const parser::InsertStmt& written, std::size_t width) {
    std::vector<const catalog::SysColumnRow*> out;
    if (!written.columns.empty()) {
        if (width != written.columns.size()) return out;
        for (const parser::IndexColumnRef& named : written.columns) {
            auto found = ta.schema.FindColumn(named.name, named.byte_offset);
            if (!found.ok()) return found.status();
            if (found.value() == nullptr) return std::vector<const catalog::SysColumnRow*>();
            out.push_back(found.value());
        }
        return out;
    }
    std::vector<const catalog::SysColumnRow*> live;
    for (const catalog::SysColumnRow& c : ta.schema.columns) {
        if (c.visible()) live.push_back(&c);
    }
    if (width == live.size()) return live;
    // A heap's row is its declared columns, never one short (BZ-R3).
    if (!ta.schema.heap() && width + 1 == live.size()) {
        return std::vector<const catalog::SysColumnRow*>(live.begin() + 1, live.end());
    }
    return out;
}

// **The values written as expressions, typed and evaluated** (BU-S8): each
// for the column its position reaches - the listed column, or the live
// column at its position, one later when the row leaves its tuple key out -
// by the SET value's typer (BJ-R3, no implicit conversion), then folded
// once, before any row is placed. A VALUES row has no row to read, so an
// expression naming a column is refused; a row whose length reaches no
// column is left to the arity refusal below.
StatusOr<parser::InsertStmt> EvaluateInsertExpressions(const catalog::TableAccess& ta,
                                                       const parser::InsertStmt& written) {
    const exec::StatementContext context = exec::StatementContextNow();
    parser::InsertStmt out = written;
    out.value_exprs.clear();
    for (const parser::InsertStmt::ValueExpr& v : written.value_exprs) {
        auto targets = InsertTargetColumns(ta, written, written.rows[v.row].size());
        if (!targets.ok()) return targets.status();
        if (targets.value().empty()) continue;
        const catalog::SysColumnRow* target = targets.value()[v.col];
        auto typed = exec::TypeSetExpression(ta, *v.expr, *target);
        if (!typed.ok()) return typed.status();
        // The column test, not `IsConstantSetExpression`: that one also says
        // no to `NOW()` and `CURRENT_DATE`, which read no column and fold here
        // against the statement's one instant.
        if (exec::SetExpressionColumns(*typed.value()) != 0) {
            return Status::InvalidArgument("a VALUES expression reads no column (byte " +
                                           std::to_string(v.expr->byte_offset) +
                                           "); a VALUES row has no row to read");
        }
        auto value = exec::EvaluateSetExpression(*typed.value(), {}, context);
        if (!value.ok()) return value.status();
        value.value().byte_offset = v.expr->byte_offset;
        out.rows[v.row][v.col] = std::move(value.value());
    }
    return out;
}

// **A column-listed statement's rows in the physical shape** (BW-S6, BW-Q12
// of `instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`): each
// value goes to the column its list position names, so no column change can
// shift it (BW-Q11's gap does not reach a listed row). The tuple key comes
// first when the list names it and the row's value is not `NULL`; a `NULL`
// one, or a list without the key, has an id issued. Every dropped position
// and every nullable column the list leaves out is a NULL placeholder - to
// everything downstream the same as an explicit `NULL`.
//
// Refused in BW-Q12.12's order, each `InvalidArgument` at a real byte: a
// name the relation has no live column for (a dropped column's included,
// BW-R6), matched ignoring case as every column name is (`Schema::FindColumn`;
// raft-marks-2026-10-15.md section 1 overrode BW-Q12.3); a name listed
// twice, at its second occurrence; a NOT NULL column the list leaves out,
// once, at the list's `(` - before any row, and never left to the codec,
// whose placeholder would carry byte 0; and a row whose length is not the
// list's, at its first value, with its ordinal on a multi-row statement.
StatusOr<parser::InsertStmt> NormalizeListedInsertRows(const catalog::TableAccess& ta,
                                                       const parser::InsertStmt& written) {
    const std::vector<catalog::SysColumnRow>& columns = ta.schema.columns;
    // The list position each physical column takes its value from, or none.
    std::vector<std::optional<std::size_t>> source(columns.size());
    for (std::size_t j = 0; j < written.columns.size(); ++j) {
        const parser::IndexColumnRef& named = written.columns[j];
        auto found = ta.schema.FindColumn(named.name, named.byte_offset);
        if (!found.ok()) return found.status();
        const catalog::SysColumnRow* col = found.value();
        if (col == nullptr) {
            return Status::InvalidArgument("unknown column '" + named.name + "' at byte " +
                                           std::to_string(named.byte_offset));
        }
        const auto i = static_cast<std::size_t>(col - columns.data());
        if (source[i].has_value()) {
            return Status::InvalidArgument("column '" + named.name +
                                           "' is listed twice in the column list at byte " +
                                           std::to_string(named.byte_offset));
        }
        source[i] = j;
    }
    // The tuple key (column 0) left out is issued, never refused.
    for (std::size_t i = 1; i < columns.size(); ++i) {
        if (!columns[i].dropped() && columns[i].notnull && !source[i].has_value()) {
            return Status::InvalidArgument(
                "column '" + std::string(catalog::NameView(columns[i].name)) +
                "' is NOT NULL and the column list leaves it out (byte " +
                std::to_string(written.columns_byte_offset) + "); there is no DEFAULT");
        }
    }

    parser::InsertStmt out = InsertShellOf(written);
    out.rows.reserve(written.rows.size());
    for (std::size_t k = 0; k < written.rows.size(); ++k) {
        const std::vector<parser::AstValue>& row = written.rows[k];
        if (row.size() != written.columns.size()) {
            const std::string byte = !row.empty() && row.front().byte_offset != 0
                                         ? " at byte " + std::to_string(row.front().byte_offset)
                                         : "";
            const std::string at =
                written.rows.size() > 1 ? " (row " + std::to_string(k + 1) + ")" : "";
            return Status::InvalidArgument(
                "expected " + std::to_string(written.columns.size()) +
                " value(s), one per listed column; got " + std::to_string(row.size()) + byte +
                at);
        }
        const bool names_key =
            source[0].has_value() && row[*source[0]].type != parser::ValueType::kNull;
        std::vector<parser::AstValue>& physical = out.rows.emplace_back();
        physical.reserve(columns.size());
        for (std::size_t i = names_key ? 0 : 1; i < columns.size(); ++i) {
            if (source[i].has_value()) {
                physical.push_back(row[*source[i]]);
            } else {
                physical.emplace_back().type = parser::ValueType::kNull;
            }
        }
    }
    return out;
}

// **Each row in the shape everything downstream reads**: the tuple key value first
// when the row names one, then one value per physical column, a NULL
// placeholder at every dropped position (BW-R6) - so the body is indexed by
// `column_no - 1` whatever was dropped. A row whose tuple key is `NULL` asks for an
// issued id, as a KWP load row's does (bulkinsert.md BI15), and loses it.
// A column-listed statement always takes `NormalizeListedInsertRows`, to
// which BW-Q9 below never applies (BW-Q12.9).
//
// **BW-Q9 (`[quiet-wrong]`)**: on a relation whose column set has changed
// (`RowLayout::column_set_changed`), a positional row carries every live
// column. The short form - the tuple key left out - is refused there,
// because after an `ADD` its length is that of a row written for the old
// columns with its tuple key named, and admitting it would put every value
// one column over with no error. **What a positional row's length cannot
// catch** (BW-Q11, `known-gaps.md`): after a `DROP`, an old row with its
// tuple key left out has the new full length. A column list closes it
// (BW-S6).
//
// **A heap's row is the short form, always** (BZ-R3 of
// `instructions/v3.2.0/workorder-bz-heap-hidden-key.md`): it carries exactly
// the visible columns and its key is issued - no length names the hidden key,
// so BW-Q9's ambiguity cannot arise and a changed heap takes the same rule.
// `[quiet-wrong]`: a heap row one value longer must be refused, never read
// as naming the key. A KWP load describes the hidden key as its field 0
// (BI15), so its rows take the general rule.
StatusOr<std::optional<parser::InsertStmt>> NormalizeInsertRows(
    const catalog::TableAccess& ta, const parser::InsertStmt& written) {
    if (!written.columns.empty()) {
        auto listed = NormalizeListedInsertRows(ta, written);
        if (!listed.ok()) return listed.status();
        return std::optional<parser::InsertStmt>(std::move(listed.value()));
    }
    const bool heap = ta.schema.heap() && written.described.oid == 0;
    const std::size_t live = ta.layout.live_columns;
    const bool changed = ta.layout.column_set_changed();
    // A row the copy below must reshape or refuse: on a heap, any row but
    // the short form; elsewhere one asking for an issued key with a NULL.
    const auto needs_copy = [&](const std::vector<parser::AstValue>& row) {
        if (heap) return row.size() + 1 != live;
        return row.size() == live && row.front().type == parser::ValueType::kNull;
    };
    // The common case copies nothing: an unchanged relation's rows are
    // already the shape.
    if (!changed && std::none_of(written.rows.begin(), written.rows.end(), needs_copy)) {
        return std::optional<parser::InsertStmt>();
    }
    parser::InsertStmt out = InsertShellOf(written);
    out.rows.reserve(written.rows.size());
    for (std::size_t k = 0; k < written.rows.size(); ++k) {
        const std::vector<parser::AstValue>& row = written.rows[k];
        const bool full = !heap && row.size() == live;
        if (!full && ((changed && !heap) || row.size() + 1 != live)) {
            // The row's ordinal, as the bulk loop appends to its refusals
            // (BB-S3's review): a KWP client reads the `Status`, not the line.
            const std::string at =
                written.rows.size() > 1 ? " (row " + std::to_string(k + 1) + ")" : "";
            const std::string byte = !row.empty() && row.front().byte_offset != 0
                                         ? " (byte " + std::to_string(row.front().byte_offset) +
                                               ")"
                                         : "";
            if (heap) {
                return Status::InvalidArgument("expected " + std::to_string(live - 1) +
                                               " value(s), one per column; got " +
                                               std::to_string(row.size()) + byte + at);
            }
            if (!changed) {
                return Status::InvalidArgument(InsertArityRefusal(ta, live, row).message() + at);
            }
            return Status::InvalidArgument(
                "expected " + std::to_string(live) + " value(s), one per column, with "
                "tuple key column '" +
                std::string(catalog::NameView(ta.schema.columns.front().name)) +
                "' NULL to have it issued; got " + std::to_string(row.size()) + byte +
                ". The relation's columns were changed by ALTER TABLE, so a row that leaves "
                "the key out is refused: it cannot be told from a row written for the old "
                "columns" + at);
        }
        const bool names_key = full && row.front().type != parser::ValueType::kNull;
        std::vector<parser::AstValue>& physical = out.rows.emplace_back();
        physical.reserve(ta.schema.columns.size());
        auto next = row.begin() + (full && !names_key ? 1 : 0);
        for (std::size_t i = names_key ? 0 : 1; i < ta.schema.columns.size(); ++i) {
            if (ta.schema.columns[i].dropped()) {
                physical.emplace_back().type = parser::ValueType::kNull;
            } else {
                physical.push_back(*next++);
            }
        }
    }
    return std::optional<parser::InsertStmt>(std::move(out));
}

}  // namespace

std::vector<parser::AstValue> CommandDispatcher::InsertBodyOf(
    const catalog::TableAccess& ta, const std::vector<parser::AstValue>& values) {
    const std::size_t ncols = ta.schema.columns.size();
    if (ncols > 0 && values.size() == ncols) {
        return std::vector<parser::AstValue>(values.begin() + 1, values.end());
    }
    if (ncols > 0 && values.size() == ncols - 1) return values;
    // Neither legal arity. Empty rather than a guess: `InsertOneRow` refuses
    // this row by name, and resolving foreign keys against a shape nobody
    // can read would be resolving the wrong columns.
    return {};
}

namespace {

// The Keystone id an integer value names, or nothing when it names none: an
// id is in [kFirstRowId, kMaxKeystoneId] (invariant 11), so a negative value,
// zero (`-0` too), a value past 2^48 - 1, and a literal past int64 (a uint64
// above INT64_MAX among them, whose `int_val` has wrapped - `2^64 + 5` must
// not read as 5) are none. The one answer for a named tuple key (`OutOfRange`,
// BD-R5) and a foreign-key value (an absent parent, `FK_VIOLATION`, not
// looked up); each caller words its own refusal.
std::optional<std::uint64_t> KeystoneIdOf(const parser::AstValue& value) {
    if (parser::IntLiteralWrapped(value) ||
        value.int_val < static_cast<std::int64_t>(catalog::kFirstRowId) ||
        static_cast<std::uint64_t>(value.int_val) > kMaxKeystoneId) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(value.int_val);
}

}  // namespace

Status CommandDispatcher::ResolveForeignKeyParents(const catalog::TableAccess& child,
                                                    const std::vector<parser::AstValue>& body,
                                                    exec::FkParentVerdicts& into,
                                                    const WriteScope& scope) {
    for (const catalog::ForeignKeyRef& fk : child.fkeys_out) {
        if (fk.column_no == 0 || fk.column_no > body.size()) continue;
        const parser::AstValue& value = body[fk.column_no - 1];
        // The same bail `CheckForeignKeyOnWrite` takes, and it must be the
        // same one: a value that is not an id cannot reference one, and
        // resolving it would put a nonsense tuple key in the held set. An integer
        // that is no id is that check's violation, with nothing to look up.
        if (value.type != parser::ValueType::kInt) continue;
        const std::optional<std::uint64_t> id = KeystoneIdOf(value);
        if (!id.has_value()) continue;

        // ---- The one thing this pass does not hoist -------------------
        //
        // **A self-referencing foreign key stays a per-row check.** Hoisting
        // it would change answers: in `INSERT INTO t VALUES (1, NULL),
        // (2, 1)` on a `t` that references itself, row 2's parent is row 1,
        // which is written *by this statement* - so a verdict taken before
        // any row work says "no such parent" where today's per-row check,
        // running after row 1 landed, says pass.
        //
        // Carving it out costs nothing AH wants, and it rests on the
        // ordering above alone.
        if (fk.rel_oid == child.oid) continue;

        const std::uint64_t tuple_key = *id;
        if (into.Find(fk.rel_oid, tuple_key) != nullptr) continue;  // AH-R2's dedup

        auto parent = catalog_.InitTableAccess(fk.rel_oid);
        if (!parent.ok()) return parent.status();

        // The owner question and the foreign-key probe retired (AT-S5f);
        // their history is at cf889b63. The check runs here for every
        // parent, under the same view and through the same descent, which
        // is the whole of D5's shape (AT-R15).

        // ---- D9(a): `IS`, then `S`, then the descent (AY-R4) ------------
        //
        // Held to the decide, so a parent `DELETE` waits for this child on
        // any core (`foreign-keys.md` §2a, §3a). **The `S` before the
        // descent** (AY-Q2, `[quiet-wrong]` if reversed): after it, a whole
        // `DELETE` of the parent fits between the read and the hold. A
        // refused `S` is the wait - the statement has written nothing and
        // runs again at the release - and the cap refuses it past
        // `max_locks_per_txn` (AO-R10).
        if (std::optional<Status> held =
                BorrowOrWait(scope, txn::LockKey::Tuple(fk.rel_oid, tuple_key),
                             RepeatableReadWait::kCapable, txn::LockMode::kShared)) {
            return ParentRowHeld(child, fk.rel_oid, tuple_key, *held);
        }

        // The view is minted **after** the grant, so a writer that decided
        // between the statement's start and the grant is visible to it
        // rather than read as busy.
        const txn::ReadView check_view = CheckView(scope);
        std::uint64_t busy_trx = 0;
        auto verdict = exec::CheckParentPresent(page_store_, *parent.value(), tuple_key, check_view,
                                                &budget_, &busy_trx);
        if (!verdict.ok()) return verdict.status();

        // **Busy under a granted `S` means no lock table**: every writer of
        // a row holds its `X` (AO-S6a), which would have refused the `S`.
        // Without a table the `IsInFlight` poll is the wait (AO-S3); with
        // one the busy verdict refuses retryably, defensively.
        if (verdict.value() == exec::FkVerdict::kBusy && busy_trx != 0 && locks_ == nullptr) {
            NoteBlockingWriter(scope.txn, busy_trx, tuple_key, RepeatableReadWait::kCapable);
        }

        // Recorded **per resolution, not per row**: a thousand-row insert
        // against one parent reports one lookup in `SHOW ACCESS`.
        RecordFkAccess(exec::AccessKind::kLookup, fk.rel_oid, 1);
        into.Put(fk.rel_oid, tuple_key, verdict.value());
    }
    return Status::OK();
}

Status CommandDispatcher::ParentRowHeld(const catalog::TableAccess& child,
                                        catalog::Oid parent_rel, std::uint64_t tuple_key,
                                        const Status& held) {
    // The hold's own refusal names a row and its holder; the client asked
    // about a foreign key, so a conflict says which one. The cap's
    // `ResourceExhausted` is about the transaction, and stands as it is.
    if (held.code() != StatusCode::kTxnConflict) return held;
    return Status::TxnConflict("the foreign key on '" + RelationNameOf(child.oid) +
                               "' cannot check row id=" + std::to_string(tuple_key) + " of '" +
                               RelationNameOf(parent_rel) + "' yet: " + std::string(held.message()));
}

Status CommandDispatcher::CheckForeignKeyOnWrite(const catalog::TableAccess& child,
                                                 const catalog::ForeignKeyRef& fk,
                                                 const parser::AstValue& value,
                                                 const exec::FkParentVerdicts& held,
                                                 const WriteScope& scope) {
    // A value that is not an id cannot reference one. Left alone rather than
    // failed here, and the same bail carries two different outcomes:
    //   - a NULL is MATCH SIMPLE's vacuous pass - the codec stores it if the
    //     column was declared NULL, and refuses it by name if not, so the
    //     NOT NULL refusal is the gate and no kFkNullable read is needed;
    //   - a wrong-typed value is refused by the codec a moment later with a
    //     message about the column's declared type, which is the better
    //     error - a type mistake reported as a constraint violation sends
    //     the reader looking at the wrong table.
    // An integer is not a type mistake, and one that is no Keystone id -
    // `-7`, `0`, past 2^48 - 1 - names no parent that can exist: the
    // violation, with no lock taken and nothing descended.
    if (value.type != parser::ValueType::kInt) return Status::OK();

    const std::optional<std::uint64_t> id = KeystoneIdOf(value);
    // 0 for no id: no `S` is ever held on tuple key 0, so the release below meets
    // nothing of it.
    const std::uint64_t parent_tuple_key = id.value_or(0);

    // The held verdict, resolved by `ResolveForeignKeyParents` before any
    // row work (§2a). No descent happens here and none may be added: this
    // call site is inside an open `WriteScope`, which is the place AH-R1
    // exists to keep free of anything that could need to wait.
    exec::FkVerdict resolved{};
    if (!id.has_value()) {
        resolved = exec::FkVerdict::kViolation;
    } else if (const exec::FkVerdict* found = held.Find(fk.rel_oid, parent_tuple_key);
               found != nullptr) {
        resolved = *found;
    } else if (fk.rel_oid == child.oid) {
        // The self-referencing arm, the one case the extraction pass
        // deliberately skips (see there).
        //
        // **It takes the hoist's pair, per row** (AY-Q3,
        // `raft-marks-2026-09-30.md` §3): `IS` on the relation - which the
        // write already covers with its `IX` - then `S` on the parent row,
        // then the descent, held to the decide. A busy parent is waited
        // for rather than refused: a refused `S` records its wait exactly as
        // the insert's own tuple borrow records its (`BorrowOrWait`), and
        // the statement ends there to park. A parent this statement wrote
        // itself is its own `X`, which never conflicts with its own `S`.
        if (std::optional<Status> row_held =
                BorrowOrWait(scope, txn::LockKey::Tuple(fk.rel_oid, parent_tuple_key),
                             RepeatableReadWait::kCapable, txn::LockMode::kShared)) {
            return ParentRowHeld(child, fk.rel_oid, parent_tuple_key, *row_held);
        }
        auto parent = catalog_.InitTableAccess(fk.rel_oid);
        if (!parent.ok()) return parent.status();
        auto verdict = exec::CheckParentPresent(page_store_, *parent.value(), parent_tuple_key,
                                                CheckView(scope), &budget_);
        if (!verdict.ok()) return verdict.status();
        RecordFkAccess(exec::AccessKind::kLookup, fk.rel_oid, 1);
        resolved = verdict.value();
    } else {
        // AH-R3: a statement whose parent set could not be enumerated is
        // refused rather than run against a partial one. Refused and never
        // quietly re-checked here, because a silent re-check is exactly how
        // the descent gets back inside the write scope and the crossing
        // becomes unreachable again.
        //
        // **`NotImplemented` and not `Corruption`**: nothing on disk
        // disagrees with anything. What has happened is that a statement
        // shape reached the write path which the extraction pass does not
        // enumerate - which is the two-code rule's "nobody built this yet",
        // exactly. Unreachable today (F1 makes every fk value a literal or
        // a bound parameter, so the pass is total), and written out because
        // "can't happen" is not a thing to encode as silence.
        return Status::NotImplemented(
            "the foreign key on '" + RelationNameOf(child.oid) + "' names parent row id=" +
            std::to_string(parent_tuple_key) + " of '" + RelationNameOf(fk.rel_oid) +
            "', which this statement's shape did not resolve before writing "
            "(docs/spec/foreign-keys.md §2a)");
    }

    const std::string column =
        fk.column_no < child.schema.columns.size()
            ? std::string(catalog::NameView(child.schema.columns[fk.column_no].name))
            : std::to_string(fk.column_no);

    switch (resolved) {
        case exec::FkVerdict::kPass:
            return Status::OK();
        case exec::FkVerdict::kBusy:
            // **Reached only by a writer the lock table does not know**
            // since AY-S5: both arms take the parent row's `S` before they
            // descend, and a writer holding its `X` refuses that ask, which
            // is a wait. What is left is a dispatcher with no table, where
            // the extraction pass recorded `NoteBlockingWriter`'s poll and
            // the self-referencing arm records nothing. Retryable: the
            // holder decides and the retry gets a real answer.
            return Status::TxnConflict("row id=" + std::to_string(value.int_val) + " of '" +
                                       RelationNameOf(fk.rel_oid) +
                                       "' is being written by another transaction that has "
                                       "not decided, so the foreign key on '" +
                                       column + "' cannot be checked yet");
        case exec::FkVerdict::kViolation:
            break;
    }
    // **Every absent parent's `S` goes with the violation, whoever took it**
    // (AZ-S5, AZ-R5 as amended 2026-10-02): this one's, and every other the
    // statement resolved as absent, whose rows it never reaches. None
    // protects a row, and kept they refused every insert of those parents
    // until the client's `ROLLBACK` (`foreign-keys.md` §2c says why it is
    // sound). A present parent's `S` stays, and a row `X` this transaction
    // holds is untouched; nothing held is a no-op.
    // **The relation's `IS` stays**: the statement's present parent rows
    // stand under it, and without it a relation `X` could be granted over
    // them.
    // One pass over the ledger, whatever the count (`LockTable::ReleaseIf`).
    // The failing key is named on its own for the self-referencing arm,
    // whose verdict is not in `held`; any other is in `held` already.
    if (locks_ != nullptr && scope.txn != nullptr) {
        const txn::LockKey failing = txn::LockKey::Tuple(fk.rel_oid, parent_tuple_key);
        locks_->ReleaseIf(scope.txn->id(), scope.txn->borrows(),
                          [&](const txn::LockHoldings::Held& h) {
                              if (h.mode != txn::LockMode::kShared ||
                                  h.key.unit != txn::LockUnit::kTuple) {
                                  return false;
                              }
                              if (h.key == failing) return true;
                              const exec::FkVerdict* verdict = held.Find(h.key.rel_oid, h.key.lo);
                              return verdict != nullptr &&
                                     *verdict == exec::FkVerdict::kViolation;
                          });
    }
    // The value as written - a literal's own digits, which a wrapped
    // `int_val` is not - and its byte (a computed value carries its
    // expression's).
    return Status::FkViolation("'" + column + "' references row id=" +
                               exec::FormatValue(0, value) + " of '" +
                               RelationNameOf(fk.rel_oid) + "', which does not exist (byte " +
                               std::to_string(value.byte_offset) + ")");
}

Status CommandDispatcher::CheckNoChildrenBeforeDelete(const catalog::TableAccess& parent,
                                                      std::uint64_t parent_tuple_key,
                                                      const WriteScope& scope) {
    // **Minted here, after the caller holds the parent row's `X`** - the
    // forward hoist's rule from the other side, and load-bearing for the
    // moved-row read (AY-Q8). A child that referenced this parent was
    // written under its `S`, so its writer decided before that grant; a view
    // minted earlier - once per statement, as this was - can see that writer
    // as invisible, and `ResolveThroughUndo` then answers an inserted
    // version "no version" where it references the parent: a `DELETE`
    // reaching row 7 late in its walk, after a child of 7 was inserted and
    // committed and an undecided `UPDATE` moved it off 7, answered "no
    // children", and that `UPDATE`'s rollback restored a child of a
    // deleted parent.
    const txn::ReadView check_view = CheckView(scope);
    // The reference intent and the reverse fan-out retired (AT-S5f); their
    // history is at cf889b63. A child row is a row every core can read, so
    // the walk below sees it: an uncommitted one answers `kBusy` and a
    // committed one `kViolation` (AT-R15, D5).

    // RESTRICT (F2): the first child that still references this row refuses
    // the delete. No action of any kind is taken on the child - v1 never
    // writes to the other relation, which is what CASCADE would start.
    for (const catalog::ForeignKeyRef& fk : parent.fkeys_in) {
        auto child = catalog_.InitTableAccess(fk.rel_oid);
        if (!child.ok()) return child.status();

        exec::FkReverseOptions options;
        if (txn_ != nullptr) options.undo = &txn_->undo();
        const catalog::TableAccess::CabinRef cabin = child.value()->CabinOn(fk.column_no);
        if (cabins_ != nullptr && cabin.id != 0) {
            options.cabins = cabins_;
            options.cabin_id = cabin.id;
        }

        auto outcome = exec::CheckNoChildReferences(page_store_, *child.value(), fk.column_no,
                                                    parent_tuple_key, check_view, options, &budget_);
        if (!outcome.ok()) return outcome.status();

        // A cabin-served check probed one value's set; a walk read the
        // relation filtered on one column. Two different shapes, recorded as
        // what they were.
        RecordFkAccess(outcome.value().served_from_cabin ? exec::AccessKind::kCabinProbe
                                                         : exec::AccessKind::kFilterScan,
                       fk.rel_oid, std::uint64_t{1} << fk.column_no);

        const std::string column =
            fk.column_no < child.value()->schema.columns.size()
                ? std::string(catalog::NameView(child.value()->schema.columns[fk.column_no].name))
                : std::to_string(fk.column_no);

        switch (outcome.value().verdict) {
            case exec::FkVerdict::kPass:
                continue;
            case exec::FkVerdict::kBusy: {
                // **A child writer that holds no `S` on this parent is
                // waited for, not refused** (AY-Q8). A writer that *set*
                // the fk column to this parent - an insert, or an update of
                // the column - holds its `S` since AY-S5, and the row's `X`
                // this statement took before the walk was refused by it.
                // What reaches here is every other undecided writer of a
                // referencing row - a child `DELETE`, an `UPDATE` of other
                // columns or one moving the column off - and its decide is
                // what the answer depends on. The caller parks on it.
                if (outcome.value().busy_trx != 0) {
                    WaitForChildRowWriter(scope, fk.rel_oid, outcome.value().busy_tuple_key,
                                          outcome.value().busy_trx);
                }
                return Status::TxnConflict("a row of '" + RelationNameOf(fk.rel_oid) +
                                           "' referencing id=" + std::to_string(parent_tuple_key) +
                                           " is being written by another transaction");
            }
            case exec::FkVerdict::kViolation:
                return Status::FkViolation("row id=" + std::to_string(parent_tuple_key) +
                                           " is still referenced by '" +
                                           RelationNameOf(fk.rel_oid) + "." + column + "'");
        }
    }
    return Status::OK();
}

void CommandDispatcher::WaitForChildRowWriter(const WriteScope& scope, catalog::Oid child_rel,
                                              std::uint64_t tuple_key, std::uint64_t holder) {
    // **The wake is the child row's own**, asked for the way the forward
    // check asked for its parent's until AY-S5 folded that into its `S`: the
    // writer holds the row's `X` (AO-S6a), the table is the instance's, and
    // the release flips the slot from whichever core releases. A refused
    // ask leaves the table as it found it but for the registration, and a
    // granted one - the writer released between the walk and this line -
    // is given back at once, with a wake already flipped so the statement
    // runs again rather than being refused for a wait that has ended (the
    // stale-memo arm of `BorrowChain` does the same). Nothing here outlives
    // the ask, which is why no intention is taken above it.
    //
    // **Not over a hold this transaction already has**: the child row is a
    // parent of its own children, and an `S` taken on it for a grandchild's
    // reference would be the one the grant arm's release drops.
    const txn::LockKey unit = txn::LockKey::Tuple(child_rel, tuple_key);
    if (locks_ != nullptr && scope.txn != nullptr && may_park_ &&
        !scope.txn->borrows().Holds(unit)) {
        std::uint64_t blocker = 0;
        txn::LockWake wake;
        auto took = locks_->TryAcquire(scope.txn->id(), unit, txn::LockMode::kShared,
                                       scope.txn->borrows(), &blocker, &wake);
        if (took.ok() && took.value()) {
            locks_->ReleaseOne(scope.txn->id(), unit, txn::LockMode::kShared, scope.txn->borrows());
            auto flipped = std::make_shared<txn::LockWaitSlot>();
            flipped->ready.store(true, std::memory_order_release);
            TakeLockWait(blocking_wake_,
                         DispatchOutcome::LockWait{unit, holder, std::move(flipped), true,
                                                   txn::LockMode::kShared});
        } else if (took.ok() && wake.slot != nullptr) {
            TakeLockWait(blocking_wake_,
                         DispatchOutcome::LockWait{wake.key, blocker != 0 ? blocker : holder,
                                                   std::move(wake.slot), true,
                                                   txn::LockMode::kShared});
        }
    }
    // The reverse check reads latest state and the writer's decide changes
    // it either way, so the re-run is not futile at any level.
    NoteBlockingWriter(scope.txn, holder, tuple_key, RepeatableReadWait::kCapable);
}

StatusOr<catalog::Oid> CommandDispatcher::ResolveCreateNamespace(std::string_view qualifier,
                                                                std::uint32_t byte_offset,
                                                                const Session& session,
                                                                const txn::ReadView* view) {
    // The session's namespace was live at its `USE`; whether it still is,
    // `Catalog::CreateTable` asks under the hold a drop's RESTRICT takes.
    if (qualifier.empty()) return session.default_namespace();
    auto ns = catalog_.FindNamespaceOidByName(qualifier, view);
    if (ns.ok()) {
        // `sys` resolves - it has to, or `sys.tables` would stop naming the
        // views - and creating *into* it is refused here rather than at the
        // catalog, where the message would be about a system relation
        // instead of about the namespace that was written.
        if (catalog::IsSystemNamespace(ns.value())) {
            return Status::InvalidArgument("namespace '" + std::string(qualifier) +
                                            "' is the catalog's and holds no user relation "
                                            "(byte " + std::to_string(byte_offset) + ")");
        }
        return ns.value();
    }
    if (ns.status().code() != StatusCode::kNotFound) return ns.status();
    return Status::NotFound("no namespace named '" + std::string(qualifier) +
                            "' (byte " + std::to_string(byte_offset) +
                            "); CREATE NAMESPACE it first - a namespace is never created by "
                            "being named");
}

std::string CommandDispatcher::RelationNameOf(catalog::Oid oid) {
    // Unfiltered by design (DT3c): this renders a name for an oid the
    // caller already holds, so hiding it would print an empty label
    // rather than protect anything.
    if (auto tables = catalog_.ListTables(); tables.ok()) {
        for (const catalog::SysObjectRow& obj : tables.value()) {
            if (obj.oid == oid) return std::string(catalog::NameView(obj.name));
        }
    }
    return "oid=" + std::to_string(oid);
}

// One parsed column definition as the `sys.columns` row it declares:
// its type resolved and every argument checked, each refusal with its byte.
// `CREATE TABLE`'s and `ALTER TABLE ... ADD COLUMN`'s (BW-R1), so the two
// cannot accept different spellings of one type.
std::optional<DispatchOutcome> CommandDispatcher::ColumnRowOf(const parser::ColumnDef& col,
                                                            std::uint32_t pos,
                                                            catalog::SysColumnRow& row) {
    // A name the catalog's fixed-width field cannot hold is refused, never
    // truncated: `SetName` keeps at most `kCatalogNameMax - 1` bytes, and a
    // truncated name is not the one that was declared - a read of it would
    // miss, and two names sharing their first 63 bytes would be one name.
    // RENAME COLUMN's own bound (`CheckRenameName`).
    if (col.name.size() >= catalog::kCatalogNameMax) {
        const Status s = Status::InvalidArgument(
            "column name '" + col.name + "' is longer than " +
            std::to_string(catalog::kCatalogNameMax - 1) + " bytes (byte " +
            std::to_string(col.name_byte_offset) + ")");
        return DispatchOutcome{ErrorReply(s), false, 0, s};
    }
    auto type_row = catalog_.ResolveTypeByName(col.type_name);
    if (!type_row.ok()) {
        return DispatchOutcome{ErrorReply(type_row.status()), false, 0, type_row.status()};
    }

    // A cabin policy on the tuple key is refused rather than ignored.
    // The tuple key's cabin is the clustered tree (spec §2), so any of the
    // three answers would be a statement about something that cannot
    // exist - and silently dropping the clause would leave an operator
    // believing they had said something.
    if (pos == 0 && col.cabin_policy != catalog::kCabinPolicyUnset) {
        return DispatchOutcome{"ERR the tuple key column '" + col.name +
                    "' takes no cabin policy - the clustered tree is its cabin (byte " +
                    std::to_string(col.cabin_byte_offset) + ")",
                false};
    }
    // Invariant 11 at the surface, with the byte the layout's own
    // defense cannot know: the first column is the tuple key, carried by
    // the Keystone word, which has no NULL encoding.
    if (pos == 0 && !col.notnull) {
        return DispatchOutcome{"ERR the tuple key column '" + col.name +
                    "' cannot be declared NULL - the tuple key is carried by the Keystone "
                    "word, which has no NULL encoding (byte " +
                    std::to_string(col.null_byte_offset) + ")",
                false};
    }

    row = catalog::SysColumnRow{};
    row.pos = pos;
    catalog::SetName(row.name, col.name);
    row.type_val = type_row.value().type_val;
    row.len = type_row.value().len;
    row.notnull = col.notnull;  // D1: NOT NULL unless declared NULL
    row.cabin_policy = col.cabin_policy;

    // ---- The arity refusals, both above the type arms ---------------
    //
    // A type that takes no arguments refuses the ones it was given,
    // rather than dropping them: silently ignoring an argument leaves
    // an operator believing they said something. Both are unreachable
    // through the parser, which decides arity by the type name - so
    // this is the catalog's own defense against a statement that
    // arrived by another door, and the two live together because a
    // third argument-taking type must find one place to extend.
    const bool takes_precision = row.type_val == catalog::kTypeValDecimal ||
                                  row.type_val == catalog::kTypeValDecimalWide;
    const bool takes_width = row.type_val == catalog::kTypeValChar ||
                              row.type_val == catalog::kTypeValVarchar;
    if ((col.has_precision && !takes_precision) || (col.has_width && !takes_width)) {
        return DispatchOutcome{"ERR type '" + col.type_name + "' takes no arguments (byte " +
                    std::to_string(col.type_arg_byte_offset) + ")",
                false};
    }

    // ---- decimal(p, s) (docs/spec/types.md TY2, TY9) ----------------
    //
    // The pair replaces the type's default `len`, which for a decimal
    // was never read as a width - `RowLayout::ColumnWidth` gives every
    // decimal its bytes from its `type_val` alone (catalog/rows.hpp
    // says why the field was free).
    //
    // **The declared precision selects the width, here and only here.**
    // `decimal(p, s)` with p <= 18 is the 8-byte type and with p >= 19
    // the 16-byte one (`kTypeValDecimalWide`) - TY2's separate type,
    // not a widening, so the promotion is a different type_val and a
    // different schema constant, chosen from the one fact the client
    // declared.
    //
    // **`decimal128(p, s)` is not a spelling this code ever sees**:
    // the parser admits a type-argument list for `DECIMAL`, `CHAR`
    // and `VARCHAR` only, so it is refused as taking no arguments.
    // The wide type is reached by `decimal(p, s)` with p >= 19, the
    // promotion above - one type per declaration, by precision.
    if (row.type_val == catalog::kTypeValDecimal ||
        row.type_val == catalog::kTypeValDecimalWide) {
        if (!col.has_precision) {
            // Unreachable through the parser, which refuses a bare
            // `decimal`. Checked anyway: a schema can be built without
            // one, and a decimal with no scale stored is a column whose
            // values have no defined meaning.
            return DispatchOutcome{"ERR column '" + col.name + "' is decimal with no precision or scale",
                    false};
        }
        if (row.type_val == catalog::kTypeValDecimal &&
            col.precision >= exec::kMinDecimalPrecisionWide) {
            row.type_val = catalog::kTypeValDecimalWide;
        }
        Status bounds = row.type_val == catalog::kTypeValDecimalWide
                            ? exec::CheckDecimalWidePrecisionScale(col.precision, col.scale)
                            : exec::CheckDecimalPrecisionScale(col.precision, col.scale);
        if (!bounds.ok()) {
            return DispatchOutcome{ErrorReply(bounds) + " (byte " +
                        std::to_string(col.type_byte_offset) + ")",
                    false};
        }
        row.len = catalog::PackDecimalLen(static_cast<std::uint8_t>(col.precision),
                                          static_cast<std::uint8_t>(col.scale));
    } else if (row.type_val == catalog::kTypeValChar) {
        // ---- char(N) / varchar(N) --------------------------------
        //
        // **The two say different things with one number**, and the
        // difference is the whole design:
        //
        //   char(N)     N is the cell. The value lives in exactly
        //               those bytes or is refused.
        //   varchar(N)  N is *this column's* kds.inline_cell_width -
        //               a width, not a cap. It is therefore
        //               validated by the instance setting's own
        //               validator and never by a second one, which
        //               is the operator's rule and the reason no
        //               `max_inline_char_size` exists.
        if (col.has_width) {
            if (col.width == 0) {
                return DispatchOutcome{"ERR column '" + col.name +
                            "' cannot be char(0) - every column must occupy bytes (byte " +
                            std::to_string(col.type_arg_byte_offset) + ")",
                        false};
            }
            row.len = col.width;
        }
        // Nothing said: `char` is `char(1)`, which is the sys.types
        // default already in `row.len` and what the standard means
        // by a bare `char`. Not refused the way a bare `decimal` is
        // - that refusal guards a silent decision about what a
        // stored value *means*, and char(1) decides nothing.
    } else if (row.type_val == catalog::kTypeValVarchar) {
        if (col.has_width) {
            if (Status s = storage::CheckInlineCellWidth(col.width); !s.ok()) {
                // Through `ErrorReply` like every other refusal in
                // this file: the width validator answers
                // InvalidArgument today, which renders bare, so the
                // line is unchanged - but a coded refusal reaching
                // this one path bare is exactly the hole the rest of
                // the file just closed.
                return DispatchOutcome{ErrorReply(s.WithContext("column '" + col.name + "'")) + " (byte " +
                            std::to_string(col.type_arg_byte_offset) + ")",
                        false};
            }
            row.len = col.width;
        }
        // Nothing said: `len` stays 0, which is what every varchar
        // column written before this version carries and what it has
        // always meant - the instance's pinned width. That is the
        // whole compatibility story; no file changes, no bump.
    }

    return std::nullopt;
}

DispatchOutcome CommandDispatcher::HandleCreateTableSql(std::string_view line,
                                                        Session& session) {
    return InDdlStatement(session, [&](WriteScope& scope) -> DispatchOutcome {
        auto parsed = parser::Parse(line);
        if (!parsed.ok()) {
            return {ErrorReply(parsed.status()), false, 0, parsed.status()};
        }
        if (!std::holds_alternative<parser::CreateTableStmt>(parsed.value())) {
            return {"ERR expected a CREATE TABLE statement", false};
        }
        auto& stmt = std::get<parser::CreateTableStmt>(parsed.value());

        // The qualifier decides the namespace; an unqualified name lands in
        // the session's (`USE`, namespace.md NS7). A named namespace must
        // already exist, because implicit creation is what makes a typo
        // indistinguishable from an intent (AF-6, shape (a) over (b)).
        //
        // **Resolved before the name-collision check below**, which asks
        // for the name in that namespace alone (NS6).
        auto create_ns = ResolveNames(session, [&](const txn::ReadView* v) {
            return ResolveCreateNamespace(stmt.schema, stmt.table_byte_offset, session, v);
        });
        if (!create_ns.ok()) {
            return {ErrorReply(create_ns.status()), false, 0, create_ns.status()};
        }

        // The duplicate check, answered early so a taken name costs no type
        // resolution and no foreign-key lookup; `ExistingRelationReply` says
        // why it is unfiltered and why it is not the answer that binds.
        if (auto exists = ExistingRelationReply(create_ns.value(), stmt.table_name);
            exists.has_value()) {
            return *exists;
        }

        // Resolve each column's parsed type_name against sys.types - the
        // stand-in type registry (Catalog::ResolveTypeByName(), see its
        // comment) - before touching storage, so a bad type name fails clean
        // with nothing created.
        catalog::Schema schema;
        schema.spare_slots = stmt.spare_slots;
        // A heap's tuple key is the engine's column 0 (BZ-R1), so its
        // declared columns start at position 1 and none of them is the key.
        if (stmt.heap) schema.columns.push_back(catalog::HiddenTupleKeyColumn());
        const std::size_t first_declared = schema.columns.size();
        std::uint32_t pos = static_cast<std::uint32_t>(first_declared);
        for (const auto& col : stmt.columns) {
            catalog::SysColumnRow row{};
            if (auto refused = ColumnRowOf(col, pos++, row); refused.has_value()) return *refused;
            // Column names are unique ignoring case
            // (`catalog::ColumnNamesCollide`); the later name is the one
            // refused, at its byte. `Catalog::CreateTable` asks again. Stored
            // name against stored name - the comparison the catalog makes -
            // though `ColumnRowOf` refusing an unstorable length makes each
            // the name as written.
            for (const catalog::SysColumnRow& earlier : schema.columns) {
                if (catalog::ColumnNamesCollide(catalog::NameView(earlier.name),
                                                catalog::NameView(row.name))) {
                    const Status s = Status::InvalidArgument(
                        "column '" + col.name + "' repeats column '" +
                        std::string(catalog::NameView(earlier.name)) +
                        "' (names are unique ignoring case) (byte " +
                        std::to_string(col.name_byte_offset) + ")");
                    return {ErrorReply(s), false, 0, s};
                }
            }
            schema.columns.push_back(row);
        }

        // ---- REFERENCES, checked before anything is created -----------------
        //
        // A foreign key is a **constraint**, so it does not get the Cabin's
        // treatment below, where a failure is reported as a warning and the
        // table is created anyway: a relation that says REFERENCES and enforces
        // nothing is worse than a refused CREATE TABLE, and there is no DROP
        // TABLE to undo one with. Everything decidable without the child
        // relation existing is therefore decided here, with nothing written.
        struct PendingForeignKey {
            std::uint16_t column_no;
            catalog::Oid parent_oid;
            std::string parent_name;
        };
        std::vector<PendingForeignKey> pending_fkeys;

        for (std::size_t i = 0; i < stmt.columns.size(); ++i) {
            const parser::ColumnDef& col = stmt.columns[i];
            if (col.references_table.empty()) continue;

            // Under the session's view (DT3c): a child may reference a parent
            // its own transaction created, and may not reference one another
            // transaction has not committed.
            // `REFERENCES orders.customer` names where the parent lives; an
            // unqualified parent is in the session's namespace (NS5).
            auto parent_oid = ResolveNames(session, [&](const txn::ReadView* v) {
                return catalog_.ResolveRelation(col.references_schema, col.references_table,
                                                session.default_namespace(),
                                                col.references_byte_offset, v);
            });
            if (!parent_oid.ok()) {
                return {ErrorReply(parent_oid.status()), false, 0, parent_oid.status()};
            }
            // The parent's `IS`, held to the decide (BF-Q11 (a)), taken as
            // soon as the parent is named and before anything is written,
            // so a refusal leaves no rows and a wait can re-run: a drop of
            // the parent holding `X` refuses this create, and one arriving
            // after waits for it and then finds the foreign key on its
            // second RESTRICT ask. A drop that committed between the lookup
            // above and this grant is not closed here; the pages stay
            // allocated while this statement runs (BF-R9's epoch), and a
            // later bind of the dangling parent is refused (BF-R10).
            if (std::optional<Status> held =
                    BorrowRelationForDdl(scope.txn, parent_oid.value(), /*poisons=*/true,
                                         txn::LockMode::kIntentionShared);
                held.has_value()) {
                return {ErrorReply(*held), false, 0, *held};
            }
            auto parent = catalog_.InitTableAccess(parent_oid.value());
            if (!parent.ok()) {
                return {ErrorReply(parent.status()), false, 0, parent.status()};
            }

            // The shared declaration checks (catalog/foreign_key.hpp) - the same
            // ones Catalog::CreateForeignKey() applies at the door, so a
            // declaration cannot pass here and fail there.
            if (Status s = catalog::CheckForeignKeyDeclaration(
                    *parent.value(), schema.columns[first_declared + i],
                    static_cast<std::uint16_t>(first_declared + i));
                !s.ok()) {
                return {ErrorReply(s) + " (byte " +
                            std::to_string(col.references_byte_offset) + ")",
                        false};
            }
            pending_fkeys.push_back(PendingForeignKey{static_cast<std::uint16_t>(first_declared + i),
                                                      parent_oid.value(), col.references_table});
        }

        // ---- Storage (docs/spec/heap-and-tuple.md §4.1) --------------------------
        //
        // Every user relation is a btree. `HEAP` decided only whether its
        // tuple key is the engine's hidden column 0 (BZ-R1, above). There is
        // no key mode (removed 2026-08-25), so no `CREATE TABLE` shape is
        // refused for a key-mode reason.

        DdlScope ddl = DdlScopeFor(scope);
        auto oid = catalog_.CreateTable(create_ns.value(), stmt.table_name, schema, ddl.trx_id, ddl.sink());
        // Registered before the status is read: a create that failed partway
        // still left rows on the page, and those are exactly the rows a
        // rollback has to retire.
        const bool wrote_rows = !ddl.written.empty();
        NoteDdlRows(ddl);
        if (!oid.ok()) {
            // The name taken by another core between the early check and
            // the catalog's held one (AT-S17): the reply a second arrival
            // gets, `EXISTS`, which is not an `ERR` and poisons nothing -
            // and only while nothing was written, since a non-`ERR` reply
            // lets autocommit commit whatever was.
            if (oid.status().code() == StatusCode::kAlreadyExists && !wrote_rows) {
                if (auto exists = ExistingRelationReply(create_ns.value(), stmt.table_name);
                    exists.has_value()) {
                    return *exists;
                }
            }
            return {ErrorReply(oid.status()), false, 0, oid.status()};
        }

        // The constraints, now that there is a child relation to hang them on.
        // What can still fail here is catalog I/O. Since D2 the ERR
        // makes FinishDdlStatement abort the statement's transaction, so
        // the relation's own rows are taken back - the message must not
        // claim otherwise. What the abort does NOT take back is any
        // sys.fkeys row already written in this loop: CreateForeignKey
        // reports no CatalogRowRef, so those rows never reach the trail
        // (`git show 925f483:docs/workplan-rv3-catalog-recovery.md`'s remainder).
        // The cross-owner foreign-key notice retired (AT-S5f, AT-S9); its
        // history is at cf889b63.
        for (const PendingForeignKey& fk : pending_fkeys) {
            auto created = catalog_.CreateForeignKey(oid.value(), fk.column_no, fk.parent_oid);
            if (!created.ok()) {
                // No survival claim in either direction: autocommit's
                // abort takes the relation back, an explicit transaction
                // keeps it until ROLLBACK (§6's per-transaction failure
                // atomicity) - a message asserting either would be false
                // in the other arm.
                return {"ERR CREATE TABLE '" + stmt.table_name +
                            "' failed: foreign key on column " +
                            std::to_string(fk.column_no) + " referencing '" + fk.parent_name +
                            "': " + created.status().message(),
                        false};
            }
        }

        // ---- `CABIN` on a column creates one now (docs/spec/cabin.md) -------
        //
        // The policy is enforced at exactly two moments, and this is the first:
        // an *enabled* column gets its Cabin as part of the CREATE TABLE that
        // declared it. The second is `Catalog::CreateCabin`, which refuses a
        // *disabled* column whoever asks.
        //
        // `kCabinPolicyAuto` does nothing here, by design: CREATE TABLE never
        // decides for the engine. The engine's decision is the cabin
        // optimizer's, made later and only while it is on (`rows.hpp`) - so
        // auto never quietly behaves like `enabled`.
        //
        // A failure here does not fail the CREATE TABLE. The relation exists and
        // is correct; what is missing is an accelerator, so it is a warning.
        // The row is the CREATE TABLE's own, on its trail, so a ROLLBACK
        // retires it with the relation.
        std::vector<std::string> warnings;
        for (const catalog::SysColumnRow& col : schema.columns) {
            if (catalog::EffectiveCabinPolicy(col.cabin_policy) != catalog::kCabinPolicyEnabled) {
                continue;
            }
            catalog::CatalogRowRef written;
            auto cabin = catalog_.CreateCabin(oid.value(), static_cast<std::uint16_t>(col.pos),
                                               catalog::kCabinOriginUser, ddl.trx_id, &written);
            // Before the status: a row logged before a later failure is
            // still the rollback's to retire.
            NoteRowWritten(ddl, written);
            if (!cabin.ok()) {
                warnings.push_back("column '" + std::string(catalog::NameView(col.name)) +
                                   "' asked for a cabin and did not get one: " +
                                   cabin.status().message());
            }
        }
        // Info: DDL is rare and changes the shape of everything after it, so
        // it belongs in a default-level log even though ordinary writes do not.
        if (logging(LogLevel::kInfo)) {
            log_->Info("ddl", "created table '" + std::string(stmt.table_name) +
                                  "' oid=" + std::to_string(oid.value()) +
                                  " columns=" + std::to_string(schema.columns.size()));
        }
        std::ostringstream created;
        created << "CREATED oid=" << oid.value();
        for (const std::string& warning : warnings) {
            created << "\\n" << "WARN " << warning;
        }
        return {created.str(), false};
    });
}

Status CommandDispatcher::LogIndexWrites(const std::vector<exec::IndexWrite>& writes,
                                         std::uint64_t txn_id) {
    for (const exec::IndexWrite& write : writes) {
        if (Status s = LogIndexWrite(write, txn_id); !s.ok()) return s;
    }
    return Status::OK();
}

// An index split's record holds every page it wrote and the anchor, under
// the stream's bound (`storage::LogBtreeSplit`'s size rule, for the index's
// own cap).
static_assert(index::kMaxIndexChanges + 1 <= wal::kMaxBtreeSplitPages);

Status CommandDispatcher::LogIndexWrite(const exec::IndexWrite& write, std::uint64_t txn_id) {
    if (wal_ == nullptr) return Status::OK();
    // A split's pages take one BTREE_SPLIT and **no** INDEX_INSERT: the
    // images are taken after the entry is in, so emitting both would apply
    // it twice. The clustered tree's record, for the clustered tree's
    // reason - one record replays whole or not at all, where a run of
    // FULL_PAGE_IMAGEs could end between a divided leaf and the parent that
    // routes to its new half - and a root the split grew rides in it
    // (`LogTreeSplit`).
    if (!write.restructured.empty()) {
        return LogTreeSplit(write.restructured, txn_id, write.rel_oid, write.index_oid,
                            write.new_root, write.anchor_page_id, write.anchor_pin);
    }

    std::vector<std::byte> buf(wal::kIndexInsertFixedSize + write.entry.size());
    const wal::IndexInsertPayload fields{write.slot,
                                         static_cast<std::uint16_t>(write.entry.size())};
    if (auto n = wal::EncodeIndexInsert(buf, fields, write.entry); !n.ok()) {
        return n.status();
    }
    auto rec = wal_->Append(
        wal::RecordSpec{wal::RecordType::kIndexInsert, txn_id, write.page_id}, buf);
    if (!rec.ok()) return rec.status();
    return page_store_.StampPageLsn(write.page_id, rec.value());
}

std::function<Status(const exec::AppendedSpill&)> CommandDispatcher::SpillLogFor(
    const WriteScope& scope, std::uint32_t rel_oid, std::uint64_t tuple_key) {
    if (scope.txn == nullptr) return nullptr;
    const std::uint64_t txn_id = scope.txn->id();
    return [this, &scope, rel_oid, tuple_key, txn_id](const exec::AppendedSpill& spill) -> Status {
        // RV3's order: the undo record that can release the value, then the
        // records that make it durable. **Logged even when the undo record
        // failed**: the append already wrote the page - a created page, the
        // link to it, the slot - and left unlogged, the next append into the
        // chain logs against a page redo never built, which refuses the mount.
        // Logged, the value is one leaked slot no row names and no undo
        // record releases.
        const Status noted = NoteSpills(scope, rel_oid, tuple_key, {spill});
        const Status logged = exec::LogSpill(wal_, page_store_, spill, txn_id, rel_oid);
        return noted.ok() ? logged : noted;
    };
}

exec::IndexWriteLog CommandDispatcher::IndexWriteLogFor(const WriteScope& scope,
                                                        std::vector<exec::IndexWrite>& pending) {
    if (wal_ == nullptr) return nullptr;
    if (scope.txn != nullptr) {
        const std::uint64_t txn_id = scope.txn->id();
        return [this, txn_id](const exec::IndexWrite& write) {
            return LogIndexWrite(write, txn_id);
        };
    }
    // Collected for `LogInsert`, which logs them under the transaction it
    // opens - but not every exit reaches it (an UPDATE never does, and an
    // INSERT's take-back exits before it), so a root a split grew is
    // published here with its own ANCHOR_UPDATE, as it was before the root
    // rode in the split's record, and the copy kept carries none.
    return [this, &pending](const exec::IndexWrite& write) -> Status {
        if (write.new_root == kInvalidPageId) {
            pending.push_back(write);
            pending.back().anchor_pin = nullptr;  // the pin dies with the insert's result
            return Status::OK();
        }
        if (Status s = catalog_.PublishRoot(write.rel_oid, write.index_oid, write.new_root,
                                            write.anchor_page_id);
            !s.ok()) {
            return s.WithContext("republishing the root of index " +
                                 std::to_string(write.index_oid));
        }
        pending.push_back(write);
        pending.back().new_root = kInvalidPageId;
        pending.back().anchor_pin = nullptr;  // the pin dies with the insert's result
        return Status::OK();
    };
}

Status CommandDispatcher::LogFullPageImage(PageId page_id, std::uint64_t txn_id) {
    // The one writer (storage/log_page_image.hpp), kept as a member only
    // for its callers' brevity.
    return storage::LogFullPageImage(wal_, page_store_, txn_id, page_id);
}

// The durability a statement with no commit record owes its
// acknowledgement (workplan-rv3-catalog-recovery.md's remainder round):
// assertion, cabin and ALTER DDL write no commit record of their own, so
// nothing ever synced their records - an acknowledged CREATE ASSERTION
// could die with the WAL buffer, which is the durability promise broken.
//
// **D2 syncs here too, and that is not a policy choice.** `wal.md` §1 gives
// D2 the same durability point as D1 ("D1/D2 differ only in batching,
// never in the durability point"), and D2 is the default class. D2's
// batching lives in `pending_commit_lsn_`, which is keyed
// to a registered group commit (`DrainOnce` syncs only when
// `pending_group_commits_ > 0`); a statement with no commit record has
// nothing to register, so the honest way to keep D2's point is to sync.
// DDL is rare, so what that costs is one fsync per declaration.
// D3 keeps its loss window, exactly as it does for DML.
Status CommandDispatcher::AwaitDdlDurability() {
    if (wal_ == nullptr || effective_durability_ == wal::DurabilityClass::kRelaxed) {
        return Status::OK();
    }
    return wal_->SyncAll();
}


Status CommandDispatcher::NoteSpills(const WriteScope& scope, std::uint32_t rel_oid,
                                     std::uint64_t tuple_key,
                                     const std::vector<exec::AppendedSpill>& spills) {
    if (scope.txn == nullptr) return Status::OK();
    for (const exec::AppendedSpill& spill : spills) {
        if (Status s = txn_->NoteVarHeapAppend(*scope.txn, rel_oid, spill.ptr.page_id,
                                                spill.ptr.slot, tuple_key);
            !s.ok()) {
            return s;
        }
    }
    return Status::OK();
}

Status CommandDispatcher::LogInsert(storage::InsertPlacement& placed,
                                    std::span<const std::byte> tuple, std::uint64_t trx_id,
                                    std::uint64_t owner_oid, PageId anchor_page_id,
                                    const std::vector<exec::AppendedSpill>& spills,
                                    const std::vector<exec::IndexWrite>& index_writes,
                                    bool own_txn) {
    if (wal_ == nullptr) {
        // Nothing to log, and a root the split grew is published all the
        // same.
        return placed.restructured()
                   ? LogSplit(placed, wal::kNoTxnId, owner_oid, anchor_page_id)
                   : Status::OK();
    }

    // `own_txn` is false once a TransactionManager runs the transaction:
    // it already appended TXN_BEGIN at Begin() and will append TXN_COMMIT
    // at Commit(), so emitting a second pair here would describe two
    // transactions where one happened - and recovery would believe it.
    const std::uint64_t txn_id = own_txn ? next_txn_id_++ : trx_id;
    if (own_txn) {
        if (auto begun = wal_->Append(wal::RecordSpec{wal::RecordType::kTxnBegin, txn_id});
            !begun.ok()) {
            return begun.status();
        }
    }

    // **A B+ tree leaf logs one record for the row** (BD-R3 E2, BD-R12):
    // a BTREE_SPLIT carrying every page the split wrote, the row already in
    // whichever leaf took it, or a BTREE_INSERT naming the slot the row was
    // placed at. Either comes after the row's spills and index entries.

    // The var-heap values this tuple points at, before the tuple itself.
    // That order is the whole of the var-heap's recovery story (spec
    // section 5): a replay must never reach a cell whose pointer resolves
    // to nothing, and the reverse failure - a value with no tuple - is an
    // unreferenced value purge collects.
    if (Status s = exec::LogSpills(wal_, page_store_, spills, txn_id, owner_oid); !s.ok()) return s;

    // The index entries this row is now reachable through, before the row
    // itself (docs/spec/index.md §12.1). Same direction as the var-heap
    // above, reached from the opposite pointer: a dangling entry is dropped
    // by verification, a row with no entry is lost.
    if (Status s = LogIndexWrites(index_writes, txn_id); !s.ok()) return s;

    if (placed.restructured()) {
        // The split's one record, which carries the row: no insert record
        // follows it, as an index split logs no INDEX_INSERT.
        if (Status s = LogSplit(placed, txn_id, owner_oid, anchor_page_id);
            !s.ok()) {
            return s;
        }
    } else {
        // undo_ptr 0: an insert supersedes no version, so its undo chain
        // ends at itself (wal.md section 5.1).
        std::vector<std::byte> payload(wal::kHeapWriteFixedSize + tuple.size());
        const wal::HeapWritePayload fields{trx_id, /*undo_ptr=*/0, placed.slot,
                                           static_cast<std::uint16_t>(tuple.size())};
        if (auto n = wal::EncodeHeapWrite(payload, fields, tuple); !n.ok()) return n.status();

        auto rec = wal_->Append(
            wal::RecordSpec{wal::RecordType::kBtreeInsert, txn_id, placed.page_id},
            payload);
        if (!rec.ok()) return rec.status();
        if (Status s = page_store_.StampPageLsn(placed.page_id, rec.value()); !s.ok()) return s;
    }

    // Every record naming a page this insert holds is appended and stamped,
    // so the holds go - here, and not at the caller's return: an own
    // transaction's commit below may wait on durability, which never runs
    // under a latch.
    placed.held.clear();

    // The commit and its wait belong to whoever owns the transaction. When
    // a manager does, EndWrite() performs both.
    if (!own_txn) return Status::OK();

    auto commit = wal_->Commit(txn_id, effective_durability_);
    if (!commit.ok()) return commit.status();

    // kStrict already synced inside Commit(). kGroup did not: it staged
    // the commit for the next drain, and the acknowledgement owed to the
    // client is "durable", so the wait happens here. With one connection
    // the batch is always this one commit and the drain is one sync - the
    // batching only pays off once concurrent committers exist to fill it
    // (manager.hpp). kRelaxed waits for nothing by definition.
    if (effective_durability_ == wal::DurabilityClass::kGroup &&
        !wal_->IsDurable(commit.value())) {
        pending_commit_lsn_ = commit.value();
    }
    return Status::OK();
}

Status CommandDispatcher::LogSplit(storage::InsertPlacement& placed, std::uint64_t txn_id,
                                   catalog::Oid oid, PageId anchor_page_id) {
    std::vector<PageId> pages;
    pages.reserve(placed.changes().size());
    for (const storage::StructuralChange& change : placed.changes()) {
        pages.push_back(change.page_id);
    }
    return LogTreeSplit(pages, txn_id, oid, /*index_oid=*/0, placed.new_root, anchor_page_id,
                        &placed.anchor);
}

Status CommandDispatcher::LogTreeSplit(std::span<const PageId> pages, std::uint64_t txn_id,
                                       catalog::Oid rel_oid, std::uint64_t index_oid,
                                       PageId new_root, PageId anchor_page_id,
                                       storage::UnlatchedPin* anchor_pin) {
    // The anchor is taken after every page the split wrote and dropped
    // first. The pages it then latches beside it are ones the split created,
    // which nothing reaches but through a page already held.
    std::optional<storage::PageRef> anchor;
    Status refused = Status::OK();
    if (new_root != kInvalidPageId) {
        // A hit on the split's pinned frame; the staging's hold makes the pin
        // redundant, and dropping it keeps the images below under the ceiling.
        auto staged = catalog_.StageAnchorRoot(anchor_page_id, rel_oid, index_oid, new_root);
        if (anchor_pin != nullptr) anchor_pin->Release();
        if (staged.ok()) {
            anchor.emplace(std::move(staged.value()));
        } else {
            refused = staged.status();
        }
    }
    // A refused staging still logs the split: its pages are written either
    // way, and a split no record describes refuses the next mount
    // (`wal.md` §6). Only the root goes unpublished, and the error says so.
    if (Status s = storage::LogBtreeSplit(wal_, page_store_, txn_id, pages,
                                          anchor.has_value() ? &*anchor : nullptr);
        !s.ok()) {
        return s;
    }
    if (!refused.ok()) {
        if (logging(LogLevel::kError)) {
            log_->Error("btree", "relation oid " + std::to_string(rel_oid) + " index oid " +
                                     std::to_string(index_oid) +
                                     " grew a level but its root could not be repointed at page " +
                                     std::to_string(new_root) + ": " + refused.message());
        }
        return refused;
    }
    if (!anchor.has_value()) return Status::OK();
    anchor.reset();
    catalog_.AdoptRoot(rel_oid, index_oid, new_root);
    return Status::OK();
}

DispatchOutcome CommandDispatcher::HandleInsert(std::string_view line, Session& session) {
    // The write scope is opened before anything is parsed, so that a
    // statement inside an explicit transaction re-mints its read view at
    // the same boundary a SELECT does.
    // ErrorReply, not a bare "ERR ": a refusal here carries the wire's
    // `retryable=1` where its code is retryable, which is what a client's
    // retry loop reads.
    auto opened = BeginWrite(session);
    if (!opened.ok()) return {ErrorReply(opened.status()), false, 0, opened.status()};
    WriteScope scope = opened.value();

    DispatchOutcome out = WithReturning(line, session, scope, InsertInner(line, scope));


    // The reply is the verdict, which is the same rule Dispatch() itself
    // applies one level up. A statement that answered ERR did not happen as
    // far as the client is concerned, so an autocommit scope unwinds it.
    const bool failed = out.response.rfind("ERR ", 0) == 0;
    Status verdict = failed ? Status::InvalidArgument(out.response) : Status::OK();
    if (Status s = EndWrite(session, scope, verdict); !s.ok() && !failed) {
        return {ErrorReply(s), false, 0, s};
    }
    return out;
}

Status CommandDispatcher::CheckWriteAdmission(const catalog::TableAccess& access) {
    // **Nothing here asks about a core.** The write-affinity check and its
    // range resolution retired (AT-S9); their history is at cf889b63.
    //
    // **`CannotEnforce` stays**, with its *measured* failure
    // (`bench/v2.2.0/results-shipping-part-a-*` Finding 2, a second row in
    // a group under `CHECK COUNT(*) <= 1`): an
    // assertion the instance knows of and could not revive at this mount
    // refuses the relation's writes on every core, there being one registry
    // since AT-S5d. What reaches it is a revive that failed, a mount whose
    // snapshots do not cover the base (`server/mount_recovery.cpp`), or a
    // checkpoint no snapshot run could carry the cabin for (AZ-S3), which
    // can land mid-statement - so the admissions ask again per row.
    if (enforcer_->CannotEnforce(access.oid)) return exec::AssertionEnforcer::CannotEnforceRefusal();
    return Status::OK();
}

namespace {

// **One renderer per output shape** (R4-A/AG3). A second formatter here would be exactly the drift
// the local renderer's own warning names: a sorted reply rendering a DATE
// as an epoch day because one of two copies forgot
// `projection_types`. The fold's output row has the same trap in its
// `type_val` lookup, which is why both live here rather than one.

// **They are `TextResultSink`'s two methods now** (P08,
// `server/result_sink.hpp`). The warning above is why: KWP is a second
// output form, and writing it beside these would have been the second
// formatter they exist to prevent, four times over. So the four emission
// points below call a sink, the text sink is these two bodies moved whole,
// and the wire sink is the new one - one call site, two implementations,
// no third copy to forget a `type_val`.

// **A fold's output type is the function's, not the column's.**
//
// `AggregateItem::type_val` is the type of the column *read*, which is what
// the accumulator needs; it is not the type of the answer, and for three of
// the five functions the two differ. `Aggregator::Finish` is the authority
// and this mirrors it exactly:
//
//   COUNT   an int64. Never NULL, never the column's type - `COUNT(*)`
//           has no column, which is why the item carries `type_val` 0.
//   SUM     the decimal types keep theirs and re-attach the scale; every
//           integer column folds into an **int64** accumulator, so
//           reporting the column's narrower type would declare a width the
//           answer can exceed.
//   MIN/MAX the column's, because an extreme of a set is a member of it.
//   AVG     always a decimal - the one divide, rounded half to even at the
//           column's scale, which for an integer column is scale 0. It is a
//           `kDecimal` value even there, so declaring the column's integer
//           type would be a type the value is not.
//
// The text renderer was insensitive to all of this (`FormatValue` reads a
// decimal's scale off the value and renders an int from its digits either
// way), which is why the distinction only had to be drawn when a *typed*
// boundary appeared. It is drawn once, here, for both.
std::pair<std::uint32_t, std::uint32_t> AggregateOutputType(const exec::AggregateItem& item) {
    const bool wide = item.type_val == catalog::kTypeValDecimalWide;
    // SUM and AVG: the precision is the type's maximum rather than the
    // column's, because the fold widens and the column's precision would
    // understate the answer's range. The scale is the column's.
    const auto decimal_mod = [&](bool as_wide) {
        return catalog::PackDecimalLen(
            as_wide ? exec::kMaxDecimalPrecisionWide : exec::kMaxDecimalPrecision,
            catalog::DecimalScaleOf(item.decimal_len));
    };
    // **`func` is meaningless unless `is_aggregate`** (AG5): a grouping
    // column carried through to the output leaves the field at its default,
    // `kCount`, so reading it first reported a `date` group key as an int64
    // and rendered every group as an epoch day.
    if (item.is_aggregate) {
        switch (item.func) {
            case parser::AggFunc::kCount:
                return {catalog::kTypeValInt64, 0};
            case parser::AggFunc::kSum:
                if (wide) return {catalog::kTypeValDecimalWide, decimal_mod(true)};
                if (item.type_val == catalog::kTypeValDecimal) {
                    return {catalog::kTypeValDecimal, decimal_mod(false)};
                }
                return {catalog::kTypeValInt64, 0};
            case parser::AggFunc::kAvg:
                return wide ? std::pair{catalog::kTypeValDecimalWide, decimal_mod(true)}
                            : std::pair{catalog::kTypeValDecimal, decimal_mod(false)};
            case parser::AggFunc::kMin:
            case parser::AggFunc::kMax:
                break;  // an extreme of a set is a member of it
        }
    }
    // MIN/MAX, and a grouping column: the column's own type, its precision
    // included - the answer is one of its values, so it fits, and a SET
    // assigning it back to a column of that type needs the exact type
    // (types.md §3.2b).
    return {item.type_val, catalog::TypeModOf(item.type_val, item.decimal_len)};
}

std::vector<std::uint32_t> AggregateOutputTypes(const exec::AggregateSpec& spec) {
    std::vector<std::uint32_t> out;
    out.reserve(spec.items.size());
    for (const exec::AggregateItem& item : spec.items) {
        out.push_back(AggregateOutputType(item).first);
    }
    return out;
}

std::vector<std::uint32_t> AggregateOutputTypeMods(const exec::AggregateSpec& spec) {
    std::vector<std::uint32_t> out;
    out.reserve(spec.items.size());
    for (const exec::AggregateItem& item : spec.items) {
        out.push_back(AggregateOutputType(item).second);
    }
    return out;
}

// One result-set description from three positional lists. The `type_len`
// is derived rather than passed - it is a function of the type and of
// whether the field is a relation's column 0 (`wire::WireValueType`), so a
// caller that supplied one could only ever agree with it or be wrong.
std::vector<wire::FieldDescription> DescribeFields(
    std::span<const std::string> names, std::span<const std::uint32_t> type_vals,
    std::span<const std::uint32_t> type_mods,
    std::span<const exec::ColumnRef> projection = {}, std::uint16_t from_slot = 0) {
    std::vector<wire::FieldDescription> fields;
    fields.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        wire::FieldDescription f;
        f.name = names[i];
        f.type_oid = i < type_vals.size() ? type_vals[i] : 0;
        // Any relation's column 0 is its Keystone id and travels as u64,
        // the joined relation's included (`wire::WireValueType`).
        f.type_len = wire::WireTypeLen(
            i < projection.size() ? wire::WireValueType(f.type_oid, projection[i].col_pos)
                                  : f.type_oid);
        f.type_mod = i < type_mods.size() ? type_mods[i] : 0;
        // **`kFieldFlagKeystone` marks the relation's own column 0**, not
        // the first field of whatever was projected (`wire/row_codec.hpp`:
        // "field 0 of every user relation is the Keystone id"). So it is
        // set from the *reference*, which is the only thing that knows -
        // `SELECT v, id FROM t` flags its second field, and a fold flags
        // none, because a `COUNT(*)` is not anybody's tuple key.
        //
        // A **star** read passes a projection built from the schema, whose
        // element 0 is exactly `{0, 0, 0}`, so the two routes to one
        // `SELECT *` describe it alike. A star read that passed no
        // projection would flag nothing.
        //
        // **The FROM relation's**, at `from_slot`: the join planner may run
        // it at another slot (CB-S2), and a description that followed the
        // plan would change with the catalog.
        if (i < projection.size() && projection[i].up == 0 && projection[i].rel_slot == from_slot &&
            projection[i].col_pos == 0) {
            f.flags |= wire::kFieldFlagKeystone;
        }
        fields.push_back(std::move(f));
    }
    return fields;
}

// The three positional lists a *star* read describes from, built from the
// relation's schema. `wire::DescribeSchema` answers the same description
// directly; this exists because the two chain-rendered shapes reach
// `DescribeFields` with their own lists, and one builder taking one shape
// is what keeps the keystone rule and the type_mod rule in one place.
struct StarDescription {
    std::vector<exec::ColumnRef> projection;
    std::vector<std::uint32_t> types;
    std::vector<std::uint32_t> type_mods;
};

StarDescription DescribeStar(std::span<const catalog::SysColumnRow> columns) {
    StarDescription out;
    out.projection.reserve(columns.size());
    out.types.reserve(columns.size());
    out.type_mods.reserve(columns.size());
    for (std::size_t i = 0; i < columns.size(); ++i) {
        // A dropped column is not in `*` (BW-R6), nor a heap's hidden tuple
        // key (BZ-R3), as the compiler's `column_names` leaves them out.
        if (!columns[i].visible()) continue;
        out.projection.push_back(exec::ColumnRef{0, 0, static_cast<std::uint16_t>(i)});
        out.types.push_back(columns[i].type_val);
        out.type_mods.push_back(catalog::TypeModOf(columns[i].type_val, columns[i].len));
    }
    return out;
}

}  // namespace

// ---- BA-S15: a `SELECT` whose walk outlives its head ------------------------
//
// **Everything a plain `SELECT`'s walk reads, owned in one place**, so the
// walk can run after `HandleSelect` returns and yield its core between
// slices (BA-R12, `sched.md` §3). The head builds one for every `SELECT`
// and fills it as it parses, compiles and describes; the analyze, the fold
// and a catalog view finish inside the head and drop it there. The plain
// path hands it on in `DispatchOutcome::select_run` when the statement may
// park - `DispatchAsync`, which awaits `Execute` - and runs it inline
// otherwise.
//
// What it holds across a suspension, and why each is safe to hold: the read
// view and its lease (statement-scoped, no page); the read borrow (the
// relation `IS` a `DROP TABLE` waits for); the parse and the chain, whose
// tokens are views into the statement text `DispatchAsync`'s caller keeps
// alive; a scratch set borrowed from the dispatcher's pool; and, once handed
// on, this core's statement epoch held at the word the head revalidated
// against and a pin on the catalog memo - so neither a later head's
// revalidation nor a drop's reclaim frees what the chain points into.
class SelectRun {
public:
    SelectRun(CommandDispatcher& d, Session& session) : d_(d), session_(session) {
        slice_.cancel = session.cancel_flag().get();
        if (d_.select_scratch_pool_.empty()) {
            scratch_ = std::make_unique<CommandDispatcher::SelectScratch>();
        } else {
            scratch_ = std::move(d_.select_scratch_pool_.back());
            d_.select_scratch_pool_.pop_back();
        }
    }
    ~SelectRun() {
        if (epoch_held_) d_.epochs_->Release(d_.core_id_, epoch_word_);
        d_.select_scratch_pool_.push_back(std::move(scratch_));
    }
    SelectRun(const SelectRun&) = delete;
    SelectRun& operator=(const SelectRun&) = delete;

    CommandDispatcher::SelectScratch& scratch() noexcept { return *scratch_; }

    // Taken by the head as it hands the walk on, while it still publishes
    // the word it revalidated against.
    void HoldAcrossSuspension() {
        if (d_.epochs_ != nullptr) {
            epoch_word_ = d_.catalog_.cache_built_at();
            d_.epochs_->Hold(d_.core_id_, epoch_word_);
            epoch_held_ = true;
        }
        memo_pin_.emplace(d_.catalog_);
    }

    // The walk, yielding every `slice.pages_per_slice` pages, then the
    // sorted drain and the recording; the reply is written over `*out`.
    sched::Coro Execute(DispatchOutcome* out);

    // `Execute` driven to its end on this thread, for a caller that cannot
    // park: a yield is resumed at once.
    DispatchOutcome RunInline();

    // The head's state, filled by `HandleSelect` in order.
    std::optional<txn::LeasedSnapshot> snapshot;
    std::optional<parser::Parser> parser;
    std::optional<parser::Statement> statement;
    std::optional<ReadBorrow> borrow;
    std::optional<exec::StepChain> chain;
    TextResultSink text_sink;
    ResultSink* sink = nullptr;
    std::optional<StatementIdentity> identity;
    exec::TrailCollector* trail = nullptr;
    const exec::TrailReplay* replay = nullptr;
    std::optional<exec::EmissionQuota> quota;
    StarDescription star;
    std::span<const exec::ColumnRef> projection;
    std::span<const std::uint32_t> types;
    sched::MonoTimeNs started_ns = 0;

private:
    DispatchOutcome Finish(const Status& ran);

    CommandDispatcher& d_;
    // The statement's session, which outlives it: a connection is torn
    // down only once its statement has ended (`TcpServer::CloseClient`).
    Session& session_;
    // Released after the public members above, which point into the memo
    // and dereference nothing of it as they go.
    std::optional<catalog::Catalog::MemoPin> memo_pin_;
    std::unique_ptr<CommandDispatcher::SelectScratch> scratch_;
    std::uint64_t epoch_word_ = 0;
    bool epoch_held_ = false;
    std::string row_scratch_;
    Status encode_error_ = Status::OK();
    exec::SlicePolicy slice_;
};

void SetSelectRunStart(SelectRun& run, sched::MonoTimeNs started_ns) noexcept {
    run.started_ns = started_ns;
}

sched::Coro SelectRun::Execute(DispatchOutcome* out) {
    CommandDispatcher::SelectScratch& scratch = *scratch_;
    // Encoding one row of the reply. Shared by the two paths below so the
    // sorted and unsorted replies are formatted by one routine - the bug
    // this shape avoids is a sorted statement rendering a DATE as an epoch
    // day because a second formatter forgot `projection_types`.
    auto render = [this](const exec::ChainFrame& frame, std::string& into) {
        // A failure is remembered rather than thrown: the sort path calls
        // this from a place that has already decided to keep the row, and
        // the walk's own sink is the only caller that can end the
        // statement. Checked at both, so an encode failure fails the
        // statement rather than emitting a row the sink refused.
        Status s = sink->EncodeProjectedRow(projection, types, frame, into);
        if (!s.ok() && encode_error_.ok()) encode_error_ = std::move(s);
    };
    // Named, not a temporary: the walk holds it across every suspension.
    const exec::RowSink visit =
        [this, &scratch, render](const exec::ChainFrame& frame) -> StatusOr<storage::VisitControl> {
        if (scratch.sorter.active()) {
            // **Ask before rendering.** The sort cannot skip or stop -
            // the quota runs after the order exists - but under a
            // `LIMIT` most rows are beaten by the heap's worst retained
            // row and will never be seen, and rendering them is what
            // made top-N bound memory without bounding work.
            auto admitted = scratch.sorter.Admit(frame);
            if (!admitted.ok()) return admitted.status();
            if (admitted.value()) {
                render(frame, row_scratch_);
                if (!encode_error_.ok()) return encode_error_;
                scratch.sorter.Take(row_scratch_);
            }
            return storage::VisitControl::kContinue;
        }
        const exec::QuotaVerdict verdict = quota->Note();
        if (verdict == exec::QuotaVerdict::kStop) return storage::VisitControl::kStop;
        if (verdict == exec::QuotaVerdict::kSkip) return storage::VisitControl::kContinue;
        render(frame, row_scratch_);
        if (!encode_error_.ok()) return encode_error_;
        if (Status s = sink->Emit(row_scratch_); !s.ok()) return s;
        return verdict == exec::QuotaVerdict::kEmitThenStop ? storage::VisitControl::kStop
                                                            : storage::VisitControl::kContinue;
    };
    const Status ran = co_await exec::ExecuteAsync(
        d_.catalog_, d_.page_store_, *chain, visit, &scratch.stats, d_.budget_, trail, replay,
        d_.cabins_, &snapshot->snap, d_.indexes_enabled_, &*borrow, slice_);
    const std::uint64_t statement = out->statement;
    *out = Finish(ran);
    out->statement = statement;
    co_return Status::OK();
}

DispatchOutcome SelectRun::RunInline() {
    DispatchOutcome out;
    sched::Coro walk = Execute(&out);
    while (!walk.done()) {
        // Nothing beneath parks - the walk only yields - so a refused
        // resume is a defect, answered rather than spun on.
        if (!walk.TryResumeDeepest()) {
            const Status parked = Status::InvalidArgument(
                "a SELECT's walk parked on a wait under a synchronous dispatch");
            return {ErrorReply(parked), false, 0, parked};
        }
    }
    return out;
}

DispatchOutcome SelectRun::Finish(const Status& ran) {
    // The recording below writes catalog and Waystone pages, outside the
    // head's guard: as this dispatcher's core, as `DispatchAndStage` runs.
    const CurrentCoreGuard as_this_core(d_.core_id_);
    if (!ran.ok()) {
        // A cancelled statement leaves its transaction failed (§10's
        // "post-cancel session state = failed-txn rules"), whatever a
        // read's other refusals do.
        if (ran.code() == StatusCode::kCancelled) session_.Poison();
        // **No trail on the failure path.** A statement that errored part
        // way through touched some tuples and then stopped; a trail
        // describing that is a trail describing a state no reader should
        // ever be pointed at (workplan P10).
        return {ErrorReply(ran), false, 0, ran};
    }
    CommandDispatcher::SelectScratch& scratch = *scratch_;

    // The order exists only now, so the quota is applied here rather than
    // in the sink - and it is the same quota object, so `LIMIT n OFFSET m`
    // still means rows [m, m+n) of the reply the unlimited statement gives.
    // What changed is which reply that is: the sorted one.
    if (scratch.sorter.active()) {
        scratch.sorter.Finish();
        Status drained = Status::OK();
        exec::DrainSorted(*quota, scratch.sorter.rows(), [&](const exec::OutputSort::Row& row) {
            if (!drained.ok()) return;
            drained = sink->Emit(row.text);
        });
        if (!drained.ok()) return {ErrorReply(drained), false, 0, drained};
    }

    d_.RecordExecution(identity, trail, *chain, scratch.stats);

    if (d_.logging(LogLevel::kTrace)) {
        d_.log_->Trace("query", "chain of " + std::to_string(chain->steps.size()) +
                                    " step(s), class " +
                                    std::to_string(static_cast<int>(chain->klass)));
    }
    return {text_sink.Take(), false};
}

sched::Coro CommandDispatcher::CompleteSelect(std::string_view line, DispatchOutcome* out) {
    // Owned by this frame for the walk's life: `*out` is overwritten by the
    // reply, and a core torn down with the walk queued destroys this frame -
    // and the walk's beneath it - before the run they read
    // (`CoreRuntime`'s `DiscardTasks`). A connection closed mid-walk is not
    // torn down until the statement ends (`TcpServer::CloseClient`).
    const std::shared_ptr<SelectRun> run = std::move(out->select_run);
    co_await run->Execute(out);
    LogStatement(line, *out, run->started_ns);
    co_return Status::OK();
}

StatusOr<parser::Statement> CommandDispatcher::ParseKeepingShape(std::string_view line) {
    // `parser::Parse`, keeping the fingerprint the lexer took on the way:
    // `sys.statement_stats`' key costs the statement no second lex (BO-S5).
    parser::Parser parser(line);
    auto parsed = parser.Parse();
    // The statement's own parse comes first; a SET value's subquery, run
    // through `HandleSelect` inside the UPDATE, must not re-key it.
    if (const auto fingerprint = parser.fingerprint();
        fingerprint.has_value() && statement_fetch_id_ == 0) {
        statement_fetch_id_ = fingerprint->fetch_id;
    }
    return parsed;
}

DispatchOutcome CommandDispatcher::InsertInner(std::string_view line, WriteScope& scope) {
    auto parsed = ParseKeepingShape(line);
    if (!parsed.ok()) {
        return {ErrorReply(parsed.status()), false, 0, parsed.status()};
    }
    if (!std::holds_alternative<parser::InsertStmt>(parsed.value())) {
        return {"ERR expected an INSERT statement", false};
    }
    return InsertParsed(std::get<parser::InsertStmt>(parsed.value()), scope);
}

std::optional<DispatchOutcome> CommandDispatcher::ArchiveRefusal() const {
    // Archiving is failing and the WAL filesystem is near full: a write is
    // refused by name rather than let the log fill the volume or recycle
    // past the archive.
    if (archiver_ == nullptr || !archiver_->blocking()) return std::nullopt;
    const Status s = Status::ResourceExhausted(
        "the WAL archive is failing (" + archiver_->last_error() +
        ") and the WAL filesystem is near full; statements that write the log are refused "
        "until archiving recovers (byte 0)");
    return DispatchOutcome{ErrorReply(s), false, 0, s};
}

StatusOr<catalog::Oid> CommandDispatcher::ResolveRelationName(Session& session,
                                                              std::string_view qualifier,
                                                              std::string_view name) {
    return ResolveNames(session, [&](const txn::ReadView* v) {
        return catalog_.ResolveRelation(qualifier, name, session.default_namespace(), 0, v);
    });
}

DispatchOutcome CommandDispatcher::ExecuteInsert(const parser::InsertStmt& stmt,
                                                 Session& session) {
    // The KWP load stream comes in here, past `DispatchInner`'s gate.
    if (auto refused = ArchiveRefusal(); refused.has_value()) return *refused;
    // HandleInsert's body around the parsed half: same scope, same verdict
    // rule, so a load chunk and a textual statement are indistinguishable
    // from the write pipeline's side (KW5, BI2).
    auto opened = BeginWrite(session);
    if (!opened.ok()) return {ErrorReply(opened.status()), false, 0, opened.status()};
    WriteScope scope = opened.value();

    DispatchOutcome out = InsertParsed(stmt, scope);

    const bool failed = out.response.rfind("ERR ", 0) == 0;
    Status verdict = failed ? Status::InvalidArgument(out.response) : Status::OK();
    if (Status s = EndWrite(session, scope, verdict); !s.ok() && !failed) {
        return {ErrorReply(s), false, 0, s};
    }
    return out;
}

// **`INSERT ... SELECT`'s rows** (BU-S8): the source run by the query
// layer at the statement's snapshot - taken after `BeginWrite`, as an
// UPDATE takes its own - and read whole before any row is placed, so the
// statement never reads a row it wrote. Each column is assignable to its
// target as a SET value is (BJ-R3): one type, two text types meeting, a
// decimal at the target's scale.
StatusOr<parser::InsertStmt> CommandDispatcher::InsertSelectRows(const catalog::TableAccess& ta,
                                                                 const parser::InsertStmt& written,
                                                                 WriteScope& scope) {
    if (scope.session == nullptr) {
        return Status::NotImplemented("INSERT ... SELECT needs a session (byte " +
                                      std::to_string(written.select_byte_offset) + ")");
    }
    auto snapshot = SnapshotFor(*scope.session);
    if (!snapshot.ok()) return snapshot.status();
    auto rs = RunLayerQuery(*written.select, written.select_text, *scope.session,
                            snapshot.value().snap, StatementContextFor(*scope.session));
    if (!rs.ok()) return rs.status();
    const std::vector<exec::LayerColumn>& from = rs.value().columns;
    auto targets = InsertTargetColumns(ta, written, from.size());
    if (!targets.ok()) return targets.status();
    if (targets.value().empty()) {
        return Status::InvalidArgument(
            "the SELECT returns " + std::to_string(from.size()) + " column(s), which name no " +
            (written.columns.empty() ? "row of the relation" : "row of the column list") +
            " (byte " + std::to_string(written.select_byte_offset) + ")");
    }
    for (std::size_t i = 0; i < from.size(); ++i) {
        const exec::SetType& got = from[i].type;
        const catalog::SysColumnRow& want = *targets.value()[i];
        const bool text = exec::TypeFamilyOf(got.type_val) == exec::TypeFamily::kText &&
                          exec::TypeFamilyOf(want.type_val) == exec::TypeFamily::kText;
        // `len` is a type only for a decimal (its scale, here) - for every
        // other type it is a stored width, and a settled `int64` literal
        // carries 0 where an `int64` column carries its width.
        const bool decimal = want.type_val == catalog::kTypeValDecimal ||
                             want.type_val == catalog::kTypeValDecimalWide;
        const bool same = got.type_val == want.type_val &&
                          (!decimal || catalog::DecimalScaleOf(got.len) ==
                                           catalog::DecimalScaleOf(want.len));
        if (!text && !same) {
            return Status::InvalidArgument(
                "the SELECT's column " + std::to_string(i + 1) + " is " + exec::SetTypeName(got) +
                " but column '" + std::string(catalog::NameView(want.name)) + "' is " +
                exec::SetTypeName(exec::SetType{exec::SetType::Kind::kConcrete, want.type_val,
                                                want.len}) +
                " (byte " + std::to_string(written.select_byte_offset) +
                "); there is no implicit conversion (BJ-R3), CAST it");
        }
        // A decimal is matched by scale alone, so its precision is the row's
        // check: the encoder stores any unscaled value of the right scale,
        // and a decimal(12,2) value must not land in a decimal(10,2) column.
        if (!decimal) continue;
        const exec::SetType column{exec::SetType::Kind::kConcrete, want.type_val, want.len};
        for (std::size_t r = 0; r < rs.value().rows.size(); ++r) {
            if (!exec::ValueInRange(rs.value().rows[r][i], column)) {
                return Status::OutOfRange(
                    "row " + std::to_string(r + 1) + "'s value for column '" +
                    std::string(catalog::NameView(want.name)) + "' does not fit " +
                    exec::SetTypeName(column) + " (byte " +
                    std::to_string(written.select_byte_offset) + ")");
            }
        }
    }
    parser::InsertStmt out = InsertShellOf(written);
    out.columns = written.columns;
    out.columns_byte_offset = written.columns_byte_offset;
    out.rows = std::move(rs.value().rows);
    return out;
}

DispatchOutcome CommandDispatcher::InsertParsed(const parser::InsertStmt& written,
                                                WriteScope& scope) {
    // BI3: the row cap, at the first config-aware layer (the parser is a
    // pure syntax layer and deliberately config-blind). A refusal naming
    // the cap and the count, never a truncation.
    if (written.rows.size() > max_insert_rows_) {
        return {"ERR INSERT of " + std::to_string(written.rows.size()) +
                    " rows exceeds max_insert_rows (" + std::to_string(max_insert_rows_) + ")",
                false};
    }

    // BI4's atomicity is the transaction scope's: rows placed before a
    // failure are unwound by rollback, and rollback replays the manager's
    // in-memory trail. A configuration without one (tests; production
    // always builds it) could place rows it cannot take back, which is a
    // wrong answer with a right answer's shape - refused upfront instead.
    if (written.rows.size() > 1 && txn_ == nullptr) {
        return {"ERR a multi-row INSERT requires the transaction manager; this configuration "
                "cannot unwind a partially placed statement and is single-row only",
                false};
    }

    // Resolved under the writing session's view (DT3c): a write to a
    // relation another transaction created and has not committed must not
    // find it.
    auto oid = ResolveNames(scope.session, [&](const txn::ReadView* v) {
        // An INSERT is never parked, so the session's namespace is the
        // statement's; `public` with no session (NS5).
        return catalog_.ResolveRelation(
            written.schema, written.table_name,
            scope.session != nullptr ? scope.session->default_namespace()
                                     : catalog::kNamespacePublic,
            written.table_byte_offset, v);
    });
    if (!oid.ok()) {
        return {ErrorReply(oid.status()), false, 0, oid.status()};
    }

    ReadBorrow borrow(locks_, NextReadHolder(), &read_borrows_, oid.value());  // AT-R1

    auto access = catalog_.InitTableAccess(oid.value());
    if (!access.ok()) {
        return {ErrorReply(access.status()), false, 0, access.status()};
    }
    // Borrowed from the catalog's cache, not owned: valid for this
    // statement, including across AllocateRowId() (catalog.hpp), and
    // refreshed by InsertOneRow when a root repoint invalidates it.
    const catalog::TableAccess* ta = access.value();

    // Insert routing and spreading retired (AT-S9); their history is at
    // cf889b63. A row lands in its relation's one structure on the core the
    // session is on.

    // Before anything is written, once per statement - every row goes to
    // the one relation.
    if (Status admitted = CheckWriteAdmission(*ta); !admitted.ok()) {
        // ErrorReply, not a bare "ERR ": what the gate refuses
        // (`CannotEnforce`) must reach the wire with its code.
        return {ErrorReply(admitted), false, 0, admitted};
    }
    // ---- The relation's intention, before anything is admitted (AT-S5e,
    // AT-0 item 13) ---------------------------------------------------------
    //
    // **Here and not at the first id borrow.** A DDL that claims the relation - a `CREATE INDEX` about to
    // backfill, a `CREATE ASSERTION` about to build a cabin - takes its `X`
    // against this `IX`, and it is a fence only if it stands over everything
    // the statement decides from the relation's secondary structures:
    // assertion admission, not just the placement. Taken after them, a writer could be admitted
    // against no assertion, let a build finish and publish, and then place
    // a row the cabin never counts. Held for the transaction, so every row
    // of the statement is covered by this one ask.
    if (std::optional<Status> held = BorrowOrWait(scope, txn::LockKey::Relation(ta->oid),
                                                  RepeatableReadWait::kCapable)) {
        return {ErrorReply(*held), false, 0, *held};
    }

    // **Positional `VALUES` name the live columns** (BW-R6, BW-Q9 of
    // `instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`), and a
    // column list names its own (BW-S6): a column-listed statement, a
    // relation whose column set changed, or a row whose tuple key is `NULL`,
    // takes its rows normalized here, once (`NormalizeInsertRows`), so
    // everything downstream keeps indexing the body by `column_no - 1`.
    // **After the `IX`, never before it**: a `DROP COLUMN`'s overwrite is in
    // place and visible before its decide, so a row counted against an open
    // drop's schema would be refused on a column set that may yet roll back;
    // behind the `IX` the statement parks on that drop and re-resolves.
    //
    // **A KWP load's rows are positional against the shape it described**
    // (BW-Q10): a chunk re-resolves the relation by name, so it is refused
    // unless that is still the same relation with the same column set - a
    // drop and an add of equal net width would otherwise land its values in
    // other columns. Asked behind the `IX`, where no column DDL can move it.
    if (written.described.oid != 0 &&
        (written.described.oid != ta->oid ||
         written.described.physical_columns != ta->schema.columns.size() ||
         written.described.live_columns != ta->layout.live_columns)) {
        const Status s = Status::InvalidArgument(
            "relation '" + written.table_name +
            "' is not the relation this load described at its BEGIN: it was dropped or its "
            "columns were changed by ALTER TABLE since; begin a new load");
        return {ErrorReply(s), false, 0, s};
    }
    std::optional<parser::InsertStmt> evaluated;
    if (!written.value_exprs.empty()) {
        auto e = EvaluateInsertExpressions(*ta, written);
        if (!e.ok()) return {ErrorReply(e.status()), false, 0, e.status()};
        evaluated.emplace(std::move(e.value()));
    }
    if (written.select != nullptr) {
        auto e = InsertSelectRows(*ta, written, scope);
        if (!e.ok()) return {ErrorReply(e.status()), false, 0, e.status()};
        evaluated.emplace(std::move(e.value()));
        if (evaluated->rows.size() > max_insert_rows_) {
            return {"ERR INSERT of " + std::to_string(evaluated->rows.size()) +
                        " rows exceeds max_insert_rows (" + std::to_string(max_insert_rows_) + ")",
                    false};
        }
        if (evaluated->rows.size() > 1 && txn_ == nullptr) {
            return {"ERR a multi-row INSERT requires the transaction manager; this configuration "
                    "cannot unwind a partially placed statement and is single-row only",
                    false};
        }
        if (evaluated->rows.empty()) {
            return {"INSERTED oid=" + std::to_string(oid.value()) + " rows=0", false, 0};
        }
    }
    const parser::InsertStmt& valued = evaluated.has_value() ? *evaluated : written;
    auto normalized = NormalizeInsertRows(*ta, valued);
    if (!normalized.ok()) return {ErrorReply(normalized.status()), false, 0, normalized.status()};
    const parser::InsertStmt& shaped =
        normalized.value().has_value() ? *normalized.value() : valued;

    // ---- ON CONFLICT DO NOTHING (BU-Q18) --------------------------------
    //
    // Decided for every row before any is admitted, so a skipped row takes
    // no foreign-key hold, no assertion reservation and no id: a row naming
    // a placed tuple key - live or delete-marked (BD-R5), or named by an
    // earlier row of this statement - is dropped, and the key's row unit,
    // taken as the placement would take it (BD-Q9's wait included), is kept
    // for a row that stays, so no other writer can place it in between.
    std::optional<parser::InsertStmt> kept;
    if (written.on_conflict_do_nothing) {
        if (written.conflict_target.has_value()) {
            auto col = ta->schema.FindColumn(written.conflict_target->name,
                                             written.conflict_target->byte_offset);
            if (!col.ok()) return {ErrorReply(col.status()), false, 0, col.status()};
            if (col.value() != &ta->schema.columns.front()) {
                const Status s = Status::NotImplemented(
                    "ON CONFLICT (" + written.conflict_target->name + ") at byte " +
                    std::to_string(written.conflict_target->byte_offset) +
                    (ta->schema.heap()
                         ? ": the tuple key is the one conflict there is (BU-Q18), and a "
                           "heap's is hidden (BZ-R4); name no column"
                         : ": the tuple key is the one conflict there is (BU-Q18); name it or "
                           "no column"));
                return {ErrorReply(s), false, 0, s};
            }
        }
        kept.emplace(InsertShellOf(shaped));
        std::set<std::uint64_t> named;
        for (const std::vector<parser::AstValue>& r : shaped.rows) {
            const std::optional<std::uint64_t> id =
                r.size() == ta->schema.columns.size() ? KeystoneIdOf(r.front()) : std::nullopt;
            if (id.has_value()) {
                if (named.count(*id) != 0) continue;
                auto placed = TupleKeyPlaced(*ta, *id, r.front().byte_offset, scope);
                if (!placed.ok()) {
                    return {ErrorReply(placed.status()), false, 0, placed.status()};
                }
                if (placed.value()) continue;
                named.insert(*id);
            }
            kept->rows.push_back(r);
        }
        if (kept->rows.empty()) {
            return {"INSERTED oid=" + std::to_string(oid.value()) + " rows=0", false, 0};
        }
    }
    const parser::InsertStmt& stmt = kept.has_value() ? *kept : shaped;

    // ---- The bulk loop (docs/spec/bulkinsert.md §2.3, §4) ---------------
    //
    // One write scope, one resolution, one affinity check - and then the
    // full single-row pipeline per row, in the same order (BI2). Admission
    // at row k sees rows 1..k-1's reservations, which is why validate-all-
    // then-place-all is forbidden: a statement must fail on its own third
    // row when the third row is the one that breaks a group bound. Any
    // failure fails the whole statement (BI4) - the ordinal is appended
    // rather than prefixed so the wire's leading compatibility tokens
    // (TXN_CONFLICT, FK_VIOLATION, ASSERTION_VIOLATION) never move.
    const bool bulk = stmt.rows.size() > 1;

    // ---- The extraction pass (foreign-keys.md §2a, AH-R1) ---------------
    //
    // Over **every** row, before the loop enters `InsertOneRow` for any of
    // them. That is the whole hoist on this path: a thousand-row insert
    // against one parent descends once rather than a thousand times.
    exec::FkParentVerdicts fk_held;
    if (!ta->fkeys_out.empty()) {
        for (const auto& values : stmt.rows) {
            const std::vector<parser::AstValue> body = InsertBodyOf(*ta, values);
            if (Status s = ResolveForeignKeyParents(*ta, body, fk_held, scope); !s.ok()) {
                return {ErrorReply(s), false, 0, s};
            }
        }
    }

    InsertRowResult first{};
    InsertRowResult last{};
    for (std::size_t k = 0; k < stmt.rows.size(); ++k) {
        InsertRowResult row{};
        // The refusal travels as its `Status`, so the code it carries - an
        // `AlreadyExists` or an `OutOfRange` for a named key (BD-R5) - reaches
        // the wire rather than being re-read off the rendered line.
        if (std::optional<Status> err =
                InsertOneRow(oid.value(), ta, stmt.rows[k], scope, fk_held, row)) {
            // The row's ordinal rides in the carried message too, so a KWP
            // client - which reads the `Status`, not the line - is told which
            // row (BB-S3's review).
            const Status carried =
                bulk ? Status::FromWire(static_cast<std::uint32_t>(err->code()),
                                        err->message() + " (row " + std::to_string(k + 1) + ")")
                     : *err;
            return {ErrorReply(carried), false, 0, carried};
        }
        if (k == 0) first = row;
        last = row;
    }

    if (bulk || written.on_conflict_do_nothing) {
        // rows= is BI13's rows_affected; the id range is what a loader
        // wants back, and with per-row allocation it is contiguous exactly
        // when nothing else allocated concurrently - so both ends are
        // reported and no contiguity is promised.
        return {"INSERTED oid=" + std::to_string(oid.value()) +
                    " rows=" + std::to_string(stmt.rows.size()) +
                    " first_id=" + std::to_string(first.id) +
                    " last_id=" + std::to_string(last.id),
                false, stmt.rows.size()};
    }
    // The single-row reply, byte-identical to what it always was.
    return {"INSERTED oid=" + std::to_string(oid.value()) + " id=" + std::to_string(last.id) +
                " page=" + std::to_string(last.page_id) + " slot=" + std::to_string(last.slot),
            false, 1};
}

namespace {

// **Whether a version an in-flight writer holds is that writer's undecided
// insert** (BD-R5, BD-Q9 (a), `instructions/v3.0.0/workorder-bd-sorted-leaf-
// named-keys.md`): its version chain, walked while each earlier version's
// writer is still in flight, ends at no version at all - the key did not
// exist before that transaction. A chain that reaches a decided writer is a
// committed row under an undecided update or delete; an unreadable step is
// answered the same.
bool UndecidedInsert(txn::TransactionManager& txn, std::uint64_t undo_ptr) {
    std::uint64_t ptr = undo_ptr;
    for (std::uint32_t steps = 0; steps < txn::kMaxUndoChainLength; ++steps) {
        if (ptr == txn::kNoUndoPtr) return true;  // no version before: an insert
        auto version = txn.undo().Read(ptr);
        if (!version.ok() || !txn.IsInFlight(version.value().prior_trx_id)) return false;
        ptr = version.value().prior_undo_ptr;
    }
    return false;
}

// How many times an omitted tuple key is issued again when the id it drew was
// placed meanwhile by a named key (BD-R6's one collision: a named key at
// the mark borrows before an issue draws the same id, then places it).
// Each round burns one id; two rounds at once is already a race against a
// client naming the mark itself, so a small bound is a guard, not a budget.
constexpr int kMaxIssueRounds = 8;

// How many times a unique check asks again when the writer it found busy
// had decided by the time the wait would be recorded (BX). Each round needs
// a decide in its gap, so a small bound is a guard, not a budget; the last
// round's busy verdict is answered retryably.
constexpr int kUniqueCheckRounds = 3;

}  // namespace

Status CommandDispatcher::NamedKeyVerdict(const catalog::TableAccess& ta, std::uint64_t id,
                                          std::uint32_t byte, const txn::Transaction* own) {
    auto present = btree::BtreeLookup(page_store_, ta.desc_page_id, id);
    if (!present.ok()) {
        // Answered by the placement's own descent, so a structural refusal
        // here is not the statement's (BA-S14).
        storage::ClearStructuralRefusal();
        return Status::OK();
    }
    auto version = heap::PageView(present.value().leaf.bytes()).ReadTuple(present.value().slot);
    if (!version.ok()) return Status::OK();
    const Status found = btree::DuplicateKey(id, present.value().page_id, present.value().slot,
                                             version.value().deleted);
    const Status duplicate =
        Status::AlreadyExists(found.message() + " (byte " + std::to_string(byte) + ")");
    if (own != nullptr && version.value().trx_id == own->id()) return duplicate;
    const bool undecided = txn_ != nullptr && txn_->IsInFlight(version.value().trx_id);
    const std::uint64_t undo_ptr = version.value().undo_ptr;
    present.value().leaf.Release();  // before the undo pages are read
    if (!undecided) return duplicate;
    return UndecidedInsert(*txn_, undo_ptr) ? Status::OK() : duplicate;
}

StatusOr<bool> CommandDispatcher::TupleKeyPlaced(const catalog::TableAccess& ta, std::uint64_t id,
                                                 std::uint32_t byte, WriteScope& scope) {
    const txn::LockKey unit = txn::LockKey::Tuple(ta.oid, id);
    const bool held_before = scope.txn != nullptr && scope.txn->borrows().Holds(unit);
    if (std::optional<Status> held =
            BorrowOrWait(scope, unit, RepeatableReadWait::kCapable, txn::LockMode::kExclusive,
                         [&]() { return NamedKeyVerdict(ta, id, byte, scope.txn); })) {
        if (held->code() == StatusCode::kAlreadyExists) return true;
        return *held;
    }
    // Granted: with a lock table no other writer holds the key, so a slot
    // keyed `id` is decided or this transaction's own - placed either way.
    // The same verdict as the refusal's, not "any slot is placed": without a
    // table every borrow is granted, and another's undecided insert must
    // reach the placement (which answers it as a plain INSERT is answered),
    // never be skipped as though its rollback could not free the key.
    if (NamedKeyVerdict(ta, id, byte, scope.txn).ok()) return false;
    if (!held_before && locks_ != nullptr && scope.txn != nullptr) {
        locks_->ReleaseIf(scope.txn->id(), scope.txn->borrows(),
                          [&](const txn::LockHoldings::Held& h) { return h.key == unit; });
    }
    return true;
}

std::optional<Status> CommandDispatcher::InsertOneRow(
    catalog::Oid oid, const catalog::TableAccess*& ta_ptr,
    const std::vector<parser::AstValue>& values, WriteScope& scope,
    const exec::FkParentVerdicts& fk_held, InsertRowResult& out) {
    const catalog::TableAccess& ta = *ta_ptr;

    // ---- How a refusal below reaches the wire -----------------------------
    //
    // Every failure below is returned as its `Status` and rendered once, by
    // the caller, through `ErrorReply` - the one spelling that puts
    // `retryable=1` on the wire for a TxnConflict (status.hpp's
    // IsRetryable) - with the `Status` itself on the outcome, so a client
    // reads the code a refusal carries rather than the line (BD-R5's
    // `AlreadyExists` and `OutOfRange` among them). A lock wait's fault net
    // is what refuses that way.

    // ---- Arity, and where the tuple key comes from -----------------------------
    //
    // **Two arities, and the row picks** (docs/spec/heap-and-tuple.md section
    // 4.1); there is no key mode:
    //
    //   ncols     - the caller names the key. values[0] is the tuple key, and the
    //               rest are the columns after it.
    //   ncols - 1 - the caller omits it. The engine issues one from the
    //               relation's cursor, and every value is a body column.
    //
    // The two cannot be confused: no body column may be omitted, so a row's
    // length names one reading. A column-listed row arrives here already in
    // this shape - `NormalizeListedInsertRows` reshaped it, a left-out
    // nullable column as a NULL placeholder (BW-S6).
    const std::size_t ncols = ta.schema.columns.size();
    const bool explicit_key = ncols > 0 && values.size() == ncols;

    if (ncols > 0 && values.size() != ncols && values.size() != ncols - 1) {
        // Before the id: the codec would refuse this row at encode, which
        // sits after the id is settled - and BI9's rule is that a refused row
        // burns nothing.
        return InsertArityRefusal(ta, ncols, values);
    }

    // When the row names its key, the tuple key is values[0] and the body is the
    // rest. Split here, once, so everything downstream - the FK check,
    // assertion admission, EncodeRow, the Cabin witness, index maintenance -
    // keeps receiving exactly the shape it already expects: the columns after
    // the key. The copy is paid only by a row that names its key.
    std::vector<parser::AstValue> body_storage;
    std::uint64_t supplied_id = 0;
    if (explicit_key) {
        const parser::AstValue& key = values.front();
        if (key.type != parser::ValueType::kInt) {
            return Status::InvalidArgument(
                "tuple key column '" + std::string(catalog::NameView(ta.schema.columns.front().name)) +
                "' needs an integer literal (byte " + std::to_string(key.byte_offset) + ")");
        }
        // **Exhausted** (BD-R5 2., `instructions/v3.0.0/workorder-bd-sorted-
        // leaf-named-keys.md`): a key that is no Keystone id is one of the two
        // reasons a named key is refused, `OutOfRange` with its byte - zero, a
        // negative number, one above `kMaxKeystoneId`, or a literal the lexer
        // wrapped past 64 bits (`KeystoneIdOf`, the foreign key's test too).
        // Before the foreign-key and assertion steps, either of which can
        // wait.
        const std::optional<std::uint64_t> id = KeystoneIdOf(key);
        if (!id.has_value()) {
            return Status::OutOfRange(
                "tuple key " + exec::FormatValue(0, key) + " is outside the Keystone id space [" +
                std::to_string(catalog::kFirstRowId) + ", " + std::to_string(kMaxKeystoneId) +
                "] (byte " + std::to_string(key.byte_offset) + ")");
        }
        supplied_id = *id;
        body_storage.assign(values.begin() + 1, values.end());
    }
    const std::vector<parser::AstValue>& body = explicit_key ? body_storage : values;

    // ---- The forward check (docs/spec/foreign-keys.md §2) ---------------
    //
    // **Before the id is allocated**, which is a stronger form of §2's
    // "before the heap write": a refused row costs no undo work *and* no
    // Keystone id (BI9). VALUES supplies the columns after the tuple key, so a
    // column at schema position c is at index c-1.
    //
    // **The resolution is the caller's** (§2a, AH-R1): `HandleInsert` runs
    // the extraction pass over every row before this function is entered
    // for any of them, so a bulk statement naming one parent descends once.
    // What is left here is the per-row *answer*, which is what keeps the
    // refusal on the row that caused it.
    if (!ta.fkeys_out.empty()) {
        for (const catalog::ForeignKeyRef& fk : ta.fkeys_out) {
            if (fk.column_no == 0 || fk.column_no > body.size()) continue;
            if (Status s = CheckForeignKeyOnWrite(ta, fk, body[fk.column_no - 1], fk_held, scope);
                !s.ok()) {
                return s;
            }
        }
    }

    // ---- The Transition check (BV-R3) ----------------------------------
    //
    // Before the id, for the forward check's reason: a refused row costs
    // no undo work and no Keystone id. Every governed column's value must
    // be an initial state.
    if (!ta.transitions.empty()) {
        if (Status s = exec::CheckTransitionInsert(ta, body, supplied_id); !s.ok()) return s;
    }

    // ---- The unique check (BX-R4) --------------------------------------
    //
    // Before the id, for the forward check's reason, and before anything is
    // placed, so a refused row is a row nothing touched - and holding no
    // page, since the check descends the clustered tree. The key units stay
    // held until this function returns: the row's entries are appended by
    // then, or nothing of it is written (BX-Q2 (a)).
    UniqueKeyHolds unique_holds(locks_, scope.txn);
    if (!ta.indexes.empty()) {
        const UniqueKeysOfRow unique_keys =
            UniqueKeysOf(ta, body, /*first_col_pos=*/1, /*previous=*/{});
        if (std::optional<Status> refused =
                CheckUniqueKeys(scope, ta, unique_keys, /*self=*/0, unique_holds)) {
            return *refused;
        }
    }

    // ---- The admission check (docs/spec/assertion.md §6.2 step 2) -------
    //
    // Before the id for FK's reason: a refused row burns nothing. The
    // reservation (step 3) happens after placement, when the entry has a
    // tuple key and a location to carry - and in a bulk statement this row's
    // admission sees every earlier row's reservation, which is the
    // intra-statement accumulation BI2 exists to keep.
    //
    // **The admitted contribution is held until then** (AT-S5d). The
    // registry is the instance's, and the page work between here and the
    // reservation gives another core's writer time to be admitted against
    // the same group; a pure check here is what let two cores both admit at
    // count 0. `admitted` gives its hold back on every exit that does not
    // reach the reservation - a refusal below, a failed placement, a borrow
    // that parks and re-runs this row.
    //
    // **A rejection a reservation caused is a wait** (AO-S6e-c, census row
    // 11): the registry names the delta's owner (`ReserverOnLocked`), so the
    // statement waits for its decide and asks again - an abort admits the
    // re-run, a commit makes the refusal true. Two transactions each holding
    // a reservation the other needs is a real cycle, refused by the
    // wait-for graph (AO-S4a).
    //
    // `kCapable`: an admission reads the **live** aggregate, never the
    // waiter's view. The tuple key is 0 because no id is issued yet, and 0 is no
    // row's key (`kFirstRowId`).
    std::uint64_t reserver = 0;
    exec::AssertionEnforcer::Hold admitted;
    if (Status s = enforcer_->AdmitInsert(oid, body, WriterId(scope), admitted, &reserver);
        !s.ok()) {
        if (reserver != 0) {
            NoteBlockingWriter(scope.txn, reserver, /*tuple_key=*/0, RepeatableReadWait::kCapable);
        }
        return s;
    }

    // ---- The id, and where the row lands ---------------------------------
    //
    // **A btree places a row where its key sorts** (BD-R2, BD-R6,
    // `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`), so its
    // id is fixed before the descent and no page hold covers the issue or
    // the admission: BB-R1, which fixed the id under the hold of the page it
    // lands on so that placement order was issue order, is withdrawn there
    // (W10) - its one reason was a leaf's key order, which placement keeps.
    // An omitted tuple key: issue, borrow, encode, descend, place. A named key:
    // borrow, encode, admit (advance-or-nothing), descend, the duplicate
    // check, place. A heap is a btree (BZ-R1), so this is every relation's.
    //
    // Both sources sit after admission and the foreign-key check, so a row
    // they refuse burns nothing. **The borrow always precedes the encode**
    // (BB §1.4): a recorded wait survives only while the statement's trail is
    // unchanged, and a spill is a trail entry.
    std::uint64_t row_id = 0;
    std::vector<std::byte> encoded;
    // With a manager, each spill is noted and logged at its append, under
    // its page's hold (`SpillLogFor`); `spills` then stays empty.
    std::vector<exec::AppendedSpill> spills;
    const auto encode = [&]() -> Status {
        const auto log_spill = SpillLogFor(scope, oid, row_id);
        auto row = exec::EncodeRow(
            ta.schema, ta.layout, row_id, body,
            exec::VarHeapSink{&page_store_, ta.varheap_page_id, &spills, ta.oid,
                              log_spill ? &log_spill : nullptr});
        if (!row.ok()) return row.status();
        encoded = std::move(row.value());
        return Status::OK();
    };
    // **A duplicate carries the tuple key token's byte** (BD-R5), as the exhausted
    // refusal above does.
    const auto duplicate_at_tuple_key = [&](const Status& duplicate) {
        return Status::AlreadyExists(duplicate.message() + " (byte " +
                                     std::to_string(values.front().byte_offset) + ")");
    };
    // The key's borrow, given back by a statement it protected nothing for
    // (BB-S3's review; AZ-S5's shape for a failed check's `S`): nothing was
    // written under it, and kept to the decide it would stall another insert
    // of the same id, waiting on a holder that can no longer place it.
    const auto give_back = [&](const txn::LockKey& unit, bool held_before) {
        if (held_before || locks_ == nullptr || scope.txn == nullptr) return;
        locks_->ReleaseIf(scope.txn->id(), scope.txn->borrows(),
                          [&](const txn::LockHoldings::Held& h) { return h.key == unit; });
    };

    // ---- The no-refuse window (BE-Q11) -----------------------------------
    //
    // From here the row writes pages - its spills, its leaf, the indexes,
    // the reservation, the undo record, a root's re-publish - and fetches
    // or creates more after each write. A full pool may refuse only here,
    // before the first of them: past this line a refusal would leave a
    // placed row with no undo record (`a-fetch-refused-after-a-page-write-
    // leaves-the-mutation-half-done.md`, closed; at eeb88f47 under
    // docs/inflight/bugs/). Closed when
    // this function returns, with the row's trail entry written.
    auto window = storage::NoRefuseWindow::Open(page_store_);
    if (!window.ok()) return window.status();

    std::optional<StatusOr<storage::InsertPlacement>> attempt;
    if (explicit_key) {
        const txn::LockKey key_unit = txn::LockKey::Tuple(ta.oid, supplied_id);
        // **A refused borrow is judged once, and only then** (AO-S6c-c's
        // rule: a key that can never be admitted never waits), so a granted
        // borrow pays nothing for it.
        //
        // A btree (BD-R5): a slot keyed `k` whose latest writer has decided
        // is a duplicate at once. Where that writer is in flight, an
        // undecided **insert** is not yet a duplicate - its rollback frees
        // `k` (W12) - so the statement waits for its decide and the re-run
        // answers by the outcome (BD-Q9 (a)); a committed row under an
        // undecided update or delete is a duplicate at once. Absent, or a
        // lookup that failed, is a wait like any other (a fence over `k`).
        // No mark is consulted (W2 of BZ).
        const auto refuse_if_never_admissible = [&]() -> Status {
            return NamedKeyVerdict(ta, supplied_id, values.front().byte_offset, scope.txn);
        };
        const bool held_before = scope.txn != nullptr && scope.txn->borrows().Holds(key_unit);
        if (std::optional<Status> held =
                BorrowOrWait(scope, key_unit, RepeatableReadWait::kCapable,
                             txn::LockMode::kExclusive, refuse_if_never_admissible)) {
            return *held;
        }
        row_id = supplied_id;
        if (Status s = encode(); !s.ok()) {
            give_back(key_unit, held_before);
            return s;
        }
        if (after_row_id_fixed_for_test_) after_row_id_fixed_for_test_(row_id);
        // **Admitting before the descent is safe** (BD-R6): every placed id
        // is below the mark, so a key at or above it cannot be a duplicate,
        // and two statements naming one key serialise on its row unit. The
        // admission never refuses; it moves the mark past `k` or leaves it
        // where it is.
        if (Status s = catalog_.AdmitExplicitRowId(oid, supplied_id); !s.ok()) {
            give_back(key_unit, held_before);
            return s;
        }
        attempt.emplace(btree::BtreeInsert(page_store_, ta.desc_page_id, row_id, encoded,
                                           /*trx_id=*/WriterId(scope), ta.oid,
                                           ta.anchor_page_id));
        if (!attempt->ok()) give_back(key_unit, held_before);
    } else {
        // A refused borrow burns the id (K3), which is then free (BD-R4):
        // nothing is on the trail, so the wait survives and `DispatchAsync`
        // re-runs the statement for a fresh id.
        //
        // **An issued id can meet a named key** (BD-S3's review): a named
        // key at the mark borrows it, an issue draws the same id before the
        // named key's admission moves the mark, and the named key places it
        // first. Its row unit serialises the two, and this placement then
        // finds the key present - which is not a reason an omitted tuple key is
        // refused for. The id is burned, its borrow given back, and another
        // drawn; a spill the abandoned round wrote is left as any failed
        // placement leaves one.
        for (int round = 0;; ++round) {
            auto issued = catalog_.AllocateRowId(oid);
            if (!issued.ok()) return issued.status();
            row_id = issued.value();
            if (after_row_id_issued_for_test_) after_row_id_issued_for_test_(row_id);
            const txn::LockKey unit = txn::LockKey::Tuple(ta.oid, row_id);
            if (std::optional<Status> held =
                    BorrowOrWait(scope, unit, RepeatableReadWait::kCapable)) {
                return *held;
            }
            spills.clear();
            if (Status s = encode(); !s.ok()) return s;
            if (after_row_id_fixed_for_test_) after_row_id_fixed_for_test_(row_id);
            attempt.emplace(btree::BtreeInsert(page_store_, ta.desc_page_id, row_id, encoded,
                                               /*trx_id=*/WriterId(scope), ta.oid,
                                               ta.anchor_page_id));
            if (attempt->ok()) break;
            // Nothing was placed, so the burned id's borrow is given back
            // whatever refused it: a structural refusal inside `BEGIN` is
            // re-run (BA-S14) rather than rolled back, and would otherwise
            // hold one burned id per re-run until the decide.
            give_back(unit, /*held_before=*/false);
            if (attempt->status().code() != StatusCode::kAlreadyExists ||
                round + 1 == kMaxIssueRounds) {
                break;
            }
        }
    }
    StatusOr<storage::InsertPlacement> placed = std::move(*attempt);
    if (!placed.ok()) {
        const StatusCode code = placed.status().code();
        // A duplicate, BD-R5's first reason, carries the tuple key token's byte.
        if (explicit_key && code == StatusCode::kAlreadyExists) {
            return duplicate_at_tuple_key(placed.status());
        }
        // Anything else - `OutOfRange` included, which is invariant 3
        // failing - is a placement failure, and is logged.
        if (logging(LogLevel::kWarn)) {
            log_->Warn("btree",
                       "insert into the relation rooted at page " +
                           std::to_string(ta.desc_page_id) +
                           " failed: " + placed.status().message());
        }
        return placed.status();
    }

    // A level the tree grew is published inside the split's own record
    // (`LogSplit`), on the success path below and on E5's take-back alike.

    // ---- E5: a placement no record will describe is taken back ----------
    //
    // BD-R3 E5 (`instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`).
    // From here to the row's trail entry, a failure leaves a row on its leaf
    // with no trail entry and no record, and the leaf is still held
    // (AT-S21). Left there it would shift neighbours redo never sees shift,
    // and its slot would make the next logged insert's slot name a row redo
    // never placed - the mount refuses it (BD-S1's cell). So a btree leaf
    // gives it back under the hold: the slot and its bytes go, the last the
    // leaf allocated. A split cannot give back a rebuilt page, so there the
    // row's slot is retired, keyless, the split is logged without a live row
    // (`kNoTxnId`: no transaction owns a structural change), and a root it
    // grew is published as the success path would.
    const auto take_back = [&](Status failed) -> Status {
        Status undone = [&]() -> Status {
            auto page = page_store_.Get(placed.value().page_id);
            if (!page.ok()) return page.status();
            heap::PageView leaf(page.value().bytes());
            const bool restructured = placed.value().restructured();
            const std::uint16_t slot = placed.value().slot;
            if (Status s = restructured ? leaf.RetireSlot(slot) : leaf.UnInsertTuple(slot);
                !s.ok()) {
                return s;
            }
            if (!restructured) return Status::OK();
            page.value().Release();
            if (Status s = LogSplit(placed.value(), wal::kNoTxnId, oid, ta.anchor_page_id);
                !s.ok()) {
                return s;
            }
            placed.value().held.clear();
            return Status::OK();
        }();
        if (!undone.ok() && logging(LogLevel::kError)) {
            log_->Error("btree",
                        "taking back the unlogged placement of id " +
                            std::to_string(row_id) + " failed: " + undone.message());
        }
        return failed;
    };

    // ---- The Cabin witness (docs/spec/cabin.md §5) ----------------------
    //
    // **Before the log, deliberately.** A WAL failure below reports an error
    // and leaves the tuple in the page - that is a stated, accepted gap
    // (LogInsert's comment) - and a row sitting in a page that no Cabin
    // witnessed is exactly the completeness break C1 forbids. Witnessing
    // first makes the failure cost a log record and never an authority.
    //
    // `ta` is still valid here: the only thing that invalidates it is the
    // desc-page relink below, which happens after.
    NoteCabinWrite(ta, body, /*first_col_pos=*/1, row_id, placed.value().page_id,
                   placed.value().slot);

    // ---- Index maintenance (docs/spec/index.md §2) ----------------------
    //
    // Beside the Cabin witness and before the log, for the same reason and a
    // stronger one: a Cabin that missed an append can be un-observed, and an
    // index that missed one has lost a row to every later probe. So this
    // **fails the statement** where the hook above absorbs.
    //
    // `ta` survives this call even when a split republishes an index root:
    // Catalog::AdoptRoot updates the cached entry in place rather than
    // invalidating it (catalog_cache.hpp), which is exactly what the
    // desc-page relink below does *not* do.
    // Collected only when there is a log to write to, so the unlogged path
    // stays the code it always was.
    std::vector<exec::IndexWrite> index_writes;
    const exec::IndexWriteLog log_index = IndexWriteLogFor(scope, index_writes);
    // The seam fails this exit, not one of its own (BD-S1, E5).
    Status maintained =
        after_placement_for_test_ ? after_placement_for_test_() : Status::OK();
    if (maintained.ok()) {
        maintained = exec::MaintainIndexes(catalog_, page_store_, ta, body,
                                           /*first_col_pos=*/1, encoded, row_id,
                                           /*previous=*/{}, log_index ? &log_index : nullptr);
    }
    if (Status s = maintained; !s.ok()) {
        if (logging(LogLevel::kError)) {
            log_->Error("index", "maintaining the indexes of table oid " +
                                     std::to_string(oid) + " for id " +
                                     std::to_string(row_id) + " failed: " + s.message());
        }
        return take_back(s);
    }

    // ---- The reservation (docs/spec/assertion.md §6.2 step 3) -----------
    //
    // The arrival entry, the group delta, the ASSERT_RESERVE record - after
    // placement so the entry carries the row's real location, before the
    // heap records are logged so a crash that kept the reservation and lost
    // the row over-reserves (compensated by the loser's rollback) rather
    // than under-reserving, which no compensation could see. A failure here
    // fails the statement; the abort path unwinds whatever was applied.
    if (Status s = enforcer_->ReserveInsert(page_store_, wal_, WriterId(scope), admitted, oid,
                                            body, row_id, placed.value().page_id,
                                            placed.value().slot);
        !s.ok()) {
        return take_back(s);
    }

    // ---- The rollback trail, and the durable record beside it -----------
    //
    // The trail entry is what a *live* rollback reads; the undo record is
    // what survives a crash. An insert writes a record (RV10) not for
    // visibility but because each record links to the transaction's
    // previous one, and recovery walks that chain
    // (`docs/workplan-wal-recovery.md` §4b).
    //
    // **The tuple is not stamped with this pointer.** `undo_ptr == 0` still
    // means "inserted" to every reader; the record is reachable only
    // through the transaction chain, which is what keeps §3.6's visibility
    // rule intact.
    //
    // **The spills are already noted**, each at its append (`SpillLogFor`),
    // so the take-back's last exit is the undo append. An insert undo record
    // with no row behind it is work recovery counts done.
    if (scope.txn != nullptr) {
        txn::UndoRecordFields rec{};
        rec.prior_trx_id = txn::kNoTrxId;
        rec.prior_undo_ptr = txn::kNoUndoPtr;
        rec.target_page_id = placed.value().page_id;
        rec.target_slot = placed.value().slot;
        rec.type = static_cast<std::uint8_t>(txn::UndoRecordType::kInsert);
        auto ptr = txn_->AppendUndo(*scope.txn, rec, row_id, {});
        if (!ptr.ok()) return take_back(ptr.status());
    }
    if (scope.txn != nullptr) {
        txn_->NoteInsert(*scope.txn, oid, placed.value().page_id, placed.value().slot,
                         row_id);
    }

    if (before_insert_log_for_test_) before_insert_log_for_test_();

    // Logged under the holds the placement handed out, and before the client
    // is answered - this class's header's ordering note (AT-S21).
    if (Status s = LogInsert(placed.value(), encoded,
                             WriterId(scope), oid, ta.anchor_page_id, spills, index_writes,
                             /*own_txn=*/scope.txn == nullptr);
        !s.ok()) {
        if (logging(LogLevel::kError)) {
            log_->Error("wal", "logging the insert of id " + std::to_string(row_id) +
                                   " failed: " + s.message());
        }
        return s;
    }

    // A relation growing a page is rare and structural - the closest thing
    // this engine has to a file extending - so it is Debug, above the
    // per-tuple Trace line below.
    if (placed.value().restructured() && logging(LogLevel::kDebug)) {
        log_->Debug("btree",
                    "relation of table oid " + std::to_string(oid) +
                        " restructured: the row's page " +
                        std::to_string(placed.value().page_id) +
                        " min_key=" + std::to_string(row_id) + " pages_logged=" +
                        std::to_string(placed.value().changes().size()));
    }

    // Trace: one line per inserted tuple. Logged from here rather than from
    // PageView, which is a bare view over page bytes with no business
    // owning a logger - and which the catalog also writes through, where a
    // "heap insert" line would describe a catalog row, not a user tuple.
    if (logging(LogLevel::kTrace)) {
        log_->Trace("btree", "insert page=" + std::to_string(placed.value().page_id) +
                                " slot=" + std::to_string(placed.value().slot) +
                                " id=" + std::to_string(row_id) +
                                " bytes=" + std::to_string(encoded.size()));
    }

    // The page id rides the (single-row) reply because it is no longer
    // implied by the table: a client that wants to `SHOW PAGE` the row it
    // just wrote would otherwise have to walk the chain to guess.
    out.id = row_id;
    out.page_id = placed.value().page_id;
    out.slot = placed.value().slot;
    return std::nullopt;
}

Status CommandDispatcher::WalkBtreeLeaves(
    PageId root, storage::PageAccess page_access,
    const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&, std::uint16_t)>&
        fn,
    WalkCursor* cursor) {
    const bool resuming = cursor != nullptr && cursor->active;
    const std::uint64_t resume_tuple_key = resuming ? cursor->tuple_key : 0;

    // **A fresh descent, not a remembered leaf**, and that is the whole of
    // the btree arm's safety argument. While the statement was parked
    // another session may have split the leaf it stopped in, moving the
    // upper half of that leaf - possibly including rows this walk had
    // already written - into a new right sibling. Returning to the
    // remembered page would visit those rows a second time. Descending by
    // key cannot: the tree is ordered by Keystone id, so a row this walk
    // finished sorts below `resume_tuple_key` wherever a split has since put it,
    // and the skip below is exact rather than positional.
    auto start = resuming ? btree::BtreeSeekLeaf(page_store_, root, resume_tuple_key)
                          : btree::BtreeLeftmostLeaf(page_store_, root);
    if (!start.ok()) return start.status();

    bool cut = false;
    std::uint64_t cut_tuple_key = 0;
    PageId leaf = start.value();
    const PageId walk_origin = leaf;
    for (std::uint32_t leaves = 0; leaf != kInvalidPageId; ++leaves) {
        if (Status s = storage::CheckPageWalkBudget(leaves, walk_origin, "relation walk");
            !s.ok()) {
            return s;
        }
        const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&,
                                                            std::uint16_t)>
            guarded = [&](PageId page_id, heap::PageView& page,
                          std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            // The key is read for two reasons and only where each is
            // needed: to skip what a resume has already written, and to
            // record where a stop happened. A first walk that never stops
            // reads none of them.
            const auto key_at = [&](std::uint16_t at) -> std::optional<std::uint64_t> {
                auto payload = page.PayloadAt(at, page.slot_count());
                if (!payload.ok()) return std::nullopt;  // dead or retired slot
                auto key = KeystoneIdOfPayload(payload.value());
                if (!key.ok()) return std::nullopt;
                return key.value();
            };
            if (resuming) {
                const std::optional<std::uint64_t> key = key_at(slot);
                // A slot with no readable key is left to `fn`, which is the
                // one place that decides what a dead slot means. Skipping
                // it here would hide a corrupt page behind a resume.
                if (key.has_value() && *key < resume_tuple_key) return storage::VisitControl::kContinue;
            }
            auto outcome = fn(page_id, page, slot);
            if (!outcome.ok() || !outcome.has_value()) return outcome;
            if (outcome.value() == storage::VisitControl::kStop) {
                const std::optional<std::uint64_t> key = key_at(slot);
                // **A stop with no readable key is `Corruption`, not a
                // resume from zero.** `fn` has just read this slot live, so
                // failing to read it back is a corrupt page rather than an
                // ordinary dead slot - and the tempting fallback is the
                // worst outcome the stage can produce. Leaving `cut_tuple_key` at
                // 0 while the cursor goes active makes the resume descend
                // to the *leftmost* leaf and skip nothing, so every row
                // already written is written a second time: the trail
                // doubles, the count doubles, and a ten-row statement
                // answers `UPDATED 16`. That is the silent double-write
                // this arm's key ordering exists to prevent, so it is
                // refused here rather than carried into the resume.
                if (!key.has_value()) {
                    return Status::Corruption(
                        "btree walk stopped at page " + std::to_string(page_id) + " slot " +
                        std::to_string(slot) +
                        " whose Keystone id cannot be read back, so there is no key to resume "
                        "from; refusing rather than resuming from the start of the relation");
                }
                cut = true;
                cut_tuple_key = *key;
            }
            return outcome;
        };
        auto next = btree::BtreeVisitLeafPage(page_store_, leaf, page_access, guarded);
        if (!next.ok()) return next.status();
        if (cut) break;
        leaf = next.value();
    }

    if (cursor != nullptr) {
        if (cut) {
            cursor->tuple_key = cut_tuple_key;
            cursor->active = true;
        } else {
            const std::uint32_t done = cursor->rows_done;
            *cursor = WalkCursor{};
            cursor->rows_done = done;
        }
    }
    return Status::OK();
}

Status CommandDispatcher::VisitRelation(
    const catalog::TableAccess& access, storage::PageAccess page_access,
    const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&, std::uint16_t)>&
        fn,
    WalkCursor* cursor) {
    // Every user relation is a btree (BZ-R1); a catalog chain is walked by
    // the catalog, never here.
    if (access.clustered_type != catalog::ClusteredType::kBtree) {
        return Status::Corruption("relation oid " + std::to_string(access.oid) +
                                  " is not a btree");
    }
    return WalkBtreeLeaves(access.desc_page_id, page_access, fn, cursor);
}

std::optional<std::uint64_t> CommandDispatcher::TupleKeyLiteral(const catalog::TableAccess& access,
                                                          const parser::Condition& cond) const {
    if (access.schema.columns.empty()) return std::nullopt;
    // **`op` only means anything for a `kCompareValue`** (`ast.hpp:226`
    // says so), and every other kind leaves it at its `kEq` default. For
    // most of them that is harmless because `val` is unset and the type
    // test below rejects it - but **`kBetween` carries a real integer
    // literal in `val`: its *low bound*.** Without this line
    // `WHERE id BETWEEN 2 AND 5` reads as `WHERE id = 2`, and the callers
    // then act on it: the point-lookup fast path applies an UPDATE/DELETE
    // to the low bound's row alone and reports `UPDATED 1` - a silent
    // wrong answer; the read path is unaffected because
    // `exec::CompileWhere` lowers a `kBetween` into two conjuncts before
    // anything executes, and the consumers of the *raw* condition are what
    // this function exists to keep honest.
    if (cond.kind != parser::PredicateKind::kCompareValue) return std::nullopt;
    if (cond.rhs_kind != parser::RhsKind::kLiteral) return std::nullopt;
    if (cond.val.type != parser::ValueType::kInt) return std::nullopt;
    // A literal past int64 wrapped in the lexer, so `int_val` is not its
    // value: no window, rather than the one its wrap names (BH-S5's review).
    if (parser::IntLiteralWrapped(cond.val)) return std::nullopt;
    // Negative ids do not exist (invariant 7 zero-extends the id),
    // so a negative literal is a guaranteed miss - and casting it to
    // uint64 would probe an enormous tuple key instead.
    if (cond.val.int_val < 0) return std::nullopt;
    if (!NamesTupleKey(access.schema, cond.col.name)) return std::nullopt;
    return static_cast<std::uint64_t>(cond.val.int_val);
}

std::optional<std::uint64_t> CommandDispatcher::TupleKeyEqualityTarget(
    const catalog::TableAccess& access, const std::vector<parser::Condition>& where) const {
    // Deliberately storage-agnostic: this answers "is this statement a bare
    // tuple key point lookup", which is a property of the WHERE clause alone.
    // Whether anything can shortcut it - a tree descent, or nothing at
    // all - is LocateByTupleKey's question.
    //
    // Exactly one condition: an extra AND could exclude the row the probe
    // would find, and the probe cannot evaluate the second predicate.
    // Falling through costs a scan; getting this wrong costs a wrong answer.
    // **That is what keeps this separate from `DeclaredWriteBorrow`**,
    // which asks a different question of the same conjuncts - "what window
    // does this AND-list bound" - and is right to read every one of them.
    if (where.size() != 1) return std::nullopt;
    const parser::Condition& cond = where.front();
    if (cond.op != parser::CompareOp::kEq) return std::nullopt;
    return TupleKeyLiteral(access, cond);
}

CommandDispatcher::DdlScope CommandDispatcher::DdlScopeFor(WriteScope& write) {
    DdlScope scope;
    if (write.txn == nullptr) return scope;  // no manager: kBootstrapXid, as ever
    scope.txn = write.txn;
    scope.trx_id = write.txn->id();
    // **Open before the first catalog write**, not at `MarkHoldsDdl` after
    // it: a row on the page while no core counts it open is one another
    // core's unfiltered read binds (`ViewFor`).
    OpenDdl(scope.trx_id);

    // RV3-3: the undo record for each catalog write, appended **inside**
    // the catalog's write points so it precedes the row's own record in
    // the log - redo alone must never be able to resurrect a loser's row
    // that the undo phase has no record to retire. Captures the raw
    // transaction pointer; FinishDdlStatement uninstalls before the scope
    // resolves, so the capture cannot outlive its transaction - and the
    // dispatcher is core-local, so no other session's statement can run
    // between install and uninstall.
    catalog_.SetDdlUndoHook([this, t = write.txn](
                                const catalog::Catalog::DdlUndoEvent& e) -> Status {
        txn::UndoRecordFields rec{};
        rec.target_page_id = e.page_id;
        rec.target_slot = e.slot;
        rec.prior_trx_id = e.prior_trx;
        rec.prior_undo_ptr = e.prior_undo;
        std::span<const std::byte> image{};
        switch (e.kind) {
            case catalog::Catalog::DdlUndoEvent::Kind::kInsert:
                rec.type = static_cast<std::uint8_t>(txn::UndoRecordType::kInsert);
                rec.prior_trx_id = txn::kNoTrxId;
                rec.prior_undo_ptr = txn::kNoUndoPtr;
                break;
            case catalog::Catalog::DdlUndoEvent::Kind::kOverwrite:
                rec.type = static_cast<std::uint8_t>(txn::UndoRecordType::kOverwrite);
                image = e.bytes;  // the prior image - the only copy a crash leaves
                break;
            case catalog::Catalog::DdlUndoEvent::Kind::kDeleteMark:
                rec.type = static_cast<std::uint8_t>(txn::UndoRecordType::kDeleteMark);
                break;
        }
        auto ptr = txn_->AppendUndo(*t, rec, e.tuple_key, image);
        return ptr.ok() ? Status::OK() : ptr.status();
    });
    return scope;
}

// Every DDL route runs inside this and only this, which is what makes "no
// handler may skip FinishDdlStatement" structural rather than a convention
// a fifth route forgets: the undo hook the body installs
// through DdlScopeFor(scope) dies here, on every exit, and the implicit
// transaction D2 opens resolves here too.
template <typename Fn>
DispatchOutcome CommandDispatcher::InDdlStatement(Session& session, Fn&& body) {
    auto opened = BeginWrite(session);
    if (!opened.ok()) return {ErrorReply(opened.status()), false, 0, opened.status()};
    WriteScope scope = opened.value();
    DispatchOutcome out = body(scope);
    FinishDdlStatement(session, scope, out);
    return out;
}

void CommandDispatcher::FinishDdlStatement(Session& session, WriteScope& scope,
                                           DispatchOutcome& out) {
    // Unconditionally, every exit: the hook captures this statement's
    // transaction, and the next statement on this shared dispatcher may
    // belong to another session.
    catalog_.SetDdlUndoHook(nullptr);

    const bool owned = scope.owned && scope.txn != nullptr;
    const std::uint64_t id = owned ? scope.txn->id() : 0;
    const bool failed = out.response.rfind("ERR ", 0) == 0;
    const Status verdict =
        failed ? Status::InvalidArgument(out.response) : Status::OK();
    const Status ended = EndWrite(session, scope, verdict);
    if (!ended.ok() && !failed) {
        // The DDL succeeded and its commit did not - the commit failure is
        // the client's answer, exactly as the DML handlers report it.
        out = {ErrorReply(ended), false, 0, ended};
    }
    // The implicit transaction resolved inside EndWrite, so the seam that
    // explicit COMMIT/ROLLBACK reaches through EndDdlScope runs here: the
    // cache the open DDL filtered is stale either way, and settled marks
    // are worth one sweep (§5d). Committed, it also queues a drop's
    // reclaim (BF-R9), at the commit EndWrite acknowledged.
    const bool committed = !failed && ended.ok();
    if (owned) {
        EndDdlScopeById(id, committed ? session.acknowledged_commit_lsn() : wal::kNoLsn);
    }
}

Status CommandDispatcher::EnsureStatementBoundary(Session& session) {
    if (txn_ == nullptr || !session.in_explicit_txn()) return Status::OK();
    if (statement_boundary_taken_) return Status::OK();
    txn::Transaction* txn = session.transaction();
    if (txn == nullptr) return Status::OK();
    if (Status s = txn_->StartStatement(*txn); !s.ok()) return s;
    statement_boundary_taken_ = true;
    return Status::OK();
}

std::optional<txn::ReadView> CommandDispatcher::ViewFor(Session& session) {
    // The fast path, and the one nearly every statement takes. **The
    // instance's count, not this core's `ddl_txns_`**: a DDL open on
    // another core writes rows this core's unfiltered read would bind. And the
    // count the statement head read before it revalidated: a DDL open then
    // may have closed since, leaving a cache this head did not drop.
    if (txn_ == nullptr || (!ddl_open_at_head_ && !txn_->visibility().AnyDdlOpen())) {
        return std::nullopt;
    }
    return MintResolveView(session);
}

txn::ReadView CommandDispatcher::MintResolveView(Session& session) {
    // Take the statement boundary if nothing has yet, so a route that
    // never reaches `SnapshotFor`/`BeginWrite` still resolves under a
    // current view. A failure here means the transaction is no longer
    // active, which the statement itself is about to report - resolving
    // under the view it already holds is the harmless answer.
    (void)EnsureStatementBoundary(session);

    // Inside a transaction, that transaction's own view: it must see the
    // relations it created and no one else's uncommitted ones.
    if (session.in_explicit_txn() && session.transaction() != nullptr) {
        return session.transaction()->view();
    }
    // Autocommit: everything committed right now. Minted per resolution
    // rather than reused, because "right now" is the whole meaning of an
    // autocommit read - and this only runs while DDL is genuinely open.
    return txn_->MintCheckView(txn::kNoTrxId);
}

std::optional<DispatchOutcome> CommandDispatcher::ExistingRelationReply(
    catalog::Oid namespace_oid, std::string_view name) {
    // **Deliberately unfiltered, and this is a decision rather than an
    // omission** (ddl-transactional.md §6's second open item: what two
    // transactions creating the same name should do).
    //
    // Resolving this under the session's view would hide another
    // transaction's uncommitted relation of the same name, both creates
    // would succeed, and the catalog would end up with two rows claiming
    // one name - the last-writer-wins outcome the spec declines. Seeing
    // everything means the second create is **refused** while the first is
    // still open, which is the conservative half of that decision and the
    // one that cannot corrupt anything.
    //
    // The cost, stated because a user will hit it: the refusal can be
    // spurious - if the first transaction rolls back, the name was never
    // taken - and it names a relation the asker cannot see. Improving that
    // message, or holding the second create instead of refusing it, is
    // what the spec still has open.
    //
    // A name is taken within its namespace (namespace.md NS6), so the same
    // name in another namespace is no collision.
    auto existing = catalog_.FindTableOidByName(namespace_oid, name);
    if (existing.status().code() == StatusCode::kNotFound) return std::nullopt;
    if (!existing.ok()) {
        return DispatchOutcome{ErrorReply(existing.status()), false, 0, existing.status()};
    }
    return DispatchOutcome{"EXISTS oid=" + std::to_string(existing.value()), false};
}

void CommandDispatcher::SweepSettledDeleteMarks() {
    // **System core only, as a placement, not an authority.** The sweep
    // judges every mark by the instance's floor and horizon
    // (`ResolvedForEveryReader`, AN-S2), and every core writes the catalog
    // and takes DDL (AT-S5): one core sweeps so two do not walk the same
    // chains at once, and core 0 is the one because it is always present.
    // Sound on any core at any moment: every view that can read a
    // superseded version is registered, a check view needs no lease by
    // proof (`txn.md` §4.1), and the predicate is instance-wide.
    if (core_id_ != catalog::kSystemCore || txn_ == nullptr) return;
    const std::uint64_t marks = catalog_.MarkCount();
    if (marks == 0) return;
    const txn::TransactionManager::Settlement settlement = txn_->SettlementNow();
    if (purge_seen_ == PurgeSeen{settlement, marks}) return;
    auto purged = catalog_.PurgeSettledDeleteMarks();
    ++catalog_purge_sweeps_;
    // The count after the sweep, so its own retirements do not read as a
    // change; the settlement from before it, so a move during it does.
    purge_seen_ = PurgeSeen{settlement, catalog_.MarkCount()};
    if (purged.ok()) {
        catalog_marks_purged_ += purged.value();
    } else if (log_ != nullptr) {
        log_->Warn("catalog", "delete-mark purge failed: " + purged.status().message());
    }
}

void CommandDispatcher::EndDdlScope(const Session& session, wal::Lsn committed_lsn) {
    const txn::Transaction* txn = session.transaction();
    if (txn == nullptr) return;
    EndDdlScopeById(txn->id(), committed_lsn);
}

void CommandDispatcher::NoteOwedIndex(const WriteScope& scope, catalog::Oid rel_oid,
                                      catalog::Oid index_oid, bool created) {
    // Only a decide can settle it, and only a real transaction decides; a
    // bootstrap-id DDL (tests) is left to the next mount's scan (BY-R1).
    if (reclaim_queue_ == nullptr || scope.txn == nullptr) return;
    auto table = catalog_.GetSysTableRow(rel_oid);
    if (!table.ok() || table.value().anchor_page_id == kInvalidPageId) return;
    owed_indexes_.push_back(
        OwedIndex{scope.txn->id(), rel_oid, table.value().anchor_page_id, index_oid, created});
}

void CommandDispatcher::NoteIndexRowDropped(DdlScope& ddl,
                                            const catalog::CatalogRowChange& change) {
    if (ddl.txn == nullptr || change.page_id == kInvalidPageId) return;
    txn_->NoteDeleteMark(*ddl.txn, static_cast<std::uint32_t>(change.rel_oid), change.page_id,
                         change.slot, change.oid, change.prior_trx_id, change.prior_undo_ptr);
    MarkHoldsDdl(*ddl.txn);
}

void CommandDispatcher::NoteRowWritten(DdlScope& ddl, const catalog::CatalogRowRef& row) {
    if (ddl.txn == nullptr || row.page_id == kInvalidPageId) return;
    ddl.written.push_back(row);
    NoteDdlRows(ddl);
}

void CommandDispatcher::EndDdlScopeById(std::uint64_t txn_id, wal::Lsn committed_lsn) {
    const bool held_ddl = std::erase(ddl_txns_, txn_id) > 0;
    // **Both endings invalidate.** A rollback compensates through the page,
    // behind the catalog's back. A commit is the moment a delete-mark
    // starts counting (DT9, `ddl-transactional.md` §5b): a cache filled
    // during an open `DROP INDEX` holds the index deliberately, and holding
    // it past the commit would keep maintaining an index that is gone.
    // Unconditional, since a DDL ending is rare and the clear is cheap.
    if (held_ddl) catalog_.InvalidateAfterCompensation();
    // Uncounted after the decide - committed, its rows are visible to every
    // view; rolled back, compensated - and after the word moved above, so a
    // core that next takes the unfiltered path drops a cache filled while
    // the DDL was open at its next statement head.
    if (held_ddl) txn_->visibility().NoteDdlClosed();
    // **A committed drop is queued with the word the commit moved to**
    // (BF-R9): read after the move above, so every statement whose memo
    // could hold the relation revalidated below it. Any other ending
    // forgets the drop: a rolled-back retype owes nothing.
    for (auto owed = owed_drops_.begin(); owed != owed_drops_.end();) {
        if (owed->txn_id != txn_id) {
            ++owed;
            continue;
        }
        if (committed_lsn != wal::kNoLsn && reclaim_queue_ != nullptr) {
            reclaim_queue_->EnqueueCommittedDrop(owed->oid, owed->roots, committed_lsn,
                                                 catalog_.schema_word_now());
        }
        owed = owed_drops_.erase(owed);
    }
    // BY-R2: a committed drop's tree, and a create's tree at every other
    // ending. The gate: past a commit; at a rollback, which names no commit,
    // the log's end now - every record that names the tree, the build's
    // images, lies below it.
    const bool committed = committed_lsn != wal::kNoLsn;
    const wal::Lsn gate =
        committed ? committed_lsn + 1 : (wal_ != nullptr ? wal_->appended_lsn() : 0);
    for (auto owed = owed_indexes_.begin(); owed != owed_indexes_.end();) {
        if (owed->txn_id != txn_id) {
            ++owed;
            continue;
        }
        if (owed->created != committed) {
            reclaim_queue_->EnqueueOrphanedIndex(owed->rel_oid, owed->anchor, owed->index_oid,
                                                 gate, catalog_.schema_word_now());
        }
        owed = owed_indexes_.erase(owed);
    }
}

void CommandDispatcher::MarkHoldsDdl(txn::Transaction& txn) { txn.NoteWroteCatalog(); }

void CommandDispatcher::OpenDdl(std::uint64_t txn_id) {
    if (std::find(ddl_txns_.begin(), ddl_txns_.end(), txn_id) != ddl_txns_.end()) return;
    ddl_txns_.push_back(txn_id);
    txn_->visibility().NoteDdlOpened();
}

void CommandDispatcher::NoteCatalogRowChanges(
    DdlScope& scope, const std::vector<catalog::CatalogRowChange>& changed) {
    // The rollback trail for a DDL route that *changes* rows rather than
    // inserting them - `DROP TABLE` and `DROP NAMESPACE` (which only ever
    // retypes today).
    //
    // Nothing to do outside an explicit transaction: autocommit's DDL has
    // no rollback to serve, which is why the changes are not even collected
    // there.
    if (scope.txn == nullptr) return;
    for (const catalog::CatalogRowChange& c : changed) {
        if (c.deleted) {
            txn_->NoteDeleteMark(*scope.txn, static_cast<std::uint32_t>(c.rel_oid), c.page_id,
                                 c.slot, c.oid, c.prior_trx_id, c.prior_undo_ptr);
        } else {
            txn_->NoteOverwrite(*scope.txn, static_cast<std::uint32_t>(c.rel_oid), c.page_id,
                                c.slot, c.oid, c.prior_trx_id, c.prior_undo_ptr, c.prior_image);
        }
    }
    // Mark the transaction as holding DDL so `ViewFor` starts filtering -
    // **not** by pushing a fake row into `written`, which would put an
    // insert with an invalid page on the trail and have the abort try to
    // retire it.
    if (!changed.empty()) MarkHoldsDdl(*scope.txn);
}

void CommandDispatcher::NoteDdlRows(DdlScope& scope) {
    if (scope.txn == nullptr) return;
    if (!scope.written.empty()) MarkHoldsDdl(*scope.txn);
    for (const catalog::CatalogRowRef& row : scope.written) {
        txn_->NoteInsert(*scope.txn, static_cast<std::uint32_t>(row.rel_oid), row.page_id,
                         row.slot, row.oid);
    }
    scope.written.clear();
}

txn::TransactionManager::RowLocator CommandDispatcher::RowLocatorForRollback() {
    // How a rollback finds a row whose address moved under it
    // (txn/manager.hpp's RowLocator). A btree leaf division relocates half a
    // leaf and renumbers the rest, so an entry recorded earlier in the same
    // transaction can name a slot that now holds a different row - and
    // compensating that blindly writes over a row this transaction never
    // touched.
    //
    // Built per abort rather than installed on the manager: the manager
    // outlives a dispatcher, and a stored callback capturing `this` would
    // outlive its captures. An abort runs entirely inside one dispatch, so a
    // borrowed one cannot dangle.
    return [this](std::uint32_t rel_oid,
                  std::uint64_t tuple_key) -> StatusOr<txn::TransactionManager::RowLocation> {
        auto access = catalog_.InitTableAccess(static_cast<catalog::Oid>(rel_oid));
        if (!access.ok()) return access.status();
        TupleKeyLookup found = LocateByTupleKey(*access.value(), tuple_key);
        if (found.kind != TupleKeyLookup::Kind::kAt) {
            // kAbsent means the row is gone, kScan means the relation has no
            // descent to ask. Neither is a location, and rollback may not
            // guess one - the caller reports rather than compensating a slot
            // it cannot vouch for.
            return Status::NotFound("row id " + std::to_string(tuple_key) + " of relation oid " +
                                    std::to_string(rel_oid) +
                                    " could not be relocated for rollback");
        }
        return txn::TransactionManager::RowLocation{found.at.page_id, found.at.slot,
                                                    std::move(found.at.leaf)};
    };
}

CommandDispatcher::TupleKeyLookup CommandDispatcher::LocateByTupleKey(const catalog::TableAccess& access,
                                                          std::uint64_t tuple_key) {
    // The tree *is* the relation's storage, so its answer is
    // authoritative in both directions: a hit is where the row lives,
    // and a NotFound means no such row - the scan it replaces would
    // visit the same leaf and find the same nothing. This is the one
    // place a point lookup may skip the scan on a miss, and it is
    // allowed precisely because it is not a hint.
    // Exclusive, because every caller writes the slot it is handed.
    auto found = btree::BtreeLookup(page_store_, access.desc_page_id, tuple_key,
                                    storage::PageAccess::kWrite);
    if (found.ok()) {
        return TupleKeyLookup{TupleKeyLookup::Kind::kAt, std::move(found.value())};
    }
    if (found.status().code() == StatusCode::kNotFound) {
        return TupleKeyLookup{TupleKeyLookup::Kind::kAbsent, {}};
    }
    // A corrupt descent or an unreadable page: fall back to the scan
    // rather than fail the query. The scan reaches the leaves through
    // the sibling links, so it can still answer when the index above
    // them cannot.
    if (logging(LogLevel::kWarn)) {
        log_->Warn("btree", "descent for tuple key " + std::to_string(tuple_key) + " in table oid " +
                                std::to_string(access.oid) +
                                " failed, falling back to a scan: " +
                                found.status().message());
    }
    storage::ClearStructuralRefusal();  // the scan answers it (BA-S14)
    return TupleKeyLookup{TupleKeyLookup::Kind::kScan, {}};
}


namespace {

// Where a written column sits in a view's column list.
//
// A view resolves a column by *name* because it has no Schema to resolve
// an index against - the one place a view is not the real path. One
// function rather than one per consumer, and it answers the whole
// question rather than half of it: **the qualifier is part of what was
// written.** A resolver that checked only the name is how
// `SELECT zzz.oid FROM sys.tables` came to be refused while
// `WHERE zzz.oid = 100` was answered - the same spelling, enforced two
// ways, twelve lines apart.
StatusOr<std::size_t> ResolveViewColumn(const exec::CatalogView& view,
                                        const parser::SelectStmt& stmt,
                                        const parser::ColumnName& col) {
    if (col.qualified() && !IEquals(col.qualifier, stmt.from.binding())) {
        return Status::InvalidArgument("'" + col.qualifier + "." + col.name +
                                       "' names no relation in this statement (byte " +
                                       std::to_string(col.byte_offset) + ")");
    }
    for (std::size_t i = 0; i < view.column_names.size(); ++i) {
        if (IEquals(view.column_names[i], col.name)) return i;
    }
    return Status::InvalidArgument("view sys." + stmt.from.table_name + " has no column '" +
                                   col.name + "' (byte " + std::to_string(col.byte_offset) +
                                   ")");
}

// Where a statement's rows go: the session's sink when one is installed
// (KWP), and the newline protocol's text otherwise.
ResultSink& SinkFor(Session& session, TextResultSink& text) {
    return session.result_sink() != nullptr ? *session.result_sink()
                                            : static_cast<ResultSink&>(text);
}

}  // namespace

// **Every `SHOW` is one `SELECT` over one view** (SV1, SV4): the spelling
// is turned into the `SELECT`'s AST - not its text, so an argument keeps the
// byte the client wrote it at (SV2) - and answered by the function the
// `SELECT` path calls. `DESCRIBE <t>` is one more spelling of the same kind.
DispatchOutcome CommandDispatcher::HandleShow(std::string_view line, std::string_view target,
                                              std::string_view arg, Session& session) {
    // A token is a view into `line`; an absent one (`SplitFirstToken`'s
    // empty rest has no address) is the statement's end.
    const auto byte_of = [&](std::string_view token) {
        const std::string_view whole = Trim(line);
        return static_cast<std::uint32_t>(
            (token.data() == nullptr ? whole.data() + whole.size() : token.data()) - line.data());
    };
    const auto refuse = [&](std::string what) {
        const Status s = Status::InvalidArgument(std::move(what));
        return DispatchOutcome{ErrorReply(s), false, 0, s};
    };
    static constexpr std::string_view kBudget[] = {
        "oid", "namespace", "name", "next_id", "ids_issued", "ids_remaining", "budget_used_ppm",
        "budget_warning", "budget_exhausted"};
    static constexpr std::string_view kPage[] = {
        "page_id", "page_type", "page_lsn", "level", "min_key", "nr_slots", "lower", "upper",
        "free_space", "next_page_id", "slot", "offset", "length", "dead", "sep_key", "child"};
    static constexpr std::pair<std::string_view, std::string_view> kPlain[] = {
        {"META", "meta"},           {"TABLES", "tables"},
        {"NAMESPACES", "namespaces"}, {"PATTERNS", "patterns"},
        {"ACCESS", "access_stats"}, {"BUDGET", "relation_stats"},
        {"CABINS", "cabins"},       {"INDEXES", "indexes"},
        {"FKEYS", "fkeys"},         {"ASSERTIONS", "assertions"},
        {"CABIN_OPTIMIZER", "cabin_optimizer"},
        {"PAGE", "page_slots"},     {"TRANSITIONS", "transitions"}};

    std::string_view view;
    // `DESCRIBE <t>` / `DESC <t>`: the target is the statement's own verb,
    // so `SHOW DESCRIBE` is no spelling of it.
    if (target.data() == Trim(line).data()) {
        view = "columns";
    } else {
        for (const auto& [name, v] : kPlain) {
            if (IEquals(target, name)) view = v;
        }
    }
    if (view.empty()) {
        return refuse("unknown SHOW target '" + std::string(target) + "' (byte " +
                      std::to_string(byte_of(target)) + ")");
    }

    parser::SelectStmt stmt;
    stmt.from.schema = std::string(exec::kCatalogSchema);
    stmt.from.table_name = std::string(view);
    stmt.from.byte_offset = byte_of(target);
    const auto project = [&](std::span<const std::string_view> columns) {
        for (std::string_view c : columns) {
            parser::ColumnName col;
            col.name = std::string(c);
            col.byte_offset = byte_of(target);
            stmt.projection.push_back(std::move(col));
        }
    };
    // `key = <arg>`, the literal at the byte the client wrote it.
    const auto where_key = [&](std::string_view column, std::string_view token, bool integer) {
        parser::Condition cond;
        cond.kind = parser::PredicateKind::kCompareValue;
        cond.op = parser::CompareOp::kEq;
        cond.rhs_kind = parser::RhsKind::kLiteral;
        cond.col.name = std::string(column);
        cond.col.byte_offset = byte_of(token);
        cond.val.byte_offset = byte_of(token);
        if (integer) {
            std::uint64_t v = 0;
            const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), v);
            if (ec != std::errc() || ptr != token.data() + token.size()) {
                return false;
            }
            cond.val.type = parser::ValueType::kInt;
            cond.val.int_val = static_cast<std::int64_t>(v);
            if (v > static_cast<std::uint64_t>(INT64_MAX)) cond.val.raw_int_text = std::string(token);
        } else {
            cond.val.type = parser::ValueType::kStr;
            cond.val.str_val = std::string(token);
        }
        stmt.where.push_back(std::move(cond));
        return true;
    };

    auto [first, extra] = SplitFirstToken(Trim(arg));
    if (view == "columns") {
        if (!Trim(extra).empty()) {
            return refuse(std::string(target) + " takes at most one relation name (byte " +
                          std::to_string(byte_of(Trim(extra))) + ")");
        }
        if (first.empty()) {
            return refuse("DESCRIBE requires a table name (byte " +
                          std::to_string(byte_of(target) + target.size()) + ")");
        }
        where_key("rel_name", first, false);
    } else if (view == "page_slots") {
        if (first.empty()) {
            return refuse("SHOW PAGE requires a page id (byte " +
                          std::to_string(byte_of(target) + target.size()) + ")");
        }
        if (!where_key("page_id", first, true)) {
            return refuse("invalid page id '" + std::string(first) + "' (byte " +
                          std::to_string(byte_of(first)) + ")");
        }
        const std::string_view option = Trim(extra);
        if (option.empty()) {
            project(kPage);  // `VALUES` adds the payload column
        } else if (!IEquals(option, "VALUES")) {
            return refuse("unknown SHOW PAGE option '" + std::string(option) + "' (byte " +
                          std::to_string(byte_of(option)) + ")");
        }
    } else {
        if (!first.empty()) {
            return refuse("SHOW " + std::string(target) + " takes no argument (byte " +
                          std::to_string(byte_of(first)) + ")");
        }
        if (view == "relation_stats") project(kBudget);
    }
    return HandleCatalogView(stmt, session);
}

DispatchOutcome CommandDispatcher::HandleCatalogView(const parser::SelectStmt& stmt,
                                                     Session& session) {
    // **AG12.** A catalog view's rows come from the catalog's typed readers,
    // not from a step chain - so there is no `RowSink` for AG1's fold to
    // wrap and nothing for it to consume. Refused here rather than
    // half-supported by a second fold over a different row source, which
    // would be exactly the "second place that reasons about statement
    // shape" the placement decision exists to prevent.
    if (stmt.aggregated()) {
        return {"ERR aggregation over a catalog view (sys." + stmt.from.table_name +
                    ") is not supported; a view's rows do not come from a step chain",
                false};
    }

    const SysViewSpec* spec = FindSysView(stmt.from.table_name);
    if (spec == nullptr) {
        const Status s = Status::NotFound("no catalog view named 'sys." + stmt.from.table_name +
                                          "'");
        return {ErrorReply(s), false, 0, s};
    }
    // **A view's key** (BO-Q12, `sys_views.hpp`): the conjunct
    // `key = <literal>`, coerced to the key's type, is handed to the builder
    // as a bound on what it reads; a required key not written is refused.
    std::optional<parser::AstValue> key;
    // The conjunct the builder took as its key: it bounded what the builder
    // read, and the builder resolved it - `ns.t` names the rows whose
    // `rel_name` is `t` - so it is not compared against those rows again.
    std::optional<std::size_t> key_at;
    if (spec->key_mode != ViewKeyMode::kNone) {
        for (std::size_t i = 0; i < stmt.where.size(); ++i) {
            const parser::Condition& cond = stmt.where[i];
            if (cond.kind != parser::PredicateKind::kCompareValue ||
                cond.op != parser::CompareOp::kEq || cond.rhs_kind != parser::RhsKind::kLiteral ||
                !IEquals(cond.col.name, spec->key_column)) {
                continue;
            }
            catalog::SysColumnRow column{};
            column.type_val = spec->key_type;
            parser::AstValue literal = cond.val;
            if (Status s = exec::CoerceLiteralToColumn(column, literal)
                               .WithContext("column '" + cond.col.name + "' literal at byte " +
                                            std::to_string(cond.val.byte_offset));
                !s.ok()) {
                return {ErrorReply(s), false, 0, s};
            }
            key = std::move(literal);
            key_at = i;
            break;
        }
        // A relation-name key written bare takes its namespace from a
        // `namespace = '<literal>'` conjunct when there is one, rather than
        // from the session (NS5): `WHERE namespace = 'ledger' AND rel_name =
        // 't'` names `ledger.t`. That conjunct still filters the rows, which
        // all carry the namespace the key resolved in.
        if (key.has_value() && spec->key_type == catalog::kTypeValVarchar &&
            key->str_val.find('.') == std::string::npos) {
            for (const parser::Condition& cond : stmt.where) {
                if (cond.kind == parser::PredicateKind::kCompareValue &&
                    cond.op == parser::CompareOp::kEq && cond.rhs_kind == parser::RhsKind::kLiteral &&
                    IEquals(cond.col.name, "namespace") &&
                    cond.val.type == parser::ValueType::kStr) {
                    key->str_val = cond.val.str_val + "." + key->str_val;
                    break;
                }
            }
        }
        if (!key.has_value() && spec->key_mode == ViewKeyMode::kRequired) {
            const Status s = Status::InvalidArgument(
                "sys." + std::string(spec->name) + " is read one key at a time: write WHERE " +
                std::string(spec->key_column) + " = <literal> (byte " +
                std::to_string(stmt.from.byte_offset) + ")");
            return {ErrorReply(s), false, 0, s};
        }
    }
    auto view =
        ReadSysView(*spec, session, key.has_value() ? &*key : nullptr, stmt.from.byte_offset);
    if (!view.ok()) {
        return {ErrorReply(view.status()), false, 0, view.status()};
    }
    const exec::CatalogView& rows = view.value();

    // The projection is resolved against the view's own column list rather
    // than a Schema, because a view has none - the names here are the
    // header the reply prints, not a stored definition.
    std::vector<std::size_t> project;
    if (stmt.star()) {
        for (std::size_t i = 0; i < rows.column_names.size(); ++i) project.push_back(i);
    } else {
        for (const parser::ColumnName& col : stmt.projection) {
            auto found = ResolveViewColumn(rows, stmt, col);
            if (!found.ok()) return {ErrorReply(found.status()), false, 0, found.status()};
            project.push_back(found.value());
        }
    }

    // **`ORDER BY` over a view (BO-Q3, W4)**: each key resolved against the
    // view's columns before any row is read, as the WHERE below is, and
    // ordered by the engine's one ordering (`exec::OrderKeyOf`) under
    // the type the view declares - not by a second comparator. No
    // `sort_max_rows` cap: the cap bounds what a walk would buffer, and a
    // view's rows are already whole in memory before the sort sees them.
    std::vector<std::pair<std::size_t, bool>> sort_by;
    sort_by.reserve(stmt.order_by.size());
    for (const parser::SortKey& key : stmt.order_by) {
        auto at = ResolveViewColumn(rows, stmt, key.key.column);
        if (!at.ok()) return {ErrorReply(at.status()), false, 0, at.status()};
        sort_by.emplace_back(at.value(), key.descending);
    }

    // A WHERE clause still applies. The values are ordinary AstValues by
    // now, so this compares them exactly as the row evaluator does - but
    // by *name*, because a view has no schema to resolve an index against.
    // That is the one place a view is not the real path; it is confined
    // here, and a subquery predicate is refused rather than half-applied.
    //
    // **Resolved once, before any row is read.** Every question below is a
    // property of the statement, not of a row - is this a predicate shape
    // a view can answer, does this column exist - and asking them inside
    // the row loop made the *refusal* depend on how many rows the catalog
    // happened to hold. `SELECT * FROM sys.patterns WHERE oid =
    // fetch_id` answered a header on a fresh instance, because the loop
    // never ran, and refused over `sys.tables`, because it did. A refusal
    // that data can silence is not a refusal.
    std::vector<std::size_t> where_at;
    where_at.reserve(stmt.where.size());
    // Each conjunct's literal and BETWEEN high bound, as coerced to the
    // view's declared type: the rows compare what the check admitted.
    std::vector<std::pair<parser::AstValue, parser::AstValue>> bounds;
    bounds.reserve(stmt.where.size());
    for (const parser::Condition& cond : stmt.where) {
        if (cond.has_subquery()) {
            return {"ERR a subquery predicate over a catalog view is not supported: the "
                    "view is materialized, so there is no relation for a sub-chain to "
                    "correlate against", false};
        }
        // Before anything reads `col`: a boolean conjunct has none (BU-Q4).
        if (cond.kind == parser::PredicateKind::kBoolTree) {
            const Status refused = Status::NotImplemented(
                "a boolean conjunct (OR, NOT, an IN list, LIKE) over a catalog view (byte " +
                std::to_string(cond.tree->byte_offset) + ")");
            return {ErrorReply(refused), false, 0, refused};
        }
        // Before anything reads `col`: a function conjunct has none on a
        // call's side, and the view's evaluator compares `col op val` only.
        if (cond.kind == parser::PredicateKind::kCompareFunction) {
            const std::uint32_t at = cond.lhs_fn.has_value() ? cond.lhs_fn->byte_offset
                                                             : cond.rhs_fn->byte_offset;
            const Status refused = Status::NotImplemented(
                "a function call over a catalog view (byte " + std::to_string(at) + ")");
            return {ErrorReply(refused), false, 0, refused};
        }
        if (cond.rhs_kind == parser::RhsKind::kColumn) {
            return {"ERR a column-to-column comparison over a catalog view is not "
                    "supported", false};
        }
        auto at = ResolveViewColumn(rows, stmt, cond.col);
        if (!at.ok()) return {ErrorReply(at.status()), false, 0, at.status()};
        where_at.push_back(at.value());

        // The relation's literal rule (types.md §3.1a), against the type the
        // view declares for the column: `name = 5` and `oid = 'x'` are
        // refused rather than answered with no rows, and so is a negative
        // or past-2^64 literal against a view's uint64.
        catalog::SysColumnRow column{};
        column.type_val = rows.column_types[at.value()];
        const auto coerce = [&](parser::AstValue& literal) {
            const std::uint32_t byte = literal.byte_offset;
            return exec::CoerceLiteralToColumn(column, literal)
                .WithContext("column '" + cond.col.name + "' literal at byte " +
                             std::to_string(byte));
        };
        auto& [low, high] = bounds.emplace_back(cond.val, cond.val_high);
        Status checked = coerce(low);
        if (checked.ok() && cond.kind == parser::PredicateKind::kBetween) {
            checked = coerce(high);
        }
        if (!checked.ok()) return {ErrorReply(checked), false, 0, checked};
    }

    // **SV5 (BO-R3): a view answers as a relation does** - described and
    // emitted through the statement's `ResultSink`, typed on KWP and the
    // header-and-rows text on the newline protocol, never a reply of its
    // own rendering. A view has no Keystone column, so no field is flagged.
    TextResultSink text_sink;
    ResultSink& sink = SinkFor(session, text_sink);
    std::vector<std::string> names;
    std::vector<std::uint32_t> types;
    names.reserve(project.size());
    types.reserve(project.size());
    for (std::size_t index : project) {
        names.push_back(rows.column_names[index]);
        types.push_back(rows.column_types[index]);
    }
    if (Status s = sink.Describe(DescribeFields(names, types, {})); !s.ok()) {
        return {ErrorReply(s), false, 0, s};
    }

    std::vector<const std::vector<parser::AstValue>*> qualifying;
    for (const std::vector<parser::AstValue>& row : rows.rows) {
        bool matched = true;
        for (std::size_t i = 0; i < stmt.where.size(); ++i) {
            if (i == key_at) continue;
            const parser::Condition& cond = stmt.where[i];
            const parser::AstValue& value = row[where_at[i]];
            // The declared type, as the check above used; integers compare
            // by their digits whatever the type, so a `fetch_id` above
            // INT64_MAX is the uint64 it is.
            const std::uint32_t type_val = rows.column_types[where_at[i]];
            const auto Compare = [&](const parser::AstValue& bound, parser::CompareOp op) {
                return exec::CompareValues(type_val, value, bound, op);
            };
            // `BETWEEN` is two comparisons, inclusive at both ends, and it
            // has to be spelled out here because it is spelled out nowhere
            // else: `exec::CompileWhere` lowers a `kBetween` into two
            // ordinary conjuncts before anything executes, so no evaluator
            // downstream ever reads `kind`. This path is the one consumer
            // that never got that lowering, and it read `op` - still the
            // `kEq` its default leaves it at - so `oid BETWEEN 100 AND 130`
            // silently meant `oid = 100`, dropping the high bound and most
            // of the answer.
            matched = cond.kind == parser::PredicateKind::kBetween
                          ? Compare(bounds[i].first, parser::CompareOp::kGte) &&
                                Compare(bounds[i].second, parser::CompareOp::kLte)
                          : Compare(bounds[i].first, cond.op);
            if (!matched) break;
        }
        if (!matched) continue;
        qualifying.push_back(&row);
    }

    if (!sort_by.empty()) {
        // Normalized once per row, as `exec::OutputSort` does; the
        // qualifying order breaks ties, so the sort is stable.
        struct Keyed {
            std::vector<exec::OrderKey> keys;
            const std::vector<parser::AstValue>* row;
        };
        std::vector<Keyed> keyed;
        keyed.reserve(qualifying.size());
        for (const std::vector<parser::AstValue>* row : qualifying) {
            Keyed& k = keyed.emplace_back(Keyed{{}, row});
            for (const auto& [at, descending] : sort_by) {
                auto key = exec::OrderKeyOf(rows.column_types[at], (*row)[at]);
                if (!key.ok()) return {ErrorReply(key.status()), false, 0, key.status()};
                k.keys.push_back(std::move(key.value()));
            }
        }
        std::stable_sort(keyed.begin(), keyed.end(), [&](const Keyed& a, const Keyed& b) {
            for (std::size_t k = 0; k < sort_by.size(); ++k) {
                const int c = a.keys[k].Compare(b.keys[k]);
                if (c != 0) return sort_by[k].second ? c > 0 : c < 0;
            }
            return false;
        });
        for (std::size_t r = 0; r < keyed.size(); ++r) qualifying[r] = keyed[r].row;
    }

    // I11's contract, over the one row source that is not a chain: a
    // prefix of the rows in the order above is a prefix of an order the
    // statement has, so `LIMIT` needs no `ORDER BY` to be well defined.
    exec::EmissionQuota quota(stmt.offset, stmt.limit);
    std::vector<parser::AstValue> values(project.size());
    std::string encoded;
    for (const std::vector<parser::AstValue>* row : qualifying) {
        const exec::QuotaVerdict verdict = quota.Note();
        if (verdict == exec::QuotaVerdict::kStop) break;
        if (verdict == exec::QuotaVerdict::kSkip) continue;
        for (std::size_t i = 0; i < project.size(); ++i) values[i] = (*row)[project[i]];
        Status s = sink.EncodeValueRow(types, values, encoded);
        if (s.ok()) s = sink.Emit(encoded);
        if (!s.ok()) return {ErrorReply(s), false, 0, s};
        if (verdict == exec::QuotaVerdict::kEmitThenStop) break;
    }
    return {text_sink.Take(), false};
}

namespace {

// Re-emits multi-line text under the dispatcher's one-line wire contract:
// sections are joined with the literal two-character "\n" escape, never a
// raw newline byte (docs/spec/client-manual.md section 2). The plan printer
// produces ordinary newlines because the same text goes to a test's
// assertion unescaped; the escaping belongs here, at the wire.
void AppendEscaped(std::ostringstream& os, const std::string& text) {
    for (char c : text) {
        if (c == '\n') {
            os << "\\n";
        } else {
            os << c;
        }
    }
}

}  // namespace

// Executes `chain` with an `Aggregator` in place of the row formatter, and
// emits the fold's output rows (docs/spec/aggregate.md AG1, workplan AG06).
//
// `header` is the column-heading line the caller already built from
// `chain.column_names` - which for an aggregated chain labels the *fold's*
// output (`b`, `count(*)`, `sum(distinct x)`), so the reply's shape is one
// heading per emitted value exactly as it is for a projection.
//
// Note what this function does not touch. The trail collector and the
// replay index are passed straight through to `Execute` and behave as they
// would without a fold; nothing here consults them, and nothing here can
// change what the chain read. That is AG1 - the fold consumes rows and has
// no opinion about where they came from.
DispatchOutcome CommandDispatcher::RunAggregated(
    ResultSink& sink, TextResultSink& text_sink, const exec::StepChain& chain,
    exec::TrailCollector* trail, const exec::TrailReplay* replay,
    const std::optional<StatementIdentity>& identity, const txn::Snapshot& snapshot,
    exec::PositionSink& borrow, exec::ExecStats& stats) {
    if (Status s = aggregator_.Reset(*chain.aggregate, chain.column_names, aggregate_limits_);
        !s.ok()) {
        return {ErrorReply(s), false, 0, s};
    }

    // A MIN/MAX or a group key over a relation's column 0 is a Keystone id,
    // written as one whatever the column declares (`wire::WireValueType`).
    // Widened for the wire only: `AggregateOutputType` stays the column's
    // type, which `UPDATE ... SET` typing reads. Not through
    // `DescribeFields`' projection, which would flag a `COUNT(*)` - whose
    // ref is `{0, 0, 0}` too - as a Keystone; its int64 is unchanged here.
    const std::vector<std::uint32_t> out_types = AggregateOutputTypes(*chain.aggregate);
    std::vector<std::uint32_t> wire_types(out_types.size());
    for (std::size_t i = 0; i < out_types.size(); ++i) {
        wire_types[i] = wire::WireValueType(out_types[i], chain.aggregate->items[i].ref.col_pos);
    }
    std::vector<wire::FieldDescription> fields = DescribeFields(
        chain.column_names, out_types, AggregateOutputTypeMods(*chain.aggregate));
    for (std::size_t i = 0; i < fields.size() && i < wire_types.size(); ++i) {
        fields[i].type_len = wire::WireTypeLen(wire_types[i]);
    }
    if (Status s = sink.Describe(std::move(fields)); !s.ok()) {
        return {ErrorReply(s), false, 0, s};
    }

    // The fold's own failures - a SUM overflow, a cap - have to reach the
    // client, and a `RowSink` answers `StatusOr<VisitControl>`, so a
    // non-ok status ends the walk and propagates out of Execute. That is
    // the same path a decode error already takes.
    stats.steps.clear();
    Status ran = exec::Execute(
        catalog_, page_store_, chain,
        [&](const exec::ChainFrame& frame) -> StatusOr<storage::VisitControl> {
            if (Status s = aggregator_.Accumulate(frame); !s.ok()) return s;
            return storage::VisitControl::kContinue;
        },
        &stats, budget_, trail, replay, cabins_, &snapshot, indexes_enabled_,
        &borrow);
    if (!ran.ok()) {
        // **No trail on the failure path**, exactly as the unaggregated
        // path has it: a statement that stopped part way through touched
        // some tuples, and a trail describing that points a later reader at
        // a state no reader should be pointed at.
        return {ErrorReply(ran), false, 0, ran};
    }

    // Through the one sink every row path emits by: two formatters for a
    // folded row is how one of them comes to forget an item's `type_val`.
    std::string row_scratch;
    Status emitted = aggregator_.Finish(
        [&](std::span<const parser::AstValue> row) -> Status {
            if (Status s = sink.EncodeValueRow(wire_types, row, row_scratch); !s.ok()) return s;
            return sink.Emit(row_scratch);
        });
    if (!emitted.ok()) {
        return {ErrorReply(emitted), false, 0, emitted};
    }

    // Recorded after a *complete* execution, and unconditionally - the fold
    // is downstream of all three, so an aggregated statement records the
    // trail, the access shape and the signals its unaggregated twin would.
    RecordExecution(identity, trail, chain, stats);
    return {text_sink.Take(), false};
}

// Executes `chain` for its counters rather than its rows, and reports the
// plan beside them.
//
// The sink accepts every row and formats none: the *executor* does exactly
// what a real run does - same steps, same descents, same decodes - and only
// the dispatcher's own row formatting is skipped, because the reply is the
// plan. Anything more clever here (stopping early, skipping the sink) would
// make ANALYZE describe a run that never happened.
DispatchOutcome CommandDispatcher::RunAnalyze(const exec::StepChain& chain,
                                              exec::TrailCollector* trail,
                                              const exec::TrailReplay* replay,
                                              const std::optional<StatementIdentity>& identity,
                                              const txn::Snapshot& snapshot,
                                              exec::PositionSink& borrow,
                                              exec::OutputSort& sorter) {
    exec::ExecStats stats;
    std::uint64_t rows = 0;

    // **The fold runs under ANALYZE too** (AG15). Its whole contract is
    // that the run it describes is the run that actually happened, and a
    // fold is not free - it hashes a key and folds a state per row, and a
    // SUM that overflows fails the statement. Skipping it would make
    // ANALYZE describe an execution the client cannot reproduce, which is
    // the same reason replay is not skipped here.
    // The same hoisted aggregator the row-returning path uses. Safe to
    // share because the two are mutually exclusive per statement: ANALYZE
    // returns before the sink path is reached.
    const bool folding = chain.aggregated();
    if (folding) {
        if (Status s = aggregator_.Reset(*chain.aggregate, chain.column_names,
                                         aggregate_limits_);
            !s.ok()) {
            return {ErrorReply(s), false, 0, s};
        }
    }

    // **The quota runs under ANALYZE too**, for AG15's reason one seam
    // over: a limited statement's real run stops when the quota fills, so
    // skipping the quota here would make ANALYZE describe a run - every
    // page of a walk `LIMIT 1` never touches - that the client cannot
    // reproduce. `rows=` therefore counts emitted rows, and `examined=`
    // beside it is what an OFFSET's skipped rows still cost. A chain never
    // carries both a fold and a quota - the parser refuses the tail over
    // aggregated output - so the two wrappers cannot compose.
    // **The sort runs under ANALYZE too**, for the same contract's sake and
    // with a consequence the fold does not have: a sorted statement cannot
    // stop early, so its `pages=` and `examined=` are the *unlimited*
    // statement's however small its `LIMIT`. Skipping the sort here would
    // report the stopping run that a sorted statement never performs.
    //
    // The rows are not rendered - the reply is the plan - so the sort holds
    // keys and empty text. That is the one respect in which ANALYZE's run
    // is cheaper than the real one, and it is the same respect in which it
    // was already cheaper before a sort existed.
    exec::EmissionQuota quota(chain);
    sorter.Reset(chain, sort_max_rows_);
    std::string analyze_scratch;
    Status ran = exec::Execute(
        catalog_, page_store_, chain,
        [&](const exec::ChainFrame& frame) -> StatusOr<storage::VisitControl> {
            if (sorter.active()) {
                auto admitted = sorter.Admit(frame);
                if (!admitted.ok()) return admitted.status();
                // Admitted rows are taken with empty text, which is the one
                // respect in which this run is cheaper than the real one -
                // and the same respect in which it already was. What matters
                // is that the same rows are *admitted*, so `sorted=` and
                // `rows=` describe the run that would have happened.
                if (admitted.value()) sorter.Take(analyze_scratch);
                return storage::VisitControl::kContinue;
            }
            const exec::QuotaVerdict verdict = quota.Note();
            if (verdict == exec::QuotaVerdict::kStop) return storage::VisitControl::kStop;
            if (verdict == exec::QuotaVerdict::kSkip) return storage::VisitControl::kContinue;
            ++rows;
            if (folding) {
                if (Status s = aggregator_.Accumulate(frame); !s.ok()) return s;
            }
            return verdict == exec::QuotaVerdict::kEmitThenStop
                       ? storage::VisitControl::kStop
                       : storage::VisitControl::kContinue;
        },
        &stats, budget_, trail, replay, cabins_, &snapshot, indexes_enabled_,
        &borrow);
    if (!ran.ok()) {
        return {ErrorReply(ran), false, 0, ran};
    }
    // `rows=` counts what the client would have been sent, so on the sorted
    // path the quota runs where the real path runs it: after the order
    // exists.
    if (sorter.active()) {
        sorter.Finish();
        exec::DrainSorted(quota, sorter.rows(), [&](const exec::OutputSort::Row&) { ++rows; });
    }
    RecordExecution(identity, trail, chain, stats);

    const exec::StepStats total = stats.Total();
    std::ostringstream os;
    os << "analyze rows=" << rows << " class=" << exec::StatementClassName(chain.klass)
       << " steps=" << chain.steps.size() << " examined=" << total.rows_examined
       << " pages=" << total.pages_fetched << " opens=" << total.relation_opens;

    // The number an aggregated statement is actually about. `rows=` stays
    // what it has always been - the rows the *chain* produced - so the two
    // together say what the fold cost and what it collapsed to, which one
    // number could not.
    if (folding) {
        os << " groups=" << aggregator_.group_count();
    }

    // What the sort held, which under a `LIMIT` is what it retained rather
    // than what arrived (OB5) - the number that says what the sort cost in
    // memory. `examined=` beside it says what the walk cost, and the two
    // differing by orders of magnitude is the top-N heap doing its job.
    if (sorter.active()) {
        os << " sorted=" << sorter.rows().size();
    }

    // The statement's own pattern_id, and the fetch_id its trail is keyed
    // by - the hex `SHOW PATTERNS` lists a row under (AP-S2). The second is
    // what makes "which recorded trail does this statement use" answerable
    // by comparing two numbers, with no trail recorder needed for the
    // comparison to mean something; the first is the shape the cabin
    // optimizer counts.
    //
    // Taken from the identity the caller already holds - which came from
    // the parse, not from a second lex of `sql`.
    if (identity.has_value()) {
        os << " pattern_id=0x" << std::hex << identity->pattern_id
           << " fetch_id=0x" << identity->trail.fetch_id << std::dec;
    }

    os << "\\n";
    AppendEscaped(os, exec::FormatPlan(chain));

    const std::string per_step = exec::FormatStepStats(chain, stats);
    if (!per_step.empty()) {
        os << "\\n";
        AppendEscaped(os, per_step);
    }
    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleSelect(std::string_view line, Session& session,
                                                bool analyze, const txn::Snapshot* pinned,
                                                const exec::StatementContext* context) {
    // The statement boundary. Under READ COMMITTED this is where a new read
    // view is taken - so two SELECTs in one transaction can see different
    // data, which is the level's entire definition.
    //
    // **Everything the statement reads lives on its run** (BA-S15), which
    // the plain path hands on so its walk can outlive this function.
    const auto run = std::make_shared<SelectRun>(*this, session);
    // The statement's constants, the session's namespace among them, taken
    // here when no caller carries its own; read only by the compile below.
    exec::StatementContext own_context;
    if (context == nullptr) {
        own_context = StatementContextFor(session);
        context = &own_context;
    }
    auto snapshot = pinned != nullptr
                        ? StatusOr<txn::LeasedSnapshot>(txn::LeasedSnapshot{*pinned, {}})
                        : SnapshotFor(session);
    // `ErrorReply`, not a bare "ERR ": `SnapshotFor` can refuse, and the
    // wire's `retryable=1` is what a client's retry loop reads.
    if (!snapshot.ok()) return {ErrorReply(snapshot.status()), false, 0, snapshot.status()};
    run->snapshot.emplace(std::move(snapshot.value()));
    const txn::Snapshot& snap = run->snapshot->snap;

    // An explicit Parser rather than the free `Parse()`, so the statement's
    // fingerprint can be taken **from the parse itself** (parser.hpp): a
    // second lex was ~13% of a point join's latency
    // (bench/results-waystone-v2.md, at 1769487).
    parser::Parser& parser = run->parser.emplace(line);
    auto parsed = parser.Parse();
    // `sys.statement_stats` (BO-S5): the first parse of the statement keys
    // it - an UPDATE's SET subquery runs here after the UPDATE's own parse.
    if (const auto fingerprint = parser.fingerprint();
        fingerprint.has_value() && statement_fetch_id_ == 0) {
        statement_fetch_id_ = fingerprint->fetch_id;
    }
    if (!parsed.ok()) {
        // BU-Q9: what the native grammar refuses, the query layer may read.
        if (!analyze) {
            if (auto layered = TryQueryLayer(line, session, snap, context); layered.has_value()) {
                return std::move(*layered);
            }
        }
        return {ErrorReply(parsed.status()), false, 0, parsed.status()};
    }
    if (!std::holds_alternative<parser::SelectStmt>(parsed.value())) {
        return {"ERR expected a SELECT statement", false};
    }
    run->statement.emplace(std::move(parsed.value()));
    return SelectBody(run, session, analyze, context, line);
}

DispatchOutcome CommandDispatcher::SelectBody(const std::shared_ptr<SelectRun>& run,
                                              Session& session, bool analyze,
                                              const exec::StatementContext* context,
                                              std::string_view line) {
    const txn::Snapshot& snap = run->snapshot->snap;
    auto& stmt = std::get<parser::SelectStmt>(*run->statement);

    // A catalog view (`SELECT * FROM sys.tables`) is answered before the
    // compiler is asked for a chain, because it is not a relation: its
    // rows are produced by the catalog's typed readers, not walked out of
    // pages, so there is nothing for a step to read (exec/catalog_view.hpp
    // says why the on-disk formats cannot be unified).
    if (!stmt.from.schema.empty() || !stmt.joins.empty()) {
        const bool from_is_view =
            IEquals(stmt.from.schema, exec::kCatalogSchema) &&
            FindSysView(stmt.from.table_name) != nullptr;
        // The same test the FROM relation gets: under a user namespace a
        // non-empty qualifier is not a view.
        bool any_join_is_view = false;
        for (const parser::JoinClause& join : stmt.joins) {
            if (IEquals(join.relation.schema, exec::kCatalogSchema) &&
                FindSysView(join.relation.table_name) != nullptr) {
                any_join_is_view = true;
            }
        }
        if (from_is_view || any_join_is_view) {
            if (analyze) {
                return {"ERR a catalog view has no plan to analyze: sys.* rows are produced by "
                        "the catalog's readers rather than read from pages, so nothing compiles "
                        "to a step chain",
                        false};
            }
            if (!stmt.joins.empty()) {
                return {"ERR a catalog view cannot be joined: sys.* rows are produced by the "
                        "catalog's readers rather than read from pages, so there is no "
                        "relation for a join step to walk",
                        false};
            }
            return HandleCatalogView(stmt, session);
        }
    }
    if (!stmt.from.schema.empty()) {
        // Two different mistakes, two different messages. A known schema
        // with an unknown view is a typo in the view name; an unknown
        // schema is a wrong idea about what schemas exist. Reporting the
        // second for the first sends the reader looking in the wrong
        // place, which is the whole failure mode a positioned error
        // exists to avoid.
        if (IEquals(stmt.from.schema, exec::kCatalogSchema)) {
            std::string known;
            for (const std::string& name : SysViewNames()) {
                if (!known.empty()) known += ", ";
                known += "sys." + name;
            }
            return {"ERR no catalog view named 'sys." + stmt.from.table_name + "' (known: " +
                        known + ")",
                    false};
        }
        // A user namespace falls through to the compiler, whose resolution
        // (NS5) refuses an unknown one at its byte - one site for FROM, JOIN
        // and every subquery.
    }

    // V17: parse -> compile -> execute. Which relation, which access path
    // and where each predicate is evaluated are the compiler's, in the chain.
    // Opened before the compile, which declares into it at every bind
    // (`step_compiler.hpp`'s `declare`; AT-R1). Lives on the run to the end
    // of the statement, which a sliced walk carries past this function
    // (BA-S15). A second compile declares into it again, which asks each
    // relation once (`ReadBorrow::Position`).
    ReadBorrow& borrow = run->borrow.emplace(locks_, NextReadHolder(), &read_borrows_);
    auto chain = ResolveNames(session, [&](const txn::ReadView* v) {
        exec::PlanOptions plan;
        plan.join_build_max_rows = budget_.join_build_max_rows();
        plan.sort_max_rows = sort_max_rows_;
        plan.statistics = plan_stats_ != nullptr ? &plan_source_ : nullptr;
        plan.memo = &plan_memo_;
        if (const auto fingerprint = run->parser->fingerprint(); fingerprint.has_value()) {
            plan.fetch_id = fingerprint->fetch_id;
            plan.arg_hash = fingerprint->arg_hash;
            plan.literals_known = fingerprint->param_count == 0;
        }
        return exec::Compile(catalog_, stmt, v, &borrow, context,
                             plan_join_order_ ? &plan : nullptr);
    });
    if (!chain.ok()) {
        // BU-Q9: a form the native compile declines goes to the query layer,
        // whose refusal stands only if its grammar reads the text.
        if (!analyze && !line.empty() && chain.status().code() == StatusCode::kNotImplemented) {
            if (auto layered = TryQueryLayer(line, session, snap, context); layered.has_value()) {
                return std::move(*layered);
            }
        }
        return {ErrorReply(chain.status()), false, 0, chain.status()};
    }

    // **Every read runs where the session is, walking the whole relation** (AT-S9,
    // AT-0 item 4 answered "struck"): one frame table serves every core
    // (AM-S2 step 3), so every page is this core's to fault, and a walk
    // here reaches the whole relation.
    // The fan-in and the two-step join pipeline retired (AT-S9); their
    // history is at cf889b63.

    // Same one-line-per-response contract as SHOW PAGE: a header line of
    // column names, then one "\n"-escaped section per matching row
    // (comma-joined values), never a raw newline byte - which is what
    // `TextResultSink` writes, and what a caller that installed its own
    // sink gets instead.
    //
    // **The description is not emitted here**: a fold's output columns are
    // typed by its items and a projection's by its columns, and the fork
    // between them is below.
    // Where this statement's rows go (`server/result_sink.hpp`): the
    // session's sink when one is installed, and the newline protocol's
    // rendering otherwise. Built here, above every fork below, because the
    // fold, the sorted drain and the plain walk all emit through it - and
    // it *is* the reply buffer, so there is no second one.
    TextResultSink& text_sink = run->text_sink;
    ResultSink& sink = SinkFor(session, text_sink);
    run->sink = &sink;

    // Resolved once, outside the row loop: the projection reads the frame
    // by index, and `SELECT *` means every column of the one step - which
    // the grammar admits only for a single relation (V06).
    const exec::StepChain& compiled = run->chain.emplace(std::move(chain.value()));

    // ---- Waystone: the instance, taken from the parse -------------------
    //
    // Free now: the lexer accumulated it while the parser walked the tokens
    // (lexer.hpp), so identifying the statement costs nothing beyond the
    // parse that had to happen anyway.
    //
    // **The guard is the chain's own shape.** A chain with no lookup/probe
    // step can neither record nor replay - invariant 9 forbids a trail
    // replacing a search - so a scan-only statement skips the catalog
    // lookup and the trail read entirely.
    const bool waystone_usable =
        (recorder_ != nullptr || replay_enabled_) && exec::HasReplayableStep(compiled);

    // **A D2 statement neither records a trail nor reads one** (AR1 §3,
    // D2): a row-volatile function makes its instance key name no stable
    // set of rows. It keeps its identity, so the cabin optimizer still
    // counts it; only the trail is withheld. D1 is D0 here until AQ folds
    // its values into the key (AP-Q5).
    const bool trailable = compiled.determinism != exec::DeterminismClass::kD2;

    std::optional<StatementIdentity>& identity = run->identity;
    // The optimizer's S1 widens this beyond Waystone's shape guard, and the
    // difference is the point: a *scan-only* statement is exactly the shape
    // whose decayed frequency the cabin optimizer's CREATE decision prices
    // (physical-optimizer.md §II.4's f_i), and it is the one shape
    // invariant 9 keeps Waystone away from. The trail and replay reads
    // below still guard on their own switches, so deriving the identity
    // here costs nothing they did not already pay.
    if (run->parser.has_value() && (waystone_usable || optimizer_signals_ != nullptr)) {
        if (auto fingerprint = run->parser->fingerprint(); fingerprint.has_value()) {
            identity = StatementIdentity{
                stats::InstanceKey{fingerprint->fetch_id, fingerprint->arg_hash},
                fingerprint->pattern_id};
        }
    }

    // The trail a previous execution of this instance recorded. Read once,
    // indexed once, consulted per keyed step.
    //
    // The index is pooled scratch, reused rather than rebuilt: it is the
    // only allocation on the replay path, and one malloc per SELECT for
    // what is usually one or two entries is the same cost the collector
    // already had to be hoisted to avoid.
    exec::TrailReplay& replay_scratch = run->scratch().replay;
    replay_scratch.Clear();
    const exec::TrailReplay* replay_ptr = nullptr;
    if (replay_enabled_ && trailable && identity.has_value()) {
        // Served from the catalog cache, so a pattern nobody has recorded
        // costs a hash lookup and stops here. `has_waystone_directory()` is
        // the authority on whether there is anything to walk (rows.hpp).
        if (auto pattern = catalog_.FindPattern(identity->trail.fetch_id);
            pattern.ok() && pattern.value()->has_waystone_directory()) {
            auto entries = stats::ReadTrail(page_store_, pattern.value()->waystone_root,
                                            pattern.value()->dir_depth, identity->trail);
            // A trail that cannot be read is a trail that does not exist:
            // the statement descends, exactly as it did before there were
            // trails at all (invariant 8).
            if (entries.ok() && !entries.value().empty()) {
                replay_scratch.Build(compiled, entries.value());
                if (!replay_scratch.empty()) replay_ptr = &replay_scratch;
            }
        }
    }

    // The trail this execution leaves, if anything is recording.
    //
    // **Reused, not constructed per statement**: a collector reserves room
    // for a whole trail (253 x 32 bytes), an 8 KB malloc per SELECT
    // otherwise. Pooled scratch since BA-S15; Clear() keeps the reservation.
    exec::TrailCollector* trail = nullptr;
    if (recorder_ != nullptr && trailable && identity.has_value()) {
        run->scratch().trail.Clear();
        trail = &run->scratch().trail;
    }
    run->trail = trail;
    run->replay = replay_ptr;

    // ANALYZE runs everything above, deliberately. Its whole contract is
    // that the run it describes is the run that actually happened - same
    // parse, same compile, same executor - and a diagnostic that skipped
    // replay would report descents a real execution does not perform, which
    // is the one thing it must not do.
    if (analyze) {
        return RunAnalyze(compiled, trail, replay_ptr, identity, snap, borrow,
                          run->scratch().sorter);
    }

    // ---- AG1: the fold wraps the sink, and nothing else moves -----------
    //
    // Everything above this point ran unchanged and unconditionally for an
    // aggregated statement: the compile, the affinity check, the Waystone
    // lookup, the trail collector. The fold is strictly downstream of all
    // of them, which is what makes AG10's "recording, replay, Cabin probes
    // and access statistics hold unchanged" a structural fact rather than a
    // list of things that were remembered.
    if (compiled.aggregated()) {
        return RunAggregated(sink, text_sink, compiled, trail, replay_ptr, identity, snap, borrow,
                             run->scratch().stats);
    }

    // ---- V09: the emission quota wraps the sink, and nothing else moves --
    //
    // The same seam as the fold above and with the same consequence: the
    // compile, the affinity check, the Waystone lookup and the trail
    // collector all ran unchanged, and the quota is strictly downstream of
    // them. `kEmitThenStop` rides `RowSink`'s kStop (V03), so the walk
    // stops on the very tuple that filled the quota and fetches no further
    // page - early termination is the existing stop propagation.
    // ---- OB4: the sort wraps the sink above the quota -------------------
    //
    // The same seam again, with one difference the fold and the quota did
    // not have: **the quota runs downstream of the sort**, after the walk,
    // because rows [m, m+n) of the sorted reply are not rows [m, m+n) of
    // the emitted one. A chain never carries both a fold and a sort - the
    // parser refuses the tail over aggregated output.
    run->quota.emplace(compiled);
    run->scratch().sorter.Reset(compiled, sort_max_rows_);
    run->scratch().stats.steps.clear();

    // `SELECT *` renders from the relation's schema, which the chain
    // deliberately does not carry types for. Resolved **once**, not per row:
    // this is the commonest statement shape in the engine, and a catalog
    // lookup per row was 50-63 ns of it (`bench/results-order-by.md`).
    const catalog::TableAccess* star_access = nullptr;
    if (compiled.star()) {
        auto access = catalog_.InitTableAccess(compiled.steps[0].rel_oid);
        if (!access.ok()) return {ErrorReply(access.status()), false, 0, access.status()};
        star_access = access.value();
    }

    // **A star read is a projection, resolved once**, so exactly one row
    // path below formats both shapes.
    StarDescription& star = run->star;
    if (star_access != nullptr) star = DescribeStar(star_access->schema.columns);
    const std::span<const exec::ColumnRef> projection =
        star_access != nullptr ? std::span<const exec::ColumnRef>(star.projection)
                               : std::span<const exec::ColumnRef>(compiled.projection);
    const std::span<const std::uint32_t> types =
        star_access != nullptr ? std::span<const std::uint32_t>(star.types)
                               : std::span<const std::uint32_t>(compiled.projection_types);
    const std::span<const std::uint32_t> type_mods =
        star_access != nullptr ? std::span<const std::uint32_t>(star.type_mods)
                               : std::span<const std::uint32_t>(compiled.projection_type_mods);

    // The FROM relation's slot: its step id is the lowest, its written
    // position, wherever the join planner runs it (CB-Q3).
    const auto from_step = std::min_element(
        compiled.steps.begin(), compiled.steps.end(),
        [](const exec::Step& a, const exec::Step& b) { return a.step_id < b.step_id; });
    const auto from_slot = static_cast<std::uint16_t>(from_step - compiled.steps.begin());
    if (Status described = sink.Describe(
            DescribeFields(compiled.column_names, types, type_mods, projection, from_slot));
        !described.ok()) {
        return {"ERR " + described.message(), false, 0, described};
    }

    run->projection = projection;
    run->types = types;

    // ---- BA-S15: the walk, handed on or run here -------------------------
    //
    // Handed on where the statement may park (`DispatchAsync`), which runs
    // it sliced after this head returns; a traced statement keeps its walk
    // inside the request span it is timing, and every synchronous caller has
    // nowhere to park. The hold and the pin are taken here, while this head
    // still publishes the word it revalidated against.
    //
    // **A tuple key descent is never handed on**: it reads one leaf and cannot fill
    // a slice, so it would pay the hand-off - a frame, a hold, a pin - for
    // a yield it never reaches.
    const exec::AccessKind outer = compiled.steps.front().kind;
    const bool descends = outer == exec::AccessKind::kLookup || outer == exec::AccessKind::kProbe;
    if (!may_park_ || descends) return run->RunInline();
    run->HoldAcrossSuspension();
    DispatchOutcome handed;
    handed.select_run = run;
    return handed;
}

void CommandDispatcher::RecordExecution(const std::optional<StatementIdentity>& identity,
                                        exec::TrailCollector* trail,
                                        const exec::StepChain& chain,
                                        const exec::ExecStats& stats) {
    RecordTrail(identity, trail, chain);
    RecordAccessShapes(chain);
    RecordOptimizerSignals(identity, chain, stats);
    RecordPlanStats(chain, stats);
}

void CommandDispatcher::RecordPlanStats(const exec::StepChain& chain,
                                        const exec::ExecStats& stats) {
    if (plan_stats_ == nullptr || chain.planned_relations.empty()) return;

    // A relation the snapshot has no prior for is wanted by the steward -
    // from every searched block, a stopped run's and a large block's too.
    const std::shared_ptr<const stats::PlanStatsSnapshot> snapshot = plan_source_.Pin();
    std::array<catalog::Oid, exec::kMaxPlannedRelations> wanted{};
    std::size_t nwanted = 0;
    for (const catalog::Oid oid : chain.planned_relations) {
        if ((snapshot == nullptr || !snapshot->RelationRows(oid).has_value()) &&
            nwanted < wanted.size()) {
            wanted[nwanted++] = oid;
        }
    }
    plan_stats_->WantPriors(core_id_, std::span<const catalog::Oid>(wanted.data(), nwanted));

    // A keyed block's complete run only: a stopped run counted a prefix.
    if (!chain.planned.has_value() || stats.stopped) return;
    const stats::PlanKey& key = *chain.planned;
    std::uint32_t base = chain.steps.front().step_id;
    for (const exec::Step& step : chain.steps) base = std::min(base, step.step_id);

    // card(T) after each executed slot, T the written positions so far, and
    // an annotated build that ran and did not publish as a decline (PS2,
    // PS3, PS11). A build's own `build_rows` is not folded: it leaves out
    // NULL-keyed rows, so it is not the same fact as card({w}).
    stats::PlanObservation observation;
    observation.key = key;
    std::uint16_t mask = 0;
    for (const exec::Step& step : chain.steps) {
        const std::uint32_t written = step.step_id - base;
        if (written >= key.n || step.step_id >= stats.steps.size()) return;
        const exec::StepStats& counts = stats.steps[step.step_id];
        mask = static_cast<std::uint16_t>(mask | (1u << written));
        observation.rows[mask] = counts.rows_matched;
        observation.present = static_cast<std::uint16_t>(observation.present | (1u << mask));
        if (step.build.has_value() && counts.relation_opens > 0 && counts.inner_builds == 0 &&
            budget_.join_build_max_rows() > 0) {
            observation.declines = static_cast<std::uint8_t>(observation.declines | (1u << written));
        }
    }
    plan_stats_->Fold(core_id_, observation);
}

void CommandDispatcher::RecordAccessShapes(const exec::StepChain& chain) {
    // Unconditional on a successful SELECT, and independent of Waystone:
    // this is the physical optimizer's input (docs/spec/heap-and-tuple.md §7),
    // not a trail, and it is collected whether or not anything is recording
    // or replaying one.
    // **The instance's one switch decides it on every core** (AT-S7):
    // every core writes the relation itself, so `access_statistics` means
    // the same thing wherever the statement ran.
    if (!access_stats_enabled_) return;
    stats::RecordChainAccess(catalog_, chain, static_cast<std::uint64_t>(NowNs()),
                             &access_counters_);
}

void CommandDispatcher::NoteCabinWrite(const catalog::TableAccess& access,
                                        std::span<const parser::AstValue> values,
                                        std::uint16_t first_col_pos, std::uint64_t tuple_key,
                                        PageId page_id, std::uint16_t slot,
                                        std::span<const parser::AstValue> previous) {
    // The test a relation with no Cabin pays, and nothing else.
    if (!CabinsWitness(access)) return;

    // Filled on the first entry actually appended (see below).
    std::optional<std::uint32_t> write_epoch;
    const auto append = [&](const stats::CabinKey& key) {
        stats::CabinEntry entry;
        entry.tuple_key = tuple_key;
        entry.page_id = page_id;
        // The page's current epoch, fetched lazily - once per statement
        // that actually appends, and only then - so a relation whose write
        // touches no cabined column pays nothing new. A buffer hit: the
        // page was written by this very statement moments ago. Through the
        // one producer both heal sites use, so a hint minted here and a
        // hint repaired there carry the same stamp by construction.
        if (!write_epoch.has_value()) {
            write_epoch = exec::CurrentRelayoutEpoch(page_store_, page_id);
        }
        entry.page_epoch = *write_epoch;
        entry.slot = slot;
        entry.flags = stats::kCabinHintValid;
        cabins_->NoteWrite(key, entry);
    };

    for (std::uint16_t col = 1; col < 64; ++col) {
        if ((access.cabin_mask & (std::uint64_t{1} << col)) == 0) continue;
        if (col < first_col_pos) continue;
        const std::size_t at = static_cast<std::size_t>(col - first_col_pos);
        if (at >= values.size()) continue;

        // **Coerced first, and this is load-bearing.** `values` holds the
        // literals as written, so a DATE column's value here is still the
        // string `'2026-08-07'` while everything that *reads* this Cabin
        // keys on the epoch integer the compiler produced. Keying on the
        // raw literal put the append under a key no read ever looks up:
        // the value stayed observed, its set stopped growing, and queries
        // returned rows that existed before the observation and silently
        // dropped every row inserted after it.
        //
        // Through the codec's shared coercion, so the write key and the
        // read key are produced by one routine rather than by two that
        // agree today.
        const catalog::SysColumnRow& column = access.schema.columns[col];
        parser::AstValue value = values[at];
        if (Status s = exec::CoerceLiteralToColumn(column, value); !s.ok()) {
            // Unreachable: encode is the only gate (spec §7) and it already
            // accepted this row, so a literal that reaches here parses. If
            // that ever stops being true, un-observing is the right answer
            // and always legal (cabin.md §1) - a set that might have
            // missed an append is not a superset, and serving it would lose
            // a row.
            // `Discard`, not `Unobserve` of the uncoerced key, which no
            // reader looks up, nor `Forget`, which drops an in-flight
            // recording's mark and lets its walk bank a set missing this row.
            cabins_->Discard(access.CabinOn(col).id);
            continue;
        }

        // **§5's third row: an UPDATE that did not touch the key column does
        // nothing.** Appending anyway would still be *correct* - the entry
        // set stays a superset, and the read dedupes - but it is unbounded:
        // a workload that updates a row's other columns repeatedly would
        // grow that value's set by one entry per write forever, until the
        // per-value cap un-observed it and the Cabin stopped serving the
        // very relation it was declared for. Correct and useless is still a
        // defect, so the comparison is made here rather than left to the
        // cap.
        //
        // The comparison uses the column's own `type_val` and the coerced
        // value: against the raw literal a decoded date never compared
        // equal to the string it was written as, so this check silently
        // never fired for a typed column and every write appended.
        if (at < previous.size() &&
            exec::CompareValues(column.type_val, previous[at], value, parser::CompareOp::kEq)) {
            continue;
        }

        auto key = stats::MakeCabinKey(access.CabinOn(col).id, value);
        // A value that can never be observed - NULL, an unbound param -
        // cannot have a set to append to either, so there is nothing to
        // witness. Silent, exactly as it is on the read path.
        if (!key.has_value()) continue;
        append(*key);
    }

    // ---- Expression and predicate Cabins (AQ-R4, `[quiet-wrong]`) -------
    //
    // AR1 §10's witness for a key function `F`: evaluate `F` on the new row
    // and append under its key. **A write that skips this leaves a set
    // missing a row**, so every write site that reaches the column loop
    // above reaches this too - the two callers, INSERT and UPDATE, are the
    // whole surface.
    if (access.expr_cabins.empty()) return;

    // `F` reads its inputs by schema position, coerced to their columns'
    // types - the same coercion the column arm above makes, so a key on the
    // write side is the value the read side compares. Only inputs are
    // filled; the tuple key is in hand where the row body leaves it out.
    std::vector<parser::AstValue> row(access.schema.columns.size());
    const auto input = [&](std::uint16_t pos) -> bool {
        if (pos < first_col_pos) {
            row[pos].type = parser::ValueType::kInt;
            row[pos].int_val = static_cast<std::int64_t>(tuple_key);
        } else {
            const std::size_t at = static_cast<std::size_t>(pos - first_col_pos);
            if (at >= values.size()) return false;
            row[pos] = values[at];
            if (!exec::CoerceLiteralToColumn(access.schema.columns[pos], row[pos]).ok()) {
                return false;
            }
        }
        return true;
    };
    for (const catalog::TableAccess::ExprCabinRef& ref : access.expr_cabins) {
        // An `F` the loader did not fill is one no statement compiled a
        // probe for, so nothing observed under it (`Catalog::SetDeclarationLoader`).
        if (ref.f == nullptr) continue;
        bool readable = true;
        bool changed = previous.empty();
        for (const std::uint16_t pos : ref.f->inputs) {
            if (!input(pos)) {
                readable = false;
                break;
            }
            // §5's third row, on `F`'s inputs (AR1 §10): an UPDATE that moved
            // none of them leaves every key where it was.
            if (!changed && (pos >= previous.size() ||
                             !exec::CompareValues(access.schema.columns[pos].type_val,
                                                  previous[pos], row[pos],
                                                  parser::CompareOp::kEq))) {
                changed = true;
            }
        }
        if (!readable) {
            // Unreachable for the column arm's reason: encode accepted this
            // row. A key that cannot be computed cannot be witnessed, so
            // every value of the Cabin is un-observed - always legal (§1) -
            // through `Discard`, which also refuses an in-flight recording's
            // commit, where `Forget` would let it bank a set missing this row.
            cabins_->Discard(ref.id);
            continue;
        }
        if (!changed) continue;
        const std::optional<parser::AstValue> value = exec::CabinKeyOf(*ref.f, row);
        if (!value.has_value()) continue;
        if (auto key = stats::MakeCabinKey(ref.id, *value); key.has_value()) append(*key);
    }
}

void CommandDispatcher::RecordTrail(const std::optional<StatementIdentity>& identity,
                                     exec::TrailCollector* trail,
                                     const exec::StepChain& chain) {
    // Never on the failure path - both callers reach here only after the
    // execution succeeded - and only when something was actually located: a
    // collector that gathered nothing describes no trail, and writing an
    // empty one would replace a populated trail with nothing.
    if (recorder_ == nullptr || trail == nullptr || trail->empty()) return;
    if (!identity.has_value()) return;
    recorder_->OnPatternResult(identity->trail, *trail, exec::StoredStatementClass(chain.klass));
}

void CommandDispatcher::RecordOptimizerSignals(const std::optional<StatementIdentity>& identity,
                                               const exec::StepChain& chain,
                                               const exec::ExecStats& stats) {
    // Success path only, like its two siblings, and only for a statement
    // with an identity - the fingerprint is the key the cost-benefit model
    // aggregates by, so a statement without one has nowhere to be counted.
    if (optimizer_signals_ == nullptr || !identity.has_value()) return;

    // The shape's cabin candidacy (§II.4's Σ_i linkage): a kCabinProbe step
    // on a column Cabin names it outright; otherwise the first kFilterScan's
    // equality column is the column a Cabin *would* serve - the shape whose
    // decayed frequency prices a CREATE. A chain with neither has no
    // candidacy and is recorded as pure heat. An expression Cabin is not a
    // column's and the controller prices columns only (AQ-Q5), so its probe
    // names no candidate.
    stats::CandidateRef candidate;
    // In written-position order (CC-Q15): the step with the lowest id of
    // each arm, which for a planned block is the written order's first, not
    // the executed one's. The planner's order still decides each step's
    // kind, so a reorder can change which arm a relation takes.
    const exec::Step* cabin_step = nullptr;
    const exec::Step* filter_step = nullptr;
    for (const exec::Step& step : chain.steps) {
        if (step.kind == exec::AccessKind::kCabinProbe && step.cabin.has_value() &&
            step.cabin->f == nullptr) {
            if (cabin_step == nullptr || step.step_id < cabin_step->step_id) cabin_step = &step;
        }
        // **The lowest non-tuple-key column of an equality in the residual**
        // (AQ-R7) - `access_columns`, sorted - a deterministic pick rather
        // than a claim of primacy. Not `filter_columns`, which holds a
        // function's arguments since AP-S4: `DATE(ts) = d` priced as a column
        // Cabin on `ts` would build one no such statement probes. Below 64,
        // the columns a Cabin's mask can carry (`TableAccess::cabin_mask`).
        if (step.kind == exec::AccessKind::kFilterScan && !step.access_columns.empty() &&
            step.access_columns.front() < 64) {
            if (filter_step == nullptr || step.step_id < filter_step->step_id) filter_step = &step;
        }
    }
    if (cabin_step != nullptr) {
        candidate.rel_oid = cabin_step->rel_oid;
        candidate.col_pos = cabin_step->cabin->col_pos;
        candidate.cabin_id = cabin_step->cabin->cabin_id;
    } else if (filter_step != nullptr) {
        candidate.rel_oid = filter_step->rel_oid;
        candidate.col_pos = filter_step->access_columns.front();
    }
    optimizer_signals_->NoteExecution(identity->pattern_id, stats.Total().pages_fetched,
                                      candidate);
}

DispatchOutcome CommandDispatcher::HandleUpdate(std::string_view line, Session& session) {
    WriteScope scope;
    txn::Snapshot snap;
    WalkCursor resume_from;
    exec::StatementContext context;  // NOW()'s instant, carried across a park (AP-S4)
    std::vector<exec::SubqueryResult> subquery_answers;  // BJ-S7, carried the same way

    // **A resume re-enters the scope and the view it already had** (AO-S3b):
    // the rows already written live in that transaction, and a second
    // snapshot would show one statement two states of the database.
    if (session.parked_write().has_value() && !session.parked_write()->is_delete) {
        const Session::ParkedWrite& parked = *session.parked_write();
        scope = WriteScope{parked.txn, parked.owned, &session};
        snap = parked.snapshot;
        resume_from = parked.cursor;
        // R6-5's mark belongs to the *statement*, and the resume is the
        // same statement - so it is restored rather than re-taken, which
        // would say this statement had written nothing.
        statement_trail_mark_ = parked.trail_mark;
        context = parked.context;
        subquery_answers = parked.subqueries;
        // **And so is its statement boundary** (BH-S1, Census A row 5). The
        // re-dispatch cleared it, and a READ COMMITTED transaction would
        // otherwise re-mint its registered view at the next `ViewFor`
        // while this walk goes on reading at `snap` - a view the horizon no
        // longer sees, so a `PURGE` on another core could retire a row
        // this statement's snapshot is still entitled to.
        statement_boundary_taken_ = true;
        session.clear_parked_write();
    } else {
        auto opened = BeginWrite(session);
        if (!opened.ok()) return {ErrorReply(opened.status()), false, 0, opened.status()};
        scope = opened.value();
        context = StatementContextFor(session);

        // The read view this UPDATE filters through. An UPDATE reads before
        // it writes, and it must not see a row a SELECT in the same
        // transaction would not - so it takes the snapshot the same way.
        //
        // **After `BeginWrite`, and the order is a correctness fact** (BH-S1,
        // Census A row 4): an autocommit `snap` is a copy whose lease
        // `SnapshotFor` drops before the walk, so what keeps the horizon at
        // or below it - and a `PURGE` on another core off a row it can see
        // - is the owned transaction `BeginWrite` registered first, which
        // stays live until `EndWrite`. Taken the other way round, the walk
        // would read at a snapshot no reader registration covers.
        auto snapshot = SnapshotFor(session);
        // `ErrorReply`, not a bare "ERR ": `SnapshotFor` can refuse, and the
        // wire's `retryable=1` is what a client's retry loop reads.
        if (!snapshot.ok()) return {ErrorReply(snapshot.status()), false, 0, snapshot.status()};
        snap = snapshot.value().snap;
    }

    DispatchOutcome out = UpdateInner(line, scope, snap, resume_from, context, subquery_answers);

    // **Parked mid-walk, the scope stays open** (AO-S3b): the statement has
    // written rows and there is no savepoint, so its transaction, view and
    // position go to the session for the branch at the top to resume.
    if (out.parked_mid_walk) {
        session.set_parked_write(Session::ParkedWrite{scope.txn, scope.owned, snap,
                                                      out.walk_cursor, statement_trail_mark_,
                                                      /*is_delete=*/false, context,
                                                      std::move(subquery_answers)});
        return out;
    }
    out = WithReturning(line, session, scope, std::move(out));


    const bool failed = out.response.rfind("ERR ", 0) == 0;
    Status verdict = failed ? Status::InvalidArgument(out.response) : Status::OK();
    if (Status s = EndWrite(session, scope, verdict); !s.ok() && !failed) {
        return {ErrorReply(s), false, 0, s};
    }
    return out;
}

namespace {

// What a scalar subquery's rows land in: the first column of at most one row.
class ScalarSubquerySink final : public ResultSink {
public:
    Status Describe(std::vector<wire::FieldDescription> fields) override {
        if (fields.size() != 1) {
            return Status::InvalidArgument("a subquery used as an expression must return one "
                                           "column, and this one returns " +
                                           std::to_string(fields.size()));
        }
        field_ = std::move(fields[0]);
        return Status::OK();
    }
    // The row's text is the index of its value, so what `Emit` is handed is the
    // value that was encoded for it: a sorted statement encodes rows as the top-N
    // heap admits them and emits the survivors afterwards, in order, and "the
    // last row encoded" is not the row emitted.
    Status EncodeProjectedRow(std::span<const exec::ColumnRef> projection,
                              std::span<const std::uint32_t>, const exec::ChainFrame& frame,
                              std::string& out) override {
        rendered_.push_back(projection.empty() ? parser::AstValue{} : frame.Get(projection[0]));
        out = std::to_string(rendered_.size() - 1);
        return Status::OK();
    }
    Status EncodeValueRow(std::span<const std::uint32_t>, std::span<const parser::AstValue> values,
                          std::string& out) override {
        rendered_.push_back(values.empty() ? parser::AstValue{} : values[0]);
        out = std::to_string(rendered_.size() - 1);
        return Status::OK();
    }
    Status Emit(std::string_view row) override {
        if (++rows_ > 1) {
            return Status::InvalidArgument(
                "more than one row returned by a subquery used as an expression");
        }
        value_ = std::move(rendered_[static_cast<std::size_t>(std::stoull(std::string(row)))]);
        return Status::OK();
    }

    const wire::FieldDescription& field() const noexcept { return field_; }
    parser::AstValue TakeValue() { return std::move(value_); }

private:
    wire::FieldDescription field_;
    std::uint64_t rows_ = 0;
    std::vector<parser::AstValue> rendered_;
    parser::AstValue value_;  // NULL until a row arrives
};

}  // namespace

StatusOr<exec::SubqueryResult> CommandDispatcher::RunScalarSubquery(const parser::Expr& node,
                                                                    const parser::UpdateStmt& stmt,
                                                                    WriteScope& scope,
                                                                    const txn::Snapshot& snapshot,
                                                                    const exec::StatementContext& context) {
    const std::string at = " (byte " + std::to_string(node.byte_offset) + ")";
    Session& session = *scope.session;

    // **Correlated or not**: a subquery that compiles on its own names nothing
    // outside it. One that does not may name the relation being updated, and
    // the SELECT compiler says so by compiling it as an `EXISTS` over that
    // relation, where a reference that resolves outward makes its sub-chain
    // correlated. Both are compiles of the parsed statement; neither runs.
    // A subquery that fails both is left for the real run to refuse in its own
    // words.
    if (Status correlated = ResolveNames(session, [&](const txn::ReadView* resolve_view) {
            if (exec::Compile(catalog_, *node.subquery, resolve_view, nullptr, &context).ok()) {
                return Status::OK();
            }
            parser::SelectStmt probe;
            probe.from.schema = stmt.schema;
            probe.from.table_name = stmt.table_name;
            parser::Condition exists;
            exists.kind = parser::PredicateKind::kExists;
            exists.subquery = node.subquery;
            probe.where.push_back(std::move(exists));
            auto chain = exec::Compile(catalog_, probe, resolve_view, nullptr, &context);
            if (!chain.ok()) return Status::OK();
            for (const exec::Step& step : chain.value().steps) {
                for (const exec::SubChain& sub : step.sub_chains) {
                    if (sub.correlated) {
                        return Status::NotImplemented(
                            "a subquery that reads the row being updated is not supported "
                            "in a SET value yet (BJ-Q6)" + at);
                    }
                }
            }
            return Status::OK();
        });
        !correlated.ok()) {
        return correlated;
    }

    ScalarSubquerySink sink;
    ResultSink* const previous = session.result_sink();
    session.set_result_sink(&sink);
    struct Restore {
        Session& session;
        ResultSink* sink;
        ~Restore() { session.set_result_sink(sink); }
    } restore{session, previous};

    // The text is padded to where it stood in the statement, so every byte the
    // SELECT reports is a byte of the UPDATE.
    const std::string text = std::string(node.byte_offset + 1, ' ') + node.subquery_sql;
    DispatchOutcome out = HandleSelect(text, session, /*analyze=*/false, &snapshot, &context);
    // The walk is handed on for a yield; a SET value's subquery is run to its
    // end here, before the statement's own walk begins.
    if (out.select_run != nullptr) {
        const std::shared_ptr<SelectRun> run = std::move(out.select_run);
        out = run->RunInline();
    }
    // The subquery's own refusals carry their byte already; the rest are given
    // the subquery's.
    const auto Positioned = [&](const std::string& what) {
        return what.find("byte ") != std::string::npos ? what : what + at;
    };
    if (!out.status.ok()) return out.status.WithMessage(Positioned(out.status.message()));
    if (out.response.rfind("ERR", 0) == 0) {
        return Status::InvalidArgument(Positioned(out.response.substr(4)));
    }

    const wire::FieldDescription& field = sink.field();
    if (field.type_oid == 0) {
        return Status::InvalidArgument("the subquery's column has no type to assign" + at);
    }
    exec::SubqueryResult result;
    result.type.kind = exec::SetType::Kind::kConcrete;
    result.type.type_val = field.type_oid == catalog::kTypeValChar ? catalog::kTypeValVarchar
                                                                    : field.type_oid;
    result.type.len = field.type_mod;
    result.value = sink.TakeValue();
    result.value.byte_offset = node.byte_offset;
    return result;
}

// ---- The query layer's door (BU-S4, BU-Q8, BU-Q9) ---------------------------

namespace {

// A layer walk's rows, captured as values. The row's text is the index of
// its values, `ScalarSubquerySink`'s device, so what `Emit` keeps is the row
// that was encoded for it. Bounded as the layer's own row sets are (BU-Q10):
// the walk is refused at the row past `max_rows`, not held whole first.
class LayerWalkSink final : public ResultSink {
public:
    explicit LayerWalkSink(std::size_t max_rows) : max_rows_(max_rows) {}
    Status Describe(std::vector<wire::FieldDescription>) override { return Status::OK(); }
    Status EncodeProjectedRow(std::span<const exec::ColumnRef> projection,
                              std::span<const std::uint32_t>, const exec::ChainFrame& frame,
                              std::string& out) override {
        std::vector<parser::AstValue> row;
        row.reserve(projection.size());
        for (const exec::ColumnRef& ref : projection) row.push_back(frame.Get(ref));
        rendered_.push_back(std::move(row));
        out = std::to_string(rendered_.size() - 1);
        return Status::OK();
    }
    Status EncodeValueRow(std::span<const std::uint32_t>, std::span<const parser::AstValue> values,
                          std::string& out) override {
        rendered_.emplace_back(values.begin(), values.end());
        out = std::to_string(rendered_.size() - 1);
        return Status::OK();
    }
    Status Emit(std::string_view row) override {
        if (rows_.size() >= max_rows_) {
            return Status::ResourceExhausted(
                "this statement's relation walk would hold more than sort_max_rows (" +
                std::to_string(max_rows_) +
                ") rows; the query layer answers from rows it holds (BU-Q10) - raise "
                "sort_max_rows, or narrow the statement");
        }
        rows_.push_back(std::move(rendered_[static_cast<std::size_t>(std::stoull(std::string(row)))]));
        return Status::OK();
    }
    std::vector<std::vector<parser::AstValue>> TakeRows() { return std::move(rows_); }

private:
    std::size_t max_rows_;
    std::vector<std::vector<parser::AstValue>> rendered_;
    std::vector<std::vector<parser::AstValue>> rows_;
};

}  // namespace

// What the layer asks of the dispatcher: a relation's description and its
// walks, every one at the statement's snapshot.
class CommandDispatcher::LayerHostImpl final : public exec::LayerHost {
public:
    LayerHostImpl(CommandDispatcher& d, Session& session, const txn::Snapshot& snap,
                  const exec::StatementContext& context)
        : d_(d),
          session_(session),
          snap_(snap),
          context_(context),
          borrow_(d.locks_, d.NextReadHolder(), &d.read_borrows_) {}

    StatusOr<std::vector<exec::LayerColumn>> DescribeRelation(
        const parser::RelationRef& rel) override {
        if (IEquals(rel.schema, exec::kCatalogSchema)) {
            return Status::NotImplemented("a catalog view in this query form is not supported (byte " +
                                          std::to_string(rel.byte_offset) +
                                          "); sys.* rows are produced by the catalog's readers");
        }
        parser::SelectStmt probe;
        probe.from = rel;
        // Resolved as the native path resolves a name, and declared into the
        // statement's one borrow, held until the statement ends (AT-S1): a
        // `DROP TABLE` waits for the layer's walks, as for a native read's.
        auto chain = d_.ResolveNames(session_, [&](const txn::ReadView* v) {
            return exec::Compile(d_.catalog_, probe, v, &borrow_, &context_);
        });
        if (!chain.ok()) return chain.status();
        auto access = d_.catalog_.InitTableAccess(chain.value().steps[0].rel_oid);
        if (!access.ok()) return access.status();
        std::vector<exec::LayerColumn> out;
        const auto& columns = access.value()->schema.columns;
        for (std::size_t i = 0; i < columns.size(); ++i) {
            // A dropped column (BW) and a heap's hidden tuple key (BZ-R3)
            // are in no row a walk emits, so they are no column of the
            // layer's either.
            if (!columns[i].visible()) continue;
            exec::LayerColumn c;
            c.name = std::string(catalog::NameView(columns[i].name));
            c.type.kind = exec::SetType::Kind::kConcrete;
            c.type.type_val = columns[i].type_val;
            c.type.len = columns[i].len;
            c.keystone = i == 0;
            out.push_back(std::move(c));
        }
        return out;
    }

    StatusOr<exec::RowSet> WalkRelation(const parser::RelationRef& rel,
                                        const std::vector<parser::Condition>& where) override {
        parser::SelectStmt stmt;
        stmt.from = rel;
        stmt.where = where;
        auto rows = d_.WalkForLayer(std::move(stmt), session_, snap_, context_);
        if (!rows.ok()) return rows.status();
        exec::RowSet out;
        out.rows = std::move(rows.value());
        return out;
    }

private:
    CommandDispatcher& d_;
    Session& session_;
    const txn::Snapshot& snap_;
    const exec::StatementContext& context_;
    ReadBorrow borrow_;
};

StatusOr<std::vector<std::vector<parser::AstValue>>> CommandDispatcher::WalkForLayer(
    parser::SelectStmt stmt, Session& session, const txn::Snapshot& snap,
    const exec::StatementContext& context) {
    LayerWalkSink sink(sort_max_rows_);
    ResultSink* const previous = session.result_sink();
    session.set_result_sink(&sink);
    struct Restore {
        Session& session;
        ResultSink* sink;
        ~Restore() { session.set_result_sink(sink); }
    } restore{session, previous};

    const auto run = std::make_shared<SelectRun>(*this, session);
    run->snapshot.emplace(txn::LeasedSnapshot{snap, {}});
    run->statement.emplace(std::move(stmt));
    DispatchOutcome out = SelectBody(run, session, /*analyze=*/false, &context, {});
    if (out.select_run != nullptr) {
        const std::shared_ptr<SelectRun> handed = std::move(out.select_run);
        out = handed->RunInline();
    }
    if (!out.status.ok()) return out.status;
    if (out.response.rfind("ERR", 0) == 0) return Status::InvalidArgument(out.response.substr(4));
    return sink.TakeRows();
}

DispatchOutcome CommandDispatcher::WithReturning(std::string_view line, Session& session,
                                                 WriteScope& scope, DispatchOutcome out) {
    if (out.response.rfind("ERR", 0) == 0 || !ContainsWordIgnoringCase(line, "RETURNING")) {
        return out;
    }
    parser::Parser parser(line);
    auto parsed = parser.Parse();
    if (!parsed.ok()) return out;
    const parser::Returning* returning = nullptr;
    std::string schema;
    std::string table;
    txn::TrailAction wrote = txn::TrailAction::kInsert;
    if (const auto* ins = std::get_if<parser::InsertStmt>(&parsed.value())) {
        returning = &ins->returning;
        schema = ins->schema;
        table = ins->table_name;
    } else if (const auto* upd = std::get_if<parser::UpdateStmt>(&parsed.value())) {
        returning = &upd->returning;
        schema = upd->schema;
        table = upd->table_name;
        wrote = txn::TrailAction::kOverwrite;
    } else if (const auto* del = std::get_if<parser::DeleteStmt>(&parsed.value())) {
        returning = &del->returning;
        schema = del->schema;
        table = del->table_name;
        wrote = txn::TrailAction::kDeleteMark;
    }
    if (returning == nullptr || returning->items.empty()) return out;
    const auto refuse = [](const Status& s) {
        return DispatchOutcome{ErrorReply(s), false, 0, s};
    };
    if (txn_ == nullptr || scope.txn == nullptr) {
        return refuse(Status::NotImplemented(
            "RETURNING needs the transaction manager, whose trail names the rows written (byte " +
            std::to_string(returning->byte_offset) + ")"));
    }
    auto oid = ResolveNames(session, [&](const txn::ReadView* v) {
        return catalog_.ResolveRelation(schema, table, session.default_namespace(), 0, v);
    });
    if (!oid.ok()) return refuse(oid.status());
    auto access = catalog_.InitTableAccess(oid.value());
    if (!access.ok()) return refuse(access.status());
    const catalog::TableAccess& ta = *access.value();

    // ---- The items, typed over the relation's live columns ---------------
    const exec::StatementContext context = exec::StatementContextNow();
    std::vector<std::unique_ptr<exec::TypedSetExpr>> items;
    std::vector<wire::FieldDescription> fields;
    std::vector<std::uint32_t> wire_types;
    const auto add_field = [&](std::string name, const exec::SetType& type, bool key) {
        wire::FieldDescription f;
        f.name = std::move(name);
        f.type_oid = type.type_val;
        const std::uint32_t wire_type = key ? wire::WireValueType(type.type_val, 0) : type.type_val;
        f.type_len = wire::WireTypeLen(wire_type);
        f.type_mod = catalog::TypeModOf(type.type_val, type.len);
        if (key) f.flags |= wire::kFieldFlagKeystone;
        fields.push_back(std::move(f));
        wire_types.push_back(wire_type);
    };
    const auto column_node = [&](std::size_t i) {
        auto n = std::make_unique<exec::TypedSetExpr>();
        n->kind = parser::Expr::Kind::kColumn;
        n->column_no = i;
        n->type = exec::SetType{exec::SetType::Kind::kConcrete, ta.schema.columns[i].type_val,
                                ta.schema.columns[i].len};
        return n;
    };
    exec::ExprScope scope_of;
    scope_of.clause = "in RETURNING";
    scope_of.resolve = [&](const parser::Expr& e) -> StatusOr<std::optional<exec::ResolvedColumn>> {
        if (!e.qualifier.empty() && !IEquals(e.qualifier, table)) {
            return Status::NotImplemented(
                "RETURNING reads the written relation's columns only; '" + e.qualifier +
                "' is not it (byte " + std::to_string(e.byte_offset) + ")");
        }
        auto col = ta.schema.FindColumn(e.column, e.byte_offset);
        if (!col.ok()) return col.status();
        if (col.value() == nullptr) return std::optional<exec::ResolvedColumn>();
        const auto i = static_cast<std::size_t>(col.value() - ta.schema.columns.data());
        return std::optional<exec::ResolvedColumn>(
            exec::ResolvedColumn{i, column_node(i)->type});
    };
    for (const parser::OutputItem& item : returning->items) {
        if (item.star) {
            if (!item.star_qualifier.empty() && !IEquals(item.star_qualifier, table)) {
                return refuse(Status::InvalidArgument(
                    "'" + item.star_qualifier + "' names no relation of this statement (byte " +
                    std::to_string(item.byte_offset) + ")"));
            }
            for (std::size_t i = 0; i < ta.schema.columns.size(); ++i) {
                if (!ta.schema.columns[i].visible()) continue;  // BW-R6, BZ-R3
                auto n = column_node(i);
                add_field(std::string(catalog::NameView(ta.schema.columns[i].name)), n->type, i == 0);
                items.push_back(std::move(n));
            }
            continue;
        }
        auto typed = exec::TypeQueryExpression(*item.expr, scope_of);
        if (!typed.ok()) return refuse(typed.status());
        const bool key = item.expr->kind == parser::Expr::Kind::kColumn &&
                         typed.value()->column_no == 0;
        // Headed as the query layer heads an item (parser-v2.md I19): the
        // alias, else the item as written.
        add_field(item.alias.empty() ? item.text : item.alias, typed.value()->type, key);
        items.push_back(std::move(typed.value()));
    }

    // ---- The rows this statement wrote, in the order it wrote them -------
    std::vector<std::uint64_t> keys;
    std::set<std::uint64_t> seen;
    const std::vector<txn::TrailEntry>& trail = scope.txn->trail();
    for (std::size_t t = statement_trail_mark_; t < trail.size(); ++t) {
        const txn::TrailEntry& e = trail[t];
        if (e.rel_oid != ta.oid || e.action != wrote) continue;
        if (seen.insert(e.tuple_key).second) keys.push_back(e.tuple_key);
    }

    TextResultSink text_sink;
    ResultSink& sink = SinkFor(session, text_sink);
    if (Status s = sink.Describe(std::move(fields)); !s.ok()) return refuse(s);
    std::vector<parser::AstValue> image(ta.schema.columns.size());
    std::vector<exec::PendingSpill> spills;
    std::string row_scratch;
    for (std::uint64_t key : keys) {
        // The slot holds this statement's version: an INSERT's and an
        // UPDATE's after-image, and a DELETE's before-image under its mark.
        auto at = btree::BtreeLookup(page_store_, ta.desc_page_id, key);
        if (!at.ok()) return refuse(at.status());
        auto tuple = heap::PageView(at.value().leaf.bytes()).ReadTuple(at.value().slot);
        if (!tuple.ok()) return refuse(tuple.status());
        const std::vector<std::byte> payload(tuple.value().payload.begin(),
                                             tuple.value().payload.end());
        at.value().leaf.Release();
        spills.clear();
        if (Status s = exec::DecodeAndResolve(page_store_, ta.schema, ta.layout, payload, image,
                                              exec::kAllColumnsMask, spills);
            !s.ok()) {
            return refuse(s);
        }
        std::vector<parser::AstValue> row;
        row.reserve(items.size());
        for (const auto& item : items) {
            auto v = exec::EvaluateQueryExpression(*item, image, context, nullptr);
            if (!v.ok()) return refuse(v.status());
            row.push_back(std::move(v.value()));
        }
        if (Status s = sink.EncodeValueRow(wire_types, row, row_scratch); !s.ok()) return refuse(s);
        if (Status s = sink.Emit(row_scratch); !s.ok()) return refuse(s);
    }
    out.returning = true;
    if (session.result_sink() == nullptr) out.response = text_sink.Take() + "\\n" + out.response;
    return out;
}

StatusOr<exec::RowSet> CommandDispatcher::RunLayerQuery(const parser::Query& query,
                                                        std::string_view text, Session& session,
                                                        const txn::Snapshot& snap,
                                                        const exec::StatementContext& context) {
    LayerHostImpl host(*this, session, snap, context);
    exec::LayerLimits limits;
    limits.max_rows = sort_max_rows_;
    limits.max_groups = aggregate_limits_.max_groups;
    limits.max_distinct = aggregate_limits_.max_distinct;
    return exec::RunQuery(query, text, host, context, limits);
}

std::optional<DispatchOutcome> CommandDispatcher::TryQueryLayer(
    std::string_view line, Session& session, const txn::Snapshot& snap,
    const exec::StatementContext* context) {
    parser::Parser parser(line);
    auto query = parser.ParseQuery();
    if (!query.ok()) return std::nullopt;

    const exec::StatementContext now =
        context != nullptr ? *context : StatementContextFor(session);
    auto result = RunLayerQuery(query.value(), line, session, snap, now);
    if (!result.ok()) return DispatchOutcome{ErrorReply(result.status()), false, 0, result.status()};

    TextResultSink text_sink;
    ResultSink& sink = SinkFor(session, text_sink);
    const exec::RowSet& rs = result.value();
    std::vector<wire::FieldDescription> fields;
    std::vector<std::uint32_t> wire_types;
    for (const exec::LayerColumn& c : rs.columns) {
        wire::FieldDescription f;
        f.name = c.heading.empty() ? c.name : c.heading;
        f.type_oid = c.type.type_val;
        const std::uint32_t wire_type =
            c.keystone ? wire::WireValueType(c.type.type_val, 0) : c.type.type_val;
        f.type_len = wire::WireTypeLen(wire_type);
        f.type_mod = catalog::TypeModOf(c.type.type_val, c.type.len);
        if (c.driving_keystone) f.flags |= wire::kFieldFlagKeystone;
        fields.push_back(std::move(f));
        wire_types.push_back(wire_type);
    }
    if (Status s = sink.Describe(std::move(fields)); !s.ok()) {
        return DispatchOutcome{ErrorReply(s), false, 0, s};
    }
    std::string row_scratch;
    for (const auto& row : rs.rows) {
        if (Status s = sink.EncodeValueRow(wire_types, row, row_scratch); !s.ok()) {
            return DispatchOutcome{ErrorReply(s), false, 0, s};
        }
        if (Status s = sink.Emit(row_scratch); !s.ok()) {
            return DispatchOutcome{ErrorReply(s), false, 0, s};
        }
    }
    return DispatchOutcome{text_sink.Take(), false};
}

DispatchOutcome CommandDispatcher::UpdateInner(std::string_view line, WriteScope& scope,
                                               const txn::Snapshot& snapshot,
                                               WalkCursor resume_from,
                                               const exec::StatementContext& context,
                                               std::vector<exec::SubqueryResult>& subqueries) {
    // AO-S3b: set by the row callback when it meets a row held by a writer
    // that has not decided and the wait is one this statement may make.
    // The callback cannot park - it runs under a page span, which AO-R2
    // forbids parking under - so it records the fact and stops the walk,
    // and the park happens in `DispatchAsync` with no span held.
    // `blocked_verdict` is the conflict it stopped on, kept because a
    // statement that turns out to have written nothing is answered with it
    // after all (see the stop's arm below).
    bool parked_on_row = false;
    Status blocked_verdict;
    auto parsed = ParseKeepingShape(line);
    if (!parsed.ok()) {
        return {ErrorReply(parsed.status()), false, 0, parsed.status()};
    }
    if (!std::holds_alternative<parser::UpdateStmt>(parsed.value())) {
        return {"ERR expected an UPDATE statement", false};
    }
    auto& stmt = std::get<parser::UpdateStmt>(parsed.value());

    // Resolved under the writing session's view (DT3c): a write to a
    // relation another transaction created and has not committed must not
    // find it.
    auto oid = ResolveNames(scope.session, [&](const txn::ReadView* v) {
        // The statement's namespace, carried across a park with its NOW().
        return catalog_.ResolveRelation(stmt.schema, stmt.table_name, context.default_namespace,
                                        stmt.table_byte_offset, v);
    });
    if (!oid.ok()) {
        return {ErrorReply(oid.status()), false, 0, oid.status()};
    }

    ReadBorrow borrow(locks_, NextReadHolder(), &read_borrows_, oid.value());  // AT-R1

    auto access = catalog_.InitTableAccess(oid.value());
    if (!access.ok()) {
        return {ErrorReply(access.status()), false, 0, access.status()};
    }
    // Borrowed from the catalog's cache, not owned: valid for this
    // statement, including across AllocateRowId() (catalog.hpp).
    const catalog::TableAccess& ta = *access.value();

    // The range-owner destination and the multi-owner refusal retired
    // (AT-S9); their history is at cf889b63. This write walks the whole
    // relation here, on the core its session is on.
    if (Status admitted = CheckWriteAdmission(ta); !admitted.ok()) {
        return {ErrorReply(admitted), false, 0, admitted};
    }

    // Resolve the SET list before touching storage, so a bad target fails
    // clean with no partial update. All three refusals - an unknown column,
    // a column named twice (BJ-R2), and the tuple key (K2, `Unsupported`) -
    // live in the compiler beside
    // CompileWhere rather than here: the two halves of an UPDATE's compile
    // belong at one layer, and a check the dispatcher owns is one a second
    // write path can be written without.
    // BJ-S4: the typed expressions come back with it, and a column-reading
    // expression into a foreign-key column is refused here (BJ-Q10).
    std::vector<exec::CompiledAssignment> compiled;
    // Built only for a statement with an expression in it: a capture this wide
    // is a heap allocation, and most UPDATEs are literals.
    std::size_t subqueries_asked = 0;
    exec::SubqueryResolver resolve_subquery;
    if (std::any_of(stmt.assignments.begin(), stmt.assignments.end(),
                    [](const parser::Assignment& a) { return a.expr != nullptr; })) {
        resolve_subquery = [&](const parser::Expr& node) -> StatusOr<exec::SubqueryResult> {
            const std::size_t index = subqueries_asked++;
            if (index < subqueries.size()) return subqueries[index];  // a resume: the first run's
            auto answer = RunScalarSubquery(node, stmt, scope, snapshot, context);
            if (answer.ok()) subqueries.push_back(answer.value());
            return answer;
        };
    }
    if (Status s = exec::CompileAssignments(ta, stmt.assignments, &compiled, &resolve_subquery);
        !s.ok()) {
        return {ErrorReply(s), false, 0, s};
    }
    // One value per assignment for the row in hand. A bare literal's slot is
    // filled once, here; an expression's is overwritten for every row. Owned
    // by the statement so no row allocates for it after the first.
    // `assigned` names the cells a row's new image re-encodes, one per
    // assignment, pointing into `values`; every other cell is carried from
    // the row's own bytes (`exec::EncodeAssigned`, BN-R3,
    // `workorder-bn-opt-read-update-path.md`).
    std::vector<parser::AstValue> values(compiled.size());
    std::vector<exec::AssignedCell> assigned(compiled.size());
    for (std::size_t i = 0; i < compiled.size(); ++i) {
        if (compiled[i].expr == nullptr) values[i] = stmt.assignments[i].val;
        assigned[i] = {compiled[i].column_no, &values[i]};
    }

    // The WHERE clause compiles to the same resolved predicates a chain
    // step carries, and is evaluated by the same evaluator (V16). UPDATE
    // reads one relation, so the frame has one step. A subquery's relations
    // resolve under the session's view, as the written relation's did.
    auto predicates = ResolveNames(scope.session, [&](const txn::ReadView* v) {
        return exec::CompileWhere(catalog_, ta, stmt.table_name, stmt.where, v, &borrow,
                                  &context);
    });
    if (!predicates.ok()) {
        return {ErrorReply(predicates.status()), false, 0, predicates.status()};
    }
    const std::vector<const catalog::Schema*> schemas = {&ta.schema};
    exec::ChainFrame frame;
    frame.Open(schemas, /*parent=*/nullptr);

    // Per-row scratch, owned by the statement rather than by the row loop:
    // each is cleared and refilled per scanned row, so after the first row
    // none of them allocates. `payload_copy` is the row's own bytes, copied
    // out of the page for the reason its use site states.
    std::vector<std::byte> payload_copy;
    std::vector<exec::PendingSpill> frame_spills;
    std::vector<exec::PendingSpill> spills;
    const std::uint64_t filter_mask = predicates.value().filter_columns;
    bool conjuncts_checked = false;
    // The collected targets (`WalkCursor::targets`): when set, a row is
    // written if its key is in them and the WHERE is not evaluated again.
    // `collecting` is the collection pass's sink, set only during it.
    std::shared_ptr<const std::unordered_set<std::uint64_t>> targets = resume_from.targets;
    std::unordered_set<std::uint64_t>* collecting = nullptr;

    // Stage 2's decode target and what it decodes (BN-R2): every column
    // where a Cabin or an index reads the row (an assertion is asked per
    // row, below), otherwise the columns the SET expressions read.
    // `row_poisoned` says the slots outside the mask hold `kUndecoded`.
    std::vector<parser::AstValue> row(ta.schema.columns.size());
    bool row_poisoned = false;
    // The negation of `NoteCabinWrite`'s and `MaintainIndexes`' early
    // returns: loosen either and that hook is handed `kUndecoded`.
    // `CabinsWitness` is asked once per statement, so a relation with no
    // Cabin pays no per-row call (AQ's close: the call alone cost an
    // every-row UPDATE 20-30 ns a row).
    const bool witnessed = CabinsWitness(ta);
    const bool hooked = witnessed || !ta.indexes.empty();
    std::uint64_t set_read_mask = 0;
    for (const exec::CompiledAssignment& c : compiled) {
        if (c.expr != nullptr) set_read_mask |= exec::SetExpressionColumns(*c.expr);
    }

    // ---- Which Transitions this SET list touches (BV-R3) ----------------
    //
    // Resolved once, for the foreign key's reason below. A governed column
    // is decoded on every row, since its old value is half the check: in
    // the read mask, or - past the mask's 64 bits - by decoding whole.
    std::vector<std::pair<const catalog::TransitionRef*, std::size_t>> transition_assignments;
    bool transition_needs_whole_row = false;
    for (std::size_t i = 0; i < compiled.size(); ++i) {
        const catalog::TransitionRef* t = ta.TransitionOn(compiled[i].column_no);
        if (t == nullptr) continue;
        transition_assignments.emplace_back(t, i);
        if (t->column_no < 64) {
            set_read_mask |= std::uint64_t{1} << t->column_no;
        } else {
            transition_needs_whole_row = true;
        }
    }

    // ---- Which foreign keys this SET list touches (§2) -------------------
    //
    // Resolved once, here: an UPDATE that changes no fk column must cost
    // nothing, and asking per row would mean a name comparison per row per
    // foreign key to answer a question about the *statement*.
    // Each pairs a foreign key with the index of the assignment that sets its
    // column. BJ-S4: that assignment is a bare literal or an expression, and
    // `CompileAssignments` already refused one that reads a column.
    std::vector<std::pair<const catalog::ForeignKeyRef*, std::size_t>> fk_assignments;
    for (const catalog::ForeignKeyRef& fk : ta.fkeys_out) {
        if (fk.column_no >= ta.schema.columns.size()) continue;
        for (std::size_t i = 0; i < compiled.size(); ++i) {
            if (compiled[i].column_no != fk.column_no) continue;
            fk_assignments.emplace_back(&fk, i);
            break;
        }
    }

    // No view is minted here since AY-S5: each check mints its own after
    // it holds the parent row (`ResolveForeignKeyParents`).
    exec::FkParentVerdicts fk_held;
    if (!fk_assignments.empty()) {
        // ---- The extraction pass (foreign-keys.md §2a, AH-R1) -----------
        //
        // A foreign-key SET value is constant, so the parent set is a
        // property of the *statement*, resolved once before any row work.
        //
        // **Resolving is not failing**: an UPDATE matching zero rows answers
        // `UPDATED 0` even when its SET names a missing parent, so the
        // verdict is *held* here and *applied* per matched row.
        for (const auto& [fk, index] : fk_assignments) {
            // A constant expression is folded at compile (BJ-Q10); one whose
            // fold failed has nothing to resolve, and the row that meets the
            // failure raises it before any foreign-key check.
            if (!compiled[index].constant.has_value()) continue;
            std::vector<parser::AstValue> one(ta.schema.columns.size() > 0
                                                  ? ta.schema.columns.size() - 1
                                                  : 0);
            if (fk->column_no == 0 || fk->column_no > one.size()) continue;
            one[fk->column_no - 1] = *compiled[index].constant;
            if (Status s = ResolveForeignKeyParents(ta, one, fk_held, scope); !s.ok()) {
                return {ErrorReply(s), false, 0, s};
            }
        }
        // Resolved before a single row qualifies - which is also why an
        // UPDATE matching nothing still waits here on a busy parent where it
        // would not report a violation: the resolution is what the
        // statement needs to run at all, not a verdict about a row.
    }

    std::uint32_t updated = resume_from.rows_done;  // AO-S3b: the statement's count, not this run's
    std::uint32_t pages_touched = 0;
    PageId last_page = kInvalidPageId;

    // **The unit this write declares** (AO-0 item 14), read off the
    // predicate and taken once; a coarse declaration keeps a bulk write
    // inside `max_locks_per_txn` (`DeclaredWriteBorrow` says why it is not
    // escalation).
    //
    // **A refused declaration is waited for, not demoted** (AO-S6c-c): a
    // demoted statement would write rows and then park, which the
    // re-runnability rule turns into a refusal; the coarse wait parks
    // having written nothing.
    const std::optional<txn::LockKey> declared = DeclaredWriteBorrow(ta, stmt.where);
    if (declared.has_value()) {
        // `kFutile`: a declared unit is refused by whoever conflicts with
        // it, and the table reports who and not what they hold - so a
        // holder that already wrote a row this walk will reach cannot be
        // told from a fence that wrote nothing, and a repeatable-read
        // statement keeps the exclusion rather than guessing. The row-level
        // arm below is where the two *are* distinguishable, because there
        // the header names the row's own writer.
        if (std::optional<Status> held =
                BorrowOrWait(scope, *declared, RepeatableReadWait::kFutile)) {
            return {ErrorReply(*held), false, 0, *held};
        }
    }

    // **A row that moves a unique key is checked with the span released**
    // (BX-R4). `apply` encodes its keys under the walk's span and stops
    // there with nothing of the row written (`unique_pending`); `clear_unique`
    // takes the key units and checks with no page held - the check descends
    // the clustered tree, and two user relation pages are unordered
    // (`page.md` §6) - and the walk resumes at the row, which finds its keys
    // cleared (`unique_cleared`) and writes, the units held in between.
    std::optional<std::pair<std::uint64_t, UniqueKeysOfRow>> unique_pending;
    std::optional<std::pair<std::uint64_t, UniqueKeysOfRow>> unique_cleared;
    UniqueKeyHolds unique_holds(locks_, scope.txn);
    const auto same_keys = [](const UniqueKeysOfRow& a, const UniqueKeysOfRow& b) {
        return std::ranges::equal(a, b, [](const auto& x, const auto& y) {
            return x.first == y.first && x.second.encoded == y.second.encoded;
        });
    };

    // Applies the SET list to one slot if it matches the WHERE. Shared by
    // the probe path and the scan, for the same reason SELECT shares its
    // formatter: two copies of "how a row is updated" is two chances for
    // the probed path and the scanned path to do different things to the
    // same tuple.
    auto apply = [&](PageId page_id, heap::PageView& page, std::uint16_t slot) -> Status {
        auto tuple = page.ReadTuple(slot);
        if (!tuple.ok()) return Status::OK();  // dead or out-of-range slot - skip

        // ---- MVCC, the same predicate the step VM applies --------------
        //
        // UPDATE does not compile to a chain, so it is the one read path
        // outside `AcceptTupleAt` (workplan A1). It filters here rather
        // than reimplementing anything: Classify is the same function, and
        // a row this reader cannot see is a row it must not write.
        //
        // **A version reached through the undo chain is not updatable.**
        // Its bytes are not the bytes on the page, so an edit of it written
        // back would lose whatever superseded it. kNeedsUndoWalk therefore
        // falls through to the conflict check below rather than resolving -
        // and that check is what rejects it, because a writer this view
        // cannot see is either in flight or committed after it.
        if (txn::Classify(snapshot.view, tuple.value()) == txn::Visibility::kNoVersion) {
            return Status::OK();
        }

        // **The row is copied out of the page once, and decoded from the
        // copy**, which is what makes the two-stage decode below legal
        // under R1: stage 1 resolves spills and may evaluate a subquery,
        // both of which fetch pages, and R1's rule is that no span into a
        // tuple's page may be live across a nested fetch. The pin this walk
        // holds would in fact keep the bytes valid - `PageRef::bytes()` is
        // valid as long as the handle is - so this is the *discipline*
        // rather than a dangling-pointer fix, and it is the discipline
        // because this path has no PageSpanGuard to catch a violation of
        // it. `payload_copy` is hoisted to the statement, so this is one
        // memcpy of a schema-constant row size per scanned row and no
        // allocation after the first.
        payload_copy.assign(tuple.value().payload.begin(), tuple.value().payload.end());
        auto id = KeystoneIdOfPayload(payload_copy);
        if (!id.ok()) return id.status();
        const std::uint64_t trx_id = tuple.value().trx_id;
        const std::uint64_t undo_ptr = tuple.value().undo_ptr;
        // Not a collected target: nothing to decode.
        if (targets != nullptr && !targets->contains(id.value())) return Status::OK();

        // ---- Stage 1: only what the WHERE reads (OPT-001) ---------------
        //
        // A row is decoded only as far as the WHERE needs before it is
        // tested. `filter_mask` is the compiler's, hoisted to the
        // statement because that is what it is a fact about; it is
        // kAllColumns whenever a sub-chain or a >64-column relation makes a
        // partial decode unsound (step_compiler.cpp's CompileWhere).
        if (Status s = exec::DecodeAndResolve(page_store_, ta.schema, ta.layout, payload_copy,
                                              frame.SlotsFor(0), filter_mask, frame_spills);
            !s.ok()) {
            return s;
        }
        // Checked at the first row, as a SELECT's step is (BT-R5): an
        // empty relation refuses nothing.
        if (!conjuncts_checked) {
            if (Status s = exec::CheckConjuncts(schemas, predicates.value(), frame);
                !s.ok()) {
                return s;
            }
            conjuncts_checked = true;
        }
        if (targets == nullptr) {
            auto matched = exec::EvaluateConjuncts(catalog_, page_store_, schemas,
                                                   predicates.value(), frame, /*stats=*/nullptr,
                                                   budget_, &snapshot);
            if (!matched.ok()) return matched.status();
            if (!matched.value()) return Status::OK();
            if (collecting != nullptr) {
                collecting->insert(id.value());
                return Status::OK();
            }
        }

        // ---- First-updater-wins (docs/spec/txn.md section 5) ----------------
        //
        // Checked only once the row has qualified: a conflict is reported
        // about a row this statement actually wanted, never about one it
        // scanned past. No lock and no wait - the verdict is a pure
        // function of the tuple's current writer and this view.
        if (scope.txn != nullptr) {
            // **The borrow, before the conflict is decided** (AO-S6a, the
            // operator's choice of order). No re-read after the grant is
            // owed (AO-S6b): the walk holds a live write `PageRef` for the
            // page and carries no `co_await`, so the captured `trx_id`
            // cannot go stale between the read and the grant.
            //
            // Per row, so no refusal of an earlier row is ever read as this
            // one's - `CheckWriteConflictBlocking` falls it back to `cur`
            // where the table has nothing to say.
            std::uint64_t blocker = 0;
            if (!declared.has_value()) {
                auto took = BorrowChain(scope, txn::LockKey::Tuple(ta.oid, id.value()), &blocker);
                if (!took.ok()) return took.status();
            }
            if (Status s = CheckWriteConflictBlocking(scope, trx_id, id.value(), blocker);
                !s.ok()) {
                // **A conflict a wait can get past stops the walk rather
                // than failing the statement** (AO-S3b). `blocking_writer_`
                // is set only under `may_park_` with a live clock; returning
                // OK lets the caller turn this into a `kStop` holding no span.
                //
                // **Compared against this row's `blocker`, not tested
                // non-zero**: the member is the statement's, and the forward
                // foreign-key check may have left a parent's holder in it.
                // `CheckWriteConflictBlocking` writes back the holder it
                // recorded - the table's (a range holder, AO-S6c-b), or
                // `trx_id` where the table named nobody.
                if (blocker != 0 && blocking_writer_ == blocker) {
                    parked_on_row = true;
                    blocked_verdict = s;
                    return Status::OK();
                }
                return s;
            }
        }

        // ---- Stage 2: what the SET list and the hooks read (BN-R2) -------
        //
        // Into `row`, the statement's, because the SET expressions and the
        // write hooks read it (the new image is encoded from `payload_copy`,
        // BN-R3). Decoded from `payload_copy`, whose bytes outlive every page
        // fetch stage 1 may have made.
        //
        // **Whole only where a hook reads it.** A Cabin, an index and an
        // assertion each take the row full-width, so a relation with any of
        // them decodes every column; one with none decodes only the columns
        // the SET expressions read (`set_read_mask`), and every other slot
        // holds `kUndecoded`, which the evaluator refuses `Corruption` rather
        // than read as a value.
        //
        // **The registry is asked here, after the conflict check, and only
        // here** - the answer is the assertion site's too (AT-S5d: an
        // assertion adopted on another core between two asks would send
        // the check an empty `previous`). After, because the check is where
        // the row's writer takes the relation's `IX`, and a build adopts its
        // assertion under `X`: asked before the grant, a build that adopts in
        // between would leave this row written unchecked.
        const bool asserted = enforcer_->AnyOn(ta.oid);
        const bool whole_row = hooked || asserted || transition_needs_whole_row;
        if (whole_row) {
            row_poisoned = false;
        } else if (!row_poisoned) {
            for (std::size_t c = 0; c < row.size(); ++c) {
                if (c >= 64 || (set_read_mask & (std::uint64_t{1} << c)) == 0) {
                    row[c] = exec::kUndecoded;
                }
            }
            row_poisoned = true;
        }
        if (Status s = exec::DecodeAndResolve(page_store_, ta.schema, ta.layout, payload_copy, row,
                                              whole_row ? exec::kAllColumnsMask : set_read_mask,
                                              spills);
            !s.ok()) {
            return s;
        }

        // ---- BJ-S4: every expression is evaluated here, against the row as
        // the walk read it (BJ-R2) ------------------------------------------
        //
        // After the row qualified and its conflict was decided, before
        // anything of it is written and before the no-refuse window opens (an
        // evaluation can fail, BJ-R10). The values are collected first and
        // written second, so a later assignment never reads an earlier one's
        // result. A failure names the expression's first byte and the row's
        // tuple key and fails the statement like every mid-statement write failure
        // (the AS9 resolution). A value that evaluates but does not fit its
        // target - a NULL into a NOT NULL column, a `char(N)` too long - is
        // the encode's verdict, further down.
        for (std::size_t i = 0; i < compiled.size(); ++i) {
            if (compiled[i].expr == nullptr) continue;
            const std::uint32_t expression_byte = stmt.assignments[i].expr->byte_offset;
            auto value = exec::EvaluateSetExpression(*compiled[i].expr, row, context);
            if (!value.ok()) {
                return value.status().WithMessage(
                    value.status().message() + " in the expression at byte " +
                    std::to_string(expression_byte) + ", row id " + std::to_string(id.value()));
            }
            values[i] = std::move(value.value());
            // A computed value carries no position of its own; the encode's
            // refusals name this one.
            values[i].byte_offset = expression_byte;
        }

        // ---- The forward check, for an fk column this SET touches (§2) --
        //
        // Per matched row, after the row has qualified and the conflict
        // check has passed. No descent here (§2a, AH-T1): the parent was
        // resolved once above, and this applies the held verdict.
        for (const auto& [fk, index] : fk_assignments) {
            if (Status s = CheckForeignKeyOnWrite(ta, *fk, values[index], fk_held, scope);
                !s.ok()) {
                return s;
            }
        }

        // ---- The Transition check, for a governed column this SET touches
        //
        // `row` is still the version being replaced (the SET values land in
        // it below), and the head the conflict check above proved this
        // writer holds. Before the assertion reservation, the undo record
        // and the no-refuse window (BV-R3).
        for (const auto& [t, index] : transition_assignments) {
            if (Status s = exec::CheckTransitionUpdate(ta, *t, row[t->column_no], values[index],
                                                       id.value());
                !s.ok()) {
                return s;
            }
        }

        // The row as it stands *before* the SET list is applied - what tells
        // the Cabin, index and assertion hooks whether a column they read
        // actually moved (cabin.md §5's third row, index.md §2) - taken only
        // where a hook reads it, which is where the row was decoded whole.
        std::vector<parser::AstValue> previous;
        if (whole_row) {
            previous = row;
            for (std::size_t i = 0; i < compiled.size(); ++i) {
                row[compiled[i].column_no] = values[i];
            }
        }

        // ---- The unique check (BX-R4, BX-R5) ---------------------------
        //
        // Only the keys this SET moved. Not yet cleared, the row stops the
        // walk here, before the assertion reservation and every write. A
        // resumed row is written under the clearance only if its keys encode
        // as the checked ones did; otherwise they are checked again.
        if (!ta.indexes.empty()) {
            UniqueKeysOfRow unique_keys = UniqueKeysOf(ta, row, /*first_col_pos=*/0, previous);
            if (!unique_keys.empty() &&
                !(unique_cleared.has_value() && unique_cleared->first == id.value() &&
                  same_keys(unique_cleared->second, unique_keys))) {
                unique_pending.emplace(id.value(), std::move(unique_keys));
                return Status::OK();
            }
        }

        // ---- The assertion check, §4.2's delta rules --------------------
        //
        // Before the undo record and the overwrite, so a refused row is a
        // row nothing touched. `previous` holds the old values whenever an
        // assertion lives on this relation (the condition above); a refusal
        // mid-statement leaves earlier rows written and the transaction
        // poisoned - the AS9 resolution, decided 2026-08-09: uniform with
        // every other write failure, because "open and usable" cannot be
        // promised once a multi-row statement has partly happened.
        //
        // **The no-refuse window opens first** (BE-Q11): the assertion
        // reservation below is this row's first write, then its spills, and
        // the undo record, the overwrite, the indexes and an index root's
        // re-publish each fetch or create after one. A full pool refuses
        // here, with nothing of this row written; earlier rows are on the
        // trail. Closed when this row's visit returns.
        auto window = storage::NoRefuseWindow::Open(page_store_);
        if (!window.ok()) return window.status();
        if (asserted) {
            std::uint64_t reserver = 0;
            if (Status s = enforcer_->AdmitAndReserveUpdate(page_store_, wal_, WriterId(scope),
                                                           ta.oid, previous, row,
                                                           id.value(), page_id, slot, &reserver);
                !s.ok()) {
                // AO-S6e-c, and the INSERT site states the argument.
                // **`tuple key = 0` although the tuple key is known**: the holder holds a
                // *group* reservation and may never have touched this row
                // (`assertion.md` §6.2 states the rule for both arms).
                if (reserver != 0) {
                    NoteBlockingWriter(scope.txn, reserver, /*tuple_key=*/0,
                                       RepeatableReadWait::kCapable);
                }
                return s;
            }
        }

        // The spills this re-encode appended, collected so they can be noted
        // and logged below - a spill left unlogged is a recovered cell
        // pointing at bytes no record describes. Collected when there is a
        // log **or a transaction to roll them back** (VC-B3): a live Abort
        // needs the trail entry whether or not anything is logged.
        //
        // `appended_spills`, not `spills`: the enclosing scope already has a
        // `PendingSpill` list, which is the opposite direction - values this
        // statement *read* out of the var-heap to evaluate the WHERE clause.
        std::vector<exec::AppendedSpill> appended_spills;
        const bool collect_spills = wal_ != nullptr || scope.txn != nullptr;
        // With a manager, noted and logged at each append (`SpillLogFor`),
        // under the var-heap page's hold - this leaf's hold outside it, the
        // order a reader takes too; `appended_spills` then stays empty.
        const auto log_spill = SpillLogFor(scope, ta.oid, id.value());
        // **Only the assigned cells are encoded** (BN-R3): every other cell
        // is carried from `payload_copy`, the version this walk read, so a
        // spilled value the SET list does not name is shared with the new
        // version rather than appended again - and no longer leaked by every
        // UPDATE of its row. The tuple key is the tuple's own Keystone word, never
        // assignable (rejected above).
        auto encoded = exec::EncodeAssigned(
            ta.schema, ta.layout, id.value(), payload_copy, assigned,
            exec::VarHeapSink{&page_store_, ta.varheap_page_id,
                              collect_spills ? &appended_spills : nullptr, ta.oid,
                              log_spill ? &log_spill : nullptr});
        if (!encoded.ok()) return encoded.status();

        // HOT-style in-place overwrite - see PageView::OverwriteTuple's
        // comment. There is no retire+reinsert fallback because there is
        // nothing to fall back *from*: under the fixed-length rule the new
        // payload is exactly the same size as the old one, since a row's
        // size is a schema constant and not a function of its values
        // (invariant 13).
        //
        // That is the property the whole fixed-length rule exists for. An
        // UPDATE can never migrate a tuple, so combined with the immutable
        // min_key a row's (page_id, slot) is stable for life until relayout
        // moves it on purpose - which is what stops an UPDATE from burning
        // Waystone trail entries through epoch churn, and why no page's
        // min_key can be invalidated by one.
        // Re-fetched rather than written through `page`: encoding may have
        // appended to the var-heap, and a store is free to move its frames
        // when it hands out a new page (the same reason heap_chain.cpp
        // re-fetches its tail after CreateNew()).
        // ---- The before-image (docs/spec/txn.md section 3.3) ----------------
        //
        // Written **before** the page is overwritten, and the tuple is
        // stamped with the pointer it returns. A tuple carrying an
        // undo_ptr whose record was never written is a version chain that
        // dead-ends in garbage, so a failure here abandons the write.
        std::uint64_t new_trx_id = trx_id;
        std::uint64_t new_undo_ptr = undo_ptr;
        if (scope.txn != nullptr) {
            // The bytes as they stand: `payload_copy`, the version this walk
            // read - not reconstructed from `row`, which has had the SET list
            // applied and would restore the *new* values. Nothing has written
            // the slot since the copy: the walk holds this page's write
            // `PageRef` and carries no `co_await` (the conflict check's
            // argument above), and the new image the carry starts from is
            // the same buffer (BN-R3).
            const std::vector<std::byte>& image = payload_copy;

            txn::UndoRecordFields rec{};
            rec.prior_trx_id = trx_id;
            rec.prior_undo_ptr = undo_ptr;
            rec.target_page_id = page_id;
            rec.target_slot = slot;
            rec.type = static_cast<std::uint8_t>(txn::UndoRecordType::kOverwrite);

            auto ptr = txn_->AppendUndo(*scope.txn, rec, id.value(), image);
            if (!ptr.ok()) return ptr.status();
            new_trx_id = scope.txn->id();
            new_undo_ptr = ptr.value();

            // What rollback compensates from. Recorded before the
            // mutation, so an abort can undo a write a later failure
            // interrupted.
            txn_->NoteOverwrite(*scope.txn, ta.oid, page_id, slot, id.value(), trx_id, undo_ptr,
                                image);
        }

        auto page_again = page_store_.Get(page_id);
        if (!page_again.ok()) return page_again.status();
        heap::PageView fresh(page_again.value().bytes());
        if (Status s = fresh.OverwriteTuple(slot, encoded.value(), new_trx_id, new_undo_ptr);
            !s.ok()) {
            return s;
        }

        // ---- The Cabin witness, UPDATE half (docs/spec/cabin.md §5) -----
        //
        // `row` now holds the **new** values, so this appends the tuple key
        // to v′'s set for every cabined column. The old value's set is
        // deliberately left alone: a pre-update snapshot is still entitled
        // to match through it, and for newer readers the stale entry is a
        // surplus the read-time key re-check subtracts. Removal here would
        // be incorrect, not an optimization forgone.
        //
        // The location is unchanged by construction - an UPDATE is an
        // in-place overwrite under invariant 13 - so the appended hint is
        // the row's real address.
        if (witnessed) {
            NoteCabinWrite(ta, row, /*first_col_pos=*/0, id.value(), page_id, slot, previous);
        }

        // The index half. `previous` is what makes §2's rule work: an UPDATE
        // that moved no key and no covered column appends nothing, which is
        // what keeps an index from growing by an entry per write forever.
        std::vector<exec::IndexWrite> index_writes;
        const exec::IndexWriteLog log_index = IndexWriteLogFor(scope, index_writes);
        if (Status s = exec::MaintainIndexes(catalog_, page_store_, ta, row,
                                              /*first_col_pos=*/0, encoded.value(), id.value(),
                                              previous, log_index ? &log_index : nullptr);
            !s.ok()) {
            return s;
        }
        // The row's entries are in, so its key units go back (BX-Q2 (a)).
        unique_holds.Release();
        unique_cleared.reset();

        // ---- HEAP_OVERWRITE (wal.md section 5.2) -----------------------
        //
        // UPDATE was unlogged **entirely** before this: it mutated a page
        // and told the log nothing, so a crash lost the change with no
        // record that it had happened. Logged now, after the undo record it
        // points at *and* after the index entries that reach the new
        // version - both for the same reason the var-heap has: a replay must
        // never reach a pointer that resolves to nothing, and a version no
        // index entry names is a row a probe cannot find
        // (docs/spec/index.md §12.1).
        // Each spill's rollback, before the records that make the spill
        // durable: an UNDO_WRITE must precede the VARHEAP_APPEND it can
        // undo, so redo alone can never resurrect an append the undo phase
        // has no record to release (RV3's ordering rule, wal.md §11a).
        // Outside the logging gate below because the trail entry is what a
        // *live* Abort reads, and that is owed whether or not there is a log.
        if (Status s = NoteSpills(scope, ta.oid, id.value(), appended_spills); !s.ok()) {
            return s;
        }

        if (wal_ != nullptr && scope.txn != nullptr) {
            // The var-heap first, for the reason the comment above gives and
            // INSERT already obeyed: the cell in the tuple record below points
            // into these pages, so the records that create, link and fill them
            // must precede it.
            if (Status s = exec::LogSpills(wal_, page_store_, appended_spills, scope.txn->id(), ta.oid);
                !s.ok()) {
                return s;
            }
            // The index entries were logged as maintenance made them, under
            // this transaction (`IndexWriteLogFor`).

            std::vector<std::byte> buf(wal::kHeapWriteFixedSize + encoded.value().size());
            const wal::HeapWritePayload fields{
                new_trx_id, new_undo_ptr, slot,
                static_cast<std::uint16_t>(encoded.value().size())};
            if (auto n = wal::EncodeHeapWrite(buf, fields, encoded.value()); !n.ok()) {
                return n.status();
            }
            auto rec = wal_->Append(
                wal::RecordSpec{wal::RecordType::kHeapOverwrite, scope.txn->id(), page_id}, buf);
            if (!rec.ok()) return rec.status();
            if (Status s = page_store_.StampPageLsn(page_id, rec.value()); !s.ok()) return s;
        }

        ++updated;
        if (after_update_row_applied_for_test_) after_update_row_applied_for_test_(id.value());
        if (page_id != last_page) {
            last_page = page_id;
            ++pages_touched;
        }
        return Status::OK();
    };

    // Runs with no page held: the walk or the probe has stopped at the row
    // `unique_pending` names. A refusal that is a wait parks the statement as
    // a held row does (AO-S3b); `blocking_writer_` is cleared around the
    // check so only this check's note can read as one.
    const auto clear_unique = [&]() -> Status {
        auto pending = std::move(*unique_pending);
        unique_pending.reset();
        unique_holds.Release();
        const std::uint64_t noted_before = blocking_writer_;
        blocking_writer_ = 0;
        if (std::optional<Status> refused =
                CheckUniqueKeys(scope, ta, pending.second, pending.first, unique_holds)) {
            unique_holds.Release();
            if (blocking_writer_ != 0) {
                parked_on_row = true;
                blocked_verdict = *refused;
                return Status::OK();
            }
            blocking_writer_ = noted_before;
            return *refused;
        }
        blocking_writer_ = noted_before;
        unique_cleared = std::move(pending);
        return Status::OK();
    };

    // Same fast path as SELECT, and the same contract: a point UPDATE by tuple key
    // is the other statement shape whose cost is otherwise linear in the
    // relation. Any reason to decline falls through to the scan, which
    // produces the identical result - the locator picks the slot to look
    // at, never which rows match. Its target is the WHERE's only conjunct,
    // so a point write carries no subquery to evaluate under the exclusive
    // leaf (the collection pass's reason, below).
    if (std::optional<std::uint64_t> tuple_key = TupleKeyEqualityTarget(ta, stmt.where); tuple_key.has_value()) {
        // A second round only when a moved unique key sent the row round:
        // checked with the leaf released, then found and applied again under
        // the clearance (BX-R4).
        for (;;) {
            TupleKeyLookup found = LocateByTupleKey(ta, *tuple_key);
            if (found.kind == TupleKeyLookup::Kind::kAbsent) {
                return {"UPDATED 0", false, 0};  // no such row, on the tree's authority
            }
            if (found.kind != TupleKeyLookup::Kind::kAt) break;
            // Through the leaf the descent holds exclusive and dirty (btree.hpp
            // `Location`): a shared lookup then `Get()` let a divide renumber
            // the slot, and `apply` declined another row on the WHERE.
            heap::PageView page(found.at.leaf.bytes());
            if (Status s = apply(found.at.page_id, page, found.at.slot); !s.ok()) {
                return {ErrorReply(s), false, 0, s};
            }
            if (unique_pending.has_value()) {
                found.at.leaf.Release();
                if (Status s = clear_unique(); !s.ok()) return {ErrorReply(s), false, 0, s};
                if (!parked_on_row) continue;
            }
            // **`apply` answers OK for a row it declined to write**
            // (AO-S3b), so without this a held row would render
            // `UPDATED 0`. One row is in scope, so nothing was written:
            // the conflict is the answer, and `EndWrite` keeps the blocker
            // that makes `DispatchAsync` wait and re-run the statement.
            if (parked_on_row) {
                return {ErrorReply(blocked_verdict), false, 0, blocked_verdict};
            }
            if (logging(LogLevel::kTrace)) {
                log_->Trace("query", "tuple key " + std::to_string(*tuple_key) + " updated at " +
                                         std::to_string(found.at.page_id) + ":" +
                                         std::to_string(found.at.slot));
            }
            return {"UPDATED " + std::to_string(updated), false, updated};
        }
    }

    // **The collection pass**, when the WHERE holds a subquery: a read walk
    // that evaluates the WHERE over every row and writes nothing, so the
    // walk below writes exactly the rows that matched and runs no subquery.
    // Two reasons, either sufficient. Over the relation this writes, a
    // subquery re-run per row would read the rows the statement has already
    // written (parser-v2.md R2). Over any relation, it would read pages
    // under the write walk's exclusive leaf: a held `X` asking an `S`, which
    // two statements on two cores close into a cycle no latch wait breaks
    // (`rules.md` §3's page latch row). A resume carries its targets and
    // does not collect again, and one that collects nothing writes nothing.
    if (targets == nullptr && !predicates.value().sub_chains.empty()) {
        auto collected = std::make_shared<std::unordered_set<std::uint64_t>>();
        collecting = collected.get();
        Status pass = VisitRelation(
            ta, storage::PageAccess::kRead,
            [&](PageId page_id, heap::PageView& page,
                std::uint16_t slot) -> StatusOr<storage::VisitControl> {
                if (Status s = apply(page_id, page, slot); !s.ok()) return s;
                return storage::VisitControl::kContinue;
            },
            /*cursor=*/nullptr);
        collecting = nullptr;
        if (!pass.ok()) return {ErrorReply(pass), false, 0, pass};
        if (collected->empty()) return {"UPDATED " + std::to_string(updated), false, updated};
        targets = std::move(collected);
    }

    // AO-S3b's resume position. A first walk starts inactive, which is
    // "from the head"; a resumed statement is handed the one its park left.
    WalkCursor walk_cursor = resume_from;
    walk_cursor.targets = targets;
    Status scan;
    for (;;) {
        scan = VisitRelation(
            ta, storage::PageAccess::kWrite,
            [&](PageId page_id, heap::PageView& page,
                std::uint16_t slot) -> StatusOr<storage::VisitControl> {
                // UPDATE has no early exit *of its own*: it must consider every
                // row, since the WHERE is evaluated per tuple and any of them
                // may match. It stops at a row held by a writer that has not
                // decided (AO-S3b), so the page span is released and the
                // statement above can park, and at a row whose unique key it
                // must check with no span (BX-R4).
                if (Status s = apply(page_id, page, slot); !s.ok()) return s;
                if (parked_on_row || unique_pending.has_value()) {
                    return storage::VisitControl::kStop;
                }
                return storage::VisitControl::kContinue;
            },
            &walk_cursor);
        // The stop left the cursor at the row, so the next walk offers it
        // again, now cleared.
        if (!scan.ok() || !unique_pending.has_value()) break;
        if (Status s = clear_unique(); !s.ok()) {
            scan = s;
            break;
        }
        if (parked_on_row) break;
    }
    if (!scan.ok()) {
        // Partial **within the statement**, which is section 6's stated
        // rule rather than an exposure now. In autocommit EndWrite() aborts
        // this scope, so the rows already updated are compensated and the
        // statement is atomic after all. Inside an explicit transaction
        // they stay written and the session is poisoned - the client must
        // ROLLBACK, which undoes all of them.
        return {ErrorReply(scan), false, 0, scan};
    }

    // **The walk stopped on a held row, so this is not an answer** (AO-S3b):
    // `updated` counts rows written *so far*, and returning it would be a
    // silent partial UPDATE.
    if (parked_on_row) {
        // **Only a statement that has written rows resumes.** One that has
        // not is answered with its conflict and re-run whole by
        // `DispatchAsync` (AO-S3) under a fresh snapshot, which sees a holder
        // that committed meanwhile; a resume keeps the original view, under
        // which that holder stays invisible.
        if (scope.txn == nullptr || scope.txn->trail().size() == statement_trail_mark_) {
            return {ErrorReply(blocked_verdict), false, 0, blocked_verdict};
        }
        DispatchOutcome parked;
        parked.parked_mid_walk = true;
        walk_cursor.rows_done = updated;
        parked.walk_cursor = walk_cursor;
        return parked;
    }

    if (updated > 0 && logging(LogLevel::kTrace)) {
        log_->Trace("heap", "overwrite rows=" + std::to_string(updated) + " across " +
                                std::to_string(pages_touched) + " page(s) of table oid " +
                                std::to_string(oid.value()));
    }
    return {"UPDATED " + std::to_string(updated), false, updated};
}

// ---- Transaction control (docs/spec/txn.md sections 1, 6) ---------------------

DispatchOutcome CommandDispatcher::HandleBegin(std::string_view args, Session& session) {
    if (txn_ == nullptr) {
        return {"ERR this server was built without a transaction manager", false};
    }
    if (session.in_explicit_txn()) {
        // Not silently ignored, and not a nested transaction: there are no
        // savepoints (section 9), so a second BEGIN has no meaning that is
        // not a guess about which one a later COMMIT ends.
        return {"ERR a transaction is already open; COMMIT or ROLLBACK first", false};
    }

    // `BEGIN [TRANSACTION] [ISOLATION LEVEL <name>] [DURABILITY <class>]`.
    // Either clause given here overrides the session's for this transaction
    // only - the third rung of the two precedence chains, and the spelling
    // KWP's `C_TXN_BEGIN{durability}` reaches (protocol.md §9). Both are
    // optional and order-free, because two independent overrides that must
    // be written in one order would be a grammar rule with nothing behind
    // it.
    txn::IsolationLevel level = session.isolation();
    std::optional<wal::DurabilityClass> durability;
    auto [word, rest] = SplitFirstToken(args);
    if (IEquals(word, "TRANSACTION") || IEquals(word, "WORK")) {
        std::tie(word, rest) = SplitFirstToken(rest);
    }
    while (!word.empty()) {
        if (IEquals(word, "ISOLATION")) {
            auto [level_word, after] = SplitFirstToken(rest);
            if (!IEquals(level_word, "LEVEL")) {
                return {"ERR expected LEVEL after ISOLATION", false};
            }
            // **The level name is not one token** - `repeatable read` is two
            // - so it runs to the end of the clause, which is either the end
            // of the statement or the next clause keyword.
            auto [name, tail] = SplitBeforeClause(after, "DURABILITY");
            auto parsed = txn::ParseIsolationLevel(Trim(name));
            if (!parsed.ok()) return {"ERR " + parsed.status().message(), false, 0, parsed.status()};
            level = parsed.value();
            std::tie(word, rest) = SplitFirstToken(tail);
            continue;
        }
        if (IEquals(word, "DURABILITY")) {
            // One token by construction: every accepted spelling is a
            // single word (`strict`/`group`/`relaxed`, `d1`/`d2`/`d3`).
            auto [name, tail] = SplitFirstToken(rest);
            auto parsed = wal::ParseDurabilityClass(Trim(name));
            if (!parsed.ok()) return {"ERR " + parsed.status().message(), false, 0, parsed.status()};
            durability = parsed.value();
            std::tie(word, rest) = SplitFirstToken(tail);
            continue;
        }
        return {ErrorReply(Status::InvalidArgument(
                    "expected ISOLATION LEVEL or DURABILITY after BEGIN, got '" +
                    std::string(word) + "'")),
                false};
    }

    auto begun = txn_->Begin(level);
    // ErrorReply, not a bare "ERR ": the id this draws can fail to carve,
    // and inside an explicit transaction that refusal lands *here* rather
    // than at the INSERT - the id is drawn once, at BEGIN - so this is the
    // site a retryable refusal has to reach the wire from.
    if (!begun.ok()) return {ErrorReply(begun.status()), false, 0, begun.status()};
    session.Adopt(begun.value());
    // After `Adopt`, which is what makes `EffectiveDurability` read the
    // transaction rung at all - and after `Begin`, so a refused BEGIN
    // leaves no class behind on the session.
    if (durability.has_value()) session.set_txn_durability(*durability);
    // **And re-stamp**, because the class this transaction commits under
    // was resolved at the top of the statement that opened it, when the
    // transaction rung did not yet exist. Every later statement re-reads it
    // on its own dispatch; only BEGIN's own statement can set the rung it
    // already passed.
    effective_durability_ = session.EffectiveDurability(durability_);

    return {"BEGIN trx_id=" + std::to_string(begun.value()->id()) + " isolation=" +
                txn::IsolationLevelName(level) + " durability=" +
                wal::DurabilityClassName(session.EffectiveDurability(durability_)),
            false};
}

DispatchOutcome CommandDispatcher::HandleCommit(Session& session) {
    if (!session.in_explicit_txn()) {
        return {"ERR no transaction is open", false};
    }
    if (session.failed()) {
        // Reachable only through the gate's whitelist, which does not admit
        // COMMIT - kept as a second line because "commit a failed
        // transaction" must never quietly succeed.
        return {"ERR current transaction is aborted; ROLLBACK", false};
    }

    // The cross-owner commit fork retired (AT-S6); its history is at
    // cf889b63. A transaction has no half on another core, so the local
    // path is the only path there is.
    return CommitLocal(session);
}

DispatchOutcome CommandDispatcher::CommitLocal(Session& session) {
    txn::Transaction* txn = session.transaction();
    const std::uint64_t id = txn->id();

    // The transaction's reservations become committed entries (§6.2 step
    // 4): flags cleared, ASSERT_COMMIT logged, before the commit record. A
    // failure leaves the transaction open - the client may retry COMMIT or
    // ROLLBACK - but **not its pending set**: `CommitTxn` took it before the
    // settle that failed. A retried COMMIT commits with the unsettled
    // entries' flags left set; a ROLLBACK finds nothing to unapply and
    // leaves their group totals counting rows it took back - too high, so
    // the assertion refuses writes it could admit
    // (`assertion-reservations-stranded-by-a-failed-settle.md`).
    if (Status s = enforcer_->CommitTxn(page_store_, wal_, id); !s.ok()) {
        return {ErrorReply(s), false, 0, s};
    }
    const bool deferred = DefersCommit(id);
    auto committed = deferred ? txn_->CommitDeferred(*txn) : txn_->Commit(*txn, effective_durability_);

    if (!committed.ok()) {
        // **A failed commit must abort, not merely be reported.**
        // `Commit` returns on a logging failure *before* it clears
        // `active_`, and `Release` refuses to erase an active
        // transaction - so without this the transaction sits in `live_`
        // for the life of the process: holding the instance's floor and
        // its commit window at its own id, and since DT9 answering
        // `IsInFlight` true forever, which would keep
        // every catalog row it delete-marked alive to every unfiltered
        // read. A dropped index maintained and probed for ever after.
        //
        // Aborting is what the line below already claimed - "the session
        // leaves the transaction either way" - carried through to the
        // transaction itself. It also undoes the writes the failed commit
        // never made durable, which is the only outcome that leaves the
        // instance consistent with the "ERR" the client is about to read.
        //
        // Before `EndDdlScope`, so the invalidation it does describes
        // pages the compensation has already rewritten.
        Status rolled = txn_->Abort(*txn, RowLocatorForRollback());
        EndDdlScope(session);
        session.Finish();
        txn_->Release(*txn);
        // The commit failure is the client's answer; a failure to unwind
        // on top of it is a second, worse fact and is not swallowed.
        if (!rolled.ok()) {
            return {ErrorReply(committed.status()) +
                        " (and rolling it back failed: " + rolled.message() + ")",
                    false};
        }
        return {ErrorReply(committed.status()), false, 0, committed.status()};
    }

    if (deferred) {
        // **Parked before its publish** (BA-S7 part 2): the statement parks
        // on the record's durability, and the second half runs then. No
        // catalog write is in this transaction (`DefersCommit`), so
        // `EndDdlScope` has nothing to settle and the session's own
        // bookkeeping can go now.
        session.Finish();
        session.set_acknowledged_commit_lsn(committed.value());
        DeferCommit(txn, committed.value());
        return {"COMMIT trx_id=" + std::to_string(id), false};
    }

    // Its catalog rows are committed now, so every reader may see them
    // unfiltered again (DT3c). Before `Finish()`, which clears the
    // session's transaction pointer this reads. A drop it made is queued
    // for reclamation at this commit (BF-R9).
    EndDdlScope(session, committed.value());
    session.Finish();
    // The session's next statement mints no snapshot below this (BA-R1c).
    session.set_acknowledged_commit_lsn(committed.value());

    // The durability wait the client is owed, for the same reason
    // LogInsert() takes it: kGroup staged the commit for the next drain,
    // and the acknowledgement means "durable". `kStrict` synced inside
    // `Commit` before it returned and `kRelaxed` stages nothing.
    if (wal_ != nullptr && effective_durability_ == wal::DurabilityClass::kGroup &&
        !wal_->IsDurable(committed.value())) {
        pending_commit_lsn_ = committed.value();
    }
    txn_->Release(*txn);
    return {"COMMIT trx_id=" + std::to_string(id), false};
}

DispatchOutcome CommandDispatcher::HandleRollback(Session& session) {
    if (!session.in_explicit_txn()) {
        return {"ERR no transaction is open", false};
    }
    if (const std::uint64_t expired = session.expired_trx_id(); expired != 0) {
        // The lifetime sweep already rolled it back (`Session::Expire`);
        // what is left to end is the session's failed state.
        session.Finish();
        return {"ROLLBACK trx_id=" + std::to_string(expired), false};
    }

    DispatchOutcome out = RollbackLocal(session);

    // The participants' abort notice retired (AT-S6); its history is at cf889b63.

    return out;
}

DispatchOutcome CommandDispatcher::RollbackLocal(Session& session) {
    txn::Transaction* txn = session.transaction();
    const std::uint64_t id = txn->id();

    // The reservations first (§6.2 step 5): each one removed from its
    // group, ASSERT_ROLLBACK logged, before the undo trail replays - so the
    // directory and the pages unwind in the same statement the rows do.
    //
    // **A failed unwind does not skip the rollback**, as `AbortOwnedScope`'s
    // does not: `AbortTxn` has already taken the pending list and given some
    // reservations back, so returning here left the transaction open with
    // its rows, and a `COMMIT` after it kept the rows against totals that no
    // longer counted them - an assertion admitting a violation.
    Status unwound = enforcer_->AbortTxn(page_store_, wal_, id);
    Status aborted = txn_->Abort(*txn, RowLocatorForRollback());
    if (!unwound.ok()) aborted = unwound;
    // Its catalog rows were retired by that abort, so there is nothing
    // left for anyone to be isolated from - and nothing that may still be
    // cached about them (DT3c, DT4). Before `Finish()`, which clears the
    // pointer this reads.
    EndDdlScope(session);
    session.Finish();
    txn_->Release(*txn);
    if (!aborted.ok()) return {ErrorReply(aborted), false, 0, aborted};
    return {"ROLLBACK trx_id=" + std::to_string(id), false};
}

// `USE <namespace>` - the namespace an unqualified relation name means for
// every later statement of the session (namespace.md NS8). A session
// statement, routed on `DispatchInner`'s tokens as `SET` is and for
// `HandleSetDurability`'s reason; never fingerprinted.
//
// Admitted inside a transaction: it changes what a name written later
// means, not what any view sees, and is not undone by a `ROLLBACK`. The
// namespace resolves under the session's view, so one this transaction
// created can be used. `sys` is refused: its relations are catalog views,
// read only qualified.
DispatchOutcome CommandDispatcher::HandleUse(std::string_view line, std::string_view args,
                                             Session& session) {
    // One trailing `;`, as every parsed statement takes.
    std::string_view name = Trim(args);
    if (!name.empty() && name.back() == ';') name = Trim(name.substr(0, name.size() - 1));
    // `args` is empty, with no position in `line`, when nothing follows the
    // verb: the refusal points past the line's end.
    const std::string_view whole = Trim(line);
    const auto byte = static_cast<std::uint32_t>(
        (name.empty() ? whole.data() + whole.size() : name.data()) - line.data());
    const auto refuse = [](Status s) { return DispatchOutcome{ErrorReply(s), false, 0, s}; };
    const auto at = " (byte " + std::to_string(byte) + ")";
    if (!parser::IsIdentifier(name)) {
        return refuse(Status::InvalidArgument("USE takes one namespace name" + at));
    }
    auto ns = ResolveNames(session, [&](const txn::ReadView* v) {
        return catalog_.FindNamespaceOidByName(name, v);
    });
    if (!ns.ok()) {
        if (ns.status().code() != StatusCode::kNotFound) return refuse(ns.status());
        return refuse(Status::NotFound("no namespace named '" + std::string(name) + "'" + at));
    }
    if (catalog::IsSystemNamespace(ns.value())) {
        return refuse(Status::InvalidArgument(
            "namespace '" + std::string(name) +
            "' holds the catalog, whose relations are read qualified as catalog views "
            "(sys.tables); it cannot be the session's namespace" + at));
    }
    session.set_default_namespace(ns.value());
    return {"USE namespace=" + std::string(name), false};
}

DispatchOutcome CommandDispatcher::HandleSetIsolation(std::string_view args, Session& session) {
    auto [level_word, name] = SplitFirstToken(args);
    if (!IEquals(level_word, "LEVEL")) {
        return {"ERR expected LEVEL after SET ISOLATION", false};
    }
    auto parsed = txn::ParseIsolationLevel(Trim(name));
    if (!parsed.ok()) return {ErrorReply(parsed.status()), false, 0, parsed.status()};

    // Applies to the *next* transaction, never the open one: changing what
    // a running transaction's read view means halfway through would make
    // its earlier statements unexplainable.
    if (session.in_explicit_txn()) {
        return {"ERR cannot change the isolation level inside a transaction", false};
    }
    session.set_isolation(parsed.value());
    return {std::string("SET isolation=") + txn::IsolationLevelName(parsed.value()), false};
}

// `SET DURABILITY {STRICT|GROUP|RELAXED}` - the session rung of §9's chain
// (protocol-wp.md P03).
//
// **Why this is not a `parser::Statement` arm**, which is what P03's row
// asks for. `SET ISOLATION LEVEL` and `SET CABIN_OPTIMIZER` were both built
// after that row was written, and neither reaches the parser: a session
// statement is routed here, on the tokens `DispatchInner` already split,
// and the AST has no `SET` arm for either. Adding one for `DURABILITY`
// alone would put a third session statement in a second place, so the
// engine would hold two models of what a session statement is and the
// grammar would describe one of them. P03's own acceptance survives that
// choice intact - "fingerprinting treats SET as non-pattern" is satisfied
// by a statement the fingerprinter never sees, which is the stronger
// version of the same guarantee (`FingerprintOf` excludes SET explicitly,
// and this spelling never gets that far).
//
// The class applies to the **next** transaction, never the open one, for
// `HandleSetIsolation`'s reason and one of its own: a transaction that has
// already logged writes under D3's window cannot be given D1's promise
// retroactively, because the window it spent is spent.
DispatchOutcome CommandDispatcher::HandleSetDurability(std::string_view args, Session& session) {
    auto parsed = wal::ParseDurabilityClass(Trim(args));
    if (!parsed.ok()) return {"ERR " + parsed.status().message(), false, 0, parsed.status()};
    if (session.in_explicit_txn()) {
        return {"ERR cannot change the durability class inside a transaction", false};
    }
    session.set_durability(parsed.value());
    return {std::string("SET durability=") + wal::DurabilityClassName(parsed.value()), false};
}

// ---- Snapshots and the write scope ---------------------------------------

StatusOr<txn::LeasedSnapshot> CommandDispatcher::SnapshotFor(Session& session) {
    if (txn_ == nullptr) return txn::LeasedSnapshot{};  // sees everything, as before

    if (session.in_explicit_txn()) {
        txn::Transaction* txn = session.transaction();
        // The statement boundary. Under READ COMMITTED this re-mints;
        // under REPEATABLE READ it is a no-op, and that one branch is the
        // whole difference between the levels.
        if (Status s = EnsureStatementBoundary(session); !s.ok()) return s;
        // No lease: the transaction is its own registration - `live_` is
        // what ReadHorizon() walks, and it holds this view until the
        // transaction resolves.
        txn::LeasedSnapshot out;
        out.snap = txn_->SnapshotFor(*txn);
        return out;
    }

    // Autocommit: a view over the committed state, owned by no
    // transaction, leased because that seam leases (manager.hpp says why,
    // and says plainly that
    // *this* holder's statement never parks with it: the dispatch path is
    // synchronous, and the statement drops this object before any wait).
    return txn::AutocommitSnapshot(txn_);
}

StatusOr<CommandDispatcher::WriteScope> CommandDispatcher::BeginWrite(Session& session) {
    WriteScope scope;
    scope.session = &session;
    if (txn_ == nullptr) return scope;  // no manager: kBootstrapXid, as before

    if (session.in_explicit_txn()) {
        scope.txn = session.transaction();
        scope.owned = false;
        if (Status s = EnsureStatementBoundary(session); !s.ok()) return s;
        // R6-5: where this statement's own writes begin in a trail that may
        // already hold earlier statements'. Taken after the statement
        // boundary, so a re-minted read view does not sit between the mark
        // and the writes it marks.
        statement_trail_mark_ = scope.txn->trail().size();
        return scope;
    }

    auto begun = txn_->Begin(session.isolation());
    if (!begun.ok()) return begun.status();
    scope.txn = begun.value();
    scope.owned = true;
    // An owned scope's trail starts empty, so this is 0 - written rather
    // than assumed, because the mark's meaning is "this statement's first
    // write" on both arms.
    statement_trail_mark_ = scope.txn->trail().size();
    return scope;
}

void CommandDispatcher::RefuseParkedWrite(DispatchOutcome& out, Session& session,
                                          const Status& refused, bool poison) {
    // **The wait ended badly and a mid-walk park still holds a scope open**
    // (AO-S3b). With no resume coming it is ended here, or never; through
    // `EndWrite`, so an owned scope aborts and releases and an explicit
    // transaction keeps its rows and poisons, like any failed statement.
    if (session.parked_write().has_value()) {
        const Session::ParkedWrite parked = *session.parked_write();
        session.clear_parked_write();
        WriteScope scope{parked.txn, parked.owned, &session};
        statement_trail_mark_ = parked.trail_mark;
        if (Status s = EndWrite(session, scope, refused); !s.ok() && logging(LogLevel::kWarn)) {
            log_->Warn("lock", "the scope of a refused mid-walk write did not end cleanly: " +
                                   s.message());
        }
    }
    out.parked_mid_walk = false;
    // A wait refused before it parked - the victim, or the net read at the
    // loop's top - still holds its unit's registration (AX-S2b).
    if (out.write_block.has_value() && out.write_block->slot != nullptr) {
        locks_->DropWake(out.write_block->key, out.write_block->slot);
    }
    out.write_block.reset();
    out.response = ErrorReply(refused);
    // The carried status, not only the rendered line - see the declaration.
    out.status = refused;
    // The poison `EndWrite` withheld while the wait was still possible.
    // Inside an explicit transaction this is a failed statement like any
    // other and the client must ROLLBACK; in autocommit the scope was
    // already unwound.
    if (poison && session.in_explicit_txn()) session.Poison();
    if (logging(LogLevel::kWarn)) log_->Warn("lock", refused.message());
}

void CommandDispatcher::NoteBlockingWriter(const txn::Transaction* waiter, std::uint64_t trx,
                                           std::uint64_t tuple_key, RepeatableReadWait rerun) {
    if (!may_park_ || clock_ == nullptr || txn_ == nullptr) return;
    // A relation-level refusal already parked this statement on the table's
    // slot (`BorrowChain`); a second wait on the same statement would be
    // one the outcome cannot carry.
    if (lock_wait_.has_value()) return;
    // **A holder the refusing unit's wake names is waited for even when it
    // is no longer in flight** (B6, AY-S3; `raft-marks-2026-09-29.md` §13).
    // A decide retires the transaction before it releases its borrows, so a
    // first encounter inside that window meets a unit still held by a
    // transaction `IsInFlight` calls decided. The refused ask has registered
    // its wake on that unit (`BorrowChain`), and the release flips it -
    // already, or imminently - so the park ends at the release rather than
    // the statement being refused for a wait that was about to end. A holder
    // with no wake behind it keeps the poll's test: nothing would flip a
    // slot for it, and a decided one has nothing left to wait for.
    const bool woken_at_release = blocking_wake_.has_value() && blocking_wake_->holder == trx;
    if (!woken_at_release && !txn_->IsInFlight(trx)) return;
    // The two guards the declaration argues for. A null `waiter` is
    // autocommit before its transaction is opened: it holds nothing and
    // will mint a fresh view, so both tests pass vacuously.
    if (waiter != nullptr) {
        // **Without a detector only a waiter holding nothing may wait**
        // (AO-S3): no cycle can then form. With a lock table the edge is
        // recorded before the park and a closing one is refused (AO-S4a).
        if (locks_ == nullptr && !waiter->trail().empty()) return;
        // A repeatable-read waiter is not parked where the re-run is futile
        // (`kFutile`: the holder wrote the row, whose commit its view cannot
        // see, or the site cannot tell). A holder of a unit that wrote
        // no version of the row is waited for (AO-S6d, item 17); if it
        // writes the row meanwhile the re-run is refused first-updater-wins
        // - the operator's ruling of 2026-09-09.
        if (waiter->isolation() == txn::IsolationLevel::kRepeatableRead &&
            rerun == RepeatableReadWait::kFutile) {
            return;
        }
    }
    blocking_writer_ = trx;
    blocked_tuple_key_ = tuple_key;
}

std::optional<Status> CommandDispatcher::BorrowRelationForDdl(txn::Transaction* holder,
                                                              catalog::Oid oid, bool poisons,
                                                              txn::LockMode mode) {
    if (locks_ == nullptr || holder == nullptr) return std::nullopt;

    // AR2 §3's DDL row: **the relation, `X`, for the DDL transaction**. It
    // meets a positioned reader's `IS` (AO-R12) and every writer's `IX`
    // (AT-S5e) by the intention rule, so no unit below it is looked at.
    const txn::LockKey unit = txn::LockKey::Relation(oid);
    std::uint64_t blocker = 0;
    txn::LockWake wake;
    // A wake is registered only where there is a reactor to park on - the
    // same condition every other wait in this file records under. Without
    // it the honest answer is the refusal itself, which is what a
    // synchronous `Dispatch()` has always had.
    auto took = locks_->TryAcquire(holder->id(), unit, mode, holder->borrows(), &blocker,
                                   may_park_ ? &wake : nullptr);
    if (!took.ok()) return took.status();
    if (took.value()) return std::nullopt;
    if (wake.slot != nullptr) {
        lock_wait_ =
            DispatchOutcome::LockWait{wake.key, blocker, std::move(wake.slot), poisons, mode};
    }
    // Retryable, and it says who rather than what to do: a client that
    // reached this over the synchronous path has the same recourse it has
    // for any conflict, and one that reached it over `DispatchAsync` never
    // sees it - the wait above swallows it and runs the statement again.
    return RelationHeld(oid, blocker);
}

std::uint64_t CommandDispatcher::NextReadHolder() noexcept {
    if (locks_ == nullptr) return 0;
    return ReadHolderId(core_id_, ++read_borrow_seq_);
}

void CommandDispatcher::TakeLockWait(std::optional<DispatchOutcome::LockWait>& into,
                                     DispatchOutcome::LockWait wait) {
    // The declaration carries the argument. Here only the order matters:
    // the outgoing registration is dropped **before** the new one is
    // installed, so a throw or an early return cannot leave two live -
    // unless it is the same slot: `TryAcquire` hands a same-key re-ask the
    // registration it already holds, and dropping that would erase the one
    // being kept and leave the wait on a slot nobody flips.
    if (into.has_value() && locks_ != nullptr && into->slot != wait.slot) {
        locks_->DropWake(into->key, into->slot);
    }
    into = std::move(wait);
}

StatusOr<bool> CommandDispatcher::BorrowChain(const WriteScope& scope,
                                             const txn::LockKey& unit,
                                             std::uint64_t* blocker, txn::LockMode mode) {
    if (locks_ == nullptr || scope.txn == nullptr) return true;
    txn::LockHoldings& holdings = scope.txn->borrows();
    const std::uint64_t id = scope.txn->id();

    // **The cap refuses the statement** (AO-S6c-c, AO-R10: a cap refuses
    // and never truncates). The ledger decides who may write, so a
    // truncated one would leave rows with no fence and no wait, silently.
    //
    // The detail is recorded rather than attached: a `Status` carries none,
    // so `DispatchOutcome::resource_detail` is how `kLockCap` reaches the
    // session that builds the wire error.
    const auto refused = [this](const Status& cap) -> Status {
        ++borrow_cap_stops_;
        last_refusal_ = cap;
        last_refusal_detail_ = static_cast<std::uint16_t>(wire::ResourceDetail::kLockCap);
        if (logging(LogLevel::kWarn)) {
            log_->Warn("lock", "core " + std::to_string(core_id_) +
                                   " refused a statement at max_locks_per_txn: " + cap.message());
        }
        return cap;
    };  // `Status`, not `StatusOr<bool>`: the cap has no granted arm to report.

    // **No unit without the intention above it.** `lock_table.hpp` states
    // the obligation and states its failure mode: a unit borrowed under a
    // relation entry this transaction has no `IX` on is invisible to a
    // relation-level ask, so the table answers a later `S`/`X` at the
    // relation "free" while a key beneath it is held - a wrong answer
    // given quietly rather than a refusal.
    //
    // **And the one ask here a DDL refuses** (AT-S5e). A relation `X` -
    // `DROP TABLE`, `CREATE`/`DROP INDEX`, a `CREATE ASSERTION`'s build -
    // refuses this `IX`, and that holder may be on any core. A refusal
    // here parks on the table's own slot, flipped by the release from
    // whichever core releases and followed by a kick -
    // `BorrowRelationForDdl`'s wait, from the other side.
    //
    // **`IS` over a shared unit** (D9(a), AY-S5): a foreign key's parent row
    // is read and held, not written, so the relation above it is declared
    // for reading - which a DDL's `X` refuses all the same (AY-Q4), and
    // another writer's `IX` does not.
    const txn::LockKey relation = txn::LockKey::Relation(unit.rel_oid);
    const bool first_intention = !holdings.Holds(relation);
    const txn::LockMode intention = mode == txn::LockMode::kShared
                                        ? txn::LockMode::kIntentionShared
                                        : txn::LockMode::kIntentionExclusive;
    txn::LockWake wake;
    auto rel = locks_->TryAcquire(id, relation, intention, holdings, blocker,
                                  may_park_ ? &wake : nullptr);
    if (!rel.ok()) return refused(rel.status());
    if (!rel.value()) {
        if (wake.slot != nullptr) {
            TakeLockWait(lock_wait_, DispatchOutcome::LockWait{wake.key, *blocker,
                                                               std::move(wake.slot), true,
                                                               intention});
        }
        return false;
    }
    // **The first intention is where the statement's memo has to be
    // current** (AT-S5e). The memo was settled at the task
    // boundary, and the `IX` is asked later - after the resolution, and for
    // a per-row write after a walk - so a `CREATE INDEX` or a `CREATE
    // ASSERTION` could take the relation `X`, publish and release in
    // between, leaving this statement to write rows into a relation whose
    // new index or assertion it never resolved. From this grant on no DDL
    // can take the relation until this transaction decides, so the word
    // unmoved here is the proof; moved, the statement has written nothing
    // under the relation yet and runs again through a boundary that
    // re-resolves - parked on a slot already flipped, so the re-run is
    // immediate. Conservative: any catalog write anywhere moves the word.
    if (first_intention && !catalog_.MemoIsCurrent()) {
        if (may_park_) {
            auto flipped = std::make_shared<txn::LockWaitSlot>();
            flipped->ready.store(true, std::memory_order_release);
            TakeLockWait(lock_wait_, DispatchOutcome::LockWait{relation, /*holder=*/0,
                                                               std::move(flipped), true,
                                                               intention});
        }
        return Status::TxnConflict("relation oid " + std::to_string(unit.rel_oid) +
                                   ": the catalog changed between this statement's resolution "
                                   "and its first write to the relation; it runs again against "
                                   "the current schema");
    }
    // A relation unit asks for the intention alone: it is a writer
    // declaring the relation it is about to write (`InsertParsed`), never a
    // DDL's claim on the object, which asks the table itself
    // (`BorrowRelationForDdl`) because it needs its own wait.
    if (unit.unit == txn::LockUnit::kRelation) return true;
    // **With a wake, where the statement can park** (AX-S2b): a refusal
    // leaves a registration so the holder's release kicks this core, which
    // the row wait - `NoteBlockingWriter`'s - had nothing of until then.
    // Kept on `blocking_wake_`, where the statement's end either hands it to
    // the wait or drops it.
    //
    // **On the unit that refused, which since AY-S2 need not be `unit`**: a
    // declared range refused by a row's `X`, or a row refused by a range
    // fence, is found by the table's verify in another unit's entry and is
    // registered there (`lock_table.hpp`, `AcquireResult::slot_key`). Until
    // then that refusal came back with no slot and the wait was a poll of
    // `IsInFlight` that no release kicked.
    txn::LockWake unit_wake;
    auto under = locks_->TryAcquire(id, unit, mode, holdings, blocker,
                                    may_park_ ? &unit_wake : nullptr);
    if (!under.ok()) return refused(under.status());
    if (!under.value() && unit_wake.slot != nullptr) {
        TakeLockWait(blocking_wake_, DispatchOutcome::LockWait{unit_wake.key, *blocker,
                                                               std::move(unit_wake.slot), true,
                                                               mode});
    }
    return under.value();
}

CommandDispatcher::TupleKeyWindow CommandDispatcher::FoldTupleKeyWindow(
    const catalog::TableAccess& access, const std::vector<parser::Condition>& where) const {
    TupleKeyWindow w;
    w.hi = kIdSpaceEnd;
    // No tuple key to compare against: nothing folds, and a caller refusing what
    // did not fold must see that (BH-S3's review, C4).
    if (access.schema.columns.empty()) {
        if (!where.empty()) w.unfolded = &where.front();
        return w;
    }
    // The first conjunct the fold could not read, which a caller may refuse.
    const auto unread = [&w](const parser::Condition& cond) {
        if (w.unfolded == nullptr) w.unfolded = &cond;
    };

    for (const parser::Condition& cond : where) {
        if (cond.kind == parser::PredicateKind::kBetween) {
            // The one kind `TupleKeyLiteral` deliberately declines, because it
            // carries two bounds rather than one. Its own guards, in the
            // same order and for the same reasons.
            if (cond.rhs_kind != parser::RhsKind::kLiteral ||
                cond.val.type != parser::ValueType::kInt ||
                cond.val_high.type != parser::ValueType::kInt || cond.val.int_val < 0 ||
                cond.val_high.int_val < 0 || parser::IntLiteralWrapped(cond.val) ||
                parser::IntLiteralWrapped(cond.val_high) ||
                !NamesTupleKey(access.schema, cond.col.name)) {
                unread(cond);
                continue;
            }
            // Inclusive at both ends (`ast.hpp`), and a range lock is half-open.
            w.lo = std::max(w.lo, static_cast<std::uint64_t>(cond.val.int_val));
            w.hi = std::min(w.hi, static_cast<std::uint64_t>(cond.val_high.int_val) + 1);
            w.bounded = true;
            continue;
        }
        const std::optional<std::uint64_t> v = TupleKeyLiteral(access, cond);
        if (!v.has_value()) {
            unread(cond);
            continue;
        }
        switch (cond.op) {
            case parser::CompareOp::kEq:
                w.lo = std::max(w.lo, *v);
                w.hi = std::min(w.hi, *v + 1);
                w.bounded = true;
                break;
            case parser::CompareOp::kGt:
                w.lo = std::max(w.lo, *v + 1);
                w.bounded = true;
                break;
            case parser::CompareOp::kGte:
                w.lo = std::max(w.lo, *v);
                w.bounded = true;
                break;
            case parser::CompareOp::kLt:
                w.hi = std::min(w.hi, *v);
                w.bounded = true;
                break;
            case parser::CompareOp::kLte:
                w.hi = std::min(w.hi, *v + 1);
                w.bounded = true;
                break;
            // `kNeq` names no window, and the two null tests name no id at
            // all. Left per-row rather than widened to the relation: a
            // predicate that does not bound the tuple key is exactly the shape the
            // mark leaves alone.
            case parser::CompareOp::kNeq:
            case parser::CompareOp::kIsNull:
            case parser::CompareOp::kIsNotNull:
            case parser::CompareOp::kLike:  // never flat (BU-S2); refused all the same
                unread(cond);
                break;
        }
    }
    return w;
}

std::optional<txn::LockKey> CommandDispatcher::DeclaredWriteBorrow(
    const catalog::TableAccess& access, const std::vector<parser::Condition>& where) const {
    if (locks_ == nullptr || access.schema.columns.empty()) return std::nullopt;

    // No predicate at all: the statement touches every row of the
    // relation, so **the whole id space** is what it declares.
    //
    // Not the relation *unit* (AO-S6e-b): that is the relation as an
    // **object** - what DDL claims and a positioned reader's `IS` covers -
    // while a write's claim is over **keys**, and on the relation entry it
    // would conflict with every reader's `IS` for rows MVCC already answers.
    if (where.empty()) return txn::LockKey::Range(access.oid, 0, kIdSpaceEnd);

    // Otherwise, the tuple key window the conjuncts name - if they name one.
    //
    // **The vector is an AND-list**, so a conjunct the fold cannot read can
    // only remove rows from the set, never add one: the window derived from
    // the tuple key conjuncts alone is a *superset* of what the statement writes,
    // and a borrow over a superset covers every row written. That is what
    // makes it sound to ignore `WHERE name = 'x'` sitting beside
    // `WHERE id < 50` - `unfolded` is `PURGE`'s question, not this one's.
    const TupleKeyWindow window = FoldTupleKeyWindow(access, where);
    const std::uint64_t lo = window.lo;
    const std::uint64_t hi = window.hi;
    if (!window.bounded) return std::nullopt;
    // An empty window writes nothing, so there is nothing to declare.
    if (lo >= hi) return std::nullopt;
    // **A window of one key stays per-row**: a fence would raise the
    // relation's fence counter and send every other writer through the
    // all-partition scan AO-R3 keeps them out of, for `WHERE id = k`.
    if (hi - lo <= 1) return std::nullopt;
    // `WHERE id >= 0` borrows as no `WHERE` does: a range, not the relation.
    return txn::LockKey::Range(access.oid, lo, hi);
}

Status CommandDispatcher::HeldByHolder(std::uint64_t tuple_key, std::uint64_t holder) {
    // `HolderName`, because a holder is not always a transaction: a read
    // borrow holds under an id of its own (AO-S6e-b).
    return Status::TxnConflict("row id=" + std::to_string(tuple_key) + " is held by " +
                               HolderName(holder));
}

std::optional<Status> CommandDispatcher::BorrowOrWait(const WriteScope& scope,
                                                      const txn::LockKey& unit,
                                                      RepeatableReadWait rerun,
                                                      txn::LockMode mode,
                                                      FunctionRef<Status()> before_wait) {
    std::uint64_t blocker = 0;
    auto took = BorrowChain(scope, unit, &blocker, mode);
    if (!took.ok()) return took.status();
    if (took.value() || blocker == 0) return std::nullopt;
    // Refused at the relation: parked on the slot `BorrowChain` registered,
    // not on the holder's decide - see there. (A relation unit is only ever
    // refused there; a narrower one refused there on a path that cannot
    // park is named by its rows below, which a DDL holding the relation
    // does hold.)
    //
    // **Tested by the unit the wait names, not by a wait existing**
    // (AT-S5f). A statement asks more than one relation's intention - its
    // own, and since AY-S5 each foreign key's parent relation - so a wait
    // left by one ask is not evidence about another: a presence test would
    // name the wrong relation and skip the `NoteBlockingWriter` a narrower
    // refusal owes. `BorrowChain` installs a relation unit and nothing
    // else, so the key is exactly the question this line means to ask.
    const bool refused_at_relation = lock_wait_.has_value() &&
                                     lock_wait_->key.unit == txn::LockUnit::kRelation &&
                                     lock_wait_->key.rel_oid == unit.rel_oid;
    if (refused_at_relation || unit.unit == txn::LockUnit::kRelation) {
        return RelationHeld(unit.rel_oid, blocker);
    }
    // **A refusal of a narrower unit that is not to become a wait**
    // (AO-S6c-c's rule; a named key's judgement since BD-R5): the caller's
    // judgement, asked before anything records it as one. The
    // unit's registration goes at the statement's end, which drops a wake no
    // blocker carries. A relation refusal above stays a wait: the relation's
    // holder decides whether the statement can resolve at all.
    if (before_wait) {
        if (Status final_answer = before_wait(); !final_answer.ok()) return final_answer;
    }

    // **A refused borrow stops the write** (AO-S6c-c). An insert has no
    // header to judge, so without this a range fence (item 14's declared
    // unit) would guard against rows that exist and not rows that appear.
    //
    // `NoteBlockingWriter` is the recorder every other wait in this file
    // uses and carries the same conditions - it records nothing outside
    // `DispatchAsync`'s `may_park_`, so a synchronous dispatch still gets
    // the plain refusal rather than a park nothing would resume.
    //
    // **`rerun` comes from the caller and not from `unit`** (AO-S6d,
    // item 17). The unit asked for says nothing about who refused it: an
    // `INSERT` asks for a tuple and a range fence is what stops it, and a
    // declared range can be stopped by a tuple holder that wrote the row.
    //
    // A key unit's interval is a hash, not tuple keys (BX-R3), so it names
    // no row to the wait and none to the client.
    if (unit.unit == txn::LockUnit::kIndexKey) {
        NoteBlockingWriter(scope.txn, blocker, /*tuple_key=*/0, rerun);
        return Status::TxnConflict("a key of a unique index of relation oid " +
                                   std::to_string(unit.rel_oid) + " is being written by " +
                                   HolderName(blocker));
    }
    NoteBlockingWriter(scope.txn, blocker, unit.lo, rerun);
    if (unit.unit == txn::LockUnit::kTuple) return HeldByHolder(unit.lo, blocker);
    return Status::TxnConflict("rows id=[" + std::to_string(unit.lo) + ", " +
                               std::to_string(unit.hi) + ") are held by " + HolderName(blocker));
}

void CommandDispatcher::UniqueKeyHolds::Release() {
    if (!units_.empty() && locks_ != nullptr && txn_ != nullptr) {
        locks_->ReleaseIf(txn_->id(), txn_->borrows(), [&](const txn::LockHoldings::Held& h) {
            return std::find(units_.begin(), units_.end(), h.key) != units_.end();
        });
    }
    units_.clear();
}

CommandDispatcher::UniqueKeysOfRow CommandDispatcher::UniqueKeysOf(
    const catalog::TableAccess& ta, std::span<const parser::AstValue> values,
    std::uint16_t first_col_pos, std::span<const parser::AstValue> previous) {
    UniqueKeysOfRow out;
    for (const catalog::TableAccess::IndexRef& ix : ta.indexes) {
        if (!ix.unique) continue;
        auto key = exec::EncodeUniqueKey(ta, ix, values, first_col_pos, previous);
        // **A key that does not encode is not checked**: the row cannot be
        // written with it - the row's own encode refuses a NULL in a NOT NULL
        // column or a mistyped literal, and the index append any value its
        // encoder refuses - so the write keeps the refusal it always had,
        // with its own wording and byte, rather than the key encoder's.
        if (!key.ok()) return UniqueKeysOfRow{};
        if (key.value().has_value()) out.emplace_back(&ix, std::move(*key.value()));
    }
    return out;
}

std::optional<Status> CommandDispatcher::CheckUniqueKeys(const WriteScope& scope,
                                                         const catalog::TableAccess& ta,
                                                         const UniqueKeysOfRow& keys,
                                                         std::uint64_t self,
                                                         UniqueKeyHolds& holds) {
    for (const auto& [ix, key] : keys) {
        // The refusal's byte: the first key value the statement wrote. A key
        // column an UPDATE did not assign carries 0, a decoded value's.
        std::uint32_t byte = 0;
        for (const parser::AstValue& v : key.values) {
            if (v.byte_offset != 0) {
                byte = v.byte_offset;
                break;
            }
        }
        // The unit before the check, held until the row's entry is in
        // (BX-Q2 (a)): two writers of one key cannot both find it free.
        // `kCapable`: the check reads latest state, never the waiter's view,
        // so a repeatable-read re-run is not futile.
        const txn::LockKey unit = exec::UniqueKeyUnit(ta, *ix, key);
        if (std::optional<Status> held =
                BorrowOrWait(scope, unit, RepeatableReadWait::kCapable)) {
            return held;
        }
        holds.Add(unit);

        // A busy writer that decided between the check and here is asked
        // again rather than waited for: nothing would flip a wait on a
        // transaction that is gone. Bounded, since each round is a decide.
        StatusOr<exec::UniqueVerdict> verdict = exec::UniqueVerdict{};
        for (int round = 0; round < kUniqueCheckRounds; ++round) {
            const txn::ReadView view = txn_ != nullptr
                                           ? txn_->MintCheckView(WriterId(scope))
                                           : txn::ReadView::Everything();
            verdict = exec::CheckUniqueKey(page_store_, ta, *ix, key, view, self);
            if (!verdict.ok()) return verdict.status();
            if (verdict.value().kind != exec::UniqueVerdict::Kind::kBusy ||
                txn_->IsInFlight(verdict.value().writer)) {
                break;
            }
        }
        switch (verdict.value().kind) {
            case exec::UniqueVerdict::Kind::kFree:
                break;
            case exec::UniqueVerdict::Kind::kHeld:
                return exec::UniqueViolation(ta, *ix, key, verdict.value().tuple_key, byte);
            case exec::UniqueVerdict::Kind::kBusy:
                // Its decide answers: a commit makes the key held, a rollback
                // frees it (BX-Q3 (a)). Waited for where nothing of the
                // statement is written yet; refused where something is.
                NoteBlockingWriter(scope.txn, verdict.value().writer, verdict.value().tuple_key,
                                   RepeatableReadWait::kCapable);
                return Status::TxnConflict(exec::UniqueKeyText(ta, *ix, key) +
                                           " is being written by " +
                                           HolderName(verdict.value().writer) + " in tuple key " +
                                           std::to_string(verdict.value().tuple_key));
        }
    }
    return std::nullopt;
}

Status CommandDispatcher::CheckWriteConflictBlocking(const WriteScope& scope, std::uint64_t cur,
                                                     std::uint64_t tuple_key,
                                                     std::uint64_t& blocker) {
    Status verdict = txn_->CheckWriteConflict(*scope.txn, cur, tuple_key);

    // **A refused borrow is a conflict in its own right** (AO-S6c-b). The
    // MVCC check answers about the row's last *writer* and is silent about
    // a transaction holding a **range** over the key that has not reached
    // the row yet - the lost update item 14's coarse units prevent.
    //
    // The message follows `CheckWriteConflict`'s, which `txn/manager.cpp`
    // calls part of the wire contract rather than a diagnostic: same shape,
    // and it names the holder rather than the writer because that is who
    // the client is waiting for.
    if (verdict.ok() && blocker != 0) verdict = HeldByHolder(tuple_key, blocker);
    if (verdict.ok()) return verdict;
    // **The row is held by an undecided writer, so the statement waits
    // instead of refusing** (AO-S3; AR2-A §1: a client retry loop is a
    // wait in the wrong place). Noted, not acted on: this is inside a page
    // span and a row callback, so `DispatchAsync` waits on the statement
    // once it is known that nothing was written.
    //
    // **The blocker is not a diagnostic**: it tells `EndWrite` to withhold
    // the poison, so recording one nothing will re-run would leave a failed
    // transaction committable (§6). `NoteBlockingWriter` holds every
    // condition on recording it.
    //
    // **The holder is the table's where the table names one, the header's
    // where it does not** (AO-S6c-b): the table names range holders the
    // header cannot. Still a union because a dispatcher with no lock table
    // grants every borrow vacuously, and reading the table alone would
    // delete its waits (`WithoutATableTheNarrowGuardIsWhatKeepsTheStageSafe`).
    //
    // Written back rather than kept local, because the write sites tell
    // this row's own recording from a leftover on the same member by
    // comparing against it - and what they must compare against is the
    // holder that was recorded, not the half of it the table supplied.
    //
    // **And the role is read before the fallback overwrites it**
    // (AO-S6d, item 17): a blocker the table named that is not `cur` is a
    // holder of a unit over this key which has written no version of it,
    // and that is the one case a repeatable-read waiter may wait on. Where
    // the table named nobody the blocker *becomes* `cur`, which is the
    // row's own writer and the case the exclusion still covers.
    const RepeatableReadWait rerun = (blocker != 0 && blocker != cur)
                                         ? RepeatableReadWait::kCapable
                                         : RepeatableReadWait::kFutile;
    if (blocker == 0) blocker = cur;
    NoteBlockingWriter(scope.txn, blocker, tuple_key, rerun);
    return verdict;
}

Status CommandDispatcher::EndWrite(Session& session, WriteScope& scope, const Status& result) {
    if (scope.txn == nullptr) {
        // No manager: every statement is its own transaction under
        // kBootstrapXid, and this is its end - so its assertion
        // reservations settle here, exactly as a real transaction's do
        // below. One statement at a time per registry is what makes the shared
        // id safe.
        return result.ok()
                   ? enforcer_->CommitTxn(page_store_, wal_, catalog::kBootstrapXid)
                   : enforcer_->AbortTxn(page_store_, wal_, catalog::kBootstrapXid);
    }

    // **R6-5: was this refusal one a wait could get past, and is the
    // statement still re-runnable?** Both halves, decided here because this
    // is the one place that sees the scope's trail after the statement ran.
    // A statement that wrote rows before it hit the conflict is not
    // re-runnable at any price - re-applying `SET v = v + 1` to the rows it
    // did write would be a second increment - so the blocker is dropped and
    // the client gets the conflict now. Only a statement that wrote nothing
    // reaches `DispatchAsync`'s wait.
    // (`scope.txn` is non-null here: the no-manager arm returned above.)
    if (blocking_writer_ != 0 &&
        (result.ok() || scope.txn->trail().size() != statement_trail_mark_)) {
        blocking_writer_ = 0;
        blocked_tuple_key_ = 0;
    }
    // **And the relation-lock wait (AO-S6e-b), on the same test**, its
    // registration dropped with it so it cannot outlive a statement that
    // will not be re-run.
    if (lock_wait_.has_value() &&
        (result.ok() || scope.txn->trail().size() != statement_trail_mark_)) {
        if (locks_ != nullptr) locks_->DropWake(lock_wait_->key, lock_wait_->slot);
        lock_wait_.reset();
    }
    // **A structural refusal from a statement that wrote nothing is re-run**
    // (BA-S14): `DispatchAsync` and `Dispatch` wait a turn and run it again,
    // so it does not poison - the refusal that ends the re-runs does, there.
    // Any other end - rows written (BA-R11's "who it reaches"), or a caller
    // that re-runs nothing (`ExecuteInsert`, `RefuseParkedWrite`) - clears
    // the note, so the refusal is answered as before. Not
    // a test on `result`'s code: the write paths hand this an
    // `InvalidArgument` verdict built from the rendered line.
    const bool structural_rerun = (may_park_ || reruns_structural_) && !result.ok() &&
                                  storage::StructuralRefusalNoted() &&
                                  scope.txn->trail().size() == statement_trail_mark_;
    if (!structural_rerun) storage::ClearStructuralRefusal();

    if (!scope.owned) {
        // Inside an explicit transaction. A failure does **not** unwind:
        // failure atomicity is per transaction, not per statement (section
        // 6), so the rows already written stay and the client must
        // ROLLBACK. That is the deviation from SQL savepoints would close.
        // An assertion violation poisons like any other write failure (the
        // AS9 resolution, assertion.md §4.4); the statement's
        // reservations stay pending and ROLLBACK's hook unwinds them with
        // everything else.
        //
        // **The failures that do not poison yet**: a write refused by an
        // undecided holder before it wrote anything (R6-5), a refused
        // relation borrow (AO-S6e-b), and a structural refusal due a re-run
        // (BA-S14). The transaction is as it was, and poisoning would turn
        // the wait `DispatchAsync` is about to take into a forced ROLLBACK;
        // the refusal that ends the wait poisons like any other.
        if (!result.ok() && blocking_writer_ == 0 && !lock_wait_.has_value()) {
            if (structural_rerun) {
                structural_poison_withheld_ = true;
            } else {
                session.Poison();
            }
        }
        return Status::OK();
    }

    // Autocommit: this statement is the whole transaction, so behaviour
    // here *is* statement-atomic - reservations included, on both arms.
    if (!result.ok()) {
        return AbortOwnedScope(scope, /*reported=*/Status::OK());
    }

    // Flags before the commit record (§6.2 step 4's piggyback): if the
    // commit then fails, the abort arm removes the entries whatever their
    // flags say, so clearing early is recoverable in both directions.
    //
    // **Both failures below unwind** (AO-S6d, item 15), through
    // `AbortOwnedScope`. Both are states `Abort` is defined for:
    // `CommitTxn` here only clears assertion entries, and
    // `TransactionManager::Commit` fails only ahead of its `PublishCommit`.
    //
    // **What the unwind cannot reach on the first arm**:
    // `AssertionEnforcer::CommitTxn` takes the reservation list out of
    // `pending_` before anything can fail, so a part-way failure strands
    // its unsettled reservations and the Bound Cabin counts a contribution
    // whose row was compensated. **Fail-closed** - an aggregate too high
    // refuses writes, never admits one it should refuse. Its bug entry was
    // closed unreproduced (`git show eeb88f47:docs/inflight/bugs/assertion-reservations-stranded-by-a-failed-settle.md`).
    if (Status s = enforcer_->CommitTxn(page_store_, wal_, scope.txn->id()); !s.ok()) {
        return AbortOwnedScope(scope, s);
    }
    const bool deferred = DefersCommit(scope.txn->id());
    auto committed = deferred ? txn_->CommitDeferred(*scope.txn)
                              : txn_->Commit(*scope.txn, effective_durability_);
    if (!committed.ok()) {
        return AbortOwnedScope(scope, committed.status());
    }
    // The session's next statement mints no snapshot below this (BA-R1c).
    session.set_acknowledged_commit_lsn(committed.value());
    if (deferred) {
        // Parked before its publish (BA-S7 part 2; `CommitLocal` says how).
        DeferCommit(scope.txn, committed.value());
        scope.txn = nullptr;
        return Status::OK();
    }
    if (wal_ != nullptr && effective_durability_ == wal::DurabilityClass::kGroup &&
        !wal_->IsDurable(committed.value())) {
        pending_commit_lsn_ = committed.value();
    }
    txn_->Release(*scope.txn);
    scope.txn = nullptr;
    return Status::OK();
}

Status CommandDispatcher::AbortOwnedScope(WriteScope& scope, const Status& reported) {
    const std::uint64_t id = scope.txn->id();

    // **The enforcer's failure does not skip the transaction's own unwind**
    // (AO-S6d): unsettled assertion entries say nothing about whether the
    // trail can be compensated. A no-op after a failed `enforcer_->CommitTxn`
    // (nothing pending is left; `EndWrite` says what that leaves), called
    // unconditionally because on the other two callers it is the whole
    // assertion unwind.
    Status unwound = enforcer_->AbortTxn(page_store_, wal_, id);
    if (Status s = txn_->Abort(*scope.txn, RowLocatorForRollback()); unwound.ok()) {
        unwound = s;
    }
    txn_->Release(*scope.txn);
    scope.txn = nullptr;

    // **The statement's answer is the failure it was called with**, not
    // the compensation's: a client told "the abort's record could not be
    // appended" would be reading about the recovery from its problem
    // instead of the problem. What the unwind could not finish is the
    // mount's undo phase to complete (`recovery_undo.hpp`), so it is
    // logged here and nowhere claimed to have succeeded.
    if (reported.ok()) return unwound;
    if (!unwound.ok() && logging(LogLevel::kWarn)) {
        log_->Warn("txn", "transaction " + std::to_string(id) +
                              " could not commit and its unwind did not complete either: " +
                              unwound.message());
    }
    return reported;
}

std::uint64_t CommandDispatcher::WriterId(const WriteScope& scope) {
    // No manager means no transaction, and every row carries the
    // always-visible id - which is exactly what the engine did before
    // MVCC, and why a dispatcher without one behaves identically.
    return scope.txn != nullptr ? scope.txn->id() : catalog::kBootstrapXid;
}

DispatchOutcome CommandDispatcher::HandleDelete(std::string_view line, Session& session) {
    WriteScope scope;
    txn::Snapshot snap;
    WalkCursor resume_from;
    exec::StatementContext context;  // NOW()'s instant, carried across a park (AP-S4)

    // AO-S3b's resume: `HandleUpdate`'s branch, for its reason.
    if (session.parked_write().has_value() && session.parked_write()->is_delete) {
        const Session::ParkedWrite& parked = *session.parked_write();
        scope = WriteScope{parked.txn, parked.owned, &session};
        snap = parked.snapshot;
        resume_from = parked.cursor;
        statement_trail_mark_ = parked.trail_mark;
        context = parked.context;
        statement_boundary_taken_ = true;  // `HandleUpdate`'s resume, for its reason
        session.clear_parked_write();
    } else {
        auto opened = BeginWrite(session);
        if (!opened.ok()) return {ErrorReply(opened.status()), false, 0, opened.status()};
        scope = opened.value();
        context = StatementContextFor(session);

        // After `BeginWrite`: `HandleUpdate`'s order, for its reason (Census A row 4).
        auto snapshot = SnapshotFor(session);
        if (!snapshot.ok()) return {ErrorReply(snapshot.status()), false, 0, snapshot.status()};
        snap = snapshot.value().snap;
    }

    DispatchOutcome out = DeleteInner(line, scope, snap, resume_from, context);

    // AO-S3b's park: `HandleUpdate`'s arm, for its reason. Ahead of
    // `EndWrite` below because that *ends* the scope and this must not - a
    // DELETE that has already marked rows has nothing to roll them back to.
    if (out.parked_mid_walk) {
        session.set_parked_write(Session::ParkedWrite{scope.txn, scope.owned, snap,
                                                      out.walk_cursor, statement_trail_mark_,
                                                      /*is_delete=*/true, context});
        return out;
    }
    out = WithReturning(line, session, scope, std::move(out));


    const bool failed = out.response.rfind("ERR ", 0) == 0;
    Status verdict = failed ? Status::InvalidArgument(out.response) : Status::OK();
    if (Status s = EndWrite(session, scope, verdict); !s.ok() && !failed) {
        return {ErrorReply(s), false, 0, s};
    }
    return out;
}

namespace {

// PU10's conjunct rows: a negative literal on the tuple key is simply wrong, and
// every other conjunct the fold could not read is a form nobody built. The
// byte is the conjunct's function, column or literal - or, for a conjunct
// with no position of its own (a bare subquery), the `WHERE` token.
Status PurgeConjunctRefusal(const catalog::TableAccess& access, const parser::Condition& cond,
                            std::uint32_t where_byte) {
    // A name matching two live columns ignoring case is why the fold could
    // not read it (`NamesTupleKey`), and PURGE has no compiler behind it to
    // say so: the refusal is the ambiguity, not a form nobody built.
    bool on_tuple_key = false;
    if (!cond.col.name.empty()) {
        auto found = access.schema.FindColumn(cond.col.name, cond.col.byte_offset);
        if (!found.ok()) return found.status();
        on_tuple_key = found.value() != nullptr && found.value()->pos == 0;
    }
    const bool literal = cond.rhs_kind == parser::RhsKind::kLiteral &&
                         (cond.kind == parser::PredicateKind::kCompareValue ||
                          cond.kind == parser::PredicateKind::kBetween);
    if (on_tuple_key && literal) {
        for (const parser::AstValue* v : {&cond.val, &cond.val_high}) {
            if (parser::IntLiteralWrapped(*v)) {
                return Status::InvalidArgument(
                    "tuple key literal " + v->raw_int_text +
                    " is outside the Keystone id space [" +
                    std::to_string(catalog::kFirstRowId) + ", " + std::to_string(kMaxKeystoneId) +
                    "] (byte " +
                    std::to_string(v->byte_offset) + ")");
            }
            if (v->type == parser::ValueType::kInt && v->int_val < 0) {
                return Status::InvalidArgument(
                    "tuple key literal " + std::to_string(v->int_val) +
                    " is negative, and no key is below 1 (byte " + std::to_string(v->byte_offset) +
                    ")");
            }
            if (cond.kind != parser::PredicateKind::kBetween) break;
        }
    }
    const std::uint32_t at = cond.lhs_fn.has_value() ? cond.lhs_fn->byte_offset
                             : cond.tree != nullptr   ? cond.tree->byte_offset
                             : !cond.col.name.empty() ? cond.col.byte_offset
                                                      : where_byte;
    return Status::NotImplemented(
        "PURGE admits only comparisons of the tuple key with an integer literal (=, <, <=, >, "
        ">=, BETWEEN); this conjunct is not one (byte " +
        std::to_string(at) + ")");
}

}  // namespace

DispatchOutcome CommandDispatcher::HandlePurge(std::string_view line, Session& session) {
    const auto refuse = [](const Status& s) { return DispatchOutcome{ErrorReply(s), false, 0, s}; };

    // PU9, BH-Q4: autocommit only, refused before anything opens, so the
    // transaction it meets is not poisoned.
    if (session.in_explicit_txn()) {
        return refuse(Status::NotImplemented(
            "PURGE inside a transaction is not implemented; run it in autocommit (byte 0)"));
    }
    auto parsed = parser::Parse(line);
    if (!parsed.ok()) return refuse(parsed.status());
    if (!std::holds_alternative<parser::PurgeStmt>(parsed.value())) {
        return {"ERR expected a PURGE statement", false};
    }
    const auto& stmt = std::get<parser::PurgeStmt>(parsed.value());
    const std::string at_name = " (byte " + std::to_string(stmt.table_byte_offset) + ")";

    // A system relation never resolves (`Catalog::ResolveRelation` refuses
    // `sys`), so the catalog's marks - the engine's own purge - are out of
    // reach before anything calls `InitTableAccess`, which has no columns
    // for one.
    auto oid = ResolveNames(session, [&](const txn::ReadView* v) {
        return catalog_.ResolveRelation(stmt.schema, stmt.table_name,
                                        session.default_namespace(), stmt.table_byte_offset, v);
    });
    if (!oid.ok()) return refuse(oid.status());
    ReadBorrow borrow(locks_, NextReadHolder(), &read_borrows_, oid.value());  // IS, AT-R1
    auto access = catalog_.InitTableAccess(oid.value());
    if (!access.ok()) return refuse(access.status());
    const catalog::TableAccess& ta = *access.value();
    // BZ-R4: a heap's tuple key is hidden, and PURGE's window is written
    // in tuple key comparisons, so there is no window a statement can name.
    if (ta.schema.heap()) {
        return refuse(Status::Unsupported("'" + stmt.table_name +
                                          "' is a heap, whose tuple key is hidden; PURGE's "
                                          "window names tuple keys, so it frees none on a heap" +
                                          at_name));
    }
    // BV-Q4 (a): a freed key could be inserted again in an initial state,
    // restarting a row's history under its old identity - the one way past
    // the induction a Transition's path property rests on (CN-4 §2, §9.1).
    // Refused, so a key on a governed relation stays bound for life. A
    // Transition created after this read takes the relation `X`, which the
    // purge's `IX` waits for, and the memo check re-runs this statement.
    if (!ta.transitions.empty()) {
        return refuse(Status::Unsupported(
            "'" + stmt.table_name + "' carries transition '" + ta.transitions.front().name +
            "', whose state history a freed key would restart; PURGE frees no key of a "
            "relation that carries a Transition" +
            at_name));
    }

    // PU1: the one recognizer of the window, refusing what it cannot read
    // rather than skipping it (BH-R2), clamped to the key space.
    const TupleKeyWindow window = FoldTupleKeyWindow(ta, stmt.where);
    if (window.unfolded != nullptr) {
        return refuse(PurgeConjunctRefusal(ta, *window.unfolded, stmt.where_byte_offset));
    }
    const std::uint64_t lo = std::max(window.lo, catalog::kFirstRowId);
    const std::uint64_t hi = std::min(window.hi, kIdSpaceEnd);
    if (lo >= hi) return {"PURGED 0", false, 0};  // names no key

    // **An owned transaction per attempt** (BH-R5): a re-run after a wait
    // begins afresh, so the view it registers never predates the deleter
    // it waited for, and between attempts it holds nothing - no view to
    // keep the horizon down, no `IX` to keep a DDL out.
    auto opened = BeginWrite(session);
    if (!opened.ok()) return refuse(opened.status());
    WriteScope scope = opened.value();
    // `purged` keys are written whatever the decide does - their records
    // are no transaction's - so a commit that fails after them still says
    // how many (PU6).
    const auto partly = [](const Status& s, std::uint64_t purged) {
        return s.WithMessage(s.message() + "; this PURGE purged " + std::to_string(purged) +
                             " key(s) of its window before it, and running it again finishes "
                             "the window");
    };
    const auto ended = [&](DispatchOutcome out, std::uint64_t purged = 0) {
        const Status verdict = out.status.ok() && out.response.rfind("ERR", 0) != 0
                                   ? Status::OK()
                                   : (out.status.ok() ? Status::InvalidArgument(out.response)
                                                      : out.status);
        if (Status s = EndWrite(session, scope, verdict); !s.ok() && verdict.ok()) {
            return refuse(purged == 0 ? s : partly(s, purged));
        }
        return out;
    };

    // BH-R4: the relation's `IX` and no row or range unit. Held to the end
    // of the attempt, through the last spill release, because `CREATE
    // INDEX`'s backfill and an assertion build are safe only while their
    // `X` keeps this out (Census A).
    if (std::optional<Status> held =
            BorrowOrWait(scope, txn::LockKey::Relation(ta.oid), RepeatableReadWait::kCapable)) {
        return ended(refuse(*held));
    }

    // ---- Phase 1: judge the whole window, writing nothing (BH-R6) ------
    //
    // BH-R3: a target is a delete-marked slot whose deleter is decided and
    // resolved for every reader. A slot whose latest writer is in flight,
    // or whose deleter some reader cannot see yet, is a wait (PU4, PU5) -
    // the first one found ends the walk, since the attempt re-runs whole.
    struct Target {
        std::uint64_t tuple_key;
        std::uint64_t deleter;
    };
    std::vector<Target> targets;
    std::optional<DispatchOutcome::PurgeWait> pending;
    bool one_key_live = false;
    const bool one_key = hi - lo == 1;
    auto first = btree::BtreeSeekLeaf(page_store_, ta.desc_page_id, lo);
    if (!first.ok()) return ended(refuse(first.status()));
    Status walked = btree::BtreeVisitFrom(
        page_store_, first.value(), storage::PageAccess::kRead,
        [&](PageId, heap::PageView& page, std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            auto tuple = page.ReadTuple(slot);
            if (!tuple.ok()) {
                if (tuple.status().code() == StatusCode::kNotFound) {
                    return storage::VisitControl::kContinue;  // a retired slot: absent
                }
                return tuple.status();
            }
            auto tuple_key = KeystoneIdOfPayload(tuple.value().payload);
            if (!tuple_key.ok()) return tuple_key.status();
            if (tuple_key.value() < lo) return storage::VisitControl::kContinue;
            if (tuple_key.value() >= hi) return storage::VisitControl::kStop;
            const std::uint64_t trx = tuple.value().trx_id;
            const bool in_flight = txn_ != nullptr && txn_->IsInFlight(trx);
            if (tuple.value().deleted) {
                if (in_flight) {
                    pending = DispatchOutcome::PurgeWait{tuple_key.value(), trx, /*horizon=*/false};
                    return storage::VisitControl::kStop;
                }
                if (txn_ != nullptr && !txn_->ResolvedForEveryReader(trx)) {
                    pending = DispatchOutcome::PurgeWait{tuple_key.value(), trx, /*horizon=*/true};
                    return storage::VisitControl::kStop;
                }
                targets.push_back({tuple_key.value(), trx});
                return storage::VisitControl::kContinue;
            }
            // PU3, BH-Q2: a live key refuses a window of one key - once its
            // writer has decided, since an undecided insert may yet roll
            // back - and a wider window passes over it.
            if (one_key) {
                if (in_flight) {
                    pending = DispatchOutcome::PurgeWait{tuple_key.value(), trx, /*horizon=*/false};
                } else {
                    one_key_live = true;
                }
                return storage::VisitControl::kStop;
            }
            return storage::VisitControl::kContinue;
        });
    if (!walked.ok()) return ended(refuse(walked));
    if (one_key_live) {
        const std::uint32_t at = stmt.where.front().val.byte_offset;
        return ended(refuse(Status::InvalidArgument(
            "tuple key " + std::to_string(lo) +
            " is live; PURGE frees only a key whose row was deleted (byte " + std::to_string(at) +
            ")")));
    }
    if (pending.has_value()) {
        const std::string why =
            pending->horizon
                ? "a snapshot older than its DELETE's commit is still open"
                : "its latest writer, transaction " + std::to_string(pending->trx_id) +
                      ", has not decided";
        DispatchOutcome out = ended(refuse(Status::TxnConflict(
            "PURGE cannot free tuple key " + std::to_string(pending->tuple_key) + " yet: " + why +
            "; nothing was purged, and running it again once that ends will")));
        out.purge_wait = pending;
        return out;
    }

    if (purge_seam_for_test_) purge_seam_for_test_();

    // ---- Phase 2: each key whole, in key order (BH-R6, BH-R7) ----------
    //
    // Drain mode, as a rollback's: a pass that is not one row's mutation and
    // holds no latch between its keys. A refusal now keeps every key already
    // purged - each is whole - and says so (PU6, BH-Q5).
    std::uint64_t purged = 0;
    {
        const storage::DrainOnPressure drain(page_store_);  // the keys, not the decide
        for (const Target& t : targets) {
            auto one = exec::PurgeKey(page_store_, wal_, ta, t.tuple_key, t.deleter);
            if (!one.ok()) return ended(refuse(partly(one.status(), purged)));
            if (one.value() == exec::PurgeOutcome::kPurged) ++purged;
        }
    }
    return ended({"PURGED " + std::to_string(purged), false, purged}, purged);
}

DispatchOutcome CommandDispatcher::DeleteInner(std::string_view line, WriteScope& scope,
                                               const txn::Snapshot& snapshot,
                                               WalkCursor resume_from,
                                               const exec::StatementContext& context) {
    // AO-S3b, and `UpdateInner`'s site states the argument.
    bool parked_on_row = false;
    Status blocked_verdict;
    auto parsed = parser::Parse(line);
    if (!parsed.ok()) return {ErrorReply(parsed.status()), false, 0, parsed.status()};
    if (!std::holds_alternative<parser::DeleteStmt>(parsed.value())) {
        return {"ERR expected a DELETE statement", false};
    }
    const auto& stmt = std::get<parser::DeleteStmt>(parsed.value());

    // As INSERT and UPDATE (DT3c): a write resolves under its session.
    auto oid = ResolveNames(scope.session, [&](const txn::ReadView* v) {
        // The statement's namespace, carried across a park with its NOW().
        return catalog_.ResolveRelation(stmt.schema, stmt.table_name, context.default_namespace,
                                        stmt.table_byte_offset, v);
    });
    if (!oid.ok()) return {ErrorReply(oid.status()), false, 0, oid.status()};
    ReadBorrow borrow(locks_, NextReadHolder(), &read_borrows_, oid.value());  // AT-R1

    auto access = catalog_.InitTableAccess(oid.value());
    if (!access.ok()) return {ErrorReply(access.status()), false, 0, access.status()};
    const catalog::TableAccess& ta = *access.value();

    // Before anything is marked: UPDATE's rule and UPDATE's reason. A
    // delete-mark is a write.
    if (Status admitted = CheckWriteAdmission(ta); !admitted.ok()) {
        return {ErrorReply(admitted), false, 0, admitted};
    }

    // The same WHERE compilation UPDATE uses, so a DELETE's predicate means
    // exactly what the SELECT that found the rows meant.
    auto predicates = ResolveNames(scope.session, [&](const txn::ReadView* v) {
        return exec::CompileWhere(catalog_, ta, stmt.table_name, stmt.where, v, &borrow,
                                  &context);
    });
    if (!predicates.ok()) return {ErrorReply(predicates.status()), false, 0, predicates.status()};
    const std::vector<const catalog::Schema*> schemas = {&ta.schema};
    exec::ChainFrame frame;
    frame.Open(schemas, /*parent=*/nullptr);

    // Per-row scratch owned by the statement, as UPDATE's site explains:
    // cleared and refilled per scanned row, allocating only on the first.
    std::vector<std::byte> payload_copy;
    std::vector<exec::PendingSpill> frame_spills;
    const std::uint64_t filter_mask = predicates.value().filter_columns;
    bool conjuncts_checked = false;
    // The collected targets, as UPDATE's (`WalkCursor::targets`).
    std::shared_ptr<const std::unordered_set<std::uint64_t>> targets = resume_from.targets;

    // ---- Stage 1 of the walk -------------------------------------------
    //
    // Only what the WHERE reads (OPT-001): the row's tuple key if a version is
    // visible under the snapshot and qualifies, else nothing. DELETE walks
    // the whole relation exactly as UPDATE does, so it decoded every row of
    // it to mark one; the mask is the compiler's, and is kAllColumns
    // wherever a partial decode would be unsound. `payload_copy` holds the
    // row's bytes afterwards - copied out of the page before anything can
    // fetch another one, the same reason UPDATE's apply() copies - and the
    // version's ids are handed out for the mark that needs them.
    auto qualifies = [&](heap::PageView& page, std::uint16_t slot, std::uint64_t* trx_id_out,
                         std::uint64_t* undo_ptr_out) -> StatusOr<std::optional<std::uint64_t>> {
        auto tuple = page.ReadTuple(slot);
        if (!tuple.ok()) return std::optional<std::uint64_t>{};  // dead or out-of-range slot
        // Already delete-marked, or invisible to this reader: either way
        // there is no version here to delete.
        if (txn::Classify(snapshot.view, tuple.value()) == txn::Visibility::kNoVersion) {
            return std::optional<std::uint64_t>{};
        }
        payload_copy.assign(tuple.value().payload.begin(), tuple.value().payload.end());
        auto id = KeystoneIdOfPayload(payload_copy);
        if (!id.ok()) return id.status();
        // Not a collected target: nothing to decode.
        if (targets != nullptr && !targets->contains(id.value())) {
            return std::optional<std::uint64_t>{};
        }
        if (trx_id_out != nullptr) *trx_id_out = tuple.value().trx_id;
        if (undo_ptr_out != nullptr) *undo_ptr_out = tuple.value().undo_ptr;
        if (Status s = exec::DecodeAndResolve(page_store_, ta.schema, ta.layout, payload_copy,
                                              frame.SlotsFor(0), filter_mask, frame_spills);
            !s.ok()) {
            return s;
        }
        // Checked once at the first row, as UPDATE's (BT-R5).
        if (!conjuncts_checked) {
            if (Status s = exec::CheckConjuncts(schemas, predicates.value(), frame);
                !s.ok()) {
                return s;
            }
            conjuncts_checked = true;
        }
        if (targets != nullptr) return std::optional<std::uint64_t>{id.value()};
        auto matched = exec::EvaluateConjuncts(catalog_, page_store_, schemas, predicates.value(),
                                               frame, /*stats=*/nullptr, budget_, &snapshot);
        if (!matched.ok()) return matched.status();
        if (!matched.value()) return std::optional<std::uint64_t>{};
        return std::optional<std::uint64_t>{id.value()};
    };
    // The reverse hoist retired with the fan-out (AT-S5f); its history is
    // at cf889b63. The check runs per row inside the walk, because a walk
    // that sees every chain has nothing to ask anyone - and it mints its
    // own view, after the row's `X` (`CheckNoChildrenBeforeDelete`).

    std::uint32_t deleted = resume_from.rows_done;  // AO-S3b, as UPDATE

    // The unit this write declares, as UPDATE - `UpdateInner`'s site
    // states the argument.
    const std::optional<txn::LockKey> declared = DeclaredWriteBorrow(ta, stmt.where);
    if (declared.has_value()) {
        // `kFutile`, and `UpdateInner`'s site states the argument.
        if (std::optional<Status> held =
                BorrowOrWait(scope, *declared, RepeatableReadWait::kFutile)) {
            return {ErrorReply(*held), false, 0, *held};
        }
    }

    auto mark = [&](PageId page_id, heap::PageView& page, std::uint16_t slot) -> Status {
        std::uint64_t trx_id = 0;
        std::uint64_t undo_ptr = 0;
        auto qualified = qualifies(page, slot, &trx_id, &undo_ptr);
        if (!qualified.ok()) return qualified.status();
        if (!qualified.value().has_value()) return Status::OK();
        const std::uint64_t id = *qualified.value();

        // ---- Stage 2: complete the row that qualified -------------------
        //
        // **The frame is read after this point** - the assertion enforcer
        // takes the departing row's values from it - so a masked row must
        // be finished before anything downstream sees it, or a slot outside
        // the mask would hand over the *previous* row's value. The step VM
        // completes a matched row with the same complement decode.
        //
        // The complement needs no width arithmetic of its own: the decoder
        // clamps the mask to the relation's column count, so `~filter_mask`
        // names the rest and nothing beyond it. Deliberately not gated on
        // whether an assertion exists - that would couple the decode to one
        // downstream reader, and the next reader added here would silently
        // get stale slots.
        if (filter_mask != exec::kAllColumnsMask) {
            if (Status s = exec::DecodeAndResolve(page_store_, ta.schema, ta.layout, payload_copy,
                                                  frame.SlotsFor(0), ~filter_mask, frame_spills);
                !s.ok()) {
                return s;
            }
        }

        if (scope.txn != nullptr) {
            // The borrow first, `UpdateInner`'s site states the order.
            std::uint64_t blocker = 0;
            if (!declared.has_value()) {
                auto took = BorrowChain(scope, txn::LockKey::Tuple(ta.oid, id), &blocker);
                if (!took.ok()) return took.status();
            }
            if (Status s = CheckWriteConflictBlocking(scope, trx_id, id, blocker); !s.ok()) {
                // AO-S3b, and `UpdateInner`'s site states the argument -
                // including why this is an equality, and why it is against
                // the lock's blocker rather than the header's writer.
                if (blocker != 0 && blocking_writer_ == blocker) {
                    parked_on_row = true;
                    blocked_verdict = s;
                    return Status::OK();
                }
                return s;
            }
        }

        // ---- The Transition check (BV-R3) ------------------------------
        //
        // The departing row's state must be a deletable one. After the
        // conflict check - the frame is the head this writer holds - and
        // before the reverse check, which may wait: a refusal the row alone
        // decides waits for nobody.
        if (!ta.transitions.empty()) {
            if (Status s = exec::CheckTransitionDelete(ta, frame.SlotsFor(0), id); !s.ok()) {
                return s;
            }
        }

        // ---- The reverse check (docs/spec/foreign-keys.md §3) ----------
        //
        // RESTRICT: a row still referenced may not be deleted. Run per
        // qualifying row, since the id being deleted *is* the value every
        // child is checked against, and before the mark - the same
        // check-before-write ordering INSERT uses, and here it also means a
        // refused delete leaves no undo record behind.
        if (!ta.fkeys_in.empty()) {
            if (Status s = CheckNoChildrenBeforeDelete(ta, id, scope); !s.ok()) {
                // AY-Q8: a child row whose writer has not decided parks the
                // walk here, as a held row does above - nothing of this row
                // is written yet. The check recorded the writer; a held row
                // above would have returned before reaching it.
                if (blocking_writer_ != 0) {
                    parked_on_row = true;
                    blocked_verdict = s;
                    return Status::OK();
                }
                return s;
            }
        }

        // ---- The assertion departure (§4.2's DELETE row) ----------------
        //
        // Check-free (AS11: strictly decreasing cannot violate an upper
        // bound) but **not** maintenance-free: §5's coverage contract is
        // "100% of live rows", and a header that kept counting deleted rows
        // would overstate forever - nothing prunes - refusing valid writes
        // without bound. The departure entry is what keeps
        // header == Σ(entries) true while the aggregate goes down. Before
        // the undo record, so a failure here leaves none behind.
        if (enforcer_->AnyOn(ta.oid)) {
            if (Status s = enforcer_->ReserveDelete(page_store_, wal_, WriterId(scope), ta.oid,
                                                   frame.SlotsFor(0), id, page_id, slot);
                !s.ok()) {
                return s;
            }
        }

        std::uint64_t new_trx_id = trx_id;
        std::uint64_t new_undo_ptr = undo_ptr;
        if (scope.txn != nullptr) {
            // **An empty image.** A delete-mark changes no tuple bytes, so
            // there are none to restore; stepping back over this record
            // keeps whatever payload the reader already had (section 4.3).
            txn::UndoRecordFields rec{};
            rec.prior_trx_id = trx_id;
            rec.prior_undo_ptr = undo_ptr;
            rec.target_page_id = page_id;
            rec.target_slot = slot;
            rec.type = static_cast<std::uint8_t>(txn::UndoRecordType::kDeleteMark);

            auto ptr = txn_->AppendUndo(*scope.txn, rec, id, {});
            if (!ptr.ok()) return ptr.status();
            new_trx_id = scope.txn->id();
            new_undo_ptr = ptr.value();

            txn_->NoteDeleteMark(*scope.txn, ta.oid, page_id, slot, id, trx_id, undo_ptr);
        }

        auto again = page_store_.Get(page_id);
        if (!again.ok()) return again.status();
        heap::PageView fresh(again.value().bytes());

        // Two writes, and both are needed: the header carries the link back
        // to the version this supersedes, and the slot flag is what makes
        // the row gone for newer readers. DeleteMark re-stamps the writer,
        // so the header write goes first.
        auto reread = fresh.ReadTuple(slot);
        if (!reread.ok()) return reread.status();
        const std::vector<std::byte> same(reread.value().payload.begin(),
                                          reread.value().payload.end());
        if (Status s = fresh.OverwriteTuple(slot, same, new_trx_id, new_undo_ptr); !s.ok()) {
            return s;
        }
        if (Status s = fresh.DeleteMark(slot, new_trx_id); !s.ok()) return s;

        if (wal_ != nullptr && scope.txn != nullptr) {
            std::array<std::byte, wal::kDeleteMarkPayloadSize> buf{};
            const wal::HeapDeleteMarkPayload fields{new_trx_id, slot};
            if (auto n = wal::EncodeHeapDeleteMark(buf, fields); !n.ok()) return n.status();
            auto rec = wal_->Append(
                wal::RecordSpec{wal::RecordType::kHeapDeleteMark, scope.txn->id(), page_id}, buf);
            if (!rec.ok()) return rec.status();
            if (Status s = page_store_.StampPageLsn(page_id, rec.value()); !s.ok()) return s;
        }

        // No Cabin write hook, and no index one either: removal is
        // forbidden (cabin.md section 5, index.md IX2), because an
        // older snapshot may still match this row through the undo chain.
        // The entry stays and the read-time check subtracts it - which is
        // the visibility predicate as well as the key re-check.
        //
        // Stated rather than left as an omission: a DELETE calling
        // MaintainIndexes with nothing to do would read as maintenance that
        // happens to be empty, when the truth is that maintenance here would
        // be a defect.

        ++deleted;
        return Status::OK();
    };

    // The same point-lookup fast path SELECT and UPDATE take, and the same
    // contract: the locator picks the slot to look at, never which rows
    // match, so falling through to the scan produces the identical answer.
    // Its target is the WHERE's only conjunct, so it carries no subquery.
    if (std::optional<std::uint64_t> tuple_key = TupleKeyEqualityTarget(ta, stmt.where); tuple_key.has_value()) {
        TupleKeyLookup found = LocateByTupleKey(ta, *tuple_key);
        if (found.kind == TupleKeyLookup::Kind::kAbsent) {
            return {"DELETED 0", false, 0};
        }
        if (found.kind == TupleKeyLookup::Kind::kAt) {
            // Through the held leaf, as UPDATE's point arm (btree.hpp `Location`).
            heap::PageView page(found.at.leaf.bytes());
            if (Status s = mark(found.at.page_id, page, found.at.slot); !s.ok()) {
                return {ErrorReply(s), false, 0, s};
            }
            // AO-S3b, and UPDATE's point arm states the argument: a
            // held row leaves `mark` answering OK with nothing marked,
            // and rendering the count would answer `DELETED 0` for a
            // row this statement is waiting on.
            if (parked_on_row) {
                return {ErrorReply(blocked_verdict), false, 0, blocked_verdict};
            }
            return {"DELETED " + std::to_string(deleted), false, deleted};
        }
    }

    // The collection pass, and `UpdateInner`'s site states the argument.
    if (targets == nullptr && !predicates.value().sub_chains.empty()) {
        auto collected = std::make_shared<std::unordered_set<std::uint64_t>>();
        Status pass = VisitRelation(
            ta, storage::PageAccess::kRead,
            [&](PageId, heap::PageView& page,
                std::uint16_t slot) -> StatusOr<storage::VisitControl> {
                auto qualified = qualifies(page, slot, nullptr, nullptr);
                if (!qualified.ok()) return qualified.status();
                if (qualified.value().has_value()) collected->insert(*qualified.value());
                return storage::VisitControl::kContinue;
            },
            /*cursor=*/nullptr);
        if (!pass.ok()) return {ErrorReply(pass), false, 0, pass};
        if (collected->empty()) return {"DELETED " + std::to_string(deleted), false, deleted};
        targets = std::move(collected);
    }

    // AO-S3b, and `UpdateInner`'s site states the argument.
    WalkCursor walk_cursor = resume_from;
    walk_cursor.targets = targets;
    Status scan = VisitRelation(
        ta, storage::PageAccess::kWrite,
        [&](PageId page_id, heap::PageView& page,
            std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            if (Status s = mark(page_id, page, slot); !s.ok()) return s;
            // AO-S3b, and UPDATE's walk states the argument.
            if (parked_on_row) return storage::VisitControl::kStop;
            return storage::VisitControl::kContinue;
        },
        &walk_cursor);
    if (!scan.ok()) return {ErrorReply(scan), false, 0, scan};

    // AO-S3b, and `UpdateInner`'s site states both arms of the argument -
    // the probe-resume arm (AO-S6d) included.
    if (parked_on_row) {
        if (scope.txn == nullptr || scope.txn->trail().size() == statement_trail_mark_) {
            return {ErrorReply(blocked_verdict), false, 0, blocked_verdict};
        }
        DispatchOutcome parked;
        parked.parked_mid_walk = true;
        walk_cursor.rows_done = deleted;
        parked.walk_cursor = walk_cursor;
        return parked;
    }

    return {"DELETED " + std::to_string(deleted), false, deleted};
}

}  // namespace kds::server
