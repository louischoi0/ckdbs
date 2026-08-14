#include "kds/server/command_dispatcher.hpp"

#include "kds/exec/type_literals.hpp"
#include "kds/server/mount_recovery.hpp"  // SHOW META's recovery block (RC09)

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

// ---- The error surface (docs/txn.md section 5, protocol.md section 11) ---
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
std::string ErrorReply(const Status& status) {
    if (status.code() == StatusCode::kTxnConflict) {
        return "ERR TXN_CONFLICT retryable=1 " + status.message();
    }
    // A constraint the statement broke, as opposed to a race it lost. Given
    // a spelling of its own for the same reason TXN_CONFLICT has one - a
    // client library switches on it - and carrying `retryable=0` explicitly
    // rather than by omission, so the two look alike where they are read.
    if (status.code() == StatusCode::kFkViolation) {
        return "ERR FK_VIOLATION retryable=0 " + status.message();
    }
    // The third constraint spelling (docs/feat-assertion.md §4.4, AS9),
    // shaped exactly like FK_VIOLATION and for its reason. Nothing produces
    // this Status until AST07 compiles the admission check into the write
    // paths; the spelling lands first because it is a compatibility surface,
    // and a client written against it must not see the message arrive as a
    // bare "ERR ..." in the meantime.
    if (status.code() == StatusCode::kAssertionViolation) {
        return "ERR ASSERTION_VIOLATION retryable=0 " + status.message();
    }
    return "ERR " + status.message();
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
    *out = DispatchAndStage(line, session);

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

// ---- The durability wait (docs/wal.md D2) --------------------------------
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

// The statement-class → role table (role.hpp's model, docs/protocol.md
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
    auto [cmd, rest] = SplitFirstToken(line);

    if (cmd.empty()) {
        return {"ERR empty command", false};
    }

    // ---- Authorization (role.hpp; docs/protocol.md §14) -----------------
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

    // ---- The failed-txn gate (docs/txn.md section 10-8) -----------------
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
        if (IEquals(sub, "TABLES")) return HandleListTables();
        if (IEquals(sub, "PAGE")) return HandleShowPage(sub_rest);
        if (IEquals(sub, "PATTERNS")) return HandleShowPatterns();
        if (IEquals(sub, "ACCESS")) return HandleShowAccess();
        if (IEquals(sub, "BUDGET")) return HandleShowBudget();
        if (IEquals(sub, "CABINS")) return HandleShowCabins();
        if (IEquals(sub, "INDEXES")) return HandleShowIndexes();
        if (IEquals(sub, "FKEYS")) return HandleShowFkeys();
        if (IEquals(sub, "ASSERTIONS")) return HandleShowAssertions();
        if (IEquals(sub, "RELAYOUT")) return HandleShowRelayout(sub_rest);
        if (IEquals(sub, "CABIN_OPTIMIZER")) return HandleShowCabinOptimizer();
        return {"ERR unknown SHOW target", false};
    }
    if (IEquals(cmd, "DESCRIBE") || IEquals(cmd, "DESC")) {
        return HandleDescribe(rest);
    }
    if (IEquals(cmd, "CREATE")) {
        auto [sub, sub_rest] = SplitFirstToken(rest);
        if (IEquals(sub, "PATTERN")) {
            return HandleCreatePattern(Trim(line));
        }
        if (IEquals(sub, "CABIN")) {
            return HandleCabin(Trim(line));
        }
        if (IEquals(sub, "ASSERTION")) {
            return HandleAssertion(Trim(line));
        }
        // `UNIQUE` routes here too, so its refusal comes from the parser
        // with the byte offset of the word itself rather than from this
        // layer as "unknown CREATE target".
        if (IEquals(sub, "INDEX") || IEquals(sub, "UNIQUE")) {
            return HandleIndex(Trim(line));
        }
        if (IEquals(sub, "TABLE")) {
            // Disambiguate the bare-name form ("CREATE TABLE foo")
            // from the SQL form ("CREATE TABLE foo (col type, ...)"): the
            // bare form's argument is just a name, so it never contains
            // '(' - the SQL grammar always does (ast.hpp: a column list is
            // mandatory). Route on that rather than trying to parse both
            // ways and see which succeeds.
            if (sub_rest.find('(') != std::string_view::npos) {
                return HandleCreateTableSql(Trim(line));
            }
            return HandleCreateTable(sub_rest);
        }
        return {"ERR unknown CREATE target", false};
    }
    if (IEquals(cmd, "ALTER")) {
        return HandleAlter(Trim(line));
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
            return HandleIndex(Trim(line));
        }
        if (IEquals(sub, "ASSERTION")) {
            return HandleAssertion(Trim(line));
        }
        if (IEquals(sub, "TABLE")) {
            return HandleDropTable(Trim(line));
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
       << " cabin_optimizer=" << (cabin_optimizer_enabled_ ? "on" : "off");

    // The last recovery, for the operator who has to answer "what did the
    // restart do" (RC09, `docs/wal.md` §13). Absent rather than zeroed when no
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
        // RV3's report, both halves. `relations_missing_pages` is the one that
        // is computable; `catalog_recovered=0` is the standing statement that
        // the converse - rows whose relation the crash erased - is **not
        // detectable**, because no page names its relation
        // (mount_recovery.hpp). Printed as a flag rather than left unsaid, so
        // "recovery succeeded" is never read as "nothing was lost".
        os << " recovery_relations_checked=" << recovery_->relations_checked
           << " recovery_relations_missing_pages=" << recovery_->relations_missing_pages
           << " catalog_recovered=0";
        // RC07: what the mount could resume enforcing, and the honest
        // remainder. A surviving declaration whose directory could not be
        // rebuilt is counted here and left *out* of the registry, so
        // SHOW ASSERTIONS reports `enforcing=0` for it rather than a
        // constraint that would admit every write.
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

DispatchOutcome CommandDispatcher::HandleListTables() {
    auto tables = catalog_.ListTables();
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

    auto page = page_store_.Get(page_id);
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
    // this class's header comment / docs/client-manual.md section 2), so a
    // raw newline byte can't appear here - it would desync any client that
    // reads "up to the next \n" as one reply. Instead, sections are joined
    // with the two-character escape "\n" (backslash + n); the bundled CLI
    // (tools/ckdbs_cli.py) unescapes it back into real newlines before
    // printing, giving a readable multi-line dump for developers without
    // breaking the one-line-per-response contract on the wire.
    std::ostringstream os;
    os << "page_id=" << page_id << "\\n"
       << "page_type=" << (is_leaf ? "BTREE_LEAF" : "HEAP") << "\\n"
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

DispatchOutcome CommandDispatcher::HandleCreateTable(std::string_view args) {
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

    catalog::Schema schema;
    // kAssigned by name: this legacy command has no syntax for a key mode
    // and is not gaining one (the SQL path owns DDL surface), so the
    // relation it makes is engine-keyed and says so.
    auto oid = catalog_.CreateTable(catalog::kNamespacePublic, args, schema,
                                     catalog::ClusteredType::kHeap,
                                     catalog::KeyMode::kAssigned);
    if (!oid.ok()) {
        return {"ERR " + oid.status().message(), false};
    }
    return {"CREATED oid=" + std::to_string(oid.value()), false};
}

DispatchOutcome CommandDispatcher::HandleDescribe(std::string_view args) {
    if (args.empty()) {
        return {"ERR DESCRIBE requires a table name", false};
    }

    auto oid = catalog_.FindTableOidByName(args);
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
    std::ostringstream os;
    os << "oid=" << oid.value() << " root_page_id=" << table_row.value().desc_page_id
       << " clustered_type=" << clustered
       << " key_mode=" << catalog::KeyModeName(table_row.value().key_mode)
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
        auto height = btree::BtreeHeight(page_store_, table_row.value().desc_page_id);
        auto leaves = btree::BtreeLeafCount(page_store_, table_row.value().desc_page_id);
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

        // The pk is autoincrement only where the engine is the one issuing
        // it. On an EXPLICIT relation the caller names the id and the
        // cursor is merely admitted past it (heap-and-tuple.md section 4.1),
        // so `yes` here would describe a sequence that never runs - and a
        // field that is convenient to print is not a reason to print
        // something untrue.
        const bool autoincrement =
            is_pk && table_row.value().key_mode == catalog::KeyMode::kAssigned;
        os << "\\n"
           << "pos=" << col.pos << " name=" << catalog::NameView(col.name) << " type=" << type_name
           << " notnull=" << (col.notnull ? "yes" : "no")
           << " pk=" << (is_pk ? "yes" : "no")
           << " autoincrement=" << (autoincrement ? "yes" : "no");

        // The declared cabin policy (docs/feat-cabin.md), printed for every
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
            // (docs/impl-foreign-keys.md §1). Read from the relation's own
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

    auto result = exec::CreatePattern(catalog_, page_store_, stmt);
    if (!result.ok()) {
        return {"ERR " + result.status().message(), false};
    }

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

    auto pattern_id = exec::DropPattern(catalog_, page_store_, stmt.name);
    if (!pattern_id.ok()) {
        return {"ERR " + pattern_id.status().message(), false};
    }

    std::ostringstream os;
    os << "DROPPED PATTERN name=" << stmt.name << " pattern_id=0x" << std::hex
       << pattern_id.value() << std::dec;
    if (logging(LogLevel::kInfo)) {
        log_->Info("ddl", "dropped pattern '" + stmt.name + "'");
    }
    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleIndex(std::string_view line) {
    parser::Parser parser(line);
    auto parsed = parser.Parse();
    if (!parsed.ok()) {
        return {"ERR " + parsed.status().message(), false};
    }
    if (!std::holds_alternative<parser::IndexStmt>(parsed.value())) {
        return {"ERR expected a CREATE INDEX or DROP INDEX statement", false};
    }
    const auto& stmt = std::get<parser::IndexStmt>(parsed.value());

    if (stmt.drop) {
        auto index_oid = exec::DropIndex(catalog_, stmt);
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

    auto result = exec::CreateIndex(catalog_, page_store_, stmt);
    if (!result.ok()) {
        return {"ERR " + result.status().message(), false};
    }

    std::ostringstream os;
    // `entries=0` is printed for the reason `observed=0` is on a fresh
    // Cabin: it is the thing most likely to be misunderstood. Creating an
    // index over an empty relation indexes nothing, and only the rows
    // written after it exist in it.
    os << "CREATED INDEX name=" << stmt.index_name << " on=" << stmt.table_name
       << " index_oid=" << result.value().index_oid
       << " root_page=" << result.value().root_page_id
       << " key_width=" << result.value().key_width
       << " entry_width=" << result.value().entry_width << " entries=0";
    for (const std::string& warning : result.value().warnings) {
        os << "\\n" << "WARN " << warning;
    }
    if (logging(LogLevel::kInfo)) {
        log_->Info("ddl", "created index '" + stmt.index_name + "' on " + stmt.table_name);
    }
    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleShowIndexes() {
    auto rows = catalog_.ListIndexes();
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

        os << " root_page=" << row.root_page_id << " key_width=" << row.key_width
           << " entry_width=" << row.entry_width;

        // The physical half. Height and entry count are what say whether an
        // index is worth its write hook, and the catalog cannot answer
        // either: it stores that an index exists, never what is in it.
        const index::IndexLayout layout{row.key_width,
                                        static_cast<std::uint16_t>(row.entry_width -
                                                                   row.key_width -
                                                                   index::kIndexPkWidth)};
        auto height = index::IndexHeight(page_store_, row.root_page_id, layout);
        auto entries = index::IndexEntryCount(page_store_, row.root_page_id, layout);
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

DispatchOutcome CommandDispatcher::HandleAlter(std::string_view line) {
    auto parsed = parser::Parse(line);
    if (!parsed.ok()) {
        return {"ERR " + parsed.status().message(), false};
    }
    if (!std::holds_alternative<parser::AlterStmt>(parsed.value())) {
        return {"ERR expected an ALTER TABLE statement", false};
    }
    const auto& stmt = std::get<parser::AlterStmt>(parsed.value());

    auto oid = catalog_.FindTableOidByName(stmt.table_name);
    if (!oid.ok()) {
        return {"ERR " + oid.status().message(), false};
    }

    // AL7: the catalog's own names are load-bearing for bootstrap and are
    // nobody's to change - refused here so both forms share the answer,
    // and RenameTable's own guard is defense rather than the door.
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

DispatchOutcome CommandDispatcher::HandleDropTable(std::string_view line) {
    auto parsed = parser::Parse(line);
    if (!parsed.ok()) {
        return {"ERR " + parsed.status().message(), false};
    }
    if (!std::holds_alternative<parser::DropTableStmt>(parsed.value())) {
        return {"ERR expected a DROP TABLE statement", false};
    }
    const auto& stmt = std::get<parser::DropTableStmt>(parsed.value());

    auto oid = catalog_.FindTableOidByName(stmt.table_name);
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
    if (Status s = catalog_.DropTable(oid.value(), dropped_cabins); !s.ok()) {
        return {"ERR " + s.message(), false};
    }
    // The catalog rows are gone and the compiler stops emitting probes;
    // the in-memory sets would only leak, so they are forgotten, not
    // protected (feat-cabin.md - un-observing is always legal).
    if (cabins_ != nullptr) {
        for (const std::uint64_t cabin_id : dropped_cabins) cabins_->Forget(cabin_id);
    }

    if (logging(LogLevel::kInfo)) {
        log_->Info("ddl", "dropped table " + stmt.table_name + " (oid " +
                              std::to_string(oid.value()) +
                              "); pages orphaned pending reclamation");
    }
    return {"DROPPED TABLE " + stmt.table_name + " oid=" + std::to_string(oid.value()), false};
}

DispatchOutcome CommandDispatcher::HandleAssertion(std::string_view line) {
    parser::Parser parser(line);
    auto parsed = parser.Parse();
    if (!parsed.ok()) {
        return {"ERR " + parsed.status().message(), false};
    }
    if (!std::holds_alternative<parser::AssertionStmt>(parsed.value())) {
        return {"ERR expected a CREATE ASSERTION or DROP ASSERTION statement", false};
    }
    const auto& stmt = std::get<parser::AssertionStmt>(parsed.value());

    if (stmt.drop) {
        auto id = exec::DropAssertion(catalog_, page_store_, stmt, wal_);
        if (!id.ok()) {
            return {ErrorReply(id.status()), false};
        }
        enforcer_.Evict(id.value());
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
    // The physical optimizer's shadow report (docs/feat-physical-optimizer.md
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

    for (const catalog::SysCabinRow& row : rows.value()) {
        os << "\\n";
        os << "cabin_id=" << row.cabin_id;

        // Names resolved here rather than stored, exactly as SHOW ACCESS
        // does: the row holds oids so it stays fixed width, and an
        // inspection surface can afford the lookup.
        auto access = catalog_.InitTableAccess(row.rel_oid);
        os << " rel=";
        bool named = false;
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

// ---- Foreign-key checks (docs/impl-foreign-keys.md §§2-4) ----------------

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
    // failed here: the row codec refuses it a moment later with a message
    // about the column's declared type, which is the better error - a type
    // mistake reported as a constraint violation sends the reader looking at
    // the wrong table.
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

DispatchOutcome CommandDispatcher::HandleCreateTableSql(std::string_view line) {
    auto parsed = parser::Parse(line);
    if (!parsed.ok()) {
        return {"ERR " + parsed.status().message(), false};
    }
    if (!std::holds_alternative<parser::CreateTableStmt>(parsed.value())) {
        return {"ERR expected a CREATE TABLE statement", false};
    }
    auto& stmt = std::get<parser::CreateTableStmt>(parsed.value());

    auto existing = catalog_.FindTableOidByName(stmt.table_name);
    if (existing.ok()) {
        return {"EXISTS oid=" + std::to_string(existing.value()), false};
    }
    if (existing.status().code() != StatusCode::kNotFound) {
        return {"ERR " + existing.status().message(), false};
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

        catalog::SysColumnRow row{};
        row.pos = pos++;
        catalog::SetName(row.name, col.name);
        row.type_val = type_row.value().type_val;
        row.len = type_row.value().len;
        row.notnull = true;  // no NULL support yet - see row_codec.hpp
        row.cabin_policy = col.cabin_policy;

        // ---- decimal(p, s) (docs/spec-types.md TY2, TY9) ----------------
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

        auto parent_oid = catalog_.FindTableOidByName(col.references_table);
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

    // ---- Resolving the two trailing words (docs/heap-and-tuple.md §4.1) --
    //
    // A written word always wins; `default_key_mode` decides only what
    // silence means. The instance-wide setting exists so a database whose
    // keys come from outside says so once instead of on every statement,
    // and it must never be able to change what a statement that *did* name
    // a mode does.
    const catalog::KeyMode key_mode =
        stmt.key_mode_given ? stmt.key_mode : default_key_mode_;

    // Storage follows the resolved mode when the statement named neither.
    // An explicit relation must be btree-clustered, so under an explicit
    // default a bare `CREATE TABLE t (...)` has to mean BTREE EXPLICIT -
    // otherwise the default would be a configuration whose every
    // unqualified statement is refused, which is not a default at all.
    // A written storage word still wins, including one that contradicts the
    // mode: that is the refusal below, and it belongs to the writer.
    const catalog::ClusteredType clustered =
        stmt.clustered_given ? stmt.clustered
        : (key_mode == catalog::KeyMode::kExplicit ? catalog::ClusteredType::kBtree
                                                   : catalog::ClusteredType::kHeap);

    // **An EXPLICIT relation must be BTREE-clustered**, and the refusal is
    // here rather than in the parser because it is a statement about where
    // rows can be put, not about how the words go together: the grammar
    // takes the two trailing words in either order and neither one's
    // meaning depends on the other. Unsupported, not InvalidArgument - the
    // combination is understood and declined.
    //
    // Only reachable now when the writer named the storage themselves,
    // since the resolution above never pairs an explicit mode with a heap
    // it chose. The byte points at the key-mode word when there is one and
    // at the statement's start when the mode came from configuration - a
    // refusal about a word nobody wrote has no byte of its own to name.
    if (key_mode == catalog::KeyMode::kExplicit &&
        clustered != catalog::ClusteredType::kBtree) {
        return {ErrorReply(Status::Unsupported(
                    "an EXPLICIT relation must be BTREE (byte " +
                    std::to_string(stmt.key_mode_byte_offset) +
                    ") - a supplied id is not drawn from the cursor, so placing it and proving "
                    "it unique both need a descent, and a heap chain grows only at its tail")),
                false};
    }

    // Passed by name rather than defaulted, per PK01's rule: a defaulted
    // mode is how the wrong one reaches a relation without anyone reading
    // the line.
    auto oid = catalog_.CreateTable(catalog::kNamespacePublic, stmt.table_name, schema,
                                     clustered, key_mode);
    if (!oid.ok()) {
        return {"ERR " + oid.status().message(), false};
    }

    // The constraints, now that there is a child relation to hang them on.
    // What can still fail here is the colocation check (F5), which needs the
    // child's assigned owner core, and catalog I/O. Either leaves a created
    // relation behind and is reported as an error naming that - the honest
    // reply, since nothing can take the relation back.
    for (const PendingForeignKey& fk : pending_fkeys) {
        auto created = catalog_.CreateForeignKey(oid.value(), fk.column_no, fk.parent_oid);
        if (!created.ok()) {
            return {"ERR relation '" + stmt.table_name + "' was created (oid " +
                        std::to_string(oid.value()) +
                        ") but its foreign key on column " + std::to_string(fk.column_no) +
                        " referencing '" + fk.parent_name +
                        "' was not: " + created.status().message(),
                    false};
        }
    }

    // ---- `CABIN` on a column creates one now (docs/feat-cabin.md) -------
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
    if (wal_ == nullptr) return Status::OK();

    auto bytes = page_store_.Get(page_id);
    if (!bytes.ok()) return bytes.status();

    std::vector<std::byte> image(wal::kFullPageImagePayloadSize);
    if (auto n = wal::EncodeFullPageImage(image,
                                          std::span<const std::byte, kPageSize>(bytes.value().bytes()));
        !n.ok()) {
        return n.status();
    }
    auto fpi = wal_->Append(wal::RecordSpec{wal::RecordType::kFullPageImage, txn_id, page_id},
                            image);
    if (!fpi.ok()) return fpi.status();
    // The stamp is the half a hand-copied block loses: redo gates on page_lsn,
    // so an unstamped page replays a record whose effect it already holds.
    return page_store_.StampPageLsn(page_id, fpi.value());
}

Status CommandDispatcher::LogSpills(const std::vector<exec::AppendedSpill>& spills,
                                   std::uint64_t txn_id, std::uint64_t owner_oid) {
    if (wal_ == nullptr) return Status::OK();

    for (const exec::AppendedSpill& spill : spills) {
        // ---- The page the append created ---------------------------------
        //
        // `varheap::ChainAppend` grows a chain through the store's plain
        // allocation path, and a VARHEAP_APPEND does not say its page is new -
        // so without this record redo meets an append naming a page nothing
        // creates, and refuses the mount. `wal::ApplyPageInit` already formats
        // a kVarHeap page, so this is the record nobody wrote rather than an
        // applier nobody built.
        //
        // Unstamped, for the reason the heap path gives for a new tuple page:
        // the append below lands in exactly this page and stamps it.
        if (spill.created_page_id != kInvalidPageId) {
            std::array<std::byte, wal::kPageInitPayloadSize> init{};
            const wal::PageInitPayload init_fields{
                /*min_key=*/0, static_cast<std::uint8_t>(PageType::kVarHeap), {0, 0, 0},
                /*reserved2=*/0, owner_oid};
            if (auto n = wal::EncodePageInit(init, init_fields); !n.ok()) return n.status();
            if (auto rec = wal_->Append(
                    wal::RecordSpec{wal::RecordType::kPageInit, txn_id, spill.created_page_id},
                    init);
                !rec.ok()) {
                return rec.status();
            }
        }

        // ---- The link that made it reachable -----------------------------
        //
        // A full page image, because no record type describes a next-page
        // link - the same answer this function's caller gives for a heap link
        // edit. Losing it is the quieter half of the same defect: the value
        // page survives redo and no chain walk ever reaches it.
        if (spill.linked_page_id != kInvalidPageId) {
            if (Status s = LogFullPageImage(spill.linked_page_id, txn_id); !s.ok()) return s;
        }

        // ---- The value itself --------------------------------------------
        std::vector<std::byte> vh(wal::kVarHeapAppendFixedSize + spill.value.size());
        const wal::VarHeapAppendPayload vh_fields{
            spill.ptr.slot, 0, static_cast<std::uint32_t>(spill.value.size())};
        if (auto n = wal::EncodeVarHeapAppend(vh, vh_fields, spill.value); !n.ok()) {
            return n.status();
        }
        auto rec = wal_->Append(
            wal::RecordSpec{wal::RecordType::kVarHeapAppend, txn_id, spill.ptr.page_id}, vh);
        if (!rec.ok()) return rec.status();
        if (Status s = page_store_.StampPageLsn(spill.ptr.page_id, rec.value()); !s.ok()) {
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
            std::array<std::byte, wal::kPageInitPayloadSize> init{};
            const wal::PageInitPayload init_fields{change.min_key,
                                                   static_cast<std::uint8_t>(leaf_type),
                                                   {0, 0, 0},
                                                   /*reserved2=*/0, owner_oid};
            if (auto n = wal::EncodePageInit(init, init_fields); !n.ok()) return n.status();
            if (auto rec = wal_->Append(
                    wal::RecordSpec{wal::RecordType::kPageInit, txn_id, change.page_id}, init);
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
    if (Status s = LogSpills(spills, txn_id, owner_oid); !s.ok()) return s;

    // The index entries this row is now reachable through, before the row
    // itself (docs/feat-index.md §12.1). Same direction as the var-heap
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
    auto opened = BeginWrite(session);
    if (!opened.ok()) return {"ERR " + opened.status().message(), false};
    WriteScope scope = opened.value();

    DispatchOutcome out = InsertInner(line, scope);

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

    auto access = catalog_.InitTableAccess(read->rel_oid);
    if (!access.ok()) {
        remote_reads_->Close(tag);
        return {ErrorReply(access.status()), false};
    }
    const catalog::Schema& schema = access.value()->schema;

    // Byte-identical to the local star reply: the header of column names,
    // then one "\n"-escaped comma row per match, FormatValue's rendering.
    std::ostringstream os;
    bool first_col = true;
    for (const auto& col : schema.columns) {
        if (!first_col) os << ',';
        os << catalog::NameView(col.name);
        first_col = false;
    }

    for (const auto& batch : read->batches) {
        std::span<const std::byte> rows;
        auto header = DecodeStepBatchHeader(batch, rows);
        if (!header.ok()) {
            remote_reads_->Close(tag);
            return {ErrorReply(header.status()), false};
        }
        auto decoded = wire::DecodeRowBatch(rows, schema.columns.size());
        if (!decoded.ok()) {
            remote_reads_->Close(tag);
            return {ErrorReply(decoded.status()), false};
        }
        for (const auto& row : decoded.value()) {
            os << "\\n";
            bool first_val = true;
            for (std::size_t i = 0; i < schema.columns.size(); ++i) {
                if (!first_val) os << ',';
                os << exec::FormatValue(schema.columns[i].type_val,
                                        wire::FieldToValue(schema.columns[i], row[i]));
                first_val = false;
            }
        }
    }
    remote_reads_->Close(tag);
    return {os.str(), false};
}

Status CommandDispatcher::CheckReadAffinity(const exec::StepChain& chain) {
    // Every step, hoisted sub-chains included: a sub-chain reads a real
    // relation and is exactly as unable to reach another core's pages.
    auto check = [this](const std::vector<exec::Step>& steps) -> Status {
        for (const exec::Step& step : steps) {
            auto access = catalog_.InitTableAccess(step.rel_oid);
            if (!access.ok()) return access.status();
            if (access.value()->owner_core != core_id_) {
                return CrossCoreReadUnsupported(core_id_, access.value()->owner_core,
                                                step.rel_name);
            }
        }
        return Status::OK();
    };

    for (const exec::SubChain& sub : chain.hoisted) {
        if (Status s = check(sub.steps); !s.ok()) return s;
    }
    return check(chain.steps);
}

DispatchOutcome CommandDispatcher::InsertInner(std::string_view line, WriteScope& scope) {
    auto parsed = parser::Parse(line);
    if (!parsed.ok()) {
        return {"ERR " + parsed.status().message(), false};
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
    if (!opened.ok()) return {"ERR " + opened.status().message(), false};
    WriteScope scope = opened.value();

    DispatchOutcome out = InsertParsed(stmt, scope);

    const bool failed = out.response.rfind("ERR ", 0) == 0;
    Status verdict = failed ? Status::InvalidArgument(out.response) : Status::OK();
    if (Status s = EndWrite(session, scope, verdict); !s.ok() && !failed) {
        return {ErrorReply(s), false};
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

    auto oid = catalog_.FindTableOidByName(stmt.table_name);
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

    // Before anything is written: a relation this core does not own, or a
    // transaction already bound to another core, is refused retryably
    // (crosscore.md CC3, core_affinity.hpp). Once per statement - every
    // row goes to the one relation.
    if (Status affinity = CheckWriteAffinity(*ta, stmt.table_name, *scope.session);
        !affinity.ok()) {
        return {"ERR " + affinity.message(), false};
    }

    // ---- The bulk loop (docs/spec-bulkinsert.md §2.3, §4) ---------------
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

    // ---- T3: the sorted heap fill (docs/workplan-t3.md) -----------------
    //
    // Page-at-a-time placement and one image per touched page, engaged
    // only inside T3-2's gate; everything else takes the row loop below.
    if (bulk && SortedFillEligible(*ta, oid.value())) {
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
    // kAssigned is not implied by kHeap, even though an EXPLICIT relation
    // must be btree-clustered and so cannot reach here through DDL: the
    // catalog can be driven directly, and this path's whole shape - one
    // contiguous id range carved up front, appended in order - is wrong for
    // ids the caller names. Stated rather than inherited, so the coupling
    // cannot be broken silently from the other end.
    return ta.clustered_type == catalog::ClusteredType::kHeap &&
           ta.key_mode == catalog::KeyMode::kAssigned && ta.varheap_page_id == kInvalidPageId &&
           ta.indexes.empty() && ta.cabin_mask == 0 && !enforcer_.AnyOn(oid);
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
        if (ncols > 0 && values.size() == ncols) {
            err = "ERR do not supply a value for primary-key column '" +
                  std::string(catalog::NameView(ta.schema.columns.front().name)) +
                  "' - it is autoincrement and engine-assigned";
        } else if (ncols > 0 && values.size() != ncols - 1) {
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

    // ---- Arity, and where the pk comes from -----------------------------
    //
    // Which one of these is right is the relation's key mode
    // (docs/heap-and-tuple.md section 4.1), fixed at CREATE TABLE:
    //
    //   kAssigned - the engine issues the id, so VALUES supplies the columns
    //               *after* it and naming the pk is refused.
    //   kExplicit - the caller names the id, so VALUES supplies every column
    //               starting with it and omitting the pk is refused.
    //
    // One arity per relation, never two: INSERT is positional with no column
    // list, so a relation that accepted both counts could not say which of
    // them a wrong-length row meant.
    const std::size_t ncols = ta.schema.columns.size();
    const bool explicit_key = ta.key_mode == catalog::KeyMode::kExplicit;
    const std::size_t want = explicit_key ? ncols : ncols - 1;

    if (ncols > 0 && values.size() != want) {
        // The two common mistakes get the message that names the rule rather
        // than the count, because the count alone reads as an off-by-one.
        if (!explicit_key && values.size() == ncols) {
            return "ERR do not supply a value for primary-key column '" +
                   std::string(catalog::NameView(ta.schema.columns.front().name)) +
                   "' - it is autoincrement and engine-assigned";
        }
        if (explicit_key && values.size() == ncols - 1) {
            return "ERR supply a value for primary-key column '" +
                   std::string(catalog::NameView(ta.schema.columns.front().name)) +
                   "' - this relation is EXPLICIT, so the caller names its keys";
        }
        // Any other arity error, *before* the id: the codec would refuse
        // this row at encode, which sits after the id is settled - and BI9's
        // rule is that a refused row burns nothing. Same spelling as the
        // codec's own check (row_codec.cpp), which stays the authority on
        // everything deeper; this is only the count, hoisted above the burn.
        return "ERR expected " + std::to_string(want) + " value(s)" +
               (explicit_key ? " including the primary key" : " after the primary key") +
               ", got " + std::to_string(values.size());
    }

    // On an explicit relation the pk is values[0] and the body is the rest.
    // Split here, once, so everything downstream - the FK check, assertion
    // admission, EncodeRow, the Cabin witness, index maintenance - keeps
    // receiving exactly the shape it already expects: the columns after the
    // key. The copy is paid only by explicit relations, and only per row.
    std::vector<parser::AstValue> body_storage;
    std::uint64_t supplied_id = 0;
    if (explicit_key && ncols > 0) {
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

    // ---- The forward check (docs/impl-foreign-keys.md §2) ---------------
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

    // ---- The admission check (docs/feat-assertion.md §6.2 step 2) -------
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

    // The id, from whichever source the mode names. Both sit at exactly this
    // point in the statement - after admission, before the encode - so a
    // refused row still burns nothing either way, and the explicit path
    // advances the relation's high-water mark rather than drawing from it.
    std::uint64_t row_id = 0;
    if (explicit_key) {
        if (Status s = catalog_.AdmitExplicitRowId(oid, supplied_id); !s.ok()) {
            return ErrorReply(s);
        }
        row_id = supplied_id;
    } else {
        auto issued = catalog_.AllocateRowId(oid);
        if (!issued.ok()) {
            return "ERR " + issued.status().message();
        }
        row_id = issued.value();
    }

    std::vector<exec::AppendedSpill> spills;
    auto encoded = exec::EncodeRow(
        ta.schema, ta.layout, row_id, body,
        exec::VarHeapSink{&page_store_, ta.varheap_page_id, &spills, ta.oid});
    if (!encoded.ok()) {
        return "ERR " + encoded.status().message();
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
        return "ERR " + placed.status().message();
    }

    // ---- The Cabin witness (docs/feat-cabin.md §5) ----------------------
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

    // ---- Index maintenance (docs/feat-index.md §2) ----------------------
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
        return "ERR " + s.message();
    }

    // ---- The reservation (docs/feat-assertion.md §6.2 step 3) -----------
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
        if (!ptr.ok()) return "ERR " + ptr.status().message();

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
    // follow. This invalidates the catalog cache, so `ta` is dangling from
    // here on - the rest of this function uses only `oid`, `id` and
    // `placed`.
    if (placed.value().new_root != kInvalidPageId) {
        if (Status s = catalog_.UpdateRelationDescPage(oid, placed.value().new_root);
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
        // The relink invalidated the catalog cache, so `ta` (and the
        // caller's pointer) dangle from here on. Harmless on a statement's
        // last row; fatal to its next one - so the borrow is refreshed
        // before returning, which is the whole reason the pointer comes in
        // by reference.
        auto fresh = catalog_.InitTableAccess(oid);
        if (!fresh.ok()) return "ERR " + fresh.status().message();
        ta_ptr = fresh.value();
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
            if (col.qualified() && !IEquals(col.qualifier, stmt.from.binding())) {
                return {"ERR '" + col.qualifier + "." + col.name + "' names no relation in "
                        "this statement", false};
            }
            std::size_t found = rows.column_names.size();
            for (std::size_t i = 0; i < rows.column_names.size(); ++i) {
                if (IEquals(rows.column_names[i], col.name)) {
                    found = i;
                    break;
                }
            }
            if (found == rows.column_names.size()) {
                return {"ERR view sys." + stmt.from.table_name + " has no column '" + col.name +
                        "'", false};
            }
            project.push_back(found);
        }
    }

    // A WHERE clause still applies. The values are ordinary AstValues by
    // now, so this compares them exactly as the row evaluator does - but
    // by *name*, because a view has no schema to resolve an index against.
    // That is the one place a view is not the real path; it is confined
    // here, and a subquery predicate is refused rather than half-applied.
    std::ostringstream os;
    bool first_col = true;
    for (std::size_t index : project) {
        if (!first_col) os << ',';
        os << rows.column_names[index];
        first_col = false;
    }

    for (const std::vector<parser::AstValue>& row : rows.rows) {
        bool matched = true;
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
            std::size_t at = rows.column_names.size();
            for (std::size_t i = 0; i < rows.column_names.size(); ++i) {
                if (IEquals(rows.column_names[i], cond.col.name)) {
                    at = i;
                    break;
                }
            }
            if (at == rows.column_names.size()) {
                return {"ERR view sys." + stmt.from.table_name + " has no column '" +
                        cond.col.name + "'", false};
            }
            // type_val 0: the view's values carry their own kind, and none
            // of them is a uint64 column needing the digit-text path.
            if (!exec::CompareValues(/*type_val=*/0, row[at], cond.val, cond.op)) {
                matched = false;
                break;
            }
        }
        if (!matched) continue;

        os << "\\n";
        bool first_val = true;
        for (std::size_t index : project) {
            if (!first_val) os << ',';
            // type_val 0, for the reason the CompareValues call above
            // gives: a catalog view's values carry their own kind, and none
            // of them is a DATE or TIMESTAMP column.
            os << exec::FormatValue(/*type_val=*/0, row[index]);
            first_val = false;
        }
    }
    return {os.str(), false};
}

namespace {

// Re-emits multi-line text under the dispatcher's one-line wire contract:
// sections are joined with the literal two-character "\n" escape, never a
// raw newline byte (docs/client-manual.md section 2). The plan printer
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
// emits the fold's output rows (docs/feat-aggregate.md AG1, workplan AG06).
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
    if (!snapshot.ok()) return {"ERR " + snapshot.status().message(), false};

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
    auto chain = exec::Compile(catalog_, stmt);
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
    // `emit_in_key_order`. That flag is how a `kExplicit` relation's walk
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
            // A step the descriptor refuses (an index probe, say) falls
            // through to the honest refusal rather than a worse error.
        }
    }
    if (Status affinity = CheckReadAffinity(chain.value()); !affinity.ok()) {
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
    // (feat-physical-optimizer.md §II.4's f_i), and it is the one shape
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
    if (analyze) return RunAnalyze(compiled, trail, replay_ptr, instance, snapshot.value());

    // ---- AG1: the fold wraps the sink, and nothing else moves -----------
    //
    // Everything above this point ran unchanged and unconditionally for an
    // aggregated statement: the compile, the affinity check, the Waystone
    // lookup, the trail collector. The fold is strictly downstream of all
    // of them, which is what makes AG10's "recording, replay, Cabin probes
    // and access statistics hold unchanged" a structural fact rather than a
    // list of things that were remembered.
    if (compiled.aggregated()) {
        return RunAggregated(compiled, os, trail, replay_ptr, instance, snapshot.value());
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
        &exec_stats_, budget_, trail, replay_ptr, cabins_, &snapshot.value(), indexes_enabled_);
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
    // this is the physical optimizer's input (docs/heap-and-tuple.md §7),
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
            // and always legal (feat-cabin.md §1) - a set that might have
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
    if (!opened.ok()) return {"ERR " + opened.status().message(), false};
    WriteScope scope = opened.value();

    // The read view this UPDATE filters through. An UPDATE reads before it
    // writes, and it must not see a row a SELECT in the same transaction
    // would not - so it takes the snapshot the same way.
    auto snapshot = SnapshotFor(session);
    if (!snapshot.ok()) return {"ERR " + snapshot.status().message(), false};

    DispatchOutcome out = UpdateInner(line, scope, snapshot.value());

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

    auto oid = catalog_.FindTableOidByName(stmt.table_name);
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

    // Before anything is written: a relation this core does not own, or a
    // transaction already bound to another core, is refused retryably
    // (crosscore.md CC3, core_affinity.hpp).
    if (Status affinity = CheckWriteAffinity(ta, stmt.table_name, *scope.session); !affinity.ok()) {
        return {"ERR " + affinity.message(), false};
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

        // ---- First-updater-wins (docs/txn.md section 5) ----------------
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
        // ...and what tells the index hook the same thing (feat-index.md §2).
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
        // points at bytes no record describes (`docs/known-gaps.md`'s var-heap
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
        // ---- The before-image (docs/txn.md section 3.3) ----------------
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

        // ---- The Cabin witness, UPDATE half (docs/feat-cabin.md §5) -----
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
        // (docs/feat-index.md §12.1).
        if (wal_ != nullptr && scope.txn != nullptr) {
            // The var-heap first, for the reason the comment above gives and
            // INSERT already obeyed: the cell in the tuple record below points
            // into these pages, so the records that create, link and fill them
            // must precede it.
            if (Status s = LogSpills(appended_spills, scope.txn->id(), ta.oid); !s.ok()) return s;
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

// ---- Transaction control (docs/txn.md sections 1, 6) ---------------------

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
    if (!begun.ok()) return {"ERR " + begun.status().message(), false};
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

    // The session leaves the transaction either way: a commit that failed
    // to log is not a transaction the client may keep writing into.
    session.Finish();
    if (!committed.ok()) {
        txn_->Release(*txn);
        return {"ERR " + committed.status().message(), false};
    }

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

StatusOr<txn::Snapshot> CommandDispatcher::SnapshotFor(Session& session) {
    if (txn_ == nullptr) return txn::Snapshot{};  // sees everything, as before

    if (session.in_explicit_txn()) {
        txn::Transaction* txn = session.transaction();
        // The statement boundary. Under READ COMMITTED this re-mints;
        // under REPEATABLE READ it is a no-op, and that one branch is the
        // whole difference between the levels.
        if (Status s = txn_->StartStatement(*txn); !s.ok()) return s;
        return txn_->SnapshotFor(*txn);
    }

    // Autocommit: a view over the committed state, owned by no transaction.
    auto view = txn_->MintReadView(txn::kNoTrxId);
    if (!view.ok()) return view.status();
    txn::Snapshot snap;
    snap.view = view.value();
    snap.undo = &txn_->undo();
    return snap;
}

StatusOr<CommandDispatcher::WriteScope> CommandDispatcher::BeginWrite(Session& session) {
    WriteScope scope;
    scope.session = &session;
    if (txn_ == nullptr) return scope;  // no manager: kBootstrapXid, as before

    if (session.in_explicit_txn()) {
        scope.txn = session.transaction();
        scope.owned = false;
        if (Status s = txn_->StartStatement(*scope.txn); !s.ok()) return s;
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
        // AS9 resolution, feat-assertion.md §4.4); the statement's
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

    DispatchOutcome out = DeleteInner(line, scope, snapshot.value());

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

    auto oid = catalog_.FindTableOidByName(stmt.table_name);
    if (!oid.ok()) return {"ERR " + oid.status().message(), false};
    auto access = catalog_.InitTableAccess(oid.value());
    if (!access.ok()) return {"ERR " + access.status().message(), false};
    const catalog::TableAccess& ta = *access.value();

    // Before anything is marked: same rule as INSERT and UPDATE
    // (crosscore.md CC3). A delete-mark is a write.
    if (Status affinity = CheckWriteAffinity(ta, stmt.table_name, *scope.session); !affinity.ok()) {
        return {"ERR " + affinity.message(), false};
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

        // ---- The reverse check (docs/impl-foreign-keys.md §3) ----------
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
        // forbidden (feat-cabin.md section 5, feat-index.md IX2), because an
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
