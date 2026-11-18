#include <array>
#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/captured_plans.hpp"
#include "kds/exec/join_order.hpp"
#include "kds/exec/step_compiler.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/plan_stats.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"

// `join_order = fixed` (CX-S0, instructions/v3.2.0/workorder-cx-managed-plans.md):
// a shape's first priced search is captured, and every later compile of the
// shape runs the captured order whatever the statistics do after. Pinned on
// the compile's chain and on what a client sees - ANALYZE's plan line,
// `SET JOIN_ORDER`'s reply, `SHOW META`'s block.

namespace kds::server {
namespace {

using exec::CapturedKey;
using exec::CapturedOrder;
using exec::CapturedPlans;
using exec::PlanSource;

class CapturedPlansTest : public ::testing::Test {
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
    }
    void TearDown() override { dispatcher_->set_plan_stats(nullptr); }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    void Ok(const std::string& sql) {
        const std::string out = Run(sql);
        ASSERT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    }
    std::string Plan(const std::string& sql) { return Run("ANALYZE " + sql); }
    // The reply's row count: the lines after the header.
    std::size_t RowCount(const std::string& sql) {
        const std::string out = Run(sql);
        EXPECT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
        std::size_t rows = 0;
        for (std::size_t at = out.find("\\n"); at != std::string::npos;
             at = out.find("\\n", at + 2)) {
            ++rows;
        }
        return rows;
    }

    // Scenario 1's compare-all, as the planner's own suite fills it: 8
    // models, 64 results, model_id round robin.
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
    // Priors under which the search puts models first (order 1, 0), and
    // their reverse, under which it keeps the written order (0, 1).
    void PublishModelsSmall() {
        stats_.PublishForTest({{models_, stats::BandOf(8)}, {results_, stats::BandOf(2872)}}, {});
    }
    void PublishModelsLarge() {
        stats_.PublishForTest({{models_, stats::BandOf(2872)}, {results_, stats::BandOf(8)}}, {});
    }

    // A compile under `fixed` against `table`, or under `planned` with none.
    exec::StepChain Compile(const std::string& sql, CapturedPlans* table, bool keyed = true) {
        parser::Parser parser(sql);
        EXPECT_TRUE(parser.Parse().ok());
        auto parsed = parser::Parse(sql);
        EXPECT_TRUE(parsed.ok()) << parsed.status().message();
        exec::PlanOptions options;
        options.statistics = &source_;
        options.captured = table;
        if (keyed && parser.fingerprint().has_value()) {
            options.fetch_id = parser.fingerprint()->fetch_id;
        }
        auto chain = exec::Compile(boot_->catalog, std::get<parser::SelectStmt>(parsed.value()),
                                   nullptr, nullptr, nullptr, &options);
        EXPECT_TRUE(chain.ok()) << sql << ": " << chain.status().message();
        return chain.ok() ? std::move(chain.value()) : exec::StepChain{};
    }
    exec::StepChain Fixed(const std::string& sql) { return Compile(sql, &table_); }
    exec::StepChain Planned(const std::string& sql) { return Compile(sql, nullptr); }

    static std::vector<std::uint32_t> Order(const exec::StepChain& chain) {
        std::vector<std::uint32_t> ids;
        for (const exec::Step& step : chain.steps) ids.push_back(step.step_id);
        return ids;
    }
    static PlanSource SourceOf(const exec::StepChain& chain) {
        EXPECT_TRUE(chain.plan_note.has_value());
        return chain.plan_note.has_value() ? chain.plan_note->source : PlanSource::kSearched;
    }

    static constexpr const char* kCompareAll =
        "SELECT results.pnl, models.name FROM results JOIN models ON results.model_id = models.id";
    const std::vector<std::uint32_t> kModelsFirst{1, 0};
    const std::vector<std::uint32_t> kWritten{0, 1};

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    stats::PlanStats stats_{1};
    stats::PlanStatsSource source_{&stats_};
    std::optional<CommandDispatcher> dispatcher_;
    CapturedPlans table_;
    catalog::Oid models_ = 0;
    catalog::Oid results_ = 0;
};

// ---- The capture -------------------------------------------------------------

TEST_F(CapturedPlansTest, TheFirstPricedSearchIsCapturedAndTheStatisticsNoLongerMoveIt) {
    FillModels();
    PublishModelsSmall();
    const exec::StepChain first = Fixed(kCompareAll);
    EXPECT_EQ(Order(first), kModelsFirst);
    EXPECT_EQ(SourceOf(first), PlanSource::kCapturing);
    EXPECT_EQ(table_.shapes(), 1u);

    // The statistics turn round: `planned` follows them, `fixed` does not.
    PublishModelsLarge();
    ASSERT_EQ(Order(Planned(kCompareAll)), kWritten) << "the premise: the search's answer moved";
    const exec::StepChain later = Fixed(kCompareAll);
    EXPECT_EQ(Order(later), kModelsFirst);
    EXPECT_EQ(SourceOf(later), PlanSource::kCaptured);
    // It read no statistic: no estimate, no generation.
    EXPECT_TRUE(later.plan_note->estimated_rows.empty());
    EXPECT_EQ(later.plan_note->generation, 0u);
    // Everything but the order is the compile's again: the captured order's
    // chain is the chain the search compiled for it.
    ASSERT_EQ(later.steps.size(), first.steps.size());
    for (std::size_t k = 0; k < later.steps.size(); ++k) {
        EXPECT_EQ(later.steps[k].kind, first.steps[k].kind) << k;
        EXPECT_EQ(later.steps[k].build.has_value(), first.steps[k].build.has_value()) << k;
    }
    EXPECT_EQ(table_.shapes(), 1u);
    EXPECT_EQ(table_.served(), 1u);
}

TEST_F(CapturedPlansTest, ACapturedWrittenOrderIsKeptToo) {
    // The capture is of the search's answer, whichever order that is.
    FillModels();
    PublishModelsLarge();
    EXPECT_EQ(Order(Fixed(kCompareAll)), kWritten);
    EXPECT_EQ(table_.shapes(), 1u);
    PublishModelsSmall();
    ASSERT_EQ(Order(Planned(kCompareAll)), kModelsFirst);
    const exec::StepChain later = Fixed(kCompareAll);
    EXPECT_EQ(Order(later), kWritten);
    EXPECT_EQ(SourceOf(later), PlanSource::kCaptured);
}

TEST_F(CapturedPlansTest, ACaptureWaitsUntilEveryRelationHasAPublishedPrior) {
    // CX-Q19: an order priced blind, or half blind, is not fixed. The block
    // is searched and its answer runs, uncaptured.
    FillModels();
    EXPECT_EQ(SourceOf(Fixed(kCompareAll)), PlanSource::kDeferred);
    stats_.PublishForTest({{models_, stats::BandOf(8)}}, {});
    const exec::StepChain half = Fixed(kCompareAll);
    EXPECT_EQ(SourceOf(half), PlanSource::kDeferred);
    EXPECT_FALSE(half.plan_note->estimated_rows.empty()) << "a deferred block is priced";
    EXPECT_EQ(table_.shapes(), 0u);
    EXPECT_EQ(table_.deferred(), 2u);
    PublishModelsSmall();
    EXPECT_EQ(SourceOf(Fixed(kCompareAll)), PlanSource::kCapturing);
    EXPECT_EQ(table_.shapes(), 1u);
}

TEST_F(CapturedPlansTest, OneCaptureServesEveryBindingOfTheShape) {
    // CX-Q13: the key holds no literal, so the order the first binding's
    // search chose runs for every other.
    FillModels();
    PublishModelsSmall();
    const std::string head =
        "SELECT results.pnl FROM results JOIN models ON results.model_id = models.id "
        "WHERE results.pnl > ";
    const exec::StepChain first = Fixed(head + "5");
    ASSERT_EQ(SourceOf(first), PlanSource::kCapturing);
    const exec::StepChain other = Fixed(head + "60");
    EXPECT_EQ(SourceOf(other), PlanSource::kCaptured);
    EXPECT_EQ(Order(other), Order(first));
    EXPECT_EQ(table_.shapes(), 1u);
    // Another shape over the same relations is another key.
    EXPECT_EQ(SourceOf(Fixed(kCompareAll)), PlanSource::kCapturing);
    EXPECT_EQ(table_.shapes(), 2u);
}

TEST_F(CapturedPlansTest, ARecreatedRelationNeverRunsItsOldCapture) {
    // CX-R3: the key holds every oid, and an oid is never reissued. A fetch
    // id hashes names, so only the oids make this miss.
    FillModels();
    PublishModelsSmall();
    ASSERT_EQ(SourceOf(Fixed(kCompareAll)), PlanSource::kCapturing);
    Ok("DROP TABLE results");
    Ok("CREATE TABLE results (id int64, model_id int64, pnl int64) BTREE");
    const exec::StepChain again = Fixed(kCompareAll);
    EXPECT_EQ(SourceOf(again), PlanSource::kDeferred) << "the new relation has no prior";
    EXPECT_EQ(table_.served(), 0u);
    stats_.PublishForTest({{models_, stats::BandOf(8)}, {OidOf("results"), stats::BandOf(2872)}},
                          {});
    EXPECT_EQ(SourceOf(Fixed(kCompareAll)), PlanSource::kCapturing);
    EXPECT_EQ(table_.shapes(), 2u);
}

TEST_F(CapturedPlansTest, ATupleKeyChainNeverReachesTheTable) {
    // CX-R6: a lookup and its probes leave the compile before the table is
    // asked, as they leave before the statistics.
    FillModels();
    PublishModelsSmall();
    const exec::StepChain chain = Fixed(std::string(kCompareAll) + " WHERE results.id = 5");
    EXPECT_FALSE(chain.plan_note.has_value());
    EXPECT_EQ(table_.shapes() + table_.served() + table_.deferred() + table_.unkeyed(), 0u);
}

TEST_F(CapturedPlansTest, ABlockNoKeyFitsRunsAsWritten) {
    // CX-Q20: no fetch id, no key - and no search, which under `fixed`
    // would be a plan that moves with the statistics.
    FillModels();
    PublishModelsSmall();
    ASSERT_EQ(Order(Planned(kCompareAll)), kModelsFirst);
    const exec::StepChain chain = Compile(kCompareAll, &table_, /*keyed=*/false);
    EXPECT_EQ(Order(chain), kWritten);
    EXPECT_EQ(SourceOf(chain), PlanSource::kUnkeyed);
    EXPECT_EQ(table_.unkeyed(), 1u);
    EXPECT_EQ(table_.shapes(), 0u);
}

TEST_F(CapturedPlansTest, PastTheCeilingANewShapeRunsAsWrittenAndAHeldOneStillRuns) {
    // CX-Q21: nothing is evicted, so a held shape keeps its order.
    FillModels();
    PublishModelsSmall();
    ASSERT_EQ(SourceOf(Fixed(kCompareAll)), PlanSource::kCapturing);
    CapturedKey filler;
    filler.n = 2;
    for (std::uint64_t k = 1; table_.shapes() < CapturedPlans::kMaxShapes; ++k) {
        filler.fetch_id = k;
        ASSERT_TRUE(table_.Capture(filler, CapturedOrder{}).has_value());
    }
    ASSERT_TRUE(table_.full());
    filler.fetch_id = 0;
    EXPECT_FALSE(table_.Capture(filler, CapturedOrder{}).has_value());
    EXPECT_EQ(table_.shapes(), CapturedPlans::kMaxShapes);

    const exec::StepChain fresh = Fixed(std::string(kCompareAll) + " WHERE results.pnl > 5");
    EXPECT_EQ(Order(fresh), kWritten);
    EXPECT_EQ(SourceOf(fresh), PlanSource::kOverCeiling);
    EXPECT_EQ(table_.refused(), 1u);
    const exec::StepChain held = Fixed(kCompareAll);
    EXPECT_EQ(SourceOf(held), PlanSource::kCaptured);
    EXPECT_EQ(Order(held), kModelsFirst);
}

// ---- The table ---------------------------------------------------------------

TEST(CapturedPlansTableTest, TheFirstInstallWinsAndIsWhatEveryCallerIsAnswered) {
    CapturedPlans table;
    CapturedKey key;
    key.fetch_id = 7;
    key.n = 2;
    key.oids = {200, 201};
    EXPECT_FALSE(table.Find(key).has_value());
    const CapturedOrder first{1, 0};
    const CapturedOrder second{0, 1};
    EXPECT_EQ(table.Capture(key, first), first);
    EXPECT_EQ(table.Capture(key, second), first);
    EXPECT_EQ(table.Find(key), first);
    EXPECT_EQ(table.shapes(), 1u);
    // Exact matching: another oid, another count, another fetch id.
    CapturedKey other = key;
    other.oids[1] = 202;
    EXPECT_FALSE(table.Find(other).has_value());
    other = key;
    other.n = 3;
    EXPECT_FALSE(table.Find(other).has_value());
    other = key;
    other.fetch_id = 8;
    EXPECT_FALSE(table.Find(other).has_value());
}

TEST(CapturedPlansTableTest, CoresRacingToCaptureOneShapeAllRunOneOrder) {
    // CX-R7: no shape runs two captured plans.
    CapturedPlans table;
    constexpr std::size_t kThreads = 8;
    constexpr std::uint64_t kShapes = 256;
    std::array<std::array<CapturedOrder, kShapes>, kThreads> answered{};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (std::uint64_t shape = 0; shape < kShapes; ++shape) {
                CapturedKey key;
                key.fetch_id = shape + 1;
                key.n = 2;
                key.oids = {200, 201};
                CapturedOrder mine{};
                mine[0] = static_cast<std::uint8_t>(t);
                const auto in_force = table.Capture(key, mine);
                answered[t][shape] = in_force.value_or(CapturedOrder{0xff});
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (std::thread& thread : threads) thread.join();
    EXPECT_EQ(table.shapes(), kShapes);
    for (std::uint64_t shape = 0; shape < kShapes; ++shape) {
        for (std::size_t t = 1; t < kThreads; ++t) {
            EXPECT_EQ(answered[t][shape], answered[0][shape]) << "shape " << shape;
        }
    }
}

// ---- Through the dispatcher --------------------------------------------------

TEST_F(CapturedPlansTest, ADispatcherThatIsNeverToldRunsPlanned) {
    EXPECT_EQ(dispatcher_->join_order(), exec::JoinOrder::kPlanned);
    EXPECT_NE(Run("SHOW META").find("join_order,join_order,planned\\njoin_order,captured_plans,0\\n"),
              std::string::npos);
}

TEST_F(CapturedPlansTest, SetJoinOrderSwitchesAtRunTimeAndKeepsTheCapturedPlans) {
    // CX-R10, CX-Q18.
    FillModels();
    PublishModelsSmall();
    EXPECT_EQ(Run("SET JOIN_ORDER fixed"), "OK join_order=fixed");
    const std::string first = Plan(kCompareAll);
    EXPECT_NE(first.find("reordered=1"), std::string::npos) << first;
    EXPECT_NE(first.find("planned order=planned gen="), std::string::npos) << first;
    EXPECT_NE(first.find(" capture=first"), std::string::npos) << first;
    const std::string second = Plan(kCompareAll);
    EXPECT_NE(second.find("reordered=1"), std::string::npos) << second;
    EXPECT_NE(second.find("planned order=captured\\n"), std::string::npos) << second;
    EXPECT_EQ(RowCount(kCompareAll), 64u);

    // The statistics turn round. `fixed` keeps the order.
    PublishModelsLarge();
    EXPECT_NE(Plan(kCompareAll).find("planned order=captured\\n"), std::string::npos);
    // `planned` follows them and leaves the captured plan where it is.
    EXPECT_EQ(Run("SET JOIN_ORDER = PLANNED"), "OK join_order=planned");
    const std::string planned = Plan(kCompareAll);
    EXPECT_EQ(planned.find("reordered=1"), std::string::npos) << planned;
    EXPECT_NE(planned.find("planned order=written gen="), std::string::npos) << planned;
    EXPECT_EQ(planned.find("capture="), std::string::npos) << planned;
    EXPECT_EQ(dispatcher_->captured_plans().shapes(), 1u);
    // `written` plans nothing.
    EXPECT_EQ(Run("SET JOIN_ORDER written"), "OK join_order=written");
    EXPECT_EQ(Plan(kCompareAll).find("planned order="), std::string::npos);
    // Back to `fixed`: the captured order again, not a new capture.
    EXPECT_EQ(Run("SET JOIN_ORDER fixed"), "OK join_order=fixed");
    const std::string back = Plan(kCompareAll);
    EXPECT_NE(back.find("reordered=1"), std::string::npos) << back;
    EXPECT_NE(back.find("planned order=captured\\n"), std::string::npos) << back;
    EXPECT_EQ(RowCount(kCompareAll), 64u);

    const std::string meta = Run("SHOW META");
    EXPECT_NE(meta.find("join_order,join_order,fixed\\njoin_order,captured_plans,1\\n"), std::string::npos)
        << meta;
}

TEST_F(CapturedPlansTest, ADeferredShapeIsCapturedOnceTheStewardSeedsItsRelations) {
    // The whole path with nothing published by hand: the deferred run names
    // its relations to the steward, whose steps seed and publish their
    // priors, and the next compile captures.
    FillModels();
    Ok("SET JOIN_ORDER fixed");
    const std::string blind = Plan(kCompareAll);
    EXPECT_NE(blind.find(" capture=deferred"), std::string::npos) << blind;
    EXPECT_EQ(dispatcher_->captured_plans().shapes(), 0u);
    for (std::uint64_t second : {1u, 2u}) {
        ASSERT_TRUE(stats_.StewardStep(boot_->catalog, store_, second * 1'000'000'000).ok());
    }
    const std::string informed = Plan(kCompareAll);
    EXPECT_NE(informed.find(" capture=first"), std::string::npos) << informed;
    EXPECT_NE(Plan(kCompareAll).find("planned order=captured\\n"), std::string::npos);
    EXPECT_EQ(dispatcher_->captured_plans().shapes(), 1u);
    EXPECT_EQ(dispatcher_->captured_plans().deferred(), 1u);
}

TEST_F(CapturedPlansTest, UnderAMintedViewNoCaptureIsMadeAndNoneIsRead) {
    // CX-R7: while a DDL is open the catalog a compile prices against may
    // roll back, so its order is not fixed - and a captured one is not read.
    FillModels();
    PublishModelsSmall();
    Ok("SET JOIN_ORDER fixed");
    Session ddl;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &ddl).response.rfind("ERR", 0), std::string::npos);
    const std::string created =
        dispatcher_->Dispatch("CREATE TABLE side (id int64) BTREE", &ddl).response;
    ASSERT_EQ(created.rfind("CREATED", 0), 0u) << created;

    const std::string open = Plan(kCompareAll);
    EXPECT_NE(open.find("planned order=planned gen="), std::string::npos) << open;
    EXPECT_EQ(open.find("capture="), std::string::npos) << open;
    EXPECT_EQ(dispatcher_->captured_plans().shapes(), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &ddl).response.rfind("ERR", 0), std::string::npos);

    EXPECT_NE(Plan(kCompareAll).find(" capture=first"), std::string::npos);
    // And with one held: a DDL open again, the search runs and it is not read.
    PublishModelsLarge();
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &ddl).response.rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(dispatcher_->Dispatch("CREATE TABLE side (id int64) BTREE", &ddl)
                  .response.rfind("CREATED", 0),
              0u);
    const std::string searched = Plan(kCompareAll);
    EXPECT_NE(searched.find("planned order=written gen="), std::string::npos) << searched;
    EXPECT_EQ(dispatcher_->captured_plans().served(), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &ddl).response.rfind("ERR", 0), std::string::npos);
    EXPECT_NE(Plan(kCompareAll).find("planned order=captured\\n"), std::string::npos);
}

TEST_F(CapturedPlansTest, AnInstancesValueAndTableAreTheOnesEveryDispatcherUses) {
    // CX-R10, CX-R7: handed an `OptimizerSurface`, a dispatcher's `SET` flips
    // the instance's value and its captures land in the instance's table.
    FillModels();
    PublishModelsSmall();
    std::atomic<exec::JoinOrder> instance_value{exec::JoinOrder::kFixed};
    OptimizerSurface surface;
    surface.plan_stats = &stats_;
    surface.join_order = &instance_value;
    surface.captured_plans = &table_;
    dispatcher_->set_optimizer_surface(surface);
    EXPECT_EQ(dispatcher_->join_order(), exec::JoinOrder::kFixed);
    EXPECT_NE(Plan(kCompareAll).find(" capture=first"), std::string::npos);
    EXPECT_EQ(table_.shapes(), 1u);
    // What another core captured is what this one runs.
    EXPECT_EQ(SourceOf(Fixed(kCompareAll)), PlanSource::kCaptured);
    Ok("SET JOIN_ORDER written");
    EXPECT_EQ(instance_value.load(), exec::JoinOrder::kWritten);
}

TEST_F(CapturedPlansTest, SetJoinOrderRefusesAnythingButOneOfItsThreeValues) {
    const auto refused = [&](const std::string& sql, const std::string& byte) {
        const std::string out = Run(sql);
        EXPECT_EQ(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
        EXPECT_NE(out.find("planned, fixed or written"), std::string::npos) << out;
        EXPECT_NE(out.find("(byte " + byte + ")"), std::string::npos) << out;
    };
    refused("SET JOIN_ORDER", "14");
    refused("SET JOIN_ORDER cost", "15");
    refused("SET JOIN_ORDER fixed now", "21");
    refused("SET JOIN_ORDER =", "16");
    EXPECT_EQ(dispatcher_->join_order(), exec::JoinOrder::kPlanned) << "a refusal sets nothing";
}

TEST_F(CapturedPlansTest, SetJoinOrderIsTheAdmins) {
    Session writer;
    writer.set_role(Role::kReadWrite);
    const std::string out = dispatcher_->Dispatch("SET JOIN_ORDER written", &writer).response;
    EXPECT_EQ(out.rfind("ERR", 0), 0u) << out;
    EXPECT_NE(out.find("admin"), std::string::npos) << out;
    EXPECT_EQ(dispatcher_->join_order(), exec::JoinOrder::kPlanned);
}

}  // namespace
}  // namespace kds::server
