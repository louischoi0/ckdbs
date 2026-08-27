#include "sim/loop.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include "sim/faults.hpp"
#include "sim/instance.hpp"
#include "sim/integrity.hpp"
#include "sim/oracle.hpp"
#include "sim/reply.hpp"

namespace kds::sim {

const char* SimModeName(SimMode mode) {
    switch (mode) {
        case SimMode::kClean: return "clean";
        case SimMode::kSyncCrash: return "sync-crash";
        case SimMode::kCrash: return "crash";
    }
    return "unknown";
}

std::optional<SimMode> ParseSimMode(std::string_view name) {
    for (const SimMode mode : {SimMode::kClean, SimMode::kSyncCrash, SimMode::kCrash}) {
        if (name == SimModeName(mode)) return mode;
    }
    return std::nullopt;
}

std::string FeatureToggles::Describe() const {
    std::string out;
    out += waystone ? "waystone" : "-";
    out += cabins ? "/cabins" : "/-";
    out += access_statistics ? "/access" : "/-";
    return out;
}

std::string SimVerdict::Summary(const SimConfig& config) const {
    std::string out = ok ? "SIM ok" : "SIM FAIL";
    out += " seed=" + std::to_string(config.seed);
    out += " mode=";
    out += SimModeName(config.mode);
    out += " profile=";
    out += ProfileName(config.profile);
    out += " iterations=" + std::to_string(iterations_run);
    out += " ops=" + std::to_string(ops_run);
    out += " reads=" + std::to_string(reads_checked);
    if (writes_checked != 0) out += " writes=" + std::to_string(writes_checked);
    if (transactions != 0) out += " txns=" + std::to_string(transactions);
    if (config.faults != FaultProfile::kNone) {
        out += " faults=";
        out += FaultProfileName(config.faults);
        out += " armed=" + std::to_string(faults_armed);
        out += " fired=" + std::to_string(faults_fired);
        out += " errored_ops=" + std::to_string(errored_ops);
        if (reads_skipped != 0) out += " reads_skipped=" + std::to_string(reads_skipped);
        if (counts_skipped != 0) out += " counts_skipped=" + std::to_string(counts_skipped);
        if (ops_on_lost_relation != 0) {
            out += " ops_on_lost_relation=" + std::to_string(ops_on_lost_relation);
        }
    }
    if (gated_missing_rows != 0) {
        out += " gated_missing_rows=" + std::to_string(gated_missing_rows) + " [GATED: recovery]";
    }
    if (unlogged_ddl_lost_tables != 0) {
        out += " unlogged_ddl_lost_tables=" + std::to_string(unlogged_ddl_lost_tables);
    }
    if (!ok) out += "\n  " + detail;
    return out;
}

namespace {

// The last few fault events, so a failure names what provoked it (SIM05).
// A ring rather than a full log: the interesting window is the handful of
// events before the failure, and a 2000-op fault run's whole schedule in
// every verdict line would bury it.
class FaultTrace {
public:
    void Note(std::string event) {
        recent_.push_back(std::move(event));
        if (recent_.size() > kKeep) recent_.erase(recent_.begin());
    }

    std::string Tail() const {
        if (recent_.empty()) return {};
        std::string out = " [";
        for (const std::string& event : recent_) out += event + "; ";
        out.pop_back();
        out.back() = ']';
        return out;
    }

private:
    static constexpr std::size_t kKeep = 8;
    std::vector<std::string> recent_;
};

// Everything one iteration threads through its helpers. A struct rather
// than nine parameters: the set grew with the fault schedule and again
// with transactions, and every helper wants most of it.
struct Iteration {
    SimInstance& instance;
    Oracle& oracle;
    SimVerdict& verdict;
    const SimConfig& config;
    std::size_t index = 0;
    FaultTrace trace{};

    // Transaction state as the *replies* report it, never as the generator
    // assumes it: under injection a BEGIN can fail, and a loop that trusted
    // the generator would then apply a whole transaction's writes to an
    // oracle the engine never opened one for.
    bool txn_open = false;

    bool faults_on() const { return config.faults != FaultProfile::kNone; }
};

// First failure wins; everything after it is noise from the same cause.
void Fail(Iteration& it, std::string detail) {
    if (!it.verdict.ok) return;
    it.verdict.ok = false;
    it.verdict.detail = "seed=" + std::to_string(it.config.seed) +
                        " iteration=" + std::to_string(it.index) + ": " + std::move(detail) +
                        it.trace.Tail();
}

std::vector<std::string> RowsOf(const std::string& reply) {
    std::vector<std::string> lines = SplitEscapedLines(reply);
    return {lines.begin() + 1, lines.end()};
}

// "UPDATED 7" / "DELETED 0" -> 7 / 0.
std::optional<std::size_t> ParseCount(const std::string& reply, std::string_view verb) {
    if (reply.compare(0, verb.size(), verb) != 0) return std::nullopt;
    std::size_t n = 0;
    bool any = false;
    for (std::size_t i = verb.size(); i < reply.size(); ++i) {
        if (reply[i] < '0' || reply[i] > '9') break;
        n = n * 10 + static_cast<std::size_t>(reply[i] - '0');
        any = true;
    }
    if (!any) return std::nullopt;
    return n;
}

// Rows the read returned that the oracle neither expects nor has an
// unknown outcome for. Order is preserved, so an ordered comparison
// survives the filter.
std::vector<std::string> WithoutUnknowns(const std::vector<std::string>& actual,
                                         const std::vector<std::string>& expected,
                                         const Oracle& oracle, const std::string& table) {
    std::multiset<std::string> unmatched(expected.begin(), expected.end());
    std::vector<std::string> kept;
    kept.reserve(actual.size());
    for (const std::string& row : actual) {
        const auto it = unmatched.find(row);
        if (it != unmatched.end()) {
            unmatched.erase(it);
            kept.push_back(row);
            continue;
        }
        if (oracle.Ignorable(table, row)) continue;  // may or may not exist
        kept.push_back(row);
    }
    return kept;
}

// A read must agree with the oracle. Scans are compared order-insensitively
// (sorted), a pk point read exactly — the SIM03 contract. Under fault
// injection a row whose write's outcome is unknown is permitted but never
// required (sim/oracle.hpp).
bool ReadAgrees(const std::string& reply, std::vector<std::string> expected, bool ordered,
                const Oracle& oracle, const std::string& table, std::string& why) {
    if (IsErr(reply)) {
        why = "read answered an error: " + reply;
        return false;
    }
    std::vector<std::string> actual = RowsOf(reply);
    if (!ordered) {
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());
    }
    if (actual == expected) return true;
    if (oracle.has_unknowns()) {
        actual = WithoutUnknowns(actual, expected, oracle, table);
        if (actual == expected) return true;
    }
    why = "expected " + std::to_string(expected.size()) + " row(s), got " +
          std::to_string(actual.size());
    const std::size_t n = std::min(actual.size(), expected.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (actual[i] != expected[i]) {
            why += "; first divergence at row " + std::to_string(i) + ": expected '" +
                   expected[i] + "', got '" + actual[i] + "'";
            break;
        }
    }
    return false;
}

// One read op. Returns false only on a real disagreement — an error reply
// is a failure without faults, and with them it never reaches here.
bool CheckRead(Iteration& it, const Op& op, const std::string& reply,
               std::vector<std::string> expected, bool ordered, std::size_t op_index) {
    if (it.faults_on() && !it.oracle.HasTable(op.table)) {
        // A relation whose CREATE was answered with an error is not in the
        // oracle, so there is nothing to compare against; the engine may
        // legally have it either way.
        ++it.verdict.reads_skipped;
        return true;
    }
    std::string why;
    if (!ReadAgrees(reply, std::move(expected), ordered, it.oracle, op.table, why)) {
        Fail(it, "op " + std::to_string(op_index) + " [" + op.sql + "]: " + why);
        return false;
    }
    ++it.verdict.reads_checked;
    return true;
}

Oracle::Predicate PredicateOf(const Op& op) {
    return Oracle::Predicate{op.by_pk, op.key, op.pred_v};
}


// A mutation's row count is the sharpest single assertion the harness
// makes: the engine's own count of what it touched, against a map that has
// been tracking the same rows all along. What can stop it is an unknown
// the predicate might match — which for a pk predicate is one id and for a
// value predicate is any of them (Oracle::CountCheckable).
bool CheckCount(Iteration& it, const Op& op, const std::string& reply, std::string_view verb,
                std::size_t expected, std::size_t op_index) {
    const std::optional<std::size_t> reported = ParseCount(reply, verb);
    if (!reported.has_value()) {
        Fail(it, "op " + std::to_string(op_index) + " [" + op.sql + "]: " + reply);
        return false;
    }
    if (!it.oracle.CountCheckable(op.table, PredicateOf(op))) {
        ++it.verdict.counts_skipped;
        return true;
    }
    if (*reported != expected) {
        Fail(it, "op " + std::to_string(op_index) + " [" + op.sql + "]: the engine reports " +
                     std::string(verb) + std::to_string(*reported) + ", the oracle expects " +
                     std::to_string(expected));
        return false;
    }
    ++it.verdict.writes_checked;
    return true;
}

// Defined below, beside the shutdown path that is its other caller.
void CloseOpenTransaction(Iteration& it);

// An error reply under injection. Nothing is applied to the oracle, and
// what becomes *unknown* rather than absent depends on where the error
// landed (sim/oracle.hpp's two kinds).
void AbsorbError(Iteration& it, const Op& op, std::size_t op_index) {
    ++it.verdict.errored_ops;

    // A relation whose CREATE was answered with an error never comes back:
    // the generator keeps naming it and the engine keeps refusing, so every
    // one of those ops is workload thrown away. Absorbing them silently is
    // how a fault run passes vacuously — a run whose first statement died
    // errors half its ops, checks nothing, and reports "SIM ok". Counted
    // here so the verdict says how much of itself it threw away.
    if (!op.table.empty() && !it.oracle.HasTable(op.table)) ++it.verdict.ops_on_lost_relation;

    if (op.kind == Op::Kind::kCommit || op.kind == Op::Kind::kRollback) {
        // The one genuinely two-sided outcome: a COMMIT's record may have
        // reached the device before the error. Every row it touched stops
        // being anyone's to assert on — and the transaction is **not
        // assumed closed**: `HandleCommit` and `HandleRollback` both return
        // early on an enforcer failure, saying so where they do ("the
        // client may retry COMMIT or ROLLBACK"). A harness that assumed
        // otherwise would run the rest of its stream inside a transaction
        // it did not know was open, writing to an oracle that thinks it is
        // committing. So the client does what the engine tells it to.
        if (it.txn_open) {
            it.oracle.Abandon();
            it.instance.Execute("ROLLBACK");  // no-op if it really did end
            it.txn_open = false;
        }
        it.trace.Note("txn abandoned at op " + std::to_string(op_index));
        return;
    }
    if (it.txn_open) {
        // A failed statement poisons the session: only ROLLBACK and a
        // three-command whitelist are admitted until the client rolls back
        // (manual/sql/sql.md §5). So the client rolls back — and if that
        // succeeds the transaction's writes are *definitely* gone, which is
        // a stronger fact than the abandonment above.
        CloseOpenTransaction(it);
        it.trace.Note("poisoned txn rolled back at op " + std::to_string(op_index));
        return;
    }

    switch (op.kind) {
        case Op::Kind::kInsert:
            it.oracle.NoteIndeterminate(op.table);
            break;
        case Op::Kind::kUpdate:
        case Op::Kind::kDelete:
            for (const std::uint64_t id : it.oracle.Matching(op.table, PredicateOf(op))) {
                it.oracle.NoteUnchecked(op.table, id);
            }
            break;
        default:
            break;
    }
}

bool ExecuteOp(Iteration& it, const Op& op, std::size_t op_index) {
    std::string reply = it.instance.Execute(op.sql);

    // **A CREATE TABLE that catches an injection is retried once.** The
    // schema is what the rest of the stream is written against: a relation
    // that dies at op 0 turns every later op naming it into an absorbed
    // error, and the iteration then verifies nothing while reporting green
    // — measured at 1291 of 3000 ops on one corpus seed, which is what
    // `ops_on_lost_relation` counts. Retrying is what a client does.
    //
    // A retry answering **EXISTS** is the DDL's version of an errored
    // write's unknown outcome, not a violation: the injection that killed
    // the acknowledgement (a failed commit sync, in every case the corpus
    // produces) landed *after* the catalog write, so "it happened" is one
    // of the two legal outcomes and the retry is how the client learns
    // which. The relation is adopted. What would be a finding is a
    // relation that half exists — visible by one route and absent by
    // another — and EXISTS is not that; the routes are what
    // `ddl-transactional.md`'s own test walks.
    if (IsErr(reply) && it.faults_on() && op.kind == Op::Kind::kCreateTable) {
        ++it.verdict.errored_ops;
        it.trace.Note("create-table errored at op " + std::to_string(op_index) + ", retried");
        reply = it.instance.Execute(op.sql);
        if (reply.rfind("EXISTS", 0) == 0) {
            it.trace.Note("the first attempt had reached the catalog after all");
            it.oracle.CreateTable(op.table);
            ++it.verdict.ops_run;
            return true;
        }
    }

    if (IsErr(reply) && it.faults_on()) {
        AbsorbError(it, op, op_index);
        ++it.verdict.ops_run;
        return true;
    }

    switch (op.kind) {
        case Op::Kind::kCreateTable:
            if (reply.rfind("CREATED", 0) != 0) {
                Fail(it, "op " + std::to_string(op_index) + " [" + op.sql + "]: " + reply);
                return false;
            }
            it.oracle.CreateTable(op.table);
            break;
        case Op::Kind::kCreateCabin:
        case Op::Kind::kCreatePattern:
            // Advisory declarations: they may not change a single result,
            // which is what the toggle pairing proves. Here the only
            // assertion is that they are accepted — and **ADOPTED is an
            // acceptance**: with recording on, the shape has usually been
            // auto-registered already, so the declaration adopts that
            // pattern instead of minting one. Same id, same directory; the
            // verb is the dispatcher reporting which it did.
            if (reply.rfind("CREATED", 0) != 0 && reply.rfind("ADOPTED", 0) != 0) {
                Fail(it, "op " + std::to_string(op_index) + " [" + op.sql + "]: " + reply);
                return false;
            }
            break;
        case Op::Kind::kInsert: {
            const std::optional<std::uint64_t> id = ParseInsertedId(reply);
            if (!id.has_value()) {
                Fail(it, "op " + std::to_string(op_index) + " [" + op.sql + "]: " + reply);
                return false;
            }
            it.oracle.ApplyInsert(op.table, *id, OracleRow{op.v, op.name});
            break;
        }
        case Op::Kind::kUpdate: {
            const std::size_t expected = it.oracle.ApplyUpdate(
                op.table, PredicateOf(op), Oracle::Assignment{op.set_name, op.v, op.name});
            if (!CheckCount(it, op, reply, "UPDATED ", expected, op_index)) return false;
            break;
        }
        case Op::Kind::kDelete: {
            const std::size_t expected = it.oracle.ApplyDelete(op.table, PredicateOf(op));
            if (!CheckCount(it, op, reply, "DELETED ", expected, op_index)) return false;
            break;
        }
        case Op::Kind::kBegin:
            if (reply.rfind("BEGIN ", 0) != 0) {
                Fail(it, "op " + std::to_string(op_index) + " [" + op.sql + "]: " + reply);
                return false;
            }
            it.oracle.Begin();
            it.txn_open = true;
            break;
        case Op::Kind::kCommit:
            if (reply.rfind("COMMIT ", 0) != 0) {
                Fail(it, "op " + std::to_string(op_index) + " [" + op.sql + "]: " + reply);
                return false;
            }
            it.oracle.Commit();
            it.txn_open = false;
            ++it.verdict.transactions;
            break;
        case Op::Kind::kRollback:
            if (reply.rfind("ROLLBACK ", 0) != 0) {
                Fail(it, "op " + std::to_string(op_index) + " [" + op.sql + "]: " + reply);
                return false;
            }
            it.oracle.Rollback();
            it.txn_open = false;
            ++it.verdict.transactions;
            break;
        case Op::Kind::kSelectPk:
            if (!CheckRead(it, op, reply, it.oracle.ExpectPk(op.table, op.key),
                           /*ordered=*/true, op_index)) {
                return false;
            }
            break;
        case Op::Kind::kSelectRange:
            if (!CheckRead(it, op, reply, it.oracle.ExpectRange(op.table, op.lo, op.hi),
                           /*ordered=*/false, op_index)) {
                return false;
            }
            break;
        case Op::Kind::kFilterScan:
            if (!CheckRead(it, op, reply, it.oracle.ExpectFilter(op.table, op.v),
                           /*ordered=*/false, op_index)) {
                return false;
            }
            break;
        case Op::Kind::kSync:
            if (reply != "OK synced") {
                Fail(it, "op " + std::to_string(op_index) + " [SYNC]: " + reply);
                return false;
            }
            it.oracle.MarkSynced();
            break;
    }
    ++it.verdict.ops_run;
    return true;
}

// A client that stops mid-transaction rolls back, and the harness has to
// be that client in two places: before the quiescence probe (which reads
// committed state and would otherwise be handed the pending set), and
// before a *clean* shutdown, whose checkpoint records an empty
// active-transaction list by construction — leaving one open would publish
// an anchor claiming there was nothing to undo.
void CloseOpenTransaction(Iteration& it) {
    if (!it.txn_open) return;
    const std::string reply = it.instance.Execute("ROLLBACK");
    it.txn_open = false;
    if (IsErr(reply)) {
        it.oracle.Abandon();
    } else {
        it.oracle.Rollback();
    }
}

// SIM05's quiescence probe: with every injection disarmed, the instance
// must answer every relation cleanly and agree with the oracle. An engine
// that survives a fault run by refusing everything afterwards passes every
// other check in this file, and fails this one.
bool ProbeQuiesced(Iteration& it) {
    it.instance.page_device().ClearInjections();
    it.instance.log_device().ClearInjections();
    std::string why;
    if (!ScanAgreesWithOracle(it.instance, it.oracle, why)) {
        Fail(it, "with the fault schedule exhausted and every injection disarmed, the "
                 "quiescence probe fails: " + why);
        return false;
    }
    it.verdict.reads_checked += it.oracle.tables().size();
    return true;
}

// Post-restart reconciliation. The mode decides which absences are
// failures, which are the recovery gate's debt, and which are the
// documented price of unlogged DDL.
void Reconcile(Iteration& it) {
    const SimMode mode = it.config.mode;
    const Oracle& oracle = it.oracle;
    for (const auto& [table, accepted] : oracle.tables()) {
        const auto synced_it = oracle.synced().find(table);
        const bool table_synced = synced_it != oracle.synced().end();

        const std::string reply = it.instance.Execute("SELECT * FROM " + table);
        if (IsErr(reply)) {
            if (mode != SimMode::kClean && !table_synced) {
                // Created after the last SYNC and CREATE TABLE is unlogged
                // by design — lost, expected, counted.
                ++it.verdict.unlogged_ddl_lost_tables;
                continue;
            }
            Fail(it, "after restart, relation '" + table + "' is gone: " + reply);
            continue;
        }

        std::vector<std::string> actual = RowsOf(reply);
        std::sort(actual.begin(), actual.end());

        // No duplicates: a row emitted twice is wrong in every mode.
        for (std::size_t i = 1; i < actual.size(); ++i) {
            if (actual[i] == actual[i - 1]) {
                Fail(it, "after restart, relation '" + table + "' emits a row twice: '" +
                             actual[i] + "'");
            }
        }

        // No fabrication: every row read back must be one the oracle
        // accepted, byte for byte, or one whose outcome the engine left
        // unknown by answering its write with an error. This also catches a
        // docs/spec/txn.md section 8 ghost — a row whose statement was never
        // acknowledged.
        std::set<std::string> accepted_rendered;
        for (const auto& [id, row] : accepted) {
            accepted_rendered.insert(Oracle::Render(id, row));
        }
        for (const std::string& row : actual) {
            if (accepted_rendered.count(row)) continue;
            if (oracle.Ignorable(table, row)) continue;
            Fail(it, "after restart, relation '" + table +
                         "' returned a row the oracle never accepted: '" + row + "'");
        }

        const std::set<std::string> present(actual.begin(), actual.end());

        // SYNC's promise holds in every mode: a row synced to the device
        // must be there — **as long as nothing superseded it**. v2's
        // mutations make that qualifier load-bearing: a row synced and then
        // deleted is legitimately gone, and one synced and then updated is
        // legitimately a different row now. Both leave the synced rendering
        // out of the accepted set, which is exactly the test.
        std::set<std::string> synced_rendered;
        if (table_synced) {
            for (const auto& [id, row] : synced_it->second) {
                const std::string rendered = Oracle::Render(id, row);
                if (!accepted_rendered.count(rendered)) continue;  // superseded since
                synced_rendered.insert(rendered);
                if (present.count(rendered)) continue;
                if (oracle.Ignorable(table, rendered)) continue;
                Fail(it, "after restart, relation '" + table + "' lost a SYNCed row: '" +
                             rendered + "'");
            }
        }

        // The rest of the acknowledged state. Clean shutdown promises all
        // of it now; a crash only promises it once recovery exists.
        std::size_t missing_unsynced = 0;
        for (const std::string& rendered : accepted_rendered) {
            if (present.count(rendered)) continue;
            if (synced_rendered.count(rendered)) continue;  // already failed above
            if (oracle.Ignorable(table, rendered)) continue;
            ++missing_unsynced;
        }
        if (missing_unsynced != 0) {
            if (mode == SimMode::kClean || it.config.assert_recovery) {
                Fail(it, "after restart, relation '" + table + "' is missing " +
                             std::to_string(missing_unsynced) + " acknowledged row(s)");
            } else {
                // [GATED: recovery] — the WAL holds their commit records;
                // this is the count recovery must drive to zero.
                it.verdict.gated_missing_rows += missing_unsynced;
            }
        }
    }
}

// The three advisory switches for one iteration: whatever the config
// pinned, or a seeded draw. Drawing them is the point — the oracle does
// not know they exist, so every iteration is also a test that they
// changed nothing.
FeatureToggles TogglesFor(const SimConfig& config, const Rng& iteration_rng) {
    if (config.toggles.has_value()) return *config.toggles;
    Rng rng = iteration_rng.Fork("toggles");
    FeatureToggles toggles;
    toggles.waystone = rng.Chance(50);
    toggles.cabins = rng.Chance(50);
    toggles.access_statistics = rng.Chance(50);
    return toggles;
}

SimInstance::Options InstanceOptions(const SimConfig& config, const FeatureToggles& toggles) {
    SimInstance::Options options;
    options.skip_recovery = config.skip_recovery;
    options.waystone = toggles.waystone;
    options.cabins = toggles.cabins;
    options.access_statistics = toggles.access_statistics;
    return options;
}

bool RunIteration(const SimConfig& config, const SimPlan& plan, std::size_t iteration,
                  SimVerdict& verdict) {
    auto instance_or = SimInstance::Create(InstanceOptions(config, plan.toggles));
    if (!instance_or.ok()) {
        verdict.ok = false;
        verdict.detail = "seed=" + std::to_string(config.seed) + " iteration=" +
                         std::to_string(iteration) +
                         ": instance creation failed: " + instance_or.status().message();
        return false;
    }

    Oracle oracle;
    Iteration it{*instance_or.value(), oracle, verdict, config, iteration};
    it.trace.Note("toggles " + plan.toggles.Describe());

    std::uint64_t fired_before = 0;
    for (std::size_t i = 0; i < plan.entries.size(); ++i) {
        const SimPlan::Entry& entry = plan.entries[i];
        for (const FaultKind kind : entry.faults) {
            ArmFault(ScheduledFault{i, kind}, it.instance.page_device(),
                     it.instance.log_device());
            ++verdict.faults_armed;
            it.trace.Note("armed " + ScheduledFault{i, kind}.Describe());
        }
        const bool op_ok = ExecuteOp(it, entry.op, i);
        if (it.faults_on()) {
            const std::uint64_t fired =
                InjectionsFired(it.instance.page_device(), it.instance.log_device());
            // Carried on every op, not summed once the loop ends: a failing
            // iteration leaves through the `return` below, and the counter
            // that says whether the schedule really disturbed the engine
            // read zero on exactly the runs it is wanted for.
            verdict.faults_fired = fired;
            if (fired != fired_before) {
                it.trace.Note("fired " + std::to_string(fired - fired_before) + " by op " +
                              std::to_string(i));
                fired_before = fired;
            }
        }
        if (!op_ok) return false;
    }
    if (it.faults_on()) {
        // An iteration whose every CREATE TABLE died verified *nothing* —
        // no read compared, no count checked, no relation reconciled — and
        // "SIM ok" would be a false report of it. The retry above makes
        // this rare; saying so when it happens is what keeps the counter
        // beside it honest rather than decorative.
        if (it.oracle.tables().empty()) {
            Fail(it, "every CREATE TABLE was lost to an injected error, so this iteration "
                     "checked nothing");
            return false;
        }
        CloseOpenTransaction(it);
        if (!ProbeQuiesced(it)) return false;
    }

    switch (config.mode) {
        case SimMode::kClean:
            CloseOpenTransaction(it);
            if (Status s = it.instance.CleanShutdown(); !s.ok()) {
                Fail(it, s.message());
                return false;
            }
            break;
        case SimMode::kSyncCrash: {
            // Deliberately *not* closing an open transaction: a crash with
            // one in flight is what undo exists for, and the SYNC below
            // makes its pages durable first. The oracle's committed state
            // never held it, so its removal is recovery's to owe.
            const std::string reply = it.instance.Execute("SYNC");
            if (reply == "OK synced") {
                oracle.MarkSynced();
            } else if (!it.faults_on()) {
                Fail(it, "pre-crash SYNC: " + reply);
                return false;
            }
            it.instance.Crash();
            break;
        }
        case SimMode::kCrash:
            it.instance.Crash();
            break;
    }

    if (Status s = it.instance.Reboot(); !s.ok()) {
        Fail(it, "reboot failed: " + s.message());
        return false;
    }

    const IntegrityReport report =
        CheckInstance(it.instance.store(), it.instance.page_device(), it.instance.catalog());
    if (!report.ok()) {
        Fail(it, report.Summary());
        return false;
    }

    Reconcile(it);
    return verdict.ok;
}

}  // namespace

void SimVerdict::Absorb(const SimVerdict& other) {
    ops_run += other.ops_run;
    reads_checked += other.reads_checked;
    writes_checked += other.writes_checked;
    transactions += other.transactions;
    faults_armed += other.faults_armed;
    faults_fired += other.faults_fired;
    errored_ops += other.errored_ops;
    reads_skipped += other.reads_skipped;
    counts_skipped += other.counts_skipped;
    ops_on_lost_relation += other.ops_on_lost_relation;
    gated_missing_rows += other.gated_missing_rows;
    unlogged_ddl_lost_tables += other.unlogged_ddl_lost_tables;
    if (ok && !other.ok) {
        ok = false;
        detail = other.detail;
    }
}

SimPlan BuildPlan(const SimConfig& config, std::size_t iteration) {
    const Rng iteration_rng =
        Rng(config.seed).Fork("iteration/" + std::to_string(iteration));

    SimPlan plan;
    plan.toggles = TogglesFor(config, iteration_rng);

    // The crash point is drawn before the ops are, so the plan holds
    // exactly the ops that will run: an iteration that crashes at op 40
    // *is* a forty-op plan, and the mode says what happens after the last
    // one.
    std::size_t stop_at = config.ops;
    if (config.mode != SimMode::kClean) {
        Rng crash_rng = iteration_rng.Fork("crash");
        stop_at = 1 + crash_rng.Below(config.ops);
    }

    const FaultSchedule faults(iteration_rng.Fork("faults"), config.ops, config.faults,
                               config.fault_rate);
    Workload workload(iteration_rng.Fork("workload"), config.profile);

    plan.entries.reserve(stop_at);
    for (std::size_t i = 0; i < stop_at; ++i) {
        SimPlan::Entry entry;
        for (const ScheduledFault& fault : faults.At(i)) entry.faults.push_back(fault.kind);
        entry.op = workload.Next();
        plan.entries.push_back(std::move(entry));
    }
    return plan;
}

SimVerdict RunPlan(const SimConfig& config, const SimPlan& plan, std::size_t iteration) {
    SimVerdict verdict;
    verdict.iterations_run = 1;
    RunIteration(config, plan, iteration, verdict);
    return verdict;
}

SimVerdict RunSimulation(const SimConfig& config) {
    SimVerdict verdict;
    for (std::size_t i = 0; i < config.iterations; ++i) {
        const SimVerdict one = RunPlan(config, BuildPlan(config, i), i);
        verdict.iterations_run = i + 1;
        verdict.Absorb(one);
        if (!verdict.ok) break;
    }
    return verdict;
}

bool ScanAgreesWithOracle(SimInstance& instance, const Oracle& oracle, std::string& why) {
    for (const auto& [table, rows] : oracle.tables()) {
        const std::string reply = instance.Execute("SELECT * FROM " + table);
        if (IsErr(reply)) {
            why = "relation '" + table + "' answers an error: " + reply;
            return false;
        }
        std::string detail;
        if (!ReadAgrees(reply, oracle.ExpectAll(table), /*ordered=*/false, oracle, table,
                        detail)) {
            why = "relation '" + table + "': " + detail;
            return false;
        }
    }
    return true;
}

// Whether two instances answered one statement the same way, for the
// toggle pairing's purposes (SIM06). Three tiers, and the tier is decided
// by what the statement *is*, not by patching strings that happened to
// differ:
//
//   **Queries and mutations — byte for byte.** Every SELECT, every
//   `UPDATED <n>` / `DELETED <n>`, every transaction reply. This is the
//   invariant: no advisory feature may change an answer.
//
//   **INSERT — the assigned id, not the placement.** The reply carries
//   `page=` and `slot=`, and the advisory features keep their own state in
//   `sys.*` relations whose pages come out of the same free map, so a
//   trail or a Cabin shifts the next user page by one or two. Invariant 8
//   promises the advisory state cannot change what a query answers; it
//   never promised the free map would allocate identically, and asserting
//   that would be asserting a coincidence. The id is compared, because the
//   id is identity (invariant 11) and every later statement names rows by
//   it.
//
//   **Declarations — acceptance only.** `CREATE PATTERN` answers CREATED
//   or **ADOPTED** depending on whether the recorder had already
//   auto-registered the shape, and `CREATE CABIN` appends a WARN when the
//   access statistics hold no filter on the column. Both replies report
//   advisory state *on purpose*: that is what a declaration's reply is
//   for. Neither is a row anyone reads.
bool SameOutcome(const Op& op, const std::string& bare, const std::string& full) {
    switch (op.kind) {
        case Op::Kind::kCreateTable:
        case Op::Kind::kCreateCabin:
        case Op::Kind::kCreatePattern:
            return IsErr(bare) == IsErr(full);
        case Op::Kind::kInsert:
            return ParseInsertedId(bare) == ParseInsertedId(full);
        default:
            return bare == full;
    }
}

SimVerdict RunTogglePairing(const SimConfig& config) {
    // Every advisory feature off against every one on. Nothing else
    // differs: same seed, same op stream, same devices, and no faults — an
    // injected error lands on whichever instance reaches the device first,
    // which would diverge the pair for a reason that is not the features'.
    FeatureToggles off;
    off.waystone = off.cabins = off.access_statistics = false;
    FeatureToggles on;
    on.waystone = on.cabins = on.access_statistics = true;

    // The op stream is the plan's, not a second copy of the generator's
    // seeding: the same `.sim` case file that replays a crash run replays
    // this one, which is what makes a divergence found here shrinkable.
    // The mode is forced clean so the plan is the whole op budget rather
    // than a crash prefix — the pairing never crashes.
    SimConfig plan_config = config;
    plan_config.mode = SimMode::kClean;
    plan_config.faults = FaultProfile::kNone;

    SimVerdict verdict;
    for (std::size_t iteration = 0; iteration < config.iterations; ++iteration) {
        ++verdict.iterations_run;
        const SimPlan plan = BuildPlan(plan_config, iteration);

        auto bare = SimInstance::Create(InstanceOptions(config, off));
        auto full = SimInstance::Create(InstanceOptions(config, on));
        if (!bare.ok() || !full.ok()) {
            verdict.ok = false;
            verdict.detail = "instance creation failed";
            return verdict;
        }

        for (std::size_t i = 0; i < plan.entries.size(); ++i) {
            const Op& op = plan.entries[i].op;
            const std::string bare_reply = bare.value()->Execute(op.sql);
            const std::string full_reply = full.value()->Execute(op.sql);
            ++verdict.ops_run;
            if (SameOutcome(op, bare_reply, full_reply)) continue;
            verdict.ok = false;
            verdict.detail = "seed=" + std::to_string(config.seed) +
                             " iteration=" + std::to_string(iteration) + ": op " +
                             std::to_string(i) + " [" + op.sql +
                             "] answers differently with the advisory features on:\n" +
                             "    off (" + off.Describe() + "): " + bare_reply + "\n" +
                             "    on  (" + on.Describe() + "): " + full_reply;
            return verdict;
        }
    }
    return verdict;
}

}  // namespace kds::sim
