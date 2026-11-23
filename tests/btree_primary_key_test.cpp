#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "sim/instance.hpp"
#include "view_text.hpp"

// DR-S3d and DR-S3e (`instructions/v3.2.0/workorder-dr-primary-key-over-
// keystone.md` §2a): a btree carries a declared primary key in one of three
// forms, fixed at `CREATE TABLE` from the declaration alone.
//
//   F0  a single `int64` first column: the relation as it always was, the
//       keyword a spelling of it (DR-R9).
//   F1  a key whose columns' codes total 48 bits or less: the key is the
//       Keystone id (`exec/embedded_key.hpp`), with no index.
//   P1  any other key: a unique index beside a hidden, issued Keystone, as
//       on a heap.
//
// In every form the Keystone is left as it is (DR-R1): a 48-bit id, placed
// where it sorts, never rebound but by `PURGE`.

namespace kds::server {
namespace {

using Table = std::vector<std::vector<std::string>>;

class BtreePrimaryKeyTest : public ::testing::Test {
protected:
    void SetUp() override {
        // With the lock table: `PURGE INDEX` needs its units.
        sim::SimInstanceOptions options;
        options.lock_table = true;
        auto created = sim::SimInstance::Create(options);
        ASSERT_TRUE(created.ok()) << created.status().message();
        db_ = std::move(created.value());
    }

    std::string Run(const std::string& sql) { return db_->Execute(sql); }

    testing_view::ViewText Rows(const std::string& sql) {
        return testing_view::ReadViewText(Run(sql));
    }

    std::string Plan(const std::string& select) { return Run("ANALYZE " + select); }

    void Ok(const std::string& sql, const std::string& tag) {
        const std::string reply = Run(sql);
        ASSERT_EQ(reply.rfind(tag, 0), 0u) << sql << " -> " << reply;
    }

    // The statement is refused, and its refusal says `why`.
    void Refused(const std::string& sql, const std::string& why) {
        const std::string reply = Run(sql);
        EXPECT_EQ(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
        EXPECT_NE(reply.find(why), std::string::npos) << sql << " -> " << reply;
    }

    // The names of the indexes `sys.indexes` lists that begin with `like`.
    std::vector<std::string> Indexes(const std::string& like) {
        std::vector<std::string> out;
        for (const auto& row : Rows("SELECT name FROM sys.indexes").rows) {
            if (row[0].rfind(like, 0) == 0) out.push_back(row[0]);
        }
        return out;
    }

    std::unique_ptr<sim::SimInstance> db_;
};

// ---- F0: the recommended key is today's relation (DR-R9) --------------------

TEST_F(BtreePrimaryKeyTest, TheKeywordOnAnInt64FirstColumnIsTheRelationItAlwaysWas) {
    Ok("CREATE TABLE k (id int64 PRIMARY KEY, v int64) BTREE", "CREATED");
    Ok("CREATE TABLE plain (id int64, v int64) BTREE", "CREATED");
    // No hidden column, no index: the two relations describe alike.
    for (const char* name : {"k", "plain"}) {
        const auto described = Rows(std::string("DESCRIBE ") + name);
        EXPECT_EQ(described.Column("name"), (std::vector<std::string>{"id", "v"})) << name;
        EXPECT_EQ(described.Column("tuple_key"), (std::vector<std::string>{"1", "0"})) << name;
        EXPECT_EQ(described.Column("primary_key"), (std::vector<std::string>{"1", "0"})) << name;
    }
    EXPECT_TRUE(Indexes("$").empty());

    // And they answer alike, statement for statement, plan for plan.
    for (const char* name : {"k", "plain"}) {
        const std::string t(name);
        Ok("INSERT INTO " + t + " VALUES (5, 50), (3, 30)", "INSERTED");
        Ok("INSERT INTO " + t + " (v) VALUES (60)", "INSERTED");  // an issued key
        EXPECT_EQ(Rows("SELECT id, v FROM " + t).rows,
                  (Table{{"3", "30"}, {"5", "50"}, {"6", "60"}}));
        const std::string plan = Plan("SELECT v FROM " + t + " WHERE id = 5");
        EXPECT_NE(plan.find("Lookup " + t + " key=5"), std::string::npos) << plan;
        Refused("UPDATE " + t + " SET id = 9 WHERE id = 5", "tuple key column 'id'");
        Refused("INSERT INTO " + t + " VALUES (5, 0)", "duplicate tuple key");
    }
    EXPECT_EQ(Plan("SELECT v FROM k WHERE id >= 3 AND id <= 5")
                  .substr(Plan("SELECT v FROM k WHERE id >= 3 AND id <= 5").find("step 0")),
              [&] {
                  std::string other = Plan("SELECT v FROM plain WHERE id >= 3 AND id <= 5");
                  other = other.substr(other.find("step 0"));
                  std::string::size_type at = 0;
                  while ((at = other.find("plain", at)) != std::string::npos) {
                      other.replace(at, 5, "k");
                  }
                  return other;
              }());
}

TEST_F(BtreePrimaryKeyTest, AnInt64KeyNotDeclaredFirstIsRefusedAsOnAHeap) {
    Refused("CREATE TABLE k (v int64, id int64 PRIMARY KEY) BTREE", "declared first");
    Refused("CREATE TABLE k (v int64, id int64, PRIMARY KEY (id))", "declared first");
}

// ---- P1: a key that does not embed is a unique index over a hidden Keystone -

class WideKeyTest : public BtreePrimaryKeyTest {
protected:
    void SetUp() override {
        BtreePrimaryKeyTest::SetUp();
        Ok("CREATE TABLE w (k varchar(40), n int64, v int64, PRIMARY KEY (k, n))", "CREATED");
    }
};

TEST_F(WideKeyTest, TheRelationIsABtreeWithAHiddenKeystoneAndTheKeysIndex) {
    EXPECT_EQ(Rows("SELECT name, clustered_type FROM sys.tables").Find("name", "w",
                                                                         "clustered_type"),
              "BTREE");
    EXPECT_EQ(Indexes("$pkey_").size(), 1u);
    EXPECT_TRUE(Indexes("$keystone_").empty());  // a btree's Keystone is its tree
    const auto described = Rows("DESCRIBE w");
    EXPECT_EQ(described.Column("name"), (std::vector<std::string>{"k", "n", "v"}));
    EXPECT_EQ(described.Column("tuple_key"), (std::vector<std::string>{"0", "0", "0"}));
    EXPECT_EQ(described.Column("primary_key"), (std::vector<std::string>{"1", "2", "0"}));

    Ok("INSERT INTO w VALUES ('ann', 1, 10), ('ann', 2, 20), ('bob', 1, 30)", "INSERTED");
    EXPECT_EQ(Rows("SELECT * FROM w").rows,
              (Table{{"ann", "1", "10"}, {"ann", "2", "20"}, {"bob", "1", "30"}}));
    Refused("INSERT INTO w VALUES ('ann', 2, 99)", "is already held");
    // A positional row is the declared columns: the key is issued.
    Refused("INSERT INTO w VALUES (7, 'cy', 1, 40)", "value(s)");
}

TEST_F(WideKeyTest, TheKeyIsReadWrittenAndMovedAsOnAHeap) {
    Ok("INSERT INTO w VALUES ('ann', 1, 10), ('ann', 2, 20), ('bob', 1, 30)", "INSERTED");
    const std::string select = "SELECT v FROM w WHERE k = 'ann' AND n = 2";
    EXPECT_EQ(Rows(select).rows, (Table{{"20"}}));
    EXPECT_NE(Plan(select).find("IndexProbe w"), std::string::npos) << Plan(select);

    const Table id = Rows("SELECT keyidentity FROM w WHERE k = 'ann' AND n = 2").rows;
    Ok("UPDATE w SET v = 21 WHERE k = 'ann' AND n = 2", "UPDATED 1");
    Ok("UPDATE w SET n = 5 WHERE k = 'ann' AND n = 2", "UPDATED 1");
    EXPECT_EQ(Rows("SELECT keyidentity FROM w WHERE k = 'ann' AND n = 5").rows, id);
    EXPECT_TRUE(Rows(select).rows.empty());
    Refused("UPDATE w SET n = 1 WHERE k = 'ann' AND n = 5", "is already held");
    // The value moved off is free at once, and a deleted one too.
    Ok("INSERT INTO w VALUES ('ann', 2, 22)", "INSERTED");
    Ok("DELETE FROM w WHERE k = 'bob' AND n = 1", "DELETED 1");
    Ok("INSERT INTO w VALUES ('bob', 1, 31)", "INSERTED");
    EXPECT_EQ(Rows("SELECT k, n, v FROM w ORDER BY k, n").rows,
              (Table{{"ann", "1", "10"}, {"ann", "2", "22"}, {"ann", "5", "21"},
                     {"bob", "1", "31"}}));
}

// DR-Q19, DR-Q21: such a btree keeps its delete-marked rows and its key's
// dead entries as any btree does, and both are purged by statement.
TEST_F(WideKeyTest, ItsDeletedRowsAndItsKeysDeadEntriesArePurgedByStatement) {
    Ok("INSERT INTO w VALUES ('ann', 1, 10), ('ann', 2, 20), ('bob', 1, 30)", "INSERTED");
    Ok("DELETE FROM w WHERE k = 'ann' AND n = 1", "DELETED 1");
    Ok("UPDATE w SET n = 9 WHERE k = 'bob' AND n = 1", "UPDATED 1");  // one dead entry more
    const std::string purged = Run("PURGE FROM w WHERE keyidentity >= 1");
    EXPECT_EQ(purged.rfind("PURGED 1", 0), 0u) << purged;
    const std::vector<std::string> key = Indexes("$pkey_");
    ASSERT_EQ(key.size(), 1u);
    const std::string index = Run("PURGE INDEX " + key.front());
    EXPECT_EQ(index.rfind("PURGED INDEX", 0), 0u) << index;
    EXPECT_EQ(Rows("SELECT k, n, v FROM w ORDER BY k, n").rows,
              (Table{{"ann", "2", "20"}, {"bob", "9", "30"}}));
    EXPECT_EQ(Rows("SELECT v FROM w WHERE k = 'bob' AND n = 9").rows, (Table{{"30"}}));
    // A window written in the key's columns names no tuple key here.
    Refused("PURGE FROM w WHERE n = 1", "tuple key");
    // A `$name` is a name nowhere else.
    Refused("DROP INDEX " + key.front(), "");
    Refused("SELECT $pkey FROM w", "");
}

TEST_F(WideKeyTest, ItIsAForeignKeysParentByItsKeyidentity) {
    Ok("INSERT INTO w VALUES ('ann', 1, 10)", "INSERTED");
    Ok("CREATE TABLE child (id int64, w_id int64 REFERENCES w)", "CREATED");
    const Table id = Rows("SELECT keyidentity FROM w WHERE k = 'ann' AND n = 1").rows;
    ASSERT_EQ(id.size(), 1u);
    Ok("INSERT INTO child VALUES (1, " + id[0][0] + ")", "INSERTED");
    Refused("INSERT INTO child VALUES (2, 999)", "FK_VIOLATION");
    Refused("DELETE FROM w WHERE k = 'ann' AND n = 1", "FK_VIOLATION");
}

// ---- F1: a key that embeds is the Keystone id -------------------------------

class EmbeddedKeyRelationTest : public BtreePrimaryKeyTest {
protected:
    void SetUp() override {
        BtreePrimaryKeyTest::SetUp();
        Ok("CREATE TABLE e (a int32, b int16, v int64, PRIMARY KEY (a, b))", "CREATED");
    }

    void Fill() {
        // Out of key order, negatives among them.
        Ok("INSERT INTO e VALUES (2, 1, 21), (1, 2, 12), (-3, 7, -37), (1, -5, 15), (1, 1, 11), "
           "(0, 0, 0), (2, -1, 29)",
           "INSERTED");
    }
};

TEST_F(EmbeddedKeyRelationTest, TheKeyIsTheKeystoneIdAndThereIsNoIndex) {
    EXPECT_TRUE(Indexes("$").empty());
    const auto described = Rows("DESCRIBE e");
    EXPECT_EQ(described.Column("name"), (std::vector<std::string>{"a", "b", "v"}));
    EXPECT_EQ(described.Column("primary_key"), (std::vector<std::string>{"1", "2", "0"}));
    EXPECT_EQ(described.Column("tuple_key"), (std::vector<std::string>{"0", "0", "0"}));
    Fill();
    // The id is the key's code plus one (`embedded_key_test.cpp`'s vector).
    EXPECT_EQ(Rows("SELECT keyidentity FROM e WHERE a = 1 AND b = 2").rows,
              (Table{{std::to_string(0x800000018003ull)}}));
    // Leaf order is key order: a walk reads the rows by `(a, b)`.
    EXPECT_EQ(Rows("SELECT a, b FROM e").rows,
              (Table{{"-3", "7"}, {"0", "0"}, {"1", "-5"}, {"1", "1"}, {"1", "2"}, {"2", "-1"},
                     {"2", "1"}}));
}

TEST_F(EmbeddedKeyRelationTest, AWholeKeyIsALookupAndAPrefixOrARangeOfItATupleKeyRange) {
    Fill();
    const std::string point = "SELECT v FROM e WHERE a = 1 AND b = 2";
    EXPECT_EQ(Rows(point).rows, (Table{{"12"}}));
    EXPECT_NE(Plan(point).find("Lookup e key=" + std::to_string(0x800000018003ull)),
              std::string::npos)
        << Plan(point);
    // A prefix, a prefix and a range, a range on the first column.
    for (const char* ranged : {"SELECT b FROM e WHERE a = 1",
                               "SELECT b FROM e WHERE a = 1 AND b >= -5 AND b <= 1",
                               "SELECT b FROM e WHERE a >= 0 AND a <= 1"}) {
        EXPECT_NE(Plan(ranged).find("step 0 Range e"), std::string::npos)
            << ranged << "\n" << Plan(ranged);
    }
    EXPECT_EQ(Rows("SELECT b FROM e WHERE a = 1").rows, (Table{{"-5"}, {"1"}, {"2"}}));
    EXPECT_EQ(Rows("SELECT b FROM e WHERE a = 1 AND b >= -5 AND b <= 1").rows,
              (Table{{"-5"}, {"1"}}));
    EXPECT_EQ(Rows("SELECT a, b FROM e WHERE a >= 0 AND a <= 1").rows,
              (Table{{"0", "0"}, {"1", "-5"}, {"1", "1"}, {"1", "2"}}));
    // A key no row holds, a value the column cannot hold, a bound no arm
    // reads: each answered, by the walk where no id can be told.
    EXPECT_TRUE(Rows("SELECT v FROM e WHERE a = 9 AND b = 9").rows.empty());
    EXPECT_TRUE(Rows("SELECT v FROM e WHERE a = 5000000000 AND b = 1").rows.empty());
    EXPECT_EQ(Rows("SELECT v FROM e WHERE b = 1").rows, (Table{{"11"}, {"21"}}));
    EXPECT_EQ(Rows("SELECT v FROM e WHERE a > 1").rows, (Table{{"29"}, {"21"}}));
}

// `[quiet-wrong]` if the window's arithmetic were off by a column or a sign:
// every read is answered alike by the embedded relation and by a heap with
// no key holding the same rows.
TEST_F(EmbeddedKeyRelationTest, EveryReadAnswersWhatARelationWithNoKeyAnswers) {
    Ok("CREATE TABLE plain (a int32, b int16, v int64) HEAP", "CREATED");
    std::string values;
    for (int a = -4; a <= 4; ++a) {
        for (int b = -3; b <= 3; ++b) {
            if ((a * 7 + b) % 3 == 0) continue;  // gaps
            values += (values.empty() ? "(" : ", (") + std::to_string(a) + ", " +
                      std::to_string(b * 1000) + ", " + std::to_string(a * 100 + b) + ")";
        }
    }
    Ok("INSERT INTO e VALUES " + values, "INSERTED");
    Ok("INSERT INTO plain VALUES " + values, "INSERTED");
    const char* wheres[] = {
        "a = 2 AND b = 1000",       "a = -4 AND b = -3000",     "a = 2 AND b = 7",
        "a = 0",                    "a = -1",                   "a = 4",
        "a = 1 AND b >= -1000 AND b <= 2000",                   "a = -2 AND b >= 0 AND b <= 0",
        "a >= -1 AND a <= 1",       "a >= -4 AND a <= -3",      "a >= 3 AND a <= 99",
        "a >= 2 AND a <= 1",        "a = 1 AND b >= 5 AND b <= -5",
        "a >= -2147483648 AND a <= 2147483647",                 "a = 1 AND b >= -32768 AND b <= 32767",
        "a = 1 AND b = 1000 AND v = 101",                       "a = 1 AND b = 1000 AND v = 0",
        "a = 1 AND a = 2",          "a >= 0 AND a <= 2 AND b = 1000",
        "a = 3 OR a = -3",          "b >= 0 AND b <= 1000",
    };
    for (const char* where : wheres) {
        const std::string tail = std::string(" WHERE ") + where + " ORDER BY a, b";
        EXPECT_EQ(Rows("SELECT a, b, v FROM e" + tail).rows,
                  Rows("SELECT a, b, v FROM plain" + tail).rows)
            << where;
    }
}

TEST_F(EmbeddedKeyRelationTest, AKeyIsUniqueAndStaysBoundUntilAPurge) {
    Fill();
    Refused("INSERT INTO e VALUES (1, 2, 99)", "primary key (a, b): (1, 2) is already held");
    Ok("DELETE FROM e WHERE a = 1 AND b = 2", "DELETED 1");
    // The Keystone id is the key, and a deleted id stays bound (K1).
    Refused("INSERT INTO e VALUES (1, 2, 99)", "until a PURGE frees its key");
    const std::string purged = Run("PURGE FROM e WHERE a = 1 AND b = 2");
    EXPECT_EQ(purged.rfind("PURGED 1", 0), 0u) << purged;
    Ok("INSERT INTO e VALUES (1, 2, 99)", "INSERTED");
    EXPECT_EQ(Rows("SELECT v FROM e WHERE a = 1 AND b = 2").rows, (Table{{"99"}}));
    // The one key a 48-bit form cannot name, and a value past its column.
    Refused("INSERT INTO e VALUES (2147483647, 32767, 0)",
            "the one key this relation cannot hold");
    Refused("INSERT INTO e VALUES (5000000000, 1, 0)", "primary key");
    Ok("INSERT INTO e VALUES (2147483647, 32766, 0)", "INSERTED");
    EXPECT_EQ(Rows("SELECT keyidentity FROM e WHERE a = 2147483647 AND b = 32766").rows,
              (Table{{std::to_string((std::uint64_t{1} << 48) - 1)}}));
}

TEST_F(EmbeddedKeyRelationTest, PurgeTakesTheKeysOwnWindowAndRefusesAnyOther) {
    Fill();
    Ok("DELETE FROM e WHERE a = 1", "DELETED 3");
    Ok("DELETE FROM e WHERE a = 2 AND b = 1", "DELETED 1");
    EXPECT_EQ(Run("PURGE FROM e WHERE a = 1 AND b >= -5 AND b <= 1").rfind("PURGED 2", 0), 0u);
    EXPECT_EQ(Run("PURGE FROM e WHERE a BETWEEN 1 AND 2").rfind("PURGED 2", 0), 0u);
    EXPECT_EQ(Run("PURGE FROM e WHERE a = 7").rfind("PURGED 0", 0), 0u);
    EXPECT_EQ(Run("PURGE FROM e WHERE a = 1 AND b >= 5 AND b <= -5").rfind("PURGED 0", 0), 0u);
    // A conjunct the window does not fold would widen what is freed.
    Refused("PURGE FROM e WHERE v = 12", "NOT_IMPLEMENTED");
    Refused("PURGE FROM e WHERE b = 2", "NOT_IMPLEMENTED");
    Refused("PURGE FROM e WHERE a >= 1", "NOT_IMPLEMENTED");
    Refused("PURGE FROM e WHERE a = 1 AND a = 2", "NOT_IMPLEMENTED");
    Refused("PURGE FROM e WHERE a = 5000000000", "primary key window");
    // And by the id, as any hidden-Keystone btree.
    Ok("DELETE FROM e WHERE a = 0 AND b = 0", "DELETED 1");
    EXPECT_EQ(Run("PURGE FROM e WHERE keyidentity >= 1").rfind("PURGED 1", 0), 0u);
    EXPECT_EQ(Rows("SELECT a, b FROM e").rows, (Table{{"-3", "7"}, {"2", "-1"}}));
}

TEST_F(EmbeddedKeyRelationTest, AWriteByTheWholeKeyDescendsAndAKeyColumnIsNeverAssigned) {
    Fill();
    Ok("UPDATE e SET v = v + 100 WHERE a = 1 AND b = 2", "UPDATED 1");
    Ok("UPDATE e SET v = 0 WHERE a = 1 AND b = 2 AND v = 5", "UPDATED 0");
    Ok("DELETE FROM e WHERE b = 1 AND a = 2", "DELETED 1");
    Ok("UPDATE e SET v = 1 WHERE a = 9 AND b = 9", "UPDATED 0");
    Ok("UPDATE e SET v = -v WHERE a = 1", "UPDATED 3");  // a prefix: the walk
    Ok("BEGIN", "BEGIN");
    EXPECT_EQ(Rows("SELECT a, b, v FROM e WHERE a = 1 AND b = 2 FOR UPDATE").rows,
              (Table{{"1", "2", "-112"}}));
    Ok("COMMIT", "COMMIT");
    EXPECT_EQ(Rows("SELECT a, b, v FROM e WHERE a >= 1 AND a <= 2").rows,
              (Table{{"1", "-5", "-15"}, {"1", "1", "-11"}, {"1", "2", "-112"},
                     {"2", "-1", "29"}}));
    // K2: the key is the row's identity here (DR-Q14).
    Refused("UPDATE e SET b = 3 WHERE a = 1 AND b = 2", "ERR UNSUPPORTED");
    Refused("UPDATE e SET a = 5 WHERE a = 1 AND b = 2", "primary key column 'a'");
}

TEST_F(EmbeddedKeyRelationTest, WhatARowWritesAsItsKeyidentityMustBeItsKeysId) {
    Ok("INSERT INTO e (keyidentity, a, b, v) VALUES (" + std::to_string(0x800000018003ull) +
           ", 1, 2, 12)",
       "INSERTED");
    Refused("INSERT INTO e (keyidentity, a, b, v) VALUES (77, 3, 4, 34)", "is not this row's");
    EXPECT_EQ(Rows("SELECT a, b FROM e").rows, (Table{{"1", "2"}}));
    Refused("INSERT INTO e (a, v) VALUES (1, 0)", "");  // a key column left out
}

TEST_F(EmbeddedKeyRelationTest, WhatIsNotBuiltOnAnEmbeddedKeyIsRefusedByName) {
    Fill();
    Refused("INSERT INTO e VALUES (1, 2, 0) ON CONFLICT DO NOTHING", "NOT_IMPLEMENTED");
    Refused("INSERT INTO e VALUES (1, 2, 0) ON CONFLICT (a) DO UPDATE SET v = 1",
            "NOT_IMPLEMENTED");
    Refused("ALTER TABLE e DROP COLUMN a", "part of the primary key");
    Ok("ALTER TABLE e DROP COLUMN v", "DROPPED");
    EXPECT_EQ(Rows("SELECT b FROM e WHERE a = 1 AND b = 2").rows, (Table{{"2"}}));
}

TEST_F(EmbeddedKeyRelationTest, ADateKeyAndAJoinByTheKey) {
    Ok("CREATE TABLE d (day date, shard int8, v int64, PRIMARY KEY (day, shard))", "CREATED");
    EXPECT_TRUE(Indexes("$").empty());
    Ok("INSERT INTO d VALUES ('2026-01-03', 1, 31), ('2026-01-02', 3, 23), ('2026-01-02', -1, 21)",
       "INSERTED");
    EXPECT_EQ(Rows("SELECT v FROM d WHERE day = '2026-01-02' AND shard = 3").rows,
              (Table{{"23"}}));
    EXPECT_EQ(Rows("SELECT keyidentity FROM d WHERE day = '2026-01-02' AND shard = 3").rows,
              (Table{{std::to_string(((0x80004FE7ull << 8) | 0x83ull) + 1)}}));
    EXPECT_EQ(Rows("SELECT v FROM d WHERE day = '2026-01-02'").rows, (Table{{"21"}, {"23"}}));
    EXPECT_EQ(Rows("SELECT shard FROM d").rows, (Table{{"-1"}, {"3"}, {"1"}}));

    // A join whose key is the embedded key answers, whatever arm serves it.
    Fill();
    Ok("CREATE TABLE want (id int64, a int32, b int16)", "CREATED");
    Ok("INSERT INTO want VALUES (1, 1, 2), (2, 2, -1), (3, 8, 8)", "INSERTED");
    EXPECT_EQ(Rows("SELECT w.id, e.v FROM want AS w JOIN e AS e ON e.a = w.a AND e.b = w.b "
                   "ORDER BY w.id")
                  .rows,
              (Table{{"1", "12"}, {"2", "29"}}));
}

// A primary key has at most four columns, in every form and on either kind:
// what a unique index keys and what an embedded key's catalog row names.
// Refused at the key's byte, before anything is created.
TEST_F(BtreePrimaryKeyTest, AFifthKeyColumnIsRefusedAtTheKeysByte) {
    const std::string cols = "(a int8, b int8, c int8, d int8, e int8, v int64, ";
    for (const char* storage : {"", " HEAP"}) {
        const std::string sql = "CREATE TABLE five " + cols + "PRIMARY KEY (a, b, c, d, e))" +
                                storage;
        const std::string reply = Run(sql);
        EXPECT_EQ(reply.rfind("ERR", 0), 0u) << reply;
        EXPECT_NE(reply.find("at most 4 columns and this names 5 (byte " +
                             std::to_string(sql.find("PRIMARY")) + ")"),
                  std::string::npos)
            << reply;
    }
    // Four one-byte columns embed.
    Ok("CREATE TABLE four (a int8, b int8, c int8, d int8, v int64, PRIMARY KEY (a, b, c, d))",
       "CREATED");
    EXPECT_TRUE(Indexes("$").empty());
    Ok("INSERT INTO four VALUES (1, -2, 3, -4, 9)", "INSERTED");
    EXPECT_EQ(Rows("SELECT v FROM four WHERE a = 1 AND b = -2 AND c = 3 AND d = -4").rows,
              (Table{{"9"}}));
}

// A key that does not embed - too wide, or of a type that never does - takes
// the index, and a heap never embeds: its rows are in no key order.
TEST_F(BtreePrimaryKeyTest, OnlyAKeyThatEmbedsOnABtreeHasNoIndex) {
    Ok("CREATE TABLE fits (a int32, b int16, PRIMARY KEY (a, b))", "CREATED");
    Ok("CREATE TABLE wide (a int32, b int32, PRIMARY KEY (a, b))", "CREATED");
    Ok("CREATE TABLE text (c char(4), v int64, PRIMARY KEY (c))", "CREATED");
    Ok("CREATE TABLE onheap (a int32, b int16, PRIMARY KEY (a, b)) HEAP", "CREATED");
    EXPECT_EQ(Indexes("$pkey_").size(), 3u);
}

// The key form and the hidden column are catalog facts, logged with the
// relation: what a client saw before a crash it sees after, on each form.
TEST(BtreePrimaryKeyCrashTest, EachFormSurvivesACrashWithItsKeyAndItsRows) {
    sim::SimInstanceOptions options;
    options.lock_table = true;
    auto created = sim::SimInstance::Create(options);
    ASSERT_TRUE(created.ok()) << created.status().message();
    std::unique_ptr<sim::SimInstance> db = std::move(created.value());
    const auto ok = [&](const std::string& sql, const char* tag) {
        const std::string reply = db->Execute(sql);
        ASSERT_EQ(reply.rfind(tag, 0), 0u) << sql << " -> " << reply;
    };
    ok("CREATE TABLE e (a int32, b int16, v int64, PRIMARY KEY (a, b))", "CREATED");
    ok("CREATE TABLE w (k varchar(40), n int64, v int64, PRIMARY KEY (k, n))", "CREATED");
    for (int i = 0; i < 400; ++i) {
        const std::string n = std::to_string(i);
        ok("INSERT INTO e VALUES (" + std::to_string(i % 7 - 3) + ", " + n + ", " + n + ")",
           "INSERTED");
        ok("INSERT INTO w VALUES ('k" + std::to_string(i % 11) + "', " + n + ", " + n + ")",
           "INSERTED");
    }
    ok("UPDATE w SET n = 1000 WHERE k = 'k3' AND n = 3", "UPDATED 1");
    ok("DELETE FROM e WHERE a = 0 AND b = 3", "DELETED 1");
    ASSERT_EQ(db->Execute("SYNC"), "OK synced");
    const char* reads[] = {
        "SELECT a, b, v FROM e", "SELECT v FROM e WHERE a = -3 AND b = 7",
        "SELECT b FROM e WHERE a = 2 AND b >= 100 AND b <= 200",
        "SELECT k, n, v FROM w ORDER BY k, n", "SELECT v FROM w WHERE k = 'k3' AND n = 1000",
        "DESCRIBE e", "DESCRIBE w",
    };
    std::vector<std::string> before;
    for (const char* read : reads) before.push_back(db->Execute(read));

    db->Crash();
    ASSERT_TRUE(db->Reboot().ok());

    for (std::size_t i = 0; i < std::size(reads); ++i) {
        EXPECT_EQ(db->Execute(reads[i]), before[i]) << reads[i];
    }
    // Each form still enforces its key and places a new row by it.
    EXPECT_NE(db->Execute("INSERT INTO e VALUES (-3, 7, 0)").find("is already held"),
              std::string::npos);
    EXPECT_NE(db->Execute("INSERT INTO w VALUES ('k3', 1000, 0)").find("is already held"),
              std::string::npos);
    ok("INSERT INTO e VALUES (9, 9, 99)", "INSERTED");
    ok("INSERT INTO w VALUES ('zz', 1, 1)", "INSERTED");
    EXPECT_EQ(db->Execute("SELECT v FROM e WHERE a = 9 AND b = 9"), "v\\n99");
    EXPECT_EQ(db->Execute("SELECT v FROM w WHERE k = 'zz' AND n = 1"), "v\\n1");
}

}  // namespace
}  // namespace kds::server
