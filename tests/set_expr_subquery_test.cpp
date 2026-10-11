// **A scalar subquery in a SET value** (BJ-S7, BJ-Q6): run once, before the
// walk, under the statement's view; its answer is a constant of every row.
//
// A subquery evaluated per row would see the rows this statement had already
// written (a view always sees its own transaction's writes), and over another
// relation it is the page-latch cycle the write walk's exclusive leaf makes
// (`a-write-walks-subquery-reads-pages-under-its-exclusive-leaf-hold.md`).
// So the answer is taken before the first row, and a subquery that reads the
// row being updated is refused.

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "two_core_rig.hpp"
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/in_memory_page_store.hpp"

namespace kds::server {
namespace {

class SetExprSubqueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
        Exec("CREATE TABLE t (id int64, v int64, name varchar, d decimal(10,2), n int64 NULL)");
        Exec("CREATE TABLE u (id int64, x int64, s varchar, dd decimal(10,2), y int64 NULL)");
        for (int i = 1; i <= 3; ++i) {
            Exec("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i * 10) +
                 ", 'n" + std::to_string(i) + "', '1.50', NULL)");
        }
        Exec("INSERT INTO u VALUES (1, 100, 'one', '2.25', NULL)");
        Exec("INSERT INTO u VALUES (2, 200, 'two', '3.75', 7)");
    }

    std::string Run(const std::string& line) { return dispatcher_->Dispatch(line).response; }
    void Exec(const std::string& line) {
        const std::string out = Run(line);
        ASSERT_NE(out.substr(0, 3), "ERR") << line << " -> " << out;
    }
    std::string At(const std::string& column, int id) {
        const std::string out = Run("SELECT " + column + " FROM t WHERE id = " + std::to_string(id));
        const std::string header = column + "\\n";
        return out.rfind(header, 0) == 0 ? out.substr(header.size()) : out;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

// A refusal from inside the subquery names the token that failed, once: the
// subquery's own byte is added only to a message that carries none.
TEST_F(SetExprSubqueryTest, ARefusalInsideTheSubqueryCarriesOneBytePosition) {
    const std::string out = Run("UPDATE t SET v = (SELECT nope FROM u) WHERE id = 1");
    ASSERT_EQ(out.rfind("ERR", 0), 0u) << out;
    const std::size_t first = out.find("byte ");
    ASSERT_NE(first, std::string::npos) << out;
    EXPECT_EQ(out.find("byte ", first + 1), std::string::npos) << out;
    EXPECT_NE(out.find("byte 25"), std::string::npos) << "the byte of `nope`: " << out;
}

TEST_F(SetExprSubqueryTest, EveryRowIsRaisedByThePreStatementAggregate) {
    // BJ-Q6's cell. A per-row subquery would see the rows already raised, and
    // the sums would grow: 70, then 70 + 10 + 70 ... Evaluated once, it is 60.
    EXPECT_EQ(Run("UPDATE t SET v = v + (SELECT SUM(v) FROM t)").substr(0, 9), "UPDATED 3");
    EXPECT_EQ(At("v", 1), "70");
    EXPECT_EQ(At("v", 2), "80");
    EXPECT_EQ(At("v", 3), "90");
}

TEST_F(SetExprSubqueryTest, ASubqueryOverAnotherRelationAndAnAggregateOfIt) {
    Exec("UPDATE t SET v = (SELECT MAX(x) FROM u) WHERE id = 1");
    EXPECT_EQ(At("v", 1), "200");
    Exec("UPDATE t SET v = (SELECT x FROM u WHERE id = 1) + 1 WHERE id = 2");
    EXPECT_EQ(At("v", 2), "101");
    Exec("UPDATE t SET v = v * (SELECT COUNT(*) FROM u) + 1 WHERE id = 3");
    EXPECT_EQ(At("v", 3), "61");
    Exec("UPDATE t SET name = (SELECT s FROM u WHERE id = 2) || '!' WHERE id = 1");
    EXPECT_EQ(At("name", 1), "two!");
    Exec("UPDATE t SET d = (SELECT dd FROM u WHERE id = 2) + 1.00 WHERE id = 1");
    EXPECT_EQ(At("d", 1), "4.75");
    Exec("UPDATE t SET v = CASE WHEN (SELECT COUNT(*) FROM u) > 1 THEN 1 ELSE 0 END WHERE id = 2");
    EXPECT_EQ(At("v", 2), "1");
}

TEST_F(SetExprSubqueryTest, NoRowIsNullAndAnotherColumnsNullIsNull) {
    Exec("UPDATE t SET n = (SELECT x FROM u WHERE id = 99) WHERE id = 1");
    EXPECT_EQ(At("n", 1), "NULL");
    Exec("UPDATE t SET n = 5 WHERE id = 1");
    Exec("UPDATE t SET n = (SELECT y FROM u WHERE id = 1) WHERE id = 1");
    EXPECT_EQ(At("n", 1), "NULL");
    Exec("UPDATE t SET n = (SELECT y FROM u WHERE id = 2) WHERE id = 1");
    EXPECT_EQ(At("n", 1), "7");
    // A NULL into a NOT NULL column is the row's refusal, as for any NULL.
    const std::string out = Run("UPDATE t SET v = (SELECT x FROM u WHERE id = 99) WHERE id = 1");
    EXPECT_EQ(out.substr(0, 3), "ERR") << out;
    EXPECT_NE(out.find("'v'"), std::string::npos) << out;
    EXPECT_EQ(At("v", 1), "10");
}

TEST_F(SetExprSubqueryTest, MoreThanOneRowOrColumnIsRefusedWhetherOrNotARowMatches) {
    // The answer is taken before the walk, so the statement fails even over a
    // WHERE that matches nothing: a deliberate difference from a per-row
    // evaluation, which would only fail when a row asked.
    const std::string rows = Run("UPDATE t SET v = (SELECT x FROM u) WHERE id = 999");
    EXPECT_NE(rows.find("more than one row returned by a subquery"), std::string::npos) << rows;
    const std::string columns =
        Run("UPDATE t SET v = (SELECT id, x FROM u WHERE id = 1) WHERE id = 1");
    EXPECT_NE(columns.find("must return one column"), std::string::npos) << columns;
    EXPECT_EQ(At("v", 1), "10");
}

TEST_F(SetExprSubqueryTest, ASubqueryThatReadsTheRowBeingUpdatedIsRefused) {
    const std::string sql = "UPDATE t SET v = (SELECT x FROM u WHERE u.id = t.id) WHERE id = 1";
    const std::string qualified = Run(sql);
    EXPECT_NE(qualified.find("NOT_IMPLEMENTED"), std::string::npos) << qualified;
    EXPECT_NE(qualified.find("BJ-Q6"), std::string::npos) << qualified;
    EXPECT_NE(qualified.find("byte " + std::to_string(sql.find("(SELECT"))), std::string::npos)
        << qualified;
    // `v` is not a column of `u`, so it resolves outward: correlated.
    const std::string unqualified = Run("UPDATE t SET v = (SELECT x FROM u WHERE id = v)");
    EXPECT_NE(unqualified.find("NOT_IMPLEMENTED"), std::string::npos) << unqualified;
    EXPECT_EQ(At("v", 1), "10");
    // A subquery over the same relation that names only its own columns is
    // not correlated, whatever the outer row is.
    Exec("UPDATE t SET v = (SELECT MIN(v) FROM t WHERE id > 1) WHERE id = 1");
    EXPECT_EQ(At("v", 1), "20");
}

TEST_F(SetExprSubqueryTest, TheAnswerHasItsColumnsTypeAndTheAssignmentNeedsIt) {
    const std::string out = Run("UPDATE t SET d = (SELECT x FROM u WHERE id = 1)");
    EXPECT_NE(out.find("the expression is int64 but column 'd' is decimal(10,2)"),
              std::string::npos)
        << out;
    EXPECT_NE(Run("UPDATE t SET v = (SELECT s FROM u WHERE id = 1)")
                  .find("is varchar but column 'v' is int64"),
              std::string::npos);
    EXPECT_NE(Run("UPDATE t SET v = (SELECT dd FROM u WHERE id = 1) + 1")
                  .find("is decimal(10,2) but column 'v' is int64"),
              std::string::npos);
}

// An extreme of a decimal column is one of its values, so it is typed in the
// column's own (p, s) and assigns back to a column of that type; a SUM widens
// to the type's maximum precision and does not.
TEST_F(SetExprSubqueryTest, ADecimalExtremeHasItsColumnsPrecisionAndASumDoesNot) {
    Exec("UPDATE t SET d = (SELECT MAX(dd) FROM u) WHERE id = 1");
    EXPECT_EQ(At("d", 1), "3.75");
    Exec("UPDATE t SET d = (SELECT MIN(d) FROM t) - (SELECT MIN(dd) FROM u) WHERE id = 2");
    EXPECT_EQ(At("d", 2), "-0.75");
    EXPECT_NE(Run("UPDATE t SET d = (SELECT SUM(dd) FROM u) WHERE id = 1")
                  .find("the expression is decimal(18,2) but column 'd' is decimal(10,2)"),
              std::string::npos);
    Exec("CREATE TABLE w (id int64, m decimal(12,2))");
    Exec("INSERT INTO w VALUES (1, '4.00')");
    EXPECT_NE(Run("UPDATE t SET d = (SELECT MAX(m) FROM w) WHERE id = 1")
                  .find("the expression is decimal(12,2) but column 'd' is decimal(10,2)"),
              std::string::npos);
}

TEST_F(SetExprSubqueryTest, ASubqueryTheStatementCannotRunIsRefusedWithItsOwnReason) {
    const std::string missing_sql = "UPDATE t SET v = (SELECT nosuch FROM u WHERE id = 1)";
    const std::string missing = Run(missing_sql);
    EXPECT_EQ(missing.substr(0, 3), "ERR") << missing;
    EXPECT_NE(missing.find("nosuch"), std::string::npos) << "the subquery's own refusal: " << missing;
    EXPECT_NE(missing.find("byte " + std::to_string(missing_sql.find("nosuch"))), std::string::npos)
        << "a byte of the UPDATE, not of the subquery's text: " << missing;
    EXPECT_EQ(Run("UPDATE t SET v = (SELECT x FROM nosuch)").substr(0, 3), "ERR");
    EXPECT_NE(Run("UPDATE t SET v = (SELECT oid FROM sys.tables WHERE oid = 1)")
                  .find("catalog view"),
              std::string::npos);
    EXPECT_EQ(Run("UPDATE t SET (v, n) = (SELECT x, y FROM u WHERE id = 2)").substr(0, 3), "ERR");
}

TEST_F(SetExprSubqueryTest, AnOrderedAndPaginatedSubqueryAnswersTheRowItEmits) {
    // BJ-S7's review: the sorted path renders rows as the top-N heap admits
    // them and emits the survivors afterwards, so "the last row rendered" was
    // not the row emitted - OFFSET 1 stored the third value, not the second.
    Exec("INSERT INTO u VALUES (3, 50, 'three', '1.00', 1)");
    EXPECT_EQ(Run("SELECT x FROM u ORDER BY x LIMIT 1 OFFSET 1"), "x\\n100");
    Exec("UPDATE t SET v = (SELECT x FROM u ORDER BY x LIMIT 1 OFFSET 1) WHERE id = 1");
    EXPECT_EQ(At("v", 1), "100");
    Exec("UPDATE t SET v = (SELECT x FROM u ORDER BY x DESC LIMIT 1) WHERE id = 1");
    EXPECT_EQ(At("v", 1), "200");
    Exec("UPDATE t SET v = (SELECT x FROM u ORDER BY x DESC LIMIT 1 OFFSET 2) WHERE id = 1");
    EXPECT_EQ(At("v", 1), "50");
}

TEST_F(SetExprSubqueryTest, CorrelationIsJudgedByWhatTheSubqueryNeedsFromOutside) {
    // An aggregated or paginated subquery cannot be text-probed; it is still
    // refused as correlated, and by the right code.
    EXPECT_NE(Run("UPDATE t SET v = (SELECT MAX(x) FROM u WHERE u.id = t.id)")
                  .find("NOT_IMPLEMENTED"),
              std::string::npos);
    EXPECT_NE(Run("UPDATE t SET v = (SELECT x FROM u WHERE id = t.id ORDER BY x LIMIT 1)")
                  .find("NOT_IMPLEMENTED"),
              std::string::npos);
    // A nested subquery that correlates only with the subquery's own relation
    // reads nothing of the row being updated.
    Exec("UPDATE t SET v = (SELECT x FROM u WHERE EXISTS (SELECT id FROM u AS w WHERE w.id = "
         "u.id) AND id = 2) WHERE id = 1");
    EXPECT_EQ(At("v", 1), "200");
}

TEST_F(SetExprSubqueryTest, TheSubqueryEndsAtItsOwnParenthesisAndOnlyThere) {
    // `;` ends a statement, not a subquery inside one.
    EXPECT_EQ(Run("UPDATE t SET v = (SELECT x FROM u WHERE id = 2;)").substr(0, 3), "ERR");
    // A parenthesis in a string literal or after a comment marker is text.
    Exec("UPDATE t SET name = (SELECT s FROM u WHERE s != ')x(' AND id = 1) WHERE id = 1");
    EXPECT_EQ(At("name", 1), "one");
    Exec("UPDATE t SET v = (SELECT x FROM u WHERE id = 2 -- )\n) WHERE id = 1");
    EXPECT_EQ(At("v", 1), "200");
}

TEST_F(SetExprSubqueryTest, ASubqueryInABranchNeverTakenStillRunsAndItsErrorStands) {
    // The answer is taken before the walk, whatever branch the row would take.
    const std::string out =
        Run("UPDATE t SET v = CASE WHEN 1 = 1 THEN 5 ELSE (SELECT x FROM u) END WHERE id = 1");
    EXPECT_NE(out.find("more than one row returned by a subquery"), std::string::npos) << out;
    EXPECT_EQ(At("v", 1), "10");
    Exec("UPDATE t SET v = COALESCE(5, (SELECT x FROM u WHERE id = 1)) WHERE id = 1");
    EXPECT_EQ(At("v", 1), "5");
}

TEST_F(SetExprSubqueryTest, ARepeatableReadStatementReadsTheSubqueryAtItsOwnView) {
    auto opened = TwoCoreRig::Open();
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto rig = std::move(opened.value());
    CommandDispatcher& d = rig->core(0).dispatcher();
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int64)").response.substr(0, 3), "CRE");
    ASSERT_EQ(d.Dispatch("CREATE TABLE u (id int64, x int64)").response.substr(0, 3), "CRE");
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (1, 1)").response.substr(0, 8), "INSERTED");
    ASSERT_EQ(d.Dispatch("INSERT INTO u VALUES (1, 5)").response.substr(0, 8), "INSERTED");

    Session reader;
    ASSERT_EQ(d.Dispatch("SET ISOLATION LEVEL REPEATABLE READ", &reader).response.substr(0, 3),
              "SET");
    ASSERT_EQ(d.Dispatch("BEGIN", &reader).response.substr(0, 5), "BEGIN");
    ASSERT_EQ(d.Dispatch("SELECT x FROM u WHERE id = 1", &reader).response, "x\\n5");
    // Another session commits a change after the reader's view was taken.
    ASSERT_EQ(d.Dispatch("UPDATE u SET x = 999 WHERE id = 1").response.substr(0, 9), "UPDATED 1");
    ASSERT_EQ(d.Dispatch("UPDATE t SET v = (SELECT x FROM u WHERE id = 1) WHERE id = 1", &reader)
                  .response.substr(0, 9),
              "UPDATED 1");
    EXPECT_EQ(d.Dispatch("SELECT v FROM t WHERE id = 1", &reader).response, "v\\n5")
        << "the subquery read the transaction's view, not the latest commit";
    ASSERT_EQ(d.Dispatch("ROLLBACK", &reader).response.substr(0, 8), "ROLLBACK");
}

TEST_F(SetExprSubqueryTest, AConstantSubqueryIntoAForeignKeyIsCheckedAgainstItsValue) {
    Exec("CREATE TABLE parent (id int64, name varchar)");
    Exec("INSERT INTO parent VALUES (1, 'a')");
    Exec("INSERT INTO parent VALUES (2, 'b')");
    Exec("CREATE TABLE child (id int64, pid int64 REFERENCES parent, v int64)");
    Exec("INSERT INTO child VALUES (1, 1, 10)");
    Exec("UPDATE child SET pid = (SELECT MAX(id) FROM parent) WHERE id = 1");
    EXPECT_EQ(Run("SELECT pid FROM child WHERE id = 1"), "pid\\n2");
    const std::string absent = Run("UPDATE child SET pid = (SELECT MAX(x) FROM u) WHERE id = 1");
    EXPECT_NE(absent.find("FK_VIOLATION"), std::string::npos) << absent;
    EXPECT_EQ(Run("SELECT pid FROM child WHERE id = 1"), "pid\\n2");
}

TEST_F(SetExprSubqueryTest, ASubqueryInsideATransactionSeesTheTransactionsOwnWrites) {
    auto opened = TwoCoreRig::Open();
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto rig = std::move(opened.value());
    CommandDispatcher& d = rig->core(0).dispatcher();
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int64)").response.substr(0, 3), "CRE");
    ASSERT_EQ(d.Dispatch("CREATE TABLE u (id int64, x int64)").response.substr(0, 3), "CRE");
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (1, 1)").response.substr(0, 8), "INSERTED");
    ASSERT_EQ(d.Dispatch("INSERT INTO u VALUES (1, 5)").response.substr(0, 8), "INSERTED");

    Session session;
    ASSERT_EQ(d.Dispatch("BEGIN", &session).response.substr(0, 5), "BEGIN");
    ASSERT_EQ(d.Dispatch("UPDATE u SET x = 9 WHERE id = 1", &session).response.substr(0, 9),
              "UPDATED 1");
    ASSERT_EQ(d.Dispatch("UPDATE t SET v = (SELECT x FROM u WHERE id = 1) WHERE id = 1", &session)
                  .response.substr(0, 9),
              "UPDATED 1");
    EXPECT_EQ(d.Dispatch("SELECT v FROM t WHERE id = 1", &session).response, "v\\n9");
    ASSERT_EQ(d.Dispatch("ROLLBACK", &session).response.substr(0, 8), "ROLLBACK");
    EXPECT_EQ(d.Dispatch("SELECT v FROM t WHERE id = 1").response, "v\\n1");
}

}  // namespace
}  // namespace kds::server
