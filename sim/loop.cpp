#include "sim/loop.hpp"

#include <algorithm>
#include <set>
#include <utility>

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

// First failure wins; everything after it is noise from the same cause.
void Fail(SimVerdict& verdict, std::uint64_t seed, std::size_t iteration, std::string detail) {
    if (!verdict.ok) return;
    verdict.ok = false;
    verdict.detail = "seed=" + std::to_string(seed) + " iteration=" +
                     std::to_string(iteration) + ": " + std::move(detail);
}

std::vector<std::string> RowsOf(const std::string& reply) {
    std::vector<std::string> lines = SplitEscapedLines(reply);
    return {lines.begin() + 1, lines.end()};
}

// A read must agree with the oracle. Scans are compared order-insensitively
// (sorted), a pk point read exactly — the SIM03 contract.
bool ReadAgrees(const std::string& reply, std::vector<std::string> expected, bool ordered,
                std::string& why) {
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

bool ExecuteOp(SimInstance& instance, const Op& op, Oracle& oracle, SimVerdict& verdict,
               std::uint64_t seed, std::size_t iteration, std::size_t op_index) {
    const std::string reply = instance.Execute(op.sql);
    std::string why;
    switch (op.kind) {
        case Op::Kind::kCreateTable:
            if (reply.rfind("CREATED", 0) != 0) {
                Fail(verdict, seed, iteration,
                     "op " + std::to_string(op_index) + " [" + op.sql + "]: " + reply);
                return false;
            }
            oracle.CreateTable(op.table);
            break;
        case Op::Kind::kInsert: {
            const std::optional<std::uint64_t> id = ParseInsertedId(reply);
            if (!id.has_value()) {
                Fail(verdict, seed, iteration,
                     "op " + std::to_string(op_index) + " [" + op.sql + "]: " + reply);
                return false;
            }
            oracle.ApplyInsert(op.table, *id, OracleRow{op.v, op.name});
            break;
        }
        case Op::Kind::kSelectPk:
            if (!ReadAgrees(reply, oracle.ExpectPk(op.table, op.key), /*ordered=*/true, why)) {
                Fail(verdict, seed, iteration,
                     "op " + std::to_string(op_index) + " [" + op.sql + "]: " + why);
                return false;
            }
            ++verdict.reads_checked;
            break;
        case Op::Kind::kSelectRange:
            if (!ReadAgrees(reply, oracle.ExpectRange(op.table, op.lo, op.hi),
                            /*ordered=*/false, why)) {
                Fail(verdict, seed, iteration,
                     "op " + std::to_string(op_index) + " [" + op.sql + "]: " + why);
                return false;
            }
            ++verdict.reads_checked;
            break;
        case Op::Kind::kFilterScan:
            if (!ReadAgrees(reply, oracle.ExpectFilter(op.table, op.v), /*ordered=*/false,
                            why)) {
                Fail(verdict, seed, iteration,
                     "op " + std::to_string(op_index) + " [" + op.sql + "]: " + why);
                return false;
            }
            ++verdict.reads_checked;
            break;
        case Op::Kind::kSync:
            if (reply != "OK synced") {
                Fail(verdict, seed, iteration,
                     "op " + std::to_string(op_index) + " [SYNC]: " + reply);
                return false;
            }
            oracle.MarkSynced();
            break;
    }
    ++verdict.ops_run;
    return true;
}

// Post-restart reconciliation. The mode decides which absences are
// failures, which are the recovery gate's debt, and which are the
// documented price of unlogged DDL.
void Reconcile(SimInstance& instance, const Oracle& oracle, const SimConfig& config,
               SimVerdict& verdict, std::size_t iteration) {
    const SimMode mode = config.mode;
    const std::uint64_t seed = config.seed;
    for (const auto& [table, accepted] : oracle.tables()) {
        const auto synced_it = oracle.synced().find(table);
        const bool table_synced = synced_it != oracle.synced().end();

        const std::string reply = instance.Execute("SELECT * FROM " + table);
        if (IsErr(reply)) {
            if (mode != SimMode::kClean && !table_synced) {
                // Created after the last SYNC and CREATE TABLE is unlogged
                // by design — lost, expected, counted.
                ++verdict.unlogged_ddl_lost_tables;
                continue;
            }
            Fail(verdict, seed, iteration,
                 "after restart, relation '" + table + "' is gone: " + reply);
            continue;
        }

        std::vector<std::string> actual = RowsOf(reply);
        std::sort(actual.begin(), actual.end());

        // No duplicates: a row emitted twice is wrong in every mode.
        for (std::size_t i = 1; i < actual.size(); ++i) {
            if (actual[i] == actual[i - 1]) {
                Fail(verdict, seed, iteration,
                     "after restart, relation '" + table + "' emits a row twice: '" +
                         actual[i] + "'");
            }
        }

        // No fabrication: every row read back must be one the oracle
        // accepted, byte for byte. This also catches a docs/txn.md
        // section 8 ghost — a row whose statement was never acknowledged.
        std::set<std::string> accepted_rendered;
        for (const auto& [id, row] : accepted) {
            accepted_rendered.insert(Oracle::Render(id, row));
        }
        for (const std::string& row : actual) {
            if (!accepted_rendered.count(row)) {
                Fail(verdict, seed, iteration,
                     "after restart, relation '" + table +
                         "' returned a row the oracle never accepted: '" + row + "'");
            }
        }

        const std::set<std::string> present(actual.begin(), actual.end());

        // SYNC's promise holds in every mode: a row synced to the device
        // must be there.
        if (table_synced) {
            for (const auto& [id, row] : synced_it->second) {
                const std::string rendered = Oracle::Render(id, row);
                if (!present.count(rendered)) {
                    Fail(verdict, seed, iteration,
                         "after restart, relation '" + table + "' lost a SYNCed row: '" +
                             rendered + "'");
                }
            }
        }

        // The rest of the acknowledged state. Clean shutdown promises all
        // of it now; a crash only promises it once recovery exists.
        std::size_t missing_unsynced = 0;
        for (const std::string& rendered : accepted_rendered) {
            if (present.count(rendered)) continue;
            const bool was_synced =
                table_synced && [&] {
                    // Rendered strings are unique per row, so a linear
                    // check against the synced snapshot is enough here.
                    for (const auto& [id, row] : synced_it->second) {
                        if (Oracle::Render(id, row) == rendered) return true;
                    }
                    return false;
                }();
            if (was_synced) continue;  // already failed above
            ++missing_unsynced;
        }
        if (missing_unsynced != 0) {
            if (mode == SimMode::kClean || config.assert_recovery) {
                Fail(verdict, seed, iteration,
                     "after restart, relation '" + table + "' is missing " +
                         std::to_string(missing_unsynced) + " acknowledged row(s)");
            } else {
                // [GATED: recovery] — the WAL holds their commit records;
                // this is the count recovery must drive to zero.
                verdict.gated_missing_rows += missing_unsynced;
            }
        }
    }
}

bool RunIteration(const SimConfig& config, std::size_t iteration, SimVerdict& verdict) {
    const Rng iteration_rng =
        Rng(config.seed).Fork("iteration/" + std::to_string(iteration));

    auto instance_or = SimInstance::Create();
    if (!instance_or.ok()) {
        Fail(verdict, config.seed, iteration,
             "instance creation failed: " + instance_or.status().message());
        return false;
    }
    SimInstance& instance = *instance_or.value();

    Workload workload(iteration_rng.Fork("workload"), config.profile);
    Oracle oracle;

    std::size_t stop_at = config.ops;
    if (config.mode != SimMode::kClean) {
        Rng crash_rng = iteration_rng.Fork("crash");
        stop_at = 1 + crash_rng.Below(config.ops);
    }

    for (std::size_t i = 0; i < stop_at; ++i) {
        const Op op = workload.Next();
        if (!ExecuteOp(instance, op, oracle, verdict, config.seed, iteration, i)) return false;
    }

    switch (config.mode) {
        case SimMode::kClean:
            if (Status s = instance.CleanShutdown(); !s.ok()) {
                Fail(verdict, config.seed, iteration, s.message());
                return false;
            }
            break;
        case SimMode::kSyncCrash: {
            const std::string reply = instance.Execute("SYNC");
            if (reply != "OK synced") {
                Fail(verdict, config.seed, iteration, "pre-crash SYNC: " + reply);
                return false;
            }
            oracle.MarkSynced();
            instance.Crash();
            break;
        }
        case SimMode::kCrash:
            instance.Crash();
            break;
    }

    if (Status s = instance.Reboot(); !s.ok()) {
        Fail(verdict, config.seed, iteration, "reboot failed: " + s.message());
        return false;
    }

    const IntegrityReport report =
        CheckInstance(instance.store(), instance.page_device(), instance.catalog());
    if (!report.ok()) {
        Fail(verdict, config.seed, iteration, report.Summary());
        return false;
    }

    Reconcile(instance, oracle, config, verdict, iteration);
    return verdict.ok;
}

}  // namespace

SimVerdict RunSimulation(const SimConfig& config) {
    SimVerdict verdict;
    for (std::size_t i = 0; i < config.iterations; ++i) {
        ++verdict.iterations_run;
        if (!RunIteration(config, i, verdict)) break;
    }
    return verdict;
}

}  // namespace kds::sim
