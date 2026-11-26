#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "sim/instance.hpp"
#include "view_text.hpp"

// DR (`instructions/v3.2.0/workorder-dr-primary-key-over-keystone.md`): a
// declared primary key that is not the Keystone is a name the engine
// resolves to the row's identity. The row's identity is its Keystone
// (DR-R2); the primary key is a unique key over it, of several types and
// columns. These cells pin that contract on a heap (`heap-and-tuple.md`
// §3.1b): the key is unique on full values, its value may move while the
// Keystone stays (P3), and a statement naming the whole key is answered
// through the key's index.

namespace kds::server {
namespace {

using Table = std::vector<std::vector<std::string>>;

class PrimaryKeyTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto created = sim::SimInstance::Create();
        ASSERT_TRUE(created.ok()) << created.status().message();
        db_ = std::move(created.value());
    }

    std::string Run(const std::string& sql) { return db_->Execute(sql); }

    testing_view::ViewText Rows(const std::string& sql) {
        return testing_view::ReadViewText(Run(sql));
    }

    // The statement's ANALYZE, whole: its plan and counters.
    std::string Plan(const std::string& select) { return Run("ANALYZE " + select); }

    void Ok(const std::string& sql, const std::string& tag) {
        const std::string reply = Run(sql);
        ASSERT_EQ(reply.rfind(tag, 0), 0u) << sql << " -> " << reply;
    }

    std::unique_ptr<sim::SimInstance> db_;
};

// ---- The key is unique, on full values (DR-R4) ------------------------------

TEST_F(PrimaryKeyTest, ACompositeKeyRefusesItsDuplicateAndNamesIt) {
    Ok("CREATE TABLE h (a int32, b int32, v int64, PRIMARY KEY (a, b)) HEAP", "CREATED");
    Ok("INSERT INTO h VALUES (1, 1, 10), (1, 2, 20), (2, 1, 30)", "INSERTED");
    const std::string twice = Run("INSERT INTO h VALUES (1, 2, 99)");
    EXPECT_EQ(twice.rfind("ERR", 0), 0u) << twice;
    EXPECT_NE(twice.find("on (a, b): (1, 2) is already held"), std::string::npos) << twice;
    EXPECT_EQ(Rows("SELECT a, b, v FROM h").rows.size(), 3u);
}

TEST_F(PrimaryKeyTest, StringKeysSharingTheirIndexedPrefixAreTwoKeys) {
    Ok("CREATE TABLE s (k varchar(64), v int64, PRIMARY KEY (k)) HEAP", "CREATED");
    // 40 bytes alike, then one byte apart: the index orders by 32 (IX6).
    const std::string stem(40, 'x');
    Ok("INSERT INTO s VALUES ('" + stem + "a', 1)", "INSERTED");
    Ok("INSERT INTO s VALUES ('" + stem + "b', 2)", "INSERTED");
    const std::string twice = Run("INSERT INTO s VALUES ('" + stem + "a', 3)");
    EXPECT_EQ(twice.rfind("ERR", 0), 0u) << twice;
    EXPECT_NE(twice.find("is already held"), std::string::npos) << twice;
    EXPECT_EQ(Rows("SELECT v FROM s WHERE k = '" + stem + "b'").rows, (Table{{"2"}}));
    EXPECT_EQ(Rows("SELECT v FROM s WHERE k = '" + stem + "a'").rows, (Table{{"1"}}));
}

TEST_F(PrimaryKeyTest, AKeyColumnIsNeverNullAndARelationHasOneKey) {
    const std::string null_key =
        Run("CREATE TABLE n (a int32 NULL, v int64, PRIMARY KEY (a)) HEAP");
    EXPECT_EQ(null_key.rfind("ERR", 0), 0u) << null_key;
    EXPECT_NE(null_key.find("a primary key is never NULL"), std::string::npos) << null_key;
    const std::string two = Run("CREATE TABLE n (a int32 PRIMARY KEY, b int32 PRIMARY KEY) HEAP");
    EXPECT_EQ(two.rfind("ERR", 0), 0u) << two;
    EXPECT_NE(two.find("one primary key"), std::string::npos) << two;
}

// ---- The key's value may move; the Keystone does not (P3, DR-Q1 (a)) --------

TEST_F(PrimaryKeyTest, AKeyValueMovesAndTheRowKeepsItsKeystone) {
    Ok("CREATE TABLE h (a int32, b int32, v int64, PRIMARY KEY (a, b)) HEAP", "CREATED");
    Ok("INSERT INTO h VALUES (1, 1, 10), (1, 2, 20)", "INSERTED");
    const Table before = Rows("SELECT keyidentity FROM h WHERE a = 1 AND b = 1").rows;
    ASSERT_EQ(before.size(), 1u);

    Ok("UPDATE h SET b = 7 WHERE a = 1 AND b = 1", "UPDATED 1");
    // Found under the new value, by the same Keystone; gone under the old.
    EXPECT_EQ(Rows("SELECT keyidentity FROM h WHERE a = 1 AND b = 7").rows, before);
    EXPECT_EQ(Rows("SELECT v FROM h WHERE a = 1 AND b = 7").rows, (Table{{"10"}}));
    EXPECT_TRUE(Rows("SELECT v FROM h WHERE a = 1 AND b = 1").rows.empty());
    // The old value is free at once: a key is a value, not an identity.
    Ok("INSERT INTO h VALUES (1, 1, 30)", "INSERTED");
    EXPECT_EQ(Rows("SELECT v FROM h WHERE a = 1 AND b = 1").rows, (Table{{"30"}}));
}

TEST_F(PrimaryKeyTest, AKeyMovedOntoALiveKeyIsRefusedAndNothingMoves) {
    Ok("CREATE TABLE h (a int32, b int32, v int64, PRIMARY KEY (a, b)) HEAP", "CREATED");
    Ok("INSERT INTO h VALUES (1, 1, 10), (1, 2, 20)", "INSERTED");
    const std::string onto = Run("UPDATE h SET b = 2 WHERE a = 1 AND b = 1");
    EXPECT_EQ(onto.rfind("ERR", 0), 0u) << onto;
    EXPECT_NE(onto.find("(1, 2) is already held"), std::string::npos) << onto;
    EXPECT_EQ(Rows("SELECT b, v FROM h").rows, (Table{{"1", "10"}, {"2", "20"}}));
}

TEST_F(PrimaryKeyTest, AMovedKeyRollsBackToItsOldValue) {
    Ok("CREATE TABLE h (a int32, v int64, PRIMARY KEY (a)) HEAP", "CREATED");
    Ok("INSERT INTO h VALUES (1, 10)", "INSERTED");
    Ok("BEGIN", "BEGIN");
    Ok("UPDATE h SET a = 5 WHERE a = 1", "UPDATED 1");
    EXPECT_EQ(Rows("SELECT v FROM h WHERE a = 5").rows, (Table{{"10"}}));
    Ok("ROLLBACK", "ROLLBACK");
    EXPECT_EQ(Rows("SELECT v FROM h WHERE a = 1").rows, (Table{{"10"}}));
    EXPECT_TRUE(Rows("SELECT v FROM h WHERE a = 5").rows.empty());
    // The value the rollback gave up is free.
    Ok("INSERT INTO h VALUES (5, 50)", "INSERTED");
}

TEST_F(PrimaryKeyTest, ADeletedKeyIsFreeForANewRow) {
    Ok("CREATE TABLE h (a int32, v int64, PRIMARY KEY (a)) HEAP", "CREATED");
    Ok("INSERT INTO h VALUES (1, 10)", "INSERTED");
    const Table first = Rows("SELECT keyidentity FROM h WHERE a = 1").rows;
    Ok("DELETE FROM h WHERE a = 1", "DELETED 1");
    Ok("INSERT INTO h VALUES (1, 11)", "INSERTED");
    // The value is bound again, to a new row: a new Keystone (K1).
    const Table second = Rows("SELECT keyidentity FROM h WHERE a = 1").rows;
    ASSERT_EQ(second.size(), 1u);
    EXPECT_NE(second, first);
    EXPECT_EQ(Rows("SELECT v FROM h WHERE a = 1").rows, (Table{{"11"}}));
}

// ---- The key is read through its index --------------------------------------

TEST_F(PrimaryKeyTest, AReadNamingTheWholeKeyProbesItsIndex) {
    Ok("CREATE TABLE h (a int32, d date, n int64, s varchar(16), v int64, "
       "PRIMARY KEY (a, d, n, s)) HEAP",
       "CREATED");
    for (int i = 0; i < 40; ++i) {
        Ok("INSERT INTO h VALUES (" + std::to_string(i % 3) + ", '2026-01-0" +
               std::to_string(1 + i % 5) + "', " + std::to_string(i) + ", 's" +
               std::to_string(i % 4) + "', " + std::to_string(100 + i) + ")",
           "INSERTED");
    }
    const std::string select =
        "SELECT v FROM h WHERE a = 1 AND d = '2026-01-03' AND n = 7 AND s = 's3'";
    EXPECT_EQ(Rows(select).rows, (Table{{"107"}}));
    const std::string plan = Plan(select);
    EXPECT_NE(plan.find("IndexProbe h"), std::string::npos) << plan;
    EXPECT_NE(plan.find("index_scanned=1 index_resolved=1"), std::string::npos) << plan;
}

TEST_F(PrimaryKeyTest, ARangeOverTheKeyIsServedByItsIndex) {
    Ok("CREATE TABLE h (a int32, v int64, PRIMARY KEY (a)) HEAP", "CREATED");
    std::string values;
    for (int i = 1; i <= 400; ++i) {
        values += (i == 1 ? "(" : ", (") + std::to_string(i) + ", " + std::to_string(i * 2) + ")";
    }
    Ok("INSERT INTO h VALUES " + values, "INSERTED");
    const std::string select = "SELECT a FROM h WHERE a >= 100 AND a <= 104 ORDER BY a";
    EXPECT_EQ(Rows(select).rows, (Table{{"100"}, {"101"}, {"102"}, {"103"}, {"104"}}));
    const std::string plan = Plan(select);
    EXPECT_NE(plan.find("IndexRange h"), std::string::npos) << plan;
}

TEST_F(PrimaryKeyTest, TheKeystoneOfAKeyedRowIsReadByName) {
    Ok("CREATE TABLE h (a int32, b int32, v int64, PRIMARY KEY (a, b)) HEAP", "CREATED");
    const std::string returned = Run("INSERT INTO h VALUES (3, 4, 5) RETURNING keyidentity");
    EXPECT_EQ(returned.rfind("keyidentity\\n1", 0), 0u) << returned;
    EXPECT_EQ(Rows("SELECT keyidentity FROM h WHERE a = 3 AND b = 4").rows, (Table{{"1"}}));
    // `*` is the declared columns: the key's, then the rest.
    EXPECT_EQ(Rows("SELECT * FROM h").rows, (Table{{"3", "4", "5"}}));
}

// DR-Q24: `sys.columns` and `DESCRIBE` say which columns are the key, and in
// which order - the one place a declared key is shown as a key.
TEST_F(PrimaryKeyTest, DescribeShowsTheKeysColumnsInKeyOrder) {
    Ok("CREATE TABLE h (v int64, b int32, a int32, PRIMARY KEY (a, b)) HEAP", "CREATED");
    const auto keyed = Rows("DESCRIBE h");
    EXPECT_EQ(keyed.Column("name"), (std::vector<std::string>{"v", "b", "a"}));
    EXPECT_EQ(keyed.Column("primary_key"), (std::vector<std::string>{"0", "2", "1"}));
    EXPECT_EQ(keyed.Column("tuple_key"), (std::vector<std::string>{"0", "0", "0"}));

    // Where the key is the Keystone it is the first column, on either kind.
    Ok("CREATE TABLE d (id int64 PRIMARY KEY, v int64) HEAP", "CREATED");
    EXPECT_EQ(Rows("DESCRIBE d").Column("primary_key"), (std::vector<std::string>{"1", "0"}));
    Ok("CREATE TABLE b (id int64, v int64) BTREE", "CREATED");
    EXPECT_EQ(Rows("DESCRIBE b").Column("primary_key"), (std::vector<std::string>{"1", "0"}));
    // A hidden Keystone and no declared key: no column is one.
    Ok("CREATE TABLE n (x int64, y int64) HEAP", "CREATED");
    EXPECT_EQ(Rows("DESCRIBE n").Column("primary_key"), (std::vector<std::string>{"0", "0"}));
    // A unique index a statement made is not the primary key.
    Ok("CREATE UNIQUE INDEX n_x ON n (x)", "CREATED INDEX");
    EXPECT_EQ(Rows("DESCRIBE n").Column("primary_key"), (std::vector<std::string>{"0", "0"}));
}

// ---- A foreign key to a relation whose Keystone is hidden (DR-S3c, P2a) -----
//
// F1 is unchanged: the child's column holds the parent's Keystone id. What
// DR lifts is the refusal of a parent whose Keystone is hidden, since the
// id is read by name - `RETURNING keyidentity` at the parent's insert and
// `SELECT keyidentity` by its key afterwards.

class KeyedParentTest : public PrimaryKeyTest {
protected:
    void SetUp() override {
        PrimaryKeyTest::SetUp();
        Ok("CREATE TABLE acct (branch int32, no int32, owner varchar(16), "
           "PRIMARY KEY (branch, no)) HEAP",
           "CREATED");
        Ok("CREATE TABLE entry (id int64, acct int64 REFERENCES acct, amount int64) BTREE",
           "CREATED");
    }

    // The Keystone id of the account `(branch, no)`, read by its key.
    std::string AcctId(int branch, int no) {
        const Table found = Rows("SELECT keyidentity FROM acct WHERE branch = " +
                                 std::to_string(branch) + " AND no = " + std::to_string(no))
                                .rows;
        return found.size() == 1 ? found[0][0] : std::string("<none>");
    }
};

TEST_F(KeyedParentTest, AChildNamesItsParentByTheParentsKeyidentity) {
    const std::string made = Run("INSERT INTO acct VALUES (1, 100, 'ann') RETURNING keyidentity");
    // The id the insert returned is the id the key then reads.
    const std::string id = AcctId(1, 100);
    ASSERT_EQ(made.rfind("keyidentity\\n" + id + "\\n", 0), 0u) << made;

    Ok("INSERT INTO entry VALUES (1, " + id + ", 50)", "INSERTED");
    const std::string orphan = Run("INSERT INTO entry VALUES (2, 999, 50)");
    EXPECT_EQ(orphan.rfind("ERR FK_VIOLATION", 0), 0u) << orphan;
    // A child's reference is checked when it is updated, too.
    const std::string moved_off = Run("UPDATE entry SET acct = 999 WHERE id = 1");
    EXPECT_EQ(moved_off.rfind("ERR FK_VIOLATION", 0), 0u) << moved_off;
    EXPECT_EQ(Rows("SELECT id, amount FROM entry").rows, (Table{{"1", "50"}}));
}

TEST_F(KeyedParentTest, AReferencedParentIsNotDeletedAndAFreedOneIs) {
    Ok("INSERT INTO acct VALUES (1, 100, 'ann'), (1, 200, 'bob')", "INSERTED");
    Ok("INSERT INTO entry VALUES (1, " + AcctId(1, 100) + ", 50)", "INSERTED");
    const std::string held = Run("DELETE FROM acct WHERE branch = 1 AND no = 100");
    EXPECT_EQ(held.rfind("ERR FK_VIOLATION", 0), 0u) << held;
    // A parent no child names goes, and so does the first once its child has.
    Ok("DELETE FROM acct WHERE branch = 1 AND no = 200", "DELETED 1");
    Ok("DELETE FROM entry WHERE id = 1", "DELETED 1");
    Ok("DELETE FROM acct WHERE branch = 1 AND no = 100", "DELETED 1");
    EXPECT_TRUE(Rows("SELECT owner FROM acct").rows.empty());
}

// DR-R3: the reference is the Keystone, so a parent's key may move under
// its children, and a key value taken again is another parent.
TEST_F(KeyedParentTest, AParentsKeyMovesUnderItsChildrenAndAReusedValueIsAnotherRow) {
    Ok("INSERT INTO acct VALUES (1, 100, 'ann')", "INSERTED");
    const std::string ann = AcctId(1, 100);
    Ok("INSERT INTO entry VALUES (1, " + ann + ", 50)", "INSERTED");

    Ok("UPDATE acct SET no = 101 WHERE branch = 1 AND no = 100", "UPDATED 1");
    EXPECT_EQ(AcctId(1, 101), ann);
    // The child still names its parent, which still cannot be deleted.
    const std::string held = Run("DELETE FROM acct WHERE branch = 1 AND no = 101");
    EXPECT_EQ(held.rfind("ERR FK_VIOLATION", 0), 0u) << held;
    EXPECT_EQ(Rows("SELECT a.owner FROM entry AS e JOIN acct AS a ON a.keyidentity = e.acct").rows,
              (Table{{"ann"}}));

    // The old value is free, and the row that takes it is a new parent.
    Ok("INSERT INTO acct VALUES (1, 100, 'cy')", "INSERTED");
    EXPECT_NE(AcctId(1, 100), ann);
    Ok("DELETE FROM acct WHERE branch = 1 AND no = 100", "DELETED 1");
}

// ---- A write is found by the key (DR-S3a, DR-Q16) ---------------------------
//
// An UPDATE, a DELETE or a locking read whose WHERE pins every key column of
// a unique index by an equality descends by that key's run instead of
// walking the relation. The claim every cell rests on is CU-R1's: the run
// only picks the rows to look at, and each is tested against the whole
// WHERE, so the statement writes exactly what its walk writes.

class KeyedWriteTest : public PrimaryKeyTest {
protected:
    // How often the indexes whose names start with `like` served a
    // statement (`sys.indexes`' `probes`): what tells a descent from a walk.
    std::uint64_t Probes(const std::string& like = "$pkey_") {
        const auto shown = Rows("SELECT name, probes FROM sys.indexes");
        std::uint64_t total = 0;
        for (const auto& row : shown.rows) {
            if (row[0].rfind(like, 0) == 0) total += std::stoull(row[1]);
        }
        return total;
    }
};

TEST_F(KeyedWriteTest, AnUpdateAndADeleteNamingTheWholeKeyDescendByIt) {
    Ok("CREATE TABLE h (a int32, b int32, v int64, PRIMARY KEY (a, b)) HEAP", "CREATED");
    Ok("INSERT INTO h VALUES (1, 1, 10), (1, 2, 20), (2, 1, 30), (2, 2, 40)", "INSERTED");
    const std::uint64_t before = Probes();

    Ok("UPDATE h SET v = v + 1 WHERE a = 1 AND b = 2", "UPDATED 1");
    EXPECT_EQ(Probes(), before + 1);
    Ok("DELETE FROM h WHERE b = 1 AND a = 2", "DELETED 1");
    EXPECT_EQ(Probes(), before + 2);
    EXPECT_EQ(Rows("SELECT a, b, v FROM h").rows,
              (Table{{"1", "1", "10"}, {"1", "2", "21"}, {"2", "2", "40"}}));

    // A key no row holds, and a key a row held: nothing is written.
    Ok("UPDATE h SET v = 0 WHERE a = 9 AND b = 9", "UPDATED 0");
    Ok("DELETE FROM h WHERE a = 2 AND b = 1", "DELETED 0");
    // Half a key is no key, and since DU-S2 it is an equality prefix of the
    // key's index: the rows are collected through it, one probe. A range
    // on the next column is a range of it (DU-S3), and no probe.
    const std::uint64_t half = Probes();
    Ok("UPDATE h SET v = v + 100 WHERE a = 1", "UPDATED 2");
    EXPECT_EQ(Probes(), half + 1);
    Ok("UPDATE h SET v = v + 100 WHERE a = 2 AND b >= 2 AND b <= 2", "UPDATED 1");
    EXPECT_EQ(Probes(), half + 1);
    EXPECT_EQ(Rows("SELECT a, b, v FROM h").rows,
              (Table{{"1", "1", "110"}, {"1", "2", "121"}, {"2", "2", "140"}}));
}

TEST_F(KeyedWriteTest, TheWholeWhereIsTestedAtTheRowTheKeyFinds) {
    Ok("CREATE TABLE h (a int32, b int32, v int64, PRIMARY KEY (a, b)) HEAP", "CREATED");
    Ok("INSERT INTO h VALUES (1, 1, 10), (1, 2, 20)", "INSERTED");
    // A conjunct beside the key that the row fails, and one it passes.
    Ok("UPDATE h SET v = 0 WHERE a = 1 AND b = 2 AND v = 999", "UPDATED 0");
    Ok("UPDATE h SET v = 21 WHERE a = 1 AND b = 2 AND v = 20", "UPDATED 1");
    // A key column pinned to two values holds of no row.
    Ok("DELETE FROM h WHERE a = 1 AND b = 2 AND b = 1", "DELETED 0");
    // A boolean conjunct beside the key.
    Ok("UPDATE h SET v = 22 WHERE a = 1 AND b = 2 AND (v = 21 OR v = 5)", "UPDATED 1");
    EXPECT_EQ(Rows("SELECT b, v FROM h").rows, (Table{{"1", "10"}, {"2", "22"}}));
}

// DR-R4, `[quiet-wrong]` if the run were trusted: two string keys share
// the 32 bytes the index orders by, so one key's run names both rows, and
// a write by one must leave the other.
TEST_F(KeyedWriteTest, AStringKeysRunNamesItsPrefixAndOnlyTheFullValueIsWritten) {
    Ok("CREATE TABLE s (k varchar(64), v int64, PRIMARY KEY (k)) HEAP", "CREATED");
    const std::string stem(40, 'x');
    Ok("INSERT INTO s VALUES ('" + stem + "a', 1), ('" + stem + "b', 2), ('" + stem + "c', 3)",
       "INSERTED");
    const std::uint64_t before = Probes();
    Ok("UPDATE s SET v = 20 WHERE k = '" + stem + "b'", "UPDATED 1");
    Ok("DELETE FROM s WHERE k = '" + stem + "c'", "DELETED 1");
    Ok("DELETE FROM s WHERE k = '" + stem + "'", "DELETED 0");
    EXPECT_EQ(Probes(), before + 3);
    EXPECT_EQ(Rows("SELECT v FROM s").rows, (Table{{"1"}, {"20"}}));
}

// A moved key leaves its old entry in the index (IX1): the old value's run
// still names the row, which no longer holds it.
TEST_F(KeyedWriteTest, AWriteByAKeyTheRowMovedOffWritesNothing) {
    Ok("CREATE TABLE h (a int32, v int64, PRIMARY KEY (a)) HEAP", "CREATED");
    Ok("INSERT INTO h VALUES (1, 10)", "INSERTED");
    Ok("UPDATE h SET a = 5 WHERE a = 1", "UPDATED 1");
    Ok("UPDATE h SET v = 0 WHERE a = 1", "UPDATED 0");
    Ok("DELETE FROM h WHERE a = 1", "DELETED 0");
    Ok("UPDATE h SET v = 11 WHERE a = 5", "UPDATED 1");
    // The value is taken again by a second row, and each is written by its own.
    Ok("INSERT INTO h VALUES (1, 100)", "INSERTED");
    Ok("UPDATE h SET v = v + 1 WHERE a = 1", "UPDATED 1");
    EXPECT_EQ(Rows("SELECT a, v FROM h").rows, (Table{{"5", "11"}, {"1", "101"}}));
    // A key moved by the key: the statement finds the row by the value it
    // then changes.
    Ok("UPDATE h SET a = 6 WHERE a = 5", "UPDATED 1");
    EXPECT_EQ(Rows("SELECT v FROM h WHERE a = 6").rows, (Table{{"11"}}));
}

TEST_F(KeyedWriteTest, AWriteByKeySeesItsOwnTransactionsRows) {
    Ok("CREATE TABLE h (a int32, v int64, PRIMARY KEY (a)) HEAP", "CREATED");
    Ok("INSERT INTO h VALUES (1, 10)", "INSERTED");
    Ok("BEGIN", "BEGIN");
    Ok("INSERT INTO h VALUES (2, 20)", "INSERTED");
    Ok("UPDATE h SET v = 21 WHERE a = 2", "UPDATED 1");
    Ok("UPDATE h SET v = 11 WHERE a = 1", "UPDATED 1");
    Ok("DELETE FROM h WHERE a = 2", "DELETED 1");
    Ok("UPDATE h SET v = 0 WHERE a = 2", "UPDATED 0");
    Ok("ROLLBACK", "ROLLBACK");
    EXPECT_EQ(Rows("SELECT a, v FROM h").rows, (Table{{"1", "10"}}));
}

TEST_F(KeyedWriteTest, ALockingReadNamingTheWholeKeyDescendsByIt) {
    Ok("CREATE TABLE h (a int32, b int32, v int64, PRIMARY KEY (a, b)) HEAP", "CREATED");
    Ok("INSERT INTO h VALUES (1, 1, 10), (1, 2, 20)", "INSERTED");
    const std::uint64_t before = Probes();
    Ok("BEGIN", "BEGIN");
    EXPECT_EQ(Rows("SELECT a, b, v FROM h WHERE a = 1 AND b = 2 FOR UPDATE").rows,
              (Table{{"1", "2", "20"}}));
    EXPECT_TRUE(Rows("SELECT a, b, v FROM h WHERE a = 1 AND b = 9 FOR UPDATE").rows.empty());
    Ok("COMMIT", "COMMIT");
    EXPECT_EQ(Probes(), before + 2);
}

// Every unique index, a primary key's or not, on either relation kind.
TEST_F(KeyedWriteTest, AnyUniqueIndexNamedWholeIsAKeyAWriteDescendsBy) {
    Ok("CREATE TABLE t (id int64, u int64, w int64, v int64) BTREE", "CREATED");
    Ok("INSERT INTO t VALUES (1, 100, 7, 0), (2, 200, 7, 0), (3, 300, 8, 0)", "INSERTED");
    Ok("CREATE UNIQUE INDEX t_u ON t (u)", "CREATED INDEX");
    Ok("CREATE INDEX t_w ON t (w)", "CREATED INDEX");
    const std::uint64_t unique_before = Probes("t_u");
    Ok("UPDATE t SET v = 1 WHERE u = 200", "UPDATED 1");
    Ok("DELETE FROM t WHERE u = 300", "DELETED 1");
    EXPECT_EQ(Probes("t_u"), unique_before + 2);
    // An index that is not unique is no key, and since DU-S2 it is an arm
    // all the same: the write collects its rows through it.
    const std::uint64_t plain_before = Probes("t_w");
    Ok("UPDATE t SET v = v + 5 WHERE w = 7", "UPDATED 2");
    EXPECT_EQ(Probes("t_w"), plain_before + 1);
    EXPECT_EQ(Rows("SELECT id, v FROM t").rows, (Table{{"1", "5"}, {"2", "6"}}));
    // The tuple key's own arm stands ahead of the key's.
    Ok("UPDATE t SET v = 9 WHERE id = 1 AND u = 100", "UPDATED 1");
    EXPECT_EQ(Probes("t_u"), unique_before + 2);
}

// A value the key's encoder refuses is not known to match no row (CU-Q11's
// reason), so the index is passed over and the walk answers.
TEST_F(KeyedWriteTest, AValueTheKeyCannotEncodeIsAnsweredByTheWalk) {
    Ok("CREATE TABLE h (a int32, v int64, PRIMARY KEY (a)) HEAP", "CREATED");
    Ok("INSERT INTO h VALUES (1, 10)", "INSERTED");
    const std::uint64_t before = Probes();
    const std::string wide = Run("UPDATE h SET v = 0 WHERE a = 5000000000");
    EXPECT_EQ(wide.rfind("UPDATED 0", 0), 0u) << wide;
    EXPECT_EQ(Probes(), before);
    EXPECT_EQ(Rows("SELECT v FROM h").rows, (Table{{"10"}}));
}

// What the statement leaves is what its walk leaves: one instance writes by
// the key, the other the same rows by a WHERE that names no key whole.
TEST(KeyedWriteEquivalenceTest, EveryKeyedWriteLeavesWhatItsWalkLeaves) {
    auto make = [] {
        auto created = sim::SimInstance::Create();
        EXPECT_TRUE(created.ok()) << created.status().message();
        std::unique_ptr<sim::SimInstance> db = std::move(created.value());
        EXPECT_EQ(db->Execute("CREATE TABLE h (a int32, b int32, v int64, s varchar(48), "
                              "PRIMARY KEY (a, b)) HEAP")
                      .rfind("CREATED", 0),
                  0u);
        for (int i = 0; i < 60; ++i) {
            EXPECT_EQ(db->Execute("INSERT INTO h VALUES (" + std::to_string(i % 6) + ", " +
                                  std::to_string(i / 6) + ", " + std::to_string(i) + ", 's" +
                                  std::to_string(i) + "')")
                          .rfind("INSERTED", 0),
                      0u);
        }
        return db;
    };
    std::unique_ptr<sim::SimInstance> keyed = make();
    std::unique_ptr<sim::SimInstance> walked = make();
    struct Case {
        const char* head;
        int a;
        int b;
        const char* tail = "";
    };
    const Case cases[] = {
        {"UPDATE h SET v = v + 1000 WHERE ", 3, 4},
        {"UPDATE h SET s = 'renamed' WHERE ", 0, 0, " AND v < 5"},
        {"UPDATE h SET s = 'never' WHERE ", 1, 1, " AND v > 500"},
        {"UPDATE h SET b = b + 100 WHERE ", 2, 2},   // a key move, found by the key
        {"UPDATE h SET v = 1 WHERE ", 2, 2},         // the old value: no row
        {"UPDATE h SET v = 2 WHERE ", 2, 102},       // the new value
        {"DELETE FROM h WHERE ", 5, 9},
        {"DELETE FROM h WHERE ", 5, 9},              // again: no row
        {"DELETE FROM h WHERE ", 4, 3, " AND s = 'nobody'"},
        {"UPDATE h SET v = v + 1 WHERE ", 77, 77},   // no such key
    };
    for (const Case& c : cases) {
        const std::string a = std::to_string(c.a);
        const std::string b = std::to_string(c.b);
        const std::string by_key =
            std::string(c.head) + "a = " + a + " AND b = " + b + c.tail;
        const std::string by_walk = std::string(c.head) + "a = " + a + " AND b >= " + b +
                                    " AND b <= " + b + c.tail;
        // The walk's instance walks - its key's index serves no write - and
        // the key's descends by it.
        const char* probes = "SELECT probes FROM sys.indexes";
        const std::string walked_before = walked->Execute(probes);
        const std::string keyed_before = keyed->Execute(probes);
        EXPECT_EQ(keyed->Execute(by_key), walked->Execute(by_walk)) << by_key;
        EXPECT_EQ(walked->Execute(probes), walked_before) << by_walk;
        EXPECT_NE(keyed->Execute(probes), keyed_before) << by_key;
        for (const char* read :
             {"SELECT a, b, v, s FROM h", "SELECT v FROM h WHERE a = 2 AND b = 102",
              "SELECT a, b FROM h WHERE a = 5 AND b >= 0 AND b <= 99"}) {
            EXPECT_EQ(keyed->Execute(read), walked->Execute(read)) << by_key << " then " << read;
        }
    }
}

}  // namespace
}  // namespace kds::server
