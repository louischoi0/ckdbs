#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/join_order.hpp"
#include "kds/exec/step_compiler.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// The join planner (instructions/v3.2.0/workorder-cb-join-planner.md):
// range bounds through a join key (CB-S1) and the join order (CB-S2),
// pinned on what a client sees - the reply's rows and ANALYZE's plan.
//
// **Replies compare as multisets** where the statement has no ORDER BY:
// emission order is no contract (CB-Q1 (a)), so a test that pinned it
// would pin the plan instead of the answer.

namespace kds::server {
namespace {

class JoinPlannerTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    void Ok(const std::string& sql) {
        const std::string out = Run(sql);
        ASSERT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    }

    // The reply's rows, sorted, without the header line.
    std::vector<std::string> Rows(const std::string& sql) {
        const std::string out = Run(sql);
        EXPECT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
        std::vector<std::string> rows;
        std::size_t at = out.find("\\n");
        while (at != std::string::npos) {
            const std::size_t next = out.find("\\n", at + 2);
            rows.push_back(out.substr(at + 2, next == std::string::npos ? next : next - at - 2));
            at = next;
        }
        std::sort(rows.begin(), rows.end());
        return rows;
    }

    std::string Plan(const std::string& sql) { return Run("ANALYZE " + sql); }

    // The statement compiled in its written order, or planned under
    // `plan`, against the dispatcher's catalog.
    exec::StepChain Compile(const std::string& sql, const exec::PlanOptions* plan = nullptr) {
        auto parsed = parser::Parse(sql);
        EXPECT_TRUE(parsed.ok()) << parsed.status().message();
        auto chain = exec::Compile(boot_->catalog, std::get<parser::SelectStmt>(parsed.value()),
                                   nullptr, nullptr, nullptr, plan);
        EXPECT_TRUE(chain.ok()) << sql << ": " << chain.status().message();
        return chain.ok() ? std::move(chain.value()) : exec::StepChain{};
    }

    // The cost model's estimate of the written chain, with every bound the
    // planner would read.
    exec::ChainEstimate Estimate(const std::string& sql) {
        const exec::StepChain chain = Compile(sql);
        std::vector<const catalog::TableAccess*> access;
        for (const exec::Step& step : chain.steps) {
            auto a = boot_->catalog.InitTableAccess(step.rel_oid);
            EXPECT_TRUE(a.ok());
            access.push_back(a.ok() ? a.value() : nullptr);
        }
        exec::CostInputs in;
        in.written_sorted = chain.sorted();
        in.base_step_id = chain.steps.empty() ? 0 : chain.steps[0].step_id;
        in.written_access = access;
        return exec::EstimateChain(chain, in);
    }

    // u(id, code): ids 1..4. l(id, uid, v): uid 1..4 and 9, two rows each,
    // v = 10 * uid + row. No index, so a join on l.uid is a walked join.
    void Fill() {
        Ok("CREATE TABLE u (id int64, code int64) BTREE");
        Ok("CREATE TABLE l (id int64, uid int64, v int64) BTREE");
        for (int i = 1; i <= 4; ++i) Ok("INSERT INTO u VALUES (" + std::to_string(100 + i) + ")");
        for (int uid : {1, 2, 3, 4, 9}) {
            for (int row = 0; row < 2; ++row) {
                Ok("INSERT INTO l VALUES (" + std::to_string(uid) + ", " +
                   std::to_string(10 * uid + row) + ")");
            }
        }
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- CB-S1: range bounds through a join key -------------------------------

TEST_F(JoinPlannerTest, ADerivedBoundChangesNoRowWhicheverSideItIsWrittenOn) {
    Fill();
    const std::vector<std::string> want{"20", "21", "30", "31"};
    // The bound on the outer key, on the inner key, both writings of the
    // join, and the strict ends that name the same span.
    for (const char* sql : {
             "SELECT l.v FROM u JOIN l ON l.uid = u.id WHERE u.id BETWEEN 2 AND 3",
             "SELECT l.v FROM u JOIN l ON u.id = l.uid WHERE l.uid BETWEEN 2 AND 3",
             "SELECT l.v FROM l JOIN u ON u.id = l.uid WHERE u.id BETWEEN 2 AND 3",
             "SELECT l.v FROM u JOIN l ON l.uid = u.id WHERE u.id > 1 AND u.id < 4",
             "SELECT l.v FROM l JOIN u ON l.uid = u.id WHERE l.uid > 1 AND u.id <= 3",
         }) {
        EXPECT_EQ(Rows(sql), want) << sql;
    }
}

TEST_F(JoinPlannerTest, ADerivedBoundIsMarkedAndNarrowsTheBuild) {
    Fill();
    const std::string plan =
        Plan("SELECT l.v FROM u JOIN l ON l.uid = u.id WHERE u.id BETWEEN 2 AND 3");
    EXPECT_NE(plan.find("derived"), std::string::npos) << plan;
    // The build buckets the four rows of uid 2 and 3, not all ten.
    EXPECT_NE(plan.find("build_rows=4"), std::string::npos) << plan;
}

TEST_F(JoinPlannerTest, ANullJoinKeyStaysOutWithTheDerivedBound) {
    Ok("CREATE TABLE u (id int64, code int64) BTREE");
    Ok("CREATE TABLE n (id int64, uid int64 NULL, v int64) BTREE");
    for (int i = 1; i <= 3; ++i) Ok("INSERT INTO u VALUES (" + std::to_string(i) + ")");
    Ok("INSERT INTO n VALUES (1, 1)");
    Ok("INSERT INTO n VALUES (NULL, 2)");
    Ok("INSERT INTO n VALUES (3, 3)");
    Ok("INSERT INTO n VALUES (NULL, 4)");
    EXPECT_EQ(Rows("SELECT n.v FROM u JOIN n ON n.uid = u.id WHERE u.id >= 2"),
              std::vector<std::string>{"3"});
    EXPECT_EQ(Rows("SELECT n.v FROM n JOIN u ON n.uid = u.id WHERE u.id < 3"),
              std::vector<std::string>{"1"});
}

// ---- CB-S2: the join order ------------------------------------------------

TEST_F(JoinPlannerTest, AWrittenOrderAlreadyCheapestIsKept) {
    // The outer range is 2 rows of u; the inner side builds once. Written
    // the other way round, every l row would probe u - so the written
    // order stays, and the plan names no reorder.
    Fill();
    const std::string plan =
        Plan("SELECT l.v FROM u JOIN l ON l.uid = u.id WHERE u.id BETWEEN 2 AND 3");
    EXPECT_EQ(plan.find("reordered"), std::string::npos) << plan;
    EXPECT_LT(plan.find("step 0 Range u"), plan.find("step 1 Scan l")) << plan;
}

TEST_F(JoinPlannerTest, TheRestrictedSideDrivesWhicheverWayTheJoinIsWritten) {
    // `l.v = 31` is the one restriction; written with l first it already
    // drives. Written with u first, u would be walked whole and l built
    // for every row of it - so l drives and u is probed by its tuple key.
    Fill();
    for (const char* sql : {
             "SELECT u.code FROM l JOIN u ON u.id = l.uid WHERE l.v = 31",
             "SELECT u.code FROM u JOIN l ON u.id = l.uid WHERE l.v = 31",
         }) {
        EXPECT_EQ(Rows(sql), std::vector<std::string>{"103"}) << sql;
        const std::string plan = Plan(sql);
        EXPECT_NE(plan.find("FilterScan l"), std::string::npos) << plan;
        EXPECT_NE(plan.find("Probe u"), std::string::npos) << plan;
        EXPECT_LT(plan.find("FilterScan l"), plan.find("Probe u")) << plan;
    }
    // The second writing is reordered, and its steps keep their written
    // ids: l is step 1, run first.
    const std::string plan = Plan("SELECT u.code FROM u JOIN l ON u.id = l.uid WHERE l.v = 31");
    EXPECT_NE(plan.find("reordered=1"), std::string::npos) << plan;
    EXPECT_NE(plan.find("step 1 FilterScan l"), std::string::npos) << plan;
}

TEST_F(JoinPlannerTest, AFilteredSecondRelationDrivesASelfJoin) {
    // kwp_session_test.cpp's Keystone-flag shape, pinned as reordered here.
    Ok("CREATE TABLE t (id int64, v int32, name varchar)");
    const std::string plan =
        Plan("SELECT u.id, w.id FROM t AS u JOIN t AS w ON u.id = w.v WHERE w.name = 'x'");
    EXPECT_NE(plan.find("reordered=1"), std::string::npos) << plan;
}

TEST_F(JoinPlannerTest, JoinOrderWrittenKeepsTheWrittenChain) {
    Fill();
    dispatcher_->set_plan_join_order(false);
    const std::string plan = Plan("SELECT u.code FROM u JOIN l ON u.id = l.uid WHERE l.v = 31");
    EXPECT_EQ(plan.find("reordered"), std::string::npos) << plan;
    EXPECT_EQ(Rows("SELECT u.code FROM u JOIN l ON u.id = l.uid WHERE l.v = 31"),
              std::vector<std::string>{"103"});
}

TEST_F(JoinPlannerTest, AConstantProbeBecomesTheDriver) {
    // Scenario 1's model-join: `r.model_id = 3` derives `m.id = 3`, so m
    // is one lookup and drives, and r is walked once - not r walked and m
    // probed once per matching r row.
    Ok("CREATE TABLE m (id int64, name int64) BTREE");
    Ok("CREATE TABLE r (id int64, model_id int64, pnl int64) BTREE");
    for (int i = 1; i <= 3; ++i) Ok("INSERT INTO m VALUES (" + std::to_string(i * 10) + ")");
    for (int i = 0; i < 9; ++i) {
        Ok("INSERT INTO r VALUES (" + std::to_string(1 + i % 3) + ", " + std::to_string(i) + ")");
    }
    const std::string sql =
        "SELECT r.pnl, m.name FROM r JOIN m ON r.model_id = m.id WHERE r.model_id = 3";
    EXPECT_EQ(Rows(sql), (std::vector<std::string>{"2,30", "5,30", "8,30"}));
    const std::string plan = Plan(sql);
    EXPECT_NE(plan.find("reordered=1"), std::string::npos) << plan;
    EXPECT_LT(plan.find("Lookup m"), plan.find("FilterScan r")) << plan;
}

TEST_F(JoinPlannerTest, ATupleKeyAnchoredChainKeepsItsWrittenOrder) {
    // Scenario 1's point-join: a lookup then two probes is the cheapest
    // order there is.
    Ok("CREATE TABLE ex (id int64, name int64) BTREE");
    Ok("CREATE TABLE sy (id int64, ex_id int64) BTREE");
    Ok("CREATE TABLE ba (id int64, sy_id int64, px int64) BTREE");
    Ok("INSERT INTO ex VALUES (7)");
    Ok("INSERT INTO sy VALUES (1)");
    Ok("INSERT INTO ba VALUES (1, 55)");
    const std::string sql =
        "SELECT ba.px, ex.name FROM ba JOIN sy ON ba.sy_id = sy.id "
        "JOIN ex ON sy.ex_id = ex.id WHERE ba.id = 1";
    EXPECT_EQ(Rows(sql), std::vector<std::string>{"55,7"});
    const std::string plan = Plan(sql);
    EXPECT_EQ(plan.find("reordered"), std::string::npos) << plan;
}

TEST_F(JoinPlannerTest, EveryOrderAnswersTheSameRows) {
    // Three relations, every writing of one join: the planner may pick a
    // different order for each, and the rows never move.
    Fill();
    Ok("CREATE TABLE w (id int64, v int64) BTREE");
    for (int v : {20, 21, 31, 40}) Ok("INSERT INTO w VALUES (" + std::to_string(v) + ")");
    const std::vector<std::string> want{"2,20", "2,21", "3,31"};
    for (const char* sql : {
             "SELECT u.id, l.v FROM u JOIN l ON l.uid = u.id JOIN w ON w.v = l.v "
             "WHERE u.id BETWEEN 2 AND 3",
             "SELECT u.id, l.v FROM w JOIN l ON w.v = l.v JOIN u ON l.uid = u.id "
             "WHERE u.id BETWEEN 2 AND 3",
             "SELECT u.id, l.v FROM l JOIN w ON w.v = l.v JOIN u ON l.uid = u.id "
             "WHERE u.id BETWEEN 2 AND 3",
         }) {
        EXPECT_EQ(Rows(sql), want) << sql;
    }
}

TEST_F(JoinPlannerTest, AboveFourRelationsTheGreedyOrderAnswersTheSameRows) {
    Fill();
    // Five relations: the written chain u - l - a - b - c on tuple keys.
    for (const char* t : {"a", "b", "c"}) {
        Ok(std::string("CREATE TABLE ") + t + " (id int64, x int64) BTREE");
        for (int i = 0; i < 12; ++i) {
            Ok(std::string("INSERT INTO ") + t + " VALUES (" + std::to_string(i) + ")");
        }
    }
    const std::string sql =
        "SELECT l.v, c.x FROM c JOIN b ON b.id = c.id JOIN a ON a.id = b.id "
        "JOIN l ON l.id = a.id JOIN u ON u.id = l.uid WHERE u.id = 2";
    // l's tuple keys 3 and 4 are uid 2's rows; a, b and c hold x = id - 1.
    EXPECT_EQ(Rows(sql), (std::vector<std::string>{"20,2", "21,3"}));
}

TEST_F(JoinPlannerTest, ABlockWithASubqueryKeepsItsWrittenOrder) {
    // CB-Q4 (a): sub-chains are placed against slots, so their block is
    // not reordered - even where another order would cost less.
    Fill();
    const std::string sql =
        "SELECT u.code FROM u JOIN l ON u.id = l.uid WHERE l.v = 31 AND "
        "EXISTS (SELECT l2.id FROM l AS l2 WHERE l2.uid = u.id)";
    EXPECT_EQ(Rows(sql), std::vector<std::string>{"103"});
    EXPECT_EQ(Plan(sql).find("reordered"), std::string::npos);
}

// ---- CC-S1: the data-free corrections -------------------------------------

TEST_F(JoinPlannerTest, AUniqueIndexBoundsARowOnlyWhenEveryKeyColumnIsBound) {
    // CC-Q11: UNIQUE(a, b) says nothing about `a` alone.
    Ok("CREATE TABLE p (id int64, a int64, b int64) BTREE");
    Ok("CREATE UNIQUE INDEX p_ab ON p (a, b)");
    EXPECT_EQ(Estimate("SELECT p.id FROM p WHERE p.a = 1 AND p.b = 2").rows, 1.0);
    EXPECT_GT(Estimate("SELECT p.id FROM p WHERE p.a = 1").rows, 1.0);
}

TEST_F(JoinPlannerTest, ATransitionsStatesRaiseAnEqualitysSelectivityNeverLowerIt) {
    // Two states: an equality keeps at least half the rows - the bound
    // raises the constant 1 %, and a column with no Transition keeps it.
    Ok("CREATE TABLE o (id int64, st varchar) BTREE");
    Ok("CREATE TABLE q (id int64, st varchar) BTREE");
    Ok("CREATE TRANSITION flow ON o(st) ON INSERT ('new') ON UPDATE ('new' => 'done')");
    const double with_states = Estimate("SELECT o.id FROM o WHERE o.st = 'new'").rows;
    const double without = Estimate("SELECT q.id FROM q WHERE q.st = 'new'").rows;
    EXPECT_DOUBLE_EQ(with_states, exec::kAssumedRelationRows / 2.0);
    EXPECT_DOUBLE_EQ(without, exec::kAssumedRelationRows * exec::kEqualitySelectivity);
}

TEST_F(JoinPlannerTest, TwoRangesInOneHalfOctaveCostTheSame) {
    // CC-Q12: the span is read as its band's centre, so a plan is a function
    // of the band and not of the literal.
    Fill();
    const double a = Estimate("SELECT u.code FROM u WHERE u.id BETWEEN 1 AND 100").cost;
    const double b = Estimate("SELECT u.code FROM u WHERE u.id BETWEEN 1 AND 105").cost;
    const double c = Estimate("SELECT u.code FROM u WHERE u.id BETWEEN 1 AND 1000").cost;
    EXPECT_DOUBLE_EQ(a, b);
    EXPECT_LT(a, c);
}

TEST_F(JoinPlannerTest, ALimitWithoutASortPricesOnlyThePrefixItReads) {
    Fill();
    const exec::ChainEstimate all = Estimate("SELECT l.v FROM l");
    const exec::ChainEstimate ten = Estimate("SELECT l.v FROM l LIMIT 10");
    EXPECT_EQ(all.rows, ten.rows) << "LIMIT moves the cost, not the estimate";
    EXPECT_LT(ten.cost, all.cost / 1000.0);
}

TEST_F(JoinPlannerTest, AReorderedChainKeepsTheSortItsDriverNoLongerElides) {
    // [quiet-wrong] guard (CC §7): `ORDER BY u.id` is elided while u drives.
    // Planned under LIMIT, l drives - so the chain must sort, and l's rows
    // are inserted against u.id's order, so a lost sort would show in the
    // reply.
    Ok("CREATE TABLE u (id int64, code int64) BTREE");
    Ok("CREATE TABLE l (id int64, uid int64, v int64) BTREE");
    for (int i = 1; i <= 4; ++i) Ok("INSERT INTO u VALUES (" + std::to_string(100 + i) + ")");
    for (int uid : {4, 3, 2, 1}) Ok("INSERT INTO l VALUES (" + std::to_string(uid) + ", 7)");
    const std::string sql = "SELECT u.code FROM u JOIN l ON u.id = l.uid WHERE l.v = 7 "
                            "ORDER BY u.id LIMIT 3";
    const std::string plan = Plan(sql);
    ASSERT_NE(plan.find("reordered=1"), std::string::npos) << plan;
    exec::PlanOptions options;
    const exec::StepChain chain = Compile(sql, &options);
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[0].step_id, 1u) << "l drives";
    EXPECT_TRUE(chain.sorted()) << "the elision belonged to u, which no longer drives";
    EXPECT_EQ(Run(sql), "u.code\\n101\\n102\\n103");
}

TEST_F(JoinPlannerTest, AnOrderThatAddsAnUnboundedSortIsNeverTaken) {
    // CC-Q10: the written order elides `ORDER BY u.id`; the order the model
    // prefers would sort a reply whose size no estimate knows, and could
    // meet `sort_max_rows` where the written order streams. Under a LIMIT
    // within the cap the sort is a top-N heap that cannot refuse, and the
    // order is taken; under one past the cap it is not.
    Fill();
    const std::string base = "SELECT u.code FROM u JOIN l ON u.id = l.uid WHERE l.v = 31 ORDER BY u.id";
    exec::PlanOptions options;
    EXPECT_EQ(Compile(base, &options).steps[0].step_id, 0u) << "u still drives";
    EXPECT_FALSE(Compile(base, &options).sorted());
    EXPECT_EQ(Compile(base + " LIMIT 5", &options).steps[0].step_id, 1u) << "a bounded sort";
    options.sort_max_rows = 4;
    EXPECT_EQ(Compile(base + " LIMIT 5", &options).steps[0].step_id, 0u)
        << "a LIMIT past the cap is no bound";
}

TEST_F(JoinPlannerTest, ATransitionBoundsAJoinKeysSelectivityToo) {
    // CC-Q11: a join on a column with two states keeps at least half the
    // rows per outer row, not the constant model's one.
    Ok("CREATE TABLE o (id int64, st varchar) BTREE");
    Ok("CREATE TABLE q (id int64, st varchar) BTREE");
    Ok("CREATE TRANSITION flow ON o(st) ON INSERT ('new') ON UPDATE ('new' => 'done')");
    const double governed = Estimate("SELECT q.id FROM q JOIN o ON o.st = q.st").rows;
    const double free = Estimate("SELECT o.id FROM o JOIN q ON q.st = o.st").rows;
    EXPECT_DOUBLE_EQ(governed, exec::kAssumedRelationRows * exec::kAssumedRelationRows / 2.0);
    EXPECT_DOUBLE_EQ(free, exec::kAssumedRelationRows);
}

TEST_F(JoinPlannerTest, EveryBuildCapAnswersTheSameRowsPlanned) {
    // The cap is a planner input since CB-S2 (a build past it is priced as
    // the walk), so the inner-build contract's "every cap, the same rows"
    // must hold planned too - as a multiset, since the order may differ.
    Fill();
    const std::vector<std::string> sqls{
        "SELECT u.code, l.v FROM u JOIN l ON l.uid = u.id",
        "SELECT u.code, l.v FROM l JOIN u ON l.uid = u.id WHERE l.v > 15",
        "SELECT u.code, l.v FROM u JOIN l ON l.uid = u.id WHERE u.id BETWEEN 2 AND 4",
    };
    for (const std::string& sql : sqls) {
        dispatcher_->set_join_build_max_rows(exec::kDefaultJoinBuildMaxRows);
        const std::vector<std::string> want = Rows(sql);
        for (std::size_t cap : {std::size_t{0}, std::size_t{2}, std::size_t{4}}) {
            dispatcher_->set_join_build_max_rows(cap);
            EXPECT_EQ(Rows(sql), want) << "cap " << cap << ": " << sql;
        }
    }
}

TEST_F(JoinPlannerTest, EveryAccessKindIsJudgedForTheOrderByElision) {
    // CC §7: a planner that puts any relation on slot 0 relies on this
    // predicate; `EmitsTupleKeyOrder` has no default, so a new kind fails to
    // compile there, and fails here until its verdict is written down.
    using exec::AccessKind;
    const std::vector<std::pair<AccessKind, bool>> verdicts{
        {AccessKind::kLookup, true},     {AccessKind::kProbe, true},
        {AccessKind::kRange, true},      {AccessKind::kCabinProbe, false},
        {AccessKind::kIndexProbe, true}, {AccessKind::kIndexRange, true},
        {AccessKind::kFilterScan, true}, {AccessKind::kScan, true},
    };
    ASSERT_EQ(verdicts.size(), static_cast<std::size_t>(AccessKind::kScan) + 1)
        << "a kind was added: judge it here";
    for (const auto& [kind, emits] : verdicts) {
        EXPECT_EQ(exec::EmitsTupleKeyOrder(kind), emits) << static_cast<int>(kind);
    }
}

}  // namespace
}  // namespace kds::server
