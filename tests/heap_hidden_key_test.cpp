#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "sim/instance.hpp"
#include "view_text.hpp"

// BZ's heap (`instructions/v3.2.0/workorder-bz-heap-hidden-key.md`): a btree
// relation whose tuple key is the engine's hidden column 0. These cells pin
// BZ-R3's surface - every place a user sees or counts columns sees and counts
// the declared ones only - and BZ-R4/R7's refusals.
//
// **Every relation here has two columns of one type** (`[quiet-wrong]`,
// BZ-R3): a surface that counted the hidden column would shift each value
// one column over, and a column of the same type would take it with no
// error. Two `int64`s make that shift a wrong value the assertion sees.

namespace kds::server {
namespace {

// A whole instance (`sim::SimInstance`): the transaction manager is what a
// multi-row INSERT and RETURNING need, and the crash cell below needs its
// devices.
class HeapHiddenKeyTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto created = sim::SimInstance::Create();
        ASSERT_TRUE(created.ok()) << created.status().message();
        db_ = std::move(created.value());
        ASSERT_EQ(Run("CREATE TABLE h (a int64, b int64) HEAP").substr(0, 7), "CREATED");
    }

    std::string Run(const std::string& sql) { return db_->Execute(sql); }

    testing_view::ViewText Rows(const std::string& sql) {
        return testing_view::ReadViewText(Run(sql));
    }

    std::unique_ptr<sim::SimInstance> db_;
};

TEST_F(HeapHiddenKeyTest, StarShowsTheDeclaredColumnsInTheirOrder) {
    ASSERT_EQ(Run("INSERT INTO h VALUES (10, 20)").substr(0, 8), "INSERTED");
    const auto shown = Rows("SELECT * FROM h");
    EXPECT_EQ(shown.columns, (std::vector<std::string>{"a", "b"}));
    ASSERT_EQ(shown.rows.size(), 1u);
    EXPECT_EQ(shown.rows[0], (std::vector<std::string>{"10", "20"}));
}

TEST_F(HeapHiddenKeyTest, APositionalRowCarriesExactlyTheDeclaredColumns) {
    // One value per declared column: the key is always issued.
    ASSERT_EQ(Run("INSERT INTO h VALUES (1, 2), (3, 4)").substr(0, 8), "INSERTED");
    // A row one longer would read its first value as the key on a btree with
    // a declared one; on a heap no length names the key.
    const std::string longer = Run("INSERT INTO h VALUES (9, 1, 2)");
    EXPECT_EQ(longer.rfind("ERR", 0), 0u) << longer;
    EXPECT_NE(longer.find("expected 2 value(s), one per column; got 3"), std::string::npos)
        << longer;
    EXPECT_EQ(Run("INSERT INTO h VALUES (5)").rfind("ERR", 0), 0u);
    const auto shown = Rows("SELECT a, b FROM h");
    EXPECT_EQ(shown.rows, (std::vector<std::vector<std::string>>{{"1", "2"}, {"3", "4"}}));
}

TEST_F(HeapHiddenKeyTest, AColumnListAndInsertSelectLandEachValueInItsColumn) {
    ASSERT_EQ(Run("INSERT INTO h (b, a) VALUES (2, 1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO h SELECT b, a FROM h").substr(0, 8), "INSERTED");
    const auto shown = Rows("SELECT * FROM h");
    EXPECT_EQ(shown.rows, (std::vector<std::vector<std::string>>{{"1", "2"}, {"2", "1"}}));
}

TEST_F(HeapHiddenKeyTest, ReturningStarAndTheQueryLayerStarShowTheDeclaredColumns) {
    const std::string returned = Run("INSERT INTO h VALUES (7, 8) RETURNING *");
    EXPECT_EQ(returned.rfind("a,b\\n7,8", 0), 0u) << returned;
    // `ORDER BY` an expression is the query layer's (BU-S4), whose `*` is
    // built from the walk's columns.
    ASSERT_EQ(Run("INSERT INTO h VALUES (1, 9)").substr(0, 8), "INSERTED");
    const auto shown = Rows("SELECT * FROM h ORDER BY a + 0");
    EXPECT_EQ(shown.columns, (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(shown.rows, (std::vector<std::vector<std::string>>{{"1", "9"}, {"7", "8"}}));
}

TEST_F(HeapHiddenKeyTest, DescribeAndTheCatalogViewsShowNoHiddenColumn) {
    const auto described = Rows("DESCRIBE h");
    EXPECT_EQ(described.Column("name"), (std::vector<std::string>{"a", "b"}));
    // Neither declared column is the tuple key.
    EXPECT_EQ(described.Column("tuple_key"), (std::vector<std::string>{"0", "0"}));
    const auto tables = Rows("SELECT name, clustered_type FROM sys.tables");
    EXPECT_EQ(tables.Find("name", "h", "clustered_type"), "HEAP");
}

TEST_F(HeapHiddenKeyTest, TheFirstDeclaredColumnIsAnOrdinaryOne) {
    // Any type, and nullable: it is no tuple key.
    ASSERT_EQ(Run("CREATE TABLE g (name varchar, n int64 NULL) HEAP").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("CREATE TABLE g2 (n int64 NULL, m int64) HEAP").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO g VALUES ('x', NULL)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO g2 VALUES (NULL, 4)").substr(0, 8), "INSERTED");
    EXPECT_EQ(Rows("SELECT * FROM g2").rows, (std::vector<std::vector<std::string>>{{"NULL", "4"}}));
    // And it updates, which a tuple key never does.
    ASSERT_EQ(Run("INSERT INTO h VALUES (1, 2)").substr(0, 8), "INSERTED");
    EXPECT_EQ(Run("UPDATE h SET a = 5 WHERE a = 1").substr(0, 7), "UPDATED");
    EXPECT_EQ(Rows("SELECT * FROM h").rows, (std::vector<std::vector<std::string>>{{"5", "2"}}));
    EXPECT_EQ(Run("DELETE FROM h WHERE b = 2").substr(0, 7), "DELETED");
    EXPECT_TRUE(Rows("SELECT * FROM h").rows.empty());
}

TEST_F(HeapHiddenKeyTest, NoStatementNamesTheHiddenKey) {
    // `$tuple_key` lexes as a parameter, refused everywhere.
    EXPECT_EQ(Run("SELECT $tuple_key FROM h").rfind("ERR", 0), 0u);
    // PURGE's window is written in tuple key comparisons (BZ-R4).
    const std::string purge = Run("PURGE FROM h WHERE a < 10");
    EXPECT_EQ(purge.rfind("ERR UNSUPPORTED", 0), 0u) << purge;
    EXPECT_NE(purge.find("tuple key is hidden"), std::string::npos) << purge;
}

TEST_F(HeapHiddenKeyTest, AHeapIsNoForeignKeyParentButMayBeAChild) {
    const std::string parent = Run("CREATE TABLE c (id int64, p int64 REFERENCES h)");
    EXPECT_EQ(parent.rfind("ERR", 0), 0u) << parent;
    EXPECT_NE(parent.find("is a heap"), std::string::npos) << parent;

    ASSERT_EQ(Run("CREATE TABLE acct (id int64, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO acct VALUES (1, 0)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("CREATE TABLE ledger (acct_id int64 REFERENCES acct, n int64) HEAP")
                  .substr(0, 7),
              "CREATED");
    EXPECT_EQ(Run("INSERT INTO ledger VALUES (1, 5)").substr(0, 8), "INSERTED");
    const std::string orphan = Run("INSERT INTO ledger VALUES (2, 5)");
    EXPECT_EQ(orphan.rfind("ERR FK_VIOLATION", 0), 0u) << orphan;
}

TEST_F(HeapHiddenKeyTest, ASecondaryIndexOnAHeapServesItsLookup) {
    for (int i = 0; i < 50; ++i) {
        ASSERT_EQ(Run("INSERT INTO h VALUES (" + std::to_string(i) + ", " +
                      std::to_string(i * 2) + ")")
                      .substr(0, 8),
                  "INSERTED");
    }
    const std::string created = Run("CREATE INDEX hb ON h (b)");
    ASSERT_EQ(created.rfind("CREATED", 0), 0u) << created;
    EXPECT_EQ(Rows("SELECT a FROM h WHERE b = 42").rows,
              (std::vector<std::vector<std::string>>{{"21"}}));
}

// BW's ADD and DROP meet BZ-R3 here: a heap's positional row stays the
// declared columns' width after either, a dropped position reads nothing and
// an added one is written where it sits (`[quiet-wrong]` if the count were
// off by one, which two int64s would absorb).
TEST_F(HeapHiddenKeyTest, AddAndDropColumnKeepThePositionalRowTheDeclaredWidth) {
    ASSERT_EQ(Run("CREATE TABLE g (a int64, b int64) SPARE 1 HEAP").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO g VALUES (1, 2)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("ALTER TABLE g ADD COLUMN c int64 NULL").substr(0, 5), "ADDED");
    ASSERT_EQ(Run("INSERT INTO g VALUES (3, 4, 5)").substr(0, 8), "INSERTED");
    // An old-width row cannot be told from a misplaced one, so it is refused.
    EXPECT_EQ(Run("INSERT INTO g VALUES (6, 7)").rfind("ERR", 0), 0u);
    EXPECT_EQ(Rows("SELECT * FROM g").rows,
              (std::vector<std::vector<std::string>>{{"1", "2", "NULL"}, {"3", "4", "5"}}));

    ASSERT_EQ(Run("ALTER TABLE g DROP COLUMN a").substr(0, 7), "DROPPED");
    ASSERT_EQ(Run("INSERT INTO g VALUES (8, 9)").substr(0, 8), "INSERTED");
    const auto shown = Rows("SELECT * FROM g");
    EXPECT_EQ(shown.columns, (std::vector<std::string>{"b", "c"}));
    EXPECT_EQ(shown.rows,
              (std::vector<std::vector<std::string>>{{"2", "NULL"}, {"4", "5"}, {"8", "9"}}));
}

// BZ-Q3: a heap keeps at least one declared column - without it no statement
// could write or read the relation.
TEST_F(HeapHiddenKeyTest, AHeapsLastDeclaredColumnIsNotDropped) {
    ASSERT_EQ(Run("CREATE TABLE one (a int64) HEAP").substr(0, 7), "CREATED");
    const std::string refused = Run("ALTER TABLE one DROP COLUMN a");
    EXPECT_EQ(refused.rfind("ERR", 0), 0u) << refused;
    EXPECT_NE(refused.find("last declared column"), std::string::npos) << refused;
    ASSERT_EQ(Run("ALTER TABLE h DROP COLUMN a").substr(0, 7), "DROPPED");
    EXPECT_EQ(Run("ALTER TABLE h DROP COLUMN b").rfind("ERR", 0), 0u);
}

// ON CONFLICT on a heap: the target-less form is admitted (a heap's issued
// key never collides), and a target that is no unique column is refused
// saying why.
TEST_F(HeapHiddenKeyTest, OnConflictTakesNoTargetOnAHeap) {
    const std::string bare = Run("INSERT INTO h VALUES (1, 2) ON CONFLICT DO NOTHING");
    EXPECT_EQ(bare.rfind("INSERTED", 0), 0u) << bare;
    const std::string named = Run("INSERT INTO h VALUES (1, 2) ON CONFLICT (a) DO NOTHING");
    EXPECT_EQ(named.rfind("ERR", 0), 0u) << named;
    EXPECT_NE(named.find("heap's tuple key is hidden"), std::string::npos) << named;
}

// ON CONFLICT DO UPDATE on a heap (CG-Q1 (b)): the one column of a unique
// index is its arbiter, and no target at all is refused saying why.
TEST_F(HeapHiddenKeyTest, OnConflictDoUpdateArbitratesOnAUniqueColumnOfAHeap) {
    ASSERT_EQ(Run("CREATE TABLE hu (a int64, b int64) HEAP").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("CREATE UNIQUE INDEX hu_a ON hu (a)").rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(Run("INSERT INTO hu VALUES (1, 10)").rfind("INSERTED", 0), 0u);
    const std::string upsert =
        Run("INSERT INTO hu VALUES (1, 5), (2, 7) ON CONFLICT (a) DO UPDATE SET b = b + EXCLUDED.b");
    EXPECT_EQ(upsert.rfind("INSERTED", 0), 0u) << upsert;
    EXPECT_NE(upsert.find(" rows=2"), std::string::npos) << upsert;
    const std::string rows = Run("SELECT a, b FROM hu ORDER BY a");
    EXPECT_NE(rows.find("1,15"), std::string::npos) << rows;
    EXPECT_NE(rows.find("2,7"), std::string::npos) << rows;
    const std::string bare = Run("INSERT INTO hu VALUES (1, 1) ON CONFLICT DO UPDATE SET b = 0");
    EXPECT_EQ(bare.rfind("ERR", 0), 0u) << bare;
    EXPECT_NE(bare.find("names its arbiter"), std::string::npos) << bare;
}

// Rows land in insertion order: the issued key is the leaf order, so a scan
// without ORDER BY reads them as written - across a leaf split, too.
TEST_F(HeapHiddenKeyTest, RowsReadBackInInsertionOrderAcrossLeaves) {
    constexpr int kRows = 1200;  // several leaves of two int64s
    for (int i = 0; i < kRows; ++i) {
        ASSERT_EQ(Run("INSERT INTO h VALUES (" + std::to_string(kRows - i) + ", " +
                      std::to_string(i) + ")")
                      .substr(0, 8),
                  "INSERTED");
    }
    const auto shown = Rows("SELECT b FROM h");
    ASSERT_EQ(shown.rows.size(), static_cast<std::size_t>(kRows));
    for (int i = 0; i < kRows; ++i) EXPECT_EQ(shown.rows[i][0], std::to_string(i));
}

// A crash and a recovered mount: the hidden column is a catalog fact (BZ-R2's
// flag on `sys.columns`), logged with the relation, and the rows are a
// btree's, replayed as any. What a client saw before the crash it sees after.
TEST(HeapHiddenKeyCrashTest, AHeapSurvivesACrashWithItsColumnsAndItsRows) {
    auto created = sim::SimInstance::Create();
    ASSERT_TRUE(created.ok()) << created.status().message();
    std::unique_ptr<sim::SimInstance> instance = std::move(created.value());
    ASSERT_EQ(instance->Execute("CREATE TABLE h (a int64, b int64) HEAP").substr(0, 7), "CREATED");
    for (int i = 0; i < 300; ++i) {
        ASSERT_EQ(instance->Execute("INSERT INTO h VALUES (" + std::to_string(i) + ", " +
                                    std::to_string(-i) + ")")
                      .substr(0, 8),
                  "INSERTED");
    }
    ASSERT_EQ(instance->Execute("SYNC"), "OK synced");
    // Unsynced past this point, so lost to the crash.
    ASSERT_EQ(instance->Execute("SET DURABILITY RELAXED"), "SET durability=relaxed");
    ASSERT_EQ(instance->Execute("INSERT INTO h VALUES (999, 999)").substr(0, 8), "INSERTED");
    const std::string before = instance->Execute("SELECT * FROM h WHERE a < 300");

    instance->Crash();
    ASSERT_TRUE(instance->Reboot().ok());

    EXPECT_EQ(instance->Execute("SELECT * FROM h WHERE a < 300"), before);
    const auto described = testing_view::ReadViewText(instance->Execute("DESCRIBE h"));
    EXPECT_EQ(described.Column("name"), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(instance->Execute("SELECT * FROM h WHERE a = 999"), "a,b");
    // The issue cursor survived too: the next row lands after every one.
    ASSERT_EQ(instance->Execute("INSERT INTO h VALUES (1000, 0)").substr(0, 8), "INSERTED");
    const auto all = testing_view::ReadViewText(instance->Execute("SELECT a FROM h"));
    ASSERT_FALSE(all.rows.empty());
    EXPECT_EQ(all.rows.back()[0], "1000");
}

}  // namespace
}  // namespace kds::server
