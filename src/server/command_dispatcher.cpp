#include "kds/server/command_dispatcher.hpp"

#include "kds/server/assertion_build_service.hpp"
#include "kds/server/index_build_service.hpp"
#include "kds/server/shipped_statement_executor.hpp"  // SS4: SHOW META's owner-side half
#include "kds/server/statement_ship_service.hpp"  // SS2: the fork ships through it

#include "kds/exec/type_literals.hpp"
#include "kds/storage/anchor_page.hpp"
#include "kds/server/mount_recovery.hpp"  // SHOW META's recovery block (RC09)
#include "kds/sched/scheduler.hpp"       // SHOW META's group accounting (sched.md 4)

#include "kds/stats/optimizer_signals.hpp"
#include "kds/stats/pattern_defs.hpp"
#include "kds/stats/relayout_planner.hpp"
#include "kds/stats/trail_store.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>
#include <charconv>
#include <iomanip>
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
    // server/statement_ship_service.hpp). Its own spelling because it is the
    // one refusal here that does **not** mean "nothing happened": the
    // statement may have committed on its owner. A client must be able to
    // tell it from the bare `ERR` it would otherwise wear, because the
    // correct response is to read the data back - not to retry, which
    // against engine-issued primary keys would insert twice.
    {StatusCode::kUnknownOutcome, "UNKNOWN_OUTCOME retryable=0 "},
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
    // the line the owner wrote whichever of them it was.
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

sched::Coro CommandDispatcher::DispatchAsync(std::string_view line, Session* session,
                                             DispatchOutcome* out) {
    // Today this never suspends: every statement runs on the core that owns
    // its relations, or is refused (core_affinity.hpp). The coroutine is
    // here so that when a step *can* reach another core, the suspension
    // point goes inside the executor and nothing above it changes.
    //
    // That it never suspends is also what makes this change verifiable: the
    // whole suite has to behave exactly as it did, because nothing about
    // when a reply is produced has moved yet.
    // **The statement may park from here**, which is the whole difference
    // between this entry point and `Dispatch()` - and the condition
    // statement shipping is admitted under (SS2). Set and cleared around
    // the synchronous half, which takes no suspension point, so it never
    // spans a park and never describes another statement.
    may_park_ = true;
    *out = DispatchAndStage(line, session);
    may_park_ = false;

    if (out->pending_shipped.has_value() && statement_ship_ != nullptr) {
        // The owner's execution (SS2/SS3, statement_ship_service.hpp). The
        // predicate re-finds the waiter each poll and reads the clock, so
        // the deadline ends the park with nothing having to wake it -
        // `pending_index_build`'s shape, and for its reason.
        const PendingShippedStatement shipped = std::move(*out->pending_shipped);
        out->pending_shipped.reset();
        const std::function<bool()> settled = [this, id = shipped.request_id] {
            return statement_ship_->Settled(id);
        };
        co_await sched::WaitUntil{&settled};
        *out = FinishShippedStatement(shipped);
    }

    if (out->pending_remote.has_value() && remote_reads_ != nullptr) {
        // The remote read (workplan P4c). The predicate re-finds the state
        // each poll, so a torn-down read wakes the waiter instead of
        // dangling a flag address (the reads vector may reallocate).
        const PipelineTag tag = *out->pending_remote;
        const std::function<bool()> finished = [this, tag] {
            SessionStepClient::RemoteRead* read = remote_reads_->Find(tag);
            return read == nullptr || read->done;
        };
        co_await sched::WaitUntil{&finished};
        *out = FinishRemoteRead(tag);
    }

    if (out->pending_index_build.has_value() && index_builds_ != nullptr) {
        // The owner's build (PW1c-6b-3, index_build_service.hpp). The
        // predicate re-finds the waiter each poll and reads the clock, so
        // the deadline ends the park with nothing having to wake it. The
        // pending record is copied off the outcome first: phase 2 writes
        // the outcome whole.
        const PendingIndexBuild build = std::move(*out->pending_index_build);
        const std::function<bool()> settled = [this, id = build.request_id] {
            return index_builds_->Settled(id);
        };
        co_await sched::WaitUntil{&settled};
        *out = FinishIndexBuild(build, session != nullptr ? *session : autocommit_session_);
    }

    if (out->pending_assertion_build.has_value() && assertion_builds_ != nullptr) {
        // The owner's Bound Cabin build (PW1c-6c, assertion_build_service.hpp).
        // `pending_index_build`'s shape exactly, and for its reasons: the
        // predicate re-finds the waiter and reads the clock, so the deadline
        // ends the park unaided, and the pending record is copied off the
        // outcome because phase 2 writes the outcome whole.
        const PendingAssertionBuild build = std::move(*out->pending_assertion_build);
        const std::function<bool()> settled = [this, id = build.request_id] {
            return assertion_builds_->Settled(id);
        };
        co_await sched::WaitUntil{&settled};
        *out = FinishAssertionBuild(build);
    }

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
    // Read only when something might report it; a dispatcher with no
    // logger does no clock reads at all.
    const sched::MonoTimeNs started_ns = log_ == nullptr ? 0 : NowNs();

    // A caller with no session of its own gets this dispatcher's, which is
    // permanently in autocommit unless that caller opens a transaction on
    // it. That is what makes `Dispatch(line)` mean exactly what it meant
    // before transactions existed.
    Session& active = session != nullptr ? *session : autocommit_session_;

    pending_commit_lsn_ = wal::kNoLsn;
    DispatchOutcome outcome = DispatchInner(line, active);
    // Read back out of the member the write paths set: threading it through
    // InsertInner/UpdateInner/EndWrite and every handler between would be a
    // parameter on a dozen signatures for one number, and one statement runs
    // at a time on a core (sched.md section 3), so there is no second value
    // to confuse it with.
    outcome.pending_lsn = pending_commit_lsn_;
    pending_commit_lsn_ = wal::kNoLsn;

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
    if (outcome.pending_remote.has_value()) {
        // With no reactor there is nothing to pump the reply through, so
        // the synchronous path can only finish a read that is already
        // complete - the in-process loopback arrangement tests use. An
        // incomplete one is closed and refused retryably rather than
        // spun on: a wait with nothing to run the other side is a hang.
        const PipelineTag tag = *outcome.pending_remote;
        SessionStepClient::RemoteRead* read =
            remote_reads_ != nullptr ? remote_reads_->Find(tag) : nullptr;
        if (read != nullptr && read->done) {
            outcome = FinishRemoteRead(tag);
        } else {
            if (remote_reads_ != nullptr) remote_reads_->Close(tag);
            return {ErrorReply(Status::TxnConflict(
                        "remote read needs the reactor path; retry on a served connection")),
                    false};
        }
    }
    if (outcome.pending_shipped.has_value()) {
        // Unreachable: `MayShip` refuses without `may_park_`, which only
        // `DispatchAsync` sets. Written as a refusal rather than left to a
        // null dereference because of *which* refusal it would have to be -
        // the statement is already on its way to an owner that may commit
        // it, so nothing retryable is available and `UnknownOutcome` is the
        // only truthful answer (statement_ship_service.hpp's rule 1).
        statement_ship_->Close(outcome.pending_shipped->request_id);
        return {ErrorReply(Status::UnknownOutcome(
                    "statement shipping: a statement reached core " +
                    std::to_string(outcome.pending_shipped->owner_core) +
                    " from a path that cannot await its answer; whether it ran cannot be "
                    "established from here")),
                false};
    }
    if (outcome.pending_index_build.has_value()) {
        // The same stance for the owner's build (PW1c-6b-3): with no reactor
        // nothing here receives the reply, so the statement is abandoned
        // now rather than spun on, and the owner is told - its window would
        // otherwise wait the ceiling out on a tree nobody will publish.
        // (`index_builds_` is set: nothing produces the pending outcome
        // without it.)
        const PendingIndexBuild& build = *outcome.pending_index_build;
        index_builds_->Close(build.request_id);
        index_builds_->Done(build.owner_core, build.def.index_oid, /*committed=*/false);
        return {ErrorReply(Status::TxnConflict(
                    "CREATE INDEX on '" + build.table_name +
                    "' needs the reactor path to await its owner's build; retry on a served "
                    "connection")),
                false};
    }
    if (outcome.pending_assertion_build.has_value()) {
        // The same stance for the owner's Bound Cabin (PW1c-6c): with no
        // reactor nothing here receives the reply, so the statement is
        // abandoned now and the owner told - otherwise it would be left
        // enforcing a constraint no row will ever name.
        const PendingAssertionBuild& build = *outcome.pending_assertion_build;
        assertion_builds_->Close(build.request_id);
        assertion_builds_->Done(build.owner_core, build.assertion_id, /*committed=*/false);
        return {ErrorReply(Status::TxnConflict(
                    "CREATE ASSERTION on '" + build.table_name +
                    "' needs the reactor path to await its owner's build; retry on a served "
                    "connection")),
                false};
    }
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
        IEquals(cmd, "DESC") || IEquals(cmd, "BEGIN") || IEquals(cmd, "START") ||
        IEquals(cmd, "COMMIT") || IEquals(cmd, "ROLLBACK") || IEquals(cmd, "ABORT")) {
        return Role::kReadOnly;
    }
    if (IEquals(cmd, "INSERT") || IEquals(cmd, "UPDATE") || IEquals(cmd, "DELETE")) {
        return Role::kReadWrite;
    }
    if (IEquals(cmd, "SET")) {
        // A session may steer its own reads; the server is the admin's.
        return IEquals(SplitFirstToken(rest).first, "ISOLATION") ? Role::kReadOnly : Role::kAdmin;
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
        if (IEquals(sub, "CABIN_OPTIMIZER")) return HandleSetCabinOptimizer(sub_rest);
        return {"ERR unknown SET target; SET ISOLATION LEVEL and SET CABIN_OPTIMIZER are "
                "supported",
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
        return {"ERR unknown SHOW target", false};
    }
    if (IEquals(cmd, "DESCRIBE") || IEquals(cmd, "DESC")) {
        return HandleDescribe(rest, session);
    }
    // A peer takes no DDL (workplan-peer-writer.md PW4). Every target of
    // these three verbs writes state only the system core may write, so
    // the whole verb is refused here, before any handler - the argument,
    // including why nothing below this catches it in a **release** build,
    // is at `PeerDdlRefused` (core_affinity.hpp). Do not remove this as a
    // message improvement: it is the only guard once NDEBUG is set, and
    // it is what makes §5d's purge-gate soundness argument enforced
    // rather than assumed (see the gate).
    //
    // Keyed on `catalog_read_only_` (see its declaration) and deliberately
    // not on `core_id_`, and on exactly the token the routing below reads,
    // so the two cannot disagree about what a verb is.
    if (catalog_read_only_ &&
        (IEquals(cmd, "CREATE") || IEquals(cmd, "ALTER") || IEquals(cmd, "DROP"))) {
        // Inside an explicit transaction this poisons, like every other
        // write-capability refusal - CrossCoreWriteRefused fires inside a
        // WriteScope and poisons, and two refusals of the same shape must
        // not disagree about the transaction's fate (txn.md section 10-8's
        // posture). Decided at the PW4 review, before PW5's listeners make
        // the path reachable.
        session.Poison();
        return {ErrorReply(PeerDdlRefused(core_id_, cmd)), false};
    }
    // The PW1c interim DML guard stood here from 2026-08-24 until PW1c-5
    // removed it the same day. What replaced it, so its removal is not a
    // hole: `CheckWriteAffinity`'s shape gate refuses the still-unsound
    // shapes by name (FK-linked, cabined, assertion-covered
    // - each citing the task that lifts it; btree lifted at PW2-4, indexed at
    // PW1c-6b-4, and the key mode gone entirely 2026-08-25 - a
    // caller-named pk is now refused per row in InsertOneRow), the
    // multi-row VALUES path refuses on a
    // peer before touching the catalog page, and the store's `MayWrite`
    // is enforced for leased stores in **every** build now, not Debug
    // alone - so an unfunded write is refused retryably instead of
    // surfacing as a rule-5 stamp mismatch at the next mount.
    if (IEquals(cmd, "CREATE")) {
        auto [sub, sub_rest] = SplitFirstToken(rest);
        if (IEquals(sub, "PATTERN")) {
            return HandleCreatePattern(Trim(line));
        }
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
        // Patterns, cabins and indexes can be dropped - there is still no
        // DROP TABLE, and the catalog is append-only apart from these three
        // paths. Routed by name so `DROP TABLE t` gets the parser's truthful
        // refusal with a position rather than "unknown DROP target".
        if (IEquals(sub, "PATTERN")) {
            return HandleDropPattern(Trim(line));
        }
        if (IEquals(sub, "CABIN")) {
            return HandleCabin(Trim(line));
        }
        if (IEquals(sub, "INDEX")) {
            return HandleIndex(Trim(line), session);
        }
        if (IEquals(sub, "ASSERTION")) {
            return HandleAssertion(Trim(line), session);
        }
        if (IEquals(sub, "TABLE")) {
            return HandleDropTable(Trim(line), session);
        }
        return {"ERR only DROP TABLE, DROP PATTERN, DROP CABIN, DROP INDEX and DROP ASSERTION "
                "are supported",
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
            return {"ERR " + s.message(), false};
        }
    }
    if (Status s = page_store_.Sync(); !s.ok()) {
        if (logging(LogLevel::kError)) {
            log_->Error("storage", "client SYNC failed: " + s.message());
        }
        return {"ERR " + s.message(), false};
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
       << " wal_anchor_count=" << superblock_.wal_anchor_count()
       << " cabin_optimizer=" << (cabin_optimizer_enabled_ ? "on" : "off")
       // The core serving this session (PW6, docs/inflight/in-progress/workplan-peer-writer.md).
       // Under `peer_listeners = on` the kernel picks the accepting core and
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

    // A peer's lease refills and what each cost (lease_refill_stats.hpp):
    // requests and grants per kind, and the three legs' maxima - submit to
    // grant received (the ring and core 0), grant received to the parked
    // coroutine resuming (this reactor), and the whole wait. The trace
    // PW6's four-writer cell asked for, where every refill took seconds
    // and nothing said which leg. Peers only: core 0 leases from nobody.
    const auto refill_block = [&os](const char* kind, const LeaseRefillStats* s) {
        if (s == nullptr) return;
        os << ' ' << kind << "_refill_requests=" << s->requests << ' ' << kind
           << "_refill_grants=" << s->grants << ' ' << kind
           << "_refill_wait_max_us=" << s->wait_total_max_ns / 1000 << ' ' << kind
           << "_refill_submit_lag_max_us=" << s->submit_lag_max_ns / 1000 << ' ' << kind
           << "_refill_grant_lag_max_us=" << s->wait_to_grant_max_ns / 1000 << ' ' << kind
           << "_refill_resume_lag_max_us=" << s->resume_lag_max_ns / 1000 << ' ' << kind
           << "_refill_submit_lag_max_iters=" << s->submit_lag_max_iters << ' ' << kind
           << "_refill_grant_lag_max_iters=" << s->grant_lag_max_iters << ' ' << kind
           << "_refill_resume_lag_max_iters=" << s->resume_lag_max_iters;
    };
    refill_block("extent", extent_refill_stats_);
    refill_block("trxid", trx_id_refill_stats_);
    refill_block("rowid", row_id_refill_stats_);

    // The cross-core writes this core refused, `crosscore.md` §6's "input
    // the future placement/2PC decision will be made from". Printed
    // unconditionally, zero included: unlike the blocks above it, a zero
    // here is an answer - *this workload asked for no cross-core write* -
    // and that is exactly the reading the before-shipping era is recorded
    // for.
    //
    // **The undercount, stated where it is read.** The key is
    // (home core, target core, relation oid), so a refusal that never
    // resolves a relation cannot appear:
    //
    //   - **DDL on a peer** is refused by verb before anything is parsed
    //     (`PeerDdlRefused`, the guard at the top of Dispatch) - not a
    //     relation-keyed write, and not counted.
    //   - A statement refused **before resolution** for any other reason -
    //     a parse error, `max_insert_rows`, the multi-row-without-a-
    //     transaction refusal - never reaches the affinity check.
    //   - The two **owner-core** refusals, `RelationWriteRightsPending`
    //     (PW1c-7) and `IndexBuildPending` (PW1c-6b-2), are deliberately
    //     *not* counted: the write is not cross-core at all, it is this
    //     core's own write waiting on a grant or a build window, and
    //     folding them in would inflate the 2PC evidence with cases 2PC
    //     does not address.
    //
    // `docs/inflight/in-progress/workplan-peer-writer.md` §8's pre-parse DML guard - the class
    // §6 names as invisible to a relation-keyed counter - was removed at
    // PW1c-5, so at this commit a peer's foreign write is refused by
    // `CheckWriteAffinity` and *is* counted. The list above is the
    // undercount that is real now.
    os << " cross_core_write_refusals=" << cross_core_writes_.total()
       << " cross_core_write_refusal_keys=" << cross_core_writes_.counts().size();
    if (!cross_core_writes_.counts().empty()) {
        // `home>target:oid=count`, comma-separated, in the map's order
        // (stable run to run - sched.md §8's rule for anything observable).
        // Capped, and the cap **says** it truncated: a silent cut would
        // read as "these were all of them".
        constexpr std::size_t kMaxDetailKeys = 16;
        os << " cross_core_write_refusal_detail=";
        std::size_t printed = 0;
        for (const auto& [key, count] : cross_core_writes_.counts()) {
            if (printed == kMaxDetailKeys) {
                os << ",+" << (cross_core_writes_.counts().size() - printed) << "more";
                break;
            }
            if (printed != 0) os << ',';
            os << key.home_core << '>' << key.target_core << ':' << key.rel_oid
               << '=' << count;
            ++printed;
        }
    }

    // **Statement shipping** (D7 of the statement-shipping work order).
    // Two halves, because a core is both an arrival core and an owner and
    // the two say different things: the first four are what this core
    // *sent*, the last four what it *ran for others*.
    //
    // Absent rather than zeroed where nothing is wired, the rule the
    // recovery and scheduler blocks follow - on a single-core instance
    // these lines do not exist at all, which is the honest reading of
    // "shipping is not armed here".
    //
    // **`cross_core_write_refusals` above keeps its exact meaning**, and
    // that is deliberate: before shipping it counted the whole demand, and
    // after it counts the residue - the writes shipping does *not* convert,
    // which is R6's multi-owner and in-transaction population and the
    // evidence base a 2PC decision would be made from. A field whose
    // meaning changed silently would have destroyed that series.
    if (statement_ship_ != nullptr) {
        os << " shipped_statements=" << statement_ship_->shipped()
           << " shipped_replies=" << statement_ship_->replies()
           << " shipped_refusals=" << statement_ship_->refusals()
           << " shipped_wait_us_max=" << statement_ship_->wait_ns_max() / 1000
           << " shipped_waiting=" << statement_ship_->waiting()
           << " shipped_late_executed=" << statement_ship_->late_executed_replies()
           << " shipped_identity_mismatches=" << statement_ship_->identity_mismatches();
    }
    if (shipped_statements_ != nullptr) {
        // The owner's side. `shipped_executed` against the arrival cores'
        // `shipped_statements` is what SS-B's claim 3 is read from - whether
        // the ceiling reached is the owner's execution capacity - and
        // `shipped_running` is the population doing it right now.
        // `shipped_early_evictions` is the dedup record's honesty check:
        // non-zero means a duplicate could have been re-executed.
        os << " shipped_executed=" << shipped_statements_->executed()
           << " shipped_running=" << shipped_statements_->running()
           << " shipped_deduped=" << shipped_statements_->deduped()
           << " shipped_unanswerable=" << shipped_statements_->unanswerable()
           << " shipped_early_evictions=" << shipped_statements_->early_evictions();
    }

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
           // raised, `sched_wake_race_skips` how often the pre-block
           // re-check caught a message the sender had decided not to wake
           // for - the race the flag exists for, so a run holding at 0 has
           // not exercised it - and `sched_parked_idle_blocks` the blocks
           // taken with tasks still queued, every one of which was a spin
           // before "parked is not ready".
           << " sched_idle_blocks=" << scheduler_view_->idle_blocks()
           << " sched_wake_race_skips=" << scheduler_view_->wake_race_skips()
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
           // every core and equals the sum of their `sched_wakes_received`;
           // `sched_spurious_wakes` are wakes that ended a block and found
           // an empty inbox, which the race makes ordinary rather than
           // wrong.
           << " sched_idle_block_us=" << scheduler_view_->idle_block_ns() / 1000
           << " sched_wakes_sent=" << scheduler_view_->wakes_sent()
           << " sched_wakes_received=" << scheduler_view_->wakes_received()
           << " sched_spurious_wakes=" << scheduler_view_->spurious_wakes();
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

    // PW1c-6b-2's windows on a peer: how many of this core's relations
    // refuse writes for an index build core 0 has not yet said `done` for,
    // and the oldest's age - a window is otherwise visible to a client
    // only as a retryable refusal, and to nobody as a duration.
    if (pending_index_builds_ != nullptr) {
        // Oldest first (core_affinity.hpp), so the front is the maximum.
        sched::MonoTimeNs oldest_ns = 0;
        if (!pending_index_builds_->empty()) {
            const std::uint64_t opened = pending_index_builds_->entries().front().opened_at_ns;
            const sched::MonoTimeNs now = NowNs();
            if (now > opened) oldest_ns = now - opened;
        }
        os << " index_build_windows=" << pending_index_builds_->size()
           << " index_build_window_age_max_us=" << oldest_ns / 1000;
    }

    // The last recovery, for the operator who has to answer "what did the
    // restart do" (RC09, `docs/spec/wal.md` §13). Absent rather than zeroed when no
    // report is installed - a dispatcher built without one (every socket-free
    // test) has not "recovered nothing", it has no answer, and printing zeroes
    // would be an answer.
    if (recovery_ != nullptr) {
        os << " recovery_records=" << recovery_->records
           << " recovery_committed=" << recovery_->winners
           << " recovery_rolled_back=" << recovery_->transactions_rolled_back
           << " recovery_compensations=" << recovery_->compensations
           << " recovery_redo_applied=" << recovery_->redo_applied
           << " recovery_pages_healed=" << recovery_->pages_healed
           << " recovery_torn_tail=" << (recovery_->torn_tail ? 1 : 0);
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
        // constraint that would admit every write.
        // `_foreign` is neither of those: an assertion on a relation another
        // core owns is enforced by that core (PW1c-6c), so this core reads
        // the declaration and takes nothing on. Printed rather than folded
        // into either counter, because a correctly-partitioned instance
        // would otherwise read as half-unrecovered.
        os << " recovery_assertions_enforcing=" << recovery_->assertions_enforcing
           << " recovery_assertions_unrecovered=" << recovery_->assertions_unrecovered
           << " recovery_assertions_foreign=" << recovery_->assertions_foreign;
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
        cabin_optimizer_enabled_ = true;
    } else if (IEquals(value, "OFF")) {
        cabin_optimizer_enabled_ = false;
    } else {
        return {"ERR SET CABIN_OPTIMIZER takes on or off, not '" + std::string(value) + "'",
                false};
    }
    return {std::string("OK cabin_optimizer=") + (cabin_optimizer_enabled_ ? "on" : "off"),
            false};
}

DispatchOutcome CommandDispatcher::HandleShowPatterns() {
    auto rows = catalog_.ListPatterns();
    if (!rows.ok()) {
        return {"ERR " + rows.status().message(), false};
    }

    // The declarations, joined in by pattern_id so a declared pattern
    // prints its name instead of a bare hash. Read once for the whole
    // listing rather than probed per row: the join is over tens of rows on
    // an inspection path, and a probe per pattern would rescan the relation
    // once per pattern.
    //
    // An auto-registered pattern has no definition and keeps printing as
    // hex - deliberately, since it has no name to print.
    auto defs = stats::ListPatternDefs(catalog_, page_store_);
    if (!defs.ok()) {
        return {"ERR " + defs.status().message(), false};
    }

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

        for (const stats::PatternDef& def : defs.value()) {
            if (def.pattern_id != row.pattern_id) continue;
            os << " name=" << def.name << " params=" << def.param_count;
            break;
        }

        // Origin and pinning are separate fields and are printed separately:
        // an auto pattern can be pinned and a declared one can be unpinned,
        // and collapsing them into one word would make both unreadable.
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
        return {"ERR " + rows.status().message(), false};
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
        return {"ERR " + tables.status().message(), false};
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
        return {"ERR " + tables.status().message(), false};
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
    // left `InvalidateCatalog`'s EvictClean failing on a dirty catalog
    // frame so the peer's cache never dropped again. PW1c's guard covers
    // the DML verbs; this one was a read all along.
    auto page = page_store_.GetForRead(page_id);
    if (!page.ok()) {
        return {"ERR " + page.status().message(), false};
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
       // The pair the rule-5 mount refusal names; without them here the
       // operator meeting it cannot inspect the field it cites (the
       // f19ead1 review's observability gap).
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

        auto existing = catalog_.FindTableOidByName(args);
        if (existing.ok()) {
            return {"EXISTS oid=" + std::to_string(existing.value()), false};
        }
        if (existing.status().code() != StatusCode::kNotFound) {
            return {"ERR " + existing.status().message(), false};
        }
        if (auto refused = RefuseIfNameHeldByPendingDrop(args, session); refused.has_value()) {
            return *refused;
        }

        catalog::Schema schema;
        DdlScope ddl = DdlScopeFor(scope);
        auto oid = catalog_.CreateTable(catalog::kNamespacePublic, args, schema,
                                         catalog::ClusteredType::kHeap, ddl.trx_id, ddl.sink());
        NoteDdlRows(ddl);  // before the status, for the partial-write reason above
        if (!oid.ok()) {
            return {"ERR " + oid.status().message(), false};
        }
        return {"CREATED oid=" + std::to_string(oid.value()), false};
    });
}

DispatchOutcome CommandDispatcher::HandleDescribe(std::string_view args,
                                                  Session& session) {
    if (args.empty()) {
        return {"ERR DESCRIBE requires a table name", false};
    }

    const std::optional<txn::ReadView> view = ViewFor(session);
    auto oid = catalog_.FindTableOidByName(args, view.has_value() ? &*view : nullptr);
    if (!oid.ok()) {
        return {"ERR " + oid.status().message(), false};
    }

    auto table_row = catalog_.GetSysTableRow(oid.value());
    if (!table_row.ok()) {
        return {"ERR " + table_row.status().message(), false};
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
        return {"ERR " + built.status().message(), false};
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
            return {"ERR " + anchor.status().message(), false};
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
       << " owner_core=" << table_row.value().owner_core
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

DispatchOutcome CommandDispatcher::HandleCreatePattern(std::string_view line) {
    auto parsed = parser::Parse(line);
    if (!parsed.ok()) {
        return {"ERR " + parsed.status().message(), false};
    }
    if (!std::holds_alternative<parser::CreatePatternStmt>(parsed.value())) {
        return {"ERR expected a CREATE PATTERN statement", false};
    }
    const auto& stmt = std::get<parser::CreatePatternStmt>(parsed.value());

    auto result = exec::CreatePattern(catalog_, page_store_, wal_, stmt);
    if (!result.ok()) {
        return {"ERR " + result.status().message(), false};
    }
    if (Status s = AwaitDdlDurability(); !s.ok()) return {ErrorReply(s), false};

    std::ostringstream os;
    // The pattern_id in hex, because that is what ANALYZE prints for a
    // matching statement - which makes "I declared it, why doesn't traffic
    // match" answerable by comparing two numbers rather than by guessing.
    os << (result.value().adopted ? "ADOPTED PATTERN" : "CREATED PATTERN")
       << " name=" << stmt.name << " pattern_id=0x" << std::hex << result.value().pattern_id
       << std::dec << " dir_depth=" << static_cast<int>(result.value().dir_depth)
       << " params=" << result.value().param_count;

    // Warnings ride the success response as "\n"-escaped sections, the same
    // one-line-per-reply contract every other multi-part response here uses:
    // the declaration succeeded, and a one-line protocol has no side channel
    // to put a caveat in.
    for (const std::string& warning : result.value().warnings) {
        os << "\\n" << "WARN " << warning;
    }

    if (logging(LogLevel::kInfo)) {
        log_->Info("ddl", "declared pattern '" + stmt.name + "'" +
                              (result.value().adopted ? " (adopted an auto row)" : ""));
    }
    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleDropPattern(std::string_view line) {
    auto parsed = parser::Parse(line);
    if (!parsed.ok()) {
        return {"ERR " + parsed.status().message(), false};
    }
    if (!std::holds_alternative<parser::DropPatternStmt>(parsed.value())) {
        return {"ERR expected a DROP PATTERN statement", false};
    }
    const auto& stmt = std::get<parser::DropPatternStmt>(parsed.value());

    auto pattern_id = exec::DropPattern(catalog_, page_store_, wal_, stmt.name);
    if (!pattern_id.ok()) {
        return {"ERR " + pattern_id.status().message(), false};
    }
    if (Status s = AwaitDdlDurability(); !s.ok()) return {ErrorReply(s), false};

    std::ostringstream os;
    os << "DROPPED PATTERN name=" << stmt.name << " pattern_id=0x" << std::hex
       << pattern_id.value() << std::dec;
    if (logging(LogLevel::kInfo)) {
        log_->Info("ddl", "dropped pattern '" + stmt.name + "'");
    }
    return {os.str(), false};
}

namespace {

// The one spelling of a successful CREATE INDEX (docs/spec/client-manual.md),
// for both arms of the statement. `tail` is what differs: the local arm's
// ` entries=0`, the foreign arm's ` built_by_core=<n>`.
std::string CreatedIndexReply(std::string_view name, std::string_view table,
                              catalog::Oid index_oid, PageId root, std::uint16_t key_width,
                              std::uint16_t entry_width, const std::string& tail,
                              const std::vector<std::string>& warnings) {
    std::ostringstream os;
    os << "CREATED INDEX name=" << name << " on=" << table << " index_oid=" << index_oid
       << " root_page=" << root << " key_width=" << key_width << " entry_width=" << entry_width
       << tail;
    for (const std::string& warning : warnings) os << "\\n" << "WARN " << warning;
    return os.str();
}

}  // namespace

DispatchOutcome CommandDispatcher::HandleIndex(std::string_view line,
                                               Session& session) {
    // Parsed before the DDL scope exists: the PW1c-6 refusal below must
    // not cost a transaction id (PW4's philosophy - refuse before
    // resources), and the parse is pure.
    parser::Parser parser(line);
    auto parsed = parser.Parse();
    if (!parsed.ok()) {
        return {"ERR " + parsed.status().message(), false};
    }
    if (!std::holds_alternative<parser::IndexStmt>(parsed.value())) {
        return {"ERR expected a CREATE INDEX or DROP INDEX statement", false};
    }
    const parser::IndexStmt stmt = std::get<parser::IndexStmt>(std::move(parsed.value()));

    // A relation another core owns has its index built *there* (§7c,
    // decided 2026-08-25: the owner builds - `Backfill` here would read
    // the device's stale image and miss every row the owner holds), which
    // is `BeginForeignIndexBuild`'s two phases below. The client that
    // drives them is wired on core 0 by the Expeditor (PW1c-6b-4); a
    // dispatcher without one is a socket-free test, and it is refused by
    // name rather than left to build a tree in the wrong core's pages.
    // The unfiltered row read is deliberate: the build touches physical
    // pages whichever view resolved the name. An unresolvable name falls
    // through - exec::CreateIndex owns that refusal and its byte position.
    if (!stmt.drop) {
        if (auto rel_oid = catalog_.FindTableOidByName(stmt.table_name); rel_oid.ok()) {
            auto rel_row = catalog_.GetSysTableRow(rel_oid.value());
            if (rel_row.ok() && rel_row.value().owner_core != core_id_) {
                if (index_builds_ != nullptr) {
                    return BeginForeignIndexBuild(stmt, rel_row.value().owner_core, session);
                }
                return {"ERR " +
                            Status::Unsupported(
                                "CREATE INDEX on '" + stmt.table_name + "' at byte " +
                                std::to_string(stmt.table_byte_offset) + ": the relation is "
                                "owned by core " +
                                std::to_string(rel_row.value().owner_core) +
                                ", built by its owner (workplan-peer-writer.md §7c, PW1c-6b), "
                                "and this dispatcher has no index-build client to reach it")
                                .message(),
                        false};
            }
        }
    }

    // The drop's cross-core hole the gate lift opens (PW1c-6b-4, the
    // review's finding). DROP INDEX marks the sys.indexes row and
    // `BumpVersion` broadcasts *at the mark*, before the commit; on the
    // owner, DT9's "is the deleter in flight" predicate walks that core's
    // own live list and never finds core 0's transaction, so the mark
    // reads as settled and the index leaves the owner's view at once. With
    // the shape gate lifted the owner now takes writes and maintains
    // nothing for the vanished index - and a ROLLBACK restores it missing
    // every row written meanwhile. Inside a transaction that window is the
    // client's to hold open, so it is refused by name, exactly as the
    // sibling CREATE is (BeginForeignIndexBuild) and for the same reason.
    // Autocommit keeps only the commit-failure window every DDL has, so it
    // is left admitted. Core-local (core 0 owns the relation) stays
    // isolated by DT9 and is untouched.
    if (stmt.drop && session.in_explicit_txn()) {
        if (auto ix = catalog_.FindIndexByName(stmt.index_name); ix.ok()) {
            auto rel_row = catalog_.GetSysTableRow(ix.value().table_oid);
            if (rel_row.ok() && rel_row.value().owner_core != core_id_) {
                return {"ERR " +
                            Status::Unsupported(
                                "DROP INDEX '" + stmt.index_name + "' at byte " +
                                std::to_string(stmt.byte_offset) + ": its relation is owned by "
                                "core " + std::to_string(rel_row.value().owner_core) +
                                ", whose maintenance cannot see this delete-mark's deleter in "
                                "flight (DT9 is core-local), so inside a transaction the owner "
                                "would stop maintaining the index before COMMIT and a ROLLBACK "
                                "would restore it missing the owner's meanwhile-writes; run it "
                                "in autocommit (workplan-peer-writer.md PW1c-6b-4)")
                                .message(),
                        false};
            }
        }
    }

    return InDdlStatement(session, [&](WriteScope& scope) -> DispatchOutcome {

        if (stmt.drop) {
            // **Allowed inside an explicit transaction again as of DT9, and
            // the history is the point.** DT5 shipped this as atomic *and
            // isolated* on the strength of `SHOW INDEXES` filtering; that was
            // wrong, because `InitTableAccess` builds a relation's index list
            // through `ListIndexes()` with a **null view**, so index
            // maintenance treated the delete-mark as done the moment it was
            // written - another session's INSERT wrote no index entry, and a
            // rollback restored the index missing that row. It was then
            // refused rather than answered wrongly.
            //
            // DT9 closes it at the read instead of at the statement: an
            // unfiltered catalog read now counts a delete-mark only once its
            // deleter is no longer in flight (`catalog.cpp`'s `ScanAll`), so
            // maintenance keeps writing entries for an index whose drop has
            // not committed. If the drop commits the entries go with the
            // index; if it rolls back the index is whole.
            //
            // **The claim this may carry is core-0-scoped**, not "isolated"
            // outright: `IsInFlight` answers about one core's transactions,
            // and it is every writer's core only while CC3 refuses
            // cross-core writes (`docs/spec/ddl-transactional.md` §5a).
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
                return {"ERR " + index_oid.status().message(), false};
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
        const std::optional<txn::ReadView> create_view = ViewFor(session);
        auto result = exec::CreateIndex(catalog_, page_store_, stmt, create_ddl.trx_id,
                                        create_ddl.txn != nullptr ? &created_row : nullptr,
                                        create_view.has_value() ? &*create_view : nullptr, wal_);
        // Before the status is read: a create that failed after the catalog
        // row went down still left it there.
        if (create_ddl.txn != nullptr && created_row.page_id != kInvalidPageId) {
            create_ddl.written.push_back(created_row);
            NoteDdlRows(create_ddl);
        }
        if (!result.ok()) return {ErrorReply(result.status()), false};
        if (logging(LogLevel::kInfo)) {
            log_->Info("ddl", "created index '" + stmt.index_name + "' on " + stmt.table_name);
        }
        // `entries=0` is a literal the manual documents (client-manual.md,
        // CREATE INDEX) and the backfill over a populated relation makes
        // false. It stays until the field is dropped or counted - either
        // is client-visible, so neither is done in passing (the PW1c-6b-3
        // review's finding); the foreign arm prints who built the tree and
        // never the literal.
        return {CreatedIndexReply(stmt.index_name, stmt.table_name, result.value().index_oid,
                                  result.value().root_page_id, result.value().key_width,
                                  result.value().entry_width, " entries=0",
                                  result.value().warnings),
                false};
    });
}

DispatchOutcome CommandDispatcher::BeginForeignIndexBuild(const parser::IndexStmt& stmt,
                                                          std::uint32_t owner_core,
                                                          Session& session) {
    // Inside an explicit transaction the owner's refusal window would last
    // until the client's COMMIT, however far off that is, so the statement
    // is refused by name before anything is sent (§7c). Not poisoned:
    // nothing was written and the transaction is as it was.
    if (session.in_explicit_txn()) {
        return {ErrorReply(Status::Unsupported(
                    "CREATE INDEX on '" + stmt.table_name + "' at byte " +
                    std::to_string(stmt.table_byte_offset) + ": the relation is owned by core " +
                    std::to_string(owner_core) +
                    ", which refuses its writes from the build until this statement ends - "
                    "inside a transaction, the client's COMMIT; run it in autocommit "
                    "(workplan-peer-writer.md PW1c-6b-3)")),
                false};
    }
    // Resolved under the session's view (spec §5's rule), as the local arm
    // is; `kByOwner` stands the owner refusal down, since the owner seeds
    // its own anchor. The oid is issued here, before any page exists - a
    // burned one is never reissued, the ids' standing rule - and no page of
    // the relation is touched: the catalog pages are this core's.
    const std::optional<txn::ReadView> view = ViewFor(session);
    auto def = exec::PrepareIndexDef(catalog_, stmt, view.has_value() ? &*view : nullptr,
                                     catalog::Catalog::AnchorSeed::kByOwner);
    if (!def.ok()) return {ErrorReply(def.status()), false};

    const std::uint64_t request_id = next_remote_request_++;
    if (Status s = index_builds_->Request(owner_core, request_id, def.value()); !s.ok()) {
        return {ErrorReply(s), false};
    }
    if (logging(LogLevel::kInfo)) {
        log_->Info("ddl", "asked core " + std::to_string(owner_core) + " to build index '" +
                              stmt.index_name + "' on " + stmt.table_name + " (request " +
                              std::to_string(request_id) + ")");
    }
    DispatchOutcome pending;
    pending.pending_index_build = PendingIndexBuild{request_id, owner_core,
                                                    std::move(def.value()), stmt.table_name,
                                                    stmt.key_columns[0].name};
    return pending;
}

DispatchOutcome CommandDispatcher::FinishIndexBuild(const PendingIndexBuild& build,
                                                    Session& session) {
    const IndexBuildOutcome* reply = index_builds_->Find(build.request_id);
    Status verdict = Status::OK();
    PageId root = kInvalidPageId;
    if (reply == nullptr) {
        verdict = Status::IoError("CREATE INDEX on '" + build.table_name +
                                  "': the wait for core " + std::to_string(build.owner_core) +
                                  "'s build was closed under the statement");
    } else if (!reply->arrived) {
        // The deadline. Retryable: the owner's window closes on the
        // done(aborted) below - or, if the request itself is still in the
        // ring, when its reply reaches the receiver's no-waiter branch -
        // and a retry after that builds afresh.
        verdict = Status::TxnConflict(
            "CREATE INDEX on '" + build.table_name + "': core " +
            std::to_string(build.owner_core) + " did not reply within " +
            std::to_string(kIndexBuildReplyDeadlineNs / 1'000'000'000ull) +
            " s; the build is abandoned and the owner told (workplan-peer-writer.md "
            "PW1c-6b-3)");
    } else if (!reply->status.ok()) {
        verdict = reply->status.WithContext("CREATE INDEX on '" + build.table_name +
                                            "': core " + std::to_string(build.owner_core) +
                                            " refused the build");
    } else {
        root = reply->root_page_id;
    }
    index_builds_->Close(build.request_id);
    if (!verdict.ok()) {
        index_builds_->Done(build.owner_core, build.def.index_oid, /*committed=*/false);
        if (logging(LogLevel::kWarn)) log_->Warn("ddl", verdict.message());
        return {ErrorReply(verdict), false};
    }

    // Phase 2 proper: the row alone under a DDL scope of its own - the
    // local arm's shape minus the build and the anchor seed. A refusal
    // here (a same-named index created while this was parked, the
    // relation dropped) aborts the scope and orphans the owner's tree
    // through the done(aborted) below. The staged-commit member is zeroed
    // first because DispatchAndStage, the only other place that zeroes
    // it, is not on this path.
    pending_commit_lsn_ = wal::kNoLsn;
    DispatchOutcome out = InDdlStatement(session, [&](WriteScope& scope) -> DispatchOutcome {
        DdlScope ddl = DdlScopeFor(scope);
        catalog::Catalog::IndexDef def = build.def;
        def.root_page_id = root;
        catalog::CatalogRowRef created_row;
        auto index_oid = catalog_.CreateIndex(def, ddl.trx_id,
                                              ddl.txn != nullptr ? &created_row : nullptr,
                                              catalog::Catalog::AnchorSeed::kByOwner);
        // Before the status is read: a create that failed after the row
        // went down still left it there.
        if (ddl.txn != nullptr && created_row.page_id != kInvalidPageId) {
            ddl.written.push_back(created_row);
            NoteDdlRows(ddl);
        }
        if (!index_oid.ok()) return {ErrorReply(index_oid.status()), false};
        if (logging(LogLevel::kInfo)) {
            log_->Info("ddl", "created index '" + def.name + "' on " + build.table_name +
                                  ", built by core " + std::to_string(build.owner_core));
        }
        return {CreatedIndexReply(def.name, build.table_name, index_oid.value(), root,
                                  def.key_width, def.entry_width,
                                  " built_by_core=" + std::to_string(build.owner_core),
                                  exec::IndexCreationWarnings(catalog_, def,
                                                              build.key_column_name)),
                false};
    });
    // The commit record is appended (or the scope aborted) by now: the
    // owner's window closes either way, and `committed` publishes the
    // tree. Sent before the durability wait, which the caller takes -
    // index_build_service.hpp on why that order is sound.
    const bool committed = out.response.rfind("ERR ", 0) != 0;
    index_builds_->Done(build.owner_core, build.def.index_oid, committed);
    // DispatchAndStage's read-out of the staged commit, for its reason:
    // the caller waits on `pending_lsn`, never on the member.
    out.pending_lsn = pending_commit_lsn_;
    pending_commit_lsn_ = wal::kNoLsn;
    return out;
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
        return {"ERR " + rows.status().message(), false};
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
    auto parsed = parser::Parse(line);
    if (!parsed.ok()) {
        return {"ERR " + parsed.status().message(), false};
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
            return {"ERR " + cabin_id.status().message(), false};
        }
        // The runtime half. The catalog cannot see the observed sets, and
        // they are unreachable the moment its row is gone - the compiler
        // stops emitting cabin probes for the column - so this frees memory
        // rather than protecting an answer.
        if (cabins_ != nullptr) cabins_->Forget(cabin_id.value());
        // Transactionless like the pattern and assertion routes: the
        // sys.cabins row is logged but no commit record follows it.
        if (Status s = AwaitDdlDurability(); !s.ok()) return {ErrorReply(s), false};
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
        return {"ERR " + result.status().message(), false};
    }
    if (Status s = AwaitDdlDurability(); !s.ok()) return {ErrorReply(s), false};

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
    auto parsed = parser::Parse(line);
    if (!parsed.ok()) {
        return {"ERR " + parsed.status().message(), false};
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
        return {"ERR " + oid.status().message(), false};
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
        return {"ERR " + tables.status().message(), false};
    }
    for (const auto& row : tables.value()) {
        if (row.oid == oid.value() && row.namespace_oid != catalog::kNamespacePublic) {
            return {"ERR '" + stmt.table_name + "' is a system relation and cannot be altered",
                    false};
        }
    }

    // AL4: assertions RESTRICT both forms. The stored canon is the
    // declaration's verbatim text (AS10), and the recovery-side registry
    // rebuild will re-parse it - a rename would leave an *enforcing*
    // constraint whose canon names a vanished table or column. Unlike a
    // pattern, an assertion is not allowed to die quietly. This is
    // AssertionsOnRelation()'s first live call site.
    auto restricting = exec::AssertionsOnRelation(catalog_, page_store_, oid.value());
    if (!restricting.ok()) {
        return {"ERR " + restricting.status().message(), false};
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
        return {"ERR " + renamed.message(), false};
    }
    // Transactionless like the pattern and assertion routes: the renamed
    // catalog rows are logged but no commit record follows them.
    if (Status s = AwaitDdlDurability(); !s.ok()) return {ErrorReply(s), false};

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
        auto parsed = parser::Parse(line);
        if (!parsed.ok()) {
            return {"ERR " + parsed.status().message(), false};
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
            return {"ERR " + oid.status().message(), false};
        }

        // DT3's RESTRICT, both blockers named. A referencing foreign key
        // blocks at the *declared* level - the constraint exists whether or
        // not rows do - which is the check known-gaps.md said was waiting for
        // exactly this caller.
        auto fkeys = catalog_.ListForeignKeys();
        if (!fkeys.ok()) {
            return {"ERR " + fkeys.status().message(), false};
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
            return {"ERR " + restricting.status().message(), false};
        }
        if (!restricting.value().empty()) {
            return {"ERR assertion '" + restricting.value().front().name +
                        "' is declared on this relation; DROP ASSERTION first",
                    false};
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
        if (ddl.txn != nullptr) {
            for (const catalog::CatalogRowChange& c : changed) {
                if (c.deleted) {
                    txn_->NoteDeleteMark(*ddl.txn, static_cast<std::uint32_t>(c.rel_oid),
                                         c.page_id, c.slot, c.oid, c.prior_trx_id,
                                         c.prior_undo_ptr);
                } else {
                    txn_->NoteOverwrite(*ddl.txn, static_cast<std::uint32_t>(c.rel_oid),
                                        c.page_id, c.slot, c.oid, c.prior_trx_id,
                                        c.prior_undo_ptr, c.prior_image);
                }
            }
            // Mark the transaction as holding DDL so `ViewFor` starts
            // filtering - **not** by pushing a fake row into `written`, which
            // would put an insert with an invalid page on the trail and have
            // the abort try to retire it.
            if (!changed.empty()) MarkHoldsDdl(*ddl.txn);
        }
        if (Status s = dropped; !s.ok()) {
            return {"ERR " + s.message(), false};
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

DispatchOutcome CommandDispatcher::HandleAssertion(std::string_view line, Session& session) {
    parser::Parser parser(line);
    auto parsed = parser.Parse();
    if (!parsed.ok()) {
        return {"ERR " + parsed.status().message(), false};
    }
    if (!std::holds_alternative<parser::AssertionStmt>(parsed.value())) {
        return {"ERR expected a CREATE ASSERTION or DROP ASSERTION statement", false};
    }
    const auto& stmt = std::get<parser::AssertionStmt>(parsed.value());

    // A relation another core owns has its Bound Cabin built *there*
    // (PW1c-6c): the cabin is appended to by every write to the relation,
    // and only the relation's owner may write the owner's pages, so a cabin
    // built here would be one nobody can maintain. `BeginForeignAssertionBuild`
    // is the two phases; a dispatcher with no client is a fixture without a
    // ring and is refused by name rather than left to build in the wrong
    // core's pages. The unfiltered row read is `HandleIndex`'s, for its
    // reason: the build touches physical pages whichever view resolved the
    // name, and an unresolvable name falls through to `PrepareAssertionDef`,
    // which owns that refusal and its byte position.
    //
    // **DROP keeps its catalog half here and gains one message**: the
    // registry it evicts from is the owner's now, so a drop that only
    // retired the row would leave the owner enforcing a constraint no row
    // names - the opposite failure to the one this task closes, and just as
    // wrong. The `done(aborted)` leg is exactly "forget this id", so the
    // drop arm below sends it. Fire and forget, with no waiter: a lost
    // message leaves the owner over-enforcing until its next mount, which
    // is the fail-closed side of a message that cannot be acknowledged
    // without a second protocol.
    if (!stmt.drop) {
        if (auto rel_oid = catalog_.FindTableOidByName(stmt.table_name); rel_oid.ok()) {
            auto rel_row = catalog_.GetSysTableRow(rel_oid.value());
            if (rel_row.ok() && rel_row.value().owner_core != core_id_) {
                if (assertion_builds_ != nullptr) {
                    return BeginForeignAssertionBuild(stmt, rel_row.value().owner_core, session);
                }
                return {ErrorReply(Status::Unsupported(
                            "CREATE ASSERTION on '" + stmt.table_name + "' at byte " +
                            std::to_string(stmt.table_byte_offset) +
                            ": the relation is owned by core " +
                            std::to_string(rel_row.value().owner_core) +
                            ", whose writes maintain the Bound Cabin, so the cabin is built "
                            "there (workplan-peer-writer.md §7d, PW1c-6c) - and this "
                            "dispatcher has no assertion-build client to reach it")),
                        false};
            }
        }
    }

    if (stmt.drop) {
        // Which core holds the directory, read **before** the row is
        // retired - afterwards the assertion's target oid is unreadable,
        // and with it the answer to "who has to forget this". A lookup that
        // fails leaves `owner` unset and the drop exactly as it was.
        std::optional<std::uint32_t> owner;
        if (assertion_builds_ != nullptr) {
            if (auto def = exec::FindAssertionByName(catalog_, page_store_, stmt.name);
                def.ok() && def.value().has_value()) {
                if (auto row = catalog_.GetSysTableRow(def.value()->target_oid);
                    row.ok() && row.value().owner_core != core_id_) {
                    owner = row.value().owner_core;
                }
            }
        }
        auto id = exec::DropAssertion(catalog_, page_store_, stmt, wal_);
        if (!id.ok()) {
            return {ErrorReply(id.status()), false};
        }
        if (Status s = AwaitDdlDurability(); !s.ok()) return {ErrorReply(s), false};
        // This core's registry, which holds the directory when the relation
        // is this core's, and the owner's, which holds it otherwise. Both
        // are called: `Evict` is a no-op on an id a registry never held, and
        // naming both is what keeps the drop's effect independent of where
        // the relation lives.
        enforcer_.Evict(id.value());
        if (owner.has_value()) {
            assertion_builds_->Done(*owner, id.value(), /*committed=*/false);
        }
        std::ostringstream os;
        os << "DROPPED ASSERTION name=" << stmt.name << " assertion_id=" << id.value();
        if (logging(LogLevel::kInfo)) {
            log_->Info("ddl", "dropped assertion '" + stmt.name + "'");
        }
        return {os.str(), false};
    }

    // The visibility the build's scan reads under: latest settled state,
    // minted here exactly as a FK check's view is. A dispatcher with no
    // manager reads everything, which is the pre-MVCC engine and what every
    // socket-free test runs on.
    txn::ReadView check_view = txn::ReadView::Everything();
    if (txn_ != nullptr) {
        auto minted = txn_->MintReadView(txn::kNoTrxId);
        if (!minted.ok()) return {ErrorReply(minted.status()), false};
        check_view = minted.value();
    }

    // Through ErrorReply, not a bare "ERR ": the build can refuse with the
    // two coded spellings - ASSERTION_VIOLATION for data already past the
    // bound, TXN_CONFLICT for an unsettled relation - and both are
    // compatibility surfaces a client switches on.
    auto created = exec::CreateAssertion(catalog_, page_store_, stmt, check_view, wal_);
    if (!created.ok()) {
        return {ErrorReply(created.status()), false};
    }
    exec::AssertionDdlResult& result = created.value();
    const bool adopted = result.live.has_value();
    if (adopted) {
        enforcer_.Adopt(std::move(*result.live));
    }
    // After the adoption, deliberately (the review's asymmetry note): a
    // sync failure then answers ERR with the live registry still enforcing
    // what the log already holds - over-enforcing until the operator
    // retries, where the other order left a durably created constraint
    // unenforced on the running instance.
    if (Status s = AwaitDdlDurability(); !s.ok()) return {ErrorReply(s), false};

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

DispatchOutcome CommandDispatcher::BeginForeignAssertionBuild(const parser::AssertionStmt& stmt,
                                                              std::uint32_t owner_core,
                                                              Session& session) {
    // The park would hold the client's transaction open across the owner's
    // whole scan, and the owner would be enforcing a constraint whose row
    // waits on a COMMIT that may never come. Refused by name before
    // anything is sent, exactly as the sibling CREATE INDEX is, and
    // nothing is burned: no id issued, no page written, the transaction as
    // it was. It is a divergence from the local arm, which takes this
    // statement inside a transaction and publishes at once (assertions are
    // not transactional DDL, `docs/spec/ddl-transactional.md` §5) - named
    // here rather than left to be found.
    if (session.in_explicit_txn()) {
        return {ErrorReply(Status::Unsupported(
                    "CREATE ASSERTION on '" + stmt.table_name + "' at byte " +
                    std::to_string(stmt.table_byte_offset) + ": the relation is owned by core " +
                    std::to_string(owner_core) +
                    ", which builds and adopts the Bound Cabin before this statement's row "
                    "exists - inside a transaction that row waits on the client's COMMIT; run "
                    "it in autocommit (workplan-peer-writer.md PW1c-6c)")),
                false};
    }

    // §3.1's checks and the id, on the catalog this core owns, before a
    // byte crosses: a declaration that would have been refused locally is
    // refused without asking a peer to scan a relation for it. The id is
    // issued here - `ASSERT_BUILD` records on the owner carry it - and a
    // burned one is never reissued, the ids' standing rule.
    auto prepared = exec::PrepareAssertionDef(catalog_, page_store_, stmt);
    if (!prepared.ok()) return {ErrorReply(prepared.status()), false};

    const std::uint64_t request_id = next_remote_request_++;
    if (Status s = assertion_builds_->Request(owner_core, request_id,
                                              prepared.value().target_oid,
                                              prepared.value().assertion_id, stmt.source_text);
        !s.ok()) {
        return {ErrorReply(s), false};
    }
    if (logging(LogLevel::kInfo)) {
        log_->Info("ddl", "asked core " + std::to_string(owner_core) +
                              " to build assertion '" + stmt.name + "' on " + stmt.table_name +
                              " (request " + std::to_string(request_id) + ")");
    }
    DispatchOutcome pending;
    pending.pending_assertion_build =
        PendingAssertionBuild{request_id,  owner_core,      prepared.value().assertion_id,
                              prepared.value().target_oid, stmt.name, stmt.table_name,
                              stmt.source_text};
    return pending;
}

DispatchOutcome CommandDispatcher::FinishAssertionBuild(const PendingAssertionBuild& build) {
    const AssertionBuildOutcome* reply = assertion_builds_->Find(build.request_id);
    Status verdict = Status::OK();
    PageId root = kInvalidPageId;
    std::uint64_t rows = 0;
    std::uint32_t groups = 0;
    if (reply == nullptr) {
        verdict = Status::IoError("CREATE ASSERTION on '" + build.table_name +
                                  "': the wait for core " + std::to_string(build.owner_core) +
                                  "'s build was closed under the statement");
    } else if (!reply->arrived) {
        // The deadline. Retryable: a retry builds afresh under a new id,
        // and the owner is told below, so the directory it may have adopted
        // in the meantime does not outlive this statement.
        verdict = Status::TxnConflict(
            "CREATE ASSERTION on '" + build.table_name + "': core " +
            std::to_string(build.owner_core) + " did not reply within " +
            std::to_string(kAssertionBuildReplyDeadlineNs / 1'000'000'000ull) +
            " s; the build is abandoned and the owner told (workplan-peer-writer.md PW1c-6c)");
    } else if (!reply->status.ok()) {
        // The owner's own refusal, code and message intact: an
        // `ASSERTION_VIOLATION` for data already past the bound and a
        // `TXN_CONFLICT` for an unsettled relation are both compatibility
        // surfaces a client switches on, and they must read the same
        // whichever core ran the scan.
        verdict = reply->status.WithContext("CREATE ASSERTION on '" + build.table_name +
                                            "': core " + std::to_string(build.owner_core) +
                                            " refused the build");
    } else {
        root = reply->cabin_root;
        rows = reply->rows_incorporated;
        groups = reply->group_count;
    }
    assertion_builds_->Close(build.request_id);
    if (!verdict.ok()) {
        assertion_builds_->Done(build.owner_core, build.assertion_id, /*committed=*/false);
        if (logging(LogLevel::kWarn)) log_->Warn("ddl", verdict.message());
        return {ErrorReply(verdict), false};
    }

    // Phase 2: the publish, which is the single commit point (§8.1a) and
    // the *only* thing left - the cabin, its base and its directory are all
    // the owner's already. A refusal here (a same-named assertion created
    // while this was parked, the relation dropped) orphans the owner's
    // chain through the `done(aborted)` below.
    if (Status s = exec::InsertAssertion(catalog_, page_store_, wal_, build.assertion_id,
                                         build.target_oid, build.name, build.source_text, root);
        !s.ok()) {
        assertion_builds_->Done(build.owner_core, build.assertion_id, /*committed=*/false);
        return {ErrorReply(s), false};
    }

    // `done(committed)` before the durability wait, `index_build_service.hpp`'s
    // order and this statement's own local stance: a sync failure answers
    // ERR with the owner still enforcing what its log already holds -
    // over-enforcing until the operator retries, where the other order
    // would leave a durably published constraint unenforced on the
    // instance.
    assertion_builds_->Done(build.owner_core, build.assertion_id, /*committed=*/true);
    if (Status s = AwaitDdlDurability(); !s.ok()) return {ErrorReply(s), false};

    std::ostringstream os;
    os << "CREATED ASSERTION name=" << build.name << " assertion_id=" << build.assertion_id
       << " on=" << build.table_name << " cabin_root=" << root << " rows=" << rows
       << " groups=" << groups << " enforcing=" << (kWritePathEnforcesAssertions ? 1 : 0)
       << " built_by_core=" << build.owner_core;
    if (logging(LogLevel::kInfo)) {
        log_->Info("ddl", "created assertion '" + build.name + "' on '" + build.table_name +
                              "', built and enforced by core " +
                              std::to_string(build.owner_core));
    }
    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleShowAssertions() {
    auto rows = exec::ListAssertions(catalog_, page_store_);
    if (!rows.ok()) {
        return {"ERR " + rows.status().message(), false};
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
        // (a root), the write path checks (AST07's constant), and this
        // core's registry holds the directory - which a restart empties
        // until recovery replays it, so a surviving catalog row reports 0
        // rather than claiming a check that cannot run.
        os << " enforcing="
           << ((def.cabin_root != kInvalidPageId && kWritePathEnforcesAssertions &&
                enforcer_.Holds(def.id))
                   ? 1
                   : 0);
        // **And which core that answer is about** (PW1c-6c). A relation
        // another core owns has its Bound Cabin built, held and appended to
        // there, so this core's registry does not hold the directory and
        // `enforcing=0` above means "not by this core" rather than "not at
        // all". The owner is named instead of the claim being made on its
        // behalf: nothing here can see another core's registry, and a `1`
        // printed from a catalog row would be a guess. Ask that core.
        if (auto owner = catalog_.GetSysTableRow(def.target_oid);
            owner.ok() && owner.value().owner_core != core_id_) {
            os << " enforced_by_core=" << owner.value().owner_core;
        }
        // §9's production counters, printed only while the registry holds
        // the assertion: they live and die with the directory, so an
        // unenforced row prints no numbers rather than zeros that would
        // read as "counted, and nothing happened".
        if (const auto* counters = enforcer_.CountersOf(def.id); counters != nullptr) {
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
        if (!all.ok()) return {"ERR " + all.status().message(), false};
        reports = std::move(all.value());
    } else {
        auto oid = catalog_.FindTableOidByName(name);
        if (!oid.ok()) {
            return {"ERR unknown relation '" + std::string(name) + "'", false};
        }
        // A fresh budget at the configured ceiling: the walk is priced like
        // any other relation read, and a spent budget refuses the survey
        // rather than serving a half-count.
        exec::Budget budget(budget_.limit());
        auto one = stats::PlanRelation(catalog_, page_store_, oid.value(), budget, clock_,
                                       decay_half_life_ns_);
        if (!one.ok()) return {"ERR " + one.status().message(), false};
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
        return {"ERR " + rows.status().message(), false};
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
        // values being probed are not the ones being observed.
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

DispatchOutcome CommandDispatcher::HandleShowCabinOptimizer() {
    // PO9's view (workplan PHY06). Rendering only: every number is read
    // from a surface that exists on its own merits - the controller's
    // managed table and decision log, the executor's applied counters, the
    // collector's S3 quality - so the view cannot disagree with the engine
    // about anything, only omit.
    if (cabin_controller_ == nullptr) {
        // Not an empty table: with no controller nothing is managing, and
        // an empty listing would read as "managing nothing yet" when the
        // truth is "not constructed" - SHOW CABINS' `cabins = off` rule.
        return {"CABIN_OPTIMIZER absent (cabins = off)", false};
    }

    const stats::CabinOptimizerConfig& config = cabin_controller_->config();
    std::ostringstream os;
    os << "cabin_optimizer=" << (cabin_optimizer_enabled_ ? "on" : "off")
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

StatusOr<txn::ReadView> CommandDispatcher::CheckView(const WriteScope& scope) {
    // **Minted here, not taken from the statement.** A constraint check reads
    // latest state, so it needs a view of *now*: a parent committed-deleted
    // after this statement's snapshot must still fail the check, and an
    // in-flight writer must be seen rather than looked through (§4).
    //
    // The writer's own id goes in, so a transaction's own uncommitted rows
    // satisfy its own constraints - the table's fourth row, with no special
    // case anywhere.
    if (txn_ == nullptr) return txn::ReadView::Everything();
    return txn_->MintReadView(WriterId(scope));
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

Status CommandDispatcher::CheckForeignKeyOnWrite(const catalog::TableAccess& child,
                                                 const catalog::ForeignKeyRef& fk,
                                                 const parser::AstValue& value,
                                                 const txn::ReadView& check_view) {
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

    auto parent = catalog_.InitTableAccess(fk.rel_oid);
    if (!parent.ok()) return parent.status();

    auto verdict = exec::CheckParentPresent(page_store_, *parent.value(),
                                            static_cast<std::uint64_t>(value.int_val), check_view,
                                            &budget_);
    if (!verdict.ok()) return verdict.status();

    // The pk column, which is the only column a foreign key ever probes (F1).
    RecordFkAccess(exec::AccessKind::kLookup, fk.rel_oid, 1);

    const std::string column =
        fk.column_no < child.schema.columns.size()
            ? std::string(catalog::NameView(child.schema.columns[fk.column_no].name))
            : std::to_string(fk.column_no);

    switch (verdict.value()) {
        case exec::FkVerdict::kPass:
            return Status::OK();
        case exec::FkVerdict::kBusy:
            return Status::TxnConflict("row id=" + std::to_string(value.int_val) + " of '" +
                                       RelationNameOf(fk.rel_oid) +
                                       "' is being written by another transaction, so the "
                                       "foreign key on '" +
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
        return {"ERR " + rows.status().message(), false};
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
        auto parsed = parser::Parse(line);
        if (!parsed.ok()) {
            return {"ERR " + parsed.status().message(), false};
        }
        if (!std::holds_alternative<parser::CreateTableStmt>(parsed.value())) {
            return {"ERR expected a CREATE TABLE statement", false};
        }
        auto& stmt = std::get<parser::CreateTableStmt>(parsed.value());

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
            return {"EXISTS oid=" + std::to_string(existing.value()), false};
        }
        if (existing.status().code() != StatusCode::kNotFound) {
            return {"ERR " + existing.status().message(), false};
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
                return {"ERR " + type_row.status().message(), false};
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
            // declared. Writing `decimal128(p, s)` names the wide type
            // directly and its bounds refuse p <= 18 toward the narrow
            // spelling, so either way one declaration selects exactly one type.
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
                    return {"ERR " + bounds.message() + " (byte " +
                                std::to_string(col.type_byte_offset) + ")",
                            false};
                }
                row.len = catalog::PackDecimalLen(static_cast<std::uint8_t>(col.precision),
                                                  static_cast<std::uint8_t>(col.scale));
            } else if (col.has_precision) {
                // Unreachable through the parser too, and refused rather than
                // ignored: silently dropping the arguments would leave an
                // operator believing they had said something.
                return {"ERR type '" + col.type_name + "' takes no precision or scale (byte " +
                            std::to_string(col.type_byte_offset) + ")",
                        false};
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
            auto parent = catalog_.InitTableAccess(parent_oid.value());
            if (!parent.ok()) {
                return {"ERR " + parent.status().message(), false};
            }

            // The shared declaration checks (catalog/foreign_key.hpp) - the same
            // ones Catalog::CreateForeignKey() applies at the door, so a
            // declaration cannot pass here and fail there.
            if (Status s = catalog::CheckForeignKeyDeclaration(
                    *parent.value(), schema.columns[i], static_cast<std::uint16_t>(i));
                !s.ok()) {
                return {"ERR " + s.message() + " (byte " +
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
            stmt.clustered_given ? stmt.clustered : catalog::ClusteredType::kHeap;

        DdlScope ddl = DdlScopeFor(scope);
        auto oid = catalog_.CreateTable(catalog::kNamespacePublic, stmt.table_name, schema,
                                         clustered, ddl.trx_id, ddl.sink());
        // Registered before the status is read: a create that failed partway
        // still left rows on the page, and those are exactly the rows a
        // rollback has to retire.
        NoteDdlRows(ddl);
        if (!oid.ok()) {
            return {"ERR " + oid.status().message(), false};
        }

        // The constraints, now that there is a child relation to hang them on.
        // What can still fail here is the colocation check (F5), which needs the
        // child's assigned owner core, and catalog I/O. Since D2 the ERR
        // makes FinishDdlStatement abort the statement's transaction, so
        // the relation's own rows are taken back - the message must not
        // claim otherwise (review B2). What the abort does NOT take back
        // is any sys.fkeys row already written in this loop:
        // CreateForeignKey reports no CatalogRowRef, so those rows never
        // reach the trail - the orphan the review named, pre-existing on
        // the explicit-transaction path and recorded in
        // workplan-rv3-catalog-recovery.md's remainder.
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
    if (wal_ == nullptr || durability_ == wal::DurabilityClass::kRelaxed) return Status::OK();
    return wal_->SyncAll();
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

    auto commit = wal_->Commit(txn_id, durability_);
    if (!commit.ok()) return commit.status();

    // kStrict already synced inside Commit(). kGroup did not: it staged
    // the commit for the next drain, and the acknowledgement owed to the
    // client is "durable", so the wait happens here. With one connection
    // the batch is always this one commit and the drain is one sync - the
    // batching only pays off once concurrent committers exist to fill it
    // (manager.hpp). kRelaxed waits for nothing by definition.
    if (durability_ == wal::DurabilityClass::kGroup && !wal_->IsDurable(commit.value())) {
        pending_commit_lsn_ = commit.value();
    }
    return Status::OK();
}

DispatchOutcome CommandDispatcher::HandleInsert(std::string_view line, Session& session) {
    // The write scope is opened before anything is parsed, so that a
    // statement inside an explicit transaction re-mints its read view at
    // the same boundary a SELECT does.
    // ErrorReply, not a bare "ERR ": on a peer a spent transaction-id
    // lease refuses here as TxnConflict, and the wire's `retryable=1` is
    // what the client's retry loop reads.
    auto opened = BeginWrite(session);
    if (!opened.ok()) return {ErrorReply(opened.status()), false};
    WriteScope scope = opened.value();

    DispatchOutcome out = InsertInner(line, scope);

    // Shipped (SS2): this core wrote nothing, so its scope ends without a
    // commit rather than committing an empty transaction - which on a peer
    // would also spend a leased transaction id's commit path for a
    // statement that ran somewhere else. The scope was opened before the
    // parse that found the relation foreign, which is why there is one to
    // end at all.
    if (out.pending_shipped.has_value()) {
        if (Status s = AbandonWriteForShipping(session, scope); !s.ok() && logging(LogLevel::kWarn)) {
            // Not a refusal - the statement is already on its way to its
            // owner, and the only refusal legal after that is
            // `UnknownOutcome`. Logged rather than dropped because
            // `EndWrite`'s abort arm returns without releasing the
            // transaction when the enforcer fails, which is the leaked-
            // transaction class the SS3 review already fixed once.
            log_->Warn("ship", "the local scope of a shipped statement did not end cleanly: " +
                                   s.message());
        }
        return out;
    }

    // The reply is the verdict, which is the same rule Dispatch() itself
    // applies one level up. A statement that answered ERR did not happen as
    // far as the client is concerned, so an autocommit scope unwinds it.
    const bool failed = out.response.rfind("ERR ", 0) == 0;
    Status verdict = failed ? Status::InvalidArgument(out.response) : Status::OK();
    if (Status s = EndWrite(session, scope, verdict); !s.ok() && !failed) {
        return {ErrorReply(s), false};
    }
    return out;
}

bool CommandDispatcher::MayShip(const Session& session) const noexcept {
    return statement_ship_ != nullptr && may_park_ && !session.in_explicit_txn() &&
           !session.shipped();
}

DispatchOutcome CommandDispatcher::ShipStatement(std::string_view line, catalog::Oid oid,
                                                 std::uint32_t owner_core,
                                                 std::string_view relation, Session& session) {
    // The identity the owner's dedup record keys on (D4). Minted on the
    // first ship and kept for the session's life, so a duplicate of *this*
    // statement is recognisable and the session's next statement is not.
    if (session.ship_id() == 0) session.set_ship_id(next_ship_session_id_++);
    const std::uint64_t sequence = session.NextShipSequence();
    const std::uint64_t request_id = next_remote_request_++;

    // `Ship` refuses before it sends - an over-long statement, a core this
    // instance does not have - so this refusal means the statement did not
    // run, and may say so in an ordinary spelling. After it returns OK the
    // only refusal left is `UnknownOutcome`.
    if (Status s = statement_ship_->Ship(owner_core, request_id, session.ship_id(), sequence,
                                         oid, session.role(), line);
        !s.ok()) {
        return {ErrorReply(s), false};
    }
    if (logging(LogLevel::kDebug)) {
        log_->Debug("ship", "core " + std::to_string(core_id_) + " shipped a statement on '" +
                                std::string(relation) + "' to core " +
                                std::to_string(owner_core) + " (request " +
                                std::to_string(request_id) + ", session " +
                                std::to_string(session.ship_id()) + " sequence " +
                                std::to_string(sequence) + ")");
    }
    DispatchOutcome pending;
    pending.pending_shipped =
        PendingShippedStatement{request_id, owner_core, std::string(relation)};
    return pending;
}

DispatchOutcome CommandDispatcher::FinishShippedStatement(
    const PendingShippedStatement& shipped) {
    const ShippedStatementOutcome* reply = statement_ship_->Find(shipped.request_id);
    if (reply == nullptr || !reply->arrived) {
        // The deadline, or a waiter closed under the statement. **Never
        // retryable**: the statement may have committed on its owner, and
        // against engine-issued primary keys a retry inserts a second row.
        // This is the one refusal D4 exists to produce, and the client is
        // told what to do with it instead of being invited to retry.
        statement_ship_->Close(shipped.request_id);
        return {ErrorReply(Status::UnknownOutcome(
                    "core " + std::to_string(shipped.owner_core) + " did not answer for '" +
                    shipped.relation + "' within " +
                    std::to_string(kShippedStatementDeadlineNs / 1'000'000'000ull) +
                    " s; whether the statement ran cannot be established - read the data back "
                    "rather than retrying")),
                false};
    }
    // The owner's own answer, and on the refusal arm its own spelling: the
    // code crossed, so `ErrorReply` here reproduces the line the owner
    // wrote, `retryable` bit included (statement_ship_service.hpp).
    DispatchOutcome out;
    out.response = reply->status.ok() ? reply->text : ErrorReply(reply->status);
    statement_ship_->Close(shipped.request_id);
    return out;
}

namespace {

// **Every relation a chain reads, in one walk.** Three places hold steps
// and all three read real pages: the chain's own steps, its *hoisted*
// sub-chains, and the sub-chains attached to a **step** - which is where
// `step_compiler.cpp` §3 leaves a correlated sub-chain *and* every
// value-bearing uncorrelated one (`IN` / `NOT IN` / the scalar form),
// since their set is row-independent but their comparison is not.
//
// Walking only the first two is a **wrong answer, not a missed refusal**:
// the shared-nothing fault check in `DevicePageStore::ResidentBytes` is
// `#ifndef NDEBUG`, so in a release build a step reading a relation this
// core does not own faults the page anyway and judges its visibility
// against the wrong core's transaction manager. Measured: on a two-core
// rig `SELECT * FROM peer_rel WHERE v IN (SELECT v FROM core0_rel)`
// answered an empty result set instead of the matching row.
//
// `fn` stops the walk by answering false, which both callers below use as
// their refusal.
using StepVisitor = std::function<bool(const exec::Step&)>;

bool VisitRelationSteps(const std::vector<exec::Step>& steps, const StepVisitor& fn) {
    for (const exec::Step& step : steps) {
        if (!fn(step)) return false;
        for (const exec::SubChain& sub : step.sub_chains) {
            if (!VisitRelationSteps(sub.steps, fn)) return false;
        }
    }
    return true;
}

bool VisitRelationSteps(const exec::StepChain& chain, const StepVisitor& fn) {
    for (const exec::SubChain& sub : chain.hoisted) {
        if (!VisitRelationSteps(sub.steps, fn)) return false;
    }
    return VisitRelationSteps(chain.steps, fn);
}

// **A write predicate that reaches a second relation** (`Condition::subquery`,
// parser/ast.hpp). UPDATE and DELETE never compile a chain, so their fork
// resolves the *target* relation's owner and nothing else - a shipped
// `UPDATE t SET ... WHERE v IN (SELECT v FROM u)` would have its row set
// decided on the owner by reading `u`'s pages, which the owner may not own
// and which a release build does not refuse to fault. Measured: on a
// two-core rig that statement answered `UPDATED 0` where the row matched.
//
// Refused rather than resolved: a subquery naming the *same* relation
// would be safe to ship, but proving that means resolving every nested
// SELECT's relations through the catalog on the write path, and what the
// refusal costs is the affinity answer these statements had before
// shipping - never a wrong one.
bool AnySubqueryPredicate(const std::vector<parser::Condition>& where) {
    for (const parser::Condition& cond : where) {
        if (cond.has_subquery()) return true;
    }
    return false;
}

}  // namespace

std::optional<std::uint32_t> CommandDispatcher::SoleForeignOwner(const exec::StepChain& chain) {
    std::optional<std::uint32_t> owner;
    const bool shippable = VisitRelationSteps(chain, [&](const exec::Step& step) {
        auto access = catalog_.InitTableAccess(step.rel_oid);
        if (!access.ok()) return false;
        const std::uint32_t core = access.value()->owner_core;
        // A relation this core owns: no other core can read its pages,
        // so the statement is not shippable whole.
        if (core == core_id_) return false;
        // Two foreign owners: R6's multi-owner statement, which stays
        // refused rather than being split.
        if (owner.has_value() && *owner != core) return false;
        owner = core;
        return true;
    });
    return shippable ? owner : std::nullopt;
}

Status CommandDispatcher::AbandonWriteForShipping(Session& session, WriteScope& scope) {
    return EndWrite(session, scope,
                    Status::Unsupported("the statement was shipped to the core that owns its "
                                        "relation; this core wrote nothing"));
}

Status CommandDispatcher::CheckWriteAffinity(const catalog::TableAccess& access,
                                             std::string_view relation, Session& session) {
    // A write to a relation this core does not own cannot be done here at
    // all - the pages are not this core's to fault, let alone to modify.
    if (access.owner_core != core_id_) {
        cross_core_writes_.Record(session.home_bound() ? session.home_core() : core_id_,
                                  access.owner_core, access.oid);
        return CrossCoreWriteRefused(session.home_bound() ? session.home_core() : core_id_,
                                     access.owner_core, relation);
    }
    // Owned here, but the transaction may already be committed to another
    // core. That is the CC3 restriction proper, and it survives the
    // pipeline: it is what keeps one transaction's writes in one WAL stream.
    if (!session.MayWriteOn(access.owner_core)) {
        cross_core_writes_.Record(session.home_core(), access.owner_core, access.oid);
        return CrossCoreWriteRefused(session.home_core(), access.owner_core, relation);
    }
    // PW1c-5's shape gate, a **whitelist**: on a peer, a write is admitted
    // only where the PW1c-4 grants and this core's own extent lease make it
    // sound - any relation, clustered either way (the btree arm lifted at
    // PW2-4), whose secondary structures are owner-built: an index
    // (PW1c-6b-4) and, since 2026-08-26, a Bound Cabin this core holds the
    // directory for (PW1c-6c). The key-mode arm lifted with the mode (2026-08-25) and
    // its refusal is per row in `InsertOneRow`, so this gate no longer says
    // anything about keys at all. The interim guard this
    // replaced indicted its own blacklist shape ("a page-writing verb
    // added later is admitted by omission"), and the first form of this
    // gate repeated it one level down - it missed assertions, whose entry
    // pages are the system core's (the 25059bf review's C-3). Each named
    // refusal cites the task that lifts it; the tail refusal is what makes
    // a *future* secondary structure refuse rather than slip through.
    // None poisons the session; the backstop below every admitted shape is
    // the store's every-build MayWrite (device_page_store.cpp).
    if (catalog_read_only_) {
        // PW1c-6b-2's window (index_build_service.hpp): an index of this
        // relation is being built here, or built and not yet published by
        // core 0's commit, and a row written now would be in nobody's
        // index. Retryable - `done` closes it. Before the shape gate,
        // because the relation *looks* funded until the commit lands.
        if (pending_index_builds_ != nullptr && pending_index_builds_->Covers(access.oid)) {
            return IndexBuildPending(core_id_, relation);
        }
        // cabin_ids is per-column-parallel with id 0 meaning "no Cabin"
        // (schema.hpp) - emptiness is the wrong test, and so is
        // cabin_mask != 0: a Cabin on a column past 64 folds into no bit.
        const bool any_cabin =
            std::any_of(access.cabin_ids.begin(), access.cabin_ids.end(),
                        [](const catalog::TableAccess::CabinRef& c) { return c.id != 0; });
        // The btree arm lifted 2026-08-24 (PW2-4): a root move writes the
        // relation's own granted anchor page and updates the cache in
        // place - no catalog write remains on the growth path. **The
        // indexed arm lifted 2026-08-25 (PW1c-6b-4)**: a peer-owned
        // relation's index is owner-built (§7c, PW1c-6b-3) - a peer takes
        // no DDL, and there is no migration - so every index page is this
        // core's own, allocated from its lease and stamped by its stream,
        // and maintenance is a local write: `AppendIndexEntry` writes
        // owner-stamped leaves (MayWrite passes on the own stamp) and a
        // root split's `UpdateIndexRoot` writes the relation's granted
        // anchor (PW2-4), the last catalog write off the growth path. What
        // would break this is an index whose tree is *not* the owner's,
        // and the two routes to one are shut: a relation indexed on core 0
        // and then moved needs the mover (R5), which does not exist -
        // `owner_core` is written once, by CreateTable - and a local build
        // against a foreign relation is refused by `CheckIndexDef` itself
        // (catalog.cpp, the 96b0343 review's C4). Where that refusal is
        // keyed off - a hook-less fixture catalog - the tree really is
        // core 0's, and the backstop is the store's: `MayWrite` refuses a
        // page carrying neither this lease, a grant, nor this stream's
        // stamp, so the ending is a refused write, never a torn tree.
        //
        // **The key-mode arm lifted 2026-08-25** with the mode itself
        // (heap-and-tuple.md §4.1). What it was really refusing was the
        // catalog write `AdmitExplicitRowId` makes, and that is a property of
        // the *row*, not of the relation: a row omitting its pk draws from
        // this core's lease and writes no catalog page at all. So the
        // refusal moved to `InsertOneRow`, where the supplied id is in hand -
        // which admits every peer write this gate used to refuse for having
        // the wrong mode declared, and refuses exactly the rows that would
        // have needed the system core's page.
        //
        // **The assertion arm lifted 2026-08-26 (PW1c-6c)**, and what is
        // left in its place is the narrower question it should always have
        // asked. A peer-owned relation's Bound Cabin is owner-built
        // (assertion_build_service.hpp): its pages come from this core's
        // lease, carry this core's stamp, and `ReserveInsert` appends to
        // them as an ordinary local write - so a relation whose assertions
        // *this registry holds* is funded, and refusing it would refuse
        // every write to a constraint this core is enforcing correctly.
        //
        // What still refuses is an assertion this core knows of and cannot
        // enforce (`CannotEnforce`): a cabin core 0 built for this
        // relation before PW1c-6c, whose pages `MayWrite` denies. That is
        // the arm's real predicate, and reading `AnyOn` for it was the
        // defect - `AnyOn` is false on a core whose registry never heard of
        // the assertion, which is exactly the core that must refuse
        // (`bench/v2.2.0/results-shipping-part-a-v2.2.0-11-g925f483.md`
        // Finding 2: a shipped write put a second row in a group under
        // `CHECK COUNT(*) <= 1`).
        const bool funded_shape = access.fkeys_out.empty() && access.fkeys_in.empty() &&
                                  !any_cabin && !enforcer_.CannotEnforce(access.oid);
        if (!funded_shape) {
            if (!access.fkeys_out.empty() || !access.fkeys_in.empty()) {
                return Status::Unsupported(
                    "an FK-linked relation cannot take writes on core " +
                    std::to_string(core_id_) +
                    ": validation reads the linked relation, which this core may not fault "
                    "(workplan-peer-writer.md §4)");
            }
            if (any_cabin) {
                return Status::Unsupported(
                    "a cabined relation cannot take writes on core " +
                    std::to_string(core_id_) +
                    ": whether a Bound Cabin's entry pages follow the grant is unverified "
                    "(workplan-peer-writer.md §4)");
            }
            if (enforcer_.CannotEnforce(access.oid)) {
                return Status::Unsupported(
                    "a relation under an assertion this core cannot enforce cannot take "
                    "writes on core " +
                    std::to_string(core_id_) +
                    ": the assertion's entry pages are the system core's and carry no write "
                    "grant, so admitting the write would leave the constraint unchecked; "
                    "re-create the assertion so its owner builds it "
                    "(workplan-peer-writer.md §7d, PW1c-6c)");
            }
            return Status::Unsupported(
                "this relation's shape is not funded for writes on core " +
                std::to_string(core_id_) + " (workplan-peer-writer.md §8)");
        }
        // PW1c-7's rights probe. The shape is funded; whether the *rights*
        // are here is a separate question, because every grant is
        // memory-resident and a crash before acquisition, a restart or a
        // message lost to the ring leaves a relation this core owns with
        // no writer. The store answers from its lease, its grants and the
        // stamp claim it makes on the read below (ResidentBytes,
        // device_page_store.cpp); a page none of them covers is a creation
        // page core 0 formatted and this core never acquired, which only
        // the system core can re-deliver - so the demand is recorded for
        // the drain tick's request and the statement refused retryably by
        // name, where the store's backstop would name a page id. All three
        // creation pages, because a crash between the grant path's restamp
        // flush and its admission can leave them split. One bit test per
        // page on the funded path. The null test is defence only: the sink
        // is installed at the same site, under the same condition, as
        // `catalog_read_only_` (core_runtime.cpp), so nothing reaches here
        // without one.
        if (grant_demand_ != nullptr) {
            const PageId creation[] = {access.desc_page_id, access.varheap_page_id,
                                       access.anchor_page_id};
            for (PageId page : creation) {
                if (page == kInvalidPageId || page_store_.MayWrite(page)) continue;
                (void)page_store_.GetForRead(page);  // the claim runs on the fault
                if (page_store_.MayWrite(page)) continue;
                grant_demand_->Record(access.oid);
                return RelationWriteRightsPending(core_id_, relation);
            }
        }
    }
    session.BindHomeCore(access.owner_core);
    return Status::OK();
}

DispatchOutcome CommandDispatcher::FinishRemoteRead(const PipelineTag& tag) {
    SessionStepClient::RemoteRead* read = remote_reads_->Find(tag);
    if (read == nullptr) {
        return {ErrorReply(Status::IoError("remote read state vanished before completion")),
                false};
    }
    if (!read->error.ok()) {
        const Status error = read->error;
        remote_reads_->Close(tag);
        return {ErrorReply(error), false};
    }

    // One renderer for both read shapes (P4d-4b-3): what varies is only
    // where the layout, the headings and the types come from - planned by
    // the session (the projected pipeline) or the relation's schema (the
    // P4c star read, whose layout the read leaves empty). Resolved once,
    // then one loop - a second formatter is exactly how the local
    // renderer's own warning says `projection_types` gets forgotten. The
    // decode is checked per field on both shapes (invariant 13 one level
    // up): the P4c edge is the same wire from the same peer, and a width
    // the layout disagrees with is Corruption, never interpreted.
    std::vector<catalog::SysColumnRow> layout = std::move(read->output_layout);
    std::vector<std::string> names = std::move(read->column_names);
    std::vector<std::uint32_t> types = std::move(read->projection_types);
    if (layout.empty()) {
        auto access = catalog_.InitTableAccess(read->rel_oid);
        if (!access.ok()) {
            remote_reads_->Close(tag);
            return {ErrorReply(access.status()), false};
        }
        layout = access.value()->schema.columns;
        names.reserve(layout.size());
        types.reserve(layout.size());
        for (const auto& col : layout) {
            names.emplace_back(catalog::NameView(col.name));
            types.push_back(col.type_val);
        }
    }

    // Byte-identical to the local reply: the header of column names, then
    // one "\n"-escaped comma row per match, FormatValue's rendering.
    std::ostringstream os;
    bool first_col = true;
    for (const std::string& name : names) {
        if (!first_col) os << ',';
        os << name;
        first_col = false;
    }
    for (const auto& batch : read->batches) {
        std::span<const std::byte> rows;
        auto header = DecodeStepBatchHeader(batch, rows);
        if (!header.ok()) {
            remote_reads_->Close(tag);
            return {ErrorReply(header.status()), false};
        }
        auto decoded = wire::DecodeRowBatch(rows, layout.size());
        if (!decoded.ok()) {
            remote_reads_->Close(tag);
            return {ErrorReply(decoded.status()), false};
        }
        for (const auto& row : decoded.value()) {
            os << "\\n";
            bool first_val = true;
            for (std::size_t i = 0; i < layout.size(); ++i) {
                if (!first_val) os << ',';
                auto value = wire::FieldToValueChecked(layout[i], row[i]);
                if (!value.ok()) {
                    remote_reads_->Close(tag);
                    return {ErrorReply(value.status()), false};
                }
                os << exec::FormatValue(types[i], value.value());
                first_val = false;
            }
        }
    }
    remote_reads_->Close(tag);
    return {os.str(), false};
}

Status CommandDispatcher::CheckReadAffinity(const exec::StepChain& chain) {
    // Every step a sub-chain of any kind can reach, through the one walk
    // `VisitRelationSteps` states (above `SoleForeignOwner`). It used to
    // scan the hoisted sub-chains and the chain's own steps only, which
    // left `WHERE x IN (SELECT ... FROM <another core's relation>)` and
    // every correlated sub-chain unchecked - and a release build does not
    // refuse that fault, it performs it.
    Status refusal = Status::OK();
    VisitRelationSteps(chain, [&](const exec::Step& step) {
        auto access = catalog_.InitTableAccess(step.rel_oid);
        if (!access.ok()) {
            refusal = access.status();
            return false;
        }
        if (access.value()->owner_core != core_id_) {
            refusal =
                CrossCoreReadUnsupported(core_id_, access.value()->owner_core, step.rel_name);
            return false;
        }
        return true;
    });
    return refusal;
}

DispatchOutcome CommandDispatcher::InsertInner(std::string_view line, WriteScope& scope) {
    auto parsed = parser::Parse(line);
    if (!parsed.ok()) {
        return {"ERR " + parsed.status().message(), false};
    }
    if (!std::holds_alternative<parser::InsertStmt>(parsed.value())) {
        return {"ERR expected an INSERT statement", false};
    }
    return InsertParsed(std::get<parser::InsertStmt>(parsed.value()), scope, line);
}

DispatchOutcome CommandDispatcher::ExecuteInsert(const parser::InsertStmt& stmt,
                                                 Session& session) {
    // HandleInsert's body around the parsed half: same scope, same verdict
    // rule, so a load chunk and a textual statement are indistinguishable
    // from the write pipeline's side (KW5, BI2).
    auto opened = BeginWrite(session);
    if (!opened.ok()) return {ErrorReply(opened.status()), false};
    WriteScope scope = opened.value();

    // No text, so nothing to ship: a KWP load chunk keeps the cross-core
    // refusal it has always had (command_dispatcher.hpp's note on `line`).
    DispatchOutcome out = InsertParsed(stmt, scope, /*line=*/{});

    const bool failed = out.response.rfind("ERR ", 0) == 0;
    Status verdict = failed ? Status::InvalidArgument(out.response) : Status::OK();
    if (Status s = EndWrite(session, scope, verdict); !s.ok() && !failed) {
        return {ErrorReply(s), false};
    }
    return out;
}

DispatchOutcome CommandDispatcher::InsertParsed(const parser::InsertStmt& stmt,
                                                WriteScope& scope, std::string_view line) {
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
        return {"ERR " + oid.status().message(), false};
    }

    auto access = catalog_.InitTableAccess(oid.value());
    if (!access.ok()) {
        return {"ERR " + access.status().message(), false};
    }
    // Borrowed from the catalog's cache, not owned: valid for this
    // statement, including across AllocateRowId() (catalog.hpp), and
    // refreshed by InsertOneRow when a root repoint invalidates it.
    const catalog::TableAccess* ta = access.value();

    // **The fork** (SS2), whose conditions and their reasons are stated
    // once, on `MayShip` (command_dispatcher.hpp). Here because this is
    // after the shape resolution and before the affinity check, so every
    // refusal that is not about ownership keeps its exact spelling and its
    // exact wire bit.
    if (!line.empty() && ta->owner_core != core_id_ && MayShip(*scope.session)) {
        return ShipStatement(line, ta->oid, ta->owner_core, stmt.table_name, *scope.session);
    }

    // Before anything is written: a relation this core does not own, or a
    // transaction already bound to another core, is refused retryably
    // (crosscore.md CC3, core_affinity.hpp). Once per statement - every
    // row goes to the one relation.
    if (Status affinity = CheckWriteAffinity(*ta, stmt.table_name, *scope.session);
        !affinity.ok()) {
        // ErrorReply, not a bare "ERR ": the affinity refusals are
        // TxnConflict (CrossCoreWriteRefused, RelationWriteRightsPending)
        // and the wire's `TXN_CONFLICT retryable=1` is what a client
        // retries on - all three write sites spelled it without until
        // PW1c-7's test asked for the bit.
        return {ErrorReply(affinity), false};
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
    // through the ordinary per-row path below.
    const std::size_t fill_arity = ta->schema.columns.empty() ? 0 : ta->schema.columns.size() - 1;
    const bool every_row_omits_pk =
        std::all_of(stmt.rows.begin(), stmt.rows.end(),
                    [&](const std::vector<parser::AstValue>& r) { return r.size() == fill_arity; });
    if (bulk && every_row_omits_pk && SortedFillEligible(*ta, oid.value())) {
        return SortedFillInner(stmt, oid.value(), *ta, scope);
    }

    InsertRowResult first{};
    InsertRowResult last{};
    for (std::size_t k = 0; k < stmt.rows.size(); ++k) {
        InsertRowResult row{};
        if (auto err = InsertOneRow(oid.value(), ta, stmt.rows[k], scope, row);
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
                false};
    }
    // The single-row reply, byte-identical to what it always was.
    return {"INSERTED oid=" + std::to_string(oid.value()) + " id=" + std::to_string(last.id) +
                " page=" + std::to_string(last.page_id) + " slot=" + std::to_string(last.slot),
            false};
}

bool CommandDispatcher::SortedFillEligible(const catalog::TableAccess& ta,
                                           catalog::Oid oid) const {
    // The key-mode clause is gone with the mode (heap-and-tuple.md §4.1) and
    // is **replaced by a per-statement one at the call site**, not deleted:
    // this path's whole shape is one contiguous id range carved up front and
    // appended in order, which is wrong for any id the caller names. Whether
    // a caller names one is a fact about the rows now, so the caller checks
    // the rows; what is left here is the relation-shaped half.
    // PW1c-5 (revised at the 25059bf review's S-1): the sorted fill's id
    // block is AllocateRowIdRange's, straight off the catalog page a peer
    // may never write - so a peer takes the ordinary per-row path, which
    // allocates through the lease and works. Ineligibility, not a
    // refusal: the first form refused the statement whole, which was
    // false of what the per-row path could do.
    return !catalog_read_only_ && ta.clustered_type == catalog::ClusteredType::kHeap &&
           ta.varheap_page_id == kInvalidPageId && ta.indexes.empty() && ta.cabin_mask == 0 &&
           !enforcer_.AnyOn(oid);
}

DispatchOutcome CommandDispatcher::SortedFillInner(const parser::InsertStmt& stmt,
                                                   catalog::Oid oid,
                                                   const catalog::TableAccess& ta,
                                                   WriteScope& scope) {
    const std::size_t ncols = ta.schema.columns.size();

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
            auto view = CheckView(scope);
            if (!view.ok()) {
                err = ErrorReply(view.status());
            } else {
                for (const catalog::ForeignKeyRef& fk : ta.fkeys_out) {
                    if (fk.column_no == 0 || fk.column_no > values.size()) continue;
                    if (Status s = CheckForeignKeyOnWrite(ta, fk, values[fk.column_no - 1],
                                                          view.value());
                        !s.ok()) {
                        err = ErrorReply(s);
                        break;
                    }
                }
            }
        }
        if (!err.empty()) {
            return {err + " (row " + std::to_string(k + 1) + ")", false};
        }
    }

    auto first = catalog_.AllocateRowIdRange(oid, stmt.rows.size());
    if (!first.ok()) {
        return {"ERR " + first.status().message(), false};
    }

    // Encoded up front, ids contiguous from the range. The gate excluded
    // spillable schemas, so the sink is never reached.
    std::vector<std::vector<std::byte>> payloads;
    payloads.reserve(stmt.rows.size());
    std::vector<exec::AppendedSpill> no_spills;
    for (std::size_t k = 0; k < stmt.rows.size(); ++k) {
        auto encoded =
            exec::EncodeRow(ta.schema, ta.layout, first.value() + k, stmt.rows[k],
                            exec::VarHeapSink{&page_store_, ta.varheap_page_id, &no_spills,
                                              ta.oid});
        if (!encoded.ok()) {
            return {"ERR " + encoded.status().message() + " (row " + std::to_string(k + 1) + ")",
                    false};
        }
        payloads.push_back(std::move(encoded.value()));
    }

    auto filled = heap::ChainAppendBatch(page_store_, ta.desc_page_id, first.value(), payloads,
                                         WriterId(scope), ta.oid, &ta.heap_tail_hint);
    if (!filled.ok()) {
        return {"ERR " + filled.status().message(), false};
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
            if (!ptr.ok()) return {"ERR " + ptr.status().message(), false};

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
                return {"ERR " + s.message(), false};
            }
        }
    }

    return {"INSERTED oid=" + std::to_string(oid) + " rows=" + std::to_string(stmt.rows.size()) +
                " first_id=" + std::to_string(first.value()) + " last_id=" +
                std::to_string(first.value() + stmt.rows.size() - 1),
            false};
}

std::optional<std::string> CommandDispatcher::InsertOneRow(
    catalog::Oid oid, const catalog::TableAccess*& ta_ptr,
    const std::vector<parser::AstValue>& values, WriteScope& scope, InsertRowResult& out) {
    const catalog::TableAccess& ta = *ta_ptr;

    // ---- Where a peer's leases can refuse, and how that reaches the wire --
    //
    // Every failure below renders through `ErrorReply`, never a bare "ERR ":
    // on a peer the row id comes from the row-id lease, and the var-heap
    // spill, the placement, an index leaf's growth and a full undo page all
    // allocate from the extent lease - each refuses TxnConflict when spent,
    // and `ErrorReply` is the one spelling that puts `retryable=1` on the
    // wire (status.hpp's IsRetryable). The transaction id itself is drawn in
    // BeginWrite or at BEGIN, and rendered the same way there.

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
    if (!ta.fkeys_out.empty()) {
        auto view = CheckView(scope);
        if (!view.ok()) return ErrorReply(view.status());
        for (const catalog::ForeignKeyRef& fk : ta.fkeys_out) {
            if (fk.column_no == 0 || fk.column_no > body.size()) continue;
            if (Status s = CheckForeignKeyOnWrite(ta, fk, body[fk.column_no - 1],
                                                  view.value());
                !s.ok()) {
                return ErrorReply(s);
            }
        }
    }

    // ---- The admission check (docs/spec/assertion.md §6.2 step 2) -------
    //
    // Pure, and before the id for FK's reason: a refused row burns nothing.
    // The reservation (step 3) happens after placement, when the entry has
    // a pk and a location to carry; nothing runs between the two on a
    // cooperative core, so the answer holds - and in a bulk statement this
    // row's admission sees every earlier row's reservation, which is the
    // intra-statement accumulation BI2 exists to keep.
    if (Status s = enforcer_.AdmitInsert(oid, body); !s.ok()) {
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
        // that page is the system core's. A row that omits its pk writes no
        // catalog page at all: AllocateRowId below draws from this core's
        // lease, which is why the omitted arity needs no gate.
        if (catalog_read_only_) {
            return ErrorReply(Status::Unsupported(
                "a caller-supplied primary key cannot be written on core " +
                std::to_string(core_id_) +
                ": admitting one writes the relation's catalog row, the system core's page "
                "(workplan-peer-writer.md §7a) - omit the key and this core issues one"));
        }
        if (Status s = catalog_.AdmitExplicitRowId(oid, supplied_id); !s.ok()) {
            return ErrorReply(s);
        }
        row_id = supplied_id;
    } else {
        auto issued = catalog_.AllocateRowId(oid);
        if (!issued.ok()) {
            return ErrorReply(issued.status());
        }
        row_id = issued.value();
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
    if (Status s = enforcer_.ReserveInsert(page_store_, wal_, WriterId(scope), oid,
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
        return "ERR " + s.message();
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
            return "ERR " + s.message();
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
            auto placed = heap::ChainInsert(page_store_, access.desc_page_id, id, payload,
                                            trx_id, access.oid, &access.heap_tail_hint);
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

Status CommandDispatcher::VisitRelation(
    const catalog::TableAccess& access, storage::PageAccess page_access,
    const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&, std::uint16_t)>&
        fn) {
    switch (access.clustered_type) {
        case catalog::ClusteredType::kHeap:
            return heap::ChainVisit(page_store_, access.desc_page_id, page_access, fn);
        case catalog::ClusteredType::kBtree:
            return btree::BtreeVisit(page_store_, access.desc_page_id, page_access, fn);
    }
    return Status::Corruption("relation oid " + std::to_string(access.oid) +
                              " has an unknown clustered_type");
}

std::optional<std::uint64_t> CommandDispatcher::PkEqualityTarget(
    const catalog::TableAccess& access, const std::vector<parser::Condition>& where) const {
    // Deliberately storage-agnostic: this answers "is this statement a bare
    // pk point lookup", which is a property of the WHERE clause alone.
    // Whether anything can shortcut it - a tree descent, or nothing at
    // all - is LocateByPk's question.
    if (access.schema.columns.empty()) return std::nullopt;
    // Exactly one condition: an extra AND could exclude the row the probe
    // would find, and the probe cannot evaluate the second predicate.
    // Falling through costs a scan; getting this wrong costs a wrong answer.
    if (where.size() != 1) return std::nullopt;

    const parser::Condition& cond = where.front();
    if (cond.op != parser::CompareOp::kEq) return std::nullopt;
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
    if (!opened.ok()) return {ErrorReply(opened.status()), false};
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
    auto view = txn_->MintReadView(txn::kNoTrxId);
    if (!view.ok()) return std::nullopt;
    return view.value();
}

std::optional<DispatchOutcome> CommandDispatcher::RefuseIfNameHeldByPendingDrop(
    std::string_view name, Session& session) {
    // `ViewFor` answers nullopt exactly when no transaction holds
    // uncommitted DDL - and with none open there is no pending drop for a
    // create to collide with, so the fast path pays nothing.
    const std::optional<txn::ReadView> view = ViewFor(session);
    if (!view.has_value()) return std::nullopt;

    auto held = catalog_.NameHeldByPendingDrop(name, *view);
    if (!held.ok()) return DispatchOutcome{"ERR " + held.status().message(), false};
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
        // **System core only.** This core's ReadHorizon() is blind to
        // every other core's readers, and a peer's - no transactions, no
        // leases - answers UINT64_MAX, which would retire a mark whose
        // deleter is live on core 0. Unreachable because peers take no
        // DDL - **enforced at dispatch since PW4** (PeerDdlRefused: the
        // verb guard refuses DDL wherever the store may not write the
        // catalog, which is every production peer) - and this gate
        // stays as the defense in depth that makes the soundness argument
        // local even if a new dispatch path forgets the guard (spec §5d,
        // workplan D1).
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

void CommandDispatcher::MarkHoldsDdl(const txn::Transaction& txn) {
    if (std::find(ddl_txns_.begin(), ddl_txns_.end(), txn.id()) == ddl_txns_.end()) {
        ddl_txns_.push_back(txn.id());
    }
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
        const PkLookup found = LocateByPk(*access.value(), pk);
        if (found.kind != PkLookup::Kind::kAt) {
            // kAbsent means the row is gone, kScan means the relation has no
            // descent to ask. Neither is a location, and rollback may not
            // guess one - the caller reports rather than compensating a slot
            // it cannot vouch for.
            return Status::NotFound("row id " + std::to_string(pk) + " of relation oid " +
                                    std::to_string(rel_oid) +
                                    " could not be relocated for rollback");
        }
        return txn::TransactionManager::RowLocation{found.at.page_id, found.at.slot};
    };
}

CommandDispatcher::PkLookup CommandDispatcher::LocateByPk(const catalog::TableAccess& access,
                                                          std::uint64_t pk) {
    if (access.clustered_type == catalog::ClusteredType::kBtree) {
        // The tree *is* the relation's storage, so its answer is
        // authoritative in both directions: a hit is where the row lives,
        // and a NotFound means no such row - the scan it replaces would
        // visit the same leaf and find the same nothing. This is the one
        // place a point lookup may skip the scan on a miss, and it is
        // allowed precisely because it is not a hint.
        auto found = btree::BtreeLookup(page_store_, access.desc_page_id, pk);
        if (found.ok()) {
            // Carrying the leaf out is what keeps the caller from asking
            // the store for a page the descent just held.
            return PkLookup{
                PkLookup::Kind::kAt,
                TupleLocation{found.value().page_id, found.value().slot}};
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
        return {"ERR " + view.status().message(), false};
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
            if (!found.ok()) return {"ERR " + found.status().message(), false};
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
        if (!at.ok()) return {"ERR " + at.status().message(), false};
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
    const exec::StepChain& chain, std::ostringstream& os, exec::TrailCollector* trail,
    const exec::TrailReplay* replay, const std::optional<stats::InstanceKey>& instance,
    const txn::Snapshot& snapshot) {
    if (Status s = aggregator_.Reset(*chain.aggregate, chain.column_names, aggregate_limits_);
        !s.ok()) {
        return {"ERR " + s.message(), false};
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
        &exec_stats_, budget_, trail, replay, cabins_, &snapshot, indexes_enabled_);
    if (!ran.ok()) {
        // **No trail on the failure path**, exactly as the unaggregated
        // path has it: a statement that stopped part way through touched
        // some tuples, and a trail describing that points a later reader at
        // a state no reader should be pointed at.
        return {"ERR " + ran.message(), false};
    }

    Status emitted = aggregator_.Finish(
        [&](std::span<const parser::AstValue> row) -> Status {
            os << "\\n";
            bool first = true;
            for (std::size_t i = 0; i < row.size(); ++i) {
                if (!first) os << ',';
                // One item per output value, in written order - the fold
                // emits `spec.items` and nothing else - so the item's
                // `type_val` is this value's column type. `MIN(d)` renders
                // as a date; `COUNT(*)` carries type_val 0 and renders as
                // the integer it is.
                const std::uint32_t type_val =
                    i < chain.aggregate->items.size() ? chain.aggregate->items[i].type_val : 0;
                os << exec::FormatValue(type_val, row[i]);
                first = false;
            }
            return Status::OK();
        });
    if (!emitted.ok()) {
        return {"ERR " + emitted.message(), false};
    }

    // Recorded after a *complete* execution, and unconditionally - the fold
    // is downstream of all three, so an aggregated statement records the
    // trail, the access shape and the signals its unaggregated twin would.
    RecordExecution(instance, trail, chain, exec_stats_);
    return {os.str(), false};
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
                                              const txn::Snapshot& snapshot) {
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
            return {"ERR " + s.message(), false};
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
        &stats, budget_, trail, replay, cabins_, &snapshot, indexes_enabled_);
    if (!ran.ok()) {
        return {"ERR " + ran.message(), false};
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

    // The statement's own pattern_id, in the same hex CREATE PATTERN
    // returns. This is what closes the "I declared it, why doesn't traffic
    // match" loop: an operator compares the number here against the one the
    // declaration printed, and equality *is* the answer - no trail recorder
    // has to exist for that comparison to be meaningful.
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
    // `ErrorReply`, not a bare "ERR ": `SnapshotFor` can refuse with a
    // TxnConflict (a spent transaction-id lease on a peer), and the
    // wire's `retryable=1` is what a client's retry loop reads. The
    // DELETE site has always rendered it this way; these two did not,
    // so the same refusal carried the bit on one verb and lost it on
    // the other (the SS2 review's cut 2).
    if (!snapshot.ok()) return {ErrorReply(snapshot.status()), false};

    // An explicit Parser rather than the free `Parse()`, so the statement's
    // fingerprint can be taken **from the parse itself** (parser.hpp). It
    // used to come from `FingerprintOf`, which lexed the text a second time -
    // measured at ~13% of a point join's latency, three times what the
    // recording it was for actually cost, and the whole of replay's B+ tree
    // regression (bench/results-waystone-v2.md).
    parser::Parser parser(line);
    auto parsed = parser.Parse();
    if (!parsed.ok()) {
        return {"ERR " + parsed.status().message(), false};
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
        bool any_join_is_view = false;
        for (const parser::JoinClause& join : stmt.joins) {
            if (!join.relation.schema.empty()) any_join_is_view = true;
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
        return {"ERR unknown schema '" + stmt.from.schema +
                    "'; the only qualified relations are the catalog views under `sys`",
                false};
    }

    // V17: parse -> compile -> execute. Everything that used to be
    // decided here - which relation, which access path, where each
    // predicate is evaluated - was settled by the compiler and is sitting
    // in the chain. What is left is formatting.
    const std::optional<txn::ReadView> resolve_view = ViewFor(session);
    auto chain =
        exec::Compile(catalog_, stmt, resolve_view.has_value() ? &*resolve_view : nullptr);
    if (!chain.ok()) {
        return {"ERR " + chain.status().message(), false};
    }

    // The plan is resolved; now ask whether this core may run it
    // (crosscore.md §2's fast-path-versus-pipeline decision). Every
    // relation local is the fast path; a single-step star read of a
    // relation another core owns ships to that core (workplan P4c) - and
    // everything else that spans cores keeps the affinity refusal below.
    //
    // The eligible class is deliberately narrow and each exclusion is a
    // correctness statement, not a shortcut: an aggregate would fold on
    // the wrong core's sink; a quota (LIMIT/OFFSET) applies at emission
    // and the remote side emits everything; **a sort applies at emission
    // too** (OB4) - the remote reply is the owning core's emission order,
    // and the local sink the sorter decorates is never reached, so shipping
    // a sorted statement would answer it unordered; ANALYZE describes a
    // local run it did not perform; a projection list needs the projection
    // types the remote whole-row batch does not carry yet. Every excluded
    // shape is refused exactly as before, never mis-run.
    //
    // `sorted()` and not `stmt.order_by.empty()`: an `ORDER BY <pk> ASC`
    // the compiler elided asks for the order this path already returns, so
    // it stays eligible - **except** when the elision leaned on
    // `emit_in_key_order`. That flag is how an out-of-order-keyed relation's walk
    // is made to emit in key order (heap-and-tuple.md §4.1), and
    // `EncodeStepDescriptor` does not carry it, so a shipped step would
    // walk in slot order and answer the clause wrongly. Refused here rather
    // than encoded: the wire format is versioned, and an honest affinity
    // refusal beats a reordered reply.
    if (remote_reads_ != nullptr && !analyze && chain.value().steps.size() == 1 &&
        chain.value().hoisted.empty() && chain.value().star() &&
        !chain.value().aggregated() && !chain.value().sorted() &&
        !chain.value().limit.has_value() && chain.value().offset == 0) {
        const exec::Step& step = chain.value().steps[0];
        auto owner_access = catalog_.InitTableAccess(step.rel_oid);
        if (owner_access.ok() && owner_access.value()->owner_core != core_id_ &&
            step.sub_chains.empty() && !step.emit_in_key_order) {
            auto tag = remote_reads_->Open(step, owner_access.value()->owner_core,
                                           next_remote_request_++);
            if (tag.ok()) {
                DispatchOutcome pending;
                pending.pending_remote = tag.value();
                return pending;
            }
            // A step the descriptor refuses falls through to the honest
            // refusal rather than a worse error. An index or Cabin probe
            // is no longer in that class - the session ships it as the
            // walk it would fall back to (ShippedForm) - so what
            // remains here is the genuinely unshippable.
        }
    }

    // The two-step pipeline (P4d-4b-3, widened by 4c's gated inner walk).
    // **What may ship is stated once, in `TwoStepPipelineEligible`** -
    // every shape rule, and the reason each is a correctness statement
    // rather than a shortcut, lives beside the plan it governs. What is
    // left here is the two questions it cannot answer: whether this
    // dispatcher can ship at all, and whether anything is actually on
    // another core.
    //
    // **Order matters, and it is measured.** The eligibility test is
    // chain-only and free; the two `InitTableAccess` calls below are not.
    // Asking the cheap question first is what keeps a local two-step
    // statement - one that will never ship - from paying two catalog
    // lookups to be told so (`bench/results-p4d-executor.md` §10.8 named
    // this after a revision that had it the other way round). A plan or
    // an open the machinery refuses falls through to the honest affinity
    // refusal below, never a worse error.
    if (remote_reads_ != nullptr && !analyze && TwoStepPipelineEligible(chain.value()).ok()) {
        auto outer_access = catalog_.InitTableAccess(chain.value().steps[0].rel_oid);
        auto inner_access = catalog_.InitTableAccess(chain.value().steps[1].rel_oid);
        if (outer_access.ok() && inner_access.ok() &&
            (outer_access.value()->owner_core != core_id_ ||
             inner_access.value()->owner_core != core_id_)) {
            auto plan = BuildTwoStepPipeline(
                chain.value(), outer_access.value()->schema, inner_access.value()->schema,
                outer_access.value()->owner_core, inner_access.value()->owner_core, core_id_,
                next_remote_request_++);
            if (plan.ok()) {
                if (auto tag = remote_reads_->OpenPipeline(std::move(plan.value())); tag.ok()) {
                    DispatchOutcome pending;
                    pending.pending_remote = tag.value();
                    return pending;
                }
            }
        }
    }
    if (Status affinity = CheckReadAffinity(chain.value()); !affinity.ok()) {
        // D1's read half, and the same mechanism as the write half rather
        // than a second one. It sits **below** the two pipeline paths
        // above, which keep their precedence and their measurements; this
        // catches what falls through them, which on a peer is every plain
        // read of a foreign relation (pretasks §8c-1 measured that the
        // pipeline is not reachable from dispatch for one).
        //
        // **Not under ANALYZE**, for the reason the two pipeline paths
        // above exclude it and one more: `line` here is the *stripped*
        // text, so shipping it would send a bare `SELECT` and answer a
        // request for a plan with a result set. Refused exactly as it was
        // before shipping existed; shipping the `ANALYZE` spelling itself
        // is a separate decision, since the owner would then describe a
        // run this core did not perform.
        if (!analyze && MayShip(session)) {
            if (std::optional<std::uint32_t> owner = SoleForeignOwner(chain.value());
                owner.has_value() && !chain.value().steps.empty()) {
                return ShipStatement(line, chain.value().steps[0].rel_oid, *owner,
                                     chain.value().steps[0].rel_name, session);
            }
        }
        return {"ERR " + affinity.message(), false};
    }

    // Same one-line-per-response contract as SHOW PAGE: a header line of
    // column names, then one "\n"-escaped section per matching row
    // (comma-joined values), never a raw newline byte.
    std::ostringstream os;
    bool first_col = true;
    for (const std::string& name : chain.value().column_names) {
        if (!first_col) os << ',';
        os << name;
        first_col = false;
    }

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
    if (analyze) return RunAnalyze(compiled, trail, replay_ptr, instance, snapshot.value().snap);

    // ---- AG1: the fold wraps the sink, and nothing else moves -----------
    //
    // Everything above this point ran unchanged and unconditionally for an
    // aggregated statement: the compile, the affinity check, the Waystone
    // lookup, the trail collector. The fold is strictly downstream of all
    // of them, which is what makes AG10's "recording, replay, Cabin probes
    // and access statistics hold unchanged" a structural fact rather than a
    // list of things that were remembered.
    if (compiled.aggregated()) {
        return RunAggregated(compiled, os, trail, replay_ptr, instance, snapshot.value().snap);
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
        if (!access.ok()) return {"ERR " + access.status().message(), false};
        star_access = access.value();
    }

    // Rendering one row of the reply. Shared by the two paths below so the
    // sorted and unsorted replies are formatted by one routine - the bug
    // this shape avoids is a sorted statement rendering a DATE as an epoch
    // day because a second formatter forgot `projection_types`.
    std::string row_scratch;
    auto render = [&](const exec::ChainFrame& frame, std::string& out) {
        out.clear();
        bool first_val = true;
        if (star_access != nullptr) {
            for (std::size_t i = 0; i < star_access->schema.columns.size(); ++i) {
                if (!first_val) out += ',';
                out += exec::FormatValue(
                    star_access->schema.columns[i].type_val,
                    frame.Get(exec::ColumnRef{0, 0, static_cast<std::uint16_t>(i)}));
                first_val = false;
            }
        } else {
            for (std::size_t i = 0; i < compiled.projection.size(); ++i) {
                if (!first_val) out += ',';
                out += exec::FormatValue(compiled.projection_types[i],
                                         frame.Get(compiled.projection[i]));
                first_val = false;
            }
        }
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
                    sorter_.Take(row_scratch);
                }
                return storage::VisitControl::kContinue;
            }
            const exec::QuotaVerdict verdict = quota.Note();
            if (verdict == exec::QuotaVerdict::kStop) return storage::VisitControl::kStop;
            if (verdict == exec::QuotaVerdict::kSkip) return storage::VisitControl::kContinue;
            render(frame, row_scratch);
            os << "\\n" << row_scratch;
            return verdict == exec::QuotaVerdict::kEmitThenStop
                       ? storage::VisitControl::kStop
                       : storage::VisitControl::kContinue;
        },
        &exec_stats_, budget_, trail, replay_ptr, cabins_, &snapshot.value().snap, indexes_enabled_);
    if (!ran.ok()) {
        // **No trail on the failure path.** A statement that errored part
        // way through touched some tuples and then stopped; a trail
        // describing that is a trail describing a state no reader should
        // ever be pointed at (workplan P10).
        return {"ERR " + ran.message(), false};
    }

    // The order exists only now, so the quota is applied here rather than
    // in the sink - and it is the same quota object, so `LIMIT n OFFSET m`
    // still means rows [m, m+n) of the reply the unlimited statement gives.
    // What changed is which reply that is: the sorted one.
    if (sorter_.active()) {
        sorter_.Finish();
        exec::DrainSorted(quota, sorter_.rows(),
                          [&](const exec::OutputSort::Row& row) { os << "\\n" << row.text; });
    }

    RecordExecution(instance, trail, compiled, exec_stats_);

    if (logging(LogLevel::kTrace)) {
        log_->Trace("query", "chain of " + std::to_string(compiled.steps.size()) +
                                 " step(s), class " +
                                 std::to_string(static_cast<int>(compiled.klass)));
    }
    return {os.str(), false};
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
    auto opened = BeginWrite(session);
    if (!opened.ok()) return {ErrorReply(opened.status()), false};
    WriteScope scope = opened.value();

    // The read view this UPDATE filters through. An UPDATE reads before it
    // writes, and it must not see a row a SELECT in the same transaction
    // would not - so it takes the snapshot the same way.
    auto snapshot = SnapshotFor(session);
    // `ErrorReply`, not a bare "ERR ": `SnapshotFor` can refuse with a
    // TxnConflict (a spent transaction-id lease on a peer), and the
    // wire's `retryable=1` is what a client's retry loop reads. The
    // DELETE site has always rendered it this way; these two did not,
    // so the same refusal carried the bit on one verb and lost it on
    // the other (the SS2 review's cut 2).
    if (!snapshot.ok()) return {ErrorReply(snapshot.status()), false};

    DispatchOutcome out = UpdateInner(line, scope, snapshot.value().snap);

    // Shipped: HandleInsert's branch, for its reason.
    if (out.pending_shipped.has_value()) {
        if (Status s = AbandonWriteForShipping(session, scope); !s.ok() && logging(LogLevel::kWarn)) {
            // Not a refusal - the statement is already on its way to its
            // owner, and the only refusal legal after that is
            // `UnknownOutcome`. Logged rather than dropped because
            // `EndWrite`'s abort arm returns without releasing the
            // transaction when the enforcer fails, which is the leaked-
            // transaction class the SS3 review already fixed once.
            log_->Warn("ship", "the local scope of a shipped statement did not end cleanly: " +
                                   s.message());
        }
        return out;
    }

    const bool failed = out.response.rfind("ERR ", 0) == 0;
    Status verdict = failed ? Status::InvalidArgument(out.response) : Status::OK();
    if (Status s = EndWrite(session, scope, verdict); !s.ok() && !failed) {
        return {ErrorReply(s), false};
    }
    return out;
}

DispatchOutcome CommandDispatcher::UpdateInner(std::string_view line, WriteScope& scope,
                                               const txn::Snapshot& snapshot) {
    auto parsed = parser::Parse(line);
    if (!parsed.ok()) {
        return {"ERR " + parsed.status().message(), false};
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
        return {"ERR " + oid.status().message(), false};
    }

    auto access = catalog_.InitTableAccess(oid.value());
    if (!access.ok()) {
        return {"ERR " + access.status().message(), false};
    }
    // Borrowed from the catalog's cache, not owned: valid for this
    // statement, including across AllocateRowId() (catalog.hpp).
    const catalog::TableAccess& ta = *access.value();

    // **The fork** (SS2): `MayShip` states the conditions and why each is
    // one. Here, after the shape resolution and before the affinity check,
    // so every refusal that is not about ownership keeps its spelling and
    // its wire bit - and a WHERE naming a second relation is refused with
    // them, since this fork resolves nothing about it
    // (`AnySubqueryPredicate` says why).
    if (ta.owner_core != core_id_ && !AnySubqueryPredicate(stmt.where) &&
        MayShip(*scope.session)) {
        return ShipStatement(line, ta.oid, ta.owner_core, stmt.table_name, *scope.session);
    }

    // Before anything is written: a relation this core does not own, or a
    // transaction already bound to another core, is refused retryably
    // (crosscore.md CC3, core_affinity.hpp).
    if (Status affinity = CheckWriteAffinity(ta, stmt.table_name, *scope.session); !affinity.ok()) {
        return {ErrorReply(affinity), false};  // the retryable spelling, as INSERT's site says
    }

    // Resolve the SET list before touching storage, so a bad target fails
    // clean with no partial update. Both refusals - an unknown column, and
    // the primary key (K2, `Unsupported`) - live in the compiler beside
    // CompileWhere rather than here: the two halves of an UPDATE's compile
    // belong at one layer, and a check the dispatcher owns is one a second
    // write path can be written without.
    if (Status s = exec::CompileAssignments(ta, stmt.assignments); !s.ok()) {
        return {ErrorReply(s), false};
    }

    // The WHERE clause compiles to the same resolved predicates a chain
    // step carries, and is evaluated by the same evaluator (V16). UPDATE
    // reads one relation, so the frame has one step.
    auto predicates = exec::CompileWhere(catalog_, ta, stmt.table_name, stmt.where);
    if (!predicates.ok()) {
        return {"ERR " + predicates.status().message(), false};
    }
    const std::vector<const catalog::Schema*> schemas = {&ta.schema};
    exec::ChainFrame frame;
    frame.Open(schemas, /*parent=*/nullptr);

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

    // Minted once per statement rather than per row. Nothing can join or
    // leave the live set while this statement runs: a write to this relation
    // can only come from the core that owns it, which is this one, and it is
    // running this statement (crosscore.md CC3).
    txn::ReadView check_view = txn::ReadView::Everything();
    if (!fk_assignments.empty()) {
        auto view = CheckView(scope);
        if (!view.ok()) return {ErrorReply(view.status()), false};
        check_view = view.value();
    }

    std::uint32_t updated = 0;
    std::uint32_t pages_touched = 0;
    PageId last_page = kInvalidPageId;

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

        std::vector<exec::PendingSpill> spills;
        auto row = exec::DecodeRow(ta.schema, ta.layout, tuple.value().payload, &spills);
        if (!row.ok()) return row.status();

        // Decoded a second time into the frame so the predicates read it
        // by index. UPDATE rewrites the row afterwards, which is why it
        // keeps an owned copy as well - the frame is for evaluation only.
        std::vector<exec::PendingSpill> frame_spills;
        if (Status s = exec::DecodeRowInto(ta.schema, ta.layout, tuple.value().payload,
                                            frame.SlotsFor(0), &frame_spills);
            !s.ok()) {
            return s;
        }

        // The pk is read here, while the payload span is still in hand, so
        // nothing below needs it (row_codec.hpp's R1 split).
        auto id = exec::RowKeystoneId(tuple.value().payload);
        if (!id.ok()) return id.status();
        const std::uint64_t trx_id = tuple.value().trx_id;
        const std::uint64_t undo_ptr = tuple.value().undo_ptr;

        // Spilled values are fetched only now, after everything that needed
        // the tuple's own bytes is done with them: resolving mid-decode
        // would put a var-heap fetch under a live page span.
        if (Status s = exec::ResolveSpills(page_store_, spills, row.value()); !s.ok()) return s;
        if (Status s = exec::ResolveSpills(page_store_, frame_spills, frame.SlotsFor(0));
            !s.ok()) {
            return s;
        }
        auto matched = exec::EvaluateConjuncts(catalog_, page_store_, schemas, predicates.value(),
                                               frame, /*stats=*/nullptr, budget_, &snapshot);
        if (!matched.ok()) return matched.status();
        if (!matched.value()) return Status::OK();

        // ---- First-updater-wins (docs/spec/txn.md section 5) ----------------
        //
        // Checked only once the row has qualified: a conflict is reported
        // about a row this statement actually wanted, never about one it
        // scanned past. No lock and no wait - the verdict is a pure
        // function of the tuple's current writer and this view.
        if (scope.txn != nullptr) {
            if (Status s = txn_->CheckWriteConflict(*scope.txn, trx_id, id.value()); !s.ok()) {
                return s;
            }
        }

        // ---- The forward check, for an fk column this SET touches (§2) --
        //
        // Per matched row, after the row has qualified and the cheap
        // conflict check has passed. The *value* is statement-constant - a
        // SET assigns a literal - so this repeats one descent per row where
        // one would do; hoisting it is safe only for an UPDATE that matches
        // at least one row, which is not known until one does, and the
        // probe memo that would have absorbed the repetition is in the step
        // VM this path does not use (fk_check.hpp's amendment).
        for (const auto& [fk, value] : fk_assignments) {
            if (Status s = CheckForeignKeyOnWrite(ta, *fk, *value, check_view); !s.ok()) return s;
        }

        // The row as it stands *before* the SET list is applied, kept only
        // when this relation has a Cabin - it is what tells the write hook
        // whether a key column actually moved (§5's third row). A relation
        // with no Cabin copies nothing.
        // ...and what tells the index hook the same thing (index.md §2).
        // A relation with neither copies nothing.
        std::vector<parser::AstValue> previous;
        if ((cabins_ != nullptr && ta.cabin_mask != 0) || !ta.indexes.empty() ||
            enforcer_.AnyOn(ta.oid)) {
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
        if (enforcer_.AnyOn(ta.oid)) {
            if (Status s = enforcer_.AdmitAndReserveUpdate(page_store_, wal_, WriterId(scope),
                                                           ta.oid, previous, row.value(),
                                                           id.value(), page_id, slot);
                !s.ok()) {
                return s;
            }
        }

        // The spills this re-encode appended, collected so they can be logged
        // below. **They were not collected before, and so not logged at all**:
        // an UPDATE that spilled a value wrote the bytes into a var-heap page
        // and told the log nothing, leaving a recovered tuple whose cell
        // points at bytes no record describes (`docs/inflight/known-gaps.md`'s var-heap
        // entry, hole 3). Collected only when there is a log to write them to,
        // the same condition the index half above uses.
        //
        // `appended_spills`, not `spills`: the enclosing scope already has a
        // `PendingSpill` list, which is the opposite direction - values this
        // statement *read* out of the var-heap to evaluate the WHERE clause.
        std::vector<exec::AppendedSpill> appended_spills;
        auto encoded = exec::EncodeRow(
            ta.schema, ta.layout, id.value(), body,
            exec::VarHeapSink{&page_store_, ta.varheap_page_id,
                              wal_ != nullptr ? &appended_spills : nullptr, ta.oid});
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
        const PkLookup found = LocateByPk(ta, *pk);
        if (found.kind == PkLookup::Kind::kAbsent) {
            return {"UPDATED 0", false};  // no such row, on the tree's authority
        }
        if (found.kind == PkLookup::Kind::kAt) {
            // Get(), not the span the locator carried out: the descent
            // fetched that leaf read-only, so writing through it would
            // leave the frame clean and the overwrite would never be
            // written back. Same frame, one hash lookup, dirty flag set.
            auto bytes = page_store_.Get(found.at.page_id);
            if (bytes.ok()) {
                heap::PageView page(bytes.value().bytes());
                if (Status s = apply(found.at.page_id, page, found.at.slot); !s.ok()) {
                    return {ErrorReply(s), false};
                }
                if (logging(LogLevel::kTrace)) {
                    log_->Trace("query", "pk " + std::to_string(*pk) + " updated at " +
                                             std::to_string(found.at.page_id) + ":" +
                                             std::to_string(found.at.slot));
                }
                return {"UPDATED " + std::to_string(updated), false};
            }
        }
    }

    Status scan = VisitRelation(
        ta, storage::PageAccess::kWrite,
        [&](PageId page_id, heap::PageView& page,
            std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            // UPDATE has no early exit: it must consider every row, since
            // the WHERE is evaluated per tuple and any of them may match.
            if (Status s = apply(page_id, page, slot); !s.ok()) return s;
            return storage::VisitControl::kContinue;
        });
    if (!scan.ok()) {
        // Partial **within the statement**, which is section 6's stated
        // rule rather than an exposure now. In autocommit EndWrite() aborts
        // this scope, so the rows already updated are compensated and the
        // statement is atomic after all. Inside an explicit transaction
        // they stay written and the session is poisoned - the client must
        // ROLLBACK, which undoes all of them.
        return {ErrorReply(scan), false};
    }

    if (updated > 0 && logging(LogLevel::kTrace)) {
        log_->Trace("heap", "overwrite rows=" + std::to_string(updated) + " across " +
                                std::to_string(pages_touched) + " page(s) of table oid " +
                                std::to_string(oid.value()));
    }
    return {"UPDATED " + std::to_string(updated), false};
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

    // `BEGIN [TRANSACTION] [ISOLATION LEVEL <name>]`. The level given here
    // overrides the session's for this transaction only - the third rung of
    // the same precedence chain `durability` uses.
    txn::IsolationLevel level = session.isolation();
    auto [word, rest] = SplitFirstToken(args);
    if (IEquals(word, "TRANSACTION") || IEquals(word, "WORK")) {
        std::tie(word, rest) = SplitFirstToken(rest);
    }
    if (!word.empty()) {
        if (!IEquals(word, "ISOLATION")) {
            return {"ERR expected ISOLATION LEVEL after BEGIN, got '" + std::string(word) + "'",
                    false};
        }
        auto [level_word, name] = SplitFirstToken(rest);
        if (!IEquals(level_word, "LEVEL")) {
            return {"ERR expected LEVEL after ISOLATION", false};
        }
        auto parsed = txn::ParseIsolationLevel(Trim(name));
        if (!parsed.ok()) return {"ERR " + parsed.status().message(), false};
        level = parsed.value();
    }

    auto begun = txn_->Begin(level);
    // ErrorReply, not a bare "ERR ": on a peer the id this draws comes from
    // the transaction-id lease, and a spent one refuses TxnConflict. Inside
    // an explicit transaction that refusal lands *here* rather than at the
    // INSERT - the id is drawn once, at BEGIN - so this is the site the
    // wire's `retryable=1` has to reach for a transactional client.
    if (!begun.ok()) return {ErrorReply(begun.status()), false};
    session.Adopt(begun.value());

    return {"BEGIN trx_id=" + std::to_string(begun.value()->id()) + " isolation=" +
                txn::IsolationLevelName(level),
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

    txn::Transaction* txn = session.transaction();
    const std::uint64_t id = txn->id();

    // The transaction's reservations become committed entries (§6.2 step
    // 4): flags cleared, ASSERT_COMMIT logged, before the commit record. A
    // failure leaves the transaction open - the client may retry COMMIT or
    // ROLLBACK, and the pending set is untouched until one succeeds.
    if (Status s = enforcer_.CommitTxn(page_store_, wal_, id); !s.ok()) {
        return {ErrorReply(s), false};
    }
    auto committed = txn_->Commit(*txn, durability_);

    if (!committed.ok()) {
        // **A failed commit must abort, not merely be reported.**
        // `Commit` returns on a logging failure *before* it clears
        // `active_`, and `Release` refuses to erase an active
        // transaction - so without this the transaction sits in `live_`
        // for the life of the process: in every future read view's
        // in-flight set, counting against `kMaxTrackedLiveTxns`, and
        // since DT9 answering `IsInFlight` true forever, which would keep
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
            return {"ERR " + committed.status().message() +
                        " (and rolling it back failed: " + rolled.message() + ")",
                    false};
        }
        return {"ERR " + committed.status().message(), false};
    }

    // Its catalog rows are committed now, so every reader may see them
    // unfiltered again (DT3c). Before `Finish()`, which clears the
    // session's transaction pointer this reads.
    EndDdlScope(session);
    session.Finish();

    // The durability wait the client is owed, for the same reason
    // LogInsert() takes it: kGroup staged the commit for the next drain,
    // and the acknowledgement means "durable".
    if (wal_ != nullptr && durability_ == wal::DurabilityClass::kGroup &&
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
    txn::Transaction* txn = session.transaction();
    const std::uint64_t id = txn->id();

    // The reservations first (§6.2 step 5): each one removed from its
    // group, ASSERT_ROLLBACK logged, before the undo trail replays - so the
    // directory and the pages unwind in the same statement the rows do.
    if (Status s = enforcer_.AbortTxn(page_store_, wal_, id); !s.ok()) {
        return {ErrorReply(s), false};
    }
    Status aborted = txn_->Abort(*txn, RowLocatorForRollback());
    // Its catalog rows were retired by that abort, so there is nothing
    // left for anyone to be isolated from - and nothing that may still be
    // cached about them (DT3c, DT4). Before `Finish()`, which clears the
    // pointer this reads.
    EndDdlScope(session);
    session.Finish();
    txn_->Release(*txn);
    if (!aborted.ok()) return {"ERR " + aborted.message(), false};
    return {"ROLLBACK trx_id=" + std::to_string(id), false};
}

DispatchOutcome CommandDispatcher::HandleSetIsolation(std::string_view args, Session& session) {
    auto [level_word, name] = SplitFirstToken(args);
    if (!IEquals(level_word, "LEVEL")) {
        return {"ERR expected LEVEL after SET ISOLATION", false};
    }
    auto parsed = txn::ParseIsolationLevel(Trim(name));
    if (!parsed.ok()) return {"ERR " + parsed.status().message(), false};

    // Applies to the *next* transaction, never the open one: changing what
    // a running transaction's read view means halfway through would make
    // its earlier statements unexplainable.
    if (session.in_explicit_txn()) {
        return {"ERR cannot change the isolation level inside a transaction", false};
    }
    session.set_isolation(parsed.value());
    return {std::string("SET isolation=") + txn::IsolationLevelName(parsed.value()), false};
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
    // transaction - the same one every shipped pipeline stage mints, and
    // leased because that seam leases (manager.hpp says why, and says
    // plainly that *this* holder's statement never parks with it: the
    // dispatch path is synchronous, and a statement that ships a read
    // drops this object at its `pending_remote` return, before the wait).
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
        return scope;
    }

    auto begun = txn_->Begin(session.isolation());
    if (!begun.ok()) return begun.status();
    scope.txn = begun.value();
    scope.owned = true;
    return scope;
}

Status CommandDispatcher::EndWrite(Session& session, WriteScope& scope, const Status& result) {
    if (scope.txn == nullptr) {
        // No manager: every statement is its own transaction under
        // kBootstrapXid, and this is its end - so its assertion
        // reservations settle here, exactly as a real transaction's do
        // below. One statement at a time per core is what makes the shared
        // id safe.
        return result.ok()
                   ? enforcer_.CommitTxn(page_store_, wal_, catalog::kBootstrapXid)
                   : enforcer_.AbortTxn(page_store_, wal_, catalog::kBootstrapXid);
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
        if (!result.ok()) session.Poison();
        return Status::OK();
    }

    // Autocommit: this statement is the whole transaction, so behaviour
    // here *is* statement-atomic - reservations included, on both arms.
    if (!result.ok()) {
        if (Status s = enforcer_.AbortTxn(page_store_, wal_, scope.txn->id()); !s.ok()) {
            return s;
        }
        Status aborted = txn_->Abort(*scope.txn, RowLocatorForRollback());
        txn_->Release(*scope.txn);
        scope.txn = nullptr;
        return aborted;
    }

    // Flags before the commit record (§6.2 step 4's piggyback): if the
    // commit then fails, the abort arm removes the entries whatever their
    // flags say, so clearing early is recoverable in both directions.
    if (Status s = enforcer_.CommitTxn(page_store_, wal_, scope.txn->id()); !s.ok()) {
        return s;
    }
    auto committed = txn_->Commit(*scope.txn, durability_);
    if (!committed.ok()) {
        txn_->Release(*scope.txn);
        scope.txn = nullptr;
        return committed.status();
    }
    if (wal_ != nullptr && durability_ == wal::DurabilityClass::kGroup &&
        !wal_->IsDurable(committed.value())) {
        pending_commit_lsn_ = committed.value();
    }
    txn_->Release(*scope.txn);
    scope.txn = nullptr;
    return Status::OK();
}

std::uint64_t CommandDispatcher::WriterId(const WriteScope& scope) {
    // No manager means no transaction, and every row carries the
    // always-visible id - which is exactly what the engine did before
    // MVCC, and why a dispatcher without one behaves identically.
    return scope.txn != nullptr ? scope.txn->id() : catalog::kBootstrapXid;
}

DispatchOutcome CommandDispatcher::HandleDelete(std::string_view line, Session& session) {
    auto opened = BeginWrite(session);
    if (!opened.ok()) return {ErrorReply(opened.status()), false};
    WriteScope scope = opened.value();

    auto snapshot = SnapshotFor(session);
    if (!snapshot.ok()) return {ErrorReply(snapshot.status()), false};

    DispatchOutcome out = DeleteInner(line, scope, snapshot.value().snap);

    // Shipped: HandleInsert's branch, for its reason.
    if (out.pending_shipped.has_value()) {
        if (Status s = AbandonWriteForShipping(session, scope); !s.ok() && logging(LogLevel::kWarn)) {
            // Not a refusal - the statement is already on its way to its
            // owner, and the only refusal legal after that is
            // `UnknownOutcome`. Logged rather than dropped because
            // `EndWrite`'s abort arm returns without releasing the
            // transaction when the enforcer fails, which is the leaked-
            // transaction class the SS3 review already fixed once.
            log_->Warn("ship", "the local scope of a shipped statement did not end cleanly: " +
                                   s.message());
        }
        return out;
    }

    const bool failed = out.response.rfind("ERR ", 0) == 0;
    Status verdict = failed ? Status::InvalidArgument(out.response) : Status::OK();
    if (Status s = EndWrite(session, scope, verdict); !s.ok() && !failed) {
        return {ErrorReply(s), false};
    }
    return out;
}

DispatchOutcome CommandDispatcher::DeleteInner(std::string_view line, WriteScope& scope,
                                               const txn::Snapshot& snapshot) {
    auto parsed = parser::Parse(line);
    if (!parsed.ok()) return {"ERR " + parsed.status().message(), false};
    if (!std::holds_alternative<parser::DeleteStmt>(parsed.value())) {
        return {"ERR expected a DELETE statement", false};
    }
    const auto& stmt = std::get<parser::DeleteStmt>(parsed.value());

    // As INSERT and UPDATE (DT3c): a write resolves under its session.
    const std::optional<txn::ReadView> view =
        scope.session != nullptr ? ViewFor(*scope.session) : std::nullopt;
    auto oid =
        catalog_.FindTableOidByName(stmt.table_name, view.has_value() ? &*view : nullptr);
    if (!oid.ok()) return {"ERR " + oid.status().message(), false};
    auto access = catalog_.InitTableAccess(oid.value());
    if (!access.ok()) return {"ERR " + access.status().message(), false};
    const catalog::TableAccess& ta = *access.value();

    // **The fork** (SS2): `MayShip` states the conditions and why each is
    // one. Here, after the shape resolution and before the affinity check,
    // so every refusal that is not about ownership keeps its spelling and
    // its wire bit - and a WHERE naming a second relation is refused with
    // them, since this fork resolves nothing about it
    // (`AnySubqueryPredicate` says why).
    if (ta.owner_core != core_id_ && !AnySubqueryPredicate(stmt.where) &&
        MayShip(*scope.session)) {
        return ShipStatement(line, ta.oid, ta.owner_core, stmt.table_name, *scope.session);
    }

    // Before anything is marked: same rule as INSERT and UPDATE
    // (crosscore.md CC3). A delete-mark is a write.
    if (Status affinity = CheckWriteAffinity(ta, stmt.table_name, *scope.session); !affinity.ok()) {
        return {ErrorReply(affinity), false};  // the retryable spelling, as INSERT's site says
    }

    // The same WHERE compilation UPDATE uses, so a DELETE's predicate means
    // exactly what the SELECT that found the rows meant.
    auto predicates = exec::CompileWhere(catalog_, ta, stmt.table_name, stmt.where);
    if (!predicates.ok()) return {"ERR " + predicates.status().message(), false};
    const std::vector<const catalog::Schema*> schemas = {&ta.schema};
    exec::ChainFrame frame;
    frame.Open(schemas, /*parent=*/nullptr);

    // Minted once per statement, for the reason UPDATE's copy records.
    txn::ReadView check_view = txn::ReadView::Everything();
    if (!ta.fkeys_in.empty()) {
        auto view = CheckView(scope);
        if (!view.ok()) return {ErrorReply(view.status()), false};
        check_view = view.value();
    }

    std::uint32_t deleted = 0;

    auto mark = [&](PageId page_id, heap::PageView& page, std::uint16_t slot) -> Status {
        auto tuple = page.ReadTuple(slot);
        if (!tuple.ok()) return Status::OK();  // dead or out-of-range slot - skip

        // Already delete-marked, or invisible to this reader: either way
        // there is no version here to delete.
        if (txn::Classify(snapshot.view, tuple.value()) == txn::Visibility::kNoVersion) {
            return Status::OK();
        }

        std::vector<exec::PendingSpill> frame_spills;
        if (Status s = exec::DecodeRowInto(ta.schema, ta.layout, tuple.value().payload,
                                            frame.SlotsFor(0), &frame_spills);
            !s.ok()) {
            return s;
        }
        auto id = exec::RowKeystoneId(tuple.value().payload);
        if (!id.ok()) return id.status();
        const std::uint64_t trx_id = tuple.value().trx_id;
        const std::uint64_t undo_ptr = tuple.value().undo_ptr;

        // Resolved only now, after everything that needed the tuple's own
        // bytes is done with them - the same R1 split every read path uses.
        if (Status s = exec::ResolveSpills(page_store_, frame_spills, frame.SlotsFor(0));
            !s.ok()) {
            return s;
        }
        auto matched = exec::EvaluateConjuncts(catalog_, page_store_, schemas, predicates.value(),
                                               frame, /*stats=*/nullptr, budget_, &snapshot);
        if (!matched.ok()) return matched.status();
        if (!matched.value()) return Status::OK();

        if (scope.txn != nullptr) {
            if (Status s = txn_->CheckWriteConflict(*scope.txn, trx_id, id.value()); !s.ok()) {
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
            if (Status s = CheckNoChildrenBeforeDelete(ta, id.value(), check_view); !s.ok()) {
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
        if (enforcer_.AnyOn(ta.oid)) {
            if (Status s = enforcer_.ReserveDelete(page_store_, wal_, WriterId(scope), ta.oid,
                                                   frame.SlotsFor(0), id.value(), page_id, slot);
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

            auto ptr = txn_->AppendUndo(*scope.txn, rec, id.value(), {});
            if (!ptr.ok()) return ptr.status();
            new_trx_id = scope.txn->id();
            new_undo_ptr = ptr.value();

            txn_->NoteDeleteMark(*scope.txn, ta.oid, page_id, slot, id.value(), trx_id, undo_ptr);
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
        const PkLookup found = LocateByPk(ta, *pk);
        if (found.kind == PkLookup::Kind::kAbsent) {
            return {"DELETED 0", false};
        }
        if (found.kind == PkLookup::Kind::kAt) {
            auto bytes = page_store_.Get(found.at.page_id);
            if (bytes.ok()) {
                heap::PageView page(bytes.value().bytes());
                if (Status s = mark(found.at.page_id, page, found.at.slot); !s.ok()) {
                    return {ErrorReply(s), false};
                }
                return {"DELETED " + std::to_string(deleted), false};
            }
        }
    }

    Status scan = VisitRelation(
        ta, storage::PageAccess::kWrite,
        [&](PageId page_id, heap::PageView& page,
            std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            if (Status s = mark(page_id, page, slot); !s.ok()) return s;
            return storage::VisitControl::kContinue;
        });
    if (!scan.ok()) return {ErrorReply(scan), false};

    return {"DELETED " + std::to_string(deleted), false};
}

}  // namespace kds::server
