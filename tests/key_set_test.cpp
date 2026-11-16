#include "two_core_rig.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// CU-S1 and CU-S2: a value list on the tuple key, on a column of an index's
// equality prefix or on a cabined column is a key set - one descent per
// value (instructions/v3.2.0/workorder-cu-key-set-access.md).
//
// The claim every cell here rests on is CU-R1's: the list stays in the
// residual, so a keyed list returns exactly the rows the walk of the same
// list returns. `Walked` is that walk: the same list in parentheses with one
// of its own leaves written again beside it, which is an `OR` holding an
// `OR`, no longer the shape, and so a plain scan.

namespace kds::server {
namespace {

// One engine over its own store, so two configurations can be compared.
struct Engine {
    explicit Engine(bool indexes = true) {
        auto boot = bootstrap::BootstrapDatabase(store, 1000);
        EXPECT_TRUE(boot.ok()) << boot.status().message();
        this->boot.emplace(std::move(boot.value()));
        recorder.emplace(this->boot->catalog, store);
        cabins.emplace();
        dispatcher.emplace(this->boot->superblock, this->boot->catalog, store, /*log=*/nullptr,
                           /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                           exec::Budget(), &*recorder, /*replay=*/true,
                           /*access_statistics=*/true, &*cabins, /*txn=*/nullptr,
                           txn::IsolationLevel::kReadCommitted, /*core_id=*/0, indexes);

        // t: ids 1..12 but 7 (a gap); v = 10 * id; g = id % 3; name unique;
        // sym 'a', 'b' or 'c' by id % 3, cabined. Indexes on (v), (g, v) and
        // (name), declared in that order, which is their oid order.
        // u: ids 1..6; tid names a t row (two name 9, one names the gap).
        Ok("CREATE TABLE t (id int64, v int64, g int64, name varchar, sym varchar) BTREE");
        Ok("CREATE INDEX t_by_v ON t (v)");
        Ok("CREATE INDEX t_by_g_v ON t (g, v)");
        Ok("CREATE INDEX t_by_name ON t (name)");
        Ok("CREATE CABIN ON t(sym)");
        for (int id = 1; id <= 12; ++id) {
            if (id == 7) continue;
            Ok("INSERT INTO t VALUES (" + std::to_string(id) + ", " + std::to_string(10 * id) +
               ", " + std::to_string(id % 3) + ", 'n" + std::to_string(id) + "', '" +
               std::string(1, static_cast<char>('a' + id % 3)) + "')");
        }
        Ok("CREATE TABLE u (id int64, tid int64) BTREE");
        // One row a statement: this configuration has no transaction manager.
        for (const char* row : {"(1, 2)", "(2, 9)", "(3, 7)", "(4, 9)", "(5, 12)", "(6, 1)"}) {
            Ok(std::string("INSERT INTO u VALUES ") + row);
        }
    }

    std::string Run(const std::string& sql) { return dispatcher->Dispatch(sql).response; }
    void Ok(const std::string& sql) {
        const std::string reply = Run(sql);
        EXPECT_NE(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
    }

    storage::InMemoryPageStore store{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot;
    std::optional<stats::TrailRecorder> recorder;
    std::optional<stats::CabinStore> cabins;
    std::optional<CommandDispatcher> dispatcher;
};

class KeySetTest : public ::testing::Test {
protected:
    std::string Run(const std::string& sql) { return db_.Run(sql); }
    std::string Ids(const std::string& where) { return Run("SELECT id FROM t WHERE " + where); }
    std::string Plan(const std::string& sql) { return Run("ANALYZE " + sql); }
    static bool Has(const std::string& text, const std::string& token) {
        return text.find(token) != std::string::npos;
    }

    Engine db_;
};

// ---- The shape ------------------------------------------------------------

TEST_F(KeySetTest, AListOnTheTupleKeyIsOneDescentPerValue) {
    const std::string plan = Plan("SELECT id FROM t WHERE id IN (9, 2, 5)");
    EXPECT_TRUE(Has(plan, "step 0 Lookup t key_set=3")) << plan;
    // Three rows read, where the walk reads the relation's eleven.
    EXPECT_TRUE(Has(plan, "step 0 Lookup t opens=1 examined=3 matched=3")) << plan;
    // CU-R1: the list is still the step's filter.
    EXPECT_TRUE(Has(plan, "filter bool (0:0.0 = 9 OR 0:0.0 = 2 OR 0:0.0 = 5)")) << plan;
    EXPECT_EQ(Ids("id IN (9, 2, 5)"), "id\\n2\\n5\\n9");
}

TEST_F(KeySetTest, AWrittenOrOfEqualitiesIsTheSameShape) {
    EXPECT_TRUE(Has(Plan("SELECT id FROM t WHERE id = 2 OR id = 5"), "Lookup t key_set=2"));
    EXPECT_TRUE(Has(Plan("SELECT id FROM t WHERE 5 = id OR id = 2 OR 9 = id"),
                    "Lookup t key_set=3"));
    EXPECT_EQ(Ids("5 = id OR id = 2 OR 9 = id"), "id\\n2\\n5\\n9");
}

// CU-Q6: several rows, so a step-chain select, and `kPointSelect` stays the
// one-row class.
TEST_F(KeySetTest, AOneStepSetIsNotAPointSelect) {
    EXPECT_TRUE(Has(Plan("SELECT id FROM t WHERE id IN (2, 5)"), "class=JoinSelect"));
    EXPECT_TRUE(Has(Plan("SELECT id FROM t WHERE id = 2"), "class=PointSelect"));
}

TEST_F(KeySetTest, AOneItemListIsThePlainLookupItWas) {
    const std::string plan = Plan("SELECT id FROM t WHERE id IN (5)");
    EXPECT_TRUE(Has(plan, "step 0 Lookup t key=5")) << plan;
    EXPECT_FALSE(Has(plan, "key_set=")) << plan;
    EXPECT_TRUE(Has(plan, "class=PointSelect")) << plan;
}

TEST_F(KeySetTest, ItemsAreDistinctAndAValueWithNoRowEmitsNothing) {
    const std::string plan = Plan("SELECT id FROM t WHERE id IN (5, 2, 5, 7, 2, 400)");
    EXPECT_TRUE(Has(plan, "Lookup t key_set=4")) << plan;  // 2, 5, 7, 400
    EXPECT_TRUE(Has(plan, "examined=2 matched=2")) << plan;
    EXPECT_EQ(Ids("id IN (5, 2, 5, 7, 2, 400)"), "id\\n2\\n5");
}

// CU-R3: an item the single-value arm would not key on is left out of the
// set and is still a leaf of the list.
TEST_F(KeySetTest, AnItemThatNamesNoKeyIsLeftOutOfTheSet) {
    EXPECT_TRUE(Has(Plan("SELECT id FROM t WHERE id IN (2, NULL)"), "Lookup t key_set=1"));
    EXPECT_EQ(Ids("id IN (2, NULL)"), "id\\n2");
    EXPECT_TRUE(Has(Plan("SELECT id FROM t WHERE id IN (-4, 2, 5)"), "Lookup t key_set=2"));
    EXPECT_EQ(Ids("id IN (-4, 2, 5)"), "id\\n2\\n5");
    // Past the 48 bits a key holds: keyed as the single-value arm keys it,
    // and found by neither.
    EXPECT_EQ(Ids("id IN (2, 281474976710656)"), "id\\n2");
    EXPECT_EQ(Ids("id = 281474976710656"), "id");
}

TEST_F(KeySetTest, AListWithNoKeyAtAllKeepsItsWalkAndAnswersEmpty) {
    const std::string plan = Plan("SELECT id FROM t WHERE id IN (-4, NULL)");
    EXPECT_TRUE(Has(plan, "step 0 Scan t")) << plan;
    EXPECT_FALSE(Has(plan, "key_set=")) << plan;
    EXPECT_EQ(Ids("id IN (-4, NULL)"), "id");
}

// A list literal is typed at compile as it was (CU adds no refusal and
// removes none).
TEST_F(KeySetTest, AMistypedItemIsStillRefusedAtCompile) {
    const std::string refused = Ids("id IN (2, 'x')");
    EXPECT_EQ(refused.rfind("ERR", 0), 0u) << refused;
    EXPECT_TRUE(Has(refused, "byte 33")) << refused;
}

TEST_F(KeySetTest, WhatIsNotTheShapeKeepsItsWalk) {
    for (const char* where : {
             "NOT (id IN (2, 5))",               // under NOT
             "id NOT IN (2, 5)",                 // flat inequalities
             "id = 2 OR v = 50",                 // across columns
             "id = 2 OR id > 10",                // a leaf that is not `=`
             "id IN (2, 5) OR id = 9",           // an OR under an OR
             "(id = 2 OR id = 5) OR id = 9",     // the same, parenthesized
             "id = 99 OR (id IN (2, 5) AND v > 0)",  // under an AND in the tree
             "v = 20 OR g = 1",                  // two indexed columns: an index union
         }) {
        const std::string plan = Plan(std::string("SELECT id FROM t WHERE ") + where);
        EXPECT_FALSE(Has(plan, "key_set=")) << where << ": " << plan;
        EXPECT_FALSE(Has(plan, "step 0 Lookup")) << where << ": " << plan;
    }
    EXPECT_EQ(Ids("id IN (2, 5) OR id = 9"), "id\\n2\\n5\\n9");
    EXPECT_EQ(Ids("id = 99 OR (id IN (2, 5) AND v > 20)"), "id\\n5");
}

// A list that reads another relation's column is a correlated list, not a
// set of literals.
TEST_F(KeySetTest, AListAgainstAnotherRelationsColumnIsNotASet) {
    const std::string sql = "SELECT u.id FROM u JOIN t ON t.g = u.id WHERE t.id = u.tid OR t.id = 2";
    EXPECT_FALSE(Has(Plan(sql), "key_set=")) << Plan(sql);
}

// ---- The ladder -----------------------------------------------------------

TEST_F(KeySetTest, ASingleValueEqualityWinsOverASet) {
    const std::string plan = Plan("SELECT id FROM t WHERE id = 5 AND id IN (2, 5)");
    EXPECT_TRUE(Has(plan, "step 0 Lookup t key=5")) << plan;
    EXPECT_FALSE(Has(plan, "key_set=")) << plan;
    EXPECT_EQ(Ids("id = 5 AND id IN (2, 5)"), "id\\n5");
    EXPECT_EQ(Ids("id = 3 AND id IN (2, 5)"), "id");
}

TEST_F(KeySetTest, ASetWinsOverEveryArmBelowIt) {
    // A tuple key range, an index probe and a filter scan each rank below.
    EXPECT_TRUE(Has(Plan("SELECT id FROM t WHERE id IN (2, 5, 9) AND id BETWEEN 4 AND 10"),
                    "Lookup t key_set=3"));
    EXPECT_EQ(Ids("id IN (2, 5, 9) AND id BETWEEN 4 AND 10"), "id\\n5\\n9");
    EXPECT_TRUE(Has(Plan("SELECT id FROM t WHERE id IN (2, 5, 9) AND v = 50"),
                    "Lookup t key_set=3"));
    EXPECT_EQ(Ids("id IN (2, 5, 9) AND v = 50"), "id\\n5");
    EXPECT_TRUE(Has(Plan("SELECT id FROM t WHERE id IN (2, 5, 9) AND g = 2"),
                    "Lookup t key_set=3"));
    EXPECT_EQ(Ids("id IN (2, 5, 9) AND g = 2"), "id\\n2\\n5");
}

// Two lists on the key: the first written is the set, the second filters.
TEST_F(KeySetTest, OfTwoListsTheFirstIsTheSetAndTheOtherFilters) {
    const std::string plan = Plan("SELECT id FROM t WHERE id IN (2, 5, 9) AND id IN (5, 9, 11, 12)");
    EXPECT_TRUE(Has(plan, "Lookup t key_set=3")) << plan;
    EXPECT_EQ(Ids("id IN (2, 5, 9) AND id IN (5, 9, 11, 12)"), "id\\n5\\n9");
}

// ---- CU-R1: the same rows as the walk ---------------------------------------

// The same list, no longer the shape: in parentheses, with one of its leaves
// written again beside it. A scan with the same answer.
std::string Walked(const std::string& list, const std::string& leaf) {
    return "((" + list + ") OR " + leaf + ")";
}

TEST_F(KeySetTest, EveryStatementAnswersAsTheWalkOfItsListDoes) {
    // The written order, so which step a list lands on is the statement's.
    db_.dispatcher->set_plan_join_order(false);
    struct Case {
        const char* head;  // up to the list
        const char* list;
        const char* leaf;  // one leaf of it
        const char* tail;
    };
    const std::vector<Case> cases = {
        {"SELECT * FROM t WHERE ", "id IN (9, 2, 5, 7, 400)", "id = 9", ""},
        {"SELECT * FROM t WHERE ", "id IN (9, 2, 5)", "id = 9", " AND v > 20"},
        {"SELECT * FROM t WHERE ", "id IN (9, 2, 5)", "id = 9", " AND (g = 0 OR name = 'n2')"},
        {"SELECT name, v FROM t WHERE ", "id = 12 OR id = 1 OR id = 6", "id = 1", " ORDER BY id"},
        {"SELECT * FROM t WHERE ", "id IN (9, 2, 5, 11)", "id = 9", " ORDER BY id DESC"},
        {"SELECT * FROM t WHERE ", "id IN (9, 2, 5, 11)", "id = 9", " ORDER BY v DESC LIMIT 2"},
        {"SELECT * FROM t WHERE ", "id IN (9, 2, 5, 11)", "id = 9", " ORDER BY id LIMIT 2 OFFSET 1"},
        {"SELECT g, COUNT(*), SUM(v) FROM t WHERE ", "id IN (1, 2, 3, 4, 5, 6)", "id = 1",
         " GROUP BY g"},
        {"SELECT COUNT(*) FROM t WHERE ", "id IN (7, 400)", "id = 7", ""},
        // A set on the driving step and on an inner one.
        {"SELECT u.id, t.id, t.v FROM t JOIN u ON u.tid = t.id WHERE ", "t.id IN (9, 2, 7, 12)",
         "t.id = 9", " ORDER BY u.id"},
        {"SELECT u.id, t.name FROM u JOIN t ON t.g = u.id WHERE ", "t.id IN (1, 2, 4, 5, 8)",
         "t.id = 1", " ORDER BY u.id, t.id"},
        // Inside a subquery, uncorrelated and correlated.
        {"SELECT id FROM t WHERE id IN (SELECT tid FROM u WHERE ", "id IN (2, 4, 6)", "id = 2",
         ")"},
        {"SELECT id FROM u WHERE EXISTS (SELECT id FROM t WHERE t.g = u.id AND ",
         "t.id IN (1, 2, 4, 5)", "t.id = 1", ")"},
        // CU-S2: a list on an index's column, alone, as one column of a
        // longer prefix, inside a subquery, and on a cabined column.
        {"SELECT * FROM t WHERE ", "v IN (50, 20, 20, 999)", "v = 50", ""},
        {"SELECT * FROM t WHERE ", "v IN (50, 20, 90, 110)", "v = 50",
         " AND g = 2 ORDER BY id LIMIT 1"},
        {"SELECT * FROM t WHERE g = 1 AND ", "v IN (10, 40, 70, 100)", "v = 10", ""},
        {"SELECT id, v FROM t WHERE ", "g IN (1, 2)", "g = 1", " AND v BETWEEN 20 AND 50"},
        {"SELECT id FROM u WHERE id IN (SELECT g FROM t WHERE ", "v IN (40, 80, 110)", "v = 40",
         ")"},
        {"SELECT id, sym FROM t WHERE ", "sym IN ('c', 'a', 'zz')", "sym = 'c'", " AND v > 30"},
        // Hoisted: an uncorrelated EXISTS, true and false.
        {"SELECT id FROM t WHERE id < 4 AND EXISTS (SELECT id FROM u WHERE ", "u.id IN (2, 40)",
         "u.id = 2", ")"},
        {"SELECT id FROM t WHERE id < 4 AND EXISTS (SELECT id FROM u WHERE ", "u.id IN (30, 40)",
         "u.id = 30", ")"},
    };
    for (const Case& c : cases) {
        const std::string keyed = std::string(c.head) + c.list + c.tail;
        const std::string walked = std::string(c.head) + Walked(c.list, c.leaf) + c.tail;
        EXPECT_TRUE(Has(Plan(keyed), "key_set=")) << keyed << ": " << Plan(keyed);
        EXPECT_FALSE(Has(Plan(walked), "key_set=")) << walked << ": " << Plan(walked);
        const std::string reply = Run(keyed);
        EXPECT_NE(reply.rfind("ERR", 0), 0u) << keyed << " -> " << reply;
        EXPECT_EQ(reply, Run(walked)) << keyed;
    }
}

// The set is `f(shape, catalog)`: with indexes ignored and with the written
// join order, every statement above answers the same bytes.
TEST_F(KeySetTest, TheAnswerIsTheSameWithIndexesOffAndInTheWrittenOrder) {
    Engine no_indexes(/*indexes=*/false);
    Engine written;
    written.dispatcher->set_plan_join_order(false);
    for (const char* sql : {
             "SELECT * FROM t WHERE id IN (9, 2, 5, 7, 400)",
             "SELECT * FROM t WHERE id IN (9, 2, 5) AND v = 50",
             "SELECT * FROM t WHERE v IN (50, 20, 20, 999)",
             "SELECT * FROM t WHERE g = 1 AND v IN (10, 40, 70, 100)",
             "SELECT id, v FROM t WHERE g IN (1, 2) AND v BETWEEN 20 AND 50",
             "SELECT u.id, t.id FROM u JOIN t ON t.g = u.id WHERE t.v IN (40, 80, 110) "
             "ORDER BY u.id, t.id",
             "SELECT u.id, t.id, t.v FROM u JOIN t ON t.id = u.tid WHERE t.id IN (9, 2, 7, 12) "
             "ORDER BY u.id",
             "SELECT u.id, t.name FROM u JOIN t ON t.g = u.id WHERE t.id IN (1, 2, 4, 5, 8) "
             "ORDER BY u.id, t.id",
             "SELECT u.id, t.name FROM t JOIN u ON u.tid = t.id WHERE u.id IN (2, 4, 5) AND "
             "t.id IN (9, 12) ORDER BY u.id",
         }) {
        const std::string reply = Run(sql);
        EXPECT_NE(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
        EXPECT_EQ(reply, no_indexes.Run(sql)) << sql;
        EXPECT_EQ(reply, written.Run(sql)) << sql;
    }
}

// In the written order `t` is the inner step, and its list keys it: two
// descents per outer row where it walked the relation per outer row.
TEST_F(KeySetTest, AJoinsInnerStepIsKeyedByAList) {
    db_.dispatcher->set_plan_join_order(false);
    const std::string sql =
        "SELECT u.id, t.id FROM u JOIN t ON t.g = u.id WHERE t.id IN (4, 8) ORDER BY u.id, t.id";
    const std::string plan = Plan(sql);
    EXPECT_TRUE(Has(plan, "step 1 Lookup t key_set=2")) << plan;
    // Six outer rows, two descents each.
    EXPECT_TRUE(Has(plan, "step 1 Lookup t opens=6 examined=12 matched=2")) << plan;
    EXPECT_EQ(Run(sql), "u.id,t.id\\n1,4\\n2,8");
}

// The query layer's walks are native steps (parser-v2.md I19), so a list
// under a statement only the layer reads is keyed too, and answers the same.
TEST_F(KeySetTest, AStatementTheQueryLayerReadsAnswersAsItsWalkDoes) {
    for (const char* tail : {"", " ORDER BY 1 DESC"}) {
        const std::string head = "SELECT DISTINCT g + 1 FROM t WHERE ";
        const std::string keyed = head + "id IN (1, 2, 4, 5, 8, 400)" + tail;
        const std::string reply = Run(keyed);
        EXPECT_NE(reply.rfind("ERR", 0), 0u) << keyed << " -> " << reply;
        EXPECT_EQ(reply, Run(head + Walked("id IN (1, 2, 4, 5, 8, 400)", "id = 1") + tail));
    }
}

// The cap is the conjunct's (BU-Q2): the largest list the grammar reads is
// keyed whole, and one item more is refused as it was.
TEST_F(KeySetTest, TheLargestListIsKeyedAndOneMoreIsStillRefused) {
    const auto list = [](int n) {
        std::string sql = "SELECT id FROM t WHERE id IN (";
        for (int i = 1; i <= n; ++i) sql += (i == 1 ? "" : ", ") + std::to_string(i);
        return sql + ")";
    };
    EXPECT_TRUE(Has(Plan(list(4096)), "Lookup t key_set=4096"));
    EXPECT_EQ(Run(list(4096)), "id\\n1\\n2\\n3\\n4\\n5\\n6\\n8\\n9\\n10\\n11\\n12");
    EXPECT_EQ(Run(list(4097)).rfind("ERR", 0), 0u);
}

// A heap's tuple key is hidden (heap-and-tuple.md 3.1b): its declared `id`
// is an ordinary column, and a list on it is no key set.
TEST_F(KeySetTest, AHeapsDeclaredFirstColumnIsNotItsTupleKey) {
    db_.Ok("CREATE TABLE h (id int64, v int64) HEAP");
    db_.Ok("INSERT INTO h VALUES (1, 10)");
    db_.Ok("INSERT INTO h VALUES (2, 20)");
    EXPECT_FALSE(Has(Plan("SELECT v FROM h WHERE id IN (1, 2)"), "key_set="));
    EXPECT_EQ(Run("SELECT v FROM h WHERE id IN (2, 9)"), "v\\n20");
}

// A list on a relation a write only reads is a read step, and is keyed:
// `UPDATE ... FROM` writes the rows it names and still refuses a row two
// `FROM` rows match (BU-Q16).
TEST_F(KeySetTest, AWriteThatReadsARelationByListWritesWhatItNames) {
    EXPECT_EQ(Run("UPDATE t SET g = 7 FROM u WHERE t.id = u.tid AND u.id IN (1, 5)"), "UPDATED 2");
    EXPECT_EQ(Ids("g = 7"), "id\\n2\\n12");
    const std::string twice = Run("UPDATE t SET g = 8 FROM u WHERE t.id = u.tid AND u.id IN (2, 4)");
    EXPECT_EQ(twice.rfind("ERR", 0), 0u) << twice;
    EXPECT_EQ(Ids("g = 8"), "id");
}

// The list filters what the descent found: a row deleted or moved off the
// rest of the WHERE is not answered.
TEST_F(KeySetTest, ADeletedOrMovedRowIsNotAnswered) {
    db_.Ok("DELETE FROM t WHERE id = 5");
    EXPECT_EQ(Ids("id IN (2, 5, 9)"), "id\\n2\\n9");
    db_.Ok("UPDATE t SET v = 21 WHERE id = 2");
    EXPECT_EQ(Ids("id IN (2, 5, 9) AND v >= 30"), "id\\n9");
}

// ---- CU-S2: a list on an index's column (index.md IX9) ------------------------

TEST_F(KeySetTest, AListOnAnIndexedColumnIsOneIndexDescentPerValue) {
    const std::string plan = Plan("SELECT id FROM t WHERE v IN (50, 20, 20, 999)");
    EXPECT_TRUE(Has(plan, "step 0 IndexProbe t key_set=3")) << plan;  // 20, 50, 999
    EXPECT_TRUE(Has(plan, "index_scanned=2 index_resolved=2")) << plan;
    EXPECT_TRUE(Has(plan, "filter bool (0:0.1 = 50 OR 0:0.1 = 20 OR 0:0.1 = 20 OR 0:0.1 = 999)"))
        << plan;
    EXPECT_EQ(Ids("v IN (50, 20, 20, 999)"), "id\\n2\\n5");
    // A NULL names no row and no entry.
    EXPECT_TRUE(Has(Plan("SELECT id FROM t WHERE v IN (20, NULL)"), "IndexProbe t key_set=1"));
    EXPECT_EQ(Ids("v IN (20, NULL)"), "id\\n2");
}

// IX9: the list is one equality column of the longest usable prefix,
// wherever in the prefix it stands.
TEST_F(KeySetTest, AListStandsAsOneColumnOfAnIndexPrefix) {
    // (g, v) by `g = 1` and three values of v: a longer prefix than (v)'s.
    const std::string second = Plan("SELECT id FROM t WHERE g = 1 AND v IN (10, 40, 70)");
    EXPECT_TRUE(Has(second, "step 0 IndexProbe t key_set=3")) << second;
    EXPECT_TRUE(Has(second, "index_scanned=2 index_resolved=2")) << second;  // exact entries
    EXPECT_EQ(Ids("g = 1 AND v IN (10, 40, 70)"), "id\\n1\\n4");
    // (g, v) by two values of g and `v = 50`.
    const std::string first = Plan("SELECT id FROM t WHERE g IN (0, 2) AND v = 50");
    EXPECT_TRUE(Has(first, "step 0 IndexProbe t key_set=2")) << first;
    EXPECT_TRUE(Has(first, "index_scanned=1 index_resolved=1")) << first;
    EXPECT_EQ(Ids("g IN (0, 2) AND v = 50"), "id\\n5");
}

TEST_F(KeySetTest, ARangeAfterAListIsAnIndexRangePerValue) {
    const std::string plan = Plan("SELECT id FROM t WHERE g IN (1, 2) AND v BETWEEN 20 AND 50");
    EXPECT_TRUE(Has(plan, "step 0 IndexRange t key_set=2")) << plan;
    EXPECT_TRUE(Has(plan, "index_scanned=3 index_resolved=3")) << plan;
    EXPECT_EQ(Ids("g IN (1, 2) AND v BETWEEN 20 AND 50"), "id\\n2\\n4\\n5");
}

// One list a probe: the second is a filter.
TEST_F(KeySetTest, OfTwoListsOnIndexedColumnsOneIsTheSet) {
    const std::string plan = Plan("SELECT id FROM t WHERE g IN (0, 1) AND v IN (10, 30, 60, 80)");
    EXPECT_TRUE(Has(plan, "step 0 IndexProbe t key_set=4")) << plan;  // (v), the lower oid
    EXPECT_EQ(Ids("g IN (0, 1) AND v IN (10, 30, 60, 80)"), "id\\n1\\n3\\n6");
}

// At one prefix length single equalities outrank a prefix a list fills, so a
// statement an equality already put on an index stays on it: (name) here,
// though (v), which the list could enter, has the lower oid.
TEST_F(KeySetTest, AListNeverTakesAStatementOffTheIndexItsEqualityEntered) {
    const std::string plan = Plan("SELECT id FROM t WHERE v IN (20, 50) AND name = 'n5'");
    EXPECT_TRUE(Has(plan, "step 0 IndexProbe t")) << plan;
    EXPECT_FALSE(Has(plan, "key_set=")) << plan;
    EXPECT_TRUE(Has(plan, "index_scanned=1 index_resolved=1")) << plan;
    EXPECT_EQ(Ids("v IN (20, 50) AND name = 'n5'"), "id\\n5");
}

// ... whatever range the list's prefix could go on to (CU-Q10): (g, v) would
// take the list and the range after it, and (name) keeps the statement.
TEST_F(KeySetTest, ARangeAfterAListDoesNotOutrankASingleEquality) {
    const std::string where = "name = 'n5' AND g IN (1, 2) AND v BETWEEN 20 AND 50";
    const std::string plan = Plan("SELECT id FROM t WHERE " + where);
    EXPECT_TRUE(Has(plan, "step 0 IndexProbe t")) << plan;
    EXPECT_FALSE(Has(plan, "key_set=")) << plan;
    EXPECT_TRUE(Has(plan, "index_scanned=1 index_resolved=1")) << plan;
    EXPECT_EQ(Ids(where), "id\\n5");
}

// CU-Q13: a join key's index keeps its step. In the written order `t` is
// entered once per `u` row by (g, v)'s leading column, as it was before
// lists were read; a set on (v) would walk three values' entries for every
// one of those rows. The planner's own order drives by the set instead.
TEST_F(KeySetTest, AListDoesNotTakeAStepOffItsJoinKeysIndex) {
    Engine written;
    written.dispatcher->set_plan_join_order(false);
    const std::string head = "SELECT u.id, t.id FROM u JOIN t ON t.g = u.id WHERE ";
    const std::string tail = " ORDER BY u.id, t.id";
    const std::string plan = written.Run("ANALYZE " + head + "t.v IN (40, 80, 110)" + tail);
    EXPECT_TRUE(Has(plan, "step 1 IndexProbe t key=0:0.0")) << plan;
    EXPECT_FALSE(Has(plan, "key_set=")) << plan;
    const std::string walked = head + Walked("t.v IN (40, 80, 110)", "t.v = 40") + tail;
    EXPECT_EQ(written.Run(head + "t.v IN (40, 80, 110)" + tail), written.Run(walked));

    const std::string planned = Plan(head + "t.v IN (40, 80, 110)" + tail);
    EXPECT_TRUE(Has(planned, "step 1 IndexProbe t key_set=3")) << planned;
    EXPECT_TRUE(Has(planned, "step 0 Probe u")) << planned;
    EXPECT_EQ(Run(head + "t.v IN (40, 80, 110)" + tail), written.Run(walked));
}

// The same for a join key's Cabin: `t` is asked for one `sym` per `s` row.
TEST_F(KeySetTest, AListDoesNotTakeAStepOffItsJoinKeysCabin) {
    Engine written;
    written.dispatcher->set_plan_join_order(false);
    written.Ok("CREATE TABLE s (id int64, sym varchar) BTREE");
    written.Ok("INSERT INTO s VALUES (1, 'a')");
    written.Ok("INSERT INTO s VALUES (2, 'c')");
    const std::string head = "SELECT s.id, t.id FROM s JOIN t ON t.sym = s.sym WHERE ";
    const std::string tail = " ORDER BY s.id, t.id";
    const std::string plan = written.Run("ANALYZE " + head + "t.v IN (20, 90, 100, 120)" + tail);
    EXPECT_TRUE(Has(plan, "step 1 CabinProbe t")) << plan;
    EXPECT_FALSE(Has(plan, "key_set=")) << plan;
    EXPECT_FALSE(Has(plan, "IndexProbe")) << plan;
    EXPECT_EQ(written.Run(head + "t.v IN (20, 90, 100, 120)" + tail),
              "s.id,t.id\\n1,9\\n1,12\\n2,2");
    EXPECT_EQ(written.Run(head + "t.v IN (20, 90, 100, 120)" + tail),
              written.Run(head + Walked("t.v IN (20, 90, 100, 120)", "t.v = 20") + tail));
}

// The covering filter across a set's values (index.md IX4b, AT-S22): a row
// is one row whichever value's entry reached it. Row 1's entry under the
// value it left fails the filter and its entry under the value it took
// passes, and it is answered; row 3's stale entry passes and its row does
// not, and it is not.
TEST_F(KeySetTest, ARowsEntriesUnderTwoListedValuesAreFilteredAsOneRows) {
    db_.Ok("CREATE TABLE c (id int64, k int64, q int64) BTREE");
    db_.Ok("CREATE INDEX c_by_k ON c (k) COVERING (q)");
    for (const char* row : {"(1, 1, 5)", "(2, 2, 50)", "(3, 1, 70)", "(4, 3, 90)", "(5, 2, 8)"}) {
        db_.Ok(std::string("INSERT INTO c VALUES ") + row);
    }
    db_.Ok("UPDATE c SET k = 2, q = 60 WHERE id = 1");
    db_.Ok("UPDATE c SET k = 2, q = 7 WHERE id = 3");
    const std::string list = "k IN (1, 2)";
    const std::string plan = Plan("SELECT id FROM c WHERE " + list + " AND q >= 40");
    EXPECT_TRUE(Has(plan, "step 0 IndexProbe c key_set=2")) << plan;
    EXPECT_TRUE(Has(plan, "index_filtered=")) << plan;
    EXPECT_EQ(Run("SELECT id FROM c WHERE " + list + " AND q >= 40"), "id\\n1\\n2");
    EXPECT_EQ(Run("SELECT id, q FROM c WHERE " + list + " AND q >= 40"),
              Run("SELECT id, q FROM c WHERE " + Walked(list, "k = 1") + " AND q >= 40"));
}

// The largest list the grammar reads, on an index: every value a descent,
// every leaf index carried beside its tuple key (`kKeySetLeafBits`).
TEST_F(KeySetTest, TheLargestListOnAnIndexIsKeyedWhole) {
    std::string list = "v IN (";
    for (int i = 1; i <= 4096; ++i) list += (i == 1 ? "" : ", ") + std::to_string(i);
    list += ")";
    EXPECT_TRUE(Has(Plan("SELECT id FROM t WHERE " + list), "IndexProbe t key_set=4096"));
    EXPECT_EQ(Ids(list), "id\\n1\\n2\\n3\\n4\\n5\\n6\\n8\\n9\\n10\\n11\\n12");
}

// CU-R3 on an index: each item is coerced as a single value is and keyed by
// the bytes that value's probe would be - a date, a decimal written at
// another scale, a timestamp.
TEST_F(KeySetTest, AListOfDatesDecimalsAndTimestampsIsKeyedAsItsSingleValuesAre) {
    db_.Ok("CREATE TABLE d (id int64, day date, amt decimal(10, 2), ts timestamp) BTREE");
    db_.Ok("CREATE INDEX d_by_day ON d (day)");
    db_.Ok("CREATE INDEX d_by_amt ON d (amt)");
    db_.Ok("CREATE INDEX d_by_ts ON d (ts)");
    for (const char* row : {"(1, '2026-01-02', '1.50', '2026-01-02 09:00:00')",
                            "(2, '2026-01-05', '20.00', '2026-01-05 10:30:00')",
                            "(3, '2026-01-09', '-3.25', '2026-01-09 00:00:00')",
                            "(4, '2026-01-05', '1.5', '2026-01-05 10:30:00.250000')"}) {
        db_.Ok(std::string("INSERT INTO d VALUES ") + row);
    }
    struct Case {
        const char* list;
        const char* leaf;
        const char* ids;
    };
    for (const Case& c : {
             Case{"day IN ('2026-01-05', '2026-01-09', '2026-02-01')", "day = '2026-01-05'",
                  "id\\n2\\n3\\n4"},
             Case{"amt IN ('1.5', '-3.25', '7')", "amt = '1.5'", "id\\n1\\n3\\n4"},
             Case{"amt IN ('1.50', '1.5', '20')", "amt = '20'", "id\\n1\\n2\\n4"},
             Case{"ts IN ('2026-01-05 10:30:00', '2026-01-09 00:00:00', '2026-01-01 00:00:00')",
                  "ts = '2026-01-09 00:00:00'", "id\\n2\\n3"},
         }) {
        const std::string plan = Plan(std::string("SELECT id FROM d WHERE ") + c.list);
        EXPECT_TRUE(Has(plan, "step 0 IndexProbe d key_set=")) << plan;
        EXPECT_EQ(Run(std::string("SELECT id FROM d WHERE ") + c.list), c.ids) << c.list;
        EXPECT_EQ(Run(std::string("SELECT id FROM d WHERE ") + c.list),
                  Run("SELECT id FROM d WHERE " + Walked(c.list, c.leaf)))
            << c.list;
    }
    // Two spellings of one decimal are one value: one descent.
    EXPECT_TRUE(Has(Plan("SELECT id FROM d WHERE amt IN ('1.50', '1.5', '20')"), "key_set=2"));
}

TEST_F(KeySetTest, ATupleKeySetWinsOverAnIndexSet) {
    const std::string plan = Plan("SELECT id FROM t WHERE v IN (10, 20, 30) AND id IN (2, 3, 9)");
    EXPECT_TRUE(Has(plan, "step 0 Lookup t key_set=3")) << plan;
    EXPECT_EQ(Ids("v IN (10, 20, 30) AND id IN (2, 3, 9)"), "id\\n2\\n3");
}

// CU-R2 on one statement's own writes: a row moved from one listed value to
// another has an entry under each, and comes back once.
TEST_F(KeySetTest, ARowWithAnEntryUnderTwoListedValuesIsAnsweredOnce) {
    db_.Ok("UPDATE t SET v = 50 WHERE id = 2");  // entries 20 -> 2 and 50 -> 2
    const std::string plan = Plan("SELECT id, v FROM t WHERE v IN (20, 50)");
    EXPECT_TRUE(Has(plan, "index_scanned=3 index_resolved=2")) << plan;
    EXPECT_EQ(Run("SELECT id, v FROM t WHERE v IN (20, 50)"), "id,v\\n2,50\\n5,50");
    EXPECT_EQ(Run("SELECT COUNT(*) FROM t WHERE v IN (50, 20)"), "count(*)\\n2");
}

// ---- CU-S2: a list on a cabined column (cabin.md) -----------------------------

// A Cabin set is served when every value of the list is observed, and walks
// when one is not. Either way it answers as the walk does.
TEST_F(KeySetTest, AListOnACabinedColumnIsServedWhenEveryValueIsObserved) {
    const std::string list = "sym IN ('b', 'a', 'a', NULL)";
    const std::string walked = Run("SELECT id, sym FROM t WHERE " + Walked(list, "sym = 'b'"));

    // Nothing observed yet: the step is a Cabin step, and it walks.
    const std::string cold = Plan("SELECT id, sym FROM t WHERE " + list);
    EXPECT_TRUE(Has(cold, "step 0 CabinProbe t")) << cold;
    EXPECT_TRUE(Has(cold, "key_set=2")) << cold;  // its distinct values, less the NULL
    EXPECT_TRUE(Has(cold, "cabin_misses=1")) << cold;
    EXPECT_FALSE(Has(cold, "value=")) << cold;
    EXPECT_EQ(Run("SELECT id, sym FROM t WHERE " + list), walked);

    // A list banks nothing: however often it runs, it still walks.
    for (int i = 0; i < 4; ++i) Run("SELECT id, sym FROM t WHERE " + list);
    EXPECT_TRUE(Has(Plan("SELECT id, sym FROM t WHERE " + list), "cabin_misses=1"));

    // The values observed one at a time, and one of them only.
    for (int i = 0; i < 3; ++i) Run("SELECT id FROM t WHERE sym = 'a'");
    EXPECT_TRUE(Has(Plan("SELECT id, sym FROM t WHERE " + list), "cabin_misses=1"));
    for (int i = 0; i < 3; ++i) Run("SELECT id FROM t WHERE sym = 'b'");

    const std::string served = Plan("SELECT id, sym FROM t WHERE " + list);
    EXPECT_TRUE(Has(served, "cabin_hits=1")) << served;
    EXPECT_FALSE(Has(served, "cabin_misses=")) << served;
    EXPECT_EQ(Run("SELECT id, sym FROM t WHERE " + list), walked);

    // An observed value with no row is an authoritative empty set.
    for (int i = 0; i < 3; ++i) Run("SELECT id FROM t WHERE sym = 'zz'");
    EXPECT_TRUE(Has(Plan("SELECT id FROM t WHERE sym IN ('zz', 'a')"), "cabin_hits=1"));
    EXPECT_EQ(Run("SELECT id FROM t WHERE sym IN ('zz', 'a')"),
              Run("SELECT id FROM t WHERE " + Walked("sym IN ('zz', 'a')", "sym = 'a'")));

    // One unobserved value: the whole step walks, since its rows are in no set.
    EXPECT_TRUE(Has(Plan("SELECT id FROM t WHERE sym IN ('a', 'c')"), "cabin_misses=1"));
    EXPECT_EQ(Run("SELECT id FROM t WHERE sym IN ('a', 'c')"),
              Run("SELECT id FROM t WHERE " + Walked("sym IN ('a', 'c')", "sym = 'a'")));
}

// CU-R2: a row witnessed under two listed values is in both sets, and is
// served once.
TEST_F(KeySetTest, ARowInTwoObservedSetsIsServedOnce) {
    for (int i = 0; i < 3; ++i) {
        Run("SELECT id FROM t WHERE sym = 'a'");
        Run("SELECT id FROM t WHERE sym = 'b'");
    }
    db_.Ok("UPDATE t SET sym = 'b' WHERE id = 3");  // was 'a': now in both sets
    const std::string plan = Plan("SELECT id FROM t WHERE sym IN ('a', 'b')");
    EXPECT_TRUE(Has(plan, "cabin_hits=1")) << plan;
    EXPECT_EQ(Run("SELECT id, sym FROM t WHERE sym IN ('a', 'b') AND v < 45"),
              "id,sym\\n1,b\\n3,b\\n4,b");
    EXPECT_EQ(Run("SELECT COUNT(*) FROM t WHERE sym IN ('a', 'b')"),
              Run("SELECT COUNT(*) FROM t WHERE " + Walked("sym IN ('a', 'b')", "sym = 'a'")));
}

// A Cabin set keeps the Cabin probe's exclusion from the sort elision.
TEST_F(KeySetTest, ACabinSetsOrderByIsSorted) {
    EXPECT_TRUE(Has(Plan("SELECT id FROM t WHERE sym IN ('a', 'b') ORDER BY id"), "sorted="));
    EXPECT_EQ(Ids("sym IN ('c', 'a') AND v < 60 ORDER BY id"), "id\\n2\\n3\\n5");
}

// CU-Q12: a Cabin set takes only a step that would otherwise walk its
// relation whole. A set observes nothing, so over an arm that records what
// it misses, or a range that bounds its walk, it would turn a statement
// they serve into a walk no run of it ends.
TEST_F(KeySetTest, ACabinSetTakesOnlyAStepThatWouldWalkWhole) {
    // A strict tuple key range keeps the step.
    const std::string window = "sym IN ('a', 'b') AND id > 2 AND id < 9";
    const std::string ranged = Plan("SELECT id FROM t WHERE " + window);
    EXPECT_TRUE(Has(ranged, "step 0 Range t")) << ranged;
    EXPECT_FALSE(Has(ranged, "key_set=")) << ranged;
    EXPECT_EQ(Ids(window), "id\\n3\\n4\\n6");

    // A predicate Cabin keeps the step, and banks its own set as it did.
    db_.Ok("CREATE CABIN ON t (v > 30)");
    const std::string where = "v > 30 AND sym IN ('a', 'b')";
    for (int i = 0; i < 3; ++i) Ids(where);
    const std::string predicate = Plan("SELECT id FROM t WHERE " + where);
    EXPECT_TRUE(Has(predicate, "step 0 CabinProbe t")) << predicate;
    EXPECT_FALSE(Has(predicate, "key_set=")) << predicate;
    EXPECT_TRUE(Has(predicate, "cabin_hits=1")) << predicate;
    EXPECT_EQ(Ids(where), Ids("v > 30 AND " + Walked("sym IN ('a', 'b')", "sym = 'a'")));

    // A filter scan is a whole walk, and the set takes it.
    db_.Ok("CREATE TABLE z (id int64, sym varchar(16), r int64) BTREE");
    db_.Ok("CREATE CABIN ON z(sym)");
    for (const char* row : {"(1, 'a', 1)", "(2, 'b', 1)", "(3, 'a', 2)", "(4, 'c', 1)"}) {
        db_.Ok(std::string("INSERT INTO z VALUES ") + row);
    }
    EXPECT_TRUE(Has(Plan("SELECT id FROM z WHERE r = 1 AND sym = 'q' OR r = 9"), "Scan z"));
    const std::string filter = Plan("SELECT id FROM z WHERE r = 1 AND sym IN ('a', 'b')");
    EXPECT_TRUE(Has(filter, "step 0 CabinProbe z")) << filter;
    EXPECT_TRUE(Has(filter, "key_set=2")) << filter;
    EXPECT_EQ(Run("SELECT id FROM z WHERE r = 1 AND sym IN ('a', 'b')"), "id\\n1\\n2");
}

// ---- Order (parser-v2.md I11) -----------------------------------------------

TEST_F(KeySetTest, TheStepEmitsInTupleKeyOrderSoItsOrderByIsElided) {
    const std::string plan = Plan("SELECT id FROM t WHERE id IN (9, 2, 12, 5) ORDER BY id");
    EXPECT_FALSE(Has(plan, "sorted=")) << plan;
    EXPECT_EQ(Ids("id IN (9, 2, 12, 5) ORDER BY id"), "id\\n2\\n5\\n9\\n12");
    // With no ORDER BY the order is no contract, and is the same today.
    EXPECT_EQ(Ids("id IN (9, 2, 12, 5)"), "id\\n2\\n5\\n9\\n12");
    // Any other order sorts.
    EXPECT_TRUE(Has(Plan("SELECT id FROM t WHERE id IN (9, 2, 12, 5) ORDER BY id DESC"),
                    "sorted="));
    EXPECT_EQ(Ids("id IN (9, 2, 12, 5) ORDER BY id DESC"), "id\\n12\\n9\\n5\\n2");
}

TEST_F(KeySetTest, ALimitStopsTheDescents) {
    const std::string plan = Plan("SELECT id FROM t WHERE id IN (9, 2, 12, 5) ORDER BY id LIMIT 2");
    EXPECT_TRUE(Has(plan, "examined=2 matched=2")) << plan;
    EXPECT_EQ(Ids("id IN (9, 2, 12, 5) ORDER BY id LIMIT 2"), "id\\n2\\n5");
}

// ---- Waystone (CU-Q4) -------------------------------------------------------

// A set is a search over its values: a statement whose only tuple key term
// is a list admits no trail.
TEST_F(KeySetTest, WaystoneOnOverAListAloneIsRefused) {
    const std::string reply = Run("WAYSTONE ON SELECT id FROM t WHERE id IN (2, 5)");
    EXPECT_EQ(reply.rfind("ERR", 0), 0u) << reply;
}

// In an opted statement the probe step records and replays and the set step
// beside it does neither.
TEST_F(KeySetTest, ASetStepRecordsAndReplaysNoTrail) {
    db_.dispatcher->set_plan_join_order(false);
    const std::string sql =
        "SELECT u.id, t.name FROM u JOIN t ON t.id = u.tid WHERE u.id IN (1, 2, 4)";
    ASSERT_EQ(Run("WAYSTONE ON " + sql).rfind("OK WAYSTONE ON", 0), 0u);
    const std::string first = Run(sql);
    EXPECT_EQ(first, "u.id,t.name\\n1,n2\\n2,n9\\n4,n9");
    EXPECT_EQ(Run(sql), first);
    const std::string plan = Plan(sql);
    EXPECT_TRUE(Has(plan, "step 0 Lookup u key_set=3")) << plan;
    const auto line_of = [&plan](const std::string& head) {
        const std::size_t at = plan.find(head);
        return at == std::string::npos ? std::string() : plan.substr(at, plan.find("\\n", at) - at);
    };
    const std::string set_line = line_of("step 0 Lookup u opens=");
    const std::string probe_line = line_of("step 1 Probe t opens=");
    EXPECT_FALSE(set_line.empty()) << plan;
    EXPECT_FALSE(Has(set_line, "replays=")) << set_line;
    EXPECT_TRUE(Has(probe_line, "replays=")) << plan;
    EXPECT_EQ(Run(sql), first);
}

// ---- Writes are CU-S3's ------------------------------------------------------

TEST_F(KeySetTest, AWriteByListStillWritesTheRowsItNames) {
    EXPECT_EQ(Run("UPDATE t SET g = 9 WHERE id IN (2, 5, 7)"), "UPDATED 2");
    EXPECT_EQ(Ids("g = 9"), "id\\n2\\n5");
    EXPECT_EQ(Run("DELETE FROM t WHERE id IN (5, 400)"), "DELETED 1");
    EXPECT_EQ(Ids("id IN (2, 5)"), "id\\n2");
}

// ---- Under transactions ------------------------------------------------------
//
// The descent hands a (page, slot) to `AcceptTupleAt`, where visibility is
// applied for every access kind. These cells put versions a reader cannot
// see under a set and compare it with the walk of the same list.

class KeySetTxnTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto opened = TwoCoreRig::Open();
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        rig_ = std::move(opened.value());
        Session setup;
        ASSERT_EQ(Run(0, setup, "CREATE TABLE t (id int64, v int64) BTREE").rfind("CREATED", 0), 0u);
        ASSERT_EQ(Run(0, setup, "INSERT INTO t VALUES (1, 10), (2, 20), (3, 30), (4, 40), (5, 50)")
                      .rfind("INSERTED", 0),
                  0u);
    }

    std::string Run(std::uint32_t core, Session& s, const std::string& sql) {
        return rig_->core(core).dispatcher().Dispatch(sql, &s).response;
    }
    // The set's answer, checked against the walk of the same list.
    std::string Listed(std::uint32_t core, Session& s) {
        const std::string list = "id IN (2, 3, 9, 5, 7)";
        const std::string keyed = Run(core, s, "SELECT id, v FROM t WHERE " + list);
        EXPECT_NE(Run(core, s, "ANALYZE SELECT id, v FROM t WHERE " + list).find("key_set=5"),
                  std::string::npos);
        EXPECT_EQ(keyed, Run(core, s, "SELECT id, v FROM t WHERE " + Walked(list, "id = 2")));
        return keyed;
    }

    std::unique_ptr<TwoCoreRig> rig_;
};

TEST_F(KeySetTxnTest, AnUndecidedWritersVersionsAreItsOwnOnly) {
    Session writer;
    Session reader;
    ASSERT_EQ(Run(0, writer, "BEGIN").rfind("BEGIN", 0), 0u);
    ASSERT_EQ(Run(0, writer, "INSERT INTO t VALUES (9, 90)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Run(0, writer, "DELETE FROM t WHERE id = 2"), "DELETED 1");
    ASSERT_EQ(Run(0, writer, "UPDATE t SET v = 999 WHERE id = 3"), "UPDATED 1");

    // Another session, on another core: none of it.
    EXPECT_EQ(Listed(1, reader), "id,v\\n2,20\\n3,30\\n5,50");
    // The writer: all of it.
    EXPECT_EQ(Listed(0, writer), "id,v\\n3,999\\n5,50\\n9,90");

    ASSERT_EQ(Run(0, writer, "ROLLBACK").rfind("ROLLBACK", 0), 0u);
    EXPECT_EQ(Listed(1, reader), "id,v\\n2,20\\n3,30\\n5,50");
}

// CU-R2, the index: a row moved from one listed value to another, read by a
// snapshot that sees the newer version and by one that sees the older. The
// entry the older version left still names the row, so the list reaches it
// through two entries, and each snapshot answers it once.
TEST_F(KeySetTxnTest, AnIndexEntryLeftByAnEarlierVersionAnswersItsRowOnce) {
    Session setup;
    ASSERT_EQ(Run(0, setup, "CREATE TABLE x (id int64, v int64) BTREE").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Run(0, setup, "CREATE INDEX x_v ON x (v)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Run(0, setup, "INSERT INTO x VALUES (1, 10), (2, 20), (3, 30)").rfind("INSERTED", 0),
              0u);
    const std::string keyed = "SELECT id, v FROM x WHERE v IN (20, 30)";
    const std::string walked = "SELECT id, v FROM x WHERE ((v IN (20, 30)) OR v = 20)";

    Session older;
    ASSERT_EQ(Run(1, older, "BEGIN ISOLATION LEVEL REPEATABLE READ").rfind("BEGIN", 0), 0u);
    EXPECT_EQ(Run(1, older, keyed), "id,v\\n2,20\\n3,30");

    Session writer;
    ASSERT_EQ(Run(0, writer, "UPDATE x SET v = 30 WHERE id = 2"), "UPDATED 1");

    Session newer;
    const std::string plan = Run(0, newer, "ANALYZE " + keyed);
    EXPECT_NE(plan.find("IndexProbe x key_set=2"), std::string::npos) << plan;
    EXPECT_NE(plan.find("index_scanned=3 index_resolved=2"), std::string::npos) << plan;
    EXPECT_EQ(Run(0, newer, keyed), "id,v\\n2,30\\n3,30");
    EXPECT_EQ(Run(0, newer, walked), "id,v\\n2,30\\n3,30");

    EXPECT_EQ(Run(1, older, keyed), "id,v\\n2,20\\n3,30");
    EXPECT_EQ(Run(1, older, walked), "id,v\\n2,20\\n3,30");
    ASSERT_EQ(Run(1, older, "COMMIT").rfind("COMMIT", 0), 0u);
}

// CU-R2, the Cabin: the same move on a cabined column. The row is in the set
// of the value it left and of the value it took.
TEST_F(KeySetTxnTest, ARowWitnessedUnderTwoListedValuesIsServedOnce) {
    Session setup;
    ASSERT_EQ(Run(0, setup, "CREATE TABLE y (id int64, sym varchar(16)) BTREE").rfind("CREATED", 0),
              0u);
    ASSERT_EQ(Run(0, setup, "CREATE CABIN ON y(sym)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Run(0, setup, "INSERT INTO y VALUES (1, 'a'), (2, 'b'), (3, 'a')").rfind("INSERTED", 0),
              0u);
    for (int i = 0; i < 3; ++i) {
        Run(0, setup, "SELECT id FROM y WHERE sym = 'a'");
        Run(0, setup, "SELECT id FROM y WHERE sym = 'b'");
    }
    const std::string keyed = "SELECT id, sym FROM y WHERE sym IN ('a', 'b')";
    const std::string walked = "SELECT id, sym FROM y WHERE ((sym IN ('a', 'b')) OR sym = 'a')";
    const std::string served = Run(0, setup, "ANALYZE " + keyed);
    ASSERT_NE(served.find("CabinProbe y"), std::string::npos) << served;
    ASSERT_NE(served.find("cabin_hits=1"), std::string::npos) << served;

    Session older;
    ASSERT_EQ(Run(1, older, "BEGIN ISOLATION LEVEL REPEATABLE READ").rfind("BEGIN", 0), 0u);
    EXPECT_EQ(Run(1, older, keyed), "id,sym\\n1,a\\n2,b\\n3,a");

    Session writer;
    ASSERT_EQ(Run(0, writer, "UPDATE y SET sym = 'b' WHERE id = 1"), "UPDATED 1");

    // Both snapshots are answered from the sets, not by a walk: row 1 is in
    // the set of the value it left and of the value it took.
    Session newer;
    const std::string after = Run(0, newer, "ANALYZE " + keyed);
    EXPECT_NE(after.find("cabin_hits=1"), std::string::npos) << after;
    EXPECT_EQ(Run(0, newer, keyed), "id,sym\\n1,b\\n2,b\\n3,a");
    EXPECT_EQ(Run(0, newer, walked), "id,sym\\n1,b\\n2,b\\n3,a");
    const std::string before = Run(1, older, "ANALYZE " + keyed);
    EXPECT_NE(before.find("cabin_hits=1"), std::string::npos) << before;
    EXPECT_EQ(Run(1, older, keyed), "id,sym\\n1,a\\n2,b\\n3,a");
    EXPECT_EQ(Run(1, older, walked), "id,sym\\n1,a\\n2,b\\n3,a");
    ASSERT_EQ(Run(1, older, "COMMIT").rfind("COMMIT", 0), 0u);
}

TEST_F(KeySetTxnTest, ARepeatableReadSnapshotKeepsTheVersionsItBeganWith) {
    Session reader;
    Session writer;
    ASSERT_EQ(Run(1, reader, "BEGIN ISOLATION LEVEL REPEATABLE READ").rfind("BEGIN", 0), 0u);
    EXPECT_EQ(Listed(1, reader), "id,v\\n2,20\\n3,30\\n5,50");

    ASSERT_EQ(Run(0, writer, "INSERT INTO t VALUES (7, 70)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Run(0, writer, "DELETE FROM t WHERE id = 5"), "DELETED 1");
    ASSERT_EQ(Run(0, writer, "UPDATE t SET v = 21 WHERE id = 2"), "UPDATED 1");

    // The snapshot's rows, through the undo chain where a version moved.
    EXPECT_EQ(Listed(1, reader), "id,v\\n2,20\\n3,30\\n5,50");
    ASSERT_EQ(Run(1, reader, "COMMIT").rfind("COMMIT", 0), 0u);
    EXPECT_EQ(Listed(1, reader), "id,v\\n2,21\\n3,30\\n7,70");
}

}  // namespace
}  // namespace kds::server
