#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// BU-S1: `OR`, `NOT` and parentheses in a WHERE
// (instructions/v3.0.0/workorder-bu-basic-query-surface.md).
//
// Two groups matter most. **Three-valued logic** (BU-R4): a comparison
// with a NULL operand is unknown inside the tree, and only the root's
// unknown is false - so `NOT (n = 1)` keeps no row whose `n` is NULL. And
// **the readers** (BU-R5): a comparison under an `OR` is optional, so a
// reader that took it for a conjunct - a pk lookup, an index or Cabin
// probe, a join key, equality propagation, a write's point target, PURGE's
// window - would return or write the wrong rows. Each reader cell puts an
// `OR` where that reader looks and pins the exact reply.

namespace kds::server {
namespace {

class WhereBoolTest : public ::testing::Test {
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

        // t: ids 1..6, v = 10*id (indexed), k = id - 1 (the join column),
        // sym (Cabin), n nullable: NULL on ids 2 and 5.
        Ok("CREATE TABLE t (id int64, v int64, k int64, sym varchar, n int64 NULL) BTREE");
        Ok("CREATE INDEX t_by_v ON t (v)");
        Ok("CREATE CABIN ON t(sym)");
        Ok("INSERT INTO t VALUES (10, 0, 'a', 1)");
        Ok("INSERT INTO t VALUES (20, 1, 'b', NULL)");
        Ok("INSERT INTO t VALUES (30, 2, 'a', 3)");
        Ok("INSERT INTO t VALUES (40, 3, 'b', 1)");
        Ok("INSERT INTO t VALUES (50, 4, 'a', NULL)");
        Ok("INSERT INTO t VALUES (60, 5, 'c', 6)");
        // u: ids 1..4, w = id - 1 = t.k of the same id; no index, no Cabin.
        Ok("CREATE TABLE u (id int64, w int64) BTREE");
        for (int i = 0; i < 4; ++i) Ok("INSERT INTO u VALUES (" + std::to_string(i) + ")");
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    void Ok(const std::string& sql) {
        const std::string reply = Run(sql);
        ASSERT_NE(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
    }
    // The ids a statement answers, as the reply prints them.
    std::string Ids(const std::string& where) { return Run("SELECT id FROM t WHERE " + where); }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::TrailRecorder> recorder_;
    std::optional<stats::CabinStore> cabins_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- The operators --------------------------------------------------------

TEST_F(WhereBoolTest, OrKeepsARowEitherSideKeeps) {
    EXPECT_EQ(Ids("v = 10 OR v = 40"), "id\\n1\\n4");
    EXPECT_EQ(Ids("sym = 'c' OR k = 0 OR id = 3"), "id\\n1\\n3\\n6");
}

TEST_F(WhereBoolTest, AndBindsTighterThanOr) {
    EXPECT_EQ(Ids("v = 10 OR v = 20 AND sym = 'a'"), "id\\n1");
    EXPECT_EQ(Ids("(v = 10 OR v = 20) AND sym = 'b'"), "id\\n2");
}

TEST_F(WhereBoolTest, NotNegatesAPredicateAndAGroup) {
    EXPECT_EQ(Ids("NOT (sym = 'a')"), "id\\n2\\n4\\n6");
    EXPECT_EQ(Ids("NOT sym = 'a'"), "id\\n2\\n4\\n6");
    EXPECT_EQ(Ids("NOT (sym = 'a' OR sym = 'b')"), "id\\n6");
    EXPECT_EQ(Ids("NOT NOT sym = 'c'"), "id\\n6");
}

TEST_F(WhereBoolTest, AParenthesizedAndIsTheFlatAnd) {
    EXPECT_EQ(Ids("(sym = 'a' AND k > 0)"), Ids("sym = 'a' AND k > 0"));
    EXPECT_EQ(Ids("((id = 3))"), "id\\n3");
}

TEST_F(WhereBoolTest, BetweenAndAFunctionAndAColumnPairAreLeaves) {
    EXPECT_EQ(Ids("v BETWEEN 20 AND 30 OR id = 6"), "id\\n2\\n3\\n6");
    EXPECT_EQ(Ids("NOT (v BETWEEN 20 AND 50)"), "id\\n1\\n6");
    EXPECT_EQ(Ids("k = n OR id = 6"), "id\\n6");  // no row has k = n
    EXPECT_EQ(Ids("id = 1 OR n IS NULL"), "id\\n1\\n2\\n5");
}

// ---- Three-valued logic (BU-R4) -------------------------------------------

TEST_F(WhereBoolTest, NotOfUnknownIsUnknownAndKeepsNoRow) {
    // n is NULL on 2 and 5: `n = 1` is unknown there, and so is its NOT.
    EXPECT_EQ(Ids("NOT (n = 1)"), "id\\n3\\n6");
    EXPECT_EQ(Ids("n = 1 OR NOT (n = 1)"), "id\\n1\\n3\\n4\\n6");
    EXPECT_EQ(Ids("NOT (n IS NULL)"), "id\\n1\\n3\\n4\\n6");
}

TEST_F(WhereBoolTest, TrueOrUnknownIsTrueAndFalseAndUnknownIsFalse) {
    EXPECT_EQ(Ids("n = 1 OR id = 2"), "id\\n1\\n2\\n4");
    // On 2 and 5: unknown AND false is false, so its NOT is true.
    EXPECT_EQ(Ids("NOT (n = 1 AND id = 99)"), "id\\n1\\n2\\n3\\n4\\n5\\n6");
    // On 2 and 5: unknown AND true is unknown, so its NOT keeps nothing there.
    EXPECT_EQ(Ids("NOT (n = 1 AND id < 99)"), "id\\n3\\n6");
}

TEST_F(WhereBoolTest, ANullLiteralUnderNotIsUnknown) {
    EXPECT_EQ(Ids("NOT (n = NULL)"), "id");
    EXPECT_EQ(Ids("n = NULL OR id = 1"), "id\\n1");
}

// ---- The readers (BU-R5) --------------------------------------------------

TEST_F(WhereBoolTest, OverThePkItIsNotALookupOrARange) {
    EXPECT_EQ(Ids("id = 2 OR id = 5"), "id\\n2\\n5");
    EXPECT_EQ(Ids("id BETWEEN 2 AND 3 OR id = 6"), "id\\n2\\n3\\n6");
    EXPECT_EQ(Ids("NOT (id = 2)"), "id\\n1\\n3\\n4\\n5\\n6");
}

TEST_F(WhereBoolTest, BesideAPkEqualityTheLookupStillFilters) {
    EXPECT_EQ(Ids("id = 2 AND (v = 20 OR v = 30)"), "id\\n2");
    EXPECT_EQ(Ids("id = 2 AND (v = 10 OR v = 30)"), "id");
}

TEST_F(WhereBoolTest, OverAnIndexedColumnItIsNotAnIndexProbe) {
    EXPECT_EQ(Ids("v = 20 OR v = 60"), "id\\n2\\n6");
    // The index keeps the entry for v = 20 after the row moves (append-only):
    // the row must not come back for the value it no longer has.
    Ok("UPDATE t SET v = 25 WHERE id = 2");
    EXPECT_EQ(Ids("v = 20 OR v = 60"), "id\\n6");
    EXPECT_EQ(Ids("v = 25 OR v = 60"), "id\\n2\\n6");
    EXPECT_EQ(Ids("v = 20 AND (id = 2 OR id = 6)"), "id");
}

TEST_F(WhereBoolTest, BesideACabinedEqualityItStillFilters) {
    EXPECT_EQ(Ids("sym = 'a' AND (k = 0 OR k = 4)"), "id\\n1\\n5");
    EXPECT_EQ(Ids("sym = 'b' OR sym = 'c'"), "id\\n2\\n4\\n6");
}

TEST_F(WhereBoolTest, AcrossAJoinItIsPlacedWhereBothSidesExist) {
    EXPECT_EQ(Run("SELECT t.id, u.id FROM t JOIN u ON t.k = u.w WHERE t.id = 1 OR u.id = 3"),
              "t.id,u.id\\n1,1\\n3,3");
    EXPECT_EQ(Run("SELECT t.id FROM t JOIN u ON t.k = u.w WHERE NOT (u.id = 2)"),
              "t.id\\n1\\n3\\n4");
}

TEST_F(WhereBoolTest, EqualityPropagationDrawsNothingFromUnderOr) {
    // Were `u.w = 1` propagated to `t.k = 1`, the join would drop t's row 3.
    EXPECT_EQ(Run("SELECT t.id FROM t JOIN u ON t.k = u.w WHERE u.w = 1 OR u.w = 2"),
              "t.id\\n2\\n3");
    EXPECT_EQ(Run("SELECT t.id FROM t JOIN u ON t.k = u.w WHERE NOT (u.w = 1)"),
              "t.id\\n1\\n3\\n4");
}

TEST_F(WhereBoolTest, InsideACorrelatedSubqueryItReadsTheOuterRow) {
    EXPECT_EQ(Ids("EXISTS (SELECT id FROM u WHERE u.w = t.k AND (u.id = 1 OR u.id = 3))"),
              "id\\n1\\n3");
    EXPECT_EQ(Ids("v > 10 AND NOT EXISTS (SELECT id FROM u WHERE u.w = t.k OR u.id = 99)"),
              "id\\n5\\n6");
}

TEST_F(WhereBoolTest, AnUpdateAndADeleteWriteTheRowsTheSelectFinds) {
    EXPECT_EQ(Run("UPDATE t SET k = 100 WHERE sym = 'c' OR n IS NULL"), "UPDATED 3");
    EXPECT_EQ(Ids("k = 100"), "id\\n2\\n5\\n6");
    // A pk equality beside an OR is not a point write that skips the OR.
    EXPECT_EQ(Run("UPDATE t SET k = 7 WHERE id = 1 AND (v = 999 OR sym = 'z')"), "UPDATED 0");
    EXPECT_EQ(Run("DELETE FROM t WHERE NOT (n = 1) OR id = 2"), "DELETED 3");
    EXPECT_EQ(Ids("id > 0"), "id\\n1\\n4\\n5");
}

TEST_F(WhereBoolTest, APlanShowsABooleanConjunctUnderItsOwnWord) {
    const std::string plan = Run("ANALYZE SELECT id FROM t WHERE v = 10 OR NOT (n = 1)");
    EXPECT_NE(plan.find("filter bool (0:0.1 = 10 OR NOT (0:0.4 = 1))"), std::string::npos) << plan;
}

TEST_F(WhereBoolTest, OrAndAndAreDifferentShapes) {
    const auto a = parser::FingerprintOf("SELECT id FROM t WHERE v = 10 OR v = 20");
    const auto b = parser::FingerprintOf("SELECT id FROM t WHERE v = 10 AND v = 20");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_NE(a->pattern_id, b->pattern_id);
}

TEST_F(WhereBoolTest, AStatementRunTwiceAnswersTheSameWithItsTrail) {
    const std::string sql = "SELECT id FROM t WHERE id = 3 AND (sym = 'a' OR n IS NULL)";
    const std::string first = Run(sql);
    EXPECT_EQ(first, "id\\n3");
    for (int i = 0; i < 4; ++i) EXPECT_EQ(Run(sql), first);
}

// ---- Refusals (BU-R6) -----------------------------------------------------

TEST_F(WhereBoolTest, WhatIsRefusedAndWhere) {
    // A subquery under OR or NOT is BU-S3's.
    EXPECT_EQ(Run("SELECT id FROM t WHERE id = 1 OR EXISTS (SELECT id FROM u)"),
              "ERR NOT_IMPLEMENTED retryable=0 a subquery predicate under OR or NOT is not "
              "supported yet (byte 33)");
    EXPECT_EQ(Run("SELECT id FROM t WHERE NOT (k IN (SELECT w FROM u))"),
              "ERR NOT_IMPLEMENTED retryable=0 a subquery predicate under OR or NOT is not "
              "supported yet (byte 28)");
    // NOT EXISTS stays the leaf it was.
    EXPECT_EQ(Ids("NOT EXISTS (SELECT id FROM u WHERE u.w = t.k)"), "id\\n5\\n6");
    // OR over a catalog view (BU-Q4).
    EXPECT_EQ(Run("SELECT name FROM sys.tables WHERE oid = 1 OR oid = 2"),
              "ERR NOT_IMPLEMENTED retryable=0 a boolean conjunct (OR, NOT, an IN list, LIKE) over a "
              "catalog view (byte 34)");
    // PURGE's window is a conjunction of pk comparisons, OR included.
    EXPECT_EQ(Run("PURGE FROM t WHERE id = 1 OR id = 9"),
              "ERR NOT_IMPLEMENTED retryable=0 PURGE admits only comparisons of the primary key "
              "with an integer literal (=, <, <=, >, >=, BETWEEN); this conjunct is not one "
              "(byte 19)");
    // An unclosed group.
    EXPECT_EQ(Run("SELECT id FROM t WHERE (id = 1 OR id = 2").rfind("ERR", 0), 0u);
}

TEST_F(WhereBoolTest, ATreePastTheCapIsRefusedAtItsByte) {
    std::string deep = "id = 1";
    for (int i = 0; i < 64; ++i) deep = "(" + deep + ")";
    EXPECT_EQ(Ids(deep), "id\\n1");
    const std::string refused = Ids("(" + deep + ")");
    EXPECT_EQ(refused.rfind("ERR UNSUPPORTED", 0), 0u) << refused;
    std::string nots = "id = 1";
    for (int i = 0; i < 65; ++i) nots = "NOT " + nots;
    EXPECT_EQ(Ids(nots).rfind("ERR UNSUPPORTED", 0), 0u);
}

TEST_F(WhereBoolTest, TheNestingCapIsTheStatementsNotEachWheres) {
    // 40 levels in the outer WHERE and 40 in the subquery's: each is under
    // the cap, the statement is not (BU-Q2, the review of BU-S1).
    std::string inner = "u.w = t.k";
    for (int i = 0; i < 40; ++i) inner = "(" + inner + ")";
    std::string outer = "EXISTS (SELECT id FROM u WHERE " + inner + ")";
    for (int i = 0; i < 40; ++i) outer = "(" + outer + ")";
    EXPECT_EQ(Ids(outer).rfind("ERR UNSUPPORTED", 0), 0u);
}

TEST_F(WhereBoolTest, TheSizeCapCountsAnOrGroupNotAFlatAnd) {
    std::string flat = "id > 0";
    for (int i = 0; i < 5000; ++i) flat += " AND id > 0";
    EXPECT_EQ(Ids(flat), "id\\n1\\n2\\n3\\n4\\n5\\n6");
    std::string wide = "id = 0";
    for (int i = 1; i < 4096; ++i) wide += " OR id = " + std::to_string(i);
    EXPECT_EQ(Ids(wide), "id\\n1\\n2\\n3\\n4\\n5\\n6");
    EXPECT_EQ(Ids(wide + " OR id = 9999").rfind("ERR UNSUPPORTED", 0), 0u);
}

}  // namespace
}  // namespace kds::server
