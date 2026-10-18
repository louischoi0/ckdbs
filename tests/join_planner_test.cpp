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

// ---- CC-S3: statistics into the order ---------------------------------------

// Scenario 1's compare-all: 8 models, 2,872 results, model_id round robin.
class StatisticsPlannerTest : public JoinPlannerTest {
protected:
    void FillModels() {
        Ok("CREATE TABLE models (id int64, name int64) BTREE");
        Ok("CREATE TABLE results (id int64, model_id int64, pnl int64) BTREE");
        for (int m = 1; m <= 8; ++m) Ok("INSERT INTO models VALUES (" + std::to_string(m) + ")");
        for (int r = 0; r < 64; ++r) {
            Ok("INSERT INTO results VALUES (" + std::to_string(1 + r % 8) + ", " +
               std::to_string(r) + ")");
        }
        models_ = OidOf("models");
        results_ = OidOf("results");
    }

    catalog::Oid OidOf(const std::string& name) {
        auto oid = boot_->catalog.ResolveRelation("", name, catalog::kNamespacePublic, 0, nullptr);
        EXPECT_TRUE(oid.ok()) << name;
        return oid.ok() ? oid.value() : 0;
    }

    // A planned compile reading `stats_`.
    exec::StepChain Planned(const std::string& sql) {
        exec::PlanOptions options;
        options.statistics = &source_;
        return Compile(sql, &options);
    }

    std::uint64_t FetchIdOf(const std::string& sql) {
        parser::Parser parser(sql);
        EXPECT_TRUE(parser.Parse().ok());
        return parser.fingerprint().has_value() ? parser.fingerprint()->fetch_id : 0;
    }

    static constexpr const char* kCompareAll =
        "SELECT results.pnl, models.name FROM results JOIN models ON results.model_id = models.id";

    stats::PlanStats stats_{1};
    stats::PlanStatsSource source_{&stats_};
    catalog::Oid models_ = 0;
    catalog::Oid results_ = 0;
};

TEST_F(StatisticsPlannerTest, AnEmptySnapshotCompilesTheDataFreeChain) {
    // The regression gate: with nothing published, every chain is S1's.
    FillModels();
    exec::PlanOptions data_free;
    const exec::StepChain a = Compile(kCompareAll, &data_free);
    const exec::StepChain b = Planned(kCompareAll);
    ASSERT_EQ(a.steps.size(), b.steps.size());
    for (std::size_t k = 0; k < a.steps.size(); ++k) {
        EXPECT_EQ(a.steps[k].step_id, b.steps[k].step_id);
        EXPECT_EQ(a.steps[k].kind, b.steps[k].kind);
        EXPECT_EQ(a.steps[k].build.has_value(), b.steps[k].build.has_value());
    }
}

TEST_F(StatisticsPlannerTest, RelationPriorsPutTheSmallRelationFirstAndBuildTheLarge) {
    // CC-S0's flip: priors of 8 and 2,872 rows make models drive and the
    // results walked once into a build, where every relation priced at the
    // constant keeps results driving with a probe per row.
    FillModels();
    EXPECT_EQ(Planned(kCompareAll).steps[0].step_id, 0u) << "data-free: results drives";
    stats_.PublishForTest({{models_, stats::BandOf(8)}, {results_, stats::BandOf(2872)}}, {});
    const exec::StepChain chain = Planned(kCompareAll);
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[0].step_id, 1u) << "models drives";
    EXPECT_TRUE(chain.steps[1].build.has_value()) << "results is built";
    ASSERT_TRUE(chain.plan_note.has_value());
    EXPECT_EQ(chain.plan_note->generation, stats_.generation());
    EXPECT_LT(chain.plan_note->cost_ratio, 0.5);
    // Through the dispatcher, the statistics' plan runs and returns the rows.
    dispatcher_->set_plan_stats(&stats_);
    const std::string plan = Plan(kCompareAll);
    EXPECT_NE(plan.find("reordered=1"), std::string::npos) << plan;
    EXPECT_NE(plan.find("planned order=planned gen="), std::string::npos) << plan;
    EXPECT_EQ(Rows(kCompareAll).size(), 64u);
    dispatcher_->set_plan_stats(nullptr);
}

TEST_F(StatisticsPlannerTest, ABuildPastAQuarterOfTheCapIsPricedAsTheWalk) {
    // CC-Q9: with 64 models, results of 10,000 rows build under a quarter of
    // the 65,536-row cap and the order flips; of 20,000 rows they do not,
    // and a per-row walk of them costs more than results driving.
    FillModels();
    stats_.PublishForTest({{models_, stats::BandOf(64)}, {results_, stats::BandOf(10000)}}, {});
    EXPECT_EQ(Planned(kCompareAll).steps[0].step_id, 1u) << "a build within the margin";
    stats_.PublishForTest({{models_, stats::BandOf(64)}, {results_, stats::BandOf(20000)}}, {});
    EXPECT_EQ(Planned(kCompareAll).steps[0].step_id, 0u) << "past cap / 4: written";
}

TEST_F(StatisticsPlannerTest, ASnapshotThatKnowsNothingOfABlockPricesItAsNoSnapshotDoes) {
    // The margins turn on a fact about the block, never on a snapshot
    // existing: b's estimated build (30,000 rows) lies between a quarter of
    // the cap and the cap, so a cap that tightened on any snapshot would
    // move this plan.
    Ok("CREATE TABLE ja (id int64, x int64) BTREE");
    Ok("CREATE TABLE jb (id int64, x int64, y int64) BTREE");
    Ok("CREATE TABLE other (id int64) BTREE");
    const std::string sql = "SELECT ja.id FROM jb JOIN ja ON ja.x = jb.x WHERE jb.y > 5";
    const exec::StepChain cold = Planned(sql);
    stats_.PublishForTest({{OidOf("other"), stats::BandOf(5)}}, {});
    const exec::StepChain warm = Planned(sql);
    ASSERT_EQ(cold.steps.size(), warm.steps.size());
    for (std::size_t k = 0; k < cold.steps.size(); ++k) {
        EXPECT_EQ(cold.steps[k].step_id, warm.steps[k].step_id);
        EXPECT_EQ(cold.steps[k].build.has_value(), warm.steps[k].build.has_value());
    }
    ASSERT_TRUE(warm.plan_note.has_value());
    EXPECT_EQ(warm.plan_note->cost_ratio, cold.plan_note->cost_ratio);
}

TEST_F(StatisticsPlannerTest, AJoinsEstimatedSizeIsTheSameFromEitherSide) {
    // A non-key join priced from the key side: models -> results and
    // results -> models estimate one size, 2,872 rows, under the priors.
    FillModels();
    stats_.PublishForTest({{models_, stats::BandOf(8)}, {results_, stats::BandOf(2872)}}, {});
    exec::PlanOptions options;
    options.statistics = &source_;
    for (const char* sql : {
             "SELECT results.pnl FROM results JOIN models ON results.model_id = models.id",
             "SELECT results.pnl FROM models JOIN results ON results.model_id = models.id",
         }) {
        const exec::StepChain chain = Compile(sql, &options);
        ASSERT_TRUE(chain.plan_note.has_value()) << sql;
        EXPECT_NEAR(chain.plan_note->estimated_rows.back(), stats::BandCentre(stats::BandOf(2872)),
                    1.0)
            << sql;
    }
}

TEST_F(StatisticsPlannerTest, ADeclineFactKeepsTheOrderTheBuildWouldNeed) {
    // CC-Q9: the build at results' written position declined once, so no
    // order that needs it is priced as a build again. With 64 models a
    // per-row walk of results costs more than results driving, so the
    // order the build made cheap is not taken.
    FillModels();
    stats::PlanKey key;
    key.fetch_id = FetchIdOf(kCompareAll);
    key.n = 2;
    key.oids = {results_, models_, 0, 0};
    exec::PlanOptions options;
    options.statistics = &source_;
    options.fetch_id = key.fetch_id;
    const std::vector<std::pair<catalog::Oid, std::int16_t>> priors{
        {models_, stats::BandOf(64)}, {results_, stats::BandOf(2872)}};
    stats_.PublishForTest(priors, {});
    EXPECT_EQ(Compile(kCompareAll, &options).steps[0].step_id, 1u) << "the control: models drives";
    stats::PlanStatsSnapshot::Posterior declined;
    declined.declines = 0b01;  // results, written position 0
    stats_.PublishForTest(priors, {{key, declined}});
    EXPECT_EQ(Compile(kCompareAll, &options).steps[0].step_id, 0u) << "results drives, as written";
}

TEST_F(StatisticsPlannerTest, AZeroCardinalityNeverSkipsAStep) {
    // [quiet-wrong] guard (CC §7): a posterior of no rows is clamped to one
    // and prices an order; the order still walks, and the reply is whole.
    FillModels();
    stats::PlanStatsSnapshot::Posterior empty;
    for (const unsigned mask : {0b01u, 0b10u, 0b11u}) {
        empty.present = static_cast<std::uint16_t>(empty.present | (1u << mask));
        empty.band[mask] = stats::BandOf(0);
    }
    stats::PlanKey key;
    key.fetch_id = FetchIdOf(kCompareAll);
    key.n = 2;
    key.oids = {results_, models_, 0, 0};
    stats_.PublishForTest({{models_, stats::BandOf(0)}, {results_, stats::BandOf(0)}},
                          {{key, empty}});
    exec::PlanOptions options;
    options.statistics = &source_;
    options.fetch_id = key.fetch_id;
    const exec::StepChain chain = Compile(kCompareAll, &options);
    ASSERT_TRUE(chain.plan_note.has_value());
    for (const double rows : chain.plan_note->estimated_rows) {
        EXPECT_EQ(rows, 1.0) << "the clamped posterior was read";
    }
    dispatcher_->set_plan_stats(&stats_);
    EXPECT_EQ(Rows(kCompareAll).size(), 64u);
    dispatcher_->set_plan_stats(nullptr);
}

TEST_F(StatisticsPlannerTest, ASelfJoinRunsEitherOrderUnderTwoGenerationsWithTheSameRows) {
    // PS11 and CB-Q3 across generations: one relation at two written
    // positions, a posterior saying first that position 0 alone is tiny and
    // then that position 1 is. The driver follows; the step ids stay the
    // written positions; the rows do not move.
    FillModels();
    const std::string sql =
        "SELECT a.pnl, b.pnl FROM results AS a JOIN results AS b ON a.model_id = b.model_id "
        "WHERE a.pnl < 9 AND b.pnl < 9";
    stats::PlanKey key;
    key.fetch_id = FetchIdOf(sql);
    key.n = 2;
    key.oids = {results_, results_, 0, 0};
    exec::PlanOptions options;
    options.statistics = &source_;
    options.fetch_id = key.fetch_id;
    std::vector<std::uint32_t> drivers;
    std::vector<std::vector<std::string>> rows;
    dispatcher_->set_plan_stats(&stats_);
    for (const unsigned small : {0b01u, 0b10u}) {
        stats::PlanStatsSnapshot::Posterior p;
        p.present = static_cast<std::uint16_t>((1u << 0b01) | (1u << 0b10));
        p.band[0b01] = stats::BandOf(small == 0b01u ? 1 : 50000);
        p.band[0b10] = stats::BandOf(small == 0b10u ? 1 : 50000);
        stats_.PublishForTest({{results_, stats::BandOf(50000)}}, {{key, p}});
        const exec::StepChain chain = Compile(sql, &options);
        ASSERT_EQ(chain.steps.size(), 2u);
        drivers.push_back(chain.steps[0].step_id);
        rows.push_back(Rows(sql));
    }
    dispatcher_->set_plan_stats(nullptr);
    EXPECT_EQ(drivers, (std::vector<std::uint32_t>{0, 1}));
    EXPECT_EQ(rows[0], rows[1]);
    EXPECT_FALSE(rows[0].empty());
}

TEST_F(StatisticsPlannerTest, AnAggregatedStatementCompilesItsTwinsStepsUnderTheSamePriors) {
    // AG1 under a snapshot: priors are per relation, so a fold and its
    // unaggregated twin read the same ones and compile the same steps (they
    // key different posteriors - GROUP BY is part of the fetch).
    FillModels();
    stats_.PublishForTest({{models_, stats::BandOf(8)}, {results_, stats::BandOf(2872)}}, {});
    const exec::StepChain plain = Planned(
        "SELECT models.name FROM results JOIN models ON results.model_id = models.id");
    const exec::StepChain folded = Planned(
        "SELECT models.name, COUNT(*) FROM results JOIN models ON results.model_id = models.id "
        "GROUP BY models.name");
    ASSERT_EQ(plain.steps.size(), folded.steps.size());
    for (std::size_t k = 0; k < plain.steps.size(); ++k) {
        EXPECT_EQ(plain.steps[k].step_id, folded.steps[k].step_id);
        EXPECT_EQ(plain.steps[k].kind, folded.steps[k].kind);
    }
}

TEST_F(StatisticsPlannerTest, AboveFourRelationsTheGreedyOrderReadsPriorsAndKeepsTheRows) {
    // Five relations, ordered greedily from priors alone; the rows are the
    // data-free plan's.
    FillModels();
    for (const char* t : {"t3", "t4", "t5"}) {
        Ok(std::string("CREATE TABLE ") + t + " (id int64, x int64) BTREE");
        for (int i = 0; i < 9; ++i) {
            Ok(std::string("INSERT INTO ") + t + " VALUES (" + std::to_string(i) + ")");
        }
    }
    const std::string sql =
        "SELECT results.pnl, t5.x FROM results JOIN models ON results.model_id = models.id "
        "JOIN t3 ON t3.id = models.id JOIN t4 ON t4.id = t3.id JOIN t5 ON t5.id = t4.id";
    const std::vector<std::string> cold = Rows(sql);
    stats_.PublishForTest({{models_, stats::BandOf(8)},
                           {results_, stats::BandOf(2872)},
                           {OidOf("t3"), stats::BandOf(9)},
                           {OidOf("t4"), stats::BandOf(9)},
                           {OidOf("t5"), stats::BandOf(9)}},
                          {});
    const exec::StepChain chain = Planned(sql);
    ASSERT_TRUE(chain.plan_note.has_value());
    EXPECT_EQ(chain.plan_note->generation, stats_.generation());
    EXPECT_FALSE(chain.planned.has_value()) << "no posterior key above four relations";
    dispatcher_->set_plan_stats(&stats_);
    EXPECT_EQ(Rows(sql), cold);
    dispatcher_->set_plan_stats(nullptr);
}

// ---- CC-S4: the order memo ---------------------------------------------------

class PlanMemoTest : public StatisticsPlannerTest {
protected:
    // A planned compile through the memo, keyed by the statement's fetch.
    exec::StepChain Memoized(const std::string& sql, exec::PlanMemo* memo = nullptr) {
        parser::Parser parser(sql);
        EXPECT_TRUE(parser.Parse().ok());
        const auto fingerprint = parser.fingerprint();
        exec::PlanOptions options;
        options.statistics = &source_;
        options.memo = memo != nullptr ? memo : &memo_;
        options.fetch_id = fingerprint.has_value() ? fingerprint->fetch_id : 0;
        options.arg_hash = fingerprint.has_value() ? fingerprint->arg_hash : 0;
        options.literals_known = fingerprint.has_value() && fingerprint->param_count == 0;
        return Compile(sql, &options);
    }

    // Two chains equal step by step in every field a plan is made of.
    static void ExpectSameChain(const exec::StepChain& a, const exec::StepChain& b) {
        ASSERT_EQ(a.steps.size(), b.steps.size());
        for (std::size_t k = 0; k < a.steps.size(); ++k) {
            EXPECT_EQ(a.steps[k].step_id, b.steps[k].step_id) << k;
            EXPECT_EQ(a.steps[k].kind, b.steps[k].kind) << k;
            EXPECT_EQ(a.steps[k].build.has_value(), b.steps[k].build.has_value()) << k;
            EXPECT_EQ(a.steps[k].range.has_value(), b.steps[k].range.has_value()) << k;
            EXPECT_EQ(a.steps[k].residual.size(), b.steps[k].residual.size()) << k;
        }
        EXPECT_EQ(a.sorted(), b.sorted());
    }

    static std::vector<std::uint32_t> Order(const exec::StepChain& chain) {
        std::vector<std::uint32_t> ids;
        for (const exec::Step& step : chain.steps) ids.push_back(step.step_id);
        return ids;
    }

    exec::PlanMemo memo_;
};

TEST_F(PlanMemoTest, AHitCompilesTheOrderTheSearchChose) {
    // Purity: the remembered order is the searched one, under one set of
    // inputs, whichever order that is.
    FillModels();
    stats_.PublishForTest({{models_, stats::BandOf(8)}, {results_, stats::BandOf(2872)}}, {});
    const exec::StepChain searched = Memoized(kCompareAll);
    EXPECT_EQ(memo_.misses(), 1u);
    const exec::StepChain remembered = Memoized(kCompareAll);
    EXPECT_EQ(memo_.hits(), 1u);
    EXPECT_EQ(Order(remembered), Order(searched));
    EXPECT_EQ(Order(remembered), (std::vector<std::uint32_t>{1, 0}));
    ASSERT_TRUE(remembered.plan_note.has_value());
    EXPECT_TRUE(remembered.plan_note->from_memo);
    EXPECT_EQ(remembered.steps[1].build.has_value(), searched.steps[1].build.has_value());
}

TEST_F(PlanMemoTest, ANewGenerationOrACatalogChangeSearchesAgain) {
    FillModels();
    Memoized(kCompareAll);
    Memoized(kCompareAll);
    ASSERT_EQ(memo_.hits(), 1u);
    // The statistics move: a new key, so a search, and its own order.
    stats_.PublishForTest({{models_, stats::BandOf(8)}, {results_, stats::BandOf(2872)}}, {});
    EXPECT_EQ(Order(Memoized(kCompareAll)), (std::vector<std::uint32_t>{1, 0}));
    EXPECT_EQ(memo_.hits(), 1u) << "the generation moved";
    // A DDL moves the catalog's stamp.
    Ok("CREATE INDEX results_model ON results (model_id)");
    Memoized(kCompareAll);
    EXPECT_EQ(memo_.hits(), 1u) << "the catalog moved";
}

TEST_F(PlanMemoTest, ARecreatedRelationIsNeverServedTheOldEntry) {
    // Oids are never reissued, and the key holds them exactly: a DROP and a
    // CREATE of one name under one fetch id reach a new entry.
    FillModels();
    Memoized(kCompareAll);
    Ok("DROP TABLE results");
    Ok("CREATE TABLE results (id int64, model_id int64, pnl int64) BTREE");
    Memoized(kCompareAll);
    EXPECT_EQ(memo_.hits(), 0u);
}

TEST_F(PlanMemoTest, AnotherLiteralSearchesAgainAndTheSameLiteralHits) {
    // The literals are in the key: a range on a relation the written order
    // probes - so the written chain never shows it - can flip the order in
    // another, and so can a LIMIT. Each literal searches; a repeat hits.
    FillModels();
    const std::string narrow =
        "SELECT results.pnl FROM results JOIN models ON results.model_id = models.id "
        "WHERE models.id BETWEEN 1 AND 3";
    const std::string wide =
        "SELECT results.pnl FROM results JOIN models ON results.model_id = models.id "
        "WHERE models.id BETWEEN 1 AND 100000";
    exec::PlanMemo fresh;
    const exec::StepChain narrow_searched = Memoized(narrow, &fresh);
    const exec::StepChain wide_searched = Memoized(wide, &fresh);
    EXPECT_EQ(fresh.hits(), 0u);
    ExpectSameChain(Memoized(narrow), narrow_searched);
    ExpectSameChain(Memoized(wide), wide_searched);
    EXPECT_EQ(memo_.hits(), 0u) << "two literals, two keys";
    ExpectSameChain(Memoized(narrow), narrow_searched);
    EXPECT_EQ(memo_.hits(), 1u);
    Memoized(kCompareAll + std::string(" LIMIT 10"));
    Memoized(kCompareAll + std::string(" LIMIT 20000000"));
    EXPECT_EQ(memo_.hits(), 1u) << "another LIMIT, another key";
}

TEST_F(PlanMemoTest, AThreeRelationHitIsTheSearchsChain) {
    // Purity over a larger block: a hit and a search with a cleared memo
    // compile the same chain.
    FillModels();
    Ok("CREATE TABLE fam (id int64, label int64) BTREE");
    for (int i = 0; i < 3; ++i) Ok("INSERT INTO fam VALUES (" + std::to_string(i) + ")");
    stats_.PublishForTest({{models_, stats::BandOf(8)},
                           {results_, stats::BandOf(2872)},
                           {OidOf("fam"), stats::BandOf(3)}},
                          {});
    const std::string sql =
        "SELECT results.pnl, fam.label FROM results JOIN models ON results.model_id = models.id "
        "JOIN fam ON fam.id = models.name";
    Memoized(sql);
    const exec::StepChain hit = Memoized(sql);
    EXPECT_EQ(memo_.hits(), 1u);
    exec::PlanMemo cleared;
    ExpectSameChain(hit, Memoized(sql, &cleared));
}

TEST_F(PlanMemoTest, AnotherNamespacesRelationOfOneNameIsAnotherKey) {
    // `USE ns` resolves one statement text to other relations: their oids
    // are in the key, so the entry the public relations made is not served.
    FillModels();
    Memoized(kCompareAll);
    Ok("CREATE NAMESPACE other");
    Ok("CREATE TABLE other.models (id int64, name int64) BTREE");
    Ok("CREATE TABLE other.results (id int64, model_id int64, pnl int64) BTREE");
    exec::PlanOptions options;
    parser::Parser parser(kCompareAll);
    ASSERT_TRUE(parser.Parse().ok());
    auto parsed = parser::Parse(kCompareAll);
    ASSERT_TRUE(parsed.ok());
    exec::StatementContext context = exec::StatementContextNow();
    auto other = boot_->catalog.FindNamespaceOidByName("other");
    ASSERT_TRUE(other.ok()) << other.status().message();
    context.default_namespace = other.value();
    options.statistics = &source_;
    options.memo = &memo_;
    options.fetch_id = parser.fingerprint()->fetch_id;
    options.arg_hash = parser.fingerprint()->arg_hash;
    options.literals_known = true;
    auto chain = exec::Compile(boot_->catalog, std::get<parser::SelectStmt>(parsed.value()), nullptr,
                               nullptr, &context, &options);
    ASSERT_TRUE(chain.ok()) << chain.status().message();
    EXPECT_EQ(memo_.hits(), 0u);
    EXPECT_NE(chain.value().steps[0].rel_oid, results_);
}

TEST_F(PlanMemoTest, TheDispatchersMemoServesARepeatedStatement) {
    FillModels();
    dispatcher_->set_plan_stats(&stats_);
    stats_.PublishForTest({{models_, stats::BandOf(8)}, {results_, stats::BandOf(2872)}}, {});
    Rows(kCompareAll);
    const std::string plan = Plan(kCompareAll);
    EXPECT_NE(plan.find("planned order=memo"), std::string::npos) << plan;
    EXPECT_EQ(Rows(kCompareAll).size(), 64u);
    dispatcher_->set_plan_stats(nullptr);
}

}  // namespace
}  // namespace kds::server
