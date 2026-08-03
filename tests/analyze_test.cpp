#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/plan_printer.hpp"
#include "kds/exec/step_vm.hpp"
#include "kds/exec/step_compiler.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/tagged_cell.hpp"

// ANALYZE and the per-step counters behind it.
//
// Two properties carry the feature, and both are about ANALYZE *not*
// being a special path:
//
//   1. The run it describes is the run that happens. Same parse, same
//      compile, same executor - only the reply differs - so a plan cannot
//      describe access that a real execution would not perform.
//   2. The statement a fingerprint sees is the **stripped** text. ANALYZE
//      is a dispatcher prefix, never a parser keyword, so `sys.patterns`
//      and any Waystone trail keyed on a pattern are identical whether or
//      not a client typed it. A prefix that split one pattern in two would
//      make a diagnostic tool change what it is diagnosing.

namespace kds::server {
namespace {

class AnalyzeTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));

        Run("CREATE TABLE acct (id int64, name varchar, tier varchar) BTREE");
        Run("CREATE TABLE trade (id int64, acct_id int64, sym varchar) BTREE");
        for (const char* n : {"alice", "bob", "carol"}) {
            Run(std::string("INSERT INTO acct VALUES ('") + n + "', 'gold')");
        }
        // alice(1) has two trades, bob(2) one, carol(3) none.
        for (int owner : {1, 1, 2}) {
            Run("INSERT INTO trade VALUES (" + std::to_string(owner) + ", 'AAPL')");
        }
    }

    std::string Run(const std::string& sql) {
        CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
        return d.Dispatch(sql).response;
    }

    // The reply with the wire's "\n" escapes turned back into newlines,
    // which is what tools/ckdbs_cli.py does for display.
    std::string Unescaped(const std::string& reply) {
        std::string out;
        for (std::size_t i = 0; i < reply.size(); ++i) {
            if (reply[i] == '\\' && i + 1 < reply.size() && reply[i + 1] == 'n') {
                out.push_back('\n');
                ++i;
            } else {
                out.push_back(reply[i]);
            }
        }
        return out;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
};

// ---- The plan ------------------------------------------------------------

TEST_F(AnalyzeTest, APkJoinReportsScanThenProbe) {
    const std::string plan = Unescaped(
        Run("ANALYZE SELECT acct.name FROM trade JOIN acct ON trade.acct_id = acct.id"));

    // The whole of plan selection in this engine: written order is
    // execution order, and the one decision the compiler made is each
    // step's access kind. step 0 can never be a Probe - its key would come
    // from a step that has not run.
    EXPECT_NE(plan.find("step 0 Scan trade"), std::string::npos) << plan;
    EXPECT_NE(plan.find("step 1 Probe acct"), std::string::npos) << plan;
    EXPECT_NE(plan.find("class=JoinSelect"), std::string::npos) << plan;
}

TEST_F(AnalyzeTest, ALiteralPkEqualityReportsALookup) {
    const std::string plan = Unescaped(Run("ANALYZE SELECT acct.name FROM acct WHERE acct.id = 2"));
    EXPECT_NE(plan.find("step 0 Lookup acct key=2"), std::string::npos) << plan;
    EXPECT_NE(plan.find("class=PointSelect"), std::string::npos) << plan;
}

TEST_F(AnalyzeTest, AnAliasIsReportedBesideItsTable) {
    const std::string plan =
        Unescaped(Run("ANALYZE SELECT a.name FROM acct AS a WHERE a.id = 1"));
    // Both, deliberately: the alias alone cannot be matched back to a
    // table, and the table alone cannot be matched back to the predicate.
    EXPECT_NE(plan.find("acct AS a"), std::string::npos) << plan;
}

TEST_F(AnalyzeTest, ACorrelatedSubchainIsReportedUnderTheStepItRunsFor) {
    const std::string plan = Unescaped(
        Run("ANALYZE SELECT acct.name FROM acct "
            "WHERE EXISTS (SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)"));

    EXPECT_NE(plan.find("correlated EXISTS"), std::string::npos) << plan;
    EXPECT_NE(plan.find("[correlated]"), std::string::npos) << plan;
    // The sub-chain's step shares the statement's global step_id counter,
    // which is what lets the stats vector index by id with no parent link.
    EXPECT_NE(plan.find("step 1 Scan trade"), std::string::npos) << plan;
}

// ---- The counters --------------------------------------------------------

TEST_F(AnalyzeTest, PerStepCountersDistinguishTheSteps) {
    const std::string out = Unescaped(
        Run("ANALYZE SELECT acct.name FROM trade JOIN acct ON trade.acct_id = acct.id"));

    // 3 trades scanned, one descent into acct each. The totals alone could
    // not say which step did which - that is the whole point of the split.
    EXPECT_NE(out.find("step 0 Scan trade opens=1 examined=3"), std::string::npos) << out;
    EXPECT_NE(out.find("step 1 Probe acct opens=3 examined=3"), std::string::npos) << out;
}

TEST_F(AnalyzeTest, SelectivityIsVisiblePerStep) {
    // The correlated scan reads every trade per outer row and keeps the
    // ones matching that row - a step whose selectivity is the reason the
    // statement is quadratic, and which a statement-wide total hides.
    const std::string out = Unescaped(
        Run("ANALYZE SELECT acct.name FROM acct "
            "WHERE EXISTS (SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)"));

    EXPECT_NE(out.find("sel="), std::string::npos) << out;
    EXPECT_NE(out.find("corr_scans=3"), std::string::npos) << out;
    // The outer step keeps every row it reads; the inner one does not.
    EXPECT_NE(out.find("step 0 Scan acct opens=1 examined=3 matched=3 sel=100%"),
              std::string::npos)
        << out;
}

TEST_F(AnalyzeTest, AFalseHoistedExistsLeavesTheOuterStepsUntouched) {
    // The claim is about work *not* done, and work not done leaves no
    // other trace. A step that recorded nothing is omitted rather than
    // printed as zeros, so the absence is the evidence.
    const std::string out = Unescaped(
        Run("ANALYZE SELECT acct.name FROM acct "
            "WHERE EXISTS (SELECT trade.id FROM trade WHERE trade.sym = 'NOSUCH')"));

    EXPECT_NE(out.find("rows=0"), std::string::npos) << out;
    EXPECT_NE(out.find("hoisted EXISTS"), std::string::npos) << out;
    EXPECT_EQ(out.find("step 0 Scan acct opens="), std::string::npos)
        << "the outer relation was opened despite a false hoisted EXISTS:\n"
        << out;
}

TEST_F(AnalyzeTest, VarHeapFetchesAreCountedOnTheStepThatNeededThem) {
    // The one page fetch on the decode path that a row count does not
    // imply: a spilled value costs a var-heap read per row, and only the
    // step reading the wide relation pays it.
    Run("CREATE TABLE wide (id int64, note varchar)");
    const std::uint32_t capacity = storage::InlineCapacity(storage::kDefaultInlineCellWidth);
    const std::string spilled(capacity * 3, 'x');
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(Run("INSERT INTO wide VALUES ('" + spilled + "')").substr(0, 8), "INSERTED");
    }

    const std::string out = Unescaped(Run("ANALYZE SELECT wide.note FROM wide"));
    EXPECT_NE(out.find("spills=3"), std::string::npos) << out;

    // A relation with nothing spillable never reports one.
    const std::string plain = Unescaped(Run("ANALYZE SELECT acct.id FROM acct"));
    EXPECT_EQ(plain.find("spills="), std::string::npos) << plain;
}

// ---- ANALYZE changes nothing else ---------------------------------------

TEST_F(AnalyzeTest, TheAnalyzedStatementStillReturnsItsRowsWithoutThePrefix) {
    const std::string sql = "SELECT acct.name FROM trade JOIN acct ON trade.acct_id = acct.id";
    const std::string rows = Run(sql);

    // The prefix is not a mode the engine stays in.
    EXPECT_EQ(Run(sql), rows);
    EXPECT_NE(rows.find("alice"), std::string::npos) << rows;
    // And a plain SELECT never emits a plan.
    EXPECT_EQ(rows.find("class="), std::string::npos) << rows;
}

TEST_F(AnalyzeTest, AnalyzeWithNoStatementIsRefused) {
    EXPECT_EQ(Run("ANALYZE").substr(0, 3), "ERR");
}

TEST_F(AnalyzeTest, AnalyzeOfANonSelectIsRefused) {
    const std::string reply = Run("ANALYZE INSERT INTO acct VALUES ('x', 'y')");
    EXPECT_EQ(reply.substr(0, 3), "ERR") << reply;
}

TEST_F(AnalyzeTest, AnalyzeOfACatalogViewSaysThereIsNoPlan) {
    const std::string reply = Run("ANALYZE SELECT * FROM sys.tables");
    ASSERT_EQ(reply.substr(0, 3), "ERR") << reply;
    EXPECT_NE(reply.find("no plan"), std::string::npos) << reply;
}

TEST_F(AnalyzeTest, TheReplyIsStillOneWireLine) {
    // The dispatcher's contract: sections are joined with the literal
    // two-character "\n" escape, never a raw newline byte.
    const std::string reply = Run("ANALYZE SELECT acct.name FROM acct WHERE acct.id = 1");
    EXPECT_EQ(reply.find('\n'), std::string::npos) << reply;
    EXPECT_NE(reply.find("\\n"), std::string::npos) << reply;
}

// ---- The fingerprint sees the stripped text ------------------------------

TEST_F(AnalyzeTest, TheAnalyzePrefixWouldChangeAFingerprintIfItSurvived) {
    // Why the strip has to happen in Dispatch rather than anywhere later:
    // the prefixed text is not a statement the fingerprinter recognizes at
    // all, so a pattern taken from it would be absent where the plain
    // statement's is present. This is the failure the strip prevents.
    const std::string sql = "SELECT acct.name FROM acct WHERE acct.id = 1";
    EXPECT_TRUE(parser::FingerprintOf(sql).has_value());
    EXPECT_FALSE(parser::FingerprintOf("ANALYZE " + sql).has_value());
}

TEST_F(AnalyzeTest, AnalyzeAndThePlainStatementCompileToTheSamePlan) {
    // The observable consequence of stripping in Dispatch: what runs under
    // ANALYZE is the same statement text, so it binds, resolves and
    // compiles identically. A plan that differed would mean the prefix had
    // reached the parser.
    const std::string sql = "SELECT acct.name FROM trade JOIN acct ON trade.acct_id = acct.id";
    const std::string analyzed = Unescaped(Run("ANALYZE " + sql));

    auto parsed = parser::Parse(sql);
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    auto chain = exec::Compile(boot_->catalog, std::get<parser::SelectStmt>(parsed.value()));
    ASSERT_TRUE(chain.ok()) << chain.status().message();

    // Every line the printer produces for the directly-compiled chain must
    // appear in the dispatcher's reply.
    const std::string plan = exec::FormatPlan(chain.value());
    std::size_t start = 0;
    while (start < plan.size()) {
        const std::size_t end = plan.find('\n', start);
        const std::string line = plan.substr(start, end - start);
        if (!line.empty()) {
            EXPECT_NE(analyzed.find(line), std::string::npos) << "missing: " << line;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

// ---- ExecStats, on its own -----------------------------------------------
//
// The accumulator is worth testing apart from the executor: it is indexed
// by a step_id the executor supplies, and getting the indexing wrong would
// silently attribute one step's work to another rather than fail.

TEST(ExecStatsTest, CountersAreKeptPerStepAndSummedByTotal) {
    exec::ExecStats stats;
    stats.For(0).rows_examined = 10;
    stats.For(0).rows_matched = 1;
    stats.For(2).rows_examined = 5;
    stats.For(2).spill_fetches = 3;

    // Growing to reach step 2 must not disturb step 0 or invent step 1.
    ASSERT_EQ(stats.steps.size(), 3u);
    EXPECT_EQ(stats.steps[0].rows_examined, 10u);
    EXPECT_EQ(stats.steps[1].rows_examined, 0u);
    EXPECT_EQ(stats.steps[2].rows_examined, 5u);

    const exec::StepStats total = stats.Total();
    EXPECT_EQ(total.rows_examined, 15u);
    EXPECT_EQ(total.rows_matched, 1u);
    EXPECT_EQ(total.spill_fetches, 3u);
}

TEST(ExecStatsTest, TotalOfNothingIsZero) {
    EXPECT_EQ(exec::ExecStats{}.Total().rows_examined, 0u);
}

}  // namespace
}  // namespace kds::server
