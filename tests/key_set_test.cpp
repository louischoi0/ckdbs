#include "two_core_rig.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/sched/clock.hpp"
#include "kds/txn/lock_table.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"

// CU-S1 and CU-S2: a value list on the tuple key, on a column of an index's
// equality prefix or on a cabined column is a key set - one descent per
// value (instructions/v3.2.0/workorder-cu-key-set-access.md). CU-S3: an
// UPDATE or a DELETE whose WHERE is a list on the tuple key writes by one
// descent per value. CU-S3b: beside any conjunct but a subquery, as a
// single tuple key does.
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
    db_.dispatcher->set_join_order(exec::JoinOrder::kWritten);
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
    written.dispatcher->set_join_order(exec::JoinOrder::kWritten);
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
    db_.dispatcher->set_join_order(exec::JoinOrder::kWritten);
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
    written.dispatcher->set_join_order(exec::JoinOrder::kWritten);
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
    written.dispatcher->set_join_order(exec::JoinOrder::kWritten);
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
    db_.dispatcher->set_join_order(exec::JoinOrder::kWritten);
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

// ---- CU-S3: a write by list -----------------------------------------------------

TEST_F(KeySetTest, AWriteByListWritesTheRowsItNames) {
    EXPECT_EQ(Run("UPDATE t SET g = 9 WHERE id IN (2, 5, 7)"), "UPDATED 2");
    EXPECT_EQ(Ids("g = 9"), "id\\n2\\n5");
    EXPECT_EQ(Run("DELETE FROM t WHERE id IN (5, 400)"), "DELETED 1");
    EXPECT_EQ(Ids("id IN (2, 5)"), "id\\n2");
    // No value names a row, and no value names a key at all.
    EXPECT_EQ(Run("UPDATE t SET g = 1 WHERE id IN (7, 400)"), "UPDATED 0");
    EXPECT_EQ(Run("DELETE FROM t WHERE id IN (-3, NULL)"), "DELETED 0");
}

// CU-R1 on a write: every statement leaves the relation as the walk of the
// same WHERE leaves it - its rows, what its indexes answer and what its
// Cabin answers. One engine runs the key or the list, the other its walk.
TEST(KeySetWriteTest, EveryWriteLeavesWhatTheWalkOfItsListLeaves) {
    Engine keyed;
    Engine walked;
    struct Case {
        const char* head;
        const char* list;  // the key's conjunct: a list, or a single equality
        // One of the list's own leaves, for `Walked`. For a single equality,
        // the bound that names the same key: a write walks a range.
        const char* leaf;
        const char* tail = "";  // the conjuncts after it (CU-S3b)
        bool single = false;    // `list` is one equality, `leaf` its walk
    };
    const Case cases[] = {
        // Duplicates, a gap (7) and a key past the relation; a SET that
        // reads the row, so a row written twice would show.
        {"UPDATE t SET g = g + 5 WHERE ", "id IN (9, 2, 2, 7, 400)", "id = 2"},
        {"UPDATE t SET g = g + 5 WHERE ", "id = 2 OR id = 9 OR 2 = id", "id = 2"},
        // An indexed column, a cabined one and a spilled-or-inline string.
        {"UPDATE t SET v = v + 1000 WHERE ", "id = 3 OR id = 11", "id = 3"},
        {"UPDATE t SET sym = 'z' WHERE ", "id IN (1, 4, 12)", "id = 4"},
        {"UPDATE t SET name = 'renamed' WHERE ", "id IN (6, -6, NULL)", "id = 6"},
        // No value is a key: both walk.
        {"UPDATE t SET g = 77 WHERE ", "id IN (-1, NULL)", "id = -1"},
        // CU-S3b: other conjuncts beside the list - one that leaves a listed
        // row out, one before and one after, and a bound on the key.
        {"UPDATE t SET v = v + 3 WHERE ", "id IN (9, 2, 3)", "id = 2", " AND v > 25"},
        {"UPDATE t SET name = 'again' WHERE sym = 'z' AND ", "id IN (1, 4, 12, 2)", "id = 1",
         " AND v < 1000"},
        {"UPDATE t SET v = v + 7 WHERE ", "id IN (2, 9, 11)", "id = 2", " AND id < 10"},
        // A single key beside a conjunct its row passes, one it fails, and
        // a second key that excludes it.
        {"UPDATE t SET v = v + 1 WHERE ", "id = 3", "id BETWEEN 3 AND 3", " AND g = 0", true},
        {"UPDATE t SET v = v + 1 WHERE ", "id = 3", "id BETWEEN 3 AND 3",
         " AND name = 'nobody'", true},
        {"UPDATE t SET name = 'never' WHERE ", "id = 2", "id BETWEEN 2 AND 2", " AND id = 9", true},
        {"DELETE FROM t WHERE ", "id IN (11, 12)", "id = 11", " AND g = 2"},
        {"DELETE FROM t WHERE ", "id = 4", "id BETWEEN 4 AND 4", " AND sym = 'z'", true},
        {"DELETE FROM t WHERE ", "id IN (5, 6, 400, 5)", "id = 5"},
        {"DELETE FROM t WHERE ", "8 = id OR id = 10", "id = 10"},
        // Rows the statements above already deleted.
        {"DELETE FROM t WHERE ", "id IN (5, 8, 1)", "id = 1"},
    };
    const char* reads[] = {
        "SELECT id, v, g, name, sym FROM t",
        "SELECT id FROM t WHERE v IN (20, 1030, 1110, 40)",   // by t_by_v
        "SELECT id, v FROM t WHERE g IN (0, 5, 7) AND v > 0",  // by t_by_g_v
        "SELECT id FROM t WHERE name = 'renamed'",             // by t_by_name
        "SELECT id FROM t WHERE sym = 'z'",                    // by the Cabin
        "SELECT id FROM t WHERE sym = 'a'",
    };
    for (const Case& c : cases) {
        const std::string statement = std::string(c.head) + c.list + c.tail;
        const std::string walk = c.single ? std::string(c.leaf) : Walked(c.list, c.leaf);
        const std::string reply = keyed.Run(statement);
        EXPECT_NE(reply.rfind("ERR", 0), 0u) << statement << " -> " << reply;
        EXPECT_EQ(reply, walked.Run(std::string(c.head) + walk + c.tail)) << statement;
        for (const char* read : reads) {
            // Twice, so a Cabin read is served the second time.
            keyed.Run(read);
            walked.Run(read);
            EXPECT_EQ(keyed.Run(read), walked.Run(read)) << read << " after " << statement;
        }
    }
    EXPECT_EQ(keyed.Run("SELECT id, g FROM t WHERE id IN (2, 9)"), "id,g\\n2,12\\n9,10");
    // The conjuncts chose: 2 failed `v > 25` and passed `id < 10`, 3 passed
    // `v > 25` and `g = 0`, and 11 was past the bound.
    EXPECT_EQ(keyed.Run("SELECT id, v FROM t WHERE id IN (2, 3, 9)"),
              "id,v\\n2,27\\n3,1034\\n9,100");
}

// A function conjunct beside the key is asked of the row the descent finds.
TEST_F(KeySetTest, AKeyedWriteBesideAFunctionConjunctAsksItOfTheRowFound) {
    db_.Ok("CREATE TABLE f (id int64, ts timestamp, v int64) BTREE");
    db_.Ok("INSERT INTO f VALUES (1, '2026-10-01 09:00:00', 0)");
    db_.Ok("INSERT INTO f VALUES (2, '2026-10-02 09:00:00', 0)");
    db_.Ok("INSERT INTO f VALUES (3, '2026-10-01 23:00:00', 0)");
    EXPECT_EQ(Run("UPDATE f SET v = 1 WHERE id = 2 AND DATE(ts) = '2026-10-01'"), "UPDATED 0");
    EXPECT_EQ(Run("UPDATE f SET v = 1 WHERE id = 1 AND DATE(ts) = '2026-10-01'"), "UPDATED 1");
    EXPECT_EQ(Run("UPDATE f SET v = v + 2 WHERE DATE(ts) = '2026-10-01' AND id IN (1, 2, 3)"),
              "UPDATED 2");
    EXPECT_EQ(Run("DELETE FROM f WHERE id IN (2, 3) AND DATE(ts) = '2026-10-02'"), "DELETED 1");
    EXPECT_EQ(Run("SELECT id, v FROM f"), "id,v\\n1,3\\n3,2");
}

// A heap's declared first column is an ordinary column, so a write by a
// key or a list on it walks (heap-and-tuple.md 3.1b). The rows are inserted
// out of order, so the hidden keys 1..4 hold ids 20, 10, 2 and 1 and a
// descent to the declared value would land on another row.
TEST_F(KeySetTest, AWriteOnAHeapsDeclaredFirstColumnWalks) {
    db_.Ok("CREATE TABLE h (id int64, v int64) HEAP");
    for (const int id : {20, 10, 2, 1}) {
        db_.Ok("INSERT INTO h VALUES (" + std::to_string(id) + ", 0)");
    }
    EXPECT_EQ(Run("UPDATE h SET v = v + 1 WHERE id IN (2, 4, 4, 9)"), "UPDATED 1");
    EXPECT_EQ(Run("UPDATE h SET v = v + 5 WHERE id = 2 AND v = 1"), "UPDATED 1");
    EXPECT_EQ(Run("DELETE FROM h WHERE id IN (1, 20)"), "DELETED 2");
    EXPECT_EQ(Run("DELETE FROM h WHERE id = 10 AND v = 0"), "DELETED 1");
    EXPECT_EQ(Run("SELECT id, v FROM h"), "id,v\\n2,6");
}

// ---- The descents, seen by their holds ------------------------------------------
//
// A write has no `ANALYZE`. What tells a descent from a walk is the hold: a
// write walk takes every leaf of the relation exclusive, a descent the leaf
// its key is in. The store below records the pages of one relation pinned
// exclusive, and can fail one fetch.

class LeafWatchingStore final : public storage::PageStore {
public:
    explicit LeafWatchingStore(storage::InMemoryPageStore& inner) : inner_(inner) {}

    StatusOr<std::span<std::byte, kPageSize>> CreateAtUnpinned(PageId page_id) override {
        return inner_.CreateAtUnpinned(page_id);
    }
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewUnpinned() override {
        return inner_.CreateNewUnpinned();
    }
    StatusOr<std::span<std::byte, kPageSize>> GetUnpinned(PageId page_id) override {
        if (Fails()) return Status::IoError("a fetch the test failed");
        return inner_.GetUnpinned(page_id);
    }
    StatusOr<std::span<std::byte, kPageSize>> GetForReadUnpinned(PageId page_id) override {
        if (Fails()) return Status::IoError("a fetch the test failed");
        return inner_.GetForReadUnpinned(page_id);
    }
    Status RaiseAllocationFloor(PageId first_allocatable_page_id) override {
        return inner_.RaiseAllocationFloor(first_allocatable_page_id);
    }
    void PinFrame(PageId page_id, storage::PinMode mode) noexcept override {
        if (watched_oid_ == 0 || mode != storage::PinMode::kExclusive) return;
        auto bytes = inner_.GetForReadUnpinned(page_id);
        if (bytes.ok() && storage::GetOwnerOid(bytes.value()) == watched_oid_) held_.insert(page_id);
    }

    // From now on, the pages `oid` owns that are pinned exclusive.
    void Watch(std::uint64_t oid) {
        watched_oid_ = oid;
        held_.clear();
    }
    std::size_t held() const noexcept { return held_.size(); }
    void FailNextFetch() noexcept { fail_next_ = true; }
    int failed() const noexcept { return failed_; }

private:
    bool Fails() noexcept {
        if (!fail_next_) return false;
        fail_next_ = false;
        ++failed_;
        return true;
    }

    storage::InMemoryPageStore& inner_;
    std::unordered_set<PageId> held_;
    std::uint64_t watched_oid_ = 0;
    bool fail_next_ = false;
    int failed_ = 0;
};

class KeySetWriteHoldTest : public ::testing::Test {
protected:
    static constexpr int kRows = 1200;

    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
        // w: ids 1..1200 in key order, v = 0 - several leaves, no index and
        // no Cabin, so the only pages the relation owns are its tree's.
        ASSERT_EQ(Run("CREATE TABLE w (id int64, v int64) BTREE").rfind("CREATED", 0), 0u);
        std::string inserted;
        for (int id = 1; id <= kRows; ++id) {
            inserted = Run("INSERT INTO w VALUES (" + std::to_string(id) + ", 0)");
            ASSERT_EQ(inserted.rfind("INSERTED", 0), 0u) << inserted;
        }
        const auto at = inserted.find("oid=");
        ASSERT_NE(at, std::string::npos) << inserted;
        oid_ = std::strtoull(inserted.c_str() + at + 4, nullptr, 10);

        // What a write walk holds: every leaf.
        store_.Watch(oid_);
        ASSERT_EQ(Run("UPDATE w SET v = 0 WHERE v < 0"), "UPDATED 0");
        every_leaf_ = store_.held();
        ASSERT_GE(every_leaf_, 4u) << "the relation is too small to tell a descent from a walk";
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    // The leaves `sql` took exclusive.
    std::size_t Held(const std::string& sql, const std::string& want) {
        store_.Watch(oid_);
        EXPECT_EQ(Run(sql), want) << sql;
        return store_.held();
    }

    storage::InMemoryPageStore inner_{kFirstUserPageId};
    LeafWatchingStore store_{inner_};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
    std::uint64_t oid_ = 0;
    std::size_t every_leaf_ = 0;
};

TEST_F(KeySetWriteHoldTest, AnUpdateByListHoldsOnlyTheLeavesItsKeysAreIn) {
    // Three neighbours: one leaf. The walk of the same list holds them all.
    EXPECT_EQ(Held("UPDATE w SET v = v + 1 WHERE id IN (5, 3, 4, 3, -1, NULL)", "UPDATED 3"), 1u);
    // A key past the relation is a descent too, to the last leaf, where the
    // tree answers that there is no such row.
    EXPECT_EQ(Held("UPDATE w SET v = v + 1 WHERE id IN (5, 3, 4, 999999)", "UPDATED 3"), 2u);
    EXPECT_EQ(Held("UPDATE w SET v = v + 1 WHERE " + Walked("id IN (5, 3, 4)", "id = 3"),
                   "UPDATED 3"),
              every_leaf_);
    // The first and the last row: two leaves.
    EXPECT_EQ(Held("UPDATE w SET v = v + 1 WHERE id = 1200 OR id = 1", "UPDATED 2"), 2u);
    EXPECT_EQ(Run("SELECT id, v FROM w WHERE v > 0"),
              "id,v\\n1,1\\n3,3\\n4,3\\n5,3\\n1200,1");
}

TEST_F(KeySetWriteHoldTest, ADeleteByListHoldsOnlyTheLeavesItsKeysAreIn) {
    EXPECT_EQ(Held("DELETE FROM w WHERE id IN (5, 3, 4, 3, -1, NULL)", "DELETED 3"), 1u);
    EXPECT_EQ(Held("DELETE FROM w WHERE " + Walked("id IN (8, 9)", "id = 8"), "DELETED 2"),
              every_leaf_);
    EXPECT_EQ(Held("DELETE FROM w WHERE id = 1200 OR id = 1", "DELETED 2"), 2u);
    // A deleted key's descent finds its delete-marked row and writes nothing.
    EXPECT_EQ(Held("DELETE FROM w WHERE id IN (3, 4, 5)", "DELETED 0"), 1u);
    EXPECT_EQ(Run("SELECT COUNT(*) FROM w"), "count(*)\\n" + std::to_string(kRows - 7));
}

// CU-S3b: the key's conjunct may stand beside any other but a subquery. The
// descent finds the slot and the whole WHERE is asked of its row, so each
// statement holds the leaves its keys are in and writes what its walk would.
TEST_F(KeySetWriteHoldTest, AKeyOrAListBesideOtherConjunctsHoldsOnlyItsLeaves) {
    EXPECT_EQ(Held("UPDATE w SET v = 7 WHERE id IN (3, 4) AND v = 0", "UPDATED 2"), 1u);
    EXPECT_EQ(Held("UPDATE w SET v = 8 WHERE id = 5 AND v = 0", "UPDATED 1"), 1u);
    // The row the descent finds fails the other conjunct: nothing is written.
    EXPECT_EQ(Held("UPDATE w SET v = 8 WHERE id = 5 AND v = 0", "UPDATED 0"), 1u);
    EXPECT_EQ(Held("UPDATE w SET v = 1 WHERE id IN (3, 5, 6) AND v = 8", "UPDATED 1"), 1u);
    // Wherever the key's conjunct is written, and beside a bound on the key.
    EXPECT_EQ(Held("UPDATE w SET v = 9 WHERE v = 1 AND id < 100 AND id = 5", "UPDATED 1"), 1u);
    // Two keys: the first is descended to, and the other excludes its row.
    EXPECT_EQ(Held("UPDATE w SET v = 2 WHERE id = 10 AND id = 11", "UPDATED 0"), 1u);
    // Two lists: one drives the descents and the other filters.
    EXPECT_EQ(Held("UPDATE w SET v = 2 WHERE id IN (15, 16) AND id IN (16, 17)", "UPDATED 1"),
              1u);
    // A single key beside a list is the single key's one descent: the list
    // alone would also descend to the last leaf.
    EXPECT_EQ(Held("UPDATE w SET v = 3 WHERE id IN (16, 1200) AND id = 16", "UPDATED 1"), 1u);
    EXPECT_EQ(Held("DELETE FROM w WHERE id IN (3, 4) AND v = 7", "DELETED 2"), 1u);
    EXPECT_EQ(Held("DELETE FROM w WHERE id = 5 AND v = 0", "DELETED 0"), 1u);
    EXPECT_EQ(Held("DELETE FROM w WHERE v = 9 AND id = 5", "DELETED 1"), 1u);
    EXPECT_EQ(Run("SELECT id, v FROM w WHERE v > 0"), "id,v\\n16,3");
}

// What still walks. A WHERE holding a subquery: its rows are collected by a
// read walk and written by a write walk, key or no key
// (docs/pending/keyed-write-beside-a-subquery.md). A bound on the tuple key,
// which declares a lock window and names no descent. A list on another
// column.
TEST_F(KeySetWriteHoldTest, AWriteBesideASubqueryOverABoundOrByAnotherColumnsListWalks) {
    EXPECT_EQ(Held("UPDATE w SET v = 4 WHERE id = 20 AND v IN (SELECT v FROM w WHERE id = 21)",
                   "UPDATED 1"),
              every_leaf_);
    EXPECT_EQ(Held("UPDATE w SET v = 5 WHERE id IN (22, 23) AND "
                   "EXISTS (SELECT id FROM w WHERE id = 1)",
                   "UPDATED 2"),
              every_leaf_);
    EXPECT_EQ(Held("DELETE FROM w WHERE id = 20 AND v IN (SELECT v FROM w WHERE id = 20)",
                   "DELETED 1"),
              every_leaf_);
    EXPECT_EQ(Held("UPDATE w SET v = 6 WHERE id BETWEEN 30 AND 31", "UPDATED 2"), every_leaf_);
    EXPECT_EQ(Held("UPDATE w SET v = 9 WHERE v IN (5, 6)", "UPDATED 4"), every_leaf_);
    // The single key alone is the point write it was.
    EXPECT_EQ(Held("UPDATE w SET v = 2 WHERE id = 6", "UPDATED 1"), 1u);
}

// A descent that declines - here a fetch that fails - hands the rest of the
// list to the walk from the key it declined at. Rows below it were written
// by their descents and are not offered again; `v = v + 1` shows a row
// written twice.
TEST_F(KeySetWriteHoldTest, ADescentThatDeclinesHandsTheRestToTheWalkFromItsKey) {
    dispatcher_->SetAfterUpdateRowAppliedForTest([this](std::uint64_t id) {
        if (id == 5) store_.FailNextFetch();
    });
    const std::size_t held =
        Held("UPDATE w SET v = v + 1 WHERE id IN (3, 5, 700, 1100, 1300)", "UPDATED 4");
    dispatcher_->SetAfterUpdateRowAppliedForTest(nullptr);
    EXPECT_EQ(store_.failed(), 1) << "the fetch was never failed, so no descent declined";
    // The walk took over at 700: more leaves than the list's keys are in,
    // fewer than the relation's, since the walk starts at its key's leaf.
    EXPECT_GT(held, 3u);
    EXPECT_LT(held, every_leaf_);
    EXPECT_EQ(Run("SELECT id, v FROM w WHERE v > 0"), "id,v\\n3,1\\n5,1\\n700,1\\n1100,1");
}

// The single key's descent declines, beside a conjunct and beside a list:
// the statement falls to the list's loop where there is one, and to the walk
// otherwise, and writes its row once.
TEST_F(KeySetWriteHoldTest, ASingleKeysDescentThatDeclinesIsAnsweredByTheListOrTheWalk) {
    store_.Watch(oid_);
    store_.FailNextFetch();
    EXPECT_EQ(Run("UPDATE w SET v = v + 1 WHERE id = 700 AND v = 0"), "UPDATED 1");
    EXPECT_EQ(store_.failed(), 1) << "the fetch was never failed, so no descent declined";
    EXPECT_EQ(store_.held(), every_leaf_);

    store_.Watch(oid_);
    store_.FailNextFetch();
    EXPECT_EQ(Run("UPDATE w SET v = v + 1 WHERE id = 3 AND id IN (3, 4)"), "UPDATED 1");
    EXPECT_EQ(store_.failed(), 2);
    EXPECT_EQ(store_.held(), 1u) << "the list's descents answered, not the walk";
    EXPECT_EQ(Run("SELECT id, v FROM w WHERE v > 0"), "id,v\\n3,1\\n700,1");
}

// DR-S3a (`WriteWalk::DescentDeclined`): a heap's walk resumes by place, not
// by key, so a list whose descent declines cannot hand it "the rest from this
// key". Before any row was offered the walk from the head is the same
// statement and writes each row once. After one was, the statement is
// refused - until DR it answered a count short of its rows, the walk having
// been started at no page.
TEST_F(KeySetWriteHoldTest, OnAHeapADeclinedDescentIsTheWalkFromTheHeadOrARefusal) {
    ASSERT_EQ(Run("CREATE TABLE hp (id int64 PRIMARY KEY, v int64) HEAP").rfind("CREATED", 0), 0u);
    for (int id = 1; id <= 40; ++id) {
        ASSERT_EQ(Run("INSERT INTO hp VALUES (" + std::to_string(id) + ", 0)").rfind("INSERTED", 0),
                  0u);
    }
    // The first value's descent declines: nothing was offered.
    const int failed_before = store_.failed();
    store_.FailNextFetch();
    EXPECT_EQ(Run("UPDATE hp SET v = v + 1 WHERE id IN (3, 5, 7)"), "UPDATED 3");
    EXPECT_EQ(store_.failed(), failed_before + 1) << "no descent declined";
    EXPECT_EQ(Run("SELECT id, v FROM hp WHERE v > 0 ORDER BY id"), "id,v\\n3,1\\n5,1\\n7,1");

    // A later value's descent declines: a row was offered, and the heap's
    // walk has no place to take the rest from.
    dispatcher_->SetAfterUpdateRowAppliedForTest([this](std::uint64_t id) {
        if (id == 11) store_.FailNextFetch();
    });
    const std::string refused = Run("UPDATE hp SET v = v + 1 WHERE id IN (11, 13, 15)");
    dispatcher_->SetAfterUpdateRowAppliedForTest(nullptr);
    EXPECT_EQ(refused.rfind("ERR", 0), 0u) << refused;
    EXPECT_NE(refused.find("declined after rows of the statement were offered"),
              std::string::npos)
        << refused;
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

// ---- CU-S3 under transactions: every write route by list ------------------------

TEST_F(KeySetTxnTest, AWriteByListIsItsTransactionsUntilItDecides) {
    Session writer;
    Session reader;
    ASSERT_EQ(Run(0, writer, "BEGIN").rfind("BEGIN", 0), 0u);
    EXPECT_EQ(Run(0, writer, "UPDATE t SET v = v + 1 WHERE id IN (2, 3, 9)"), "UPDATED 2");
    EXPECT_EQ(Run(0, writer, "DELETE FROM t WHERE id = 5 OR id = 1"), "DELETED 2");
    // Its own rows by its own list, written once more through its own
    // versions; the other core's reader sees none of it.
    EXPECT_EQ(Run(0, writer, "UPDATE t SET v = v + 1 WHERE id IN (3, 5)"), "UPDATED 1");
    EXPECT_EQ(Run(0, writer, "SELECT id, v FROM t"), "id,v\\n2,21\\n3,32\\n4,40");
    EXPECT_EQ(Run(1, reader, "SELECT id, v FROM t"),
              "id,v\\n1,10\\n2,20\\n3,30\\n4,40\\n5,50");
    ASSERT_EQ(Run(0, writer, "ROLLBACK").rfind("ROLLBACK", 0), 0u);
    EXPECT_EQ(Run(1, reader, "SELECT id, v FROM t"),
              "id,v\\n1,10\\n2,20\\n3,30\\n4,40\\n5,50");

    // Committed from the other core.
    EXPECT_EQ(Run(1, writer, "UPDATE t SET v = v + 1 WHERE id IN (4, 2)"), "UPDATED 2");
    EXPECT_EQ(Run(1, writer, "DELETE FROM t WHERE id IN (1, 5, 5)"), "DELETED 2");
    EXPECT_EQ(Run(0, reader, "SELECT id, v FROM t"), "id,v\\n2,21\\n3,30\\n4,41");
}

TEST_F(KeySetTxnTest, ReturningAnswersTheRowsAWriteByListWrote) {
    Session s;
    EXPECT_EQ(Run(0, s, "UPDATE t SET v = v + 1 WHERE id IN (4, 2, 9) RETURNING id, v"),
              "id,v\\n2,21\\n4,41\\nUPDATED 2");
    EXPECT_EQ(Run(0, s, "DELETE FROM t WHERE id = 5 OR id = 1 RETURNING id, v"),
              "id,v\\n1,10\\n5,50\\nDELETED 2");
}

// The checks a row's write runs are the row function's, which a descent and
// a walk share: a refusal on the list's second row unwinds its first.
TEST_F(KeySetTxnTest, AWriteByListRunsEveryCheckTheWalkRuns) {
    Session s;
    const auto refused = [&](const std::string& sql, const std::string& needle) {
        const std::string reply = Run(0, s, sql);
        EXPECT_EQ(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
        EXPECT_NE(reply.find(needle), std::string::npos) << sql << " -> " << reply;
    };
    const auto ok = [&](const std::string& sql) {
        const std::string reply = Run(0, s, sql);
        ASSERT_NE(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
    };

    // A foreign key, both directions.
    ok("CREATE TABLE p (id int64, n int64) BTREE");
    ok("INSERT INTO p VALUES (1, 0), (2, 0), (3, 0)");
    ok("CREATE TABLE c (id int64, pid int64 REFERENCES p) BTREE");
    ok("INSERT INTO c VALUES (1, 1), (2, 1), (3, 3)");
    refused("DELETE FROM p WHERE id IN (2, 3)", "c");  // 3 is referenced
    EXPECT_EQ(Run(0, s, "SELECT id FROM p"), "id\\n1\\n2\\n3");
    refused("UPDATE c SET pid = 9 WHERE id IN (1, 2)", "9");  // no such parent
    EXPECT_EQ(Run(0, s, "UPDATE c SET pid = 2 WHERE id IN (1, 2)"), "UPDATED 2");
    EXPECT_EQ(Run(0, s, "DELETE FROM p WHERE id IN (1, 4)"), "DELETED 1");
    EXPECT_EQ(Run(0, s, "SELECT id, pid FROM c"), "id,pid\\n1,2\\n2,2\\n3,3");

    // A unique index: the second row takes the first row's new key.
    ok("CREATE TABLE q (id int64, k int64) BTREE");
    ok("INSERT INTO q VALUES (1, 10), (2, 20), (3, 30)");
    ok("CREATE UNIQUE INDEX q_by_k ON q (k)");
    refused("UPDATE q SET k = 99 WHERE id IN (1, 2)", "q_by_k");
    EXPECT_EQ(Run(0, s, "SELECT id, k FROM q"), "id,k\\n1,10\\n2,20\\n3,30");
    EXPECT_EQ(Run(0, s, "UPDATE q SET k = k + 5 WHERE id IN (1, 2)"), "UPDATED 2");
    EXPECT_EQ(Run(0, s, "SELECT id FROM q WHERE k IN (15, 25)"), "id\\n1\\n2");

    // An assertion: the list's rows together pass the bound the second breaks.
    ok("CREATE TABLE a (id int64, acct int64, qty int64) BTREE");
    ok("INSERT INTO a VALUES (1, 7, 10), (2, 7, 10), (3, 7, 10)");
    ok("CREATE ASSERTION cap ON a GROUP BY (acct) CHECK SUM(qty) <= 100");
    refused("UPDATE a SET qty = 50 WHERE id IN (1, 2, 3)", "cap");
    EXPECT_EQ(Run(0, s, "SELECT id, qty FROM a"), "id,qty\\n1,10\\n2,10\\n3,10");
    EXPECT_EQ(Run(0, s, "UPDATE a SET qty = 40 WHERE id IN (1, 3)"), "UPDATED 2");
    EXPECT_EQ(Run(0, s, "DELETE FROM a WHERE id IN (1, 3)"), "DELETED 2");
    EXPECT_EQ(Run(0, s, "UPDATE a SET qty = 100 WHERE id IN (2)"), "UPDATED 1");

    // A Transition: one listed row is in a state no edge leaves.
    ok("CREATE TABLE f (id int64, st varchar(16)) BTREE");
    ok("INSERT INTO f VALUES (1, 'new'), (2, 'new'), (3, 'new')");
    ok("CREATE TRANSITION flow ON f(st) ON INSERT ('new') ON UPDATE ('new' => 'done', "
       "'new' => 'held')");
    EXPECT_EQ(Run(0, s, "UPDATE f SET st = 'done' WHERE id IN (2)"), "UPDATED 1");
    refused("UPDATE f SET st = 'held' WHERE id IN (1, 2, 3)", "flow");
    EXPECT_EQ(Run(0, s, "SELECT id, st FROM f"), "id,st\\n1,new\\n2,done\\n3,new");
    EXPECT_EQ(Run(0, s, "UPDATE f SET st = 'held' WHERE id IN (1, 3)"), "UPDATED 2");
}

// `PURGE` by list stays refused: CU-Q5 names UPDATE and DELETE only.
TEST_F(KeySetTxnTest, APurgeByListIsStillRefused) {
    Session s;
    ASSERT_EQ(Run(0, s, "DELETE FROM t WHERE id IN (2, 3)"), "DELETED 2");
    const std::string reply = Run(0, s, "PURGE FROM t WHERE id IN (2, 3)");
    EXPECT_EQ(reply.rfind("ERR", 0), 0u) << reply;
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

// ---- DT: a declined descent, then a walk that gives its core back -----------
//
// A walk pauses between two leaves and its statement is dispatched again
// (`instructions/v3.2.0/workorder-dt-write-path-yield.md` §2.3). **The
// resumed run is past its single-key arm** (DT-R3): the arm's descent
// declined, the walk took the statement from the head and wrote the key's row
// in its first slice, and a run that offered the key again would find the
// row under the statement's own version, pass its `WHERE` and write it a
// second time. A pause needs a transaction and a lock table, which
// `KeySetWriteHoldTest`'s bare dispatcher has none of; the store that fails
// one fetch is the same.
class KeyedWritePauseTest : public ::testing::Test {
protected:
    // `txns_` gives its borrows back through the table at teardown, so the
    // table is declared first and dies last (`lock_family_fixture.hpp`).
    std::unique_ptr<txn::LockTable> locks_;

    void SetUp() override {
        auto device = wal::MemoryLogDevice::Create(1 << 20);
        ASSERT_TRUE(device.ok()) << device.status().message();
        log_device_ = std::move(device.value());
        wal::WalManagerConfig config;
        config.ring_capacity = wal::kMinRingCapacity;
        auto wal = wal::WalManager::Open(log_device_.get(), clock_, /*core_id=*/0, config);
        ASSERT_TRUE(wal.ok()) << wal.status().message();
        wal_ = std::move(wal.value());
        auto boot = bootstrap::BootstrapDatabase(store_, /*now_unix_seconds=*/1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, wal_.get());
        auto table = txn::LockTable::Create(/*core_count=*/1, txn::kMaxLocksPerTxnDefault);
        ASSERT_TRUE(table.ok()) << table.status().message();
        locks_ = std::move(table.value());
        txns_.emplace(*ids_, *undo_, store_, wal_.get(), /*visibility=*/nullptr, /*core=*/0,
                      locks_.get());
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr, &clock_,
                            wal_.get(), wal::DurabilityClass::kRelaxed, exec::Budget(),
                            /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, &*txns_);
        dispatcher_->set_locks(locks_.get());
    }

    std::string Run(const std::string& sql, Session* s = nullptr) {
        return dispatcher_->Dispatch(sql, s).response;
    }

    storage::InMemoryPageStore inner_{kFirstUserPageId};
    LeafWatchingStore store_{inner_};
    sched::ManualClock clock_;
    std::unique_ptr<wal::MemoryLogDevice> log_device_;
    std::unique_ptr<wal::WalManager> wal_;
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> txns_;
    std::optional<CommandDispatcher> dispatcher_;
};

TEST_F(KeyedWritePauseTest, AResumedWalkDoesNotOfferItsSingleKeyAgain) {
    for (const std::string form : {"HEAP", "BTREE"}) {
        // About seven rows a page, so 300 rows are some forty leaves: two
        // pauses at the `analytic` level's slice of sixteen.
        const std::string t = form == "HEAP" ? "ph" : "pb";
        ASSERT_EQ(Run("CREATE TABLE " + t + " (id int64 PRIMARY KEY, v int64, pad char(1000)) " +
                      form)
                      .rfind("CREATED", 0),
                  0u);
        for (int at = 1; at <= 300;) {
            std::string values;
            for (int n = 0; n < 50 && at <= 300; ++n, ++at) {
                values += (values.empty() ? "(" : ", (") + std::to_string(at) + ", 0, 'p')";
            }
            ASSERT_EQ(Run("INSERT INTO " + t + " VALUES " + values).rfind("INSERTED", 0), 0u);
        }
        Session s;
        ASSERT_EQ(Run("SET PRIORITY analytic", &s), "SET priority=analytic");

        const int failed_before = store_.failed();
        const std::uint64_t pauses_before = dispatcher_->write_walk_pauses();
        // The key's descent declines; the walk from the head writes row 5 in
        // its first slice and pauses after it.
        store_.FailNextFetch();
        EXPECT_EQ(Run("UPDATE " + t + " SET v = v + 1 WHERE id = 5", &s), "UPDATED 1") << form;
        EXPECT_EQ(store_.failed(), failed_before + 1) << form << ": no descent declined";
        EXPECT_GE(dispatcher_->write_walk_pauses() - pauses_before, 1u)
            << form << ": the walk took no pause, so no run was a resume";
        EXPECT_EQ(Run("SELECT id, v FROM " + t + " WHERE v > 0"), "id,v\\n5,1")
            << form << ": the resumed run wrote the key's row again";

        store_.FailNextFetch();
        EXPECT_EQ(Run("DELETE FROM " + t + " WHERE id = 7", &s), "DELETED 1") << form;
        EXPECT_EQ(Run("SELECT COUNT(*) FROM " + t), "count(*)\\n299") << form;
    }
}

}  // namespace
}  // namespace kds::server
