#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/functions.hpp"
#include "kds/exec/step_compiler.hpp"
#include "kds/parser/parser.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// AP-S4: the function catalog and the function conjunct
// (instructions/v3.0.0/workorder-ap-function-catalog-fetch-id.md AP-R4, the
// AP-S4 row; AR1 §2-§3).
//
// The cells that matter most are the **readers** group. A function
// conjunct is lowered to a residual kind of its own because a dozen compiler
// readers take a `StepPredicate` as `col op value` and make it a pk bound,
// an index bound, a Cabin probe, a join key or a build key (§1.7) - and
// `f(col) op v` read as `col op v` returns wrong rows with no error. Each
// reader cell puts a function of a column where that reader looks and
// compares the reply with the plain statement it must equal. The shipped
// functions take only a TIMESTAMP, which can be none of a pk, a join key or
// an int index's column, so these cells register `plus_one(int64)` through
// the test seam.

namespace kds::server {
namespace {

parser::AstValue PlusOne(std::span<const parser::AstValue* const> args,
                         const exec::StatementContext&) {
    parser::AstValue out;
    out.type = parser::ValueType::kInt;
    out.int_val = args[0]->int_val + 1;
    return out;
}

const exec::FunctionEntry kPlusOne{"plus_one", 1, exec::Purity::kImmutable,
                                   catalog::kTypeValInt64, catalog::kTypeValInt64, &PlusOne};

// The same function declaring no purity: AR1 D2's default, kVolatileRow.
const exec::FunctionEntry kUndeclared = [] {
    exec::FunctionEntry e;
    e.name = "undeclared";
    e.arity = 1;
    e.arg_type_val = catalog::kTypeValInt64;
    e.result_type_val = catalog::kTypeValInt64;
    e.evaluate = &PlusOne;
    return e;
}();

class FunctionConjunctTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        recorder_.emplace(boot_->catalog, store_);
        // A Cabin store, or `CREATE CABIN` below declares a Cabin nothing
        // serves from and the Cabin cell walks every time.
        cabins_.emplace();
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), &*recorder_, /*replay=*/true,
                            /*access_statistics=*/true, &*cabins_);

        Ok("CREATE TABLE t (id int64, v int64, k int64, sym varchar, ts timestamp NULL) BTREE");
        Ok("CREATE INDEX t_by_v ON t (v)");
        Ok("CREATE CABIN ON t(sym)");
        Ok("CREATE TABLE u (id int64, w int64) BTREE");
        // ids are issued 1..6; v = 10*id; k = id - 1 for the join; ts spans
        // two UTC days around midnight, one row before the epoch, one NULL.
        Ok("INSERT INTO t VALUES (10, 0, 'a', '2026-10-01 00:00:00')");
        Ok("INSERT INTO t VALUES (20, 1, 'b', '2026-10-01 23:59:59.999999')");
        Ok("INSERT INTO t VALUES (30, 2, 'a', '2026-10-02 00:00:00')");
        Ok("INSERT INTO t VALUES (40, 3, 'b', '1969-12-31 23:00:00')");
        Ok("INSERT INTO t VALUES (50, 4, 'a', NULL)");
        Ok("INSERT INTO t VALUES (60, 5, 'c', '2999-01-01 00:00:00')");
        // u: ids 1..6, w = id - 1 = t.k of the same id. u.w has no index
        // and no Cabin, so a join on it is a walked join that may build.
        for (int i = 0; i < 6; ++i) Ok("INSERT INTO u VALUES (" + std::to_string(i) + ")");
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    void Ok(const std::string& sql) {
        const std::string reply = Run(sql);
        ASSERT_NE(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
    }
    // ANALYZE's row count, which reads the same execution the reply does.
    std::string RowsOf(const std::string& sql) {
        const std::string analyzed = Run("ANALYZE " + sql);
        const auto at = analyzed.find("rows=");
        if (at == std::string::npos) return "no rows= in: " + analyzed;
        return analyzed.substr(at, analyzed.find(' ', at) - at);
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::TrailRecorder> recorder_;
    std::optional<stats::CabinStore> cabins_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- The functions ------------------------------------------------------

TEST_F(FunctionConjunctTest, DateIsTheUtcDayATimestampFallsOn) {
    EXPECT_EQ(Run("SELECT id FROM t WHERE DATE(ts) = '2026-10-01'"),
              Run("SELECT id FROM t WHERE id BETWEEN 1 AND 2"));
    EXPECT_EQ(RowsOf("SELECT id FROM t WHERE DATE(ts) = '2026-10-02'"), "rows=1");
    // Floor, not truncation: an hour before the epoch is the day before it.
    EXPECT_EQ(Run("SELECT id FROM t WHERE DATE(ts) = '1969-12-31'"),
              Run("SELECT id FROM t WHERE id = 4"));
    EXPECT_EQ(RowsOf("SELECT id FROM t WHERE DATE(ts) = '1970-01-01'"), "rows=0");
    EXPECT_EQ(RowsOf("SELECT id FROM t WHERE DATE(ts) >= '2026-10-01'"), "rows=4");
}

TEST_F(FunctionConjunctTest, ANullArgumentMakesTheCallNull) {
    EXPECT_EQ(RowsOf("SELECT id FROM t WHERE DATE(ts) != '2026-10-01'"), "rows=3");
    EXPECT_EQ(Run("SELECT id FROM t WHERE DATE(ts) IS NULL"), Run("SELECT id FROM t WHERE id = 5"));
    EXPECT_EQ(RowsOf("SELECT id FROM t WHERE DATE(ts) IS NOT NULL"), "rows=5");
}

TEST_F(FunctionConjunctTest, NowIsOneInstantForTheWholeStatement) {
    // Two calls compare equal on every row - a clock read per call or per
    // row would fail this on the first microsecond it ticked.
    EXPECT_EQ(RowsOf("SELECT id FROM t WHERE NOW() = NOW()"), "rows=6");
    // The value side (raft-marks-2026-10-02.md §5): the past only.
    EXPECT_EQ(RowsOf("SELECT id FROM t WHERE ts < NOW()"), "rows=4");
    EXPECT_EQ(Run("SELECT id FROM t WHERE ts > NOW()"), Run("SELECT id FROM t WHERE id = 6"));
}

// ---- The readers: a function conjunct is never a key [quiet-wrong] ------

TEST_F(FunctionConjunctTest, OverThePkItIsNotALookup) {
    exec::ScopedTestFunction plus_one(kPlusOne);
    EXPECT_EQ(Run("SELECT id, v FROM t WHERE plus_one(id) = 4"),
              Run("SELECT id, v FROM t WHERE id = 3"));
    EXPECT_EQ(Run("SELECT id, v FROM t WHERE plus_one(id) < 3"),
              Run("SELECT id, v FROM t WHERE id < 2"));
    // The value side, on the pk: `id = plus_one(k)` holds on every row.
    EXPECT_EQ(RowsOf("SELECT id FROM t WHERE id = plus_one(k)"), "rows=6");
}

TEST_F(FunctionConjunctTest, OverAnIndexedColumnItIsNotAnIndexProbe) {
    exec::ScopedTestFunction plus_one(kPlusOne);
    EXPECT_EQ(Run("SELECT id, v FROM t WHERE plus_one(v) = 31"),
              Run("SELECT id, v FROM t WHERE v = 30"));
    EXPECT_EQ(Run("SELECT id, v FROM t WHERE plus_one(v) >= 41"),
              Run("SELECT id, v FROM t WHERE v >= 40"));
}

TEST_F(FunctionConjunctTest, BesideACabinedEqualityItStillFilters) {
    exec::ScopedTestFunction plus_one(kPlusOne);
    // The Cabin serves `sym = 'a'`; the function conjunct must still cut
    // that set, on the first execution (recording) and the second (served).
    const std::string fn = "SELECT id FROM t WHERE sym = 'a' AND plus_one(v) = 31";
    const std::string plain = "SELECT id FROM t WHERE sym = 'a' AND v = 30";
    EXPECT_EQ(Run(fn), Run(plain));
    EXPECT_EQ(Run(fn), Run(plain));
    // And the Cabin is what served it, or this cell says nothing about one.
    const std::string analyzed = Run("ANALYZE " + fn);
    EXPECT_NE(analyzed.find("CabinProbe"), std::string::npos) << analyzed;
    EXPECT_NE(analyzed.find("cabin_hits="), std::string::npos) << analyzed;
}

TEST_F(FunctionConjunctTest, OverAJoinColumnItIsNotAProbeOrABuild) {
    exec::ScopedTestFunction plus_one(kPlusOne);
    // u's ids are 1..6. `plus_one(t.k) = u.id` is `t.k + 1 = u.id`, which
    // holds for every t row; read as `t.k = u.id` it would pair each t row
    // with the u row one below and drop t's first.
    EXPECT_EQ(Run("SELECT t.id, u.w FROM t JOIN u ON t.id = u.id WHERE plus_one(t.k) = u.id"),
              Run("SELECT t.id, u.w FROM t JOIN u ON t.id = u.id"));
    // A walked join (u.w is unindexed) carrying a function conjunct, which
    // holds on every pair: the step takes no build, and read as
    // `t.k = u.id` the conjunct would turn the step into a probe that
    // finds the u row one above and answer nothing.
    EXPECT_EQ(Run("SELECT t.id, u.id FROM t JOIN u ON t.k = u.w WHERE plus_one(t.k) = u.id"),
              Run("SELECT t.id, u.id FROM t JOIN u ON t.k = u.w"));
    EXPECT_EQ(RowsOf("SELECT t.id, u.id FROM t JOIN u ON t.k = u.w"), "rows=6");
}

TEST_F(FunctionConjunctTest, BesideABetweenItIsNotABound) {
    exec::ScopedTestFunction plus_one(kPlusOne);
    EXPECT_EQ(Run("SELECT id FROM t WHERE id BETWEEN 2 AND 5 AND plus_one(id) > 4"),
              Run("SELECT id FROM t WHERE id BETWEEN 4 AND 5"));
}

TEST_F(FunctionConjunctTest, AFunctionArgumentReachingOutwardCorrelatesTheSubquery) {
    exec::ScopedTestFunction plus_one(kPlusOne);
    // The sub-chain's only outward reference is a function conjunct's
    // column, t.id. `plus_one(u.id) = t.id` holds for t.id 2..6. Classified
    // uncorrelated, the sub-chain would be hoisted and run once, with no
    // outer row to read t.id from.
    EXPECT_EQ(Run("SELECT id FROM t WHERE EXISTS (SELECT id FROM u WHERE plus_one(u.id) = t.id)"),
              Run("SELECT id FROM t WHERE id BETWEEN 2 AND 6"));
    EXPECT_EQ(Run("SELECT id FROM t WHERE EXISTS (SELECT id FROM u WHERE plus_one(u.w) = t.id)"),
              Run("SELECT id FROM t"));
}

// Placement, the other half of correlation: the sub-chain's only reference
// into the outer chain's *second* step is a function conjunct's column,
// `u.id`. It must run at that step, after u's row is in the frame;
// `plus_one(x.w) = x.id` for every x, so the EXISTS holds for every joined
// row.
TEST_F(FunctionConjunctTest, ASubqueryReachingALaterStepOnlyThroughAFunctionRunsThere) {
    exec::ScopedTestFunction plus_one(kPlusOne);
    EXPECT_EQ(Run("SELECT t.id FROM t JOIN u ON t.id = u.id WHERE EXISTS "
                  "(SELECT id FROM u AS x WHERE plus_one(x.w) = u.id)"),
              Run("SELECT t.id FROM t JOIN u ON t.id = u.id"));
}

// One instant per statement, not per conjunct: every function conjunct in
// a compiled chain carries the same context.
TEST_F(FunctionConjunctTest, EveryConjunctInAStatementSharesOneInstant) {
    auto parsed = parser::Parse(
        "SELECT id FROM t WHERE ts < NOW() AND ts > NOW() AND EXISTS "
        "(SELECT id FROM u WHERE DATE(t.ts) = '2026-10-01')");
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    auto chain = exec::Compile(boot_->catalog, std::get<parser::SelectStmt>(parsed.value()));
    ASSERT_TRUE(chain.ok()) << chain.status().message();
    std::vector<std::int64_t> instants;
    for (const exec::Step& step : chain.value().steps) {
        for (const exec::FunctionPredicate& pred : step.fn_residual) {
            instants.push_back(pred.context.now_us);
        }
        for (const exec::SubChain& sub : step.sub_chains) {
            for (const exec::Step& inner : sub.steps) {
                for (const exec::FunctionPredicate& pred : inner.fn_residual) {
                    instants.push_back(pred.context.now_us);
                }
            }
        }
    }
    ASSERT_EQ(instants.size(), 3u);
    EXPECT_EQ(instants[0], instants[1]);
    EXPECT_EQ(instants[0], instants[2]);
}

TEST_F(FunctionConjunctTest, APlanShowsAFunctionConjunctUnderItsOwnWord) {
    const std::string analyzed = Run("ANALYZE SELECT id FROM t WHERE DATE(ts) = '2026-10-01'");
    EXPECT_NE(analyzed.find("filter fn date("), std::string::npos) << analyzed;
}

// ---- Writes --------------------------------------------------------------

TEST_F(FunctionConjunctTest, AWriteFiltersOnAFunctionConjunct) {
    const std::string updated = Run("UPDATE t SET k = 99 WHERE DATE(ts) = '2026-10-01'");
    EXPECT_NE(updated.find("UPDATED 2"), std::string::npos) << updated;
    EXPECT_EQ(Run("SELECT id FROM t WHERE k = 99"), Run("SELECT id FROM t WHERE id BETWEEN 1 AND 2"));
    const std::string deleted = Run("DELETE FROM t WHERE ts > NOW()");
    EXPECT_NE(deleted.find("DELETED 1"), std::string::npos) << deleted;
    EXPECT_EQ(RowsOf("SELECT id FROM t"), "rows=5");
}

// ---- The determinism class and the trail --------------------------------

TEST_F(FunctionConjunctTest, AnImmutableCallBesideAKeyRecordsAndReplays) {
    const std::string sql = "SELECT id FROM t WHERE id = 3 AND DATE(ts) = '2026-10-02'";
    const std::string first = Run(sql);
    Run(sql);
    const std::string analyzed = Run("ANALYZE " + sql);
    EXPECT_NE(analyzed.find("replays=1"), std::string::npos) << analyzed;
    EXPECT_EQ(Run(sql), first);
}

// AP-Q5: no D1 fold, so a NOW() statement is D0 to every AP consumer - and
// sound, because the replayed row is re-filtered by this statement's NOW().
TEST_F(FunctionConjunctTest, AStableCallIsD0UntilAqFoldsIt) {
    const std::string sql = "SELECT id FROM t WHERE id = 3 AND ts < NOW()";
    Run(sql);
    Run(sql);
    const std::string analyzed = Run("ANALYZE " + sql);
    EXPECT_NE(analyzed.find("replays=1"), std::string::npos) << analyzed;
}

TEST_F(FunctionConjunctTest, AD2StatementIsNeverSightedRegisteredOrRecorded) {
    exec::ScopedTestFunction undeclared(kUndeclared);
    const std::string sql = "SELECT id FROM t WHERE id = 3 AND undeclared(v) = 31";
    const std::uint64_t sightings = recorder_->stats().sightings;
    const std::string first = Run(sql);
    Run(sql);
    Run(sql);
    EXPECT_EQ(recorder_->stats().sightings, sightings) << "a D2 statement was sighted";
    auto fp = parser::FingerprintOf(sql);
    ASSERT_TRUE(fp.has_value());
    EXPECT_FALSE(boot_->catalog.FindPattern(fp->fetch_id).ok()) << "a D2 statement registered";
    const std::string analyzed = Run("ANALYZE " + sql);
    EXPECT_EQ(analyzed.find("replays="), std::string::npos) << analyzed;
    EXPECT_EQ(Run(sql), first);
    EXPECT_EQ(first, Run("SELECT id FROM t WHERE id = 3"));
}

TEST_F(FunctionConjunctTest, AnEntryDeclaringNoPurityIsVolatileRow) {
    EXPECT_EQ(exec::FunctionEntry{}.purity, exec::Purity::kVolatileRow);
    EXPECT_EQ(kUndeclared.purity, exec::Purity::kVolatileRow);
    EXPECT_EQ(exec::ClassOf(exec::Purity::kVolatileRow), exec::DeterminismClass::kD2);
    EXPECT_EQ(exec::FindFunction("DATE")->purity, exec::Purity::kImmutable);
    EXPECT_EQ(exec::FindFunction("now")->purity, exec::Purity::kStable);
}

// ---- Refusals, each with its position ------------------------------------

TEST_F(FunctionConjunctTest, WhatIsRefusedAndWhere) {
    struct Case {
        const char* sql;
        const char* says;
    };
    const Case cases[] = {
        // AP-Q3: an unknown name is simply wrong, as an unknown column is.
        {"SELECT id FROM t WHERE nosuch(v) = 1", "unknown function 'nosuch' at byte 23"},
        {"SELECT id FROM t WHERE DATE(v) = '2026-10-01'",
         "does not take column 'v': it is not of the argument's type at byte 28"},
        {"SELECT id FROM t WHERE DATE(ts, ts) = '2026-10-01'", "takes 1 argument(s), got 2"},
        {"SELECT id FROM t WHERE DATE(ts) = 5000000",
         "a function comparison's literal at byte 34"},
        {"SELECT id FROM t WHERE v = NOW()", "different types at byte 27"},
        // Understood and not built.
        {"SELECT DATE(ts) FROM t", "NOT_IMPLEMENTED retryable=0 a function call is supported "
                                   "in a WHERE comparison only (byte 7)"},
        {"SELECT id FROM t ORDER BY DATE(ts)", "NOT_IMPLEMENTED"},
        {"SELECT id FROM t GROUP BY DATE(ts)", "NOT_IMPLEMENTED"},
        {"SELECT id FROM t WHERE DATE(DATE(ts)) = '2026-10-01'",
         "a function call inside a function call (byte 28)"},
        {"SELECT id FROM t WHERE DATE('2026-10-01') = '2026-10-01'",
         "a function's argument is a column reference (byte 28)"},
        {"SELECT id FROM t WHERE DATE(ts) BETWEEN '2026-10-01' AND '2026-10-02'",
         "NOT_IMPLEMENTED"},
        {"SELECT id FROM t WHERE DATE(ts) IN (SELECT id FROM u)", "NOT_IMPLEMENTED"},
        // A value position reads no call: named, not "expected value".
        {"SELECT id FROM t WHERE ts BETWEEN NOW() AND NOW()",
         "a function call is supported in a WHERE comparison only (byte 34)"},
        {"UPDATE t SET k = NOW() WHERE id = 1",
         "a function call is supported in a WHERE comparison only (byte 17)"},
        {"SELECT * FROM sys.tables WHERE DATE(oid) = '2026-10-01'",
         "a function call over a catalog view (byte"},
    };
    for (const Case& c : cases) {
        const std::string reply = Run(c.sql);
        EXPECT_EQ(reply.rfind("ERR", 0), 0u) << c.sql << " -> " << reply;
        EXPECT_NE(reply.find(c.says), std::string::npos) << c.sql << " -> " << reply;
    }
}

// An aggregate's name in a predicate keeps the refusal it had before AP-S4:
// it is not a function call, and is never reported as an unknown one.
TEST_F(FunctionConjunctTest, AnAggregatesNameInAPredicateIsNotAFunction) {
    const std::string reply = Run("SELECT id FROM t WHERE SUM(v) = 1");
    EXPECT_EQ(reply.rfind("ERR", 0), 0u) << reply;
    EXPECT_EQ(reply.find("unknown function"), std::string::npos) << reply;
}




}  // namespace
}  // namespace kds::server
