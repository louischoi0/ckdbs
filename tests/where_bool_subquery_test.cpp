#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// BU-S3: a subquery predicate under `OR` or `NOT`
// (instructions/v3.0.0/workorder-bu-basic-query-surface.md).
//
// Each one becomes a leaf of its boolean conjunct and a sub-chain of the step
// the conjunct is placed at, marked `in_tree` (step_chain.hpp). Three things
// can go wrong, and each has cells. **Hoisting**: an uncorrelated `EXISTS`
// under an `OR` is not a conjunct of the statement, so a false one must not
// empty it. **Three-valued logic** (BU-R4): `IN` over a set holding NULL is
// unknown, and `NOT` keeps it unknown. **Indexing**: a leaf names its
// sub-chain by its place in the step's list, beside the flat sub-chains and
// other trees' - a wrong index reads another subquery's answer.

namespace kds::server {
namespace {

class WhereBoolSubqueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        recorder_.emplace(boot_->catalog, store_);
        cabins_.emplace();
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), &*recorder_, /*replay=*/true,
                            /*access_statistics=*/true, &*cabins_);

        // t: ids 1..6, k = id - 1, n nullable: NULL on ids 2 and 5.
        Ok("CREATE TABLE t (id int64, k int64, sym varchar, n int64 NULL) BTREE");
        Ok("INSERT INTO t VALUES (0, 'a', 1)");
        Ok("INSERT INTO t VALUES (1, 'b', NULL)");
        Ok("INSERT INTO t VALUES (2, 'a', 3)");
        Ok("INSERT INTO t VALUES (3, 'b', 1)");
        Ok("INSERT INTO t VALUES (4, 'a', NULL)");
        Ok("INSERT INTO t VALUES (5, 'c', 6)");
        // u: ids 1..4, w = id - 1 (t.k of the same id), m = w but NULL on id 2.
        Ok("CREATE TABLE u (id int64, w int64, m int64 NULL) BTREE");
        Ok("INSERT INTO u VALUES (0, 0)");
        Ok("INSERT INTO u VALUES (1, NULL)");
        Ok("INSERT INTO u VALUES (2, 2)");
        Ok("INSERT INTO u VALUES (3, 3)");
        // e: one row, x = 2.
        Ok("CREATE TABLE e (id int64, x int64) BTREE");
        Ok("INSERT INTO e VALUES (2)");
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    void Ok(const std::string& sql) {
        const std::string reply = Run(sql);
        ASSERT_NE(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
    }
    std::string Ids(const std::string& where) { return Run("SELECT id FROM t WHERE " + where); }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::TrailRecorder> recorder_;
    std::optional<stats::CabinStore> cabins_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- The forms --------------------------------------------------------------

TEST_F(WhereBoolSubqueryTest, ACorrelatedExistsUnderOr) {
    EXPECT_EQ(Ids("EXISTS (SELECT id FROM u WHERE u.w = t.k) OR sym = 'c'"),
              "id\\n1\\n2\\n3\\n4\\n6");
    EXPECT_EQ(Ids("NOT (EXISTS (SELECT id FROM u WHERE u.w = t.k))"), "id\\n5\\n6");
}

TEST_F(WhereBoolSubqueryTest, InAndNotInUnderNot) {
    EXPECT_EQ(Ids("NOT (k IN (SELECT w FROM u))"), "id\\n5\\n6");
    EXPECT_EQ(Ids("NOT (k NOT IN (SELECT w FROM u)) OR id = 6"), "id\\n1\\n2\\n3\\n4\\n6");
}

TEST_F(WhereBoolSubqueryTest, AScalarSubqueryUnderOr) {
    EXPECT_EQ(Ids("k = (SELECT w FROM u WHERE u.id = 2) OR id = 6"), "id\\n2\\n6");
    // No u row for t's 5 and 6: the scalar is NULL there, the comparison
    // unknown, and only the OR's other side can keep the row.
    EXPECT_EQ(Ids("k = (SELECT w FROM u WHERE u.id = t.id) OR sym = 'c'"),
              "id\\n1\\n2\\n3\\n4\\n6");
}

// ---- Hoisting -------------------------------------------------------------

TEST_F(WhereBoolSubqueryTest, AFalseUncorrelatedExistsUnderOrEmptiesNothing) {
    EXPECT_EQ(Ids("EXISTS (SELECT id FROM e WHERE x = 99) OR sym = 'c'"), "id\\n6");
    EXPECT_EQ(Ids("id = 1 OR EXISTS (SELECT id FROM e WHERE x = 99)"), "id\\n1");
    EXPECT_EQ(Ids("EXISTS (SELECT id FROM e) OR id = 1"), "id\\n1\\n2\\n3\\n4\\n5\\n6");
    EXPECT_EQ(Ids("NOT (EXISTS (SELECT id FROM e WHERE x = 99))"), "id\\n1\\n2\\n3\\n4\\n5\\n6");
    EXPECT_EQ(Ids("NOT (EXISTS (SELECT id FROM e))"), "id");
}

TEST_F(WhereBoolSubqueryTest, AStatementRunTwiceAnswersTheSameWithItsTrail) {
    const std::string sql =
        "SELECT id FROM t WHERE id = 3 AND (EXISTS (SELECT id FROM e WHERE x = 99) OR sym = 'a')";
    EXPECT_EQ(Run(sql), "id\\n3");
    for (int i = 0; i < 4; ++i) EXPECT_EQ(Run(sql), "id\\n3");
}

// ---- Three-valued logic (BU-R4) -------------------------------------------

TEST_F(WhereBoolSubqueryTest, InOverASetHoldingNullIsUnknownAndNotKeepsItSo) {
    // u.m is {0, NULL, 2, 3}: `k IN` is true for k 0, 2, 3 and unknown for
    // the rest, so its NOT is false or unknown everywhere.
    EXPECT_EQ(Ids("NOT (k IN (SELECT m FROM u))"), "id");
    EXPECT_EQ(Ids("NOT (k IN (SELECT m FROM u))"), Ids("k NOT IN (SELECT m FROM u)"));
    EXPECT_EQ(Ids("k IN (SELECT m FROM u) OR id = 2"), "id\\n1\\n2\\n3\\n4");
    EXPECT_EQ(Ids("NOT (k IN (SELECT m FROM u)) OR n IS NULL"), "id\\n2\\n5");
}

// ---- Indexing ---------------------------------------------------------------

TEST_F(WhereBoolSubqueryTest, TwoSubqueriesInOneTree) {
    EXPECT_EQ(Ids("EXISTS (SELECT id FROM u WHERE u.w = t.k AND u.id = 1) OR "
                  "k IN (SELECT w FROM u WHERE w > 2)"),
              "id\\n1\\n4");
}

TEST_F(WhereBoolSubqueryTest, TwoTreesOnOneStepReadTheirOwnSubqueries) {
    // The second tree's leaf is the step's second sub-chain: read as the
    // first (a false EXISTS), it would drop row 5.
    EXPECT_EQ(Ids("(id = 1 OR id = 5 OR EXISTS (SELECT id FROM e WHERE x = 99)) AND "
                  "(NOT EXISTS (SELECT id FROM u WHERE u.w = t.k) OR id = 1)"),
              "id\\n1\\n5");
}

TEST_F(WhereBoolSubqueryTest, ATreeBesideAFlatSubquery) {
    const std::string where =
        "EXISTS (SELECT id FROM u WHERE u.w = t.k) AND "
        "(k IN (SELECT w FROM u WHERE w = 0) OR k IN (SELECT w FROM u WHERE w = 3))";
    EXPECT_EQ(Ids(where), "id\\n1\\n4");
    // A write's WHERE attaches the flat sub-chain first, so the tree's leaves
    // are rebased past it.
    EXPECT_EQ(Run("DELETE FROM t WHERE " + where), "DELETED 2");
    EXPECT_EQ(Ids("id > 0"), "id\\n2\\n3\\n5\\n6");
}

// ---- Placement --------------------------------------------------------------

TEST_F(WhereBoolSubqueryTest, AcrossAJoinItWaitsForTheRelationItReads) {
    // The EXISTS reads u, the join's second step: placed at the first, it
    // would read a u row not yet bound.
    EXPECT_EQ(Run("SELECT t.id FROM t JOIN u ON t.k = u.w WHERE "
                  "EXISTS (SELECT id FROM e WHERE e.x = u.w) OR t.id > 3"),
              "t.id\\n3\\n4");
}

TEST_F(WhereBoolSubqueryTest, InsideASubqueryAndReachingTwoLevelsOut) {
    EXPECT_EQ(Ids("EXISTS (SELECT id FROM u WHERE u.w = t.k AND "
                  "(EXISTS (SELECT id FROM e WHERE x = 7) OR u.id = 1))"),
              "id\\n1");
    // The innermost EXISTS reads t, two levels out: correlated, so never
    // answered once for the statement.
    EXPECT_EQ(Ids("EXISTS (SELECT id FROM u WHERE u.w = t.k AND "
                  "(EXISTS (SELECT id FROM e WHERE e.x = t.k) OR u.id = 1))"),
              "id\\n1\\n3");
}

// ---- Writes -----------------------------------------------------------------

TEST_F(WhereBoolSubqueryTest, AnUpdateAndADeleteWriteTheRowsTheSelectFinds) {
    EXPECT_EQ(Run("UPDATE t SET n = 0 WHERE EXISTS (SELECT id FROM e WHERE x = 99) OR id = 2"),
              "UPDATED 1");
    EXPECT_EQ(Ids("n = 0"), "id\\n2");
    EXPECT_EQ(Run("DELETE FROM t WHERE NOT (EXISTS (SELECT id FROM u WHERE u.w = t.k)) OR id = 1"),
              "DELETED 3");
    EXPECT_EQ(Ids("id > 0"), "id\\n2\\n3\\n4");
}

// ---- The plan ---------------------------------------------------------------

TEST_F(WhereBoolSubqueryTest, APlanNamesTheSubqueryUnderItsLeaf) {
    const std::string plan =
        Run("ANALYZE SELECT id FROM t WHERE id = 1 OR EXISTS (SELECT id FROM u WHERE u.w = t.k)");
    EXPECT_NE(plan.find("filter bool (0:0.0 = 1 OR sub0)"), std::string::npos) << plan;
    EXPECT_NE(plan.find("\\n  sub0\\n    correlated EXISTS [correlated]"), std::string::npos)
        << plan;
}

}  // namespace
}  // namespace kds::server
