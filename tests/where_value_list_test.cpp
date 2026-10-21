#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// BU-S2: `IN (v, ...)`, `NOT IN (v, ...)`, `NOT BETWEEN`, `LIKE`, and a
// literal on the left of a comparison
// (instructions/v3.0.0/workorder-bu-basic-query-surface.md).
//
// A value list is a disjunction of equalities and `NOT IN` a conjunction of
// inequalities, so each is three-valued exactly as the standard says: a
// NULL in a `NOT IN` list keeps no row, and a NULL probe is unknown for
// both. Like every `OR`, a list is never a key (BU-R5).

namespace kds::server {
namespace {

class WhereValueListTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        // **The recording arm is vacuous since CF-S1b**, named here rather
        // than left silent: the recorder is built as a server's default is,
        // but nothing records until a shape is opted in (`WAYSTONE ON`),
        // and no cell in this file opts one in. No assertion here reads a
        // trail or `replays=`, so none depends on one.
        recorder_.emplace(boot_->catalog, store_);
        cabins_.emplace();
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), &*recorder_, /*replay=*/true,
                            /*access_statistics=*/true, &*cabins_);

        // ids 1..6; v = 10*id (indexed); name; n nullable (NULL on 2 and 5);
        // d a date; m a decimal.
        Ok("CREATE TABLE t (id int64, v int64, name varchar, n int64 NULL, d date, "
           "m decimal(10,2)) BTREE");
        Ok("CREATE INDEX t_by_v ON t (v)");
        Ok("INSERT INTO t VALUES (10, 'alice', 1, '2026-01-01', '1.50')");
        Ok("INSERT INTO t VALUES (20, 'bob', NULL, '2026-01-02', '2.50')");
        Ok("INSERT INTO t VALUES (30, 'carol', 3, '2026-01-03', '3.50')");
        Ok("INSERT INTO t VALUES (40, 'dave', 1, '2026-01-04', '4.50')");
        Ok("INSERT INTO t VALUES (50, 'erin', NULL, '2026-01-05', '5.50')");
        Ok("INSERT INTO t VALUES (60, 'a_b%c', 6, '2026-01-06', '6.50')");
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

// ---- IN and NOT IN over a value list --------------------------------------

TEST_F(WhereValueListTest, InKeepsARowEqualToAnyItem) {
    EXPECT_EQ(Ids("id IN (2, 5)"), "id\\n2\\n5");
    EXPECT_EQ(Ids("v IN (30, 60, 999)"), "id\\n3\\n6");
    EXPECT_EQ(Ids("name IN ('bob', 'dave')"), "id\\n2\\n4");
    EXPECT_EQ(Ids("d IN ('2026-01-03')"), "id\\n3");
    EXPECT_EQ(Ids("m IN ('1.50', '6.50')"), "id\\n1\\n6");
}

TEST_F(WhereValueListTest, NotInKeepsARowEqualToNoItem) {
    EXPECT_EQ(Ids("id NOT IN (2, 5)"), "id\\n1\\n3\\n4\\n6");
    // A NULL probe is unknown: rows 2 and 5 are kept by neither.
    EXPECT_EQ(Ids("n IN (1, 3)"), "id\\n1\\n3\\n4");
    EXPECT_EQ(Ids("n NOT IN (1, 3)"), "id\\n6");
}

TEST_F(WhereValueListTest, ANullInTheListIsUnknownNeverAMatch) {
    EXPECT_EQ(Ids("n IN (1, NULL)"), "id\\n1\\n4");
    // `n != NULL` is unknown for every row, so NOT IN keeps nothing.
    EXPECT_EQ(Ids("n NOT IN (1, NULL)"), "id");
    EXPECT_EQ(Ids("NOT (n NOT IN (1, NULL))"), "id\\n1\\n4");
}

TEST_F(WhereValueListTest, AListCombinesWithAndOrAndNot) {
    EXPECT_EQ(Ids("id IN (1, 2, 3) AND v > 10"), "id\\n2\\n3");
    EXPECT_EQ(Ids("id IN (1) OR name IN ('erin')"), "id\\n1\\n5");
    EXPECT_EQ(Ids("NOT (id IN (1, 2, 3))"), "id\\n4\\n5\\n6");
}

TEST_F(WhereValueListTest, AWriteTakesTheRowsTheListNames) {
    EXPECT_EQ(Run("UPDATE t SET n = 0 WHERE id IN (1, 6)"), "UPDATED 2");
    EXPECT_EQ(Run("DELETE FROM t WHERE id NOT IN (1, 6) AND n IS NULL"), "DELETED 2");
    EXPECT_EQ(Ids("id > 0"), "id\\n1\\n3\\n4\\n6");
}

TEST_F(WhereValueListTest, AListLiteralIsCoercedAtCompileWithItsByte) {
    const std::string refused = Ids("v IN (10, 'x')");
    EXPECT_EQ(refused.rfind("ERR", 0), 0u) << refused;
    EXPECT_NE(refused.find("byte 33"), std::string::npos) << refused;
}

// ---- NOT BETWEEN -----------------------------------------------------------

TEST_F(WhereValueListTest, NotBetweenKeepsWhatBetweenDoesNot) {
    EXPECT_EQ(Ids("v NOT BETWEEN 20 AND 50"), "id\\n1\\n6");
    EXPECT_EQ(Ids("n NOT BETWEEN 2 AND 5"), "id\\n1\\n4\\n6");
}

// ---- LIKE ------------------------------------------------------------------

TEST_F(WhereValueListTest, LikeMatchesPercentAndUnderscore) {
    EXPECT_EQ(Ids("name LIKE 'c%'"), "id\\n3");
    EXPECT_EQ(Ids("name LIKE '_ob'"), "id\\n2");
    EXPECT_EQ(Ids("name LIKE '%a%'"), "id\\n1\\n3\\n4\\n6");
    EXPECT_EQ(Ids("name LIKE 'alice'"), "id\\n1");
    EXPECT_EQ(Ids("name LIKE '%'"), "id\\n1\\n2\\n3\\n4\\n5\\n6");
    EXPECT_EQ(Ids("name NOT LIKE '%a%'"), "id\\n2\\n5");
    // Case-sensitive bytes (BU-Q6).
    EXPECT_EQ(Ids("name LIKE 'A%'"), "id");
    // A `%` byte in the value under a pattern `%` is still matched by the
    // wildcard, not consumed as a literal (row 6 is 'a_b%c').
    EXPECT_EQ(Ids("name LIKE 'a_b%'"), "id\\n6");
    EXPECT_EQ(Ids("name LIKE '___%'"), "id\\n1\\n2\\n3\\n4\\n5\\n6");
}

TEST_F(WhereValueListTest, LikeOverANonTextColumnIsRefused) {
    EXPECT_EQ(Ids("v LIKE '1%'").rfind("ERR", 0), 0u);
}

// ---- A literal on the left -------------------------------------------------

TEST_F(WhereValueListTest, ALiteralOnTheLeftIsTheMirroredComparison) {
    EXPECT_EQ(Ids("30 < v"), Ids("v > 30"));
    EXPECT_EQ(Ids("20 >= v"), "id\\n1\\n2");
    EXPECT_EQ(Ids("3 = id"), "id\\n3");
    EXPECT_EQ(Ids("'bob' = name OR 50 = v"), "id\\n2\\n5");
}

// ---- The subquery form keeps its cap ----------------------------------------

TEST_F(WhereValueListTest, InSubqueryNestingKeepsItsCapThroughTheNewBranch) {
    // The value-list branch reads the `(` itself, so the cap is checked there
    // too (the review of BU-S2): four levels parse, the fifth is refused.
    std::string sql = "SELECT id FROM t";
    for (int i = 0; i < 4; ++i) sql = "SELECT id FROM t WHERE id IN (" + sql + ")";
    EXPECT_EQ(Run(sql).rfind("ERR", 0), std::string::npos) << Run(sql);
    const std::string deeper = Run("SELECT id FROM t WHERE id IN (" + sql + ")");
    EXPECT_EQ(deeper.rfind("ERR UNSUPPORTED", 0), 0u) << deeper;
}

TEST_F(WhereValueListTest, OverACatalogViewAOneItemListIsAnEqualityAndALongerOneIsRefused) {
    EXPECT_EQ(Run("SELECT name FROM sys.tables WHERE name IN ('t')"), "name\\nt");
    EXPECT_EQ(Run("SELECT name FROM sys.tables WHERE name IN ('t', 'u')").rfind("ERR NOT_IMPLEMENTED", 0),
              0u);
}

}  // namespace
}  // namespace kds::server
