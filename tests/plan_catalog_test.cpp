#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/captured_plans.hpp"
#include "kds/exec/declaration_relation.hpp"
#include "kds/exec/plan_catalog.hpp"
#include "kds/parser/lexer.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/expeditor.hpp"
#include "kds/stats/plan_stats.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "reclaim_census.hpp"

// `sys.plans` (CX-S1a, instructions/v3.2.0/workorder-cx-managed-plans.md;
// exec/plan_catalog.hpp): the join orders `join_order = fixed` captured,
// written off the statement by a `system` step and read back at a mount.
// What this file pins: a statement writes nothing; what a step wrote is in
// force after a clean stop and after a crash, with no search; `DROP PLAN`
// re-arms a capture; a dropped relation's plans go; and a row is read as
// what it says or not at all.

namespace kds::server {
namespace {

using exec::CapturedKey;
using exec::CapturedOrder;
using exec::CapturedPlan;
using exec::CapturedPlans;

// ---- The shape text -----------------------------------------------------------

TEST(ShapeTextTest, EveryValueLiteralIsWrittenAsAMarkerAndNothingElseMoves) {
    EXPECT_EQ(parser::ShapeText("SELECT a.x FROM a   JOIN b ON a.id = b.id\n"
                                "WHERE a.x > 5 AND b.s = 'a, b' AND a.d = 1.50 LIMIT 10;"),
              "SELECT a.x FROM a JOIN b ON a.id = b.id WHERE a.x > ? AND b.s = ? AND a.d = ? "
              "LIMIT ?");
    // A signed literal is one value; a bound parameter stays one; NULL is
    // shape, not a value.
    EXPECT_EQ(parser::ShapeText("SELECT x FROM t WHERE a = -7 AND b = ? AND c IS NULL"),
              "SELECT x FROM t WHERE a = ? AND b = ? AND c IS NULL");
    // Two statements of one shape give one text.
    EXPECT_EQ(parser::ShapeText("SELECT x FROM t WHERE a = 1"),
              parser::ShapeText("SELECT x  FROM t WHERE a = 'one' ;"));
    EXPECT_EQ(parser::ShapeText(""), "");
}

// ---- The row's text -----------------------------------------------------------

CapturedPlan PlanOf(std::uint64_t fetch_id, std::vector<catalog::Oid> oids,
                    std::vector<std::uint8_t> order, std::string statement = "SELECT 1") {
    CapturedPlan plan;
    plan.key.fetch_id = fetch_id;
    plan.key.n = static_cast<std::uint8_t>(oids.size());
    for (std::size_t i = 0; i < oids.size(); ++i) {
        plan.key.oids[i] = oids[i];
        plan.order[i] = order[i];
    }
    plan.statement = std::move(statement);
    return plan;
}

TEST(PlanTextTest, ARowsTextReadsBackAsWhatWasWritten) {
    const CapturedPlan plan =
        PlanOf(7, {4001, 4002, 4003}, {2, 0, 1}, "SELECT a.x FROM a JOIN b ON a.id = b.id\nWHERE 1");
    const std::string text = exec::EncodePlanText(plan, 1'700'000'000'000'000);
    EXPECT_EQ(text.substr(0, text.find('\n')),
              "v1 oids=4001,4002,4003 order=2,0,1 at=1700000000000000");
    exec::PlanRow row;
    ASSERT_TRUE(exec::DecodePlanText(text, row).ok());
    EXPECT_EQ(row.key.n, 3u);
    EXPECT_EQ(row.key.oids, plan.key.oids);
    EXPECT_EQ(row.order, plan.order);
    EXPECT_EQ(row.written_at_us, 1'700'000'000'000'000);
    EXPECT_EQ(row.statement, plan.statement) << "a statement's own newline is the statement's";
}

TEST(PlanTextTest, AStatementPastOneStoredValueIsCutAndTheKeyStillReads) {
    const CapturedPlan plan = PlanOf(7, {4001, 4002}, {1, 0}, std::string(20000, 'x'));
    const std::string text = exec::EncodePlanText(plan, 5);
    EXPECT_EQ(text.size(), varheap::kMaxValueSize);
    exec::PlanRow row;
    ASSERT_TRUE(exec::DecodePlanText(text, row).ok());
    EXPECT_EQ(row.key.oids, plan.key.oids);
    EXPECT_EQ(row.order, plan.order);
}

TEST(PlanTextTest, ATextThatIsNotARowIsCorruptionAndNeverAGuess) {
    for (const char* text : {
             "",
             "v1 oids=4001,4002 order=1,0 at=5",              // no statement line
             "v2 oids=4001,4002 order=1,0 at=5\nSELECT 1",    // another format
             "v1 order=1,0 oids=4001,4002 at=5\nSELECT 1",    // fields out of order
             "v1 oids=4001,4002 order=1,0\nSELECT 1",         // a field missing
             "v1 oids=4001,4002 order=1,0 at=5 x=1\nSELECT 1",  // a field too many
             "v1 oids=4001 order=0 at=5\nSELECT 1",           // one relation is no join
             "v1 oids=4001,4002 order=1 at=5\nSELECT 1",      // an order too short
             "v1 oids=4001,4002 order=1,1 at=5\nSELECT 1",    // not a permutation
             "v1 oids=4001,4002 order=0,2 at=5\nSELECT 1",    // a position past the block
             "v1 oids=4001,x order=1,0 at=5\nSELECT 1",       // not a number
             "v1 oids=4001,4002, order=1,0 at=5\nSELECT 1",   // an empty element
             "v1 oids=4001,99999999999 order=1,0 at=5\nSELECT 1",  // an oid past 32 bits
         }) {
        exec::PlanRow row;
        const Status s = exec::DecodePlanText(text, row);
        EXPECT_EQ(s.code(), StatusCode::kCorruption) << '"' << text << '"';
    }
    // Seventeen relations: one more than a key holds.
    std::string wide = "v1 oids=";
    std::string order = " order=";
    for (int i = 0; i < 17; ++i) {
        wide += (i == 0 ? "" : ",") + std::to_string(4001 + i);
        order += (i == 0 ? "" : ",") + std::to_string(i);
    }
    exec::PlanRow row;
    EXPECT_EQ(exec::DecodePlanText(wide + order + " at=5\nSELECT 1", row).code(),
              StatusCode::kCorruption);
}

// ---- The table's queues -------------------------------------------------------

TEST(CapturedPlansQueueTest, AnInstallIsQueuedOnceAndALoadIsNot) {
    CapturedPlans table;
    const CapturedPlan a = PlanOf(1, {4001, 4002}, {1, 0});
    const CapturedPlan b = PlanOf(2, {4001, 4003}, {0, 1});
    ASSERT_TRUE(table.Capture(a.key, a.order, "SELECT x FROM t WHERE y = 5").has_value());
    ASSERT_TRUE(table.Capture(a.key, b.order, "another").has_value()) << "answered the first";
    EXPECT_TRUE(table.Load(b.key, b.order));
    EXPECT_FALSE(table.Load(b.key, a.order)) << "a key holds one order";
    EXPECT_EQ(table.shapes(), 2u);
    EXPECT_EQ(table.pending(), 1u);
    EXPECT_EQ(table.loaded(), 1u);
    const std::vector<CapturedPlan> taken = table.TakePending(8);
    ASSERT_EQ(taken.size(), 1u);
    EXPECT_EQ(taken[0].key, a.key);
    EXPECT_EQ(taken[0].order, a.order);
    EXPECT_EQ(taken[0].statement, "SELECT x FROM t WHERE y = ?");
    EXPECT_EQ(table.pending(), 0u);
}

TEST(CapturedPlansQueueTest, WhatAStepCouldNotWriteGoesBackAheadUnlessItsShapeWasForgotten) {
    CapturedPlans table;
    for (std::uint64_t f = 1; f <= 4; ++f) {
        const CapturedPlan plan = PlanOf(f, {4000 + static_cast<catalog::Oid>(f), 4100}, {1, 0});
        ASSERT_TRUE(table.Capture(plan.key, plan.order).has_value());
    }
    std::vector<CapturedPlan> taken = table.TakePending(3);  // 1, 2, 3
    ASSERT_EQ(taken.size(), 3u);
    EXPECT_EQ(table.pending(), 1u);
    table.ForgetRelation(4002);  // shape 2, while it is out with the step
    taken.erase(taken.begin());  // the step wrote shape 1
    table.Requeue(std::move(taken));
    const std::vector<CapturedPlan> again = table.TakePending(8);
    ASSERT_EQ(again.size(), 2u);
    EXPECT_EQ(again[0].key.fetch_id, 3u) << "what came back is ahead of what waited";
    EXPECT_EQ(again[1].key.fetch_id, 4u);
    EXPECT_EQ(table.TakeForgotten(), std::vector<catalog::Oid>{4002});
    EXPECT_TRUE(table.TakeForgotten().empty());
}

TEST(CapturedPlansQueueTest, ForgettingARelationTakesItsShapesHeldAndQueued) {
    CapturedPlans table;
    const CapturedPlan held = PlanOf(1, {4001, 4002}, {1, 0});
    const CapturedPlan queued = PlanOf(2, {4003, 4001}, {0, 1});
    const CapturedPlan other = PlanOf(3, {4002, 4003}, {0, 1});
    ASSERT_TRUE(table.Load(held.key, held.order));
    ASSERT_TRUE(table.Capture(queued.key, queued.order).has_value());
    ASSERT_TRUE(table.Capture(other.key, other.order).has_value());
    table.ForgetRelation(4001);
    EXPECT_EQ(table.shapes(), 1u);
    EXPECT_FALSE(table.Find(held.key).has_value());
    EXPECT_FALSE(table.Find(queued.key).has_value());
    EXPECT_TRUE(table.Find(other.key).has_value());
    EXPECT_EQ(table.pending(), 1u);
    // And one shape by its key, as `DROP PLAN` does.
    EXPECT_TRUE(table.Erase(other.key));
    EXPECT_FALSE(table.Erase(other.key));
    EXPECT_EQ(table.shapes(), 0u);
}

// ---- Rows, through a dispatcher ----------------------------------------------

class PlanCatalogTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay=*/false,
                            /*access_statistics=*/true, /*cabins=*/nullptr, &*mgr_);
        dispatcher_->set_plan_stats(&stats_);
        dispatcher_->set_join_order(exec::JoinOrder::kFixed);
        Ok("CREATE TABLE models (id int64, name int64) BTREE");
        Ok("CREATE TABLE results (id int64, model_id int64, pnl int64) BTREE");
        for (int m = 1; m <= 8; ++m) Ok("INSERT INTO models VALUES (" + std::to_string(m) + ")");
        for (int r = 0; r < 64; ++r) {
            Ok("INSERT INTO results VALUES (" + std::to_string(1 + r % 8) + ", " +
               std::to_string(r) + ")");
        }
        models_ = OidOf("models");
        results_ = OidOf("results");
        PublishModelsSmall();
    }
    void TearDown() override { dispatcher_->set_plan_stats(nullptr); }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    void Ok(const std::string& sql) {
        const std::string out = Run(sql);
        ASSERT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    }
    std::string Plan(const std::string& sql) { return Run("ANALYZE " + sql); }
    catalog::Oid OidOf(const std::string& name) {
        auto oid = boot_->catalog.ResolveRelation("", name, catalog::kNamespacePublic, 0, nullptr);
        EXPECT_TRUE(oid.ok()) << name;
        return oid.ok() ? oid.value() : 0;
    }
    // Priors under which the search puts models first, and their reverse.
    void PublishModelsSmall() {
        stats_.PublishForTest({{models_, stats::BandOf(8)}, {results_, stats::BandOf(2872)}}, {});
    }
    void PublishModelsLarge() {
        stats_.PublishForTest({{models_, stats::BandOf(2872)}, {results_, stats::BandOf(8)}}, {});
    }

    CapturedPlans& table() { return dispatcher_->captured_plans(); }
    Status Persist(std::int64_t now_us = 42) {
        return exec::PersistCapturedPlans(boot_->catalog, store_, nullptr, table(), now_us);
    }
    std::vector<exec::PlanRow> Rows() {
        auto rows = exec::ListPlans(boot_->catalog, store_);
        EXPECT_TRUE(rows.ok()) << rows.status().message();
        return rows.ok() ? rows.value() : std::vector<exec::PlanRow>{};
    }
    // The compare-all join captured and written; its row.
    exec::PlanRow CaptureAndPersist() {
        EXPECT_NE(Plan(kWhere).find(" capture=first"), std::string::npos);
        EXPECT_TRUE(Persist().ok());
        const std::vector<exec::PlanRow> rows = Rows();
        EXPECT_EQ(rows.size(), 1u);
        return rows.empty() ? exec::PlanRow{} : rows.front();
    }

    static constexpr const char* kWhere =
        "SELECT results.pnl, models.name FROM results JOIN models ON results.model_id = models.id "
        "WHERE results.pnl > 5";
    static constexpr const char* kCaptured = "planned order=captured\\n";

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    stats::PlanStats stats_{1};
    std::optional<CommandDispatcher> dispatcher_;
    catalog::Oid models_ = 0;
    catalog::Oid results_ = 0;
};

TEST_F(PlanCatalogTest, AStatementWritesNoRowAndTheStepWritesTheCaptureWhole) {
    ASSERT_NE(Plan(kWhere).find(" capture=first"), std::string::npos);
    EXPECT_TRUE(Rows().empty()) << "CX-R8: the statement wrote nothing";
    EXPECT_EQ(table().pending(), 1u);

    ASSERT_TRUE(Persist(/*now_us=*/1'700'000'000'000'000).ok());
    const std::vector<exec::PlanRow> rows = Rows();
    ASSERT_EQ(rows.size(), 1u);
    const exec::PlanRow& row = rows.front();
    EXPECT_TRUE(row.captured);
    ASSERT_TRUE(row.readable);
    EXPECT_NE(row.key.fetch_id, 0u);
    EXPECT_EQ(row.name,
              "captured_" + std::to_string(row.key.fetch_id) + "_" + std::to_string(row.id));
    EXPECT_EQ(row.key.n, 2u);
    EXPECT_EQ(row.key.oids[0], results_);
    EXPECT_EQ(row.key.oids[1], models_);
    EXPECT_EQ(row.order[0], 1u) << "models, the second written, runs first";
    EXPECT_EQ(row.order[1], 0u);
    EXPECT_EQ(row.written_at_us, 1'700'000'000'000'000);
    EXPECT_EQ(row.statement,
              "SELECT results.pnl, models.name FROM results JOIN models ON results.model_id = "
              "models.id WHERE results.pnl > ?")
        << "the shape, never the client's value";
    // The row is what the table holds for the shape.
    EXPECT_EQ(table().Find(row.key), row.order);
    EXPECT_EQ(table().pending(), 0u);
    EXPECT_EQ(table().persisted(), 1u);

    // Nothing queued, nothing written; a served compile queues nothing.
    EXPECT_NE(Plan(kWhere).find(kCaptured), std::string::npos);
    ASSERT_TRUE(Persist().ok());
    EXPECT_EQ(Rows().size(), 1u);
    EXPECT_EQ(table().persisted(), 1u);
}

TEST_F(PlanCatalogTest, AMountLoadsTheRowAndTheShapeIsNotSearchedAgain) {
    const exec::PlanRow row = CaptureAndPersist();
    // A new run: an empty table, and statistics that would now choose the
    // other order.
    CapturedPlans next_run;
    auto unreadable = exec::LoadCapturedPlans(boot_->catalog, store_, nullptr, next_run);
    ASSERT_TRUE(unreadable.ok()) << unreadable.status().message();
    EXPECT_EQ(unreadable.value(), 0u);
    EXPECT_EQ(next_run.loaded(), 1u);
    EXPECT_EQ(next_run.pending(), 0u) << "a loaded capture is not written again";
    EXPECT_EQ(next_run.Find(row.key), row.order);

    PublishModelsLarge();
    OptimizerSurface surface;
    surface.plan_stats = &stats_;
    surface.captured_plans = &next_run;
    dispatcher_->set_optimizer_surface(surface);
    const std::string plan = Plan(kWhere);
    EXPECT_NE(plan.find("reordered=1"), std::string::npos) << plan;
    EXPECT_NE(plan.find(kCaptured), std::string::npos) << plan;
    EXPECT_EQ(Rows().size(), 1u);
}

TEST_F(PlanCatalogTest, DropPlanRemovesTheRowAndTheShapeIsCapturedAgain) {
    const exec::PlanRow row = CaptureAndPersist();
    EXPECT_EQ(Run("DROP PLAN " + row.name + ";"),
              "DROPPED PLAN name=" + row.name + " plan_id=" + std::to_string(row.id));
    EXPECT_TRUE(Rows().empty());
    EXPECT_EQ(table().shapes(), 0u);
    // CX-Q17: re-armed, and captured from the statistics of this moment.
    PublishModelsLarge();
    const std::string again = Plan(kWhere);
    EXPECT_EQ(again.find("reordered=1"), std::string::npos) << again;
    EXPECT_NE(again.find(" capture=first"), std::string::npos) << again;
    ASSERT_TRUE(Persist().ok());
    const std::vector<exec::PlanRow> rows = Rows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_NE(rows.front().name, row.name) << "a new row under a new id";
    EXPECT_EQ(rows.front().order[0], 0u);
}

TEST_F(PlanCatalogTest, DropPlanRefusesWhatIsNotOneNameOfAPlanOutsideATransaction) {
    const exec::PlanRow row = CaptureAndPersist();
    const auto refused = [&](const std::string& sql, const std::string& code,
                             const std::string& byte) {
        const std::string out = Run(sql);
        EXPECT_EQ(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
        EXPECT_NE(out.find(code), std::string::npos) << out;
        EXPECT_NE(out.find("(byte " + byte + ")"), std::string::npos) << out;
    };
    refused("DROP PLAN", "takes a plan's name", "9");
    refused("DROP PLAN ;", "takes a plan's name", "11");
    refused("DROP PLAN 'x'", "takes a plan's name", "10");
    refused("DROP PLAN " + row.name + " now", "nothing after it",
            std::to_string(11 + row.name.size()));
    refused("DROP PLAN no_such_plan", "no plan named \"no_such_plan\"", "10");

    Session txn;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &txn).response.rfind("ERR", 0), std::string::npos);
    const std::string inside = dispatcher_->Dispatch("DROP PLAN " + row.name, &txn).response;
    EXPECT_EQ(inside.rfind("ERR", 0), 0u) << inside;
    EXPECT_NE(inside.find("autocommit"), std::string::npos) << inside;
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &txn).response.rfind("ERR", 0),
              std::string::npos);

    Session writer;
    writer.set_role(Role::kReadWrite);
    const std::string denied = dispatcher_->Dispatch("DROP PLAN " + row.name, &writer).response;
    EXPECT_EQ(denied.rfind("ERR", 0), 0u) << denied;
    EXPECT_NE(denied.find("admin"), std::string::npos) << denied;

    EXPECT_EQ(Rows().size(), 1u) << "no refusal removed it";
    EXPECT_EQ(table().shapes(), 1u);
}

TEST_F(PlanCatalogTest, AForgottenRelationsRowsAreRemovedByTheNextStep) {
    const exec::PlanRow row = CaptureAndPersist();
    // A second shape over another pair, which stays.
    Ok("CREATE TABLE fam (id int64, label int64) BTREE");
    Ok("INSERT INTO fam VALUES (1)");
    stats_.PublishForTest({{models_, stats::BandOf(8)},
                           {results_, stats::BandOf(2872)},
                           {OidOf("fam"), stats::BandOf(3)}},
                          {});
    ASSERT_NE(Plan("SELECT fam.label FROM fam JOIN models ON models.name = fam.id")
                  .find(" capture=first"),
              std::string::npos);
    ASSERT_TRUE(Persist().ok());
    ASSERT_EQ(Rows().size(), 2u);

    table().ForgetRelation(results_);  // what a committed `DROP TABLE results` does
    EXPECT_EQ(table().shapes(), 1u);
    ASSERT_TRUE(Persist().ok());
    const std::vector<exec::PlanRow> rows = Rows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_NE(rows.front().name, row.name);
    EXPECT_EQ(rows.front().key.oids[0], OidOf("fam"));
}

TEST_F(PlanCatalogTest, AMountRemovesARowNamingARelationThatIsGone) {
    // A crash between a drop's commit and the step that removes its rows
    // leaves one. No log runs under this dispatcher, so its `DROP TABLE`
    // forgets nothing - the row is as that crash leaves it.
    CaptureAndPersist();
    Ok("DROP TABLE results");
    ASSERT_EQ(Rows().size(), 1u);
    CapturedPlans next_run;
    auto unreadable = exec::LoadCapturedPlans(boot_->catalog, store_, nullptr, next_run);
    ASSERT_TRUE(unreadable.ok()) << unreadable.status().message();
    EXPECT_EQ(next_run.shapes(), 0u);
    EXPECT_TRUE(Rows().empty());
}

TEST_F(PlanCatalogTest, ARowThisBuildCannotReadIsNotLoadedAndIsShownAsItIs) {
    CaptureAndPersist();
    exec::DeclarationRow odd;
    auto id = boot_->catalog.AllocateRowId(catalog::kSysPlansTable);
    ASSERT_TRUE(id.ok());
    odd.id = id.value();
    odd.target_oid = models_;
    odd.flags = exec::kPlanFlagCaptured;
    odd.name = "captured_from_another_build";
    odd.source_text = "v9 something else\nSELECT 1";
    ASSERT_TRUE(exec::InsertDeclaration(boot_->catalog, store_, nullptr, catalog::kSysPlansTable,
                                        "plan", odd)
                    .ok());
    CapturedPlans next_run;
    auto unreadable = exec::LoadCapturedPlans(boot_->catalog, store_, nullptr, next_run);
    ASSERT_TRUE(unreadable.ok());
    EXPECT_EQ(unreadable.value(), 1u);
    EXPECT_EQ(next_run.shapes(), 1u) << "the readable row is loaded";
    EXPECT_EQ(Rows().size(), 2u) << "and the other stays where it is";

    const std::string view =
        Run("SELECT name, origin, state, statement FROM sys.plans WHERE state = 'unreadable'");
    EXPECT_NE(view.find("captured_from_another_build,captured,unreadable,"), std::string::npos)
        << view;
    EXPECT_NE(view.find("v9 something else"), std::string::npos) << view;
    // It can still be dropped by name.
    EXPECT_EQ(Run("DROP PLAN captured_from_another_build").rfind("DROPPED PLAN", 0), 0u);
}

TEST_F(PlanCatalogTest, SysPlansShowsEachRowWithItsRelationsInWrittenAndInRunOrder) {
    const exec::PlanRow row = CaptureAndPersist();
    const std::string view = Run(
        "SELECT plan_id, name, origin, state, fetch_id, relations, join_order FROM sys.plans");
    EXPECT_EQ(view.rfind("plan_id,name,origin,state,fetch_id,relations,join_order\\n", 0), 0u)
        << view;
    EXPECT_NE(view.find(std::to_string(row.id) + "," + row.name + ",captured,enabled," +
                        std::to_string(row.key.fetch_id)),
              std::string::npos)
        << view;
    EXPECT_NE(view.find("public.results, public.models"), std::string::npos) << view;
    EXPECT_NE(view.find("public.models, public.results"), std::string::npos) << view;
    // `SHOW PLANS` is the same rows (BO-R3).
    EXPECT_EQ(Run("SHOW PLANS"), Run("SELECT * FROM sys.plans"));
    const std::string meta = Run("SHOW META");
    EXPECT_NE(meta.find("join_order,captured_pending,0\\njoin_order,captured_persisted,1\\n"
                        "join_order,captured_loaded,0\\n"),
              std::string::npos)
        << meta;
}

// ---- A whole instance: a clean stop, a crash, a drop --------------------------

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() / ("kds_plans_" + std::to_string(::getpid()) + "_" +
                                            std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

constexpr const char* kJoin =
    "SELECT freights.id FROM freights JOIN cargos ON freights.cargo_id = cargos.id";

std::string Do(Expeditor& db, const std::string& sql) {
    return db.dispatcher().Dispatch(sql).response;
}

// Two relations, rows in each, and priors that make the instance informed.
void Populate(Expeditor& db) {
    for (const std::string& line :
         {std::string("CREATE TABLE cargos (id int64, org_id int64) BTREE"),
          std::string("CREATE TABLE freights (id int64, cargo_id int64) BTREE")}) {
        const std::string reply = Do(db, line);
        ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << line << " -> " << reply;
    }
    for (int i = 1; i <= 4; ++i) {
        ASSERT_EQ(Do(db, "INSERT INTO cargos VALUES (" + std::to_string(i) + ")")
                      .rfind("INSERTED", 0),
                  0u);
        ASSERT_EQ(Do(db, "INSERT INTO freights VALUES (" + std::to_string(i) + ")")
                      .rfind("INSERTED", 0),
                  0u);
    }
    auto cargos = db.catalog().FindTableOidByName(catalog::kNamespacePublic, "cargos");
    auto freights = db.catalog().FindTableOidByName(catalog::kNamespacePublic, "freights");
    ASSERT_TRUE(cargos.ok() && freights.ok());
    const std::vector<std::pair<catalog::Oid, std::int16_t>> bands = {
        {cargos.value(), stats::BandOf(8.0)}, {freights.value(), stats::BandOf(9000.0)}};
    db.plan_stats().SeedPriors(bands);
}

TEST(PlanMountTest, AServersDefaultCapturesAndAPersistedCaptureOutlivesACleanStopAndACrash) {
    TempDir live;
    TempDir crashed;
    {
        auto opened = reclaim_census::Mount(live.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        ASSERT_EQ(db.dispatcher().join_order(), exec::JoinOrder::kFixed);
        Populate(db);

        // CX-R8: a first compile under `fixed` inside a READ ONLY transaction
        // captures and appends no record.
        Session reader;
        ASSERT_EQ(db.dispatcher().Dispatch("BEGIN READ ONLY", &reader).response.rfind("ERR", 0),
                  std::string::npos);
        const wal::Lsn before = db.wal().appended_lsn();
        const std::string first =
            db.dispatcher().Dispatch(std::string("ANALYZE ") + kJoin, &reader).response;
        ASSERT_NE(first.find(" capture=first"), std::string::npos) << first;
        EXPECT_EQ(db.wal().appended_lsn(), before) << "the capture wrote the log";
        ASSERT_EQ(db.dispatcher().Dispatch("COMMIT", &reader).response.rfind("ERR", 0),
                  std::string::npos);
        EXPECT_EQ(db.dispatcher().captured_plans().pending(), 1u);

        // The step writes it; the log reaches the device and the pages do not.
        EXPECT_FALSE(db.PersistCapturedPlans());
        EXPECT_GT(db.wal().appended_lsn(), before);
        EXPECT_EQ(db.dispatcher().captured_plans().pending(), 0u);
        ASSERT_TRUE(db.wal().Flush().ok());
        reclaim_census::CrashImage(live.path, crashed.path);
        reclaim_census::CleanShutdownCheckpoint(db);
    }
    for (const fs::path& dir : {live.path, crashed.path}) {
        auto mounted = reclaim_census::Mount(dir);
        ASSERT_TRUE(mounted.ok()) << dir << ": " << mounted.status().message();
        Expeditor& db = *mounted.value();
        CurrentCoreGuard as(0);
        EXPECT_EQ(db.dispatcher().captured_plans().loaded(), 1u) << dir;
        // No prior was seeded in this run: a shape with no captured order
        // would be deferred. This one is served.
        const std::string plan = Do(db, std::string("ANALYZE ") + kJoin);
        EXPECT_NE(plan.find("planned order=captured\\n"), std::string::npos) << dir << ": " << plan;
        EXPECT_EQ(db.dispatcher().captured_plans().pending(), 0u) << dir;
        const std::string rows = Do(db, "SELECT origin, state FROM sys.plans");
        EXPECT_NE(rows.find("captured,enabled"), std::string::npos) << rows;
        reclaim_census::CleanShutdownCheckpoint(db);
    }
}

TEST(PlanMountTest, ACaptureACrashFindsStillQueuedIsLostAndMadeAgain) {
    TempDir live;
    TempDir crashed;
    {
        auto opened = reclaim_census::Mount(live.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        Populate(db);
        ASSERT_NE(Do(db, std::string("ANALYZE ") + kJoin).find(" capture=first"),
                  std::string::npos);
        ASSERT_TRUE(db.wal().Flush().ok());
        reclaim_census::CrashImage(live.path, crashed.path);  // before any step
        reclaim_census::CleanShutdownCheckpoint(db);
    }
    auto mounted = reclaim_census::Mount(crashed.path);
    ASSERT_TRUE(mounted.ok()) << mounted.status().message();
    Expeditor& db = *mounted.value();
    CurrentCoreGuard as(0);
    EXPECT_EQ(db.dispatcher().captured_plans().loaded(), 0u);
    // CX-Q14 (a): captured again, once the run is informed.
    EXPECT_NE(Do(db, std::string("ANALYZE ") + kJoin).find(" capture=deferred"),
              std::string::npos);
    reclaim_census::CleanShutdownCheckpoint(db);
}

TEST(PlanMountTest, ACommittedDropTableForgetsTheRelationsPlansAndTheStepRemovesTheirRows) {
    TempDir live;
    auto opened = reclaim_census::Mount(live.path);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    Populate(db);
    ASSERT_NE(Do(db, std::string("ANALYZE ") + kJoin).find(" capture=first"), std::string::npos);
    ASSERT_FALSE(db.PersistCapturedPlans());
    ASSERT_NE(Do(db, "SELECT name FROM sys.plans").find("captured_"), std::string::npos);

    // A drop that rolls back forgets nothing.
    Session txn;
    ASSERT_EQ(db.dispatcher().Dispatch("BEGIN", &txn).response.rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(db.dispatcher().Dispatch("DROP TABLE freights", &txn).response.rfind("DROPPED", 0),
              0u);
    ASSERT_EQ(db.dispatcher().Dispatch("ROLLBACK", &txn).response.rfind("ERR", 0),
              std::string::npos);
    EXPECT_EQ(db.dispatcher().captured_plans().shapes(), 1u);
    EXPECT_NE(Do(db, std::string("ANALYZE ") + kJoin).find("planned order=captured\\n"),
              std::string::npos);

    // One that commits does (CX-Q24).
    ASSERT_EQ(Do(db, "DROP TABLE freights").rfind("DROPPED", 0), 0u);
    EXPECT_EQ(db.dispatcher().captured_plans().shapes(), 0u);
    ASSERT_FALSE(db.PersistCapturedPlans());
    EXPECT_EQ(Do(db, "SELECT name FROM sys.plans").find("captured_"), std::string::npos);
    reclaim_census::CleanShutdownCheckpoint(db);
}

}  // namespace
}  // namespace kds::server
