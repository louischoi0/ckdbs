#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// `SHOW RELAYOUT` (docs/spec/physical-optimizer.md §5/§8, workplan PX06):
// the shadow report's surface. The planner's own behavior is pinned in
// relayout_planner_test.cpp; this file pins the rendering, the off notice,
// the refusals, and the advisory family's standing assertion - a report
// changes no reply.

namespace kds::server {
namespace {

class Instance {
public:
    Instance() {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        EXPECT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    CommandDispatcher& dispatcher() { return *dispatcher_; }

private:
    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

void Load(Instance& db) {
    ASSERT_EQ(db.Run("CREATE TABLE h (id int64, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(db.Run("CREATE TABLE b (id int64, v int64) BTREE").substr(0, 7), "CREATED");
    for (int i = 1; i <= 8; ++i) {
        ASSERT_EQ(db.Run("INSERT INTO h VALUES (" + std::to_string(i) + ")").substr(0, 8),
                  "INSERTED");
        ASSERT_EQ(db.Run("INSERT INTO b VALUES (" + std::to_string(i) + ")").substr(0, 8),
                  "INSERTED");
    }
    for (int i = 1; i <= 3; ++i) {
        ASSERT_EQ(db.Run("DELETE FROM h WHERE id = " + std::to_string(i)).substr(0, 7),
                  "DELETED");
    }
    db.Run("SELECT * FROM h");
    db.Run("SELECT * FROM b WHERE id = 2");
}

TEST(ShowRelayoutTest, TheBareFormListsShapesPlansAndGates) {
    Instance db;
    Load(db);

    const std::string report = db.Run("SHOW RELAYOUT");
    EXPECT_EQ(report.substr(0, 20), "relayout_relations=2") << report;

    // The heap relation: three candidate plans, each naming its gate, none
    // surveyed - the bare form has no walk to back a number with.
    EXPECT_NE(report.find("rel=h clustered=heap"), std::string::npos) << report;
    EXPECT_NE(report.find("plan=compact blocked_on=reader-horizon surveyed=0"),
              std::string::npos)
        << report;
    EXPECT_NE(report.find("plan=cluster blocked_on=ordered-between"), std::string::npos)
        << report;
    EXPECT_NE(report.find("plan=defrag blocked_on=page-reuse"), std::string::npos) << report;
    EXPECT_EQ(report.find("survey "), std::string::npos)
        << "the bare form walked a relation: " << report;

    // The scan shape it recorded, weighted (no clock: raw count in Q24.8).
    EXPECT_NE(report.find("shape kind=Scan"), std::string::npos) << report;

    // The btree relation: shapes, and an empty candidate set with the
    // reason - not three plans nobody could ever enact (R5).
    EXPECT_NE(report.find("rel=b clustered=btree"), std::string::npos) << report;
    EXPECT_NE(report.find("plans=none reason=btree-outside-v1-mover-scope"), std::string::npos)
        << report;

    // Catalog relations stored in user tuple format (`pattern_defs`,
    // `assertions`) are outside the mover's jurisdiction, and the bare
    // report skips them rather than listing gates that can never open.
    EXPECT_EQ(report.find("rel=pattern_defs"), std::string::npos) << report;
    EXPECT_EQ(report.find("rel=assertions"), std::string::npos) << report;
}

TEST(ShowRelayoutTest, ACatalogRelationAskedByNameAnswersWithTheReason) {
    Instance db;
    Load(db);

    const std::string report = db.Run("SHOW RELAYOUT pattern_defs");
    EXPECT_NE(report.find("plans=none reason=catalog-relation-outside-mover-jurisdiction"),
              std::string::npos)
        << report;
    EXPECT_EQ(report.find("survey "), std::string::npos)
        << "a catalog relation must not be walked: " << report;
}

TEST(ShowRelayoutTest, ThePerRelationFormSurveysAndPredicts) {
    Instance db;
    Load(db);

    const std::string report = db.Run("SHOW RELAYOUT h");
    EXPECT_EQ(report.substr(0, 20), "relayout_relations=1") << report;
    EXPECT_NE(report.find("survey pages=1 live=5 delete_marked=3"), std::string::npos)
        << report;
    EXPECT_NE(report.find("plan=compact blocked_on=reader-horizon surveyed=1"),
              std::string::npos)
        << report;
    // Five live rows still fit the one page the chain holds, so the honest
    // prediction is zero pages saved - measured, not absent.
    EXPECT_NE(report.find("predicted_pages_saved=0"), std::string::npos) << report;

    const std::string btree = db.Run("SHOW RELAYOUT b");
    EXPECT_EQ(btree.find("survey "), std::string::npos)
        << "a btree relation must not be walked: " << btree;
    EXPECT_NE(btree.find("plans=none"), std::string::npos) << btree;
}

TEST(ShowRelayoutTest, OffAnswersTheOneLineNotice) {
    Instance db;
    Load(db);
    db.dispatcher().set_relayout(PhysicalOptimizerMode::kOff, 600'000'000'000ULL);

    EXPECT_EQ(db.Run("SHOW RELAYOUT"), "RELAYOUT off (physical_optimizer=off)");
    EXPECT_EQ(db.Run("SHOW RELAYOUT h"), "RELAYOUT off (physical_optimizer=off)");
}

TEST(ShowRelayoutTest, RefusalsNameTheProblem) {
    Instance db;
    Load(db);

    EXPECT_EQ(db.Run("SHOW RELAYOUT nope"), "ERR unknown relation 'nope'");
    EXPECT_EQ(db.Run("SHOW RELAYOUT h b"), "ERR SHOW RELAYOUT takes at most one relation name");
}

TEST(ShowRelayoutTest, TheReportChangesNoReply) {
    // The advisory family's standing rule, held trivially by a read-only
    // report and asserted anyway - the same posture every other advisory
    // surface in this engine takes.
    Instance db;
    Load(db);

    const std::vector<std::string> queries = {
        "SELECT * FROM h",
        "SELECT * FROM h WHERE id = 5",
        "SELECT * FROM b WHERE id = 2",
        "SELECT COUNT(*) FROM h",
    };
    std::vector<std::string> before;
    for (const std::string& sql : queries) before.push_back(db.Run(sql));

    db.Run("SHOW RELAYOUT");
    db.Run("SHOW RELAYOUT h");
    db.Run("SHOW RELAYOUT b");

    for (std::size_t i = 0; i < queries.size(); ++i) {
        EXPECT_EQ(db.Run(queries[i]), before[i]) << queries[i];
    }
}

}  // namespace
}  // namespace kds::server
