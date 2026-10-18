#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/plan_stats.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// The join planner's statistics (docs/spec/plan-stats.md, CC-S2): what a
// planned run folds, what the steward seeds and publishes, and what it must
// not move. Driven through a dispatcher, as a client's SELECT reaches it.

namespace kds::server {
namespace {

constexpr std::uint64_t kSecond = 1'000'000'000;

class PlanStatsTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 4000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
        dispatcher_->set_plan_stats(&stats_);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    void Ok(const std::string& sql) {
        const std::string out = Run(sql);
        ASSERT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    }

    // One steward step at `now`, which must succeed.
    void Steward(std::uint64_t now) {
        ASSERT_TRUE(stats_.StewardStep(boot_->catalog, store_, now).ok());
    }

    catalog::Oid OidOf(const std::string& name) {
        auto oid = boot_->catalog.ResolveRelation("", name, catalog::kNamespacePublic, 0, nullptr);
        EXPECT_TRUE(oid.ok()) << name;
        return oid.ok() ? oid.value() : 0;
    }

    // u(id, code): ids 1..4. l(id, uid, v): two rows per uid in 1..4.
    void Fill() {
        Ok("CREATE TABLE u (id int64, code int64) BTREE");
        Ok("CREATE TABLE l (id int64, uid int64, v int64) BTREE");
        for (int i = 1; i <= 4; ++i) Ok("INSERT INTO u VALUES (" + std::to_string(100 + i) + ")");
        for (int uid = 1; uid <= 4; ++uid) {
            for (int row = 0; row < 2; ++row) {
                Ok("INSERT INTO l VALUES (" + std::to_string(uid) + ", " +
                   std::to_string(10 * uid + row) + ")");
            }
        }
    }

    // The published posterior for `sql`'s block, keyed as the planner keys it.
    const stats::PlanStatsSnapshot::Posterior* Posterior(
        const std::shared_ptr<const stats::PlanStatsSnapshot>& snapshot,
        const std::vector<std::string>& relations, std::uint64_t fetch_id) {
        stats::PlanKey key;
        key.fetch_id = fetch_id;
        key.n = static_cast<std::uint8_t>(relations.size());
        for (std::size_t w = 0; w < relations.size(); ++w) key.oids[w] = OidOf(relations[w]);
        return snapshot->Find(key);
    }

    std::uint64_t FetchIdOf(const std::string& sql) {
        parser::Parser parser(sql);
        EXPECT_TRUE(parser.Parse().ok());
        const auto fingerprint = parser.fingerprint();
        EXPECT_TRUE(fingerprint.has_value());
        return fingerprint.has_value() ? fingerprint->fetch_id : 0;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
    stats::PlanStats stats_{1};
};

TEST(PlanStatsBandTest, BandsAreHalfOctavesAndTheirCentresInvert) {
    EXPECT_EQ(stats::BandOf(1.0), 0);
    EXPECT_EQ(stats::BandOf(0.2), 0) << "a clamp, never a negative band";
    EXPECT_EQ(stats::BandOf(4.0), 4);
    EXPECT_EQ(stats::BandOf(1000.0), 20);
    EXPECT_DOUBLE_EQ(stats::BandCentre(4), 4.0);
    EXPECT_NEAR(stats::BandCentre(stats::BandOf(1000.0)), 1024.0, 1e-9);
}

TEST_F(PlanStatsTest, APlannedRunFoldsItsSubsetCardinalitiesByWrittenPosition) {
    Fill();
    const std::string sql = "SELECT u.code FROM u JOIN l ON u.id = l.uid WHERE l.v = 31";
    Ok(sql);
    Steward(kSecond);
    const auto snapshot = stats_.Current();
    ASSERT_NE(snapshot, nullptr);
    const auto* posterior = Posterior(snapshot, {"u", "l"}, FetchIdOf(sql));
    ASSERT_NE(posterior, nullptr) << "a planned run folds under its key";
    // l (written position 1) drives: card({l}) = 1 row with v = 31; then
    // card({u, l}) = 1.
    EXPECT_NE(posterior->present & (1u << 0b10), 0);
    EXPECT_EQ(stats::BandCentre(posterior->band[0b10]), 1.0);
    EXPECT_NE(posterior->present & (1u << 0b11), 0);
    EXPECT_EQ(posterior->present & (1u << 0b01), 0) << "u alone never ran first";
}

TEST_F(PlanStatsTest, AStoppedRunAndASingleRelationFoldNothing) {
    Fill();
    Ok("SELECT u.code FROM u JOIN l ON u.id = l.uid LIMIT 1");
    Ok("SELECT l.v FROM l WHERE l.v = 31");
    Steward(kSecond);
    const auto snapshot = stats_.Current();
    // Priors may be published for wanted relations; no posterior is.
    EXPECT_TRUE(snapshot == nullptr || snapshot->posteriors() == 0);
}

TEST_F(PlanStatsTest, ThePriorIsExactAtHeightOneAndCloseAtHeightTwo) {
    Ok("CREATE TABLE small (id int64, v int64) BTREE");
    for (int i = 0; i < 7; ++i) Ok("INSERT INTO small VALUES (" + std::to_string(i) + ")");
    Ok("CREATE TABLE big (id int64, v int64) BTREE");
    // Single rows: this dispatcher has no transaction manager for a
    // multi-row INSERT. 3,000 rows of 16 bytes fill a few dozen leaves.
    for (int i = 0; i < 3000; ++i) Ok("INSERT INTO big VALUES (" + std::to_string(i) + ")");
    for (const auto& [name, rows] : {std::pair{"small", 7.0}, std::pair{"big", 3000.0}}) {
        auto access = boot_->catalog.InitTableAccess(OidOf(name));
        ASSERT_TRUE(access.ok());
        std::size_t budget = stats::PlanStats::kSeedPageBudget;
        auto prior = stats::ReadRelationPrior(store_, *access.value(), budget);
        ASSERT_TRUE(prior.ok()) << prior.status().message();
        if (rows < 10) {
            EXPECT_EQ(prior.value(), rows) << name;
        } else {
            EXPECT_GT(prior.value(), rows / 2) << name;
            EXPECT_LT(prior.value(), rows * 2) << name;
        }
    }
}

TEST_F(PlanStatsTest, APageAnotherRelationOwnsDiscardsTheSeed) {
    Ok("CREATE TABLE a (id int64, v int64) BTREE");
    Ok("INSERT INTO a VALUES (1)");
    auto access = boot_->catalog.InitTableAccess(OidOf("a"));
    ASSERT_TRUE(access.ok());
    catalog::TableAccess other = *access.value();
    other.oid += 1000;  // the same pages, claimed by another oid
    std::size_t budget = stats::PlanStats::kSeedPageBudget;
    EXPECT_FALSE(stats::ReadRelationPrior(store_, other, budget).ok());
}

TEST_F(PlanStatsTest, TheStewardSeedsWantedRelationsAndRepublishesNothingUnchanged) {
    Fill();
    const std::string sql = "SELECT u.code FROM u JOIN l ON u.id = l.uid WHERE l.v = 31";
    Ok(sql);
    Steward(kSecond);
    const auto first = stats_.Current();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->RelationRows(OidOf("u")), 4.0) << "a height-1 prior, exact";
    EXPECT_EQ(first->RelationRows(OidOf("l")), 8.0);

    // The same facts again: nothing moved, nothing is published.
    const std::uint64_t generation = stats_.generation();
    Ok(sql);
    Steward(2 * kSecond);
    EXPECT_EQ(stats_.generation(), generation);
}

TEST(PlanStatsStoreTest, ADeclinePublishesAtOnceAndBandsHoldUnderChurn) {
    // PS3, PS5 on the store alone: a decline fact is published by the next
    // step; a value moving 25 % either way within its band publishes
    // nothing; one moving past the Schmitt margin does.
    storage::InMemoryPageStore store{kFirstUserPageId};
    auto boot = bootstrap::BootstrapDatabase(store, 1000);
    ASSERT_TRUE(boot.ok());
    stats::PlanStats plan(2);
    stats::PlanObservation seen;
    seen.key.fetch_id = 7;
    seen.key.n = 2;
    seen.key.oids = {1001, 1002, 0, 0};
    seen.rows[0b01] = 1000;
    seen.present = 1u << 0b01;
    const auto step = [&](std::uint64_t at) {
        ASSERT_TRUE(plan.StewardStep(boot.value().catalog, store, at * kSecond).ok());
    };
    plan.Fold(1, seen);  // core 1's part, merged by the steward
    step(1);
    const std::uint64_t first = plan.generation();
    ASSERT_GT(first, 0u);
    for (const std::uint64_t rows : {1200u, 800u, 1100u}) {
        seen.rows[0b01] = rows;
        plan.Fold(0, seen);
        step(2);
    }
    EXPECT_EQ(plan.generation(), first) << "churn inside the band published nothing";
    seen.declines = 0b10;
    plan.Fold(0, seen);
    step(3);
    EXPECT_GT(plan.generation(), first) << "a decline publishes at once";
    const auto* posterior = plan.Current()->Find(seen.key);
    ASSERT_NE(posterior, nullptr);
    EXPECT_EQ(posterior->declines, 0b10);
}

TEST_F(PlanStatsTest, CountsAreTheSameWhateverServesTheStep) {
    // PS10: an index changes how a step finds its rows, never how many.
    // Each configuration folds into a store of its own, so the first
    // observation is the value, with no average to hide a difference; 40
    // rows of l carry v = 31, a band no 1-row count shares.
    Ok("CREATE TABLE u (id int64, code int64) BTREE");
    Ok("CREATE TABLE l (id int64, uid int64, v int64) BTREE");
    for (int i = 1; i <= 4; ++i) Ok("INSERT INTO u VALUES (" + std::to_string(100 + i) + ")");
    for (int row = 0; row < 40; ++row) {
        Ok("INSERT INTO l VALUES (" + std::to_string(1 + row % 4) + ", 31)");
    }
    const std::string sql = "SELECT u.code FROM u JOIN l ON u.id = l.uid WHERE l.v = 31";
    const auto fold = [&](stats::PlanStats& into) {
        dispatcher_->set_plan_stats(&into);
        Ok(sql);
        ASSERT_TRUE(into.StewardStep(boot_->catalog, store_, kSecond).ok());
        dispatcher_->set_plan_stats(&stats_);
    };
    stats::PlanStats walked{1};
    stats::PlanStats indexed{1};
    fold(walked);
    Ok("CREATE INDEX l_v ON l (v)");
    ASSERT_NE(Run("ANALYZE " + sql).find("Index"), std::string::npos) << "the index serves l";
    fold(indexed);
    const auto* a = Posterior(walked.Current(), {"u", "l"}, FetchIdOf(sql));
    const auto* b = Posterior(indexed.Current(), {"u", "l"}, FetchIdOf(sql));
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    for (const unsigned mask : {0b10u, 0b11u}) {
        ASSERT_NE(a->present & (1u << mask), 0) << mask;
        EXPECT_EQ(a->band[mask], b->band[mask]) << mask;
        EXPECT_EQ(stats::BandCentre(a->band[mask]), stats::BandCentre(stats::BandOf(40.0)));
    }
}

TEST_F(PlanStatsTest, ASelfJoinsMasksNameWrittenPositions) {
    // PS11: the two bindings of one relation are two written positions, so
    // the driver's mask names the binding that drives, whichever it is.
    Fill();
    const std::string sql =
        "SELECT a.v FROM l AS a JOIN l AS b ON a.uid = b.id WHERE b.v = 31";
    const std::string plan = Run("ANALYZE " + sql);
    Ok(sql);
    Steward(kSecond);
    const auto* posterior = Posterior(stats_.Current(), {"l", "l"}, FetchIdOf(sql));
    ASSERT_NE(posterior, nullptr) << plan;
    const bool b_drives = plan.find("reordered=1") != std::string::npos;
    // The driver alone is mask {b} (0b10) when b drives, {a} (0b01) when a does.
    EXPECT_NE(posterior->present & (1u << (b_drives ? 0b10u : 0b01u)), 0) << plan;
    EXPECT_EQ(posterior->present & (1u << (b_drives ? 0b01u : 0b10u)), 0) << plan;
}

TEST(PlanStatsBandTest, TheSchmittStepHoldsInsideItsMarginAndMovesPastIt) {
    // PS5: a band moves once its value lies 0.75 of a band from the
    // published band's centre - about 30 % from the centre; a value inside
    // that margin, even past the band's own edge, leaves it.
    std::int16_t band = 0;
    bool banded = false;
    EXPECT_TRUE(stats::PlanStats::Reband(band, banded, 1000.0));
    EXPECT_EQ(band, stats::BandOf(1000.0));  // 20: centre 1024
    EXPECT_FALSE(stats::PlanStats::Reband(band, banded, 1024.0 * 1.25));  // +0.64 band
    EXPECT_FALSE(stats::PlanStats::Reband(band, banded, 1024.0 / 1.25));  // -0.64 band
    EXPECT_EQ(band, 20);
    EXPECT_TRUE(stats::PlanStats::Reband(band, banded, 1024.0 * 1.35));  // +0.87 band
    EXPECT_EQ(band, 21);
}

}  // namespace
}  // namespace kds::server
