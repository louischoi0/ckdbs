#include "kds/server/command_dispatcher.hpp"
#include "kds/server/read_borrow.hpp"

#include "kds/txn/lock_table.hpp"

#include "kds/base/current_core.hpp"

#include "kds/exec/type_literals.hpp"
#include "kds/storage/anchor_page.hpp"
#include "kds/storage/tagged_cell.hpp"  // varchar(N)'s bounds are the cell width's
#include "kds/server/mount_recovery.hpp"  // SHOW META's recovery block (RC09)
#include "kds/sched/scheduler.hpp"       // SHOW META's group accounting (sched.md 4)

#include "kds/stats/optimizer_signals.hpp"
#include "kds/stats/relayout_planner.hpp"
#include "kds/stats/trail_store.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>
#include <charconv>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <variant>

#include <vector>

#include "kds/catalog/foreign_key.hpp"
#include "kds/catalog/keystone_budget.hpp"
#include "kds/exec/catalog_view.hpp"
#include "kds/exec/chain_frame.hpp"
#include "kds/exec/fk_check.hpp"
#include "kds/exec/assertion_catalog.hpp"
#include "kds/exec/index_ddl.hpp"
#include "kds/exec/index_maintain.hpp"
#include "kds/exec/pagination.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/step_compiler.hpp"
#include "kds/exec/step_vm.hpp"
#include "kds/exec/tuple_verify.hpp"
#include "kds/wal/log_page_init.hpp"
#include "kds/exec/wal_row_log.hpp"
#include "kds/storage/log_page_image.hpp"
#include "kds/storage/index/index_tree.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/parser/parser.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/wal/payload.hpp"

namespace kds::server {

namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// Who a waiter is held by, in words (AO-S6e-b). A read borrow holds under
// an id from a space of its own (`txn::kReadHolderBit`), so rendering it as
// "transaction 9223372036854775809" would send an operator looking for a
// transaction that does not exist and never did.
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

// CheckWhereQualifiers lived here from V06 to V17. It checked a qualified
// WHERE column against the statement's one binding, and refused subquery
// and column-on-the-right predicates - all three because the old
// name-matching evaluator would have answered them *wrongly* rather than
// failing.
//
// It is gone rather than kept: exec::CompilePredicates resolves the same
// clause against the same relation and produces the same three answers,
// and a second resolver is a second opinion about what a name means. The
// one that stayed is the one execution actually uses.

// Splits `line` into the first whitespace-delimited token and "the rest"
// (trimmed), so multi-word commands like "SHOW META"/"FIND TABLE x" can
// be matched one token at a time without pulling in a real tokenizer.
std::pair<std::string_view, std::string_view> SplitFirstToken(std::string_view line) {
    line = Trim(line);
    std::size_t sp = line.find_first_of(" \t");
    if (sp == std::string_view::npos) return {line, std::string_view{}};
    return {line.substr(0, sp), Trim(line.substr(sp + 1))};
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

// Hex-encodes a tuple payload for SHOW PAGE ... VALUES. Hex rather than
// raw text: an arbitrary tuple payload can contain any byte value,
// including a literal '\n' - embedding that directly would desync a
// client's one-line-per-response framing (see the escaping note in
// HandleShowPage). Hex output is plain ASCII by construction, so it is
// always safe to splice into the single response line.
std::string HexEncode(std::span<const std::byte> bytes) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::byte b : bytes) {
        auto v = static_cast<unsigned char>(b);
        out.push_back(kHexDigits[v >> 4]);
        out.push_back(kHexDigits[v & 0x0F]);
    }
    return out;
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
    // shaped exactly like FK_VIOLATION and for its reason. Its spelling
    // landed before its producer did, because a client written against it
    // must not see the message arrive as a bare "ERR ..." in the meantime.
    {StatusCode::kAssertionViolation, "ASSERTION_VIOLATION retryable=0 "},
    // A shipped statement whose reply never came (SS1,
    // `git show c63e49f:docs/spec/crosscore.md` §6) - **no producer since AT-S6** retired the
    // ship; the spelling stays because the wire pins it. Its own spelling
    // because it is the one refusal here that does **not** mean "nothing
    // happened": the statement may have committed. A client must be able to
    // tell it from the bare `ERR` it would otherwise wear, because the
    // correct response is to read the data back - not to retry, which
    // against engine-issued primary keys would insert twice.
    {StatusCode::kUnknownOutcome, "UNKNOWN_OUTCOME retryable=0 "},
    // The refusal pair (2026-08-31; status.hpp carries the test that
    // separates them). Both are spelled, and spelled together, because the
    // distinction only pays at the client: a bare `ERR ...`, which is what
    // both wore until today, answers neither "can I rewrite this?" nor
    // "should I ask a newer server?".
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
    // the line the dispatcher wrote whichever of them it was. Two codes
    // left this arm on 2026-08-31: kUnsupported and kNotImplemented come
    // back as themselves, which is what let a shipped statement's refusal
    // keep saying *which kind of no* it was while statements shipped.
    return Status::InvalidArgument(std::string(reply));
}

// Whether a write is checked against a Bound Cabin - true as of AST07,
// whose enforcer runs in the three write paths. Still a conjunct rather
// than a constant `1` in the replies, because enforcement also needs the
// *registry* to hold the assertion: the entry pages survive a restart, the
// directories do not (recovery replays AST05's records; recovery does not
// exist), and an assertion whose directory is gone is not enforced however
// true this constant is.
constexpr bool kWritePathEnforcesAssertions = true;

sched::Coro CommandDispatcher::AwaitWriteBlock(std::string_view line, Session* session,
                                               DispatchOutcome* out,
                                               sched::MonoTimeNs* statement_deadline_ns) {
    // ---- R6-5: D5's bounded wait on an in-doubt row ---------------------
    //
    // The ratified answer to D5's `[OPEN]`: a writer of a row held by a
    // transaction this core prepared **blocks**, with a bounded ceiling
    // ending in a named refusal - rather than being refused retryably up
    // front, which would surface an engine-internal state to a client that
    // can do nothing with it but spin.
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
    // blocked, freed, and blocked again by a *second* in-doubt transaction
    // still ends within one ceiling. That is what makes HP3's "no hang is
    // reachable" true of a shape that would otherwise be a loop with a
    // bounded body.
    //
    // **The discriminator is at the recording site, not here.**
    // `CheckWriteConflictBlocking` notes a blocker only under `may_park_`
    // and a live clock, which is exactly this path - so a blocker reaching
    // here is one something can act on. The two tests below are the second
    // line of that same rule rather than the thing that enforces it, and
    // they are kept because what they guard is a dereference and a
    // never-reached deadline (`NowNs()` answers 0 with no clock, so a
    // deadline built from it would make the bounded wait unbounded).
    //
    // **The first conjunct is the callers' guard repeated, and it stays**
    // (AO-S6d, a review suggestion declined with its reason). Both callers
    // test it so the frame is only allocated where there is something to
    // wait for; what this one additionally guards is the statement's
    // deadline, which is taken below and would otherwise start running for
    // a caller that had nothing to wait on.
    // **What the waiter is held by, in words** (AO-S6e-c). A pk of 0 is not
    // a row: `kFirstRowId` is 1, so zero is a sentinel no key collides
    // with, and it is what an assertion's admission records - the resource
    // contended there is a *group*, and on the `INSERT` path the id is not
    // issued at admission time. Written once and called three times, the
    // third being the Debug line that printed "row id=0" until the review
    // counted the sites.
    const auto held_name = [](std::uint64_t pk) {
        return pk != 0 ? "row id=" + std::to_string(pk)
                       : std::string("a bound assertion's group");
    };
    if (out->write_block.has_value() && txn_ != nullptr && clock_ != nullptr) {
        // **The bound is a fault net, not a ceiling** (AO-R8). Before AO-S3
        // this was `in_doubt_ceiling_ns_`, 200 ms, and reaching it was an
        // ordinary outcome: the statement was refused after waiting, which
        // is a refusal issued *after* the work was done and is worse than
        // the refusal it replaced (AR2-R10). Now the wait ends when the
        // holder decides, and this bound fires only when something is
        // broken - logged as a fault below, never as a busy answer.
        //
        // **Taken once for the statement, not once per entry into this
        // function** (AO-S6d, item 16). A foreign-key probe's resume can
        // open a wait of its own, and a deadline re-taken there would make
        // "bounded once" a property of an arm rather than of the statement
        // - a shape whose total is the net times the number of rounds.
        if (*statement_deadline_ns == 0) {
            *statement_deadline_ns =
                NowNs() + lock_wait_fault_net_ns_;
        }
        const sched::MonoTimeNs deadline_ns = *statement_deadline_ns;
        // **The waiter's identity for the wait-for graph** (AO-S4a). Only
        // an explicit transaction can be in a cycle: an autocommit
        // statement holds nothing when it parks, so nothing can be waiting
        // for it and its edge would have no incoming half. Zero means "no
        // edge", not "unknown".
        Session& waiting_session = session != nullptr ? *session : autocommit_session_;
        // **AO-S3b: a mid-walk park holds rows even in autocommit**, which
        // is what makes the sentence above no longer true of every waiter.
        // Before this stage an autocommit statement's scope was unwound by
        // `EndWrite` before this loop ran, so the waiter held nothing, no
        // cycle could contain it and 0 was the honest identity. A statement
        // parked *inside* its walk keeps its scope open with rows written
        // into it: it can be the other half of a cycle, and a waiter the
        // graph cannot name is a link no registration can close - a
        // deadlock invisible to AO-S4a's detector and ended only by the
        // fault net, which kills both waiters and is what AR2-R10 forbids.
        //
        // **Re-read every turn, because the identity can appear mid-loop.**
        // An autocommit statement that met the held row before it had
        // written anything is refused, re-run whole, and only *then* writes
        // rows and parks mid-walk - so a value taken once, before the first
        // wait, is 0 for the rest of a loop whose waiter has since become a
        // holder. That is the invisible link this stage exists to remove,
        // reintroduced by the caching alone.
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
                break;
            }
            // Settles the moment the transaction is decided, whichever way:
            // a committed one releases the row to a re-read, an aborted one
            // puts the old version back, and both are "no longer in doubt".
            // One clock read and one walk of this core's live set per
            // turn, which is the predicate shape every park in this file
            // uses.
            const std::function<bool()> decided = [this, block, deadline_ns] {
                return !txn_->IsInFlight(block.trx_id) || NowNs() >= deadline_ns;
            };
            co_await sched::WaitUntil{&decided};
            if (txn_->IsInFlight(block.trx_id)) break;  // the net, not the decision
            if (logging(LogLevel::kDebug)) {
                log_->Debug("lock", "core " + std::to_string(core_id_) + " held a write of " +
                                        held_name(block.pk) + " until transaction " +
                                        std::to_string(block.trx_id) +
                                        " decided, and is running it again");
            }
            // **The re-run is deliberately not re-stamped**, so it
            // executes under `kWhenDurable` (`CommitAck`). Unreachable
            // rather than merely unlikely: `blocking_writer_` is set only
            // by `CheckWriteConflictBlocking` on a write path, and the one
            // caller that stamps `kAtAppend` dispatches the literal
            // `COMMIT`, which writes no row and takes no conflict. And the
            // direction is the safe one - a re-run would wait for
            // durability that a decide no longer waits for, never the
            // reverse - which is why this is a comment and not a fix.
            const MayParkScope parking(*this, /*allowed=*/true);
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
        // transaction it names is still in flight, since a waiter is only
        // ever recorded against a holder `IsInFlight` admits. An RAII guard
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
            const std::string held = held_name(block.pk);
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
            // **The fault net fired, and that is not an outcome - it is a
            // defect report** (AO-R8, AR2-R10 as AR2-A amends it). A wait
            // that ends by clock reintroduces a refusal *after* the work
            // was done, which is worse than the refusal it replaced, so
            // reaching here means something is broken rather than busy:
            // before AO-S4a, a cycle nobody detected; after it, a detector
            // that missed one, or a holder that is genuinely stuck.
            //
            // The refusal is still `TxnConflict`, because `IsRetryable`
            // admits exactly that code and a client's retry loop reads the
            // wire bit it sets - and deliberately **not** `UnknownOutcome`,
            // which would tell a client to read its data back when this
            // statement plainly did nothing. What changed at AO-S3 is the
            // message: it names the net, so an operator meeting it looks
            // for the fault instead of concluding the row was busy.
            const DispatchOutcome::WriteBlock block = *out->write_block;
            const std::string held = held_name(block.pk);
            // **And what reaching it means is not the same sentence for a
            // group** (the AO-S6e-c review's B3). For a *row* the net is a
            // defect report, and AO-R8's wording says so. A bound
            // assertion's group is far coarser - every writer touching one
            // account serialises on it - so a holder that keeps one for
            // longer than the net is ordinary contention, and a re-run that
            // meets a *new* reserver each turn spends the statement's one
            // deadline on honest churn with nobody stuck at all. Telling
            // that operator to look for a stuck holder would be the
            // convenient sentence rather than the true one.
            //
            // **What this does not decide**: whether a group wait should
            // carry a shorter bound of its own. That interacts with AO-R8's
            // one-net-per-statement rule and is AO-S7's, beside the prices;
            // AO-0 item 22 records it.
            const std::string why =
                block.pk != 0
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
    // **One loop over both waits, not two arms** (the AO-S6e-a review's
    // C3). Each wait's re-run is a whole fresh statement and can meet what
    // the other waits for: a relation-lock re-run can meet a held row, and
    // a write-block re-run can meet a relation a DDL took while it waited.
    // Two sequential arms answer the second of those with a refusal the
    // first would have waited out - which is item 16's defect, on a new
    // pair. Both waits are deadline-bounded and both clear their own field
    // on every exit, so the loop ends when neither is set. One function, so
    // the first dispatch and a foreign-key probe's resume cannot diverge.
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
    // **What is waited on is the slot, not the holder's decide.** Every
    // other wait in this file polls `IsInFlight`, which is this core's live
    // set; the holder here is a read borrow that may be running on any
    // core, and on a peer that predicate is false from the first poll - a
    // re-run per reactor iteration for the length of the reader's
    // statement. The slot is flipped by the release itself, from whichever
    // core releases, and the kick that follows it is AU-S2's.
    // `locks_` and the slot are non-null by construction: this field is
    // set only where a `TryAcquire` asked for a wake and got one, which
    // needs both. Not re-tested: a guard on a state that cannot occur reads
    // as evidence that it can.
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
    // ends eleven seconds later.
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
        locks_->DropWake(wait.key, slot);
        locks_->ClearWaitFor(waiter_id);
        RefuseParkedWrite(*out, waiting_session,
                          DeadlockVictim(std::string(txn::LockUnitName(wait.key.unit)) + " oid " +
                                         std::to_string(wait.key.rel_oid) + ", held by " +
                                         HolderName(wait.holder)),
                          wait.poisons);
        co_return Status::OK();
    }

    const std::function<bool()> freed = [this, slot, deadline_ns] {
        return txn::LockWaitReady(slot) || NowNs() >= deadline_ns;
    };
    co_await sched::WaitUntil{&freed};
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
        // **The poison `EndWrite` withheld for the sake of this wait**, and
        // this is the one exit that has to restore it. The victim arm gets
        // it from `RefuseParkedWrite`; a re-run's own failure gets it from
        // `EndWrite`, `lock_wait_` being empty by then. Here the wait ended
        // in the refusal the ask produced and nothing else runs, so without
        // this the transaction whose statement failed stays usable and
        // commits - which is §6's failure atomicity broken by a wait that
        // was supposed to be invisible when it worked.
        // Not for a statement that poisons nothing when it fails - a
        // `CREATE ASSERTION`'s build (the AT-S5e review's C6).
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
    const MayParkScope parking(*this, /*allowed=*/true);
    *out = DispatchAndStage(line, session);
    co_return Status::OK();
}

sched::Coro CommandDispatcher::DispatchAsync(std::string_view line, Session* session,
                                             DispatchOutcome* out, CommitAck commit_ack) {
    // Every statement runs on the core its session is on (AT-S9). What
    // suspends here is a statement's own wait - a lock, a blocking writer,
    // the group commit - never a stage on another core.
    // **The statement may park from here**, which is the whole difference
    // between this entry point and `Dispatch()` - and the condition every
    // wait below is admitted under. Set and cleared around
    // the synchronous half, which takes no suspension point, so it never
    // spans a park and never describes another statement.
    {
        // **Both guards RAII since AO-S6d, and the argument this comment
        // used to make about only one of them is why.** A leaked
        // `may_park_` grants a parking allowance; a leaked `commit_ack_`
        // silently drops a client's durability wait. The window below
        // provably takes no suspension point - `DispatchAndStage` is not a
        // coroutine, and nothing in this file pumps a scheduler or a ring -
        // so a hand-placed pair would be correct today and silently wrong
        // the day an early `co_return` or a `co_await` appears between
        // them. AO-S6d added a third `may_park_` window, inside a loop and
        // beside a `co_await`, which is that day arriving; the guard makes
        // the property structural rather than reviewed.
        const MayParkScope parking(*this, /*allowed=*/true);
        const CommitAckScope stamped(*this, commit_ack);
        *out = DispatchAndStage(line, session);
    }

    // ---- R6-5: D5's bounded wait on an in-doubt row ---------------------
    //
    // `AwaitWriteBlock` is the whole of it, reached through
    // `AwaitStatementWaits` over what the statement's own dispatch
    // produced (the foreign-key probe arm was its second caller until
    // AT-S5f). The frame is allocated only where there is something to
    // wait for.
    //
    // **The deadline is the statement's** and is threaded through both
    // callers for the reason the function states.
    sched::MonoTimeNs statement_deadline_ns = 0;
    co_await AwaitStatementWaits(line, session, out, &statement_deadline_ns);

    // **The foreign-key probe park is gone with the probes** (AT-S5f).
    // What stood here was the one park that resumed by *re-entering* the
    // statement: a forward or reverse round was sent at the dispatch fork,
    // this block waited for every owner's reply under one deadline, fed the
    // verdicts back through `resumed_fk_verdicts_` and dispatched the line
    // again - plus the rounds loop AK-S3 added, the autocommit decide that
    // released the intents a round had been granted, and the
    // pending-delete clear the refusal arm owed.
    //
    // Both checks answer here now, so a foreign-key statement parks the way
    // every other statement does: on a row it cannot have yet, through
    // `AwaitStatementWaits` above, and it is re-run rather than resumed.

    // **The shipped-statement park and the cross-owner commit protocol
    // went with the ship** (AT-S6). What stood here waited for an owner's
    // answer to a statement this core had sent, and then ran D4's two
    // phases over the participants that ship had enrolled - the prepare,
    // the decision made durable before anyone was told, the decide, and
    // R6-5's resolution ask for a participant left in doubt.
    //
    // Nothing ships, so nothing enrols, so a transaction has no second
    // half anywhere to prepare or to decide: it is this core's, whole, and
    // its `COMMIT` is the local one. The remote **step** park that stood below
    // went at AT-S9 with the fan-in and the two-step pipeline, the last two
    // routes that opened a stage.

    if (out->pending_lsn != wal::kNoLsn) {
        // **The group commit.** Parking here rather than syncing inside the
        // statement is the whole change: every other runnable connection
        // gets to stage its own commit before the reactor's post-task hook
        // syncs once for all of them (Scheduler::SetPostTaskHook). Nothing
        // is held across this - the statement finished above, its page
        // spans with it.
        const wal::Lsn lsn = out->pending_lsn;
        const std::function<bool()> durable = [this, lsn] { return wal_->IsDurable(lsn); };
        co_await sched::WaitUntil{&durable};
        out->pending_lsn = wal::kNoLsn;
    }
    co_return Status::OK();
}

DispatchOutcome CommandDispatcher::DispatchAndStage(std::string_view line, Session* session) {
    // The statement boundary is where the catalog asks whether the schema
    // moved (AT-S2; `Catalog::Revalidate`): before this statement resolves
    // anything and after the previous one released everything it held.
    catalog_.Revalidate();
    // **This statement runs as this dispatcher's core** (AM-S2 step 3,
    // `base/current_core.hpp`). On a reactor thread it is what
    // `Scheduler::RunOnce` already declared and this costs a redundant
    // store; off one - a fixture driving several cores' dispatchers from the
    // test thread - it is the only thing that makes the identity true, and
    // the identity is written to *disk*: `StampPageLsn` records whose
    // stream the page_lsn beside it belongs to, and a peer's page stamped
    // core 0's was a lie the next mount refused (rule 5) until AW-S1b made
    // the stamp a diagnostic (`page_header.hpp`). Measured before it was
    // here: 6,250 stamps in the suite carried
    // the wrong core, all of them from fixtures, none from `ExpeditorTest`'s
    // real instance.
    //
    // One statement is one core's work by construction - the dispatcher
    // belongs to a core and a statement never migrates - so this is the
    // seam, rather than a guard at each of the sites that stamp.
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
    blocked_pk_ = 0;
    lock_wait_.reset();
    last_refusal_ = Status::OK();
    last_refusal_detail_ = wire::kNoDetail;
    statement_trail_mark_ = 0;

    // ---- H6 step 2: the trace, when this session asked for one ----------
    //
    // **Manual sampling** (`TRACE ON`), which is `observability.md` §9's
    // decision taken at H6: nothing is collected unless a session said so,
    // which makes zero-cost-when-off trivially true rather than argued.
    // `trace_` is null on every other statement and `SpanScope` over a null
    // context reads no clock at all - the property §6 sets the budget by.
    //
    // The context lives on the dispatcher for `DispatchInner`'s whole
    // subtree rather than being threaded through it, which is the same
    // trade `pending_commit_lsn_` above states and for the same reason: one
    // statement runs at a time on a core (`sched.md` §3), so there is no
    // second value to confuse it with, and threading it would put a
    // parameter on a dozen signatures. §5's *rejection* of a thread-local
    // stack is untouched by that - the danger there is a cooperative task
    // yielding mid-span and leaving the stack describing another task, and
    // a per-dispatcher context is per-core-per-statement, not per-thread.
    std::optional<stats::TraceContext> trace;
    if (tracing_ && traces_ != nullptr) {
        trace.emplace(traces_->NextId(), clock_, std::string(line));
        trace_ = &*trace;
    }
    DispatchOutcome outcome;
    {
        stats::SpanScope request(trace_, stats::Layer::kRequest);
        outcome = DispatchInner(line, active);
    }
    if (trace.has_value()) {
        trace_ = nullptr;
        traces_->Add(std::move(*trace));
    }
    // Read back out of the member the write paths set: threading it through
    // InsertInner/UpdateInner/EndWrite and every handler between would be a
    // parameter on a dozen signatures for one number, and one statement runs
    // at a time on a core (sched.md section 3), so there is no second value
    // to confuse it with.
    outcome.pending_lsn = pending_commit_lsn_;
    pending_commit_lsn_ = wal::kNoLsn;
    // R6-5's, on the same terms. `EndWrite` has already dropped the blocker
    // where the statement is not re-runnable, so a value here means "this
    // statement wrote nothing and a wait could get it past". It was also
    // never set beside a ship until AT-S6 retired the ship.
    //
    // **Never beside a relation wait** (the AT-S5e review's C4): a
    // statement can record a busy parent's writer before it asks its
    // relation `IX`, and a write-block re-run would overwrite the outcome
    // and drop the relation's wake without a `DropWake`. The relation
    // re-run re-resolves the parent anyway.
    if (blocking_writer_ != 0 && !lock_wait_.has_value()) {
        outcome.write_block = DispatchOutcome::WriteBlock{blocking_writer_, blocked_pk_};
    }
    // **The refusal, installed where the path that raised it could not
    // carry one.** `InsertOneRow` answers a rendered string and leaves
    // `status` OK, so without this the cap's category is recovered by
    // parsing the line - and `kErrorSpellings` cannot recover
    // `ResourceExhausted`, so it becomes `InvalidArgument` with the cap's
    // detail attached to it. Only installed over an OK status: a path that
    // carried its own has already said something more specific.
    if (!last_refusal_.ok() && outcome.status.ok()) outcome.status = last_refusal_;
    outcome.resource_detail = std::exchange(last_refusal_detail_, wire::kNoDetail);
    last_refusal_ = Status::OK();
    // AO-S6e-b's, on the same terms, and exclusive with the blocker above:
    // `NoteBlockingWriter` records none once a relation wait is set, and
    // the blocker is not installed when one recorded earlier meets it.
    if (lock_wait_.has_value()) outcome.lock_wait = std::move(lock_wait_);
    lock_wait_.reset();
    blocking_writer_ = 0;
    blocked_pk_ = 0;

    if (log_ == nullptr) return outcome;

    // A failed command reports at Warn and a successful one at Debug, so
    // the level has to be decided *before* the enabled() test - gating the
    // whole block on Debug would silently drop every error at any threshold
    // above it, which is exactly the threshold an operator runs at.
    const bool failed = outcome.response.rfind("ERR ", 0) == 0;
    const LogLevel level = failed ? LogLevel::kWarn : LogLevel::kDebug;
    if (!log_->enabled(level)) return outcome;

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
    return outcome;
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

DispatchOutcome CommandDispatcher::Dispatch(std::string_view line, Session* session) {
    DispatchOutcome outcome = DispatchAndStage(line, session);
    if (outcome.pending_lsn == wal::kNoLsn) return outcome;

    // Inline, on this thread: the batch is whatever happened to be staged
    // already, which with no scheduler is this commit alone.
    if (Status s = wal_->DrainOnce(); !s.ok()) {
        return {ErrorReply(s), outcome.should_stop};
    }
    if (Status s = wal_->EnsureDurable(outcome.pending_lsn); !s.ok()) {
        return {ErrorReply(s), outcome.should_stop};
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
        IEquals(cmd, "DESC") || IEquals(cmd, "TRACE") || IEquals(cmd, "BEGIN") ||
        IEquals(cmd, "START") ||
        IEquals(cmd, "COMMIT") || IEquals(cmd, "ROLLBACK") || IEquals(cmd, "ABORT")) {
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

    // ---- The failed-txn gate (docs/spec/txn.md section 10-8) -----------------
    //
    // A statement inside an explicit transaction failed, so the transaction
    // can no longer be committed. Everything but the ways out is refused -
    // a whitelist, so a statement added later is refused by default rather
    // than admitted by omission.
    if (session.failed() && !Session::AdmittedWhileFailed(cmd)) {
        return {"ERR current transaction is aborted; commands are ignored until ROLLBACK",
                false};
    }

    // ---- Transaction control --------------------------------------------
    if (IEquals(cmd, "BEGIN") || IEquals(cmd, "START")) {
        // `START TRANSACTION` is the SQL spelling; both reach one handler,
        // which strips the noise word.
        return HandleBegin(rest, session);
    }
    if (IEquals(cmd, "COMMIT")) return HandleCommit(session);
    if (IEquals(cmd, "ROLLBACK") || IEquals(cmd, "ABORT")) return HandleRollback(session);
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
        if (IEquals(sub, "META")) return HandleShowMeta();
        if (IEquals(sub, "TABLES")) return HandleListTables(session);
        if (IEquals(sub, "NAMESPACES")) return HandleShowNamespaces(session);
        if (IEquals(sub, "PAGE")) return HandleShowPage(sub_rest);
        if (IEquals(sub, "PATTERNS")) return HandleShowPatterns();
        if (IEquals(sub, "ACCESS")) return HandleShowAccess();
        if (IEquals(sub, "BUDGET")) return HandleShowBudget();
        if (IEquals(sub, "CABINS")) return HandleShowCabins();
        if (IEquals(sub, "INDEXES")) return HandleShowIndexes(session);
        if (IEquals(sub, "FKEYS")) return HandleShowFkeys();
        if (IEquals(sub, "ASSERTIONS")) return HandleShowAssertions();
        if (IEquals(sub, "RELAYOUT")) return HandleShowRelayout(sub_rest);
        if (IEquals(sub, "CABIN_OPTIMIZER")) return HandleShowCabinOptimizer();
        // H6 step 3: `TRACES` is the ring, `TRACE <id>` one span tree.
        if (IEquals(sub, "TRACES")) return HandleShowTraces();
        if (IEquals(sub, "TRACE")) return HandleShowTrace(sub_rest);
        return {"ERR unknown SHOW target", false};
    }
    // H6 step 3: manual sampling, which is `observability.md` §9's sampling
    // decision taken at H6 - nothing is collected unless a session asks, so
    // zero-cost-when-off is a property rather than an argument.
    if (IEquals(cmd, "TRACE")) return HandleTrace(rest);
    if (IEquals(cmd, "DESCRIBE") || IEquals(cmd, "DESC")) {
        return HandleDescribe(rest, session);
    }
    // **A DDL runs where the session is** (AT-S5, AT-R5). What stood here
    // until then was the refusal and its argument - *"a peer takes no DDL
    // (PW4); every target of these three verbs writes state only the
    // system core may write, so the whole verb is refused before any
    // handler, and it is what makes §5d's purge-gate soundness argument
    // enforced rather than assumed"*. Both halves are retired: the state
    // is every core's, and §5d's gate rests on AN-S2's instance-wide
    // predicate, the sweep's single core being a placement so that two do
    // not walk the same chains (`ddl-transactional.md` §5d, `catalog.md`
    // CT6). Written out rather than deleted because the sentence outlived
    // the guard by one stage. Until this stage
    // a peer refused `CREATE`/`ALTER`/`DROP` (`PeerDdlRefused`), or shipped
    // the statement to core 0 where a route could be made (CR5/CB4),
    // because the catalog pages had one writer. They have none: every core
    // writes them under the page latch, the DDL's own relation `X` (AO)
    // keeps a statement from being overtaken, and the schema word (AT-S2)
    // carries the change to every other core's memo. The ship's dedup
    // record and `pending_shipped->ddl` went with it.
    // The PW1c interim DML guard stood here from 2026-08-24 until PW1c-5
    // removed it the same day. What replaced it, so its removal is not a
    // hole: `CheckWriteAffinity`'s shape gate refused the then-unsound
    // shapes by name (cabined, assertion-covered
    // - each citing the task that lifts it; btree lifted at PW2-4, indexed at
    // PW1c-6b-4, the key mode gone entirely 2026-08-25 - a
    // caller-named pk is now refused per row in InsertOneRow - and
    // **FK-linked lifted 2026-09-01**, work order AI, once the forward
    // check probed the parent's owner instead of reading it), and the
    // multi-row VALUES path refused on a peer before touching the catalog
    // page. None of it is left: AT-S9 reduced the gate to
    // `CheckWriteAdmission`'s one assertion question. **The store's `MayWrite` is
    // no part of this any more**: it stood here as the backstop that
    // refused an unfunded write retryably rather than letting it surface
    // as a rule-5 stamp mismatch at the next mount, and it returns
    // unconditional `true` since AT-S5 - there is no funding to check,
    // and what keeps two cores off one page's bytes is the page latch.
    if (IEquals(cmd, "CREATE")) {
        auto [sub, sub_rest] = SplitFirstToken(rest);
        if (IEquals(sub, "CABIN")) {
            return HandleCabin(Trim(line));
        }
        if (IEquals(sub, "ASSERTION")) {
            return HandleAssertion(Trim(line), session);
        }
        // `UNIQUE` routes here too, so its refusal comes from the parser
        // with the byte offset of the word itself rather than from this
        // layer as "unknown CREATE target".
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
            return HandleCabin(Trim(line));
        }
        if (IEquals(sub, "INDEX")) {
            return HandleIndex(Trim(line), session);
        }
        if (IEquals(sub, "ASSERTION")) {
            return HandleAssertion(Trim(line), session);
        }
        if (IEquals(sub, "NAMESPACE")) {
            return HandleNamespace(Trim(line), session);
        }
        if (IEquals(sub, "TABLE")) {
            return HandleDropTable(Trim(line), session);
        }
        return {"ERR only DROP TABLE, DROP NAMESPACE, DROP CABIN, DROP INDEX and DROP "
                "ASSERTION are supported",
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
    if (IEquals(cmd, "SYNC")) {
        return HandleSync();
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

DispatchOutcome CommandDispatcher::HandleShowMeta() {
    std::ostringstream os;
    os << "version=" << superblock_.version() << " create_time=" << superblock_.create_time()
       << " last_mount_time=" << superblock_.last_mount_time()
       // **What the volume's log is** (AR0 M0). Always `single` since
       // AM-S4(d) - `SuperBlock::Decode` refuses every other topology - and
       // kept rather than dropped because it is a durable *format* fact a
       // client reads off the volume, not a setting that varies. A literal,
       // because there is no longer a field to read it from.
       //
       // `wal_anchor_count` stood beside it under `per-core` only, since
       // under one stream it can only ever say 1. That is now every volume,
       // so it is printed nowhere.
       << " wal_topology=single"
       << " cabin_optimizer=" << (cabin_optimizer_enabled() ? "on" : "off")
       // The core serving this session (PW6, docs/inflight/in-progress/workplan-peer-writer.md).
       // Every core listens (AT-S8), so the kernel picks the accepting core and
       // a client cannot choose it (PW5), so a client that needs to know -
       // the per-core writer benchmark, an operator reading a refusal - must
       // be able to ask. Constant per session, by M3: a session never moves.
       << " core=" << core_id_
       // §5d: delete-marked catalog rows the horizon-gated purge retired
       // *this* mount. Its sibling `catalog_marks_finalized` below counts a
       // previous mount's leftovers, and is part of the recovery report;
       // this one is live dispatcher state and prints unconditionally.
       << " catalog_marks_purged=" << catalog_marks_purged_;

    // FM10 (docs/inflight/in-progress/workplan-multi-free-map.md): what the allocation map
    // costs, printed unconditionally because a one-region database's `1`
    // is the answer that says the multi-page map is costing nothing here.
    // `map_pages_resident` below twice `map_regions` is FM6's saving made
    // visible: a database with no Waystone directory builds no headerless
    // bitmap at all, and `headerless_pages=0` is the fact that lets
    // IsHeaderless answer with no lookup on the fault, write-back and
    // WAL-gate paths.
    const auto map = page_store_.map_residency();
    os << " map_regions=" << map.regions << " map_pages_resident=" << map.resident_pages
       << " map_coverage_ids=" << map.coverage_ids
       << " headerless_pages=" << (map.has_headerless ? 1 : 0);

    // The undo purge's two numbers (docs/inflight/in-progress/workplan-undo-purge.md UP3):
    // live pages plateauing under a write-heavy loop is the feature, and
    // the recycle count is what proves the plateau came from reuse rather
    // than idleness. Absent without a manager, like the transaction rows.
    if (txn_ != nullptr) {
        os << " undo_pages_live=" << txn_->undo().LivePages()
           << " undo_pages_recycled=" << txn_->undo().PagesRecycled();
    }

    // **The device syncs this core performed** (XD0,
    // `instructions/v2.7.1/measurement-xd.md`). Read-only surfacing of two
    // counters that already increment - `WalStats::syncs` at
    // `src/wal/manager.cpp:110` and `WalWriter::syncs_` at
    // `src/wal/writer.cpp:98` - so no atomic and no path is added, and
    // `cores = 1` behaves exactly as before.
    //
    // **`wal_syncs` is the total, and it is not the writer's number.** The
    // order that asked for this said "from `Writer::syncs()`"; the source
    // says otherwise, and the reading would have been ~0 on every cell it
    // was wanted for. `WalManager::Sync()` performs every sync a caller is
    // parked on - a commit's, a prepare's `RequestDurable`, a client
    // `SYNC`, the checkpoint gate - on the reactor itself, and only D3's
    // loss-window tick is handed to the writer thread (`manager.cpp:249`).
    // A peer core starts no writer thread of its own (`expeditor.cpp` does,
    // for core 0 alone), so its `writer_syncs()` is structurally zero.
    //
    // **Under one stream a peer's `wal_syncs` and `wal_interval_syncs` are
    // structurally 0** (AR0 M0). It performs no device sync: it flushes
    // through core 0's stream and waits on core 0's writer, so every
    // `fdatasync` this instance pays is counted on core 0. Its interval
    // count is 0 because the loss window belongs to the log, which has one
    // drain bounding it (`manager.cpp`'s `DrainOnce`). So the reading rule
    // below holds on a peer trivially: nothing was performed and nothing
    // was waited on *here*, and a peer's durability cost is read as core
    // 0's writer count rising.
    //
    // **`wal_sync_failures` is the exception and is not structurally 0.** A
    // peer's flush of the shared ring can fail, and so can its wait on the
    // writer; both are counted here. A failed wait is counted on core 0
    // too, so that one field is the one place a peer and core 0 both
    // report the same event.
    //
    // Hence three fields, and the middle one is the **interval** tick
    // rather than the writer's count, so one reading rule holds on every
    // core: `wal_syncs - wal_interval_syncs` is the part somebody was
    // parked on. `interval_syncs` increments on both of D3's paths -
    // handed to the writer (`manager.cpp:250`) and taken inline where
    // there is no writer (`:256`) - where the writer's own count is
    // structurally 0 on every peer and would read there as *"every sync
    // was waited on"* when under `relaxed` none was.
    //
    // The one drift, stated because a row will divide by these: on core 0
    // `interval_syncs` counts *requests* and the writer counts
    // *completions*, so the two differ by the sync in flight and by any
    // pair the writer coalesced (it reads `requested_` once per wake,
    // `writer.cpp:66-73`). Bounded and small; a peer has neither term.
    //
    // Absent where the dispatcher has no WAL - the absent-rather-than-
    // zeroed rule - which is an in-process dispatcher, never a served core.
    if (wal_ != nullptr) {
        const wal::WalStats& wal_stats = wal_->stats();
        os << " wal_syncs=" << (wal_stats.syncs + wal_->writer_syncs())
           << " wal_interval_syncs=" << wal_stats.interval_syncs
           << " wal_sync_failures="
           << (wal_stats.sync_failures + wal_->writer_sync_failures());

        // **How many commits this core's fsyncs are amortised over**
        // (AF-T5's §3b finding). D2's whole mechanism is that one sync
        // resolves a batch, and `kds.conf.sample` states the edge case that
        // makes it matter: *"a batch of one is a batch"*. A core serving one
        // committing session has no partner to batch with and pays a sync
        // per commit, and AF-T5 measured that as **2.35x on a load phase**
        // against a placement that gave each core two committers.
        //
        // **This is the number, and a session count is not.** The obvious
        // reading of "how many sessions is this core committing for" is a
        // count of attached sessions - and under the single listener the
        // fact was found on, that was every session on core 0 and zero on
        // the peers, while the peers paid the un-batched syncs because a
        // shipped write committed on its relation's owner (until AT-S5).
        // The batch is the fact; the session count is a proxy that
        // inverted on that topology.
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
    }

    // **The `trxid_refill_*` and `rowid_refill_*` blocks went at AT-S10b**
    // with the two id leases they timed: a peer carves and issues its own
    // ids, so there is no refill to wait on.

    // **XF4's per-leg renderer stood here and went with its callers**
    // (AT-S6). It printed what each leg of a cross-owner commit cost -
    // the coordinator's four and the participant's three - and the four
    // call sites went with the protocol two hunks above. Left as a
    // `set but not used` warning by that deletion until this line.
    // **Three counter blocks retired at AT-S9**, each with the event it
    // counted: `cross_core_write_refusal` (a write to a relation another
    // core owned - there are no owners), `range_split_decline` (a range
    // opening a gate refused - nothing opens a range) and
    // `cabin_split_discard` (a Cabin's sets discarded at a split - nothing
    // splits). A client reading any of them reads its absence, which is
    // this section's absent-rather-than-zeroed rule applied to a counter
    // whose event cannot happen.
    // `cabin_scope_fallthroughs` went at AT-S10 on the same rule: a Cabin
    // fell through only for a walk narrower than its relation, which was a
    // remote stage's slice, and nothing opens a stage.

    // **How many ranges each split relation has** (R4-R §7's instrument
    // gap, added with RR1): `sys.ranges` has no column definitions, so
    // nothing else reports it from outside the process. Absent when nothing
    // is split - `SHOW META`'s absent-rather-than-zeroed rule, and since
    // AT-S9 retired range opening, the reading of every volume created
    // since: only a pre-AT split relation can print here.
    //
    // Keyed `oid:ranges`. It was `oid:ranges@stages`, the second number the
    // count of same-owner runs a fan-in read opened one upstream per; the
    // fan-in and the owners are gone, and every range is walked here.
    {
        std::map<catalog::Oid, std::size_t> split;
        if (auto tables = catalog_.ListTables(); tables.ok()) {
            for (const catalog::SysObjectRow& row : tables.value()) {
                auto ranges = catalog_.RangesOf(row.oid);
                if (!ranges.ok() || ranges.value().size() < 2) continue;
                split.emplace(row.oid, ranges.value().size());
            }
        }
        if (!split.empty()) {
            os << " split_relations=" << split.size() << " split_relation_detail=";
            bool first = true;
            for (const auto& [oid, count] : split) {
                if (!first) os << ',';
                os << oid << ':' << count;
                first = false;
            }
        }
    }

    // **CR7's block went with the batch** (AT-S7): `access_batches_sent`,
    // `access_entries_sent`, `access_batches_dropped`, the two applied
    // counts and `access_shape_overflows` each measured one end of a wire
    // a peer's access statistics took to core 0, and they take no wire -
    // every core writes `sys.access_stats` itself. What a statistic costs
    // is `steps_recorded`/`write_failures` on the core that recorded it,
    // which is where it always was.

    // **The shipping and 2PC blocks went with the protocols** (AT-S6):
    // `shipped_*` on both sides, the enrolment counters, the in-doubt
    // population and the coordinator's decision-record counters. Every one
    // of them measured a statement crossing to another core or a
    // transaction half being decided from one, and neither happens: a read
    // runs where the session is, as a write has since AT-S5.

    // Group accounting against wall time (`docs/spec/sched.md` §4's last bullet;
    // `sched/scheduler.hpp`'s accessors carry the argument for why there are
    // two counters per group). `sched_wall_us - sum(sched_*_polled_us)` is
    // the reactor time charged to no group: the idle block, the WAL drain's
    // `fdatasync`, timer callbacks, the io drain. Absent rather than zeroed
    // where no reactor is attached, the rule the recovery block follows.
    if (scheduler_view_ != nullptr) {
        os << " sched_wall_us=" << scheduler_view_->run_wall_ns() / 1000
           << " sched_iterations=" << scheduler_view_->iterations()
           // The wake path and the park rule, from this reactor's side
           // (§7). `sched_idle_blocks` is how often it slept with the flag
           // raised, and `sched_parked_idle_blocks` the blocks taken with
           // tasks still queued, every one of which was a spin before
           // "parked is not ready". `sched_wake_race_skips` went with the
           // ring at AT-S10d: it counted the pre-block re-check finding a
           // queued message, and a kick has no queue to re-check.
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
           // `sched_spurious_wakes` went with the ring at AT-S10d: it
           // counted wakes that found an empty inbox, and with no inbox
           // every wake would read as one.
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
               << "_consumed_us=" << scheduler_view_->consumed_ns(group) / 1000;
        }
    }

    // The last recovery, for the operator who has to answer "what did the
    // restart do" (RC09, `docs/spec/wal.md` §13). Absent rather than zeroed when no
    // report is installed - a dispatcher built without one (every socket-free
    // test) has not "recovered nothing", it has no answer, and printing zeroes
    // would be an answer.
    if (recovery_ != nullptr) {
        // **Where the pass ran** (AR0 M0). Printed only where it was not
        // here, the rule the prepared three below already keep: on core 0
        // its absence means "these numbers are mine".
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
        // R6-4's three, printed only when this mount actually resolved a
        // cross-owner transaction - one a log written before AT-S6 retired
        // 2PC left prepared. **Absent rather than zeroed**: nothing prepares
        // since, and three structural zeroes would say nothing while
        // looking like a measurement.
        if (recovery_->prepared != 0) {
            os << " recovery_prepared=" << recovery_->prepared
               << " recovery_prepared_committed=" << recovery_->prepared_committed
               << " recovery_prepared_aborted=" << recovery_->prepared_aborted;
        }
        if (recovery_->timings.timed) {
            os << " recovery_analysis_us=" << recovery_->timings.analysis_ns / 1000
               << " recovery_redo_us=" << recovery_->timings.redo_ns / 1000
               << " recovery_high_water_us=" << recovery_->timings.high_water_ns / 1000
               << " recovery_undo_us=" << recovery_->timings.undo_ns / 1000
               << " recovery_checkpoint_us=" << recovery_->checkpoint_ns / 1000;
        }
        // RV3 closed 2026-08-19: catalog mutations log the ordinary record
        // types, DDL runs under a real transaction, and redo/undo restore
        // catalog pages like any page - so `catalog_recovered` flips to 1.
        // `relations_missing_pages` stays as the audit: it counted the gap
        // while it existed, and a zero from here on is the proof it closed.
        os << " recovery_relations_checked=" << recovery_->relations_checked
           << " recovery_relations_missing_pages=" << recovery_->relations_missing_pages
           << " catalog_recovered=1"
           // DT10: delete-marked catalog rows a previous mount left
           // behind, retired before the listener bound. Since D2 every
           // DROP is transactional, so a non-zero here means the previous
           // mount ended - cleanly or not - with marks some reader or a
           // crash kept the §5d purge from retiring.
           << " catalog_marks_finalized=" << recovery_->catalog_marks_finalized
           // DT7 made these transactional; RV3 (2026-08-19) made them
           // durable - a committed DDL survives a crash by redo, an
           // uncommitted one is rolled back by the undo records the
           // catalog's hook appends. The pair finally reads as one true
           // statement.
           //
           // `drop-index` rejoined the list on 2026-08-18 (DT9). It was in
           // it, came out when the statement was refused inside a
           // transaction, and is back because the refusal was withdrawn -
           // which is the whole reason this is a list of statement names
           // and not a bare `=1`.
           << " ddl_transactional=create-table,drop-table,create-index,drop-index"
           << " ddl_durable=1";
        // RC07: what the mount could resume enforcing, and the honest
        // remainder. A surviving declaration whose directory could not be
        // rebuilt is counted here and left *out* of the registry, so
        // SHOW ASSERTIONS reports `enforcing=0` for it rather than a
        // constraint that would admit every write. Core 0's mount resumes
        // the instance's registry (AT-S5d), so on a peer both are 0 for
        // `recovery_by=core0`'s reason. `recovery_assertions_foreign` stood
        // beside them until then, counting the declarations a core left to
        // the relation's owner; nothing is another core's now.
        os << " recovery_assertions_enforcing=" << recovery_->assertions_enforcing
           << " recovery_assertions_unrecovered=" << recovery_->assertions_unrecovered;
    }
    return {os.str(), false};
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

DispatchOutcome CommandDispatcher::HandleShowPatterns() {
    auto rows = catalog_.ListPatterns();
    if (!rows.ok()) {
        return {ErrorReply(rows.status()), false, 0, rows.status()};
    }

    // A `name=` column stood here until 2026-08-31, joined in from
    // `sys.pattern_defs` by pattern_id so a *declared* pattern printed its
    // name instead of a bare hash. The operator withdrew declared patterns
    // and the relation is gone, so every pattern prints as hex - which is
    // exactly what an auto-registered one always did, since a pattern
    // observed from traffic has no name to print.
    //
    // Same one-line-per-response contract as DESCRIBE and SHOW PAGE: a
    // count line, then one "\n"-escaped section per pattern, never a raw
    // newline byte.
    std::ostringstream os;
    os << "patterns=" << rows.value().size();

    for (const catalog::SysPatternRow& row : rows.value()) {
        // pattern_id in hex, because it is a hash: the decimal form of a
        // 64-bit fingerprint is 20 unreadable digits, and the thing an
        // operator does with this value is compare it to another one.
        os << "\\n" << "pattern_id=0x" << std::hex << row.pattern_id << std::dec
           << " oid=" << row.oid;

        // Origin and pinning are separate fields and are printed
        // separately, which is how the row stores them (rows.hpp). Both
        // read the same on every row today - `auto` and `no` - because
        // withdrawing declared patterns took away the only writer of
        // kOriginUser and of kPatternPinned; the fields stay on disk and
        // this prints what they hold rather than what it assumes.
        os << " origin=" << (row.origin == catalog::kOriginUser ? "user" : "auto")
           << " pinned=" << ((row.flags & catalog::kPatternPinned) != 0 ? "yes" : "no")
           << " class=" << static_cast<int>(row.stmt_class)
           << " uses=" << row.use_count
           << " last_seen=" << row.last_seen
           << " waystone=";
        // dir_depth is the authority on whether a directory exists
        // (rows.hpp); reporting the root alone would print page 0 for a
        // pattern that has none.
        if (catalog::HasWaystoneDirectory(row)) {
            os << "root=" << row.waystone_root << ",depth=" << static_cast<int>(row.dir_depth);
        } else {
            os << "none";
        }
        // A row this build cannot resolve is still listed, and saying so is
        // the point of listing it: it is dead weight from a fingerprint
        // version bump, and nothing will ever look it up again.
        if (!parser::IsCurrentFingerprintVersion(row.fingerprint_version)) {
            os << " stale=v" << row.fingerprint_version;
        }
    }
    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleShowAccess() {
    auto rows = catalog_.ListAccessStats();
    if (!rows.ok()) {
        return {ErrorReply(rows.status()), false, 0, rows.status()};
    }

    // Relation and column *names*, resolved here rather than stored: the
    // statistics row holds oids and a bitmap, which is what keeps it fixed
    // width, and this is an inspection surface that can afford a lookup.
    std::ostringstream os;
    os << "access_shapes=" << rows.value().size();

    for (const catalog::SysAccessStatRow& row : rows.value()) {
        os << "\\n";

        auto kind = exec::AccessKindOfStored(row.kind);
        os << "kind=" << (kind.has_value() ? exec::AccessKindName(*kind) : "?");

        auto access = catalog_.InitTableAccess(row.rel_id);
        os << " rel=";
        if (access.ok()) {
            // Unfiltered by design (DT3c): a diagnostic answers "what does
            // this instance hold" - ddl-transactional.md §5.
            auto name = catalog_.ListTables();
            bool named = false;
            if (name.ok()) {
                for (const catalog::SysObjectRow& obj : name.value()) {
                    if (obj.oid != row.rel_id) continue;
                    os << catalog::NameView(obj.name);
                    named = true;
                    break;
                }
            }
            if (!named) os << "oid=" << row.rel_id;
        } else {
            // The relation is gone or unreadable. The statistic outlives it
            // - nothing removes these rows - so say so rather than fail the
            // listing.
            os << "oid=" << row.rel_id;
        }

        os << " columns=[";
        bool first = true;
        for (std::uint16_t col = 0; col < 64; ++col) {
            if ((row.column_mask & (std::uint64_t{1} << col)) == 0) continue;
            if (!first) os << ',';
            first = false;
            if (access.ok() && col < access.value()->schema.columns.size()) {
                os << catalog::NameView(access.value()->schema.columns[col].name);
            } else {
                os << col;
            }
        }
        os << ']';

        os << " uses=" << row.use_count << " last_seen=" << row.last_seen;
    }
    return {os.str(), false};
}

namespace {

// A budget fraction as a percentage, at a precision chosen for the one
// question this field answers: *is this relation near the threshold?*
// Three decimals put 90.000% and 0.000% on the same scale; the exact
// consumption is `issued`, which is printed beside it and is a count
// rather than a rounding.
std::string PercentString(double fraction) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(3) << (fraction * 100.0);
    return os.str();
}

}  // namespace

DispatchOutcome CommandDispatcher::HandleShowBudget() {
    // Unfiltered by design (DT3c): a diagnostic surface answers "what does
    // this instance hold", not "what may this statement touch" - see
    // ddl-transactional.md §5.
    auto tables = catalog_.ListTables();
    if (!tables.ok()) {
        return {ErrorReply(tables.status()), false, 0, tables.status()};
    }

    // Built in two passes so the summary line can carry the warning count.
    // An operator scanning a long listing should not have to read every
    // row to learn that none of them is in trouble - K-M4's acceptance is
    // that crossing the threshold is *visible*, and a count at the top is
    // what makes it visible without a search.
    std::ostringstream rows;
    std::size_t listed = 0;
    std::size_t warning = 0;
    std::size_t exhausted = 0;

    for (const catalog::SysObjectRow& obj : tables.value()) {
        auto table_row = catalog_.GetSysTableRow(obj.oid);
        if (!table_row.ok()) {
            // A sys.objects row of type table with no sys.tables row is a
            // catalog inconsistency. Reported in place, for the reason
            // DESCRIBE gives about a bad type_val: seeing *which* relation
            // is broken is the point of an inspection command.
            rows << "\\n"
                 << "rel=" << catalog::NameView(obj.name) << " oid=" << obj.oid
                 << " error=" << table_row.status().message();
            ++listed;
            continue;
        }

        const catalog::KeystoneBudget budget =
            catalog::BudgetOf(table_row.value().next_id);
        ++listed;
        if (budget.warn) ++warning;
        if (budget.exhausted) ++exhausted;

        rows << "\\n"
             << "rel=" << catalog::NameView(obj.name) << " issued=" << budget.issued
             << " remaining=" << budget.remaining
             << " used=" << PercentString(budget.used_fraction) << "%"
             << " warn=" << (budget.warn ? "yes" : "no")
             << " exhausted=" << (budget.exhausted ? "yes" : "no");
    }

    // `capacity` belongs to the summary rather than to every row: it is the
    // same constant for every relation (K4's per-relation 2^40), and
    // repeating it once per line would be the widest column in the listing
    // carrying the least information.
    std::ostringstream os;
    os << "relations=" << listed << " warning=" << warning << " exhausted=" << exhausted
       << " capacity=" << catalog::kKeystoneBudgetCapacity
       << " warn_at=" << PercentString(catalog::kKeystoneBudgetWarnFraction) << "%";
    os << rows.str();
    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleListTables(Session& session) {
    // Listed under the session's view (DT4): `SHOW TABLES` is a route
    // into "what relations exist", so it answers the same question
    // DESCRIBE and SELECT do and must answer it the same way.
    const std::optional<txn::ReadView> view = ViewFor(session);
    auto tables = catalog_.ListTables(view.has_value() ? &*view : nullptr);
    if (!tables.ok()) {
        return {ErrorReply(tables.status()), false, 0, tables.status()};
    }

    std::ostringstream os;
    bool first = true;
    for (const auto& row : tables.value()) {
        if (!first) os << ' ';
        os << catalog::NameView(row.name);
        first = false;
    }
    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleShowPage(std::string_view args) {
    if (args.empty()) {
        return {"ERR SHOW PAGE requires a page id", false};
    }

    auto [id_token, option] = SplitFirstToken(args);

    bool show_values = false;
    if (!option.empty()) {
        if (!IEquals(option, "VALUES")) {
            return {"ERR unknown SHOW PAGE option: " + std::string(option), false};
        }
        show_values = true;
    }

    PageId page_id;
    auto [ptr, ec] = std::from_chars(id_token.data(), id_token.data() + id_token.size(), page_id);
    if (ec != std::errc() || ptr != id_token.data() + id_token.size()) {
        return {"ERR invalid page id: " + std::string(id_token), false};
    }

    // **GetForRead, not Get** - everything below only formats bytes.
    // `Get()` marks the frame dirty (page_store.hpp), which made a
    // diagnostic a *write*: on a peer it was refused outright ("core 1 may
    // not write page 1"), and in a release build - where that check is
    // compiled out - it dirtied a frame of a page this core does not own,
    // to be written back by the next Sync, checkpoint or eviction, and
    // left the peer's catalog eviction (a path AT-S2 retired) failing on
    // a dirty catalog frame so its cache never dropped again. PW1c's guard covered
    // the DML verbs; this one was a read all along.
    auto page = page_store_.GetForRead(page_id);
    if (!page.ok()) {
        return {ErrorReply(page.status()), false, 0, page.status()};
    }

    // A B+ tree internal node has no slot directory and no tuples, so it
    // gets its own render rather than a heap dump of nonsense - SHOW PAGE
    // is the tool someone reaches for when a descent went somewhere
    // unexpected, and it is useless if it cannot show the separators that
    // sent it there.
    if (storage::RawPageType(page.value().bytes()) ==
        static_cast<std::uint8_t>(PageType::kBtreeInternal)) {
        btree::InternalView node(page.value().bytes());
        std::ostringstream os;
        os << "page_id=" << page_id << "\\n"
           << "page_type=BTREE_INTERNAL\\n"
           << "page_lsn=" << storage::GetPageLsn(page.value().bytes()) << "\\n"
           << "stream_stamp=" << storage::GetPageStreamStamp(page.value().bytes()) << "\\n"
           << "level=" << node.level() << "\\n"
           << "nr_entries=" << node.entry_count() << "\\n"
           << "max_entries=" << btree::kInternalMaxEntries << "\\n"
           << "leftmost_child=" << node.leftmost_child();
        for (std::uint16_t i = 0; i < node.entry_count(); ++i) {
            auto entry = node.Entry(i);
            if (!entry.ok()) continue;
            os << "\\n"
               << "entry[" << i << "] sep_key=" << entry.value().sep_key
               << " child=" << entry.value().child;
        }
        return {os.str(), false};
    }

    heap::PageView view(page.value().bytes());
    const bool is_leaf = storage::RawPageType(page.value().bytes()) ==
                         static_cast<std::uint8_t>(PageType::kBtreeLeaf);

    // The wire protocol allows exactly one response line per command (see
    // this class's header comment / docs/spec/client-manual.md section 2), so a
    // raw newline byte can't appear here - it would desync any client that
    // reads "up to the next \n" as one reply. Instead, sections are joined
    // with the two-character escape "\n" (backslash + n); the bundled CLI
    // (tools/ckdbs_cli.py) unescapes it back into real newlines before
    // printing, giving a readable multi-line dump for developers without
    // breaking the one-line-per-response contract on the wire.
    std::ostringstream os;
    os << "page_id=" << page_id << "\\n"
       << "page_type=" << (is_leaf ? "BTREE_LEAF" : "HEAP") << "\\n"
       // The pair the rule-5 mount refusal named until AW-S1b (the
       // f19ead1 review's observability gap); the stamp is a diagnostic
       // since, and this is where it is read.
       << "page_lsn=" << storage::GetPageLsn(page.value().bytes()) << "\\n"
       << "stream_stamp=" << storage::GetPageStreamStamp(page.value().bytes()) << "\\n"
       << "min_key=" << view.min_key() << "\\n"
       << "nr_slots=" << view.slot_count() << "\\n"
       << "lower=" << view.lower() << "\\n"
       << "upper=" << view.upper() << "\\n"
       << "free_space=" << view.free_space() << "\\n"
       << "next_page_id=" << view.next_page_id();

    for (std::uint16_t i = 0; i < view.slot_count(); ++i) {
        auto slot = view.DebugSlotInfo(i);
        if (!slot.ok()) continue;
        os << "\\n"
           << "slot[" << i << "] offset=" << slot.value().offset
           << " length=" << slot.value().length << " dead=" << (slot.value().dead ? 1 : 0);

        if (show_values && !slot.value().dead) {
            auto tuple = view.ReadTuple(i);
            if (tuple.ok()) {
                os << " value=" << HexEncode(tuple.value().payload);
            }
        }
    }

    return {os.str(), false};
}

// `t` or `ns.t` in a **raw argument** (AF-T3). The debug surface's commands
// take a bare token rather than a parse - `DESCRIBE`, `SHOW RELAYOUT`, the
// bare `CREATE TABLE <name>` form - and a qualifier has to mean the same
// thing there as in the grammar, or the two surfaces disagree about what a
// name is. One dot deep, exactly as `Parser::ParseQualifiedName`.
//
// A trailing or leading dot is left alone: the halves come back as written
// and the catalog refuses the empty one by name, which beats this helper
// inventing a syntax error for a surface that has no byte offsets.
namespace {

std::pair<std::string_view, std::string_view> SplitQualifiedArg(std::string_view arg) {
    const std::size_t dot = arg.find('.');
    if (dot == std::string_view::npos) return {std::string_view{}, arg};
    return {arg.substr(0, dot), arg.substr(dot + 1)};
}

}  // namespace

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
        // orders.t` means one thing on both surfaces. Before this it meant
        // a relation literally called "orders.t".
        const auto [qualifier, name] = SplitQualifiedArg(args);

        // Before the collision check, for `HandleCreateTableSql`'s reason:
        // the `EXISTS` arm below would otherwise answer a statement whose
        // namespace does not exist, absorbing the qualifier instead of
        // verifying it.
        const std::optional<txn::ReadView> ns_view = ViewFor(session);
        auto create_ns = ResolveCreateNamespace(qualifier, /*byte_offset=*/0,
                                                ns_view.has_value() ? &*ns_view : nullptr);
        if (!create_ns.ok()) {
            return {ErrorReply(create_ns.status()), false, 0, create_ns.status()};
        }

        auto existing = catalog_.FindTableOidByName(name);
        if (existing.ok()) {
        // **`EXISTS` is truthful only about the name, so a qualifier that
        // disagrees gets a sentence rather than that token** (the AF-T3
        // review's finding 5). Relation names are instance-global, so
        // `ledger.plain` cannot be created while a `plain` sits in `public`
        // - but replying `EXISTS` to it asserts that `ledger.plain` exists,
        // which is the one thing that is false. The qualifier check answers
        // with the namespace the name is actually held in.
            if (Status s = catalog_.CheckRelationQualifier(qualifier, name, existing.value());
                !s.ok()) {
                return {ErrorReply(s), false, 0, s};
            }
            return {"EXISTS oid=" + std::to_string(existing.value()), false};
        }
        if (existing.status().code() != StatusCode::kNotFound) {
            return {ErrorReply(existing.status()), false, 0, existing.status()};
        }
        if (auto refused = RefuseIfNameHeldByPendingDrop(name, session); refused.has_value()) {
            return *refused;
        }

        catalog::Schema schema;
        DdlScope ddl = DdlScopeFor(scope);
        // **SUS-1: BTREE, the third spelling of the storage default.**
        // `HandleCreateTableSql`'s fallback and `ast.hpp`'s field are the
        // other two; the work order names only those, so this one was
        // missed. Inert today - `schema` is empty and `CreateTable` refuses
        // a relation with no columns before the storage form is read - but
        // `kHeap` on a creation path is the exact shape SUS-1 forbids, and
        // it would have become live the moment the bare form gained columns.
        auto oid = catalog_.CreateTable(create_ns.value(), name, schema,
                                         catalog::ClusteredType::kBtree, ddl.trx_id, ddl.sink());
        NoteDdlRows(ddl);  // before the status, for the partial-write reason above
        if (!oid.ok()) {
            return {ErrorReply(oid.status()), false, 0, oid.status()};
        }
        return {"CREATED oid=" + std::to_string(oid.value()), false};
    });
}

DispatchOutcome CommandDispatcher::HandleDescribe(std::string_view args,
                                                  Session& session) {
    if (args.empty()) {
        return {"ERR DESCRIBE requires a table name", false};
    }

    const auto [qualifier, name] = SplitQualifiedArg(args);
    const std::optional<txn::ReadView> view = ViewFor(session);
    auto oid = catalog_.FindTableOidByName(name, view.has_value() ? &*view : nullptr);
    if (!oid.ok()) {
        return {ErrorReply(oid.status()), false, 0, oid.status()};
    }
    if (Status s = catalog_.CheckRelationQualifier(qualifier, name, oid.value(),
                                                   /*byte_offset=*/0,
                                                   view.has_value() ? &*view : nullptr);
        !s.ok()) {
        return {ErrorReply(s), false, 0, s};
    }

    auto table_row = catalog_.GetSysTableRow(oid.value());
    if (!table_row.ok()) {
        return {ErrorReply(table_row.status()), false, 0, table_row.status()};
    }

    // A relation with no registered columns is a fact, not a failure: the
    // bootstrap catalog tables encode their rows through their own
    // SysXxxRow::Encode() rather than the schema-driven row codec, so they
    // have no sys.columns entries. Reporting the header with columns=0
    // beats refusing to describe them at all.
    catalog::Schema schema;
    auto built = catalog_.BuildSchemaFromColumns(oid.value());
    if (built.ok()) {
        schema = std::move(built.value());
    } else if (built.status().code() != StatusCode::kNotFound) {
        return {ErrorReply(built.status()), false, 0, built.status()};
    }

    const char* clustered =
        table_row.value().clustered_type == catalog::ClusteredType::kBtree ? "BTREE" : "HEAP";

    // Same one-line-per-response contract as SHOW PAGE and SELECT: a
    // summary line, then one "\n"-escaped section per column, never a raw
    // newline byte.
    // PW2-3: the row's desc_page_id is the CREATE-time root; the current
    // one lives in the anchor. Read directly, not through InitTableAccess
    // (the 96b0343 review's C5): DESCRIBE resolves its name through the
    // session's view, and filling the unfiltered shared cache from a
    // view-filtered read is DT3's rule broken sideways - and an anchor
    // Corruption should reach the operator on the one surface they
    // diagnose with, not be swallowed by a fallback.
    PageId current_root = table_row.value().desc_page_id;
    if (table_row.value().anchor_page_id != kInvalidPageId) {
        auto anchor = page_store_.GetForRead(table_row.value().anchor_page_id);
        if (anchor.ok() &&
            storage::ValidatePageHeader(anchor.value().bytes(), PageType::kAnchor).ok()) {
            current_root = storage::AnchorClusteredRoot(anchor.value().bytes());
        } else if (!anchor.ok() && anchor.status().code() == StatusCode::kCorruption) {
            return {ErrorReply(anchor.status()), false, 0, anchor.status()};
        }
    }
    std::ostringstream os;
    os << "oid=" << oid.value() << " root_page_id=" << current_root
       << " clustered_type=" << clustered
       // Where `key_mode=` used to print a declaration, this prints an
       // observation (docs/spec/heap-and-tuple.md §4.1): whether any id has landed
       // below the mark, which is what decides whether a page's slot order is
       // still its key order. Kept on the line rather than dropped, because
       // the question someone reads this line for - "can I trust the pk order
       // of a walk here" - is the one it now answers.
       << " key_order=" << catalog::KeyOrderName(table_row.value().key_order)
       << " next_id=" << table_row.value().next_id
       << " columns=" << schema.columns.size();

    // K4's lifetime budget, beside the sequence it is derived from. Here as
    // well as in SHOW BUDGET because this is where someone already looks
    // after reading `next_id` and wondering what it means - and because
    // "without arithmetic" (K-M4) is a claim about the place the number is
    // read, not about one command.
    //
    // `ids_issued` is deliberately not `rows`: it counts ids spent, gaps
    // included (keystone_budget.hpp).
    const catalog::KeystoneBudget budget = catalog::BudgetOf(table_row.value().next_id);
    os << " ids_issued=" << budget.issued << " ids_remaining=" << budget.remaining
       << " budget_used=" << PercentString(budget.used_fraction) << "%";
    if (budget.warn) {
        os << " budget_warning=yes";
    }
    if (budget.exhausted) {
        os << " budget_exhausted=yes";
    }

    // The tree's shape, which is the thing you actually want to know about
    // a btree relation and cannot get from anywhere else. Reported
    // best-effort: a relation whose index pages are unreadable still has a
    // describable schema, and refusing the whole command over it would
    // remove the tool at the moment it is needed.
    if (table_row.value().clustered_type == catalog::ClusteredType::kBtree) {
        // current_root, not the row: post-PW2-3 the row is the CREATE-time
        // root, and a height computed from a superseded interior page would
        // contradict the root printed two fields up (the f5686f8 review's
        // C7).
        auto height = btree::BtreeHeight(page_store_, current_root);
        auto leaves = btree::BtreeLeafCount(page_store_, current_root);
        os << " height=" << (height.ok() ? std::to_string(height.value()) : std::string("?"))
           << " leaves=" << (leaves.ok() ? std::to_string(leaves.value()) : std::string("?"));
    }

    // The relation's access entry, for the facts that are not on a
    // sys.columns row - today its foreign keys. Best-effort like the btree
    // shape above: a bootstrap catalog relation has no sys.columns rows and
    // therefore no access entry, and that is a relation with no foreign keys
    // rather than a DESCRIBE that should fail.
    auto access = catalog_.InitTableAccess(oid.value());

    for (std::size_t i = 0; i < schema.columns.size(); ++i) {
        const catalog::SysColumnRow& col = schema.columns[i];

        // A type_val with no sys.types row is a catalog inconsistency, not
        // a reason to fail the whole DESCRIBE - report it in place, since
        // seeing *which* column is broken is the point of the command.
        auto type_row = catalog_.ResolveTypeByVal(col.type_val);
        const std::string base_name = type_row.ok()
                                          ? std::string(catalog::NameView(type_row.value().name))
                                          : "?type_val=" + std::to_string(col.type_val);
        // The declared form - `decimal(10,2)`, `char(8)`, `int64` - rather
        // than a bare name beside a `len` a reader would have to interpret.
        // For a decimal `len` is the packed (p, s) pair, so printing it as
        // a width would be actively misleading (catalog/rows.hpp).
        const std::string type_name = catalog::ColumnTypeText(col, base_name);

        // Column 0 is the Keystone primary key by construction, not by a
        // stored flag - heap-and-tuple.md section 4 makes it positional.
        const bool is_pk = i == 0;

        // Neither `yes` nor `no` is true of a pk any more, and printing
        // either would be printing something untrue for a field's
        // convenience: the sequence runs when the INSERT omits the key and
        // does not when the INSERT names one, and both are legal on every
        // relation (heap-and-tuple.md section 4.1). So the pk says which -
        // `if-omitted` - and every other column keeps the `no` it always had.
        os << "\\n"
           << "pos=" << col.pos << " name=" << catalog::NameView(col.name) << " type=" << type_name
           << " notnull=" << (col.notnull ? "yes" : "no")
           << " pk=" << (is_pk ? "yes" : "no")
           << " autoincrement=" << (is_pk ? "if-omitted" : "no");

        // The declared cabin policy (docs/spec/cabin.md), printed for every
        // non-pk column. The *effective* value, so `auto` covers both "the
        // engine may decide" and "nothing was said" - the difference is
        // recorded on disk and matters only to whoever writes the promotion
        // pipeline, where a listing that showed it would be noise. Whether a
        // Cabin actually exists is `SHOW CABINS`; this is what the schema
        // permits.
        if (!is_pk) {
            const std::uint8_t policy = catalog::EffectiveCabinPolicy(col.cabin_policy);
            os << " cabin=" << (policy == catalog::kCabinPolicyDisabled  ? "no"
                                : policy == catalog::kCabinPolicyEnabled ? "yes"
                                                                         : "auto");

            // What this column references, if anything
            // (docs/spec/foreign-keys.md §1). Read from the relation's own
            // outgoing list rather than by asking the catalog per column,
            // which is the same absence rule the cabin mask follows.
            if (access.ok()) {
                if (const catalog::ForeignKeyRef* fk =
                        access.value()->ForeignKeyOn(static_cast<std::uint16_t>(i));
                    fk != nullptr) {
                    os << " references=" << RelationNameOf(fk->rel_oid);
                }
            }
        }
    }

    return {os.str(), false};
}

namespace {

// The one spelling of a successful CREATE INDEX (docs/spec/client-manual.md).
// `entries=0` is a literal the manual documents, and the backfill over a
// populated relation makes it false; it stays until the field is dropped or
// counted - either is client-visible, so neither is done in passing (the
// PW1c-6b-3 review's finding).
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
        // resolves the index list this statement left.
        //
        // What it replaces: an owner-built `CREATE INDEX` with a refusal
        // window only the owner could see (PW1c-6b), and a local build that
        // opened none (D6) - so since AT-S5 a row written on another core
        // during a backfill was missing from the finished index. A name that
        // does not resolve takes no borrow and falls through to the refusal
        // `exec::CreateIndex`/`DropIndex` owns, with its byte position.
        const std::optional<txn::ReadView> resolve_view = ViewFor(session);
        std::optional<catalog::Oid> relation;
        if (stmt.drop) {
            if (auto ix = catalog_.FindIndexByName(stmt.index_name); ix.ok()) {
                relation = ix.value().table_oid;
            }
        } else if (auto oid = catalog_.FindTableOidByName(
                       stmt.table_name, resolve_view.has_value() ? &*resolve_view : nullptr);
                   oid.ok()) {
            relation = oid.value();
        }
        if (relation.has_value()) {
            if (std::optional<Status> held = BorrowRelationForDdl(scope.txn, *relation);
                held.has_value()) {
                return {ErrorReply(*held), false, 0, *held};
            }
        }

        if (stmt.drop) {
            // **Isolated on every core since AT-S5e.** DT5 shipped this as
            // atomic *and isolated* on the strength of `SHOW INDEXES`
            // filtering, which was wrong - index maintenance reads the list
            // with a null view, so another session's INSERT wrote no entry
            // for an index whose drop had not committed - and DT9 closed it
            // at the read: an unfiltered catalog read counts a delete-mark
            // only once its deleter is no longer in flight (`catalog.cpp`'s
            // `ScanAll`). That predicate is one core's (`IsInFlight`), so the
            // claim was core-0-scoped and a drop on a peer-owned relation was
            // refused inside a transaction (PW1c-6b-4). A writer on another
            // core still asks it at its resolution and may leave the index
            // out; what makes that harmless is the relation `X` above - the
            // writer writes nothing until the drop decides - and the decide
            // moving the schema word before it releases
            // (`Transaction::NoteWroteCatalog`), so the writer re-resolves.
            // Without the second a rollback's woken writer kept its memo and
            // wrote no entry into the index it restored (the AT-S5e review's
            // C1).
            DdlScope ddl = DdlScopeFor(scope);
            catalog::CatalogRowChange change;
            auto index_oid = exec::DropIndex(catalog_, stmt, ddl.trx_id,
                                             ddl.txn != nullptr ? &change : nullptr);
            if (ddl.txn != nullptr && change.page_id != kInvalidPageId) {
                txn_->NoteDeleteMark(*ddl.txn, static_cast<std::uint32_t>(change.rel_oid),
                                     change.page_id, change.slot, change.oid,
                                     change.prior_trx_id, change.prior_undo_ptr);
                MarkHoldsDdl(*ddl.txn);
            }
            if (!index_oid.ok()) {
                return {ErrorReply(index_oid.status()), false, 0, index_oid.status()};
            }
            std::ostringstream os;
            os << "DROPPED INDEX name=" << stmt.index_name << " index_oid=" << index_oid.value();
            if (logging(LogLevel::kInfo)) {
                log_->Info("ddl", "dropped index '" + stmt.index_name + "'");
            }
            return {os.str(), false};
        }

        DdlScope create_ddl = DdlScopeFor(scope);
        catalog::CatalogRowRef created_row;
        // Resolved under the session's view (spec §5's rule: an index is a
        // schema object and CREATE INDEX is a resolution route). An index
        // must not be built against a relation the caller cannot see.
        auto result = exec::CreateIndex(catalog_, page_store_, stmt, create_ddl.trx_id,
                                        create_ddl.txn != nullptr ? &created_row : nullptr,
                                        resolve_view.has_value() ? &*resolve_view : nullptr, wal_);
        // Before the status is read: a create that failed after the catalog
        // row went down still left it there.
        if (create_ddl.txn != nullptr && created_row.page_id != kInvalidPageId) {
            create_ddl.written.push_back(created_row);
            NoteDdlRows(create_ddl);
        }
        if (!result.ok()) return {ErrorReply(result.status()), false, 0, result.status()};
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

DispatchOutcome CommandDispatcher::HandleShowIndexes(Session& session) {
    // **Reclassified 2026-08-16.** This was grouped with the diagnostic
    // surfaces, which answer "what does this instance hold". It does not:
    // it answers "which indexes exist", a schema question, and every
    // schema route has to give one answer (DT3c's rule). Grouping it with
    // `SHOW ACCESS` made an uncommitted `DROP INDEX` visible to everyone
    // while the rest of the catalog hid it.
    const std::optional<txn::ReadView> view = ViewFor(session);
    auto rows = catalog_.ListIndexes(view.has_value() ? &*view : nullptr);
    if (!rows.ok()) {
        return {ErrorReply(rows.status()), false, 0, rows.status()};
    }

    std::ostringstream os;
    os << "indexes=" << rows.value().size();

    for (const catalog::SysIndexRow& row : rows.value()) {
        os << "\\n";
        os << "index_oid=" << row.index_oid << " name=" << catalog::NameView(row.name);

        // Names resolved here rather than stored, exactly as SHOW CABINS and
        // SHOW ACCESS do: the row holds oids so it stays fixed width, and an
        // inspection surface can afford the lookup.
        auto access = catalog_.InitTableAccess(row.table_oid);
        os << " rel=";
        bool named = false;
        // Unfiltered by design (DT3c): a diagnostic surface answers "what
        // does this instance hold" - ddl-transactional.md §5.
        if (auto tables = catalog_.ListTables(); tables.ok()) {
            for (const catalog::SysObjectRow& obj : tables.value()) {
                if (obj.oid != row.table_oid) continue;
                os << catalog::NameView(obj.name);
                named = true;
                break;
            }
        }
        if (!named) os << "oid=" << row.table_oid;

        const auto write_columns = [&](const char* label, const std::uint16_t* cols,
                                       std::size_t n) {
            os << ' ' << label << "=(";
            for (std::size_t i = 0; i < n; ++i) {
                if (i > 0) os << ',';
                if (access.ok() && cols[i] < access.value()->schema.columns.size()) {
                    os << catalog::NameView(access.value()->schema.columns[cols[i]].name);
                } else {
                    os << cols[i];
                }
            }
            os << ')';
        };
        // Declared order, which is the order the key encoding concatenates
        // them in - so what is printed is what a probe must match a prefix
        // of, not a sorted set.
        write_columns("keys", row.key_cols.data(), row.nkeys);
        if (row.ncovered > 0) {
            write_columns("covering", row.covered_cols.data(), row.ncovered);
        }

        // The anchored root, not the row's: post-PW2-3 the sys.indexes row
        // is CREATE-fixed and the access entry resolved the current root
        // through the anchor (the f5686f8 review's C7).
        PageId shown_root = row.root_page_id;
        if (access.ok()) {
            for (const auto& ref : access.value()->indexes) {
                if (ref.index_oid == row.index_oid) {
                    shown_root = ref.root_page_id;
                    break;
                }
            }
        }
        os << " root_page=" << shown_root << " key_width=" << row.key_width
           << " entry_width=" << row.entry_width;

        // The physical half. Height and entry count are what say whether an
        // index is worth its write hook, and the catalog cannot answer
        // either: it stores that an index exists, never what is in it.
        const index::IndexLayout layout{row.key_width,
                                        static_cast<std::uint16_t>(row.entry_width -
                                                                   row.key_width -
                                                                   index::kIndexPkWidth)};
        auto height = index::IndexHeight(page_store_, shown_root, layout);
        auto entries = index::IndexEntryCount(page_store_, shown_root, layout);
        if (height.ok() && entries.ok()) {
            os << " height=" << height.value() << " entries=" << entries.value();
        } else {
            // Not zeros: an unreadable tree is unknown, and printing zeros
            // would read as "empty" when the truth is "could not be walked".
            os << " height=- entries=-";
        }
    }
    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleCabin(std::string_view line) {
    // H6 step 2: the parse leg. One of `observability.md` §10's three
    // request-level spans, and the cheapest to attribute wrongly - a
    // statement that is slow to *parse* looks identical from outside to
    // one that is slow to run.
    auto parsed = [&] {
        stats::SpanScope span(trace_, stats::Layer::kParse);
        return parser::Parse(line);
    }();
    if (!parsed.ok()) {
        return {ErrorReply(parsed.status()), false, 0, parsed.status()};
    }
    if (!std::holds_alternative<parser::CabinStmt>(parsed.value())) {
        return {"ERR expected a CREATE CABIN or DROP CABIN statement", false};
    }
    const auto& stmt = std::get<parser::CabinStmt>(parsed.value());

    // One handler for both, because the two statements share a parse and a
    // reply shape and differ only in which catalog call they reach - the same
    // reason CabinStmt is one struct (ast.hpp).
    if (stmt.drop) {
        auto cabin_id = exec::DropCabin(catalog_, stmt);
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
        os << "DROPPED CABIN on=" << stmt.table_name << '.' << stmt.column_name
           << " cabin_id=" << cabin_id.value();
        if (logging(LogLevel::kInfo)) {
            log_->Info("ddl", "dropped cabin on " + stmt.table_name + "." + stmt.column_name);
        }
        return {os.str(), false};
    }

    auto result = exec::CreateCabin(catalog_, stmt);
    if (!result.ok()) {
        return {ErrorReply(result.status()), false, 0, result.status()};
    }
    if (Status s = AwaitDdlDurability(); !s.ok()) return {ErrorReply(s), false, 0, s};

    std::ostringstream os;
    // `observed=0` is printed rather than left out, because it is the thing
    // most likely to be misunderstood: creating a Cabin observes nothing and
    // accelerates nothing until traffic fills it (spec §4's miss path).
    os << "CREATED CABIN on=" << stmt.table_name << '.' << stmt.column_name
       << " cabin_id=" << result.value().cabin_id << " column=" << result.value().col_pos
       << " observed=0";
    for (const std::string& warning : result.value().warnings) {
        os << "\\n" << "WARN " << warning;
    }
    if (logging(LogLevel::kInfo)) {
        log_->Info("ddl", "created cabin on " + stmt.table_name + "." + stmt.column_name);
    }
    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleAlter(std::string_view line,
                                               Session& session) {
    // H6 step 2: the parse leg. One of `observability.md` §10's three
    // request-level spans, and the cheapest to attribute wrongly - a
    // statement that is slow to *parse* looks identical from outside to
    // one that is slow to run.
    auto parsed = [&] {
        stats::SpanScope span(trace_, stats::Layer::kParse);
        return parser::Parse(line);
    }();
    if (!parsed.ok()) {
        return {ErrorReply(parsed.status()), false, 0, parsed.status()};
    }
    if (!std::holds_alternative<parser::AlterStmt>(parsed.value())) {
        return {"ERR expected an ALTER TABLE statement", false};
    }
    const auto& stmt = std::get<parser::AlterStmt>(parsed.value());

    // You cannot alter a relation you cannot see (DT3c).
    const std::optional<txn::ReadView> view = ViewFor(session);
    auto oid =
        catalog_.FindTableOidByName(stmt.table_name, view.has_value() ? &*view : nullptr);
    if (!oid.ok()) {
        return {ErrorReply(oid.status()), false, 0, oid.status()};
    }
    if (Status s = catalog_.CheckRelationQualifier(stmt.schema, stmt.table_name, oid.value(),
                                                   stmt.byte_offset,
                                                   view.has_value() ? &*view : nullptr);
        !s.ok()) {
        return {ErrorReply(s), false, 0, s};
    }

    // AL7: the catalog's own names are load-bearing for bootstrap and are
    // nobody's to change - refused here so both forms share the answer,
    // and RenameTable's own guard is defense rather than the door.
    //
    // Unfiltered, and filtering could not change it: this asks whether an
    // **already-resolved** oid belongs to a system namespace, and every
    // system relation is a bootstrap row visible to every view (DT3c).
    auto tables = catalog_.ListTables();
    if (!tables.ok()) {
        return {ErrorReply(tables.status()), false, 0, tables.status()};
    }
    // An oid names one row, so the search **stops** at it rather than
    // walking every relation in the instance to find nothing more.
    const auto named = std::find_if(
        tables.value().begin(), tables.value().end(),
        [&](const catalog::SysObjectRow& row) { return row.oid == oid.value(); });
    // AF-P3: identity, not permission (well_known.hpp).
    if (named != tables.value().end() && catalog::IsSystemNamespace(named->namespace_oid)) {
        return {"ERR '" + stmt.table_name + "' is a system relation and cannot be altered", false};
    }

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
        stmt.rename_column
            ? catalog_.RenameColumn(oid.value(), stmt.old_column, stmt.new_name)
            : catalog_.RenameTable(oid.value(), stmt.new_name);
    if (!renamed.ok()) {
        return {ErrorReply(renamed), false, 0, renamed};
    }
    // Transactionless like the pattern and assertion routes: the renamed
    // catalog rows are logged but no commit record follows them.
    if (Status s = AwaitDdlDurability(); !s.ok()) return {ErrorReply(s), false, 0, s};

    std::ostringstream os;
    if (stmt.rename_column) {
        os << "RENAMED COLUMN " << stmt.table_name << '.' << stmt.old_column << " TO "
           << stmt.new_name;
    } else {
        os << "RENAMED TABLE " << stmt.table_name << " TO " << stmt.new_name;
    }
    if (logging(LogLevel::kInfo)) {
        log_->Info("ddl", "renamed " + stmt.table_name +
                              (stmt.rename_column ? "." + stmt.old_column : std::string()) +
                              " to " + stmt.new_name);
    }
    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleDropTable(std::string_view line,
                                                   Session& session) {
    return InDdlStatement(session, [&](WriteScope& scope) -> DispatchOutcome {
        // H6 step 2: the parse leg. One of `observability.md` §10's three
    // request-level spans, and the cheapest to attribute wrongly - a
    // statement that is slow to *parse* looks identical from outside to
    // one that is slow to run.
    auto parsed = [&] {
        stats::SpanScope span(trace_, stats::Layer::kParse);
        return parser::Parse(line);
    }();
        if (!parsed.ok()) {
            return {ErrorReply(parsed.status()), false, 0, parsed.status()};
        }
        if (!std::holds_alternative<parser::DropTableStmt>(parsed.value())) {
            return {"ERR expected a DROP TABLE statement", false};
        }
        const auto& stmt = std::get<parser::DropTableStmt>(parsed.value());

        // Nor drop one (DT3c).
        const std::optional<txn::ReadView> drop_view = ViewFor(session);
        auto oid = catalog_.FindTableOidByName(
            stmt.table_name, drop_view.has_value() ? &*drop_view : nullptr);
        if (!oid.ok()) {
            return {ErrorReply(oid.status()), false, 0, oid.status()};
        }
        if (Status s = catalog_.CheckRelationQualifier(
                stmt.schema, stmt.table_name, oid.value(), stmt.byte_offset,
                drop_view.has_value() ? &*drop_view : nullptr);
            !s.ok()) {
            return {ErrorReply(s), false, 0, s};
        }

        // DT3's RESTRICT, both blockers named. A referencing foreign key
        // blocks at the *declared* level - the constraint exists whether or
        // not rows do - which is the check known-gaps.md said was waiting for
        // exactly this caller.
        auto fkeys = catalog_.ListForeignKeys();
        if (!fkeys.ok()) {
            return {ErrorReply(fkeys.status()), false, 0, fkeys.status()};
        }
        for (const catalog::SysFkeyRow& fk : fkeys.value()) {
            if (fk.parent_rel_oid != oid.value()) continue;
            return {"ERR relation '" + stmt.table_name + "' is referenced by a foreign key on '" +
                        RelationNameOf(fk.child_rel_oid) + "'; drop the referencing relation first",
                    false};
        }

        // An enforcing constraint is not allowed to die quietly - ALTER's AL4
        // argument, same predicate, third caller.
        auto restricting = exec::AssertionsOnRelation(catalog_, page_store_, oid.value());
        if (!restricting.ok()) {
            return {ErrorReply(restricting.status()), false, 0, restricting.status()};
        }
        if (!restricting.value().empty()) {
            return {"ERR assertion '" + restricting.value().front().name +
                        "' is declared on this relation; DROP ASSERTION first",
                    false};
        }

        // ---- AO-S6e-b: the relation `X`, and what waits for what --------
        //
        // Taken **after** the RESTRICT checks and before the first catalog
        // write: a drop refused for a foreign key or an assertion is
        // refused whether or not anybody is reading, and waiting first
        // would make a client wait to be told something the catalog knew
        // at once.
        //
        // What this buys is stated in `drop-table.md` DT8 rather than
        // implied here: a reader **already positioned** in the relation
        // finishes its statement against a live schema instead of meeting
        // the post-park re-bind's clean error. It does not make the drop
        // isolated - a read that starts after this grant takes no borrow
        // and reads on under DT1, which is `ddl-transactional.md` §5a
        // unchanged.
        if (std::optional<Status> held = BorrowRelationForDdl(scope.txn, oid.value());
            held.has_value()) {
            return {ErrorReply(*held), false, 0, *held};
        }

        std::vector<std::uint64_t> dropped_cabins;
        // Transactional when inside an explicit transaction (DT5): the
        // dependents are delete-marked instead of retired, and every change -
        // the marks and the `sys.objects` retype - goes on the trail so
        // `Abort` undoes it. Registered before the status is read, for the
        // same reason CREATE's rows are: a drop that failed partway still
        // changed rows.
        DdlScope ddl = DdlScopeFor(scope);
        std::vector<catalog::CatalogRowChange> changed;
        Status dropped =
            catalog_.DropTable(oid.value(), dropped_cabins, ddl.trx_id,
                               ddl.txn != nullptr ? &changed : nullptr);
        NoteCatalogRowChanges(ddl, changed);
        if (Status s = dropped; !s.ok()) {
            return {ErrorReply(s), false, 0, s};
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
                                  "); pages orphaned pending reclamation");
        }
        return {"DROPPED TABLE " + stmt.table_name + " oid=" + std::to_string(oid.value()), false};
    });
}

// `CREATE NAMESPACE <name>` / `DROP NAMESPACE <name>` (AF-T3, AF-6's
// operator-taken shape (a); `docs/spec/namespace.md` NS7).
//
// One handler for both, as the parser has one production: they differ only
// in which catalog call they reach. Under `InDdlStatement` like every other
// DDL route, so the one `sys.objects` row this writes is on a real
// transaction's trail and a crash mid-statement has a loser to roll back -
// which matters more here than it looks, because AF-T2 makes the namespace
// the thing that decides where every relation in it will live.
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

        const std::optional<txn::ReadView> ns_view = ViewFor(session);
        auto oid = catalog_.FindNamespaceOidByName(stmt.name,
                                                   ns_view.has_value() ? &*ns_view : nullptr);
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

// `SHOW NAMESPACES` (`docs/spec/namespace.md` NS9): `sys.objects` filtered
// by type, the way `SHOW TABLES` filters by `kTypeTable`. The two
// bootstrap namespaces are listed first and by their **SQL spellings** -
// `sys` and `public` - rather than by the registry names their rows carry
// ("namespaceSys"/"namespacePublic"), because what a user needs from this
// command is the word they would write in a statement.
DispatchOutcome CommandDispatcher::HandleShowNamespaces(Session& session) {
    // Under the session's view, exactly as `SHOW TABLES` is (DT4): a
    // namespace is a schema object, so the route that reports which ones
    // exist has to answer the way every other resolution route does. A
    // surface that leaked an uncommitted `CREATE NAMESPACE` would be the
    // `SHOW INDEXES` defect again, one object kind over.
    const std::optional<txn::ReadView> view = ViewFor(session);
    auto live = catalog_.ListNamespaces(view.has_value() ? &*view : nullptr);
    if (!live.ok()) {
        return {ErrorReply(live.status()), false, 0, live.status()};
    }
    std::ostringstream os;
    os << catalog::kNamespaceSysName << ' ' << catalog::kNamespacePublicName;
    for (const catalog::SysObjectRow& row : live.value()) {
        os << ' ' << catalog::NameView(row.name);
    }
    return {os.str(), false};
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
        // Until then the owner's held it and a drop on core 0 sent it a
        // fire-and-forget `done(aborted)` to forget it.
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
    // **The session's own transaction is tested first**, by what it holds
    // rather than by who the table reports (the review's C7): with it and
    // another writer both holding the relation, the table may name the
    // other, and waiting for it would end in the same refusal. Resolved
    // under the session's view, as `HandleIndex` resolves; a name that does
    // not resolve takes no borrow and falls to `PrepareAssertionDef`.
    struct BuildLock {
        txn::TransactionManager* manager = nullptr;
        txn::Transaction* txn = nullptr;
        ~BuildLock() {
            if (txn == nullptr) return;
            (void)manager->Abort(*txn);
            manager->Release(*txn);  // `Abort` keeps it in `live_` by design
        }
    } build_lock;
    if (locks_ != nullptr && txn_ != nullptr) {
        const std::optional<txn::ReadView> view = ViewFor(session);
        if (auto rel = catalog_.FindTableOidByName(stmt.table_name,
                                                   view.has_value() ? &*view : nullptr);
            rel.ok()) {
            const txn::Transaction* own = session.transaction();
            if (own != nullptr && own->borrows().Holds(txn::LockKey::Relation(rel.value()))) {
                const Status mine = Status::TxnConflict(
                    "relation '" + stmt.table_name +
                    "' is held by this session's own transaction; CREATE ASSERTION reads "
                    "settled state - retry when it has ended");
                return {ErrorReply(mine), false, 0, mine};
            }
            auto begun = txn_->Begin(txn::IsolationLevel::kReadCommitted);
            if (!begun.ok()) return {ErrorReply(begun.status()), false, 0, begun.status()};
            build_lock.manager = txn_;
            build_lock.txn = begun.value();
            if (std::optional<Status> held =
                    BorrowRelationForDdl(build_lock.txn, rel.value(), /*poisons=*/false);
                held.has_value()) {
                return {ErrorReply(*held), false, 0, *held};
            }
        }
    }

    // The visibility the build's scan reads under: latest settled state,
    // minted here exactly as a FK check's view is. A dispatcher with no
    // manager reads everything, which is the pre-MVCC engine and what every
    // socket-free test runs on.
    txn::ReadView check_view = txn::ReadView::Everything();
    if (txn_ != nullptr) check_view = txn_->MintCheckView(txn::kNoTrxId);

    // Through ErrorReply, not a bare "ERR ": the build can refuse with the
    // two coded spellings - ASSERTION_VIOLATION for data already past the
    // bound, TXN_CONFLICT for an unsettled relation - and both are
    // compatibility surfaces a client switches on.
    auto created = exec::CreateAssertion(catalog_, page_store_, stmt, check_view, wal_);
    if (!created.ok()) {
        return {ErrorReply(created.status()), false, 0, created.status()};
    }
    exec::AssertionDdlResult& result = created.value();
    const bool adopted = result.live.has_value();
    if (adopted) {
        enforcer_->Adopt(std::move(*result.live));
    }
    // After the adoption, deliberately (the review's asymmetry note): a
    // sync failure then answers ERR with the live registry still enforcing
    // what the log already holds - over-enforcing until the operator
    // retries, where the other order left a durably created constraint
    // unenforced on the running instance.
    if (Status s = AwaitDdlDurability(); !s.ok()) return {ErrorReply(s), false, 0, s};

    // Truthful now in the other direction: the check runs (AST07), so a
    // freshly created assertion **is** enforcing - and says so. The
    // conjunction matters after a restart, when the catalog row survives
    // and this registry does not; SHOW derives the same answer the same
    // way, so the two surfaces cannot disagree.
    std::ostringstream os;
    os << "CREATED ASSERTION name=" << stmt.name
       << " assertion_id=" << result.assertion_id << " on=" << stmt.table_name
       << " cabin_root=" << result.cabin_root << " rows=" << result.rows_incorporated
       << " groups=" << result.group_count
       << " enforcing=" << ((kWritePathEnforcesAssertions && adopted) ? 1 : 0);
    if (logging(LogLevel::kInfo)) {
        log_->Info("ddl", "created assertion '" + stmt.name + "' on '" + stmt.table_name +
                              "' (built, enforcing)");
    }
    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleShowAssertions() {
    auto rows = exec::ListAssertions(catalog_, page_store_);
    if (!rows.ok()) {
        return {ErrorReply(rows.status()), false, 0, rows.status()};
    }

    std::ostringstream os;
    os << "assertions=" << rows.value().size();

    for (const exec::AssertionDef& def : rows.value()) {
        os << "\\n";
        os << "assertion_id=" << def.id << " name=" << def.name;

        // The relation's name, resolved here rather than stored - exactly as
        // SHOW CABINS and SHOW INDEXES do. The row holds an oid so it stays
        // narrow, and an inspection surface can afford the lookup.
        os << " rel=";
        bool named = false;
        // Unfiltered by design (DT3c): a diagnostic surface answers "what
        // does this instance hold" - ddl-transactional.md §5.
        if (auto tables = catalog_.ListTables(); tables.ok()) {
            for (const catalog::SysObjectRow& obj : tables.value()) {
                if (obj.oid != def.target_oid) continue;
                os << catalog::NameView(obj.name);
                named = true;
                break;
            }
        }
        if (!named) os << "oid=" << def.target_oid;

        // Three conditions, each honest on its own: the structure was built
        // (a root), the write path checks (AST07's constant), and the
        // instance's registry holds the directory - which a restart empties
        // until recovery replays it, so a surviving catalog row reports 0
        // rather than claiming a check that cannot run. The same answer from
        // every core since AT-S5d; `enforced_by_core=` named whose answer it
        // was while each core had a registry of its own.
        os << " enforcing="
           << ((def.cabin_root != kInvalidPageId && kWritePathEnforcesAssertions &&
                enforcer_->Holds(def.id))
                   ? 1
                   : 0);
        // §9's production counters, printed only while the registry holds
        // the assertion: they live and die with the directory, so an
        // unenforced row prints no numbers rather than zeros that would
        // read as "counted, and nothing happened".
        if (const auto counters = enforcer_->CountersOf(def.id); counters.has_value()) {
            os << " checks=" << counters->checks << " violations=" << counters->violations
               << " reserved=" << counters->reserved << " aborted=" << counters->aborted;
        }
        os << " def=" << def.source_text;
    }
    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleShowRelayout(std::string_view rest) {
    // The physical optimizer's shadow report (docs/spec/physical-optimizer.md
    // §5, workplan PX06). Read-only by construction: the bare form's planner
    // takes no PageStore, and the per-relation form's walk is a census
    // priced through the statement budget.
    if (relayout_mode_ == PhysicalOptimizerMode::kOff) {
        return {"RELAYOUT off (physical_optimizer=off)", false};
    }

    auto [name, extra] = SplitFirstToken(Trim(rest));
    if (!Trim(extra).empty()) {
        return {"ERR SHOW RELAYOUT takes at most one relation name", false};
    }

    std::vector<stats::RelationReport> reports;
    if (name.empty()) {
        auto all = stats::PlanAllRelations(catalog_, clock_, decay_half_life_ns_);
        if (!all.ok()) return {ErrorReply(all.status()), false, 0, all.status()};
        reports = std::move(all.value());
    } else {
        const auto [qualifier, bare] = SplitQualifiedArg(name);
        auto oid = catalog_.FindTableOidByName(bare);
        if (!oid.ok()) {
            return {"ERR unknown relation '" + std::string(name) + "'", false};
        }
        if (Status s = catalog_.CheckRelationQualifier(qualifier, bare, oid.value()); !s.ok()) {
            return {ErrorReply(s), false, 0, s};
        }
        // A fresh budget at the configured ceiling: the walk is priced like
        // any other relation read, and a spent budget refuses the survey
        // rather than serving a half-count.
        exec::Budget budget(budget_.limit());
        auto one = stats::PlanRelation(catalog_, page_store_, oid.value(), budget, clock_,
                                       decay_half_life_ns_);
        if (!one.ok()) return {ErrorReply(one.status()), false, 0, one.status()};
        reports.push_back(std::move(one.value()));
    }

    std::ostringstream os;
    os << "relayout_relations=" << reports.size();
    for (const stats::RelationReport& report : reports) {
        os << "\\n";
        os << "rel=" << report.name << " clustered="
           << (report.clustered_type == catalog::ClusteredType::kBtree ? "btree" : "heap")
           << " shapes=" << report.shapes.size()
           << " walk_weight_q8=" << stats::WalkWeightOf(report.shapes);

        for (const stats::ShapeWeight& shape : report.shapes) {
            os << "\\n";
            os << "shape kind=" << exec::AccessKindName(shape.kind) << " columns_mask=0x"
               << std::hex << shape.column_mask << std::dec << " uses=" << shape.use_count
               << " weight_q8=" << shape.decayed_weight;
        }

        if (report.survey.has_value()) {
            os << "\\n";
            os << "survey pages=" << report.survey->chain_pages
               << " live=" << report.survey->live_tuples
               << " delete_marked=" << report.survey->delete_marked
               << " tuples_per_page=" << report.survey->tuples_per_page;
            // **Absent when the survey covered the whole relation** (H3),
            // which is every relation since AT-S9 made the survey walk
            // every range - the absent-rather-than-zeroed rule, for its
            // reason: a field that reads
            // `1/1` forever teaches a reader to skip it, and then it is
            // not read on the one relation where it matters.
            if (report.survey->surveyed_ranges != report.survey->relation_ranges) {
                os << " surveyed_ranges=" << report.survey->surveyed_ranges << "/"
                   << report.survey->relation_ranges;
            }
        }

        if (report.plans.empty()) {
            // Out of jurisdiction, not "nothing to do": a btree relation has
            // no v1 mover candidate (R5), and a catalog relation may never
            // have one (§4's must-not list). Each names its reason so an
            // empty candidate set cannot be read as a clean bill of health.
            os << "\\n";
            os << (report.system_relation
                       ? "plans=none reason=catalog-relation-outside-mover-jurisdiction"
                       : "plans=none reason=btree-outside-v1-mover-scope");
            continue;
        }
        for (const stats::RelayoutPlan& plan : report.plans) {
            os << "\\n";
            os << "plan=" << stats::RelayoutPlanKindName(plan.kind)
               << " blocked_on=" << stats::RelayoutGateName(plan.blocked_on)
               << " surveyed=" << (plan.survey_backed ? 1 : 0)
               << " predicted_pages_saved=" << plan.predicted_pages_saved
               << " predicted_benefit=" << plan.predicted_benefit
               << " measured_pages_saved=" << plan.measured_pages_saved;
        }
    }
    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleShowCabins() {
    auto rows = catalog_.ListCabins();
    if (!rows.ok()) {
        return {ErrorReply(rows.status()), false, 0, rows.status()};
    }

    std::ostringstream os;
    os << "cabins=" << rows.value().size();
    // The store-wide half, which no per-cabin row can carry: a walk
    // declines to bank a set when its view could still be contradicted
    // (§6a), and that is a property of the transaction in flight, not of
    // the Cabin being probed. Together with `cap_refusals` these are what
    // separate "nobody probed this column by equality" from "every probe
    // that would have recorded was refused", which `observed=0 hits=0`
    // cannot. Printed as zeros rather than suppressed, for the reason the
    // per-row branch below gives: absent must mean *unknown*, and a
    // suppressed zero reads as absent.

    if (cabins_ != nullptr) {
        os << " unbankable_views=" << cabins_->stats().unbankable_views
           << " cap_refusals=" << cabins_->stats().cap_refusals;
    }

    for (const catalog::SysCabinRow& row : rows.value()) {
        os << "\\n";
        os << "cabin_id=" << row.cabin_id;

        // Names resolved here rather than stored, exactly as SHOW ACCESS
        // does: the row holds oids so it stays fixed width, and an
        // inspection surface can afford the lookup.
        auto access = catalog_.InitTableAccess(row.rel_oid);
        os << " rel=";
        bool named = false;
        // Unfiltered by design (DT3c): a diagnostic surface answers "what
        // does this instance hold" - ddl-transactional.md §5.
        if (auto tables = catalog_.ListTables(); tables.ok()) {
            for (const catalog::SysObjectRow& obj : tables.value()) {
                if (obj.oid != row.rel_oid) continue;
                os << catalog::NameView(obj.name);
                named = true;
                break;
            }
        }
        if (!named) os << "oid=" << row.rel_oid;

        os << " column=";
        if (access.ok() && row.column_no < access.value()->schema.columns.size()) {
            os << catalog::NameView(access.value()->schema.columns[row.column_no].name);
        } else {
            os << row.column_no;
        }

        os << " origin=" << (row.origin == catalog::kCabinOriginUser ? "user" : "auto");
        os << " status="
           << (row.status == catalog::kCabinStatusActive
                   ? "active"
                   : (row.status == catalog::kCabinStatusBuilding ? "building" : "demoted"));

        // The runtime half, from the core-local store. These are the
        // numbers that say whether a Cabin is earning its write hook -
        // which is the question §8's demotion policy will need to answer,
        // and the one the catalog cannot: it stores that a Cabin exists,
        // never what it has observed.
        //
        // `observed=0 hits=0` on an old Cabin means the column is declared
        // and never probed by equality. `observed>0 hits=0` means the
        // values being probed are not the ones being observed. The third
        // reading, `scope_declines` (SB-R4), went at AT-S10 with the one
        // walk that could produce it, a remote stage's slice.
        //
        // **And it is the instance's figure since AT-S7**: one store, so a
        // Cabin's counters are the same numbers whichever core is asked,
        // where before AK-S2's per-core store made `InfoFor` on an id this
        // core never met answer zeros - "never probed" for a Cabin serving
        // thousands elsewhere.
        //
        // **The owner test that stood here is gone with the second store**
        // (AT-S7). It printed `(held by core N)` and five dashes for any
        // relation this core did not own, because a peer's store knew
        // nothing of that Cabin; one store knows all of them, so the
        // dashes said "unknown" about a number this core can read.
        if (cabins_ != nullptr) {
            const stats::CabinStore::CabinInfo info = cabins_->InfoFor(row.cabin_id);
            os << " observed=" << info.values << " entries=" << info.entries
               << " hits=" << info.hits << " misses=" << info.misses;
        } else {
            // Not "0": the store is off, so every count is unknown rather
            // than zero, and printing zeros would read as "nothing has
            // happened" when the truth is "nothing is being recorded".
            os << " observed=- entries=- hits=- misses=- (cabins = off)";
        }
    }
    return {os.str(), false};
}

// ---- H6 step 3: the three inspection commands ---------------------------
//
// The one-line `\n`-escaped convention `SHOW PAGE` established
// (`client-manual.md`), so a text client reads them the same way.

DispatchOutcome CommandDispatcher::HandleTrace(std::string_view rest) {
    auto [word, tail] = SplitFirstToken(rest);
    if (!tail.empty()) {
        return {"ERR TRACE takes ON or OFF and nothing else", false};
    }
    if (traces_ == nullptr) {
        // **Refused rather than silently accepted**: a session told `TRACE
        // ON` that then finds an empty ring would read the absence as "the
        // statement was too fast to show anything", which is the
        // misdiagnosis this whole instrument exists to prevent.
        return {"ERR tracing is not available on this dispatcher; no trace sink is installed",
                false};
    }
    if (IEquals(word, "ON")) {
        tracing_ = true;
        return {"OK tracing on", false};
    }
    if (IEquals(word, "OFF")) {
        tracing_ = false;
        return {"OK tracing off", false};
    }
    return {"ERR TRACE takes ON or OFF", false};
}

DispatchOutcome CommandDispatcher::HandleShowTraces() {
    if (traces_ == nullptr) return {"ERR no trace sink is installed", false};
    return {stats::RenderTraceList(*traces_), false};
}

DispatchOutcome CommandDispatcher::HandleShowTrace(std::string_view rest) {
    if (traces_ == nullptr) return {"ERR no trace sink is installed", false};
    auto [id_text, tail] = SplitFirstToken(rest);
    if (id_text.empty() || !tail.empty()) {
        return {"ERR SHOW TRACE takes one trace id (SHOW TRACES lists them)", false};
    }
    std::uint64_t id = 0;
    for (char c : id_text) {
        if (c < '0' || c > '9') return {"ERR trace id must be a number", false};
        id = id * 10 + static_cast<std::uint64_t>(c - '0');
    }
    const stats::TraceContext* trace = traces_->Find(id);
    if (trace == nullptr) {
        // The ring is drop-oldest, so "gone" and "never existed" are one
        // answer here and the message says which is likelier.
        return {"ERR no trace " + std::to_string(id) +
                    " in the ring; it may have been evicted (SHOW TRACES lists what is held)",
                false};
    }
    return {stats::RenderTrace(*trace), false};
}

DispatchOutcome CommandDispatcher::HandleShowCabinOptimizer() {
    // PO9's view (workplan PHY06). Rendering only: every number is read
    // from a surface that exists on its own merits - the controller's
    // managed table and decision log, the executor's applied counters, the
    // collector's S3 quality - so the view cannot disagree with the engine
    // about anything, only omit.
    //
    // Under the view latch (AT-S8): the controller and executor are the
    // instance's, and core 0's cadence mutates both across a tick held
    // under it; a read from any core waits the tick out rather than
    // walking a table mid-rebuild. Outer to the collector's own latch,
    // which `QualityOf` below takes - the tick's order.
    const LatchGuard view(cabin_optimizer_view_latch_);
    if (cabin_controller_ == nullptr) {
        // Not an empty table: with no controller nothing is managing, and
        // an empty listing would read as "managing nothing yet" when the
        // truth is "not constructed" - SHOW CABINS' `cabins = off` rule.
        // The instance has one controller and every core is handed it
        // (AT-S8), so absent is the instance's answer, not this core's.
        return {"CABIN_OPTIMIZER absent (no cabin optimizer on this instance)", false};
    }

    const stats::CabinOptimizerConfig& config = cabin_controller_->config();
    std::ostringstream os;
    os << "cabin_optimizer=" << (cabin_optimizer_enabled() ? "on" : "off")
       << " managed=" << cabin_controller_->managed_count()
       << " pages_committed=" << cabin_controller_->pages_committed()
       << " page_budget=" << config.page_budget;
    if (cabin_executor_ != nullptr) {
        // Applied, not decided: the per-entry `last_action` below is what
        // Decide wanted, these are what Execute did, and the gap between
        // the two is the diagnostic (a deferral, a policy refusal).
        const exec::CabinOptimizerExecutor::Counters& c = cabin_executor_->counters();
        os << " ticks=" << c.ticks << " creates=" << c.creates << " extends=" << c.extends
           << " heals=" << c.heals << " drops=" << c.drops
           << " deferred=" << c.builds_deferred << " failures=" << c.build_failures;
    }

    const std::vector<stats::DecisionRecord> log = cabin_controller_->DecisionLog();
    for (const stats::ManagedEntryView& entry : cabin_controller_->ManagedEntries()) {
        os << "\\n";
        os << "rel=" << RelationNameOf(entry.rel_oid);

        // Names resolved here rather than stored, SHOW CABINS' rule: an
        // inspection surface can afford the lookup.
        os << " column=";
        auto access = catalog_.InitTableAccess(entry.rel_oid);
        if (access.ok() && entry.col_pos < access.value()->schema.columns.size()) {
            os << catalog::NameView(access.value()->schema.columns[entry.col_pos].name);
        } else {
            os << entry.col_pos;
        }

        os << " state=" << entry.state << " cabin_id=" << entry.cabin_id
           << " pages=" << entry.pages << " streak=" << entry.confirm_streak
           << " benefit_q16=" << entry.benefit << " cost_q16=" << entry.cost;

        // S3 quality as integer percentages - the Q8 scale cancels in the
        // ratio. Failure and miss rates rather than their complements,
        // because they are what the θ_heal and θ_extend rules compare.
        if (entry.cabin_id != 0 && optimizer_signals_ != nullptr) {
            const stats::SnapshotCabin q = optimizer_signals_->QualityOf(entry.cabin_id);
            const std::uint64_t lookups = q.lookups_q8;
            os << " hint_fail_pct="
               << (lookups == 0 ? 0 : (std::uint64_t{q.hint_failures_q8} * 100) / lookups)
               << " coverage_miss_pct="
               << (lookups == 0 ? 0 : (std::uint64_t{q.coverage_misses_q8} * 100) / lookups);
        }

        // The newest logged decision for this candidate. The log is
        // oldest-first, so the final match wins; a candidate no decision
        // ever fired on says so rather than printing nothing.
        const stats::DecisionRecord* last = nullptr;
        for (const stats::DecisionRecord& record : log) {
            if (record.item.rel_oid == entry.rel_oid && record.item.col_pos == entry.col_pos) {
                last = &record;
            }
        }
        if (last != nullptr) {
            os << " last_action=" << stats::CabinActionName(last->item.action)
               << " reason=" << stats::ActionReasonName(last->item.reason)
               << " epoch=" << last->decay_epoch;
        } else {
            os << " last_action=none";
        }
    }
    return {os.str(), false};
}

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

Status CommandDispatcher::ResolveForeignKeyParents(const catalog::TableAccess& child,
                                                    const std::vector<parser::AstValue>& body,
                                                    const txn::ReadView& check_view,
                                                    exec::FkParentVerdicts& into,
                                                    txn::Transaction* waiter) {
    for (const catalog::ForeignKeyRef& fk : child.fkeys_out) {
        if (fk.column_no == 0 || fk.column_no > body.size()) continue;
        const parser::AstValue& value = body[fk.column_no - 1];
        // The same bail `CheckForeignKeyOnWrite` takes, and it must be the
        // same one: a value that is not an id cannot reference one, and
        // resolving it would put a nonsense pk in the held set.
        if (value.type != parser::ValueType::kInt || value.int_val < 0) continue;

        // ---- The one thing this pass does not hoist -------------------
        //
        // **A self-referencing foreign key stays a per-row check.** Hoisting
        // it would change answers: in `INSERT INTO t VALUES (1, NULL),
        // (2, 1)` on a `t` that references itself, row 2's parent is row 1,
        // which is written *by this statement* - so a verdict taken before
        // any row work says "no such parent" where today's per-row check,
        // running after row 1 landed, says pass.
        //
        // Carving it out costs nothing AH wants. It was argued when a
        // foreign parent meant another `owner_core`, which a
        // self-referencing key could never have, so the descent it left
        // inside the write scope never crossed (AH-R1). Since AT-S5f no
        // parent is foreign and nothing crosses, so the carve-out rests on
        // the ordering above alone.
        if (fk.rel_oid == child.oid) continue;

        const auto pk = static_cast<std::uint64_t>(value.int_val);
        if (into.Find(fk.rel_oid, pk) != nullptr) continue;  // AH-R2's dedup

        auto parent = catalog_.InitTableAccess(fk.rel_oid);
        if (!parent.ok()) return parent.status();

        // ---- No owner question, since AT-S5f -------------------------
        //
        // What stood here deferred a parent whose `owner_core` was not this
        // core into that owner's group, to be asked over a
        // `kFkProbeRequest` - on the argument that `CheckParentPresent`
        // descends `parent.desc_page_id` with no ownership question in it,
        // "on a parent this core does not own that is a page it may not
        // fault".
        //
        // It is a page every core faults since AM-S2 step 3, and the
        // descent was already ownership-free, so the deferral was the only
        // thing making the parent foreign. The check runs here for every
        // parent, under the same view and through the same descent, which
        // is the whole of D5's shape (AT-R15).

        std::uint64_t busy_trx = 0;
        auto verdict = exec::CheckParentPresent(page_store_, *parent.value(), pk, check_view,
                                                &budget_, &busy_trx);
        if (!verdict.ok()) return verdict.status();

        // **F3's forward busy is a wait, on a parent held anywhere**
        // (AO-3 B row 3; AO-S3 built the same-core half and AO-S5(b) the
        // shipped probe's, and AT-S5f makes the two one). The parent row is
        // held by a transaction that has not decided, and the answer
        // genuinely depends on how it ends - commit makes the child legal,
        // abort makes it a violation. Refusing retryably told the client to
        // come back and ask again, which is a wait written in the client's
        // loop instead of here.
        //
        // This runs at the dispatch fork, before any row work, so the
        // statement has written nothing and re-running it is exactly what
        // the client would do. **D9(a)'s `S` fence is not this** - that
        // keeps the parent alive for the child's whole transaction and is
        // the following letter's; this only changes *when* the check runs,
        // never what protects the row afterwards (AO-R14).
        if (verdict.value() == exec::FkVerdict::kBusy && busy_trx != 0) {
            WaitForParentRowWriter(waiter, fk.rel_oid, pk, busy_trx);
        }

        // Recorded **per resolution, not per row**, which is the mechanism
        // made visible in `SHOW ACCESS`: a thousand-row insert against one
        // parent now reports one lookup where it reported a thousand. The
        // number moved because the work did.
        RecordFkAccess(exec::AccessKind::kLookup, fk.rel_oid, 1);
        into.Put(fk.rel_oid, pk, verdict.value());
    }
    return Status::OK();
}

void CommandDispatcher::WaitForParentRowWriter(txn::Transaction* waiter,
                                               catalog::Oid parent_rel, std::uint64_t pk,
                                               std::uint64_t holder) {
    // **The wait is on the table's slot, because the holder may be on any
    // core** (AO-S6e-b's finding, reached here by AT-S5f). Every other
    // row-level wait in this file is recorded through `NoteBlockingWriter`,
    // whose predicate is `TransactionManager::IsInFlight` - *this core's*
    // live set, which answers "not in flight" for a transaction very much
    // in flight on a peer. While a foreign parent was deferred to its
    // owner, that was sound: the park happened on the owner's core, where
    // the holder was local (AO-S5(b)). With the descent run here it is not,
    // and a wait built on it would be a spin that ends at the fault net.
    //
    // So the parent row's own borrow is asked for instead. Its writer holds
    // the tuple `X` (AO-S6a), the table is the instance's (AO-S5(a)), and
    // the slot is flipped by the release from whichever core releases.
    //
    // **Nothing is acquired by the arm that matters.** A refused
    // `TryAcquire` leaves the table as it found it - no queue position,
    // nothing in `holdings`, only the wake registration - so this check
    // takes no fence over the parent and holds nothing after it. That
    // distinction is the whole of why D9(a)'s `S` fence is a separate
    // decision and not a side effect of this line, and it is also why the
    // window before the child's write stays open across cores
    // (`foreign-keys.md` §3a, `known-gaps.md`).
    //
    // **And no `IS` above it**, which `lock_table.hpp` calls a wrong
    // answer given quietly: a unit held under a relation entry with no
    // intention on it is invisible to a relation-level ask. It is sound
    // here for a reason that is about this ask and not about the rule -
    // the only arm that lasts is the *refused* one, which holds nothing
    // for a relation ask to miss, and a grant is released before this
    // function returns with no page touched in between. A borrow that
    // outlived the ask would need the intention and would be D9(a).
    if (locks_ == nullptr || waiter == nullptr) {
        // No table to ask - a dispatcher built without one, which is the
        // configuration `CheckWriteConflictBlocking` keeps its own union
        // for - or no transaction to ask under. A peer's holder needs the
        // table to be reached at all, so where there is none the holder is
        // this core's and the per-core predicate is the right one: AO-S3's
        // wait, unchanged.
        NoteBlockingWriter(waiter, holder, pk, RepeatableReadWait::kCapable);
        return;
    }
    // `NoteBlockingWriter`'s own first two guards, on the path that does
    // not go through it: no park without the allowance, and no second
    // wait beside one the outcome already carries.
    if (!may_park_ || lock_wait_.has_value()) return;

    const txn::LockKey unit = txn::LockKey::Tuple(parent_rel, pk);
    std::uint64_t blocker = 0;
    std::shared_ptr<txn::LockWaitSlot> wake;
    auto took = locks_->TryAcquire(waiter->id(), unit, txn::LockMode::kShared, waiter->borrows(),
                                   &blocker, &wake);
    if (!took.ok()) {
        // The cap, which is not a conflict and names nobody. The statement
        // is refused by the busy verdict the caller already holds; a
        // borrow-cap refusal raised here would replace an answer about the
        // client's own data with one about a ledger it cannot act on.
        return;
    }
    if (took.value()) {
        // **Granted, which means the holder decided between the header read
        // and this ask.** The `S` is released at once and nothing waits: a
        // decided holder is exactly the state `NoteBlockingWriter` answers
        // by recording nothing, so this arm behaves as the same-core wait
        // always has, and the busy verdict refuses the statement retryably.
        locks_->ReleaseOne(waiter->id(), unit, txn::LockMode::kShared, waiter->borrows());
        return;
    }
    if (wake == nullptr) return;  // no reactor to park on: the plain refusal stands
    lock_wait_ = DispatchOutcome::LockWait{unit, blocker != 0 ? blocker : holder, std::move(wake)};
}

Status CommandDispatcher::CheckForeignKeyOnWrite(const catalog::TableAccess& child,
                                                 const catalog::ForeignKeyRef& fk,
                                                 const parser::AstValue& value,
                                                 const txn::ReadView& check_view,
                                                 const exec::FkParentVerdicts& held) {
    // A value that is not an id cannot reference one. Left alone rather than
    // failed here, and the same bail carries two different outcomes:
    //   - a NULL is MATCH SIMPLE's vacuous pass - the codec stores it if the
    //     column was declared NULL, and refuses it by name if not, so the
    //     NOT NULL refusal is the gate and no kFkNullable read is needed;
    //   - a wrong-typed value is refused by the codec a moment later with a
    //     message about the column's declared type, which is the better
    //     error - a type mistake reported as a constraint violation sends
    //     the reader looking at the wrong table.
    if (value.type != parser::ValueType::kInt || value.int_val < 0) return Status::OK();

    const auto parent_pk = static_cast<std::uint64_t>(value.int_val);

    // The held verdict, resolved by `ResolveForeignKeyParents` before any
    // row work (§2a). No descent happens here and none may be added: this
    // call site is inside an open `WriteScope`, which is the place AH-R1
    // exists to keep free of anything that could need to wait.
    exec::FkVerdict resolved{};
    if (const exec::FkVerdict* found = held.Find(fk.rel_oid, parent_pk); found != nullptr) {
        resolved = *found;
    } else if (fk.rel_oid == child.oid) {
        // The self-referencing arm, the one case the extraction pass
        // deliberately skips (see there). It can never be foreign, so the
        // descent is local by construction.
        auto parent = catalog_.InitTableAccess(fk.rel_oid);
        if (!parent.ok()) return parent.status();
        auto verdict =
            exec::CheckParentPresent(page_store_, *parent.value(), parent_pk, check_view, &budget_);
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
            std::to_string(parent_pk) + " of '" + RelationNameOf(fk.rel_oid) +
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
            // **Two paths reach here since AT-S5f**, so the sentence names
            // the holder's state and not a mechanism: the extraction pass's
            // descent, whose wait ran to the lock family's fault net
            // (`WaitForParentRowWriter`), and the self-referencing arm
            // above, which never waits at all. The three the probe protocol
            // made - a foreign parent's park on its owner's core, a parent
            // the owner had registered for deletion - went with it.
            // Retryable in both: the holder decides and the retry gets a
            // real answer.
            return Status::TxnConflict("row id=" + std::to_string(value.int_val) + " of '" +
                                       RelationNameOf(fk.rel_oid) +
                                       "' is being written by another transaction that has "
                                       "not decided, so the foreign key on '" +
                                       column + "' cannot be checked yet");
        case exec::FkVerdict::kViolation:
            break;
    }
    return Status::FkViolation("'" + column + "' references row id=" +
                               std::to_string(value.int_val) + " of '" +
                               RelationNameOf(fk.rel_oid) + "', which does not exist");
}

Status CommandDispatcher::CheckNoChildrenBeforeDelete(const catalog::TableAccess& parent,
                                                      std::uint64_t parent_pk,
                                                      const txn::ReadView& check_view) {
    // ---- Nothing is asked of another core, since AT-S5f -----------------
    //
    // Two things stood here and both were the same absence. A **reference
    // intent** granted by a forward probe was checked first, because "a
    // transaction on another core probed this parent, was told it exists,
    // and is now writing a child row that depends on it - a row this core
    // cannot see". And a child relation whose `owner_core` was not this
    // core was answered from a **fan-out** resolved at the dispatch fork,
    // because the local check would have refused it outright.
    //
    // A child row is a row every core can read now, so the walk below sees
    // it: an uncommitted one answers `kBusy` and a committed one
    // `kViolation`, which is the same evidence the intent stood in for and
    // is authoritative where the intent was advisory. With the walk
    // complete the fan-out has nothing left to ask, and both tables retire
    // with the protocol that carried them (AT-R15, D5).

    // RESTRICT (F2): the first child that still references this row refuses
    // the delete. No action of any kind is taken on the child - v1 never
    // writes to the other relation, which is what CASCADE would start.
    for (const catalog::ForeignKeyRef& fk : parent.fkeys_in) {
        auto child = catalog_.InitTableAccess(fk.rel_oid);
        if (!child.ok()) return child.status();

        exec::FkReverseOptions options;
        const catalog::TableAccess::CabinRef cabin = child.value()->CabinOn(fk.column_no);
        if (cabins_ != nullptr && cabin.id != 0) {
            options.cabins = cabins_;
            options.cabin_id = cabin.id;
        }

        auto outcome = exec::CheckNoChildReferences(page_store_, *child.value(), fk.column_no,
                                                    parent_pk, check_view, options, &budget_);
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
            case exec::FkVerdict::kBusy:
                return Status::TxnConflict("a row of '" + RelationNameOf(fk.rel_oid) +
                                           "' referencing id=" + std::to_string(parent_pk) +
                                           " is being written by another transaction");
            case exec::FkVerdict::kViolation:
                return Status::FkViolation("row id=" + std::to_string(parent_pk) +
                                           " is still referenced by '" +
                                           RelationNameOf(fk.rel_oid) + "." + column + "'");
        }
    }
    return Status::OK();
}

StatusOr<catalog::Oid> CommandDispatcher::ResolveCreateNamespace(std::string_view qualifier,
                                                                std::uint32_t byte_offset,
                                                                const txn::ReadView* view) {
    if (qualifier.empty()) return catalog::kNamespacePublic;
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

DispatchOutcome CommandDispatcher::HandleShowFkeys() {
    auto rows = catalog_.ListForeignKeys();
    if (!rows.ok()) {
        return {ErrorReply(rows.status()), false, 0, rows.status()};
    }

    std::ostringstream os;
    os << "fkeys=" << rows.value().size();

    for (const catalog::SysFkeyRow& row : rows.value()) {
        os << "\\n";
        os << "fk_id=" << row.fk_id;
        os << " child=" << RelationNameOf(row.child_rel_oid);

        os << " column=";
        auto child = catalog_.InitTableAccess(row.child_rel_oid);
        if (child.ok() && row.child_column_no < child.value()->schema.columns.size()) {
            os << catalog::NameView(child.value()->schema.columns[row.child_column_no].name);
        } else {
            os << row.child_column_no;
        }

        // The parent column is not printed because there is not one: the
        // reference is to the parent's Keystone id, always (F1). Printing
        // the pk's name would suggest a choice was made.
        os << " parent=" << RelationNameOf(row.parent_rel_oid);
        os << " action=RESTRICT";
        os << " nullable=" << ((row.flags & catalog::kFkNullable) != 0 ? "yes" : "no");
    }
    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleCreateTableSql(std::string_view line,
                                                        Session& session) {
    return InDdlStatement(session, [&](WriteScope& scope) -> DispatchOutcome {
        // H6 step 2: the parse leg. One of `observability.md` §10's three
    // request-level spans, and the cheapest to attribute wrongly - a
    // statement that is slow to *parse* looks identical from outside to
    // one that is slow to run.
    auto parsed = [&] {
        stats::SpanScope span(trace_, stats::Layer::kParse);
        return parser::Parse(line);
    }();
        if (!parsed.ok()) {
            return {ErrorReply(parsed.status()), false, 0, parsed.status()};
        }
        if (!std::holds_alternative<parser::CreateTableStmt>(parsed.value())) {
            return {"ERR expected a CREATE TABLE statement", false};
        }
        auto& stmt = std::get<parser::CreateTableStmt>(parsed.value());

        // AF-T3: the one qualifier that decides rather than asserts. An
        // unqualified name is `public`, so every statement written before
        // AF means exactly what it meant; a named namespace must already
        // exist, because implicit creation is what makes a typo
        // indistinguishable from an intent (AF-6, shape (a) over (b)).
        //
        // **Resolved before the name-collision check below, not after it.**
        // The other order let `CREATE TABLE ordrs.t` answer `EXISTS oid=n`
        // whenever some relation `t` existed anywhere - the qualifier
        // absorbed rather than verified, and AF-6's "a typo must not be
        // indistinguishable from an intent" broken by the one arm that
        // never reached the resolver. An unqualified name still reads no
        // catalog here, so this costs the ordinary path nothing.
        const std::optional<txn::ReadView> ns_view = ViewFor(session);
        auto create_ns = ResolveCreateNamespace(stmt.schema, stmt.table_byte_offset,
                                                ns_view.has_value() ? &*ns_view : nullptr);
        if (!create_ns.ok()) {
            return {ErrorReply(create_ns.status()), false, 0, create_ns.status()};
        }

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
        auto existing = catalog_.FindTableOidByName(stmt.table_name);
        if (existing.ok()) {
        // **`EXISTS` is truthful only about the name, so a qualifier that
        // disagrees gets a sentence rather than that token** (the AF-T3
        // review's finding 5). Relation names are instance-global, so
        // `ledger.plain` cannot be created while a `plain` sits in `public`
        // - but replying `EXISTS` to it asserts that `ledger.plain` exists,
        // which is the one thing that is false. The qualifier check answers
        // with the namespace the name is actually held in.
            if (Status s = catalog_.CheckRelationQualifier(stmt.schema, stmt.table_name,
                                                           existing.value(),
                                                           stmt.table_byte_offset);
                !s.ok()) {
                return {ErrorReply(s), false, 0, s};
            }
            return {"EXISTS oid=" + std::to_string(existing.value()), false};
        }
        if (existing.status().code() != StatusCode::kNotFound) {
            return {ErrorReply(existing.status()), false, 0, existing.status()};
        }
        if (auto refused = RefuseIfNameHeldByPendingDrop(stmt.table_name, session);
            refused.has_value()) {
            return *refused;
        }

        // Resolve each column's parsed type_name against sys.types - the
        // stand-in type registry (Catalog::ResolveTypeByName(), see its
        // comment) - before touching storage, so a bad type name fails clean
        // with nothing created.
        catalog::Schema schema;
        std::uint32_t pos = 0;
        for (const auto& col : stmt.columns) {
            auto type_row = catalog_.ResolveTypeByName(col.type_name);
            if (!type_row.ok()) {
                return {ErrorReply(type_row.status()), false, 0, type_row.status()};
            }

            // A cabin policy on the primary key is refused rather than ignored.
            // The pk's cabin is the clustered tree (spec §2), so any of the
            // three answers would be a statement about something that cannot
            // exist - and silently dropping the clause would leave an operator
            // believing they had said something.
            if (pos == 0 && col.cabin_policy != catalog::kCabinPolicyUnset) {
                return {"ERR the primary-key column '" + col.name +
                            "' takes no cabin policy - the clustered tree is its cabin (byte " +
                            std::to_string(col.cabin_byte_offset) + ")",
                        false};
            }
            // Invariant 11 at the surface, with the byte the layout's own
            // defense cannot know: the first column is the pk, carried by
            // the Keystone word, which has no NULL encoding.
            if (pos == 0 && !col.notnull) {
                return {"ERR the primary-key column '" + col.name +
                            "' cannot be declared NULL - the pk is carried by the Keystone "
                            "word, which has no NULL encoding (byte " +
                            std::to_string(col.null_byte_offset) + ")",
                        false};
            }

            catalog::SysColumnRow row{};
            row.pos = pos++;
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
            // third argument-taking type must find one place to extend, not
            // two (the phase-A review's shape note).
            const bool takes_precision = row.type_val == catalog::kTypeValDecimal ||
                                          row.type_val == catalog::kTypeValDecimalWide;
            const bool takes_width = row.type_val == catalog::kTypeValChar ||
                                      row.type_val == catalog::kTypeValVarchar;
            if ((col.has_precision && !takes_precision) || (col.has_width && !takes_width)) {
                return {"ERR type '" + col.type_name + "' takes no arguments (byte " +
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
            // **`decimal128(p, s)` is not a spelling this code ever sees**,
            // and the sentence that said it "names the wide type directly
            // and its bounds refuse p <= 18" was wrong (corrected
            // 2026-08-29, H8). The parser admits a type-argument list for
            // `DECIMAL`, `CHAR` and `VARCHAR` only, so `decimal128(24, 6)`
            // is refused as *"type 'decimal128' takes no arguments"* before
            // a column row is built. Nothing is lost by that: the wide type
            // is reached by declaring `decimal(p, s)` with p >= 19, which
            // is the promotion above, so one declaration still selects
            // exactly one type - by precision, and only by precision.
            if (row.type_val == catalog::kTypeValDecimal ||
                row.type_val == catalog::kTypeValDecimalWide) {
                if (!col.has_precision) {
                    // Unreachable through the parser, which refuses a bare
                    // `decimal`. Checked anyway: a schema can be built without
                    // one, and a decimal with no scale stored is a column whose
                    // values have no defined meaning.
                    return {"ERR column '" + col.name + "' is decimal with no precision or scale",
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
                    return {ErrorReply(bounds) + " (byte " +
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
                        return {"ERR column '" + col.name +
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
                        return {ErrorReply(s.WithContext("column '" + col.name + "'")) + " (byte " +
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
            const std::optional<txn::ReadView> parent_view = ViewFor(session);
            auto parent_oid = catalog_.FindTableOidByName(
                col.references_table, parent_view.has_value() ? &*parent_view : nullptr);
            if (!parent_oid.ok()) {
                return {"ERR column '" + col.name + "' references unknown relation '" +
                            col.references_table + "' (byte " +
                            std::to_string(col.references_byte_offset) + ")",
                        false};
            }
            // AF-T3: `REFERENCES orders.customer` asserts where the parent
            // lives, and a wrong assertion is refused - which matters more
            // here than anywhere else, because after AF-T2 the parent's
            // namespace is the parent's *core*.
            if (Status s = catalog_.CheckRelationQualifier(
                    col.references_schema, col.references_table, parent_oid.value(),
                    col.references_byte_offset,
                    parent_view.has_value() ? &*parent_view : nullptr);
                !s.ok()) {
                return {ErrorReply(s), false, 0, s};
            }
            auto parent = catalog_.InitTableAccess(parent_oid.value());
            if (!parent.ok()) {
                return {ErrorReply(parent.status()), false, 0, parent.status()};
            }

            // The shared declaration checks (catalog/foreign_key.hpp) - the same
            // ones Catalog::CreateForeignKey() applies at the door, so a
            // declaration cannot pass here and fail there.
            if (Status s = catalog::CheckForeignKeyDeclaration(
                    *parent.value(), schema.columns[i], static_cast<std::uint16_t>(i));
                !s.ok()) {
                return {ErrorReply(s) + " (byte " +
                            std::to_string(col.references_byte_offset) + ")",
                        false};
            }
            pending_fkeys.push_back(PendingForeignKey{static_cast<std::uint16_t>(i),
                                                      parent_oid.value(), col.references_table});
        }

        // ---- Storage (docs/spec/heap-and-tuple.md §4.1) --------------------------
        //
        // One trailing word decides anything now. The key mode was removed
        // 2026-08-25, and with it the whole resolution that used to live here:
        // an instance-wide `default_key_mode`, a storage default that moved
        // with it, and the EXPLICIT-implies-BTREE refusal that made the pair
        // consistent. A relation's storage is the writer's word or the heap,
        // and what a heap cannot do with a *supplied* id is refused per id by
        // `AdmitExplicitRowId` - so no `CREATE TABLE` shape is refused for a
        // key-mode reason at all.
        const catalog::ClusteredType clustered =
            stmt.clustered_given ? stmt.clustered : catalog::ClusteredType::kBtree;  // SUS-1: BTREE is the default

        DdlScope ddl = DdlScopeFor(scope);
        auto oid = catalog_.CreateTable(create_ns.value(), stmt.table_name, schema,
                                         clustered, ddl.trx_id, ddl.sink());
        // Registered before the status is read: a create that failed partway
        // still left rows on the page, and those are exactly the rows a
        // rollback has to retire.
        NoteDdlRows(ddl);
        if (!oid.ok()) {
            return {ErrorReply(oid.status()), false, 0, oid.status()};
        }

        // The constraints, now that there is a child relation to hang them on.
        // What can still fail here is catalog I/O (F5's colocation check
        // retired with owner cores at AT-S9). Since D2 the ERR
        // makes FinishDdlStatement abort the statement's transaction, so
        // the relation's own rows are taken back - the message must not
        // claim otherwise (review B2). What the abort does NOT take back
        // is any sys.fkeys row already written in this loop:
        // CreateForeignKey reports no CatalogRowRef, so those rows never
        // reach the trail - the orphan the review named, pre-existing on
        // the explicit-transaction path and recorded in
        // workplan-rv3-catalog-recovery.md's remainder.
        // **No notice about where the parent is** (AT-S5f, and AT-S9 made
        // it permanent). AF-T4 printed one here - a cross-owner foreign
        // key's cost, one probe round per write and a refused parent
        // `DELETE` - and both went at AT-S5f when the checks went local;
        // the owners themselves went at AT-S9. A notice invented to keep
        // the shape would tell a client a cost the engine does not charge.
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
        // `kCabinPolicyAuto` does nothing here, by design: no code creates a
        // Cabin on that policy, because the promotion pipeline that would judge
        // it does not exist (§7). The value is stored so the decision has a name
        // before the machinery that consumes it - not so that it quietly behaves
        // like `enabled`.
        //
        // A failure here does not fail the CREATE TABLE. The relation exists and
        // is correct; what is missing is an accelerator, and reporting it as a
        // warning beats leaving a half-created table behind - there is no
        // transaction to roll one back into.
        std::vector<std::string> warnings;
        for (const catalog::SysColumnRow& col : schema.columns) {
            if (catalog::EffectiveCabinPolicy(col.cabin_policy) != catalog::kCabinPolicyEnabled) {
                continue;
            }
            auto cabin = catalog_.CreateCabin(oid.value(), static_cast<std::uint16_t>(col.pos),
                                               catalog::kCabinOriginUser);
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
    if (wal_ == nullptr) return Status::OK();

    for (const exec::IndexWrite& write : writes) {
        // A split's pages take full page images and **no** INDEX_INSERT: the
        // images are taken after the entry is in, so emitting both would
        // apply it twice. Same instrument the clustered tree's internal
        // nodes take, and for the same reason - no record type describes an
        // entry-array division (wal/record.hpp).
        if (!write.restructured.empty()) {
            for (PageId page_id : write.restructured) {
                if (Status s = LogFullPageImage(page_id, txn_id); !s.ok()) return s;
            }
            continue;
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
        if (Status s = page_store_.StampPageLsn(write.page_id, rec.value()); !s.ok()) return s;
    }
    return Status::OK();
}

Status CommandDispatcher::LogFullPageImage(PageId page_id, std::uint64_t txn_id) {
    // The one writer (storage/log_page_image.hpp), kept as a member only
    // for its callers' brevity.
    return storage::LogFullPageImage(wal_, page_store_, txn_id, page_id);
}

// The durability a statement with no commit record owes its
// acknowledgement (workplan-rv3-catalog-recovery.md's remainder round):
// pattern, assertion, cabin and ALTER DDL run under no transaction, so
// nothing ever synced their records - an acknowledged CREATE ASSERTION
// could die with the WAL buffer, which is the durability promise broken.
//
// **D2 syncs here too, and that is not a policy choice.** `wal.md` §1 gives
// D2 the same durability point as D1 - "D1/D2 differ only in batching,
// never in the durability point", §14's "D1/D2 never lose an acked commit
// under any injected crash" - and D2 is the *default* class, so leaving it
// out would have left the very defect RV3 closed open for every default
// deployment. D2's batching lives in `pending_commit_lsn_`, which is keyed
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
                                     std::uint64_t pk,
                                     const std::vector<exec::AppendedSpill>& spills) {
    if (scope.txn == nullptr) return Status::OK();
    for (const exec::AppendedSpill& spill : spills) {
        if (Status s = txn_->NoteVarHeapAppend(*scope.txn, rel_oid, spill.ptr.page_id,
                                                spill.ptr.slot, pk);
            !s.ok()) {
            return s;
        }
    }
    return Status::OK();
}

Status CommandDispatcher::LogInsert(const storage::InsertPlacement& placed, PageType leaf_type,
                                    std::span<const std::byte> tuple, std::uint64_t trx_id,
                                    std::uint64_t owner_oid,
                                    const std::vector<exec::AppendedSpill>& spills,
                                    const std::vector<exec::IndexWrite>& index_writes,
                                    bool own_txn) {
    if (wal_ == nullptr) return Status::OK();

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

    // Every page the insert restructured, in the order the storage layer
    // says redo has to apply it, and all of it before the tuple's own
    // record. A brand-new tuple page is a PAGE_INIT that the HEAP_INSERT
    // below then fills; everything else - a link edit, a B+ tree internal
    // node created or amended, a new root - is a full page image, because
    // no record type describes those (insert_placement.hpp).
    for (const storage::StructuralChange& change : placed.changes()) {
        if (change.is_new_page) {
            if (auto rec = wal::LogPageInit(wal_, txn_id, change.page_id, leaf_type,
                                            change.min_key, owner_oid);
                !rec.ok()) {
                return rec.status();
            }
            // Deliberately unstamped: a new tuple page is always the page
            // the HEAP_INSERT below lands in, and that stamps it. A new
            // *internal* node is never reported as is_new_page, so it takes
            // the image arm and is stamped there.
            continue;
        }

        if (Status s = LogFullPageImage(change.page_id, txn_id); !s.ok()) return s;
    }

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

    // undo_ptr 0: an insert supersedes no version, so its undo chain ends
    // at itself (wal.md section 5.1).
    std::vector<std::byte> payload(wal::kHeapWriteFixedSize + tuple.size());
    const wal::HeapWritePayload fields{trx_id, /*undo_ptr=*/0, placed.slot,
                                       static_cast<std::uint16_t>(tuple.size())};
    if (auto n = wal::EncodeHeapWrite(payload, fields, tuple); !n.ok()) return n.status();

    auto rec = wal_->Append(
        wal::RecordSpec{wal::RecordType::kHeapInsert, txn_id, placed.page_id}, payload);
    if (!rec.ok()) return rec.status();
    if (Status s = page_store_.StampPageLsn(placed.page_id, rec.value()); !s.ok()) return s;

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

    DispatchOutcome out = InsertInner(line, scope);


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
    // **What is left of the write-affinity check since AT-S9**, and why it
    // is renamed: nothing here asks about a core. It resolved whose range a
    // write landed in and counted a write to another core's range
    // (`cross_core_write_refusal`, the evidence D18 kept of ownership); the
    // owners are gone with `owner_core` (D17), and with them the counter and
    // the range resolution it needed. An id outside every range is still
    // refused, by `TableAccess::HeapChainFor` where the row is placed. Its
    // history - PW1c-5's shape gate on peers, the btree, indexed, key-mode,
    // assertion, foreign-key and Cabin arms lifting one by one - is
    // `workplan-peer-writer.md` §4's and git's.
    //
    // **`CannotEnforce` stays**, with its *measured* failure
    // (`bench/v2.2.0/results-shipping-part-a-*` Finding 2, a shipped write
    // putting a second row in a group under `CHECK COUNT(*) <= 1`): an
    // assertion the instance knows of and could not revive at this mount
    // refuses the relation's writes on every core, there being one registry
    // since AT-S5d. What reaches it is a revive that failed or a checkpoint
    // whose snapshots do not cover the base (`server/mount_recovery.cpp`).
    if (enforcer_->CannotEnforce(access.oid)) {
        return Status::NotImplemented(
            "a relation under an assertion this core cannot enforce cannot take "
            "writes on core " +
            std::to_string(core_id_) +
            ": the assertion's Bound Cabin could not be revived at this mount, so "
            "admitting the write would leave the constraint unchecked; the mount log "
            "names why (docs/spec/assertion.md 6.1)");
    }
    return Status::OK();
}

namespace {

// **One renderer per output shape** (R4-A/AG3; the fan-in that shared it
// retired at AT-S9). A second formatter here would be exactly the drift
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
    // The precision is the type's maximum rather than the column's: a fold
    // widens, so the column's precision would understate the answer's
    // range. The scale is the half a client cannot read the value without.
    const auto decimal_mod = [&](bool as_wide) {
        return catalog::PackDecimalLen(
            as_wide ? exec::kMaxDecimalPrecisionWide : exec::kMaxDecimalPrecision, item.scale);
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
    // MIN/MAX, and a grouping column: the column's own type.
    return {item.type_val, catalog::TypeModOf(item.type_val, decimal_mod(wide))};
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
// is derived rather than passed - it is a function of the type and nothing
// else (`wire::WireTypeLen`), so a caller that supplied one could only ever
// agree with it or be wrong.
std::vector<wire::FieldDescription> DescribeFields(
    std::span<const std::string> names, std::span<const std::uint32_t> type_vals,
    std::span<const std::uint32_t> type_mods,
    std::span<const exec::ColumnRef> projection = {}) {
    std::vector<wire::FieldDescription> fields;
    fields.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        wire::FieldDescription f;
        f.name = names[i];
        f.type_oid = i < type_vals.size() ? type_vals[i] : 0;
        f.type_len = wire::WireTypeLen(f.type_oid);
        f.type_mod = i < type_mods.size() ? type_mods[i] : 0;
        // **`kFieldFlagKeystone` marks the relation's own column 0**, not
        // the first field of whatever was projected (`wire/row_codec.hpp`:
        // "field 0 of every user relation is the Keystone id"). So it is
        // set from the *reference*, which is the only thing that knows -
        // `SELECT v, id FROM t` flags its second field, and a fold flags
        // none, because a `COUNT(*)` is not anybody's primary key.
        //
        // A **star** read passes a projection built from the schema, whose
        // element 0 is exactly `{0, 0, 0}`, so the two routes to one
        // `SELECT *` describe it alike. A star read that passed no
        // projection would flag nothing, which is how the fan-in and the
        // local walk came to disagree about the same statement.
        if (i < projection.size() && projection[i].up == 0 && projection[i].rel_slot == 0 &&
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
        out.projection.push_back(exec::ColumnRef{0, 0, static_cast<std::uint16_t>(i)});
        out.types.push_back(columns[i].type_val);
        out.type_mods.push_back(catalog::TypeModOf(columns[i].type_val, columns[i].len));
    }
    return out;
}

}  // namespace

DispatchOutcome CommandDispatcher::InsertInner(std::string_view line, WriteScope& scope) {
    // H6 step 2: the parse leg. One of `observability.md` §10's three
    // request-level spans, and the cheapest to attribute wrongly - a
    // statement that is slow to *parse* looks identical from outside to
    // one that is slow to run.
    auto parsed = [&] {
        stats::SpanScope span(trace_, stats::Layer::kParse);
        return parser::Parse(line);
    }();
    if (!parsed.ok()) {
        return {ErrorReply(parsed.status()), false, 0, parsed.status()};
    }
    if (!std::holds_alternative<parser::InsertStmt>(parsed.value())) {
        return {"ERR expected an INSERT statement", false};
    }
    return InsertParsed(std::get<parser::InsertStmt>(parsed.value()), scope);
}

DispatchOutcome CommandDispatcher::ExecuteInsert(const parser::InsertStmt& stmt,
                                                 Session& session) {
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

DispatchOutcome CommandDispatcher::InsertParsed(const parser::InsertStmt& stmt,
                                                WriteScope& scope) {
    // BI3: the row cap, at the first config-aware layer (the parser is a
    // pure syntax layer and deliberately config-blind). A refusal naming
    // the cap and the count, never a truncation.
    if (stmt.rows.size() > max_insert_rows_) {
        return {"ERR INSERT of " + std::to_string(stmt.rows.size()) +
                    " rows exceeds max_insert_rows (" + std::to_string(max_insert_rows_) + ")",
                false};
    }

    // BI4's atomicity is the transaction scope's: rows placed before a
    // failure are unwound by rollback, and rollback replays the manager's
    // in-memory trail. A configuration without one (tests; production
    // always builds it) could place rows it cannot take back, which is a
    // wrong answer with a right answer's shape - refused upfront instead.
    if (stmt.rows.size() > 1 && txn_ == nullptr) {
        return {"ERR a multi-row INSERT requires the transaction manager; this configuration "
                "cannot unwind a partially placed statement and is single-row only",
                false};
    }

    // Resolved under the writing session's view (DT3c): a write to a
    // relation another transaction created and has not committed must not
    // find it.
    const std::optional<txn::ReadView> view =
        scope.session != nullptr ? ViewFor(*scope.session) : std::nullopt;
    auto oid =
        catalog_.FindTableOidByName(stmt.table_name, view.has_value() ? &*view : nullptr);
    if (!oid.ok()) {
        return {ErrorReply(oid.status()), false, 0, oid.status()};
    }
    if (Status s = catalog_.CheckRelationQualifier(stmt.schema, stmt.table_name, oid.value(),
                                                   stmt.table_byte_offset,
                                                   view.has_value() ? &*view : nullptr);
        !s.ok()) {
        return {ErrorReply(s), false, 0, s};
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

    // Whether every row omits its pk, the question T3's sorted fill further
    // down asks.
    const std::size_t fill_arity = ta->schema.columns.empty() ? 0 : ta->schema.columns.size() - 1;
    std::size_t rows_omitting_pk = 0;
    for (const std::vector<parser::AstValue>& r : stmt.rows) {
        if (r.size() == fill_arity) ++rows_omitting_pk;
    }
    const bool every_row_omits_pk = rows_omitting_pk == stmt.rows.size();

    // **No routing, since AT-S9.** R4/IS3's block stood here: it peeked the
    // row id this core would issue, asked which core owned the range that
    // id fell in, and pumped row-id lease demand so core 0 would open a
    // range for this core - insert spreading. Spreading retired with ownership (the
    // operator's ruling at AT-S9): no range is opened, and a row lands in
    // whichever existing range its id falls in (`TableAccess::HeapChainFor`,
    // which refuses an id in no range), on the core the session is on.

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
    // **Here and not at the first id borrow**, where an `INSERT` used to
    // take it. A DDL that claims the relation - a `CREATE INDEX` about to
    // backfill, a `CREATE ASSERTION` about to build a cabin - takes its `X`
    // against this `IX`, and it is a fence only if it stands over everything
    // the statement decides from the relation's secondary structures:
    // assertion admission and the sorted fill's `AnyOn` gate below, not
    // just the placement. Taken after them, a writer could be admitted
    // against no assertion, let a build finish and publish, and then place
    // a row the cabin never counts. Held for the transaction, so every row
    // of the statement is covered by this one ask.
    if (std::optional<Status> held = BorrowOrWait(scope, txn::LockKey::Relation(ta->oid),
                                                  RepeatableReadWait::kCapable)) {
        return {ErrorReply(*held), false, 0, *held};
    }

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

    // ---- T3: the sorted heap fill (docs/inflight/in-progress/workplan-t3.md) -----------------
    //
    // Page-at-a-time placement and one image per touched page, engaged
    // only inside T3-2's gate; everything else takes the row loop below.
    //
    // The per-statement half of that gate: the fill carves one contiguous id
    // range up front, so a row that names its own key has no place in it.
    // Checked over the rows rather than off the relation since 2026-08-25
    // (heap-and-tuple.md §4.1) - naming a key is a property of the row now.
    // Ineligibility, not a refusal: a statement that names keys still runs,
    // through the ordinary per-row path below. `every_row_omits_pk` is
    // counted at the top of this function, in the one pass R4's routing
    // also reads.
    if (bulk && every_row_omits_pk && SortedFillEligible(*ta, oid.value())) {
        return SortedFillInner(stmt, oid.value(), *ta, scope);
    }

    // ---- The extraction pass (foreign-keys.md §2a, AH-R1) ---------------
    //
    // Over **every** row, before the loop enters `InsertOneRow` for any of
    // them. That is the whole hoist on this path: a thousand-row insert
    // against one parent descends once rather than a thousand times. AH-T2's
    // probe round per foreign owner, which stood in for this resolution on
    // a peer, went with the probes at AT-S5f.
    exec::FkParentVerdicts fk_held;
    if (!ta->fkeys_out.empty()) {
        const txn::ReadView view = CheckView(scope);
        for (const auto& values : stmt.rows) {
            const std::vector<parser::AstValue> body = InsertBodyOf(*ta, values);
            if (Status s = ResolveForeignKeyParents(*ta, body, view, fk_held, scope.txn); !s.ok()) {
                return {ErrorReply(s), false, 0, s};
            }
        }
    }

    InsertRowResult first{};
    InsertRowResult last{};
    for (std::size_t k = 0; k < stmt.rows.size(); ++k) {
        InsertRowResult row{};
        if (auto err = InsertOneRow(oid.value(), ta, stmt.rows[k], scope, fk_held, row);
            err.has_value()) {
            if (bulk) *err += " (row " + std::to_string(k + 1) + ")";
            return {std::move(*err), false};
        }
        if (k == 0) first = row;
        last = row;
    }

    if (bulk) {
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

bool CommandDispatcher::SortedFillEligible(const catalog::TableAccess& ta,
                                           catalog::Oid oid) const {
    // The key-mode clause is gone with the mode (heap-and-tuple.md §4.1) and
    // is **replaced by a per-statement one at the call site**, not deleted:
    // this path's whole shape is one contiguous id range carved up front and
    // appended in order, which is wrong for any id the caller names. Whether
    // a caller names one is a fact about the rows now, so the caller checks
    // the rows; what is left here is the relation-shaped half.
    // PW1c-5 (revised at the 25059bf review's S-1): the sorted fill is
    // ineligible rather than refused where it cannot run, and the ordinary
    // per-row path serves the statement.
    return ta.clustered_type == catalog::ClusteredType::kHeap &&
           ta.varheap_page_id == kInvalidPageId && ta.indexes.empty() && ta.cabin_mask == 0 &&
           !enforcer_->AnyOn(oid);
}

DispatchOutcome CommandDispatcher::SortedFillInner(const parser::InsertStmt& stmt,
                                                   catalog::Oid oid,
                                                   const catalog::TableAccess& ta,
                                                   WriteScope& scope) {
    const std::size_t ncols = ta.schema.columns.size();

    // ---- The extraction pass (foreign-keys.md §2a, AH-R1) ---------------
    //
    // Every parent this statement names, resolved once, before the row loop
    // (AH-T2's park on a foreign owner stood here until AT-S5f). The row
    // loop below is unchanged in order and in what it reports: it answers
    // from what this resolved, so a refused statement still names the same
    // row ordinal it always did.
    //
    // Every row of a sorted fill omits its pk (the caller's gate), so the
    // body is the row.
    exec::FkParentVerdicts fk_held;
    if (!ta.fkeys_out.empty()) {
        const txn::ReadView view = CheckView(scope);
        for (const auto& values : stmt.rows) {
            if (Status s = ResolveForeignKeyParents(ta, values, view, fk_held, scope.txn); !s.ok()) {
                return {ErrorReply(s), false, 0, s};
            }
        }
    }

    // Admission-class checks for every row, before anything burns (BI9) -
    // arity, the pk rule, FK - in the row loop's order, so a refused
    // statement answers identically down to the ordinal.
    for (std::size_t k = 0; k < stmt.rows.size(); ++k) {
        const auto& values = stmt.rows[k];
        std::string err;
        // Arity is the caller's gate, not this one's: the fill is entered
        // only when every row omits its pk, because the id range it carves
        // leaves no room for a key a caller named (heap-and-tuple.md §4.1).
        // Restated here as checked redundancy - a row of the wrong length
        // would otherwise be encoded against the wrong column positions.
        if (ncols > 0 && values.size() != ncols - 1) {
            err = "ERR expected " + std::to_string(ncols - 1) +
                  " value(s) after the primary key, got " + std::to_string(values.size());
        } else if (!ta.fkeys_out.empty()) {
            const txn::ReadView view = CheckView(scope);
            for (const catalog::ForeignKeyRef& fk : ta.fkeys_out) {
                if (fk.column_no == 0 || fk.column_no > values.size()) continue;
                if (Status s = CheckForeignKeyOnWrite(ta, fk, values[fk.column_no - 1], view,
                                                      fk_held);
                    !s.ok()) {
                    err = ErrorReply(s);
                    break;
                }
            }
        }
        if (!err.empty()) {
            return {err + " (row " + std::to_string(k + 1) + ")", false};
        }
    }

    auto first = catalog_.AllocateRowIdRange(oid, stmt.rows.size());
    if (!first.ok()) {
        return {ErrorReply(first.status()), false, 0, first.status()};
    }

    // **The run's borrow, over the block of ids it just carved**
    // (AO-S6c-a). This path routes past `InsertOneRow` entirely, so without
    // this it would be the one writer left borrowing nothing - the exact
    // "header `trx_id`, no table entry" shape this sub-stage exists to
    // remove, and the one AO-S6c-b would then read as a free row. SUS-1
    // keeps the path off every relation created since the suspension, but
    // a pre-suspension heap relation still reaches it and the test binary
    // lifts the suspension, so it is live rather than theoretical.
    //
    // **One range rather than one entry per row, and the interval is exact
    // rather than a superset**: `AllocateRowIdRange` carved
    // `[first, first + rows)` and every row of this run takes an id from
    // it, so the declared unit covers precisely what is written. That is
    // item 14's shape arrived at from the other direction - the ids are
    // known instead of a predicate - and it is what keeps a bulk fill of
    // any size inside `max_locks_per_txn`.
    // The run is re-runnable - no row of it is written yet - and the cost
    // of a park is the block already carved, which `AllocateRowIdRange`
    // above has bumped the mark by. That is the same cost the single-id
    // path states, and `sys.tables.next_id` being a mark on what has been
    // *placed* is what allows it. `rows` is provably nonzero: the range
    // allocation refuses a count of zero and returned above.
    // `kCapable`: this is an `INSERT`, and an insert's verdict is not a
    // function of the waiter's read view - the ids are carved from the
    // relation's own sequence and the rows do not exist yet - so a
    // repeatable-read run of it may wait like any other.
    if (std::optional<Status> held = BorrowOrWait(
            scope,
            txn::LockKey::Range(ta.oid, first.value(), first.value() + stmt.rows.size()),
            RepeatableReadWait::kCapable)) {
        return {ErrorReply(*held), false, 0, *held};
    }

    // Encoded up front, ids contiguous from the range. The gate excluded
    // spillable schemas, so the sink is never reached.
    std::vector<std::vector<std::byte>> payloads;
    payloads.reserve(stmt.rows.size());
    for (std::size_t k = 0; k < stmt.rows.size(); ++k) {
        auto encoded =
            exec::EncodeRow(ta.schema, ta.layout, first.value() + k, stmt.rows[k],
                            exec::VarHeapSink{&page_store_, ta.varheap_page_id,
                                              /*appended=*/nullptr, ta.oid});
        if (!encoded.ok()) {
            return {ErrorReply(encoded.status()) + " (row " + std::to_string(k + 1) + ")",
                    false};
        }
        payloads.push_back(std::move(encoded.value()));
    }

    // RD6, and the batch needs one range for the **whole** run: the ids
    // are `[first, first + rows)` contiguous, so they share a chain
    // exactly when the run does not cross a boundary.
    //
    // **The reason is mechanical, not a rule the engine holds.** An
    // earlier draft cited `crosscore.md:311-314`'s cross-range DML
    // refusal; that passage is about a statement spanning two *owners*,
    // and a straddling batch need not span one. **Corrected at R4**: two
    // ranges of a split relation *can* now have different owners - that is
    // what insert spreading produces - but the ownership question is the
    // check above's, answered before this one and in its own words, and a
    // straddle inside **one** core's two ranges still reaches here. Nor
    // does the engine refuse the equivalent by another route: two
    // single-row INSERTs in one transaction land on either side of a
    // boundary through `InsertIntoRelation` and nothing objects. What
    // actually forces this is that `ChainAppendBatch` takes **one head**.
    //
    // That makes the refusal an implementation limit surfacing as a user
    // error, which §0's direction argues against - *a range is information
    // the user does not have* - and partitioning the run at the boundary
    // would remove it (the ids are contiguous, so the split index is
    // `boundary - first` and each sub-run is still contiguous).
    // Deliberately **not** done here: it changes what RD8 pins, and
    // whether a user-visible refusal is acceptable for a fact the user
    // cannot see is the operator's call. `workplan-range-directory.md`
    // §14f carries the proposal.
    auto chain = ta.HeapChainFor(first.value());
    if (!chain.ok()) {
        return {ErrorReply(chain.status()), false, 0, chain.status()};
    }
    if (!ta.ranges.empty()) {
        auto last = ta.HeapChainFor(first.value() + payloads.size() - 1);
        if (!last.ok()) {
            return {ErrorReply(last.status()), false, 0, last.status()};
        }
        if (last.value().head != chain.value().head) {
            // Rendered through `ErrorReply`, which is where the retryable
            // bit is spelled: a TxnConflict written out as its bare
            // message loses the `TXN_CONFLICT retryable=1 ` token a client
            // library's retry loop switches on, and this refusal *is*
            // retryable - the id block is already burnt, so the same
            // statement re-issued carves above the boundary and lands in
            // one range.
            return {ErrorReply(Status::TxnConflict(
                        "this multi-row INSERT spans a range boundary of relation '" +
                        stmt.table_name +
                        "'; retry it as separate statements, or as rows that fall in one range")),
                    false};
        }
    }
    auto filled = heap::ChainAppendBatch(page_store_, chain.value().head, first.value(), payloads,
                                         WriterId(scope), ta.oid, chain.value().tail_hint);
    if (!filled.ok()) {
        return {ErrorReply(filled.status()), false, 0, filled.status()};
    }

    // The rollback trail, row for row - BI4's unwind is the manager's,
    // which the bulk guard above this path already required.
    if (scope.txn != nullptr) {
        for (std::size_t k = 0; k < filled.value().rows.size(); ++k) {
            // One undo record per row, same as the per-row path: the chain
            // is per *write*, not per statement, so a bulk insert that
            // wrote one record for the batch would leave the rest of the
            // rows unreachable from it (RV10).
            txn::UndoRecordFields rec{};
            rec.prior_trx_id = txn::kNoTrxId;
            rec.prior_undo_ptr = txn::kNoUndoPtr;
            rec.target_page_id = filled.value().rows[k].page_id;
            rec.target_slot = filled.value().rows[k].slot;
            rec.type = static_cast<std::uint8_t>(txn::UndoRecordType::kInsert);
            auto ptr = txn_->AppendUndo(*scope.txn, rec, first.value() + k, {});
            if (!ptr.ok()) return {ErrorReply(ptr.status()), false, 0, ptr.status()};

            txn_->NoteInsert(*scope.txn, oid, filled.value().rows[k].page_id,
                             filled.value().rows[k].slot, first.value() + k);
        }
    }

    // T3-4: one image per touched page, in chain order. Only an image
    // describes a page assembled off the per-row path - the chain-growth
    // and index-split precedent - and at a page's worth of rows it is
    // smaller than the records it replaces. TXN framing is the manager's.
    if (wal_ != nullptr) {
        for (const heap::BatchTouchedPage& page : filled.value().pages) {
            // The one site that answers a client rather than a caller, so the
            // helper's Status is turned into this path's reply shape here.
            if (Status s = LogFullPageImage(page.page_id, WriterId(scope)); !s.ok()) {
                return {ErrorReply(s), false, 0, s};
            }
        }
    }

    return {"INSERTED oid=" + std::to_string(oid) + " rows=" + std::to_string(stmt.rows.size()) +
                " first_id=" + std::to_string(first.value()) + " last_id=" +
                std::to_string(first.value() + stmt.rows.size() - 1),
            false, stmt.rows.size()};
}

std::optional<std::string> CommandDispatcher::InsertOneRow(
    catalog::Oid oid, const catalog::TableAccess*& ta_ptr,
    const std::vector<parser::AstValue>& values, WriteScope& scope,
    const exec::FkParentVerdicts& fk_held, InsertRowResult& out) {
    const catalog::TableAccess& ta = *ta_ptr;

    // ---- How a refusal below reaches the wire -----------------------------
    //
    // Every failure below renders through `ErrorReply`, never a bare "ERR ":
    // it is the one spelling that puts `retryable=1` on the wire for a
    // TxnConflict (status.hpp's IsRetryable). The spent-lease refusals that
    // were its reason on a peer went with the leases - the extent lease at
    // AW-S1b, the row-id lease at AT-S10b - and a lock wait's fault net is
    // what still refuses that way.

    // ---- Arity, and where the pk comes from -----------------------------
    //
    // **Two arities, and the row picks** (docs/spec/heap-and-tuple.md section
    // 4.1). Until 2026-08-25 this was one arity fixed at CREATE TABLE by the
    // key mode; the mode is gone and both counts are legal on every relation:
    //
    //   ncols     - the caller names the key. values[0] is the pk, and the
    //               rest are the columns after it.
    //   ncols - 1 - the caller omits it. The engine issues one from the
    //               relation's cursor, exactly as an ASSIGNED relation's
    //               INSERT always did, and every value is a body column.
    //
    // The two counts cannot be confused, which is what makes accepting both
    // honest rather than ambiguous: INSERT is positional with no column list
    // and no body column may be omitted individually, so a row's length names
    // one reading and not the other. The old rule's stated reason for one
    // arity per relation - that a relation taking both counts could not say
    // which a wrong-length row meant - was about a relation whose two
    // readings had the same length, and no relation does.
    const std::size_t ncols = ta.schema.columns.size();
    const bool explicit_key = ncols > 0 && values.size() == ncols;

    if (ncols > 0 && values.size() != ncols && values.size() != ncols - 1) {
        // Before the id: the codec would refuse this row at encode, which
        // sits after the id is settled - and BI9's rule is that a refused row
        // burns nothing. Both accepted counts are named, because with two of
        // them a single number reads as an off-by-one against the wrong one.
        return "ERR expected " + std::to_string(ncols) + " value(s) including primary-key column '" +
               std::string(catalog::NameView(ta.schema.columns.front().name)) + "', or " +
               std::to_string(ncols - 1) + " to have it issued; got " +
               std::to_string(values.size());
    }

    // When the row names its key, the pk is values[0] and the body is the
    // rest. Split here, once, so everything downstream - the FK check,
    // assertion admission, EncodeRow, the Cabin witness, index maintenance -
    // keeps receiving exactly the shape it already expects: the columns after
    // the key. The copy is paid only by a row that names its key.
    std::vector<parser::AstValue> body_storage;
    std::uint64_t supplied_id = 0;
    if (explicit_key) {
        const parser::AstValue& key = values.front();
        if (key.type != parser::ValueType::kInt) {
            return "ERR primary-key column '" +
                   std::string(catalog::NameView(ta.schema.columns.front().name)) +
                   "' needs an integer literal (byte " + std::to_string(key.byte_offset) + ")";
        }
        if (key.int_val < 0) {
            return "ERR primary key " + std::to_string(key.int_val) +
                   " is negative (byte " + std::to_string(key.byte_offset) + ")";
        }
        supplied_id = static_cast<std::uint64_t>(key.int_val);
        body_storage.assign(values.begin() + 1, values.end());
    }
    const std::vector<parser::AstValue>& body = explicit_key ? body_storage : values;

    // ---- The forward check (docs/spec/foreign-keys.md §2) ---------------
    //
    // **Before the id is allocated**, which is a stronger form of §2's
    // "before the heap write": a refused row costs no undo work *and* no
    // Keystone id (BI9). VALUES supplies the columns after the pk, so a
    // column at schema position c is at index c-1.
    //
    // **The resolution is the caller's** (§2a, AH-R1): `HandleInsert` runs
    // the extraction pass over every row before this function is entered
    // for any of them, so a bulk statement naming one parent descends once.
    // What is left here is the per-row *answer*, which is what keeps the
    // refusal on the row that caused it.
    if (!ta.fkeys_out.empty()) {
        const txn::ReadView view = CheckView(scope);
        for (const catalog::ForeignKeyRef& fk : ta.fkeys_out) {
            if (fk.column_no == 0 || fk.column_no > body.size()) continue;
            if (Status s = CheckForeignKeyOnWrite(ta, fk, body[fk.column_no - 1], view, fk_held);
                !s.ok()) {
                return ErrorReply(s);
            }
        }
    }

    // ---- The admission check (docs/spec/assertion.md §6.2 step 2) -------
    //
    // Before the id for FK's reason: a refused row burns nothing. The
    // reservation (step 3) happens after placement, when the entry has a
    // pk and a location to carry - and in a bulk statement this row's
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
    // **And a rejection a reservation caused is a wait** (AO-S6e-c, census
    // row 11). `assertion.md` §6.2 called this a *bounded false rejection*
    // and accepted it: a statement refused because of a delta belonging to
    // a transaction that later aborts. The delta's owner is knowable - the
    // registry names one (`ReserverOnLocked`) - so the statement waits for that decide and
    // asks again, on exactly the channel every other wait in this family
    // uses. Its abort releases the delta and the re-run is admitted; its
    // commit makes the refusal true, and the re-run says so.
    //
    // **The wait-for graph is what makes this safe rather than clever**:
    // two transactions can each hold a reservation the other's admission
    // needs, and that cycle is a real one - `NoteBlockingWriter` records
    // the edge and AO-S4a refuses the waiter that would close it.
    //
    // `kCapable` without qualification: an admission reads the **live**
    // aggregate, never the waiter's read view, so the level does not enter.
    // The pk is 0 because there is none - this check runs before the id is
    // issued, deliberately, so a refused row burns nothing - and 0 is not a
    // key any row can have (`kFirstRowId`), which is what lets the wait's
    // messages say so rather than name row zero.
    std::uint64_t reserver = 0;
    exec::AssertionEnforcer::Hold admitted;
    if (Status s = enforcer_->AdmitInsert(oid, body, WriterId(scope), admitted, &reserver);
        !s.ok()) {
        if (reserver != 0) {
            NoteBlockingWriter(scope.txn, reserver, /*pk=*/0, RepeatableReadWait::kCapable);
        }
        return ErrorReply(s);
    }

    // The id, from whichever source *this row* named. Both sit at exactly
    // this point in the statement - after admission, before the encode - so a
    // refused row still burns nothing either way, and the supplied path
    // advances the relation's high-water mark rather than drawing from it.
    std::uint64_t row_id = 0;
    if (explicit_key) {
        // PW1c-5's shape gate used to refuse the whole relation here
        // (CheckWriteAffinity); the refusal is per row now, because that is
        // what it was always about. Admitting a supplied id writes the
        // relation's sys.tables row - the mark, or the key-order flip - and
        // that page was the system core's. A row that omits its pk bumps
        // the same row's mark in `AllocateRowId` below, on every core since
        // AT-S10b.
        // A peer refused a named key here until AT-S5 - admitting one
        // writes the relation's `sys.tables` row, which was the system
        // core's page. It is every core's now, under the page latch and
        // the no-park rule `AdmitExplicitRowId`'s hook states.
        // **The borrow rides inside the admit** (AO-S6c-c), which is the
        // only place both orderings can hold at once: after the key has
        // been judged legal - this call is the sole validation a named key
        // ever gets - and before the mark moves, so a statement that parks
        // here and re-runs still finds its own key admissible rather than
        // `OutOfRange` below a mark its first attempt advanced. The hook's
        // refusal comes back as this call's status, so a held row and an
        // illegal key reach the client by the same path.
        if (Status s = catalog_.AdmitExplicitRowId(
                oid, supplied_id,
                [&]() -> Status {
                    if (std::optional<Status> held =
                            BorrowOrWait(scope, txn::LockKey::Tuple(ta.oid, supplied_id),
                                         RepeatableReadWait::kCapable)) {
                        return *held;
                    }
                    return Status::OK();
                });
            !s.ok()) {
            return ErrorReply(s);
        }
        row_id = supplied_id;
    } else {
        // An issued id cannot be borrowed any earlier - the borrow's key is
        // the id - and needs no such care: a re-run draws a fresh one, and
        // burning one is what `sys.tables.next_id` being a high-water mark
        // on what has been *placed* allows.
        auto issued = catalog_.AllocateRowId(oid);
        if (!issued.ok()) {
            return ErrorReply(issued.status());
        }
        row_id = issued.value();
        if (std::optional<Status> held =
                BorrowOrWait(scope, txn::LockKey::Tuple(ta.oid, row_id),
                             RepeatableReadWait::kCapable)) {
            return ErrorReply(*held);
        }
    }

    std::vector<exec::AppendedSpill> spills;
    auto encoded = exec::EncodeRow(
        ta.schema, ta.layout, row_id, body,
        exec::VarHeapSink{&page_store_, ta.varheap_page_id, &spills, ta.oid});
    if (!encoded.ok()) {
        return ErrorReply(encoded.status());
    }

    // Into whichever storage the relation uses - a chain of heap pages or
    // a clustered B+ tree. Duplicate-key and min_key enforcement live in
    // there, not here: they are storage invariants, not dispatcher policy.
    const bool is_btree = ta.clustered_type == catalog::ClusteredType::kBtree;
    auto placed = InsertIntoRelation(ta, row_id, encoded.value(),
                                     /*trx_id=*/WriterId(scope));
    if (!placed.ok()) {
        if (logging(LogLevel::kWarn)) {
            log_->Warn(is_btree ? "btree" : "heap",
                       "insert into the relation rooted at page " +
                           std::to_string(ta.desc_page_id) +
                           " failed: " + placed.status().message());
        }
        return ErrorReply(placed.status());
    }

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
    // Catalog::UpdateIndexRoot updates the cached entry in place rather than
    // invalidating it (catalog_cache.hpp), which is exactly what the
    // desc-page relink below does *not* do.
    // Collected only when there is a log to write to, so the unlogged path
    // stays the code it always was.
    std::vector<exec::IndexWrite> index_writes;
    if (Status s = exec::MaintainIndexes(catalog_, page_store_, ta, body,
                                          /*first_col_pos=*/1, encoded.value(), row_id,
                                          /*previous=*/{},
                                          wal_ != nullptr ? &index_writes : nullptr);
        !s.ok()) {
        if (logging(LogLevel::kError)) {
            log_->Error("index", "maintaining the indexes of table oid " +
                                     std::to_string(oid) + " for id " +
                                     std::to_string(row_id) + " failed: " + s.message());
        }
        return ErrorReply(s);
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
        return ErrorReply(s);
    }

    // ---- The rollback trail, and the durable record beside it -----------
    //
    // The trail entry is what a *live* rollback reads; the undo record is
    // what survives a crash. Section 3.6 said an insert writes no record,
    // and RV10 reversed that - not for visibility, which is unchanged, but
    // because each record links to the transaction's previous one and an
    // insert that wrote none would break the chain recovery walks
    // (`docs/workplan-wal-recovery.md` §4b).
    //
    // **The tuple is not stamped with this pointer.** `undo_ptr == 0` still
    // means "inserted" to every reader; the record is reachable only
    // through the transaction chain, which is what keeps §3.6's visibility
    // rule intact.
    if (scope.txn != nullptr) {
        txn::UndoRecordFields rec{};
        rec.prior_trx_id = txn::kNoTrxId;
        rec.prior_undo_ptr = txn::kNoUndoPtr;
        rec.target_page_id = placed.value().page_id;
        rec.target_slot = placed.value().slot;
        rec.type = static_cast<std::uint8_t>(txn::UndoRecordType::kInsert);
        auto ptr = txn_->AppendUndo(*scope.txn, rec, row_id, {});
        if (!ptr.ok()) return ErrorReply(ptr.status());

        txn_->NoteInsert(*scope.txn, oid, placed.value().page_id, placed.value().slot,
                         row_id);
    }

    // The spills, **after** the row's own record and not before it. The
    // only ordering this owes is "before `LogInsert` writes the
    // VARHEAP_APPENDs"; putting it here shortens the window in which the
    // tuple sits in the page with no trail entry naming it, which is a row
    // a rollback would not undo. `Abort` has no suspension point, so
    // reversing the trail's order relative to the two writes is
    // unobservable.
    if (Status s = NoteSpills(scope, oid, row_id, spills); !s.ok()) return ErrorReply(s);

    // Logged after the page is mutated and before the client is answered -
    // see the ordering note in this class's header for why that is safe
    // here and what would break it.
    if (Status s = LogInsert(placed.value(),
                             is_btree ? PageType::kBtreeLeaf : PageType::kHeap, encoded.value(),
                             WriterId(scope), oid, spills, index_writes,
                             /*own_txn=*/scope.txn == nullptr);
        !s.ok()) {
        if (logging(LogLevel::kError)) {
            log_->Error("wal", "logging the insert of id " + std::to_string(row_id) +
                                   " failed: " + s.message());
        }
        return ErrorReply(s);
    }

    // The tree grew a level, so the relation's root moved. Persisted only
    // now: the new root's contents are logged above, and a root published
    // before the pages under it are described is a root recovery cannot
    // follow. Since PW2-4 the move writes the anchor and updates the
    // cached entry **in place** - `ta` stays valid, no invalidation
    // broadcast, no catalog write.
    if (placed.value().new_root != kInvalidPageId) {
        if (Status s = catalog_.UpdateRelationDescPage(oid, placed.value().new_root,
                                                       ta.anchor_page_id);
            !s.ok()) {
            if (logging(LogLevel::kError)) {
                log_->Error("btree", "table oid " + std::to_string(oid) +
                                         " grew a level but its root could not be repointed at "
                                         "page " +
                                         std::to_string(placed.value().new_root) + ": " +
                                         s.message());
            }
            return ErrorReply(s);
        }
        if (logging(LogLevel::kInfo)) {
            log_->Info("btree", "table oid " + std::to_string(oid) +
                                    " grew a level; root is now page " +
                                    std::to_string(placed.value().new_root));
        }
        // The in-place update (PW2-4) keeps `ta` valid, so the refresh the
        // pre-anchor invalidation forced is gone - the pointer reference
        // stays for the day a move ever invalidates again.
    }

    // A relation growing a page is rare and structural - the closest thing
    // this engine has to a file extending - so it is Debug, above the
    // per-tuple Trace line below.
    if (placed.value().restructured() && logging(LogLevel::kDebug)) {
        log_->Debug(is_btree ? "btree" : "heap",
                    "relation of table oid " + std::to_string(oid) +
                        " grew: new tuple page " + std::to_string(placed.value().page_id) +
                        " min_key=" + std::to_string(row_id) + " pages_logged=" +
                        std::to_string(placed.value().changes().size()));
    }

    // Trace: one line per inserted tuple. Logged from here rather than from
    // PageView, which is a bare view over page bytes with no business
    // owning a logger - and which the catalog also writes through, where a
    // "heap insert" line would describe a catalog row, not a user tuple.
    if (logging(LogLevel::kTrace)) {
        log_->Trace(is_btree ? "btree" : "heap", "insert page=" + std::to_string(placed.value().page_id) +
                                " slot=" + std::to_string(placed.value().slot) +
                                " id=" + std::to_string(row_id) +
                                " bytes=" + std::to_string(encoded.value().size()));
    }

    // The page id rides the (single-row) reply because it is no longer
    // implied by the table: a client that wants to `SHOW PAGE` the row it
    // just wrote would otherwise have to walk the chain to guess.
    out.id = row_id;
    out.page_id = placed.value().page_id;
    out.slot = placed.value().slot;
    return std::nullopt;
}

StatusOr<storage::InsertPlacement> CommandDispatcher::InsertIntoRelation(
    const catalog::TableAccess& access, std::uint64_t id, std::span<const std::byte> payload,
    std::uint64_t trx_id) {
    storage::InsertPlacement out;

    switch (access.clustered_type) {
        case catalog::ClusteredType::kHeap: {
            // The relation is a chain of heap pages (heap_chain.hpp): the
            // tuple goes into the tail, and a full tail grows the chain by
            // one page rather than failing. Duplicate-key and min_key
            // enforcement live in there - they are heap invariants, not
            // dispatcher policy.
            // RD6: the head is the *range's*, not the relation's
            // (`TableAccess::HeapChainFor` owns the argument). On an
            // unsplit relation it is `desc_page_id` and `heap_tail_hint`,
            // byte for byte what this line was. An id in no range is
            // refused by the resolve inside it.
            auto chain = access.HeapChainFor(id);
            if (!chain.ok()) return chain.status();
            auto placed = heap::ChainInsert(page_store_, chain.value().head, id, payload, trx_id,
                                            access.oid, chain.value().tail_hint);
            if (!placed.ok()) return placed.status();

            out.page_id = placed.value().page_id;
            out.slot = placed.value().slot;
            if (placed.value().grew_chain) {
                // Chain growth in the shared vocabulary, in the order redo
                // applies it: the old tail's image (which already carries
                // the new link - ChainInsert sets it before returning),
                // then the page it points at, which the HEAP_INSERT fills.
                out.Record(placed.value().linked_from, /*is_new_page=*/false, 0);
                out.Record(placed.value().page_id, /*is_new_page=*/true, id);
            }
            return out;
        }
        case catalog::ClusteredType::kBtree: {
            // The relation is a clustered B+ tree (btree.hpp) rooted at the
            // same desc page: the descent picks the leaf, and a full leaf
            // splits right without moving a key. Reports its own structural
            // set, and a new root when the tree gained a level.
            auto placed = btree::BtreeInsert(page_store_, access.desc_page_id, id, payload,
                                             trx_id, access.oid);
            if (!placed.ok()) return placed.status();

            out.page_id = placed.value().page_id;
            out.slot = placed.value().slot;
            return placed.value();
        }
    }
    return Status::Corruption("relation oid " + std::to_string(access.oid) +
                              " has an unknown clustered_type");
}

Status CommandDispatcher::WalkHeapChains(
    std::span<const PageId> heads, storage::PageAccess page_access,
    const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&, std::uint16_t)>&
        fn,
    WalkCursor* cursor) {
    // Where this walk starts. A cursor that is not active is a first walk,
    // which starts at the first range's head and slot 0 - written as the
    // same three variables so the resumed and the first walk are one loop
    // rather than two shapes that have to be kept agreeing.
    const bool resuming = cursor != nullptr && cursor->active;
    std::size_t range_index = resuming ? cursor->range : 0;
    // A resumed cursor names a page in the middle of its chain; a first
    // walk starts at the head. Both are "the page this range starts at".
    PageId resume_page = resuming ? cursor->page : kInvalidPageId;
    std::uint16_t resume_slot = resuming ? cursor->slot : 0;

    // **Set by the wrapper below, read by both loops.** `ChainVisitOnePage`
    // answers a visitor stop and a chain end with the same kInvalidPageId,
    // so the flag is the only thing that tells them apart - and the
    // difference is load-bearing here in a way it never was for
    // `heap::ChainVisit`: a stop must end the walk over *every remaining
    // range*, where a chain end must step to the next one (RD6). The
    // pre-AO-S3b code called `ChainVisit` per range and so continued to the
    // next range after a stop; no visitor of a split relation returned one,
    // which is why that never showed.
    bool cut = false;
    PageId cut_page = kInvalidPageId;
    std::uint16_t cut_slot = 0;

    for (; range_index < heads.size(); ++range_index) {
        PageId cur = resume_page != kInvalidPageId ? resume_page : heads[range_index];
        // Consumed: only the range the cursor named resumes mid-chain, and
        // every range after it starts at its own head.
        const std::uint16_t first_slot = resume_page != kInvalidPageId ? resume_slot : 0;
        resume_page = kInvalidPageId;
        resume_slot = 0;

        // Per chain, as `heap::ChainVisit` applies it - the budget bounds
        // one chain's hops, not the relation's, and a walk of several
        // ranges is several chains.
        const PageId walk_origin = cur;
        for (std::uint32_t pages = 0;; ++pages) {
            if (Status s = storage::CheckPageWalkBudget(pages, walk_origin, "relation walk");
                !s.ok()) {
                return s;
            }
            const bool on_resume_page = pages == 0 && first_slot != 0;
            const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&,
                                                                std::uint16_t)>
                guarded = [&](PageId page_id, heap::PageView& page,
                              std::uint16_t slot) -> StatusOr<storage::VisitControl> {
                // The skip is here rather than in `ChainVisitOnePage`
                // because it costs nothing to iterate a slot we do not
                // read: the tuple is only touched inside `fn`.
                if (on_resume_page && slot < first_slot) return storage::VisitControl::kContinue;
                auto outcome = fn(page_id, page, slot);
                if (!outcome.ok() || !outcome.has_value()) return outcome;
                if (outcome.value() == storage::VisitControl::kStop) {
                    cut = true;
                    cut_page = page_id;
                    // **The slot the walk stops *at*, not after it.** The
                    // visitor that stopped did not finish this row - it is
                    // the row it is waiting for - so a resume must offer
                    // the same slot again rather than step past it.
                    cut_slot = slot;
                }
                return outcome;
            };
            auto next = heap::ChainVisitOnePage(page_store_, cur, page_access, guarded);
            if (!next.ok()) return next.status();
            if (cut) break;
            if (next.value() == kInvalidPageId) break;
            cur = next.value();
        }
        if (cut) break;
    }

    if (cursor != nullptr) {
        if (cut) {
            cursor->range = range_index;
            cursor->page = cut_page;
            cursor->slot = cut_slot;
            cursor->active = true;
        } else {
            // The walk covered what it was given, so a cursor left active
            // would resume a finished statement in the middle of itself.
            // `rows_done` survives the reset: it is the statement's count,
            // not a position, and the walk is not what owns it.
            const std::uint32_t done = cursor->rows_done;
            *cursor = WalkCursor{};
            cursor->rows_done = done;
        }
    }
    return Status::OK();
}

Status CommandDispatcher::WalkBtreeLeaves(
    PageId root, storage::PageAccess page_access,
    const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&, std::uint16_t)>&
        fn,
    WalkCursor* cursor) {
    const bool resuming = cursor != nullptr && cursor->active;
    const std::uint64_t resume_pk = resuming ? cursor->pk : 0;

    // **A fresh descent, not a remembered leaf**, and that is the whole of
    // the btree arm's safety argument. While the statement was parked
    // another session may have split the leaf it stopped in, moving the
    // upper half of that leaf - possibly including rows this walk had
    // already written - into a new right sibling. Returning to the
    // remembered page would visit those rows a second time. Descending by
    // key cannot: the tree is ordered by Keystone id, so a row this walk
    // finished sorts below `resume_pk` wherever a split has since put it,
    // and the skip below is exact rather than positional.
    auto start = resuming ? btree::BtreeSeekLeaf(page_store_, root, resume_pk)
                          : btree::BtreeLeftmostLeaf(page_store_, root);
    if (!start.ok()) return start.status();

    bool cut = false;
    std::uint64_t cut_pk = 0;
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
                if (key.has_value() && *key < resume_pk) return storage::VisitControl::kContinue;
            }
            auto outcome = fn(page_id, page, slot);
            if (!outcome.ok() || !outcome.has_value()) return outcome;
            if (outcome.value() == storage::VisitControl::kStop) {
                const std::optional<std::uint64_t> key = key_at(slot);
                // **A stop with no readable key is `Corruption`, not a
                // resume from zero.** `fn` has just read this slot live, so
                // failing to read it back is a corrupt page rather than an
                // ordinary dead slot - and the tempting fallback is the
                // worst outcome the stage can produce. Leaving `cut_pk` at
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
                cut_pk = *key;
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
            cursor->pk = cut_pk;
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
    catalog::PkSpan span, WalkCursor* cursor) {
    switch (access.clustered_type) {
        case catalog::ClusteredType::kHeap:
            // RD6: **one chain per range** (CC8), so a walk is one walk per
            // range in `lo` order - which is the order RD7 concatenates in,
            // established here once rather than by two implementations
            // matching.
            //
            // The unsplit path is the single `ChainVisit` it always was,
            // reached by one branch on a cached field.
            if (!access.ranges.empty()) {
                // **The ranges this statement can touch**, which is all of
                // them unless the caller narrowed the pk window (R4/IS4).
                // Narrowing is sound because a row's id decides its range
                // (invariant 3 per range), so a pk outside `span` cannot be
                // in a range outside it either - and it is what lets a
                // `WHERE pk = k` write walk the one chain k can be in rather
                // than every range's.
                auto touched = catalog::ResolveRanges(access.ranges, span);
                if (!touched.ok()) return touched.status();
                // **Every range's chain is walked here** (AT-S5). The refusal
                // that stood here - a range another core owned, "which this
                // core cannot read locally" - was true of a per-core pool and
                // false since one frame table serves every core (AM-S2 step
                // 3); it kept a walk from skipping a foreign range silently,
                // and walking every range keeps that property better than
                // refusing did. Nothing chooses another core for a read
                // since AT-S9 (D18).
                std::vector<PageId> heads;
                heads.reserve(touched.value().size());
                for (const catalog::RangeTarget& range : touched.value()) {
                    heads.push_back(range.entry_page);
                }
                return WalkHeapChains(heads, page_access, fn, cursor);
            }
            return WalkHeapChains({&access.desc_page_id, 1}, page_access, fn, cursor);
        case catalog::ClusteredType::kBtree:
            // No range arm: D1 declines every btree relation, so one never
            // has a directory. Left as an absence rather than a refusal,
            // because the gate is what makes it unreachable.
            return WalkBtreeLeaves(access.desc_page_id, page_access, fn, cursor);
    }
    return Status::Corruption("relation oid " + std::to_string(access.oid) +
                              " has an unknown clustered_type");
}

std::optional<std::uint64_t> CommandDispatcher::PkLiteral(const catalog::TableAccess& access,
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
    // Negative ids do not exist (invariant 6 zero-extends the 40-bit id),
    // so a negative literal is a guaranteed miss - and casting it to
    // uint64 would probe an enormous pk instead.
    if (cond.val.int_val < 0) return std::nullopt;
    if (!IEquals(cond.col.name, catalog::NameView(access.schema.columns.front().name))) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(cond.val.int_val);
}

std::optional<std::uint64_t> CommandDispatcher::PkEqualityTarget(
    const catalog::TableAccess& access, const std::vector<parser::Condition>& where) const {
    // Deliberately storage-agnostic: this answers "is this statement a bare
    // pk point lookup", which is a property of the WHERE clause alone.
    // Whether anything can shortcut it - a tree descent, or nothing at
    // all - is LocateByPk's question.
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
    return PkLiteral(access, cond);
}

CommandDispatcher::DdlScope CommandDispatcher::DdlScopeFor(WriteScope& write) {
    DdlScope scope;
    if (write.txn == nullptr) return scope;  // no manager: kBootstrapXid, as ever
    scope.txn = write.txn;
    scope.trx_id = write.txn->id();

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
        auto ptr = txn_->AppendUndo(*t, rec, e.pk, image);
        return ptr.ok() ? Status::OK() : ptr.status();
    });
    return scope;
}

// Every DDL route runs inside this and only this, which is what makes "no
// handler may skip FinishDdlStatement" structural rather than a convention
// a fifth route forgets (review S4): the undo hook the body installs
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
    if (Status ended = EndWrite(session, scope, verdict); !ended.ok() && !failed) {
        // The DDL succeeded and its commit did not - the commit failure is
        // the client's answer, exactly as the DML handlers report it.
        out = {ErrorReply(ended), false};
    }
    // The implicit transaction resolved inside EndWrite, so the seam that
    // explicit COMMIT/ROLLBACK reaches through EndDdlScope runs here: the
    // cache the open DDL filtered is stale either way, and settled marks
    // are worth one sweep (§5d).
    if (owned) EndDdlScopeById(id);
}

Status CommandDispatcher::AdoptSnapshot(Session& session, std::uint64_t snapshot_lsn) {
    // **No caller since AT-S6.** It was called from `EnrolFor` alone,
    // immediately after a `BEGIN` it checked succeeded - so the session held
    // a transaction and this dispatcher a manager (`HandleBegin` refuses
    // without one); `EnrolFor` went with the ship. The one guard is the
    // null dereference's.
    if (txn_ == nullptr || session.transaction() == nullptr) {
        return Status::InvalidArgument(
            "cross-owner transaction: no transaction is open on this session to adopt a "
            "snapshot into");
    }
    return txn_->AdoptSnapshot(*session.transaction(), snapshot_lsn);
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
    // The fast path, and the one nearly every statement takes.
    if (txn_ == nullptr || ddl_txns_.empty()) return std::nullopt;

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

std::optional<DispatchOutcome> CommandDispatcher::RefuseIfNameHeldByPendingDrop(
    std::string_view name, Session& session) {
    // `ViewFor` answers nullopt exactly when no transaction holds
    // uncommitted DDL - and with none open there is no pending drop for a
    // create to collide with, so the fast path pays nothing.
    const std::optional<txn::ReadView> view = ViewFor(session);
    if (!view.has_value()) return std::nullopt;

    auto held = catalog_.NameHeldByPendingDrop(name, *view);
    if (!held.ok()) return DispatchOutcome{ErrorReply(held.status()), false};
    if (!held.value()) return std::nullopt;

    // Refused rather than allowed, for §6's reason and with §6's cost: the
    // refusal is spurious if that transaction commits its drop, and it
    // names a relation the asker can no longer see. Allowing it is the
    // outcome that corrupts - the drop's rollback restores a second live
    // row with this name, and resolution then answers with whichever one
    // sits earlier on the page.
    return DispatchOutcome{"ERR relation '" + std::string(name) +
                               "' is being dropped by a transaction that has not committed; "
                               "the name is not free until that transaction resolves",
                           false};
}

void CommandDispatcher::EndDdlScope(const Session& session) {
    const txn::Transaction* txn = session.transaction();
    if (txn == nullptr) return;
    EndDdlScopeById(txn->id());
}

void CommandDispatcher::EndDdlScopeById(std::uint64_t txn_id) {
    const bool held_ddl = std::erase(ddl_txns_, txn_id) > 0;
    // **Both endings need this, and only the rollback half used to.** A
    // rollback compensates through the page, retiring rows behind the
    // catalog's back, so anything cached about them while the transaction
    // was open now describes rows that are gone.
    //
    // The commit half is DT9's (`ddl-transactional.md` §5b). The old
    // comment here said "a commit leaves the rows in place, so what was
    // cached about them stays true", which was correct until an unfiltered
    // read started asking whether a mark's deleter is still in flight:
    // **commit is now the moment a delete-mark starts counting.** A cache
    // filled during an open `DROP INDEX` holds the index deliberately -
    // that is what keeps maintenance writing entries a rollback would
    // need - and holding it past the commit would keep maintaining an
    // index that is gone.
    //
    // Unconditional on the ending rather than split by it, because the
    // condition that would split it ("did this transaction delete-mark
    // anything?") is one more thing to keep true, and a DDL transaction
    // ending is rare enough that a cache clear it did not strictly need
    // costs nothing worth measuring.
    if (held_ddl) {
        // The first purge consumer (workplan-reader-registration.md D5).
        // Here and nowhere hotter: DDL resolution is the only event that
        // creates or settles a mark, the core is between resolutions so no
        // unregistered synchronous view is live, and the resolved
        // transaction is already inactive so its own marks are fair game
        // the moment no older reader holds a lease. **Before** the
        // invalidation below, so its flush carries the retirements too.
        // A failed sweep is a maintenance failure, not the statement's:
        // the marks it left are exactly as reachable as before, so it is
        // logged and the reply stands.
        //
        // **System core only, as defence in depth.** Until AN-S2 this
        // gate carried the soundness argument: a core's `ReadHorizon()`
        // walked its own readers alone, so a peer's - no transactions, no
        // leases - answered UINT64_MAX and would have retired a mark whose
        // deleter was live on core 0. Since AN-S2 the sweep judges every
        // mark by the instance's floor and horizon
        // (`ResolvedForEveryReader`), so a peer's sweep would be sound;
        // since AT-S5 every core writes the catalog and takes DDL, so this
        // is a **placement**, not an authority: one core sweeps so two do
        // not walk the same chains at once, and core 0 is the one because
        // it is always present (spec §5d).
        if (core_id_ == catalog::kSystemCore) {
            auto purged = catalog_.PurgeSettledDeleteMarks();
            if (purged.ok()) {
                catalog_marks_purged_ += purged.value();
            } else if (log_ != nullptr) {
                log_->Warn("catalog", "delete-mark purge failed: " + purged.status().message());
            }
        }
        catalog_.InvalidateAfterCompensation();
    }
}

void CommandDispatcher::MarkHoldsDdl(txn::Transaction& txn) {
    txn.NoteWroteCatalog();
    if (std::find(ddl_txns_.begin(), ddl_txns_.end(), txn.id()) == ddl_txns_.end()) {
        ddl_txns_.push_back(txn.id());
    }
}

void CommandDispatcher::NoteCatalogRowChanges(
    DdlScope& scope, const std::vector<catalog::CatalogRowChange>& changed) {
    // The rollback trail for a DDL route that *changes* rows rather than
    // inserting them - `DROP TABLE` and `DROP NAMESPACE`. One function
    // because the two had one copy each and the second was the first minus
    // the delete-mark branch: `DropNamespace` only ever retypes today, so
    // the copy was correct and silently assumed the other's invariant.
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
                  std::uint64_t pk) -> StatusOr<txn::TransactionManager::RowLocation> {
        auto access = catalog_.InitTableAccess(static_cast<catalog::Oid>(rel_oid));
        if (!access.ok()) return access.status();
        PkLookup found = LocateByPk(*access.value(), pk, storage::PageAccess::kWrite);
        if (found.kind != PkLookup::Kind::kAt) {
            // kAbsent means the row is gone, kScan means the relation has no
            // descent to ask. Neither is a location, and rollback may not
            // guess one - the caller reports rather than compensating a slot
            // it cannot vouch for.
            return Status::NotFound("row id " + std::to_string(pk) + " of relation oid " +
                                    std::to_string(rel_oid) +
                                    " could not be relocated for rollback");
        }
        return txn::TransactionManager::RowLocation{found.at.page_id, found.at.slot,
                                                    std::move(found.at.leaf)};
    };
}

CommandDispatcher::PkLookup CommandDispatcher::LocateByPk(const catalog::TableAccess& access,
                                                          std::uint64_t pk,
                                                          storage::PageAccess mode) {
    if (access.clustered_type == catalog::ClusteredType::kBtree) {
        // The tree *is* the relation's storage, so its answer is
        // authoritative in both directions: a hit is where the row lives,
        // and a NotFound means no such row - the scan it replaces would
        // visit the same leaf and find the same nothing. This is the one
        // place a point lookup may skip the scan on a miss, and it is
        // allowed precisely because it is not a hint.
        auto found = btree::BtreeLookup(page_store_, access.desc_page_id, pk, mode);
        if (found.ok()) {
            // The leaf rides out held, in `mode`: the caller reads or writes
            // the slot through it and never re-fetches (AT-0 item 12).
            return PkLookup{PkLookup::Kind::kAt, std::move(found.value())};
        }
        if (found.status().code() == StatusCode::kNotFound) {
            return PkLookup{PkLookup::Kind::kAbsent, {}};
        }
        // A corrupt descent or an unreadable page: fall back to the scan
        // rather than fail the query. The scan reaches the leaves through
        // the sibling links, so it can still answer when the index above
        // them cannot.
        if (logging(LogLevel::kWarn)) {
            log_->Warn("btree", "descent for pk " + std::to_string(pk) + " in table oid " +
                                    std::to_string(access.oid) +
                                    " failed, falling back to a scan: " +
                                    found.status().message());
        }
        return PkLookup{PkLookup::Kind::kScan, {}};
    }

    // A heap relation has no pk index: the chain scan is the only path,
    // and it is authoritative.
    return PkLookup{PkLookup::Kind::kScan, {}};
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
                                       "' names no relation in this statement");
    }
    for (std::size_t i = 0; i < view.column_names.size(); ++i) {
        if (IEquals(view.column_names[i], col.name)) return i;
    }
    return Status::InvalidArgument("view sys." + stmt.from.table_name + " has no column '" +
                                   col.name + "'");
}

// The `type_val` a view's value must be compared under.
//
// **Every integer a view emits is built from a `uint64_t`** -
// `catalog_view.cpp`'s `Int()` casts it into `int_val` and keeps the
// digits in `raw_int_text` - so comparing `int_val` signed puts every
// value above INT64_MAX below every value under it. `sys.patterns`
// carries exactly such a column: a `pattern_id` is a full-range 64-bit
// fingerprint, and `WHERE pattern_id < 100` answered every id with the
// top bit set. The value renders correctly all the while, because
// `FormatValue` reads the digits - so the view showed a number and then
// refused to compare it as that number.
//
// Keyed on the value rather than on the column because a view has no
// column types, only values, and every builder emits one kind per column.
// A string keeps `0`: the uint64 arm is tested before the string arm and
// would answer false for every string comparison.
std::uint32_t ViewCompareTypeVal(const parser::AstValue& value) {
    return value.type == parser::ValueType::kInt ? catalog::kTypeValUint64 : 0;
}

}  // namespace

DispatchOutcome CommandDispatcher::HandleCatalogView(const parser::SelectStmt& stmt) {
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

    // `ORDER BY` is refused here for the same reason and with the same
    // shape. `exec::OutputSort` normalizes its keys out of a `ChainFrame`
    // against `SortKey`s the compiler resolved to column indices in a
    // Schema; a view has neither, so honouring the clause would mean a
    // second comparator over `parser::AstValue` living beside the one the
    // engine already has. `LIMIT`/`OFFSET` below need no such thing - the
    // quota consumes rows and has no opinion about where they came from -
    // which is exactly why one clause of the tail is served and the other
    // is declined rather than accepted and ignored.
    if (!stmt.order_by.empty()) {
        return {"ERR ORDER BY over a catalog view (sys." + stmt.from.table_name +
                    ") is not supported; a view's rows are materialized by the catalog's "
                    "readers and carry no schema for the sort to resolve against (byte " +
                    std::to_string(stmt.order_by.front().key.byte_offset) + ")",
                false};
    }

    auto view = exec::ReadCatalogView(catalog_, stmt.from.table_name);
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
    // pattern_id` answered a header on a fresh instance, because the loop
    // never ran, and refused over `sys.tables`, because it did. A refusal
    // that data can silence is not a refusal.
    std::vector<std::size_t> where_at;
    where_at.reserve(stmt.where.size());
    for (const parser::Condition& cond : stmt.where) {
        if (cond.has_subquery()) {
            return {"ERR a subquery predicate over a catalog view is not supported: the "
                    "view is materialized, so there is no relation for a sub-chain to "
                    "correlate against", false};
        }
        if (cond.rhs_kind == parser::RhsKind::kColumn) {
            return {"ERR a column-to-column comparison over a catalog view is not "
                    "supported", false};
        }
        auto at = ResolveViewColumn(rows, stmt, cond.col);
        if (!at.ok()) return {ErrorReply(at.status()), false, 0, at.status()};
        where_at.push_back(at.value());
    }

    std::ostringstream os;
    bool first_col = true;
    for (std::size_t index : project) {
        if (!first_col) os << ',';
        os << rows.column_names[index];
        first_col = false;
    }

    // I11's contract, over the one row source that is not a chain. The
    // view's rows arrive in the order the reply prints them, so a prefix
    // of them is a prefix of an order the statement has - the same
    // sentence `pagination.hpp` makes for a walked relation, and the
    // reason `LIMIT` needs no `ORDER BY` to be well defined.
    exec::EmissionQuota quota(stmt.offset, stmt.limit);
    for (const std::vector<parser::AstValue>& row : rows.rows) {
        bool matched = true;
        for (std::size_t i = 0; i < stmt.where.size(); ++i) {
            const parser::Condition& cond = stmt.where[i];
            const parser::AstValue& value = row[where_at[i]];
            // One comparator for the row's value, so the `type_val` rule is
            // decided in one place rather than copied per operand.
            const auto Compare = [&value](const parser::AstValue& bound, parser::CompareOp op) {
                return exec::CompareValues(ViewCompareTypeVal(value), value, bound, op);
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
                          ? Compare(cond.val, parser::CompareOp::kGte) &&
                                Compare(cond.val_high, parser::CompareOp::kLte)
                          : Compare(cond.val, cond.op);
            if (!matched) break;
        }
        if (!matched) continue;

        // Noted once per *qualifying* row, after the WHERE and before the
        // formatting: OFFSET skips rows that passed the predicate, and a
        // skipped row costs no rendering.
        const exec::QuotaVerdict verdict = quota.Note();
        if (verdict == exec::QuotaVerdict::kStop) break;
        if (verdict == exec::QuotaVerdict::kSkip) continue;

        os << "\\n";
        bool first_val = true;
        for (std::size_t index : project) {
            if (!first_val) os << ',';
            // type_val 0 here and not `ViewCompareTypeVal`: rendering
            // consults the column's type only to tell a DATE or TIMESTAMP
            // from the integer it is stored as, and no view has one. An
            // integer above INT64_MAX still prints correctly, because
            // `FormatValue` reads `raw_int_text` - which is exactly why
            // the comparison above needed a rule and this does not.
            os << exec::FormatValue(/*type_val=*/0, row[index]);
            first_val = false;
        }
        if (verdict == exec::QuotaVerdict::kEmitThenStop) break;
    }
    return {os.str(), false};
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
    const std::optional<stats::InstanceKey>& instance, const txn::Snapshot& snapshot,
    exec::PositionSink& borrow) {
    if (Status s = aggregator_.Reset(*chain.aggregate, chain.column_names, aggregate_limits_);
        !s.ok()) {
        return {ErrorReply(s), false, 0, s};
    }

    if (Status s = sink.Describe(DescribeFields(chain.column_names,
                                               AggregateOutputTypes(*chain.aggregate),
                                               AggregateOutputTypeMods(*chain.aggregate)));
        !s.ok()) {
        return {ErrorReply(s), false, 0, s};
    }

    // The fold's own failures - a SUM overflow, a cap - have to reach the
    // client, and a `RowSink` answers `StatusOr<VisitControl>`, so a
    // non-ok status ends the walk and propagates out of Execute. That is
    // the same path a decode error already takes.
    exec_stats_.steps.clear();
    Status ran = exec::Execute(
        catalog_, page_store_, chain,
        [&](const exec::ChainFrame& frame) -> StatusOr<storage::VisitControl> {
            if (Status s = aggregator_.Accumulate(frame); !s.ok()) return s;
            return storage::VisitControl::kContinue;
        },
        &exec_stats_, budget_, trail, replay, cabins_, &snapshot, indexes_enabled_,
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
    const std::vector<std::uint32_t> out_types = AggregateOutputTypes(*chain.aggregate);
    Status emitted = aggregator_.Finish(
        [&](std::span<const parser::AstValue> row) -> Status {
            if (Status s = sink.EncodeValueRow(out_types, row, row_scratch); !s.ok()) return s;
            return sink.Emit(row_scratch);
        });
    if (!emitted.ok()) {
        return {ErrorReply(emitted), false, 0, emitted};
    }

    // Recorded after a *complete* execution, and unconditionally - the fold
    // is downstream of all three, so an aggregated statement records the
    // trail, the access shape and the signals its unaggregated twin would.
    RecordExecution(instance, trail, chain, exec_stats_);
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
                                              const std::optional<stats::InstanceKey>& instance,
                                              const txn::Snapshot& snapshot,
                                              exec::PositionSink& borrow) {
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
    sorter_.Reset(chain, sort_max_rows_);
    std::string analyze_scratch;
    Status ran = exec::Execute(
        catalog_, page_store_, chain,
        [&](const exec::ChainFrame& frame) -> StatusOr<storage::VisitControl> {
            if (sorter_.active()) {
                auto admitted = sorter_.Admit(frame);
                if (!admitted.ok()) return admitted.status();
                // Admitted rows are taken with empty text, which is the one
                // respect in which this run is cheaper than the real one -
                // and the same respect in which it already was. What matters
                // is that the same rows are *admitted*, so `sorted=` and
                // `rows=` describe the run that would have happened.
                if (admitted.value()) sorter_.Take(analyze_scratch);
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
    if (sorter_.active()) {
        sorter_.Finish();
        exec::DrainSorted(quota, sorter_.rows(), [&](const exec::OutputSort::Row&) { ++rows; });
    }
    RecordExecution(instance, trail, chain, stats);

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
    if (sorter_.active()) {
        os << " sorted=" << sorter_.rows().size();
    }

    // The statement's own pattern_id, in the same hex `SHOW PATTERNS`
    // lists a row under. That is what makes "which observed pattern did
    // this statement match" answerable by comparing two numbers - no trail
    // recorder has to exist for the comparison to be meaningful.
    //
    // Taken from the instance the caller already identified - which came
    // from the parse, not from a second lex of `sql`.
    if (instance.has_value()) {
        os << " pattern_id=0x" << std::hex << instance->pattern_id << std::dec;
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
                                                bool analyze) {
    // The statement boundary. Under READ COMMITTED this is where a new read
    // view is taken - so two SELECTs in one transaction can see different
    // data, which is the level's entire definition.
    auto snapshot = SnapshotFor(session);
    // `ErrorReply`, not a bare "ERR ": `SnapshotFor` can refuse (a spent
    // transaction-id lease on a peer did, until AT-S10b), and the wire's
    // `retryable=1` is what a client's retry loop reads. The
    // DELETE site has always rendered it this way; these two did not,
    // so the same refusal carried the bit on one verb and lost it on
    // the other (the SS2 review's cut 2).
    if (!snapshot.ok()) return {ErrorReply(snapshot.status()), false, 0, snapshot.status()};

    // An explicit Parser rather than the free `Parse()`, so the statement's
    // fingerprint can be taken **from the parse itself** (parser.hpp). It
    // used to come from `FingerprintOf`, which lexed the text a second time -
    // measured at ~13% of a point join's latency, three times what the
    // recording it was for actually cost, and the whole of replay's B+ tree
    // regression (bench/results-waystone-v2.md).
    parser::Parser parser(line);
    auto parsed = parser.Parse();
    if (!parsed.ok()) {
        return {ErrorReply(parsed.status()), false, 0, parsed.status()};
    }
    if (!std::holds_alternative<parser::SelectStmt>(parsed.value())) {
        return {"ERR expected a SELECT statement", false};
    }
    auto& stmt = std::get<parser::SelectStmt>(parsed.value());

    // No guards here any more. V05, V06 and V07 each added a refusal
    // because the single-relation scan below would have answered their
    // statements *wrongly* rather than failing - a join scanning only its
    // first relation, a projection emitting every column, a subquery
    // predicate matching no column and dropping every row. The compiler
    // and the step VM answer all three now, so the refusals are gone
    // rather than kept "just in case": a guard that no longer guards
    // anything is a guard nobody maintains.
    //
    // A catalog view (`SELECT * FROM sys.tables`) is answered before the
    // compiler is asked for a chain, because it is not a relation: its
    // rows are produced by the catalog's typed readers, not walked out of
    // pages, so there is nothing for a step to read (exec/catalog_view.hpp
    // says why the on-disk formats cannot be unified).
    if (!stmt.from.schema.empty() || !stmt.joins.empty()) {
        const bool from_is_view =
            IEquals(stmt.from.schema, exec::kCatalogSchema) &&
            exec::IsCatalogView(stmt.from.table_name);
        // The same test the FROM relation gets, and it had to become the
        // same test at AF-T3: `!schema.empty()` meant "is a view" only
        // while `sys` was the one qualifier there was, and under a user
        // namespace it would answer "a catalog view cannot be joined"
        // about two ordinary relations.
        bool any_join_is_view = false;
        for (const parser::JoinClause& join : stmt.joins) {
            if (IEquals(join.relation.schema, exec::kCatalogSchema) &&
                exec::IsCatalogView(join.relation.table_name)) {
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
            return HandleCatalogView(stmt);
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
            for (const std::string& name : exec::CatalogViewNames()) {
                if (!known.empty()) known += ", ";
                known += "sys." + name;
            }
            return {"ERR no catalog view named 'sys." + stmt.from.table_name + "' (known: " +
                        known + ")",
                    false};
        }
        // **AF-T3 opened this arm.** It used to answer everything but `sys`
        // with "unknown schema", because a qualifier could only ever have
        // named the catalog views. A user namespace is now a qualifier
        // too, so a name that resolves falls through to the compiler,
        // which binds the relation and checks the qualifier against it
        // (`step_compiler.cpp`, one site for FROM, JOIN and every
        // subquery). Only a namespace that does not exist is refused here,
        // and it is refused by name.
        const std::optional<txn::ReadView> schema_view = ViewFor(session);
        auto ns = catalog_.FindNamespaceOidByName(
            stmt.from.schema, schema_view.has_value() ? &*schema_view : nullptr);
        if (!ns.ok()) {
            if (ns.status().code() != StatusCode::kNotFound) {
                return {ErrorReply(ns.status()), false, 0, ns.status()};
            }
            return {"ERR no namespace named '" + stmt.from.schema + "' (byte " +
                        std::to_string(stmt.from.byte_offset) +
                        "); the catalog views are under `sys`",
                    false};
        }
    }

    // V17: parse -> compile -> execute. Everything that used to be
    // decided here - which relation, which access path, where each
    // predicate is evaluated - was settled by the compiler and is sitting
    // in the chain. What is left is formatting.
    const std::optional<txn::ReadView> resolve_view = ViewFor(session);
    // Opened before the compile, which declares into it at every bind
    // (`step_compiler.hpp`'s `declare`; AT-R1). Lives to the end of the
    // statement, which is the end of this function.
    ReadBorrow borrow(locks_, NextReadHolder(), &read_borrows_);
    auto chain = exec::Compile(catalog_, stmt,
                               resolve_view.has_value() ? &*resolve_view : nullptr, &borrow);
    if (!chain.ok()) {
        return {ErrorReply(chain.status()), false, 0, chain.status()};
    }

    // **Every read runs where the session is, walking every range** (AT-S9,
    // AT-0 item 4 answered "struck"). Two routes stood here and both chose
    // a core by ownership: the single-step fan-in sent a split relation's
    // ranges to the cores that owned them, and the two-step pipeline sent a
    // join to its relations' owners. Ownership is gone with `owner_core`
    // (D17), and neither route had a reason left that was not ownership:
    // one frame table serves every core (AM-S2 step 3), so every page is
    // this core's to fault, and a walk here reaches the whole relation
    // (`TableAccess::WalkHeads`).
    //
    // **The two-step route was also wrong**, which is why it goes whole
    // rather than behind a guard. Its stages read under their own
    // autocommit snapshot, so inside `BEGIN` a join could not see the
    // transaction's own uncommitted writes, and outside one its two
    // relations were read at two instants - AT-S6 closed exactly this for
    // the single-step route (`shipped_read_own_write_test.cpp`) and removed
    // the enrolment test that had kept the two-step route off transactions,
    // leaving it open here until the route itself went.

    // Same one-line-per-response contract as SHOW PAGE: a header line of
    // column names, then one "\n"-escaped section per matching row
    // (comma-joined values), never a raw newline byte - which is what
    // `TextResultSink` writes, and what a caller that installed its own
    // sink gets instead.
    //
    // **The description is not emitted here**, though the header line used
    // to be. A fold's output columns are typed by its items and a
    // projection's by its columns, and the fork between them is fifty
    // lines below - so describing here could only be right about the
    // names. Nothing writes to `os` between here and either branch, so the
    // move is invisible to the text form.
    // Where this statement's rows go (`server/result_sink.hpp`): the
    // session's sink when one is installed, and the newline protocol's
    // rendering otherwise. Built here, above every fork below, because the
    // fold, the sorted drain and the plain walk all emit through it - and
    // it *is* the reply buffer, so there is no second one.
    TextResultSink text_sink;
    ResultSink& sink = session.result_sink() != nullptr
                           ? *session.result_sink()
                           : static_cast<ResultSink&>(text_sink);

    // Resolved once, outside the row loop: the projection reads the frame
    // by index, and `SELECT *` means every column of the one step - which
    // the grammar admits only for a single relation (V06).
    const exec::StepChain& compiled = chain.value();

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

    std::optional<stats::InstanceKey> instance;
    // The optimizer's S1 widens this beyond Waystone's shape guard, and the
    // difference is the point: a *scan-only* statement is exactly the shape
    // whose decayed frequency the cabin optimizer's CREATE decision prices
    // (physical-optimizer.md §II.4's f_i), and it is the one shape
    // invariant 9 keeps Waystone away from. The trail and replay reads
    // below still guard on their own switches, so deriving the identity
    // here costs nothing they did not already pay.
    if (waystone_usable || optimizer_signals_ != nullptr) {
        if (auto fingerprint = parser.fingerprint(); fingerprint.has_value()) {
            instance = stats::InstanceKey{fingerprint->pattern_id, fingerprint->arg_hash};
        }
    }

    // The trail a previous execution of this instance recorded. Read once,
    // indexed once, consulted per keyed step.
    //
    // The index is a dispatcher member, reused rather than rebuilt: it is
    // the only allocation on the replay path, and one malloc per SELECT for
    // what is usually one or two entries is the same cost the collector
    // already had to be hoisted to avoid.
    replay_scratch_.Clear();
    const exec::TrailReplay* replay_ptr = nullptr;
    if (replay_enabled_ && instance.has_value()) {
        // Served from the catalog cache, so a pattern nobody has recorded
        // costs a hash lookup and stops here. `has_waystone_directory()` is
        // the authority on whether there is anything to walk (rows.hpp).
        if (auto pattern = catalog_.FindPattern(instance->pattern_id);
            pattern.ok() && pattern.value()->has_waystone_directory()) {
            auto entries = stats::ReadTrail(page_store_, pattern.value()->waystone_root,
                                            pattern.value()->dir_depth, *instance);
            // A trail that cannot be read is a trail that does not exist:
            // the statement descends, exactly as it did before there were
            // trails at all (invariant 8).
            if (entries.ok() && !entries.value().empty()) {
                replay_scratch_.Build(compiled, entries.value());
                if (!replay_scratch_.empty()) replay_ptr = &replay_scratch_;
            }
        }
    }

    // The trail this execution leaves, if anything is recording.
    //
    // **Reused, not constructed per statement.** A collector reserves room
    // for a whole trail (253 x 32 bytes), so building one per SELECT is an
    // 8 KB malloc on the read path - which measured as most of an 18%
    // regression on a point join before this was hoisted onto the
    // dispatcher. Clear() keeps the reservation.
    exec::TrailCollector* trail = nullptr;
    if (recorder_ != nullptr && instance.has_value()) {
        trail_scratch_.Clear();
        trail = &trail_scratch_;
    }

    // ANALYZE runs everything above, deliberately. Its whole contract is
    // that the run it describes is the run that actually happened - same
    // parse, same compile, same executor - and a diagnostic that skipped
    // replay would report descents a real execution does not perform, which
    // is the one thing it must not do.
    if (analyze) {
        return RunAnalyze(compiled, trail, replay_ptr, instance, snapshot.value().snap, borrow);
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
        return RunAggregated(sink, text_sink, compiled, trail, replay_ptr, instance,
                             snapshot.value().snap, borrow);
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
    exec::EmissionQuota quota(compiled);
    sorter_.Reset(compiled, sort_max_rows_);
    exec_stats_.steps.clear();

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

    // **A star read is a projection, resolved once.** It used to be a
    // second arm of the row renderer - a loop over the schema's columns,
    // building the same `{0, 0, i}` reference the projected arm was handed
    // - which meant the two shapes were formatted by two loops that had to
    // stay in step. Resolving the projection here instead leaves exactly
    // one row path below.
    StarDescription star;
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

    if (Status described =
            sink.Describe(DescribeFields(compiled.column_names, types, type_mods, projection));
        !described.ok()) {
        return {"ERR " + described.message(), false, 0, described};
    }

    // Encoding one row of the reply. Shared by the two paths below so the
    // sorted and unsorted replies are formatted by one routine - the bug
    // this shape avoids is a sorted statement rendering a DATE as an epoch
    // day because a second formatter forgot `projection_types`.
    std::string row_scratch;
    Status encode_error = Status::OK();
    auto render = [&](const exec::ChainFrame& frame, std::string& out) {
        // A failure is remembered rather than thrown: the sort path calls
        // this from a place that has already decided to keep the row, and
        // the walk's own sink is the only caller that can end the
        // statement. Checked at both, so an encode failure fails the
        // statement rather than emitting a row the sink refused.
        Status s = sink.EncodeProjectedRow(projection, types, frame, out);
        if (!s.ok() && encode_error.ok()) encode_error = std::move(s);
    };

    Status ran = exec::Execute(
        catalog_, page_store_, compiled,
        [&](const exec::ChainFrame& frame) -> StatusOr<storage::VisitControl> {
            if (sorter_.active()) {
                // **Ask before rendering.** The sort cannot skip or stop -
                // the quota runs after the order exists - but under a
                // `LIMIT` most rows are beaten by the heap's worst retained
                // row and will never be seen, and rendering them is what
                // made top-N bound memory without bounding work.
                auto admitted = sorter_.Admit(frame);
                if (!admitted.ok()) return admitted.status();
                if (admitted.value()) {
                    render(frame, row_scratch);
                    if (!encode_error.ok()) return encode_error;
                    sorter_.Take(row_scratch);
                }
                return storage::VisitControl::kContinue;
            }
            const exec::QuotaVerdict verdict = quota.Note();
            if (verdict == exec::QuotaVerdict::kStop) return storage::VisitControl::kStop;
            if (verdict == exec::QuotaVerdict::kSkip) return storage::VisitControl::kContinue;
            render(frame, row_scratch);
            if (!encode_error.ok()) return encode_error;
            if (Status s = sink.Emit(row_scratch); !s.ok()) return s;
            return verdict == exec::QuotaVerdict::kEmitThenStop
                       ? storage::VisitControl::kStop
                       : storage::VisitControl::kContinue;
        },
        &exec_stats_, budget_, trail, replay_ptr, cabins_, &snapshot.value().snap,
        indexes_enabled_, &borrow);
    if (!ran.ok()) {
        // **No trail on the failure path.** A statement that errored part
        // way through touched some tuples and then stopped; a trail
        // describing that is a trail describing a state no reader should
        // ever be pointed at (workplan P10).
        return {ErrorReply(ran), false, 0, ran};
    }

    // The order exists only now, so the quota is applied here rather than
    // in the sink - and it is the same quota object, so `LIMIT n OFFSET m`
    // still means rows [m, m+n) of the reply the unlimited statement gives.
    // What changed is which reply that is: the sorted one.
    if (sorter_.active()) {
        sorter_.Finish();
        Status drained = Status::OK();
        exec::DrainSorted(quota, sorter_.rows(), [&](const exec::OutputSort::Row& row) {
            if (!drained.ok()) return;
            drained = sink.Emit(row.text);
        });
        if (!drained.ok()) return {ErrorReply(drained), false, 0, drained};
    }

    RecordExecution(instance, trail, compiled, exec_stats_);

    if (logging(LogLevel::kTrace)) {
        log_->Trace("query", "chain of " + std::to_string(compiled.steps.size()) +
                                 " step(s), class " +
                                 std::to_string(static_cast<int>(compiled.klass)));
    }
    return {text_sink.Take(), false};
}

void CommandDispatcher::RecordExecution(const std::optional<stats::InstanceKey>& instance,
                                        exec::TrailCollector* trail,
                                        const exec::StepChain& chain,
                                        const exec::ExecStats& stats) {
    RecordTrail(instance, trail, chain);
    RecordAccessShapes(chain);
    RecordOptimizerSignals(instance, chain, stats);
}

void CommandDispatcher::RecordAccessShapes(const exec::StepChain& chain) {
    // Unconditional on a successful SELECT, and independent of Waystone:
    // this is the physical optimizer's input (docs/spec/heap-and-tuple.md §7),
    // not a trail, and it is collected whether or not anything is recording
    // or replaying one.
    // **The instance's one switch decides it on every core** (AT-S7). A
    // peer was constructed with recording off and turned on by being handed
    // a batch, because it had nowhere else to put a shape; it writes the
    // relation itself now, so `access_statistics` means the same thing
    // wherever the statement ran.
    if (!access_stats_enabled_) return;
    stats::RecordChainAccess(catalog_, chain, static_cast<std::uint64_t>(NowNs()),
                             &access_counters_);
}

void CommandDispatcher::NoteCabinWrite(const catalog::TableAccess& access,
                                        std::span<const parser::AstValue> values,
                                        std::uint16_t first_col_pos, std::uint64_t pk,
                                        PageId page_id, std::uint16_t slot,
                                        std::span<const parser::AstValue> previous) {
    // The two tests a relation with no Cabin pays, and nothing else.
    if (cabins_ == nullptr || access.cabin_mask == 0) return;

    // Filled on the first entry actually appended (see below).
    std::optional<std::uint32_t> write_epoch;

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
            if (auto stale = stats::MakeCabinKey(access.CabinOn(col).id, values[at]);
                stale.has_value()) {
                cabins_->Unobserve(*stale);
            }
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

        stats::CabinEntry entry;
        entry.pk = pk;
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
        cabins_->NoteWrite(*key, entry);
    }
}

void CommandDispatcher::RecordTrail(const std::optional<stats::InstanceKey>& instance,
                                     exec::TrailCollector* trail,
                                     const exec::StepChain& chain) {
    // Never on the failure path - both callers reach here only after the
    // execution succeeded - and only when something was actually located: a
    // collector that gathered nothing describes no trail, and writing an
    // empty one would replace a populated trail with nothing.
    if (recorder_ == nullptr || trail == nullptr || trail->empty()) return;
    if (!instance.has_value()) return;
    recorder_->OnPatternResult(*instance, *trail, exec::StoredStatementClass(chain.klass));
}

void CommandDispatcher::RecordOptimizerSignals(const std::optional<stats::InstanceKey>& instance,
                                               const exec::StepChain& chain,
                                               const exec::ExecStats& stats) {
    // Success path only, like its two siblings, and only for a statement
    // with an identity - the fingerprint is the key the cost-benefit model
    // aggregates by, so a statement without one has nowhere to be counted.
    if (optimizer_signals_ == nullptr || !instance.has_value()) return;

    // The shape's cabin candidacy (§II.4's Σ_i linkage): a kCabinProbe step
    // names its Cabin outright; otherwise the first kFilterScan's filtered
    // column is the column a Cabin *would* serve - the shape whose decayed
    // frequency prices a CREATE. A chain with neither has no candidacy and
    // is recorded as pure heat.
    stats::CandidateRef candidate;
    for (const exec::Step& step : chain.steps) {
        if (step.kind == exec::AccessKind::kCabinProbe && step.cabin.has_value()) {
            candidate.rel_oid = step.rel_oid;
            candidate.col_pos = step.cabin->col_pos;
            candidate.cabin_id = step.cabin->cabin_id;
            break;
        }
        if (step.kind == exec::AccessKind::kFilterScan && !candidate.valid()) {
            // The lowest filtered non-pk column: a single-column equality's
            // mask has one bit, and a multi-column residual's lowest is a
            // deterministic pick rather than a claim of primacy.
            const std::uint64_t non_pk = step.filter_columns & ~std::uint64_t{1};
            if (non_pk != 0 && step.filter_columns != exec::Step::kAllColumns) {
                candidate.rel_oid = step.rel_oid;
                candidate.col_pos = static_cast<std::uint16_t>(std::countr_zero(non_pk));
            }
        }
    }
    optimizer_signals_->NoteExecution(instance->pattern_id, stats.Total().pages_fetched,
                                      candidate);
}

DispatchOutcome CommandDispatcher::HandleUpdate(std::string_view line, Session& session) {
    WriteScope scope;
    txn::Snapshot snap;
    WalkCursor resume_from;

    // **AO-S3b: a resume re-enters the scope and the view it already had.**
    // Not `BeginWrite` and not `SnapshotFor`: the rows this statement has
    // already written live in that transaction, and a second snapshot would
    // read the rows it has yet to reach under a different view than the
    // ones it wrote - the same statement seeing two states of the database,
    // which is what "under one snapshot" in AO-S3b's cell rules out.
    if (session.parked_write().has_value() && !session.parked_write()->is_delete) {
        const Session::ParkedWrite& parked = *session.parked_write();
        scope = WriteScope{parked.txn, parked.owned, &session};
        snap = parked.snapshot;
        resume_from = parked.cursor;
        // R6-5's mark belongs to the *statement*, and the resume is the
        // same statement - so it is restored rather than re-taken, which
        // would say this statement had written nothing.
        statement_trail_mark_ = parked.trail_mark;
        session.clear_parked_write();
    } else {
        auto opened = BeginWrite(session);
        if (!opened.ok()) return {ErrorReply(opened.status()), false, 0, opened.status()};
        scope = opened.value();

        // The read view this UPDATE filters through. An UPDATE reads before
        // it writes, and it must not see a row a SELECT in the same
        // transaction would not - so it takes the snapshot the same way.
        auto snapshot = SnapshotFor(session);
        // `ErrorReply`, not a bare "ERR ": `SnapshotFor` can refuse (a
        // spent transaction-id lease on a peer did, until AT-S10b), and the
        // wire's `retryable=1` is what a client's retry loop reads. The
        // DELETE site has always rendered it this way; these two did not,
        // so the same refusal carried the bit on one verb and lost it on
        // the other (the SS2 review's cut 2).
        if (!snapshot.ok()) return {ErrorReply(snapshot.status()), false, 0, snapshot.status()};
        snap = snapshot.value().snap;
    }

    DispatchOutcome out = UpdateInner(line, scope, snap, resume_from);

    // **AO-S3b: parked in the middle of the walk - the scope stays open.**
    // Every other pending arm below ends its scope, because the statement
    // it parks has written nothing and will run whole on the resume. This
    // one has written rows, and there is no savepoint to unwind them to,
    // so what parks is the statement *in progress*: the transaction, the
    // view and the position are handed to the session and picked up by the
    // branch at the top of this function when `DispatchAsync` runs the
    // statement again.
    if (out.parked_mid_walk) {
        session.set_parked_write(Session::ParkedWrite{scope.txn, scope.owned, snap,
                                                      out.walk_cursor, statement_trail_mark_,
                                                      /*is_delete=*/false});
        return out;
    }


    const bool failed = out.response.rfind("ERR ", 0) == 0;
    Status verdict = failed ? Status::InvalidArgument(out.response) : Status::OK();
    if (Status s = EndWrite(session, scope, verdict); !s.ok() && !failed) {
        return {ErrorReply(s), false, 0, s};
    }
    return out;
}

DispatchOutcome CommandDispatcher::UpdateInner(std::string_view line, WriteScope& scope,
                                               const txn::Snapshot& snapshot,
                                               WalkCursor resume_from) {
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
    // H6 step 2: the parse leg. One of `observability.md` §10's three
    // request-level spans, and the cheapest to attribute wrongly - a
    // statement that is slow to *parse* looks identical from outside to
    // one that is slow to run.
    auto parsed = [&] {
        stats::SpanScope span(trace_, stats::Layer::kParse);
        return parser::Parse(line);
    }();
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
    const std::optional<txn::ReadView> view =
        scope.session != nullptr ? ViewFor(*scope.session) : std::nullopt;
    auto oid =
        catalog_.FindTableOidByName(stmt.table_name, view.has_value() ? &*view : nullptr);
    if (!oid.ok()) {
        return {ErrorReply(oid.status()), false, 0, oid.status()};
    }
    if (Status s = catalog_.CheckRelationQualifier(stmt.schema, stmt.table_name, oid.value(),
                                                   stmt.table_byte_offset,
                                                   view.has_value() ? &*view : nullptr);
        !s.ok()) {
        return {ErrorReply(s), false, 0, s};
    }

    ReadBorrow borrow(locks_, NextReadHolder(), &read_borrows_, oid.value());  // AT-R1

    auto access = catalog_.InitTableAccess(oid.value());
    if (!access.ok()) {
        return {ErrorReply(access.status()), false, 0, access.status()};
    }
    // Borrowed from the catalog's cache, not owned: valid for this
    // statement, including across AllocateRowId() (catalog.hpp).
    const catalog::TableAccess& ta = *access.value();

    // **No destination and no multi-owner refusal, since AT-S9.** R4/IS4
    // resolved the range owner a predicate-shaped write belonged on, and
    // refused one that would span several owners rather than half-apply it
    // - the refusal went stale when `VisitRelation` began walking every
    // range, and both went with ownership. This write walks every range
    // here, on the core its session is on.
    if (Status admitted = CheckWriteAdmission(ta); !admitted.ok()) {
        return {ErrorReply(admitted), false, 0, admitted};
    }

    // Resolve the SET list before touching storage, so a bad target fails
    // clean with no partial update. Both refusals - an unknown column, and
    // the primary key (K2, `Unsupported`) - live in the compiler beside
    // CompileWhere rather than here: the two halves of an UPDATE's compile
    // belong at one layer, and a check the dispatcher owns is one a second
    // write path can be written without.
    if (Status s = exec::CompileAssignments(ta, stmt.assignments); !s.ok()) {
        return {ErrorReply(s), false, 0, s};
    }

    // The WHERE clause compiles to the same resolved predicates a chain
    // step carries, and is evaluated by the same evaluator (V16). UPDATE
    // reads one relation, so the frame has one step.
    auto predicates =
        exec::CompileWhere(catalog_, ta, stmt.table_name, stmt.where, /*view=*/nullptr, &borrow);
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

    // ---- Which foreign keys this SET list touches (§2) -------------------
    //
    // Resolved once, here: an UPDATE that changes no fk column must cost
    // nothing, and asking per row would mean a name comparison per row per
    // foreign key to answer a question about the *statement*.
    std::vector<std::pair<const catalog::ForeignKeyRef*, const parser::AstValue*>> fk_assignments;
    for (const catalog::ForeignKeyRef& fk : ta.fkeys_out) {
        if (fk.column_no >= ta.schema.columns.size()) continue;
        const std::string_view column = catalog::NameView(ta.schema.columns[fk.column_no].name);
        for (const auto& assignment : stmt.assignments) {
            if (assignment.col_name != column) continue;
            fk_assignments.emplace_back(&fk, &assignment.val);
            break;
        }
    }

    // Minted once per statement rather than per row. The argument that
    // stood here - a write to this relation could only come from the core
    // that owned it, which was this one (crosscore.md CC3) - is false since
    // AT-S5: another core's writer can join or leave the live set while
    // this statement runs, and a parent deleted after the check is the
    // forward check's open interval `known-gaps.md` (Foreign keys) records.
    txn::ReadView check_view = txn::ReadView::Everything();
    exec::FkParentVerdicts fk_held;
    if (!fk_assignments.empty()) {
        check_view = CheckView(scope);

        // ---- The extraction pass (foreign-keys.md §2a, AH-R1) -----------
        //
        // A SET assigns a literal, so the parent set is a property of the
        // *statement* and is resolved here, once, before any row work -
        // which is the comment the per-row check below used to carry as a
        // regret ("this repeats one descent per row where one would do").
        //
        // **Resolving is not failing, and that distinction is what keeps
        // this byte-identical.** An UPDATE matching zero rows must still
        // answer `UPDATED 0` even when its SET names a parent that does not
        // exist, because today's check never runs when no row qualifies.
        // So the verdict is *held* here and *applied* per matched row: an
        // unmatched statement resolves a parent, uses nothing, and reports
        // what it always reported.
        for (const auto& [fk, value] : fk_assignments) {
            std::vector<parser::AstValue> one(ta.schema.columns.size() > 0
                                                  ? ta.schema.columns.size() - 1
                                                  : 0);
            if (fk->column_no == 0 || fk->column_no > one.size()) continue;
            one[fk->column_no - 1] = *value;
            if (Status s = ResolveForeignKeyParents(ta, one, check_view, fk_held, scope.txn); !s.ok()) {
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

    // **The unit this write declares** (AO-0 item 14, marked 2026-09-08),
    // read off the predicate before a row is touched and taken once. A
    // coarse declaration is what keeps a bulk write inside
    // `max_locks_per_txn`; the declaration's own definition carries the
    // argument for why choosing coarsely up front is not the escalation
    // the mark forbids.
    //
    // **A declaration the table refused is waited for, not demoted**
    // (AO-S6c-c). Falling back to per-row was right while the borrow was
    // advisory: it restored a ledger that would otherwise have recorded
    // nothing at all. With the lock as the wait it is the worse of the two,
    // and by a clear margin - a demoted statement writes the rows that do
    // not conflict and then parks on one that does, which AO-S3b's
    // re-runnability rule turns into a refusal, while waiting on the coarse
    // unit parks having written nothing and re-runs cleanly.
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
        auto id = exec::RowKeystoneId(payload_copy);
        if (!id.ok()) return id.status();
        const std::uint64_t trx_id = tuple.value().trx_id;
        const std::uint64_t undo_ptr = tuple.value().undo_ptr;

        // ---- Stage 1: only what the WHERE reads (OPT-001) ---------------
        //
        // A statement that writes one row of two thousand used to decode
        // all two thousand, twice over, before testing anything - the same
        // defect AP01 measured at 75% of a SELECT's scan, left standing on
        // the write path. `filter_mask` is the compiler's, hoisted to the
        // statement because that is what it is a fact about; it is
        // kAllColumns whenever a sub-chain or a >64-column relation makes a
        // partial decode unsound (step_compiler.cpp's CompileWhere).
        if (Status s = exec::DecodeAndResolve(page_store_, ta.schema, ta.layout, payload_copy,
                                              frame.SlotsFor(0), filter_mask, frame_spills);
            !s.ok()) {
            return s;
        }
        auto matched = exec::EvaluateConjuncts(catalog_, page_store_, schemas, predicates.value(),
                                               frame, /*stats=*/nullptr, budget_, &snapshot);
        if (!matched.ok()) return matched.status();
        if (!matched.value()) return Status::OK();

        // ---- Stage 2: the whole row, for a row that qualified -----------
        //
        // An owned copy, because UPDATE re-encodes it and hands it to the
        // write hook; the frame is for evaluation only and is not read
        // again below. Decoded from `payload_copy`, whose bytes outlive
        // every page fetch stage 1 may have made.
        auto row = exec::DecodeRow(ta.schema, ta.layout, payload_copy, &spills);
        if (!row.ok()) return row.status();
        if (Status s = exec::ResolveSpills(page_store_, spills, row.value()); !s.ok()) return s;

        // ---- First-updater-wins (docs/spec/txn.md section 5) ----------------
        //
        // Checked only once the row has qualified: a conflict is reported
        // about a row this statement actually wanted, never about one it
        // scanned past. No lock and no wait - the verdict is a pure
        // function of the tuple's current writer and this view.
        if (scope.txn != nullptr) {
            // **The borrow, before the conflict is decided** (AO-S6a) -
            // the operator's choice of order over reviving AO-R4's
            // disjunction.
            //
            // **And it is before the header matters, which AO-S6b settled
            // and this comment used to deny.** It said the grant could only
            // become the authority at the cost of a re-read after it or the
            // borrow moved above `ReadTuple`. Neither is owed: the borrow's
            // key *is* the row's pk and the pk comes only from the tuple,
            // so moving it above the read is impossible in principle; and a
            // re-read would be redundant, because this whole walk runs
            // inside a live write `PageRef` for the page and carries no
            // `co_await`, so no peer core can write it and no coroutine on
            // this core can interleave. The captured `trx_id` cannot go
            // stale between the read and the grant.
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
                // **AO-S3b: a conflict a wait can get past stops the walk
                // rather than failing the statement.** `blocking_writer_`
                // is the discriminator `CheckWriteConflictBlocking` already
                // sets, and it is set only under `may_park_` with a live
                // clock - so one naming *this row's* writer is a wait
                // something can act on. Every other conflict is the refusal
                // it always was. Returning OK is what lets the caller turn
                // this into a `kStop`: the row is not written, and the walk
                // ends holding no span.
                //
                // **Compared against `trx_id`, not merely tested non-zero.**
                // The member is the statement's, not the row's: the forward
                // foreign-key check records a busy *parent's* holder into it
                // before the walk starts (`ResolveForeignKeyParents`). A
                // bare non-zero test would then read that leftover as this
                // row's verdict and park the statement on a transaction that
                // holds nothing it wants - waiting for the wrong decide and
                // resuming into the same refusal. `NoteBlockingWriter` is
                // called here with this row's own `blocker`, so equality is
                // exactly "this row's check is the one that recorded it".
                //
                // **Against `blocker` rather than `trx_id` since AO-S6c-b**:
                // the two name the same transaction for a row an in-flight
                // writer holds, but only the lock can name a holder of a
                // *range* covering this key - and testing the header there
                // would compare a leftover against a holder the header does
                // not know, refusing a wait the table had already offered.
                // This is not the narrower test it looks like:
                // `CheckWriteConflictBlocking` writes the holder it
                // recorded back into `blocker`, which is `trx_id` itself
                // wherever the table named nobody.
                if (blocker != 0 && blocking_writer_ == blocker) {
                    parked_on_row = true;
                    blocked_verdict = s;
                    return Status::OK();
                }
                return s;
            }
        }

        // ---- The forward check, for an fk column this SET touches (§2) --
        //
        // Per matched row, after the row has qualified and the cheap
        // conflict check has passed - but **the descent is no longer here**
        // (§2a, AH-T1). The value is statement-constant, so the parent was
        // resolved once above; what remains is applying the held verdict to
        // this row, which is what keeps an UPDATE matching zero rows
        // answering `UPDATED 0` rather than a violation it never used to
        // report.
        for (const auto& [fk, value] : fk_assignments) {
            if (Status s = CheckForeignKeyOnWrite(ta, *fk, *value, check_view, fk_held); !s.ok()) {
                return s;
            }
        }

        // The row as it stands *before* the SET list is applied, kept only
        // when this relation has a Cabin - it is what tells the write hook
        // whether a key column actually moved (§5's third row). A relation
        // with no Cabin copies nothing.
        // ...and what tells the index hook the same thing (index.md §2).
        // A relation with neither copies nothing.
        //
        // **Asked once, and the answer used at both sites below** (the
        // AT-S5d review). The registry is the instance's, so an assertion
        // adopted on another core between two asks would send the check an
        // empty `previous` - a read past the end of it.
        const bool asserted = enforcer_->AnyOn(ta.oid);
        std::vector<parser::AstValue> previous;
        if ((cabins_ != nullptr && ta.cabin_mask != 0) || !ta.indexes.empty() || asserted) {
            previous = row.value();
        }

        for (const auto& assignment : stmt.assignments) {
            for (std::size_t c = 0; c < ta.schema.columns.size(); ++c) {
                if (catalog::NameView(ta.schema.columns[c].name) == assignment.col_name) {
                    row.value()[c] = assignment.val;
                    break;
                }
            }
        }

        // The pk is unchanged by construction (rejected above), so it was
        // carried straight from the tuple's own Keystone word above rather
        // than round-tripped through the decoded row.
        const std::vector<parser::AstValue> body(row.value().begin() + 1, row.value().end());

        // ---- The assertion check, §4.2's delta rules --------------------
        //
        // Before the undo record and the overwrite, so a refused row is a
        // row nothing touched. `previous` holds the old values whenever an
        // assertion lives on this relation (the condition above); a refusal
        // mid-statement leaves earlier rows written and the transaction
        // poisoned - the AS9 resolution, decided 2026-08-09: uniform with
        // every other write failure, because "open and usable" cannot be
        // promised once a multi-row statement has partly happened.
        if (asserted) {
            std::uint64_t reserver = 0;
            if (Status s = enforcer_->AdmitAndReserveUpdate(page_store_, wal_, WriterId(scope),
                                                           ta.oid, previous, row.value(),
                                                           id.value(), page_id, slot, &reserver);
                !s.ok()) {
                // AO-S6e-c, and the INSERT site states the argument. The pk
                // *is* known here, so the wait names the row it is for.
                // **`pk = 0` here too** (the review's B4). The first draft
                // passed the row's id because it had one, and the message
                // then read "row id=N is held by transaction M" about a
                // holder that may never have touched row N - it holds a
                // *group* reservation. The `INSERT` arm went to the length
                // of a sentinel to avoid exactly that sentence, and
                // `assertion.md` §6.2 states the rule for both arms.
                if (reserver != 0) {
                    NoteBlockingWriter(scope.txn, reserver, /*pk=*/0,
                                       RepeatableReadWait::kCapable);
                }
                return s;
            }
        }

        // The spills this re-encode appended, collected so they can be logged
        // below. **They were not collected before, and so not logged at all**:
        // an UPDATE that spilled a value wrote the bytes into a var-heap page
        // and told the log nothing, leaving a recovered tuple whose cell
        // points at bytes no record describes (`docs/inflight/known-gaps.md`'s var-heap
        // entry, hole 3).
        //
        // Collected when there is a log to write them to **or a transaction
        // to roll them back** - the second half added with VC-B3, because a
        // live Abort needs the trail entry whether or not anything is
        // logged, and an unlogged dispatcher that dropped the collector
        // would leak every value a rolled-back UPDATE spilled.
        //
        // `appended_spills`, not `spills`: the enclosing scope already has a
        // `PendingSpill` list, which is the opposite direction - values this
        // statement *read* out of the var-heap to evaluate the WHERE clause.
        std::vector<exec::AppendedSpill> appended_spills;
        const bool collect_spills = wal_ != nullptr || scope.txn != nullptr;
        auto encoded = exec::EncodeRow(
            ta.schema, ta.layout, id.value(), body,
            exec::VarHeapSink{&page_store_, ta.varheap_page_id,
                              collect_spills ? &appended_spills : nullptr, ta.oid});
        if (!encoded.ok()) return encoded.status();

        // HOT-style in-place overwrite - see PageView::OverwriteTuple's
        // comment. There is no retire+reinsert fallback because there is
        // nothing to fall back *from*: under the fixed-length rule the new
        // payload is exactly the same size as the old one, since a row's
        // size is a schema constant and not a function of its values
        // (invariant 13). This used to be able to fail with OutOfSpace when
        // a varchar grew past its slot's reservation.
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
            // The bytes as they stand, re-read rather than reconstructed:
            // `row` has already had the SET list applied, and an image
            // built from it would restore the *new* values.
            auto live = page_store_.GetForRead(page_id);
            if (!live.ok()) return live.status();
            heap::PageView before_page(live.value().bytes());
            auto before = before_page.ReadTuple(slot);
            if (!before.ok()) return before.status();
            const std::vector<std::byte> image(before.value().payload.begin(),
                                               before.value().payload.end());

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
        // `row.value()` now holds the **new** values, so this appends the pk
        // to v′'s set for every cabined column. The old value's set is
        // deliberately left alone: a pre-update snapshot is still entitled
        // to match through it, and for newer readers the stale entry is a
        // surplus the read-time key re-check subtracts. Removal here would
        // be incorrect, not an optimization forgone.
        //
        // The location is unchanged by construction - an UPDATE is an
        // in-place overwrite under invariant 13 - so the appended hint is
        // the row's real address.
        NoteCabinWrite(ta, row.value(), /*first_col_pos=*/0, id.value(), page_id, slot, previous);

        // The index half. `previous` is what makes §2's rule work: an UPDATE
        // that moved no key and no covered column appends nothing, which is
        // what keeps an index from growing by an entry per write forever.
        std::vector<exec::IndexWrite> index_writes;
        if (Status s = exec::MaintainIndexes(catalog_, page_store_, ta, row.value(),
                                              /*first_col_pos=*/0, encoded.value(), id.value(),
                                              previous,
                                              wal_ != nullptr ? &index_writes : nullptr);
            !s.ok()) {
            return s;
        }

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
            if (Status s = LogIndexWrites(index_writes, scope.txn->id()); !s.ok()) return s;

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
        if (page_id != last_page) {
            last_page = page_id;
            ++pages_touched;
        }
        return Status::OK();
    };

    // Same fast path as SELECT, and the same contract: a point UPDATE by pk
    // is the other statement shape whose cost is otherwise linear in the
    // relation. Any reason to decline falls through to the scan, which
    // produces the identical result - the locator picks the slot to look
    // at, never which rows match.
    if (std::optional<std::uint64_t> pk = PkEqualityTarget(ta, stmt.where); pk.has_value()) {
        PkLookup found = LocateByPk(ta, *pk, storage::PageAccess::kWrite);
        if (found.kind == PkLookup::Kind::kAbsent) {
            return {"UPDATED 0", false, 0};  // no such row, on the tree's authority
        }
        if (found.kind == PkLookup::Kind::kAt) {
            // Through the leaf the descent holds exclusive and dirty, never a
            // re-fetch: a shared lookup then `Get()` let a divide on another
            // core renumber the slot in between, and `apply` found another
            // row, declined it on the WHERE, and answered `UPDATED 0` for a
            // row that exists (AT-0 item 12).
            {
                heap::PageView page(found.at.leaf.bytes());
                if (Status s = apply(found.at.page_id, page, found.at.slot); !s.ok()) {
                    return {ErrorReply(s), false, 0, s};
                }
                // **AO-S3b: `apply` now answers OK for a row it declined to
                // write**, so the count below is no longer proof that it
                // did. Without this test a point UPDATE against a held row
                // renders `UPDATED 0` - a success reply for a write that
                // never happened, and an autocommit commit on top of it.
                // There is exactly one row in scope here, so nothing was
                // written and nothing can be resumed from: the conflict is
                // the answer, and `EndWrite` keeps the blocker that makes
                // `DispatchAsync` wait for the holder and run the whole
                // statement again - the pre-AO-S3b behaviour of this arm.
                if (parked_on_row) {
                    return {ErrorReply(blocked_verdict), false, 0, blocked_verdict};
                }
                if (logging(LogLevel::kTrace)) {
                    log_->Trace("query", "pk " + std::to_string(*pk) + " updated at " +
                                             std::to_string(found.at.page_id) + ":" +
                                             std::to_string(found.at.slot));
                }
                return {"UPDATED " + std::to_string(updated), false, updated};
            }
        }
    }

    // AO-S3b's resume position. A first walk starts inactive, which is
    // "from the head"; a resumed statement is handed the one its park left.
    WalkCursor walk_cursor = resume_from;
    Status scan = VisitRelation(
        ta, storage::PageAccess::kWrite,
        [&](PageId page_id, heap::PageView& page,
            std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            // UPDATE has no early exit *of its own*: it must consider every
            // row, since the WHERE is evaluated per tuple and any of them
            // may match. The one stop it can make is AO-S3b's - a row held
            // by a writer that has not decided, where the walk stops so the
            // page span is released and the statement above can park.
            if (Status s = apply(page_id, page, slot); !s.ok()) return s;
            if (parked_on_row) return storage::VisitControl::kStop;
            return storage::VisitControl::kContinue;
        },
        // R4/IS4: the pk window this statement can touch.
        WriteWalkSpan(ta, stmt.where),
        &walk_cursor);
    if (!scan.ok()) {
        // Partial **within the statement**, which is section 6's stated
        // rule rather than an exposure now. In autocommit EndWrite() aborts
        // this scope, so the rows already updated are compensated and the
        // statement is atomic after all. Inside an explicit transaction
        // they stay written and the session is poisoned - the client must
        // ROLLBACK, which undoes all of them.
        return {ErrorReply(scan), false, 0, scan};
    }

    // **AO-S3b: the walk stopped on a held row, so this is not an answer.**
    // Reported before the count is rendered, because `updated` here is the
    // number of rows written *so far* and returning it would be a silent
    // partial UPDATE - the one outcome this stage must never produce.
    if (parked_on_row) {
        // **Only a statement that has written rows resumes.** One that has
        // not is answered with its conflict and re-run whole by
        // `DispatchAsync`, which is AO-S3's shape and is *better* here
        // rather than merely older: the re-run mints a fresh snapshot, so
        // a holder that **committed** while it waited is visible to it and
        // the write goes through against the new version. A resume carries
        // the statement's original view by construction - it has to, or
        // the rows it already wrote and the rows it has yet to reach would
        // be read under two views - and under that view a committed holder
        // is invisible, so the wait would end in the same refusal it
        // started with. Resuming buys something only once there are rows
        // that cannot be re-applied, which is exactly this test.
        //
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
            // of the statement or the next clause keyword. Splitting it as a
            // token was this loop's first shape and it refused every level
            // this engine actually has.
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
    // site a retryable refusal has to reach the wire from. (A spent
    // transaction-id lease on a peer was that refusal until AT-S10b.)
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

    // **One commit path since AT-S6.** D1's fast path forked here on
    // whether the transaction had enrolled a participant, and took D4's
    // two phases when it had. A transaction has no half on another core
    // now - nothing ships and nothing enrols - so what R6 called "the
    // one-owner path pays nothing" is the only path there is.
    return CommitLocal(session);
}

DispatchOutcome CommandDispatcher::CommitLocal(Session& session, wal::Lsn* commit_lsn) {
    txn::Transaction* txn = session.transaction();
    const std::uint64_t id = txn->id();

    // The transaction's reservations become committed entries (§6.2 step
    // 4): flags cleared, ASSERT_COMMIT logged, before the commit record. A
    // failure leaves the transaction open - the client may retry COMMIT or
    // ROLLBACK, and the pending set is untouched until one succeeds.
    if (Status s = enforcer_->CommitTxn(page_store_, wal_, id); !s.ok()) {
        return {ErrorReply(s), false, 0, s};
    }
    auto committed = txn_->Commit(*txn, effective_durability_);

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

    // Its catalog rows are committed now, so every reader may see them
    // unfiltered again (DT3c). Before `Finish()`, which clears the
    // session's transaction pointer this reads.
    EndDdlScope(session);
    session.Finish();

    // The durability wait the client is owed, for the same reason
    // LogInsert() takes it: kGroup staged the commit for the next drain,
    // and the acknowledgement means "durable".
    //
    // `CommitAck::kAtAppend` - a cross-owner participant applying a decide -
    // has had no caller since AT-S6 (`CommitAck`). What belongs here is why
    // there is no second branch: this is a **D2 site by construction**, because `kStrict` synced inside `Commit` before it
    // returned and `kRelaxed` stages nothing, so neither class reaches this
    // statement at all and neither can be changed by the flag.
    if (wal_ != nullptr && effective_durability_ == wal::DurabilityClass::kGroup &&
        commit_ack_ == CommitAck::kWhenDurable && !wal_->IsDurable(committed.value())) {
        pending_commit_lsn_ = committed.value();
    }
    // The record's LSN whatever the class, for a caller that needs the
    // *decision* durable rather than the acknowledgement honest (R6-3) -
    // the coordinator, which went with 2PC at AT-S6; no caller passes one.
    if (commit_lsn != nullptr) *commit_lsn = committed.value();
    txn_->Release(*txn);
    // **And on the outcome too** (XF4), for the cross-owner *participant*
    // that reached this through `DispatchAsync("COMMIT")` with no
    // out-parameter and timed its own record's durability; it went with
    // 2PC at AT-S6 and nothing reads the field now. `DispatchOutcome`'s
    // header says why this is not `pending_lsn`.
    DispatchOutcome committed_out{"COMMIT trx_id=" + std::to_string(id), false};
    committed_out.commit_lsn = committed.value();
    return committed_out;
}

DispatchOutcome CommandDispatcher::HandleRollback(Session& session) {
    if (!session.in_explicit_txn()) {
        return {"ERR no transaction is open", false};
    }

    DispatchOutcome out = RollbackLocal(session);

    // **Nothing follows the local half since AT-S6.** A rollback used to
    // tell its participants - `AbortAndForget`, with nobody waiting,
    // because the outcome is abort whatever they answer and the
    // connection-close rollback runs on a path that cannot park. There are
    // no participants: nothing ships, so nothing enrols.

    return out;
}

DispatchOutcome CommandDispatcher::RollbackLocal(Session& session) {
    txn::Transaction* txn = session.transaction();
    const std::uint64_t id = txn->id();

    // The reservations first (§6.2 step 5): each one removed from its
    // group, ASSERT_ROLLBACK logged, before the undo trail replays - so the
    // directory and the pages unwind in the same statement the rows do.
    if (Status s = enforcer_->AbortTxn(page_store_, wal_, id); !s.ok()) {
        return {ErrorReply(s), false, 0, s};
    }
    Status aborted = txn_->Abort(*txn, RowLocatorForRollback());
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
    // **AO-S3b: the wait ended badly and the statement is still holding a
    // scope open.** A statement parked mid-walk left its transaction, its
    // view and its position on the session so the resume could pick them
    // up; a refusal means there is no resume, so the scope has to be ended
    // here or it is never ended at all - an autocommit transaction never
    // released, and the *next* statement on this session resuming into it.
    // Ended through `EndWrite` rather than by hand so that both arms are
    // the ones every other failed statement takes: an owned scope aborts
    // and releases, an explicit transaction keeps its rows and poisons.
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
                                           std::uint64_t pk, RepeatableReadWait rerun) {
    if (!may_park_ || clock_ == nullptr || txn_ == nullptr) return;
    // A relation-level refusal already parked this statement on the table's
    // slot (`BorrowChain`); a second wait on the same statement would be
    // one the outcome cannot carry.
    if (lock_wait_.has_value()) return;
    if (!txn_->IsInFlight(trx)) return;
    // The two guards the declaration argues for. A null `waiter` is
    // autocommit before its transaction is opened: it holds nothing and
    // will mint a fresh view, so both tests pass vacuously.
    if (waiter != nullptr) {
        // **No cycle can form while every waiter holds nothing** - AO-S3's
        // guard, and it is lifted exactly where a detector exists. With a
        // lock table this core records a `waiter -> holder` edge before it
        // parks and refuses the wait outright when that edge would close a
        // cycle (AO-S4a), so a transaction holding rows may wait; without
        // one the graph has no reader and the narrow rule is what keeps the
        // stage deadlock-free.
        if (locks_ == nullptr && !waiter->trail().empty()) return;
        // A repeatable-read waiter's re-run is refused because its view
        // cannot see a holder that **commits** after it was minted. The
        // exclusion is conservative rather than exact: a holder that
        // *aborts* restores the row's prior writer id and the same view
        // would then admit the write. Waiting only for the abort arm is a
        // narrower promise than this stage set out to make, so the level
        // stays excluded here.
        //
        // **And here is the whole of it now** (AO-S6d, item 17). Every
        // sentence above is about a holder that wrote the row; a holder of
        // a unit that wrote no version of it commits nothing this view
        // needs to see, so the re-run answers the ordinary MVCC question
        // and the wait pays off in either arm. The one case that made this
        // a decision rather than a correction: the holder may write the row
        // during the wait, and the re-run then meets a header naming it and
        // is refused first-updater-wins - the correct repeatable-read
        // outcome, reached after a wait rather than instead of one, which is
        // PostgreSQL's shape and the operator's ruling of 2026-09-09.
        if (waiter->isolation() == txn::IsolationLevel::kRepeatableRead &&
            rerun == RepeatableReadWait::kFutile) {
            return;
        }
    }
    blocking_writer_ = trx;
    blocked_pk_ = pk;
}

std::optional<Status> CommandDispatcher::BorrowRelationForDdl(txn::Transaction* holder,
                                                              catalog::Oid oid, bool poisons) {
    if (locks_ == nullptr || holder == nullptr) return std::nullopt;

    // AR2 §3's DDL row: **the relation, `X`, for the DDL transaction**. Its
    // one consumer in M2 is the read borrow beneath it (AO-R12): a
    // positioned reader holds the same entry in `IS`, and the intention
    // rule is where the two meet - which is also why no unit below this one
    // is looked at, and why a reader that has not reached this relation
    // yet, or that reads a different one, costs this nothing.
    const txn::LockKey unit = txn::LockKey::Relation(oid);
    std::uint64_t blocker = 0;
    std::shared_ptr<txn::LockWaitSlot> wake;
    // A wake is registered only where there is a reactor to park on - the
    // same condition every other wait in this file records under. Without
    // it the honest answer is the refusal itself, which is what a
    // synchronous `Dispatch()` has always had.
    auto took = locks_->TryAcquire(holder->id(), unit, txn::LockMode::kExclusive,
                                   holder->borrows(), &blocker, may_park_ ? &wake : nullptr);
    if (!took.ok()) return took.status();
    if (took.value()) return std::nullopt;
    if (wake != nullptr) {
        lock_wait_ = DispatchOutcome::LockWait{unit, blocker, std::move(wake), poisons};
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

void CommandDispatcher::TakeLockWait(DispatchOutcome::LockWait wait) {
    // The declaration carries the argument. Here only the order matters:
    // the outgoing registration is dropped **before** the new one is
    // installed, so a throw or an early return cannot leave two live.
    if (lock_wait_.has_value() && locks_ != nullptr) {
        locks_->DropWake(lock_wait_->key, lock_wait_->slot);
    }
    lock_wait_ = std::move(wait);
}

StatusOr<bool> CommandDispatcher::BorrowChain(const WriteScope& scope,
                                             const txn::LockKey& unit,
                                             std::uint64_t* blocker) {
    if (locks_ == nullptr || scope.txn == nullptr) return true;
    txn::LockHoldings& holdings = scope.txn->borrows();
    const std::uint64_t id = scope.txn->id();

    // **The cap refuses the statement** (AO-S6c-c). While the borrow was
    // advisory it was swallowed - failing a statement for the size of a
    // ledger that guarded nothing is a cost with no matching benefit, which
    // is the operator's decision of 2026-09-08 and the condition it
    // attached. That condition has lapsed: since AO-S6c-b a refused borrow
    // refuses the write, and since AO-S6c-c a fence stops an insert, so the
    // ledger is what the engine reads to decide who may write. A truncated
    // one is rows with no fence over them and no wait behind them, silently.
    // AO-R10's rule - a cap refuses and never truncates - takes over, which
    // is what AO-0 item 1's mark said the end state would be.
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
    // refuses this `IX`, and that holder may be on any core: the row-level
    // wait (`NoteBlockingWriter`) polls `IsInFlight`, which is this core's
    // live set, so it would answer "not in flight" for a DDL on a peer and
    // the write would be refused rather than held. A refusal here parks on
    // the table's own slot instead, flipped by the release from whichever
    // core releases - `BorrowRelationForDdl`'s wait, from the other side.
    const txn::LockKey relation = txn::LockKey::Relation(unit.rel_oid);
    const bool first_intention = !holdings.Holds(relation);
    std::shared_ptr<txn::LockWaitSlot> wake;
    auto rel = locks_->TryAcquire(id, relation, txn::LockMode::kIntentionExclusive, holdings,
                                  blocker, may_park_ ? &wake : nullptr);
    if (!rel.ok()) return refused(rel.status());
    if (!rel.value()) {
        if (wake != nullptr) {
            TakeLockWait(DispatchOutcome::LockWait{relation, *blocker, std::move(wake)});
        }
        return false;
    }
    // **The first intention is where the statement's memo has to be
    // current** (the AT-S5e review's C2). The memo was settled at the task
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
            TakeLockWait(DispatchOutcome::LockWait{relation, /*holder=*/0, std::move(flipped)});
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
    auto under = locks_->TryAcquire(id, unit, txn::LockMode::kExclusive, holdings, blocker);
    if (!under.ok()) return refused(under.status());
    return under.value();
}

std::optional<txn::LockKey> CommandDispatcher::DeclaredWriteBorrow(
    const catalog::TableAccess& access, const std::vector<parser::Condition>& where) const {
    if (locks_ == nullptr || access.schema.columns.empty()) return std::nullopt;

    // No predicate at all: the statement touches every row of the
    // relation, so **the whole id space** is what it declares.
    //
    // The relation *unit* is deliberately not what it takes, and the
    // distinction is the one AO-S6e-b had to draw: the relation unit means
    // the relation as an **object** - its existence and its schema, which
    // is what DDL claims and what a positioned reader declares an `IS` on -
    // while a write's claim is over **keys**. Collapsing a `WHERE`-less
    // write onto the relation entry was cheaper (one entry, no fence
    // counter) and made it conflict with a reader's `IS` there, by the
    // intention rule, for rows MVCC already answers that reader from its
    // snapshot. A range over the whole space says the same thing about
    // keys and meets other writers exactly where every other declared
    // window does.
    if (where.empty()) return txn::LockKey::Range(access.oid, 0, catalog::kIdSpaceEnd);

    // Otherwise, the pk window the conjuncts name - if they name one.
    //
    // **The vector is an AND-list**, so a conjunct this loop skips can only
    // remove rows from the set, never add one: the window derived from the
    // pk conjuncts alone is a *superset* of what the statement writes, and
    // a borrow over a superset covers every row written. That is what makes
    // it sound to ignore `WHERE name = 'x'` sitting beside `WHERE id < 50`.
    std::uint64_t lo = 0;
    std::uint64_t hi = catalog::kIdSpaceEnd;
    bool bounded = false;

    for (const parser::Condition& cond : where) {
        if (cond.kind == parser::PredicateKind::kBetween) {
            // The one kind `PkLiteral` deliberately declines, because it
            // carries two bounds rather than one. Its own guards, in the
            // same order and for the same reasons.
            if (cond.rhs_kind != parser::RhsKind::kLiteral) continue;
            if (cond.val.type != parser::ValueType::kInt) continue;
            if (cond.val_high.type != parser::ValueType::kInt) continue;
            if (cond.val.int_val < 0 || cond.val_high.int_val < 0) continue;
            if (!IEquals(cond.col.name,
                         catalog::NameView(access.schema.columns.front().name))) {
                continue;
            }
            // Inclusive at both ends (`ast.hpp`), and `PkSpan` is half-open.
            lo = std::max(lo, static_cast<std::uint64_t>(cond.val.int_val));
            hi = std::min(hi, static_cast<std::uint64_t>(cond.val_high.int_val) + 1);
            bounded = true;
            continue;
        }
        const std::optional<std::uint64_t> v = PkLiteral(access, cond);
        if (!v.has_value()) continue;
        switch (cond.op) {
            case parser::CompareOp::kEq:
                lo = std::max(lo, *v);
                hi = std::min(hi, *v + 1);
                bounded = true;
                break;
            case parser::CompareOp::kGt:
                lo = std::max(lo, *v + 1);
                bounded = true;
                break;
            case parser::CompareOp::kGte:
                lo = std::max(lo, *v);
                bounded = true;
                break;
            case parser::CompareOp::kLt:
                hi = std::min(hi, *v);
                bounded = true;
                break;
            case parser::CompareOp::kLte:
                hi = std::min(hi, *v + 1);
                bounded = true;
                break;
            // `kNeq` names no window, and the two null tests name no id at
            // all. Left per-row rather than widened to the relation: a
            // predicate that does not bound the pk is exactly the shape the
            // mark leaves alone.
            case parser::CompareOp::kNeq:
            case parser::CompareOp::kIsNull:
            case parser::CompareOp::kIsNotNull:
                break;
        }
    }

    if (!bounded) return std::nullopt;
    // An empty window writes nothing, so there is nothing to declare.
    if (lo >= hi) return std::nullopt;
    // **A window of one key stays per-row**, and this is not a
    // micro-optimisation: a tuple `X` is the finer expression of the same
    // hold at the same cost of one entry, and taking a *fence* instead
    // would raise the relation's fence counter - which sends every other
    // writer of that relation through the all-partition scan AO-R3 exists
    // to keep them out of. The commonest OLTP write there is must not pay
    // that, and it is `WHERE id = k`.
    if (hi - lo <= 1) return std::nullopt;
    // `WHERE id >= 0` is the same statement as no `WHERE` at all and
    // borrows the same way - which since AO-S6e-b is the whole id space as
    // a range rather than the relation entry, for the reason the no-`WHERE`
    // branch above states.
    return txn::LockKey::Range(access.oid, lo, hi);
}

Status CommandDispatcher::HeldByHolder(std::uint64_t pk, std::uint64_t holder) {
    // `HolderName`, because a holder is not always a transaction since
    // AO-S6e-b: a read borrow holds under an id of its own, and an operator
    // sent after "transaction 9223372036854775809" is being sent after
    // something that never existed.
    return Status::TxnConflict("row id=" + std::to_string(pk) + " is held by " +
                               HolderName(holder));
}

std::optional<Status> CommandDispatcher::BorrowOrWait(const WriteScope& scope,
                                                      const txn::LockKey& unit,
                                                      RepeatableReadWait rerun) {
    std::uint64_t blocker = 0;
    auto took = BorrowChain(scope, unit, &blocker);
    if (!took.ok()) return took.status();
    if (took.value() || blocker == 0) return std::nullopt;
    // Refused at the relation: parked on the slot `BorrowChain` registered,
    // not on the holder's decide - see there. (A relation unit is only ever
    // refused there; a narrower one refused there on a path that cannot
    // park is named by its rows below, which a DDL holding the relation
    // does hold.)
    //
    // **Tested by the unit the wait names, not by a wait existing**
    // (AT-S5f). `lock_wait_` was `BorrowChain`'s alone when this was
    // written, so its mere presence meant "the relation refused this ask".
    // The forward foreign-key check now records a *parent row's* wait at
    // the dispatch fork, before any borrow is asked for, and a presence
    // test reads that leftover as this relation's refusal: it names the
    // relation where a row or a range was held, and takes the branch that
    // skips the `NoteBlockingWriter` a narrower refusal owes.
    // `BorrowChain` installs a relation unit and nothing else, so the key
    // is exactly the question this line means to ask.
    const bool refused_at_relation = lock_wait_.has_value() &&
                                     lock_wait_->key.unit == txn::LockUnit::kRelation &&
                                     lock_wait_->key.rel_oid == unit.rel_oid;
    if (refused_at_relation || unit.unit == txn::LockUnit::kRelation) {
        return RelationHeld(unit.rel_oid, blocker);
    }

    // **A refused borrow stops the write.** An insert has no conflict check
    // of its own to convert - the row does not exist, so there is no header
    // to judge - which is why AO-S6c-b left both insert paths alone and why
    // a range fence did not stop an insert into its window until AO-S6c-c.
    // Without this, item 14's declared unit guarded its holder against
    // writers of rows that *exist* and not against rows that *appear*,
    // which is the one thing a range fence is for.
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
    NoteBlockingWriter(scope.txn, blocker, unit.lo, rerun);
    if (unit.unit == txn::LockUnit::kTuple) return HeldByHolder(unit.lo, blocker);
    return Status::TxnConflict("rows id=[" + std::to_string(unit.lo) + ", " +
                               std::to_string(unit.hi) + ") are held by " + HolderName(blocker));
}

Status CommandDispatcher::CheckWriteConflictBlocking(const WriteScope& scope, std::uint64_t cur,
                                                     std::uint64_t pk,
                                                     std::uint64_t& blocker) {
    Status verdict = txn_->CheckWriteConflict(*scope.txn, cur, pk);

    // **A refused borrow is a conflict in its own right** (AO-S6c-b), and
    // this is what makes the lock the wait rather than a name attached to
    // somebody else's refusal. The MVCC check answers about the row's own
    // *writer*: it is silent about a transaction that holds a **range**
    // over this key and has not reached this row yet, because the header
    // still names whoever wrote it last and that writer is long visible.
    // So a fence-blocked write used to pass this check and go on to write a
    // row another transaction had already claimed the right to move - the
    // lost update item 14's coarse units exist to prevent.
    //
    // The message follows `CheckWriteConflict`'s, which `txn/manager.cpp`
    // calls part of the wire contract rather than a diagnostic: same shape,
    // and it names the holder rather than the writer because that is who
    // the client is waiting for.
    if (verdict.ok() && blocker != 0) verdict = HeldByHolder(pk, blocker);
    if (verdict.ok()) return verdict;
    // **The row is held by a writer that has not decided, so the statement
    // waits for it instead of refusing** (AO-S3, AO-3 B rows 1 and 2).
    //
    // What this replaced: R6-5 waited only for a transaction *this core had
    // prepared*, on the argument that an ordinary in-flight writer "ends on
    // its own, so the client's retry finds the row free". AR2-A §1's first
    // axis rejects that argument - the client's retry loop is a wait
    // written in the wrong place, spinning a round trip per attempt on a
    // schedule nobody chose - so a holder that has not decided is one thing
    // whether or not it is prepared, and the in-doubt case is subsumed
    // rather than special-cased.
    //
    // Noted, not acted on: this function is inside a page span and a row
    // callback, which is no place to park. `DispatchAsync` is where the
    // wait happens, on the statement, once it is known that nothing was
    // written.
    //
    // **The blocker is not a diagnostic** - it is the one thing that tells
    // `EndWrite` to withhold the poison a failed statement owes an explicit
    // transaction - so recording it where nothing will re-run the statement
    // would leave a client told `ERR` and a transaction still committable,
    // which is the failure atomicity §6 states. `NoteBlockingWriter` is
    // where every condition on recording it lives, including the two that
    // keep this stage deadlock-free and its waits non-futile.
    // **The holder is the table's where the table names one, and the
    // header's where it does not** (AO-S6c-b). The lock names holders the
    // header never could - a transaction holding a *range* over this key,
    // which is what item 14's declared units put in the table - and
    // sourcing the wait from the header alone leaves those refusing
    // instead of waiting, which is the refusal AR2-A §1 measures this
    // milestone by removing.
    //
    // **The table is not read alone, and one of the two reasons has since
    // gone.** A truncated ledger was the first: while the cap was swallowed
    // a transaction past `max_locks_per_txn` kept writing and recorded
    // nothing more, so its later rows had an in-flight writer in the header
    // and no entry to find. AO-S6c-c ended that - the cap refuses the
    // statement now, so no row is ever written without its borrow.
    //
    // The second stands and is why this is still a union: **a dispatcher
    // with no lock table at all**, where `BorrowChain` grants everything
    // vacuously and `blocker` is zero for every row. That is exactly the
    // state AO-S3 waited through, so reading the table alone would not
    // *move* those waits, it would delete them - 7 of the 12 blocked-writer
    // cells failed at once when this was tried, and
    // `WithoutATableTheNarrowGuardIsWhatKeepsTheStageSafe` is the cell that
    // says the configuration still behaves.
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
    NoteBlockingWriter(scope.txn, blocker, pk, rerun);
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
        blocked_pk_ = 0;
    }
    // **And AO-S6e-b's lock wait, on the same test and with one more
    // thing to undo.** The borrow is asked for before the DDL's first
    // catalog write, so a statement that reaches here having written
    // something did not reach the ask - but the test is the shape all three
    // waits share and stating it once per wait is what keeps them one rule.
    // The registration goes with the wait: dropped here, it cannot outlive
    // a statement that will not be re-run.
    if (lock_wait_.has_value() &&
        (result.ok() || scope.txn->trail().size() != statement_trail_mark_)) {
        if (locks_ != nullptr) locks_->DropWake(lock_wait_->key, lock_wait_->slot);
        lock_wait_.reset();
    }

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
        // **The one failure that does not poison** (R6-5): a write refused
        // by an in-doubt row before it wrote anything. The transaction is
        // exactly as it was - that is what the trail check above
        // established - and poisoning it would turn a wait into a forced
        // ROLLBACK, which is the stall D5's ceiling exists to bound rather
        // than the failure it exists to avoid. The client is told nothing
        // yet: `DispatchAsync` either re-runs the statement or answers the
        // named refusal, and *that* refusal poisons like any other, at the
        // end of the wait.
        // **A refused borrow is the second such failure** (AO-S6e-b). A
        // `DROP TABLE` inside a transaction that meets a positioned reader
        // has written nothing and asked for nothing else; poisoning it
        // would turn the wait `DispatchAsync` is about to take into a
        // forced ROLLBACK, and the re-run would answer "transaction is
        // aborted" - non-retryable - where the client used to get a
        // relation dropped. The refusal that ends the wait poisons like any
        // other, because by then `lock_wait_` is empty.
        if (!result.ok() && blocking_writer_ == 0 && !lock_wait_.has_value()) session.Poison();
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
    // `AbortOwnedScope`, whose declaration carries the argument and what
    // the old `return` cost. Both are states `Abort` is defined for:
    // `CommitTxn` here only clears assertion entries, and
    // `TransactionManager::Commit` fails only ahead of its `PublishCommit`,
    // so neither has published anything a rollback would take back.
    //
    // **One thing the unwind cannot reach on this first arm**, stated here
    // rather than implied by the call below it:
    // `AssertionEnforcer::CommitTxn` takes the reservation list out of
    // `pending_` before anything that can fail (`assertion_check.cpp`
    // says why), so a failure part-way through leaves nothing for
    // `AbortTxn` to find and the reservations it had not yet settled are
    // neither committed nor unapplied. The Bound Cabin then counts a
    // contribution whose row this unwind has just compensated. That is
    // **fail-closed** - an aggregate too high refuses writes it could
    // admit, it never admits one it should refuse - and it is not a
    // regression: before AO-S6d the row leaked too, inside a transaction
    // that stayed in flight for ever and was therefore invisible to every
    // reader, so the count was already describing rows nobody could see.
    // Closing it means changing what a failed settle leaves behind, which
    // is the enforcer's decision and not this arm's:
    // `docs/inflight/bugs/assertion-reservations-stranded-by-a-failed-settle.md`.
    if (Status s = enforcer_->CommitTxn(page_store_, wal_, scope.txn->id()); !s.ok()) {
        return AbortOwnedScope(scope, s);
    }
    auto committed = txn_->Commit(*scope.txn, effective_durability_);
    if (!committed.ok()) {
        return AbortOwnedScope(scope, committed.status());
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

    // **The enforcer's failure no longer skips the transaction's own
    // unwind** (AO-S6d): it used to return here. The two are independent in
    // the direction that matters - assertion entries this core could not
    // settle say nothing about whether the trail can be compensated.
    //
    // **A no-op on one of the three callers**, worth saying at the call
    // rather than leaving to be discovered: after a failed
    // `enforcer_->CommitTxn` there is no pending list left to unapply, and
    // `EndWrite`'s comment at that arm states what that leaves behind.
    // Called unconditionally anyway, because on the other two it is the
    // whole of the assertion unwind and a test on which caller it is would
    // be the branch that goes wrong.
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

    // AO-S3b's resume: `HandleUpdate`'s branch, for its reason.
    if (session.parked_write().has_value() && session.parked_write()->is_delete) {
        const Session::ParkedWrite& parked = *session.parked_write();
        scope = WriteScope{parked.txn, parked.owned, &session};
        snap = parked.snapshot;
        resume_from = parked.cursor;
        statement_trail_mark_ = parked.trail_mark;
        session.clear_parked_write();
    } else {
        auto opened = BeginWrite(session);
        if (!opened.ok()) return {ErrorReply(opened.status()), false, 0, opened.status()};
        scope = opened.value();

        auto snapshot = SnapshotFor(session);
        if (!snapshot.ok()) return {ErrorReply(snapshot.status()), false, 0, snapshot.status()};
        snap = snapshot.value().snap;
    }

    DispatchOutcome out = DeleteInner(line, scope, snap, resume_from);

    // AO-S3b's park: `HandleUpdate`'s arm, for its reason. Ahead of
    // `EndWrite` below because that *ends* the scope and this must not - a
    // DELETE that has already marked rows has nothing to roll them back to.
    // (The probe and ship arms that also ended it went at AT-S5f and AT-S6.)
    if (out.parked_mid_walk) {
        session.set_parked_write(Session::ParkedWrite{scope.txn, scope.owned, snap,
                                                      out.walk_cursor, statement_trail_mark_,
                                                      /*is_delete=*/true});
        return out;
    }


    const bool failed = out.response.rfind("ERR ", 0) == 0;
    Status verdict = failed ? Status::InvalidArgument(out.response) : Status::OK();
    if (Status s = EndWrite(session, scope, verdict); !s.ok() && !failed) {
        return {ErrorReply(s), false, 0, s};
    }
    return out;
}

DispatchOutcome CommandDispatcher::DeleteInner(std::string_view line, WriteScope& scope,
                                               const txn::Snapshot& snapshot,
                                               WalkCursor resume_from) {
    // AO-S3b, and `UpdateInner`'s site states the argument.
    bool parked_on_row = false;
    Status blocked_verdict;
    // H6 step 2: the parse leg. One of `observability.md` §10's three
    // request-level spans, and the cheapest to attribute wrongly - a
    // statement that is slow to *parse* looks identical from outside to
    // one that is slow to run.
    auto parsed = [&] {
        stats::SpanScope span(trace_, stats::Layer::kParse);
        return parser::Parse(line);
    }();
    if (!parsed.ok()) return {ErrorReply(parsed.status()), false, 0, parsed.status()};
    if (!std::holds_alternative<parser::DeleteStmt>(parsed.value())) {
        return {"ERR expected a DELETE statement", false};
    }
    const auto& stmt = std::get<parser::DeleteStmt>(parsed.value());

    // As INSERT and UPDATE (DT3c): a write resolves under its session.
    const std::optional<txn::ReadView> view =
        scope.session != nullptr ? ViewFor(*scope.session) : std::nullopt;
    auto oid =
        catalog_.FindTableOidByName(stmt.table_name, view.has_value() ? &*view : nullptr);
    if (!oid.ok()) return {ErrorReply(oid.status()), false, 0, oid.status()};
    if (Status s = catalog_.CheckRelationQualifier(stmt.schema, stmt.table_name, oid.value(),
                                                   stmt.table_byte_offset,
                                                   view.has_value() ? &*view : nullptr);
        !s.ok()) {
        return {ErrorReply(s), false, 0, s};
    }
    ReadBorrow borrow(locks_, NextReadHolder(), &read_borrows_, oid.value());  // AT-R1

    auto access = catalog_.InitTableAccess(oid.value());
    if (!access.ok()) return {ErrorReply(access.status()), false, 0, access.status()};
    const catalog::TableAccess& ta = *access.value();

    // Before anything is marked: UPDATE's rule and UPDATE's reason (no
    // destination and no multi-owner refusal since AT-S9). A delete-mark is
    // a write.
    if (Status admitted = CheckWriteAdmission(ta); !admitted.ok()) {
        return {ErrorReply(admitted), false, 0, admitted};
    }

    // The same WHERE compilation UPDATE uses, so a DELETE's predicate means
    // exactly what the SELECT that found the rows meant.
    auto predicates =
        exec::CompileWhere(catalog_, ta, stmt.table_name, stmt.where, /*view=*/nullptr, &borrow);
    if (!predicates.ok()) return {ErrorReply(predicates.status()), false, 0, predicates.status()};
    const std::vector<const catalog::Schema*> schemas = {&ta.schema};
    exec::ChainFrame frame;
    frame.Open(schemas, /*parent=*/nullptr);

    // Per-row scratch owned by the statement, as UPDATE's site explains:
    // cleared and refilled per scanned row, allocating only on the first.
    std::vector<std::byte> payload_copy;
    std::vector<exec::PendingSpill> frame_spills;
    const std::uint64_t filter_mask = predicates.value().filter_columns;

    // ---- Stage 1 of the walk -------------------------------------------
    //
    // Only what the WHERE reads (OPT-001): the row's pk if a version is
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
        auto id = exec::RowKeystoneId(payload_copy);
        if (!id.ok()) return id.status();
        if (trx_id_out != nullptr) *trx_id_out = tuple.value().trx_id;
        if (undo_ptr_out != nullptr) *undo_ptr_out = tuple.value().undo_ptr;
        if (Status s = exec::DecodeAndResolve(page_store_, ta.schema, ta.layout, payload_copy,
                                              frame.SlotsFor(0), filter_mask, frame_spills);
            !s.ok()) {
            return s;
        }
        auto matched = exec::EvaluateConjuncts(catalog_, page_store_, schemas, predicates.value(),
                                               frame, /*stats=*/nullptr, budget_, &snapshot);
        if (!matched.ok()) return matched.status();
        if (!matched.value()) return std::optional<std::uint64_t>{};
        return std::optional<std::uint64_t>{id.value()};
    };
    // Minted once per statement, for the reason UPDATE's copy records.
    txn::ReadView check_view = txn::ReadView::Everything();
    // **The reverse hoist is gone with the fan-out** (AT-S5f). It stood
    // here for AH-R1's reason one direction over - the last point before
    // the walk, and the walk is where nothing can park - and what it
    // hoisted was a question for another core. The check runs per row
    // inside the walk now, as the local arm always did, because a walk
    // that sees every chain has nothing to ask anyone.
    if (!ta.fkeys_in.empty()) check_view = CheckView(scope);

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

        // ---- The reverse check (docs/spec/foreign-keys.md §3) ----------
        //
        // RESTRICT: a row still referenced may not be deleted. Run per
        // qualifying row, since the id being deleted *is* the value every
        // child is checked against, and before the mark - the same
        // check-before-write ordering INSERT uses, and here it also means a
        // refused delete leaves no undo record behind.
        if (!ta.fkeys_in.empty()) {
            if (Status s = CheckNoChildrenBeforeDelete(ta, id, check_view); !s.ok()) {
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
    if (std::optional<std::uint64_t> pk = PkEqualityTarget(ta, stmt.where); pk.has_value()) {
        PkLookup found = LocateByPk(ta, *pk, storage::PageAccess::kWrite);
        if (found.kind == PkLookup::Kind::kAbsent) {
            return {"DELETED 0", false, 0};
        }
        if (found.kind == PkLookup::Kind::kAt) {
            // UPDATE's point arm states why the hold, not a re-fetch.
            {
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
    }

    // AO-S3b, and `UpdateInner`'s site states the argument.
    WalkCursor walk_cursor = resume_from;
    Status scan = VisitRelation(
        ta, storage::PageAccess::kWrite,
        [&](PageId page_id, heap::PageView& page,
            std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            if (Status s = mark(page_id, page, slot); !s.ok()) return s;
            // AO-S3b, and UPDATE's walk states the argument.
            if (parked_on_row) return storage::VisitControl::kStop;
            return storage::VisitControl::kContinue;
        },
        // R4/IS4: the pk window this statement can touch.
        WriteWalkSpan(ta, stmt.where),
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
