#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
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

}  // namespace
}  // namespace kds::server
