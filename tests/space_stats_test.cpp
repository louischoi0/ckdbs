// **How much of a relation is delete-marked, and how often its leaves
// divide** (DF-S4, `instructions/v3.2.0/workorder-df-statistics-coverage.md`
// §2 part D, DF-Q9 (a)): `sys.relation_stats`' `est_delete_marked`,
// `leaf_divisions` and `leaf_compactions`, and `sys.indexes`'
// `leaf_divisions`. One dispatcher with a transaction manager - a `PURGE`
// needs one - and the join planner's statistics, whose steward the cells
// step by hand.

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/leaf_stats.hpp"
#include "kds/stats/plan_stats.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"
#include "view_text.hpp"

namespace kds::server {
namespace {

constexpr std::uint64_t kSecond = 1'000'000'000;

class SpaceStatsTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 4000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        d_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                   /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                   exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                   /*access_statistics=*/true, /*cabins=*/nullptr, &*mgr_);
        d_->set_plan_stats(&stats_);
        stats::LeafStats::ResetForTest();
    }
    void TearDown() override { stats::LeafStats::ResetForTest(); }

    std::string Run(const std::string& sql) { return d_->Dispatch(sql).response; }
    void Ok(const std::string& sql) {
        const std::string out = Run(sql);
        ASSERT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    }
    void Steward(std::uint64_t now) {
        ASSERT_TRUE(stats_.StewardStep(boot_->catalog, store_, now).ok());
    }
    catalog::Oid OidOf(const std::string& name) {
        auto oid = boot_->catalog.ResolveRelation("", name, catalog::kNamespacePublic, 0, nullptr);
        EXPECT_TRUE(oid.ok()) << name;
        return oid.ok() ? oid.value() : 0;
    }

    // One column of `relation`'s row in `sys.relation_stats`.
    std::string Relation(const std::string& relation, std::string_view column) {
        const auto value = testing_view::RelationField(*d_, "relation_stats", relation, column);
        EXPECT_TRUE(value.has_value()) << relation << " has no row";
        return value.value_or("");
    }
    // `leaf_divisions` of an index, from `sys.indexes`.
    std::string IndexDivisions(std::string_view index) {
        const auto shown =
            testing_view::ReadViewText(Run("SELECT name, leaf_divisions FROM sys.indexes"));
        return shown.Find("name", index, "leaf_divisions").value_or("");
    }

    // `rows` rows of `t(id, v)`, ids `first, first + step, ...`, one a
    // statement.
    void Insert(const std::string& t, int first, int step, int rows) {
        for (int i = 0; i < rows; ++i) {
            const int id = first + i * step;
            Ok("INSERT INTO " + t + " VALUES (" + std::to_string(id) + ", " +
               std::to_string(id) + ")");
        }
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> d_;
    stats::PlanStats stats_{1};
};

TEST_F(SpaceStatsTest, TheStewardsSampleCountsDeleteMarkedRowsAndMovesNoGenerationForThem) {
    // One leaf of a hundred rows: the sample is the leaf, and exact.
    Ok("CREATE TABLE m (id int64, v int64) BTREE");
    Ok("CREATE TABLE u (id int64, v int64) BTREE");
    Ok("CREATE TABLE never (id int64, v int64) BTREE");
    Insert("m", 1, 1, 100);
    Insert("u", 1, 1, 4);
    Insert("never", 1, 1, 4);
    // A planned block names m and u, so the steward seeds both.
    const std::string join = "SELECT u.v FROM u JOIN m ON u.id = m.id WHERE m.v = 3";
    Ok(join);
    EXPECT_EQ(Relation("m", "est_delete_marked"), "NULL") << "no seed has read a leaf of it";
    Steward(kSecond);
    EXPECT_EQ(stats_.EstDeleteMarked(OidOf("m")), 0u);
    EXPECT_EQ(Relation("m", "est_delete_marked"), "0");
    EXPECT_EQ(Relation("u", "est_delete_marked"), "0");
    // A relation no planned run named has no prior and no estimate.
    EXPECT_EQ(stats_.EstDeleteMarked(OidOf("never")), std::nullopt);
    EXPECT_EQ(Relation("never", "est_delete_marked"), "NULL");

    // Three rows delete-marked: 97 live is inside 100's band, so the prior
    // does not move - and the estimate, which is no planner fact (DF-R8),
    // moves without a snapshot published.
    const std::uint64_t generation = stats_.generation();
    ASSERT_EQ(Run("DELETE FROM m WHERE id <= 3"), "DELETED 3");
    EXPECT_EQ(Relation("m", "est_delete_marked"), "0") << "the estimate is the last seed's";
    Steward(kSecond + stats::PlanStats::kReseedNs);
    EXPECT_EQ(stats_.EstDeleteMarked(OidOf("m")), 3u);
    EXPECT_EQ(Relation("m", "est_delete_marked"), "3");
    EXPECT_EQ(stats_.generation(), generation)
        << "the space estimate published a planner snapshot";

    // A `PURGE` retires the marked rows, and a later seed brings the
    // estimate down.
    ASSERT_EQ(Run("PURGE FROM m WHERE id <= 3"), "PURGED 3");
    Steward(kSecond + 2 * stats::PlanStats::kReseedNs);
    EXPECT_EQ(stats_.EstDeleteMarked(OidOf("m")), 0u);
    EXPECT_EQ(stats_.generation(), generation);

    // A dropped relation's estimate goes with its prior.
    const catalog::Oid dropped = OidOf("m");
    Ok("DROP TABLE m");
    Steward(kSecond + 3 * stats::PlanStats::kReseedNs);
    EXPECT_EQ(stats_.EstDeleteMarked(dropped), std::nullopt);
    EXPECT_EQ(stats_.EstDeleteMarked(OidOf("u")), 0u);
}

TEST_F(SpaceStatsTest, ADivisionIsCountedAndAnAppendIsNot) {
    Ok("CREATE TABLE d (id int64, v int64) BTREE");
    // Even keys, rising: every growth is an append of a fresh leaf.
    Insert("d", 2, 2, 600);
    ASSERT_GT(std::stoull(Relation("d", "leaves")), 1u) << "the tree did not grow";
    EXPECT_EQ(Relation("d", "leaf_divisions"), "0") << "an append was counted as a division";
    EXPECT_EQ(Relation("d", "leaf_compactions"), "0");

    // An odd key inside the first leaf, which the appends left full: the
    // leaf divides.
    Ok("INSERT INTO d VALUES (3, 3)");
    EXPECT_EQ(Relation("d", "leaf_divisions"), "1");
    EXPECT_EQ(Relation("d", "leaf_compactions"), "0");
    // Half the leaf is free now: the next key into it divides nothing.
    Ok("INSERT INTO d VALUES (5, 5)");
    EXPECT_EQ(Relation("d", "leaf_divisions"), "1");
    EXPECT_EQ(testing_view::MetaField(Run("SELECT * FROM sys.meta"), "leaf_stats_dropped"), "0");
}

TEST_F(SpaceStatsTest, AFullLeafWithARetiredSlotIsCompactedInPlaceOfADivision) {
    Ok("CREATE TABLE c (id int64, v int64) BTREE");
    Insert("c", 2, 2, 600);
    // A key of the full first leaf deleted and purged: its slot is retired,
    // keyless, and still takes its room.
    ASSERT_EQ(Run("DELETE FROM c WHERE id = 4"), "DELETED 1");
    ASSERT_EQ(Run("PURGE FROM c WHERE id = 4"), "PURGED 1");
    Ok("INSERT INTO c VALUES (3, 3)");
    EXPECT_EQ(Relation("c", "leaf_compactions"), "1");
    EXPECT_EQ(Relation("c", "leaf_divisions"), "0");
    EXPECT_EQ(Run("SELECT v FROM c WHERE id = 3"), "v\\n3");
}

TEST_F(SpaceStatsTest, AnIndexsLeafSplitsAreItsOwnAndNotItsRelations) {
    Ok("CREATE TABLE x (id int64, v int64) BTREE");
    Ok("CREATE INDEX x_v ON x (v)");
    EXPECT_EQ(IndexDivisions("x_v"), "0");
    // Rising keys: the clustered tree appends, the index's leaves split.
    Insert("x", 1, 1, 1200);
    EXPECT_GT(std::stoull(IndexDivisions("x_v")), 0u);
    EXPECT_EQ(Relation("x", "leaf_divisions"), "0");
    // A rebuilt index is a new tree under a new oid; its build's own splits
    // are counted to it.
    Ok("REINDEX INDEX x_v");
    const std::uint64_t rebuilt = std::stoull(IndexDivisions("x_v"));
    EXPECT_GT(rebuilt, 0u) << "the rebuild's splits were not counted";
    const auto counts = stats::LeafStats::Snapshot();
    EXPECT_EQ(counts.count(OidOf("x")), 0u) << "the relation's tree never divided";
}

}  // namespace
}  // namespace kds::server
