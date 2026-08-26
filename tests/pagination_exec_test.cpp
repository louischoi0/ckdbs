#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/pagination.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// V09's execution half - the emission quota (docs/spec/parser-v2.md I11).
//
// The contract this file pins is the one the whole design hangs on:
//
//   **The reply to `LIMIT n OFFSET m` is exactly rows [m, m+n) of the
//   reply the unlimited statement gives.** A prefix of an order the
//   statement already has (I12), not of one it hopes for.
//
// And the optimization claim beside it, measured rather than asserted:
// a filled quota stops the walk on the tuple that filled it, so `LIMIT 1`
// over a multi-page relation touches fewer pages than the full scan -
// checkable through ANALYZE's `pages=`, because work not done leaves no
// other trace (the same argument StepStats::relation_opens makes).

namespace kds::server {
namespace {

using exec::EmissionQuota;
using exec::QuotaVerdict;

// ---- EmissionQuota unit: the verdict sequences ----------------------------

exec::StepChain ChainWith(std::optional<std::uint64_t> limit, std::uint64_t offset) {
    exec::StepChain chain;
    chain.limit = limit;
    chain.offset = offset;
    return chain;
}

TEST(EmissionQuotaTest, UnlimitedEmitsForever) {
    EmissionQuota quota(ChainWith(std::nullopt, 0));
    for (int i = 0; i < 100; ++i) EXPECT_EQ(quota.Note(), QuotaVerdict::kEmit);
}

TEST(EmissionQuotaTest, LimitZeroStopsBeforeTheFirstRow) {
    EmissionQuota quota(ChainWith(0, 0));
    EXPECT_EQ(quota.Note(), QuotaVerdict::kStop);
    EXPECT_EQ(quota.Note(), QuotaVerdict::kStop);
    EXPECT_EQ(quota.emitted(), 0u);
}

TEST(EmissionQuotaTest, TheLastAdmittedRowCarriesTheStop) {
    EmissionQuota quota(ChainWith(2, 0));
    EXPECT_EQ(quota.Note(), QuotaVerdict::kEmit);
    EXPECT_EQ(quota.Note(), QuotaVerdict::kEmitThenStop);
    EXPECT_EQ(quota.emitted(), 2u);
}

TEST(EmissionQuotaTest, OffsetSkipsBeforeAnythingCounts) {
    EmissionQuota quota(ChainWith(2, 3));
    EXPECT_EQ(quota.Note(), QuotaVerdict::kSkip);
    EXPECT_EQ(quota.Note(), QuotaVerdict::kSkip);
    EXPECT_EQ(quota.Note(), QuotaVerdict::kSkip);
    EXPECT_EQ(quota.Note(), QuotaVerdict::kEmit);
    EXPECT_EQ(quota.Note(), QuotaVerdict::kEmitThenStop);
}

TEST(EmissionQuotaTest, OffsetAloneNeverStops) {
    EmissionQuota quota(ChainWith(std::nullopt, 1));
    EXPECT_EQ(quota.Note(), QuotaVerdict::kSkip);
    for (int i = 0; i < 100; ++i) EXPECT_EQ(quota.Note(), QuotaVerdict::kEmit);
}

// ---- End to end through the dispatcher ------------------------------------

class PaginationExecTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
    }

    std::string Run(const std::string& sql) {
        auto out = dispatcher_->Dispatch(sql);
        EXPECT_EQ(out.response.rfind("ERR", 0), std::string::npos) << sql << ": " << out.response;
        return out.response;
    }

    // The value of `key=<n>` in an ANALYZE reply.
    static std::uint64_t MeterOf(const std::string& reply, const std::string& key) {
        const auto at = reply.find(key + "=");
        EXPECT_NE(at, std::string::npos) << key << " not in: " << reply;
        if (at == std::string::npos) return 0;
        return std::strtoull(reply.c_str() + at + key.size() + 1, nullptr, 10);
    }

    void FillFiveRows(const std::string& create) {
        Run(create);
        for (const char* v : {"one", "two", "three", "four", "five"}) {
            Run(std::string("INSERT INTO t VALUES ('") + v + "')");
        }
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

// The prefix-slice contract, spelled out against known data. Ids are
// engine-assigned 1..5 in insert order, and a scan emits in pk order, so
// the unlimited reply is fixed and every tail is a slice of it.
TEST_F(PaginationExecTest, TheReplyIsASliceOfTheUnlimitedReply) {
    FillFiveRows("CREATE TABLE t (id int64, v varchar)");

    EXPECT_EQ(Run("SELECT v FROM t"), "v\\none\\ntwo\\nthree\\nfour\\nfive");
    EXPECT_EQ(Run("SELECT v FROM t LIMIT 2"), "v\\none\\ntwo");
    EXPECT_EQ(Run("SELECT v FROM t OFFSET 2"), "v\\nthree\\nfour\\nfive");
    EXPECT_EQ(Run("SELECT v FROM t LIMIT 2 OFFSET 1"), "v\\ntwo\\nthree");
    EXPECT_EQ(Run("SELECT v FROM t ORDER BY id ASC LIMIT 2"), "v\\none\\ntwo");
    EXPECT_EQ(Run("SELECT v FROM t LIMIT 99"), "v\\none\\ntwo\\nthree\\nfour\\nfive");
}

TEST_F(PaginationExecTest, TheSameSlicesOnABtreeRelation) {
    FillFiveRows("CREATE TABLE t (id int64, v varchar) BTREE");

    EXPECT_EQ(Run("SELECT v FROM t LIMIT 2"), "v\\none\\ntwo");
    EXPECT_EQ(Run("SELECT v FROM t LIMIT 2 OFFSET 1"), "v\\ntwo\\nthree");
    EXPECT_EQ(Run("SELECT v FROM t OFFSET 4"), "v\\nfive");
}

TEST_F(PaginationExecTest, LimitZeroAnswersOnlyTheHeader) {
    FillFiveRows("CREATE TABLE t (id int64, v varchar)");
    EXPECT_EQ(Run("SELECT v FROM t LIMIT 0"), "v");
}

TEST_F(PaginationExecTest, AnOffsetPastTheEndAnswersOnlyTheHeader) {
    FillFiveRows("CREATE TABLE t (id int64, v varchar)");
    EXPECT_EQ(Run("SELECT v FROM t OFFSET 99"), "v");
}

TEST_F(PaginationExecTest, TheQuotaComposesWithAWhere) {
    FillFiveRows("CREATE TABLE t (id int64, v varchar)");
    // OFFSET skips *qualifying* rows: the predicate filters first.
    EXPECT_EQ(Run("SELECT v FROM t WHERE id > 1 LIMIT 2"), "v\\ntwo\\nthree");
    EXPECT_EQ(Run("SELECT v FROM t WHERE id > 1 OFFSET 1 "), "v\\nthree\\nfour\\nfive");
}

TEST_F(PaginationExecTest, AJoinChainStopsAtTheQuota) {
    FillFiveRows("CREATE TABLE t (id int64, v varchar)");
    Run("CREATE TABLE u (id int64, tid int64)");
    Run("INSERT INTO u VALUES (1)");
    Run("INSERT INTO u VALUES (1)");
    Run("INSERT INTO u VALUES (2)");

    EXPECT_EQ(Run("SELECT t.v, u.id FROM t AS t JOIN u AS u ON u.tid = t.id"),
              "t.v,u.id\\none,1\\none,2\\ntwo,3");
    EXPECT_EQ(Run("SELECT t.v, u.id FROM t AS t JOIN u AS u ON u.tid = t.id LIMIT 2"),
              "t.v,u.id\\none,1\\none,2");
    EXPECT_EQ(Run("SELECT t.v, u.id FROM t AS t JOIN u AS u ON u.tid = t.id LIMIT 1 OFFSET 1"),
              "t.v,u.id\\none,2");
}

// The optimization claim: a filled quota ends the walk, so LIMIT 1 over a
// relation spanning several pages fetches fewer than the full scan does.
// ANALYZE is the witness - it runs the quota too, precisely so that the
// run it describes is the run a client gets.
TEST_F(PaginationExecTest, AFilledQuotaStopsTheWalkEarly) {
    Run("CREATE TABLE big (id int64, n int64)");
    for (int i = 0; i < 400; ++i) Run("INSERT INTO big VALUES (7)");

    const std::string full = Run("ANALYZE SELECT n FROM big");
    const std::string limited = Run("ANALYZE SELECT n FROM big LIMIT 1");

    EXPECT_EQ(MeterOf(full, "rows"), 400u);
    EXPECT_EQ(MeterOf(limited, "rows"), 1u);
    ASSERT_GE(MeterOf(full, "pages"), 2u) << "the fixture must span pages to prove anything";
    EXPECT_LT(MeterOf(limited, "pages"), MeterOf(full, "pages"));

    // The plan names the quota, after the projection where it runs.
    EXPECT_NE(limited.find("quota limit=1"), std::string::npos) << limited;
}

TEST(PaginationBudgetTest, TheQuotaSparesTheBudgetAndOffsetStillPaysIt) {
    // The quota bounds *output*, the budget bounds *work* (V19), and this
    // is the pair of cases where the difference is visible. A filled quota
    // stops the walk before the budget is spent, so LIMIT 1 succeeds where
    // the unlimited scan is refused; an OFFSET buys no such escape,
    // because skipped rows are examined rows and charge like any other.
    storage::InMemoryPageStore store{kFirstUserPageId};
    auto boot = bootstrap::BootstrapDatabase(store, 1000);
    ASSERT_TRUE(boot.ok());
    CommandDispatcher d(boot.value().superblock, boot.value().catalog, store,
                        /*log=*/nullptr, /*clock=*/nullptr, /*wal=*/nullptr,
                        wal::DurabilityClass::kGroup, exec::Budget(10));
    auto run = [&](const std::string& sql) { return d.Dispatch(sql).response; };

    ASSERT_EQ(run("CREATE TABLE big (id int64, n int64)").substr(0, 7), "CREATED");
    for (int i = 0; i < 30; ++i) {
        ASSERT_EQ(run("INSERT INTO big VALUES (7)").substr(0, 8), "INSERTED");
    }

    EXPECT_EQ(run("SELECT n FROM big LIMIT 1"), "n\\n7");
    EXPECT_EQ(run("SELECT n FROM big").substr(0, 3), "ERR");
    EXPECT_EQ(run("SELECT n FROM big OFFSET 20").substr(0, 3), "ERR");
}

TEST_F(PaginationExecTest, AnalyzeCountsEmittedRowsAndExaminedStaysHonest) {
    Run("CREATE TABLE big (id int64, n int64)");
    for (int i = 0; i < 50; ++i) Run("INSERT INTO big VALUES (7)");

    const std::string reply = Run("ANALYZE SELECT n FROM big OFFSET 49");
    // One row reaches the client; the 49 the offset discarded were still
    // examined, which is exactly what an OFFSET costs and why the manual
    // recommends the keyset form.
    EXPECT_EQ(MeterOf(reply, "rows"), 1u);
    EXPECT_EQ(MeterOf(reply, "examined"), 50u);
    EXPECT_NE(reply.find("quota offset=49"), std::string::npos) << reply;
}

// ---- The catalog views -----------------------------------------------
//
// A `sys.*` statement never reaches a step chain: the dispatcher answers
// it from the catalog's typed readers before the compiler is asked for
// one. That second row source used to accept the pagination tail and
// enforce none of it - `SELECT name FROM sys.tables LIMIT 2` returned
// every row - so what these pin is that the *contract* is the row
// source's, not the chain's: rows [m, m+n) of the unlimited reply,
// wherever the rows came from.

// Splits a reply into its header line and its rows, on the literal
// two-character "\n" the wire contract uses.
std::vector<std::string> ReplyLines(const std::string& reply) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (true) {
        const std::size_t sep = reply.find("\\n", at);
        if (sep == std::string::npos) {
            out.push_back(reply.substr(at));
            return out;
        }
        out.push_back(reply.substr(at, sep - at));
        at = sep + 2;
    }
}

// The reply a `LIMIT n OFFSET m` must give, built from the unlimited one
// rather than from the bootstrap's relation list - which is a detail of
// what the catalog happens to hold and no part of what pagination
// promises.
std::string SliceOf(const std::string& unlimited, std::size_t offset, std::size_t limit) {
    const std::vector<std::string> lines = ReplyLines(unlimited);
    std::string out = lines.front();
    for (std::size_t i = 1 + offset; i < lines.size() && i < 1 + offset + limit; ++i) {
        out += "\\n" + lines[i];
    }
    return out;
}

TEST_F(PaginationExecTest, ACatalogViewsReplyIsASliceOfItsUnlimitedReply) {
    Run("CREATE TABLE t (id int64, v varchar)");

    const std::string all = Run("SELECT name FROM sys.tables");
    const std::size_t rows = ReplyLines(all).size() - 1;
    ASSERT_GE(rows, 5u) << "the bootstrap catalog must hold enough rows to slice: " << all;

    EXPECT_EQ(Run("SELECT name FROM sys.tables LIMIT 2"), SliceOf(all, 0, 2));
    EXPECT_EQ(Run("SELECT name FROM sys.tables LIMIT 1 OFFSET 3"), SliceOf(all, 3, 1));
    EXPECT_EQ(Run("SELECT name FROM sys.tables OFFSET 2"), SliceOf(all, 2, rows));
    EXPECT_EQ(Run("SELECT name FROM sys.tables LIMIT 99"), all);
    // Only the header: the two ends of the range, and the pair that most
    // easily degrades into "returned everything" unnoticed.
    EXPECT_EQ(Run("SELECT name FROM sys.tables LIMIT 0"), ReplyLines(all).front());
    EXPECT_EQ(Run("SELECT name FROM sys.tables OFFSET 9999"), ReplyLines(all).front());
}

TEST_F(PaginationExecTest, ACatalogViewsQuotaRunsAfterItsWhere) {
    // OFFSET skips *qualifying* rows here for the same reason it does on a
    // chain: the predicate filters, then the quota counts. `sys.columns`
    // is the view with several rows per relation, so the filtered set is
    // big enough to slice and small enough to name.
    Run("CREATE TABLE t (id int64, a int64, b int64, c int64)");
    const std::string all = Run("SELECT name FROM sys.columns WHERE rel_name = 't'");
    ASSERT_EQ(ReplyLines(all).size() - 1, 4u) << all;

    EXPECT_EQ(Run("SELECT name FROM sys.columns WHERE rel_name = 't' LIMIT 2"),
              SliceOf(all, 0, 2));
    EXPECT_EQ(Run("SELECT name FROM sys.columns WHERE rel_name = 't' LIMIT 1 OFFSET 2"),
              SliceOf(all, 2, 1));
}

TEST_F(PaginationExecTest, ACatalogViewRefusesOrderByWithTheByte) {
    // Declined, not ignored. `OutputSort` normalizes its keys against a
    // Schema the view does not have, so the clause cannot be served here -
    // and a clause that cannot be served is refused, never accepted and
    // dropped (which is what this statement used to do).
    const std::string reply =
        dispatcher_->Dispatch("SELECT name FROM sys.tables ORDER BY name DESC").response;
    EXPECT_EQ(reply.rfind("ERR", 0), 0u) << reply;
    EXPECT_NE(reply.find("ORDER BY over a catalog view"), std::string::npos) << reply;
    // The byte of the sort column, which is what the client has to fix.
    EXPECT_NE(reply.find("(byte 37)"), std::string::npos) << reply;
}

}  // namespace
}  // namespace kds::server
