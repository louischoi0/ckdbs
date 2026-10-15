#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/statement_stats.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/lock_table.hpp"
#include "kds/txn/manager.hpp"
#include "view_text.hpp"

// BW-S6 (`instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`,
// BW-R13, BW-Q12): `INSERT INTO t (c1, c2, ...) VALUES (...)` names its
// columns, so no column change can shift its values (BW-Q11's gap closes for
// a listed row). Each cell names the BW-Q12 item it pins.

namespace kds::server {
namespace {

bool IsErr(const std::string& reply) { return reply.rfind("ERR", 0) == 0; }

class InsertColumnListTest : public ::testing::Test {
protected:
    void SetUp() override { Build(kDefaultMaxRows); }

    void Build(std::size_t max_insert_rows) {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
        cabins_.emplace();
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        auto table = txn::LockTable::Create(/*core_count=*/1, /*max_locks_per_txn=*/1024);
        ASSERT_TRUE(table.ok()) << table.status().message();
        locks_ = std::move(table.value());
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr, /*visibility=*/nullptr, /*core=*/0,
                     locks_.get());
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, &*cabins_, &*mgr_,
                            txn::IsolationLevel::kReadCommitted, /*core_id=*/0,
                            /*indexes=*/true, max_insert_rows);
        dispatcher_->set_locks(locks_.get());
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    std::string Run(Session& s, const std::string& sql) {
        return dispatcher_->Dispatch(sql, &s).response;
    }
    void Ok(const std::string& sql) {
        const std::string out = Run(sql);
        EXPECT_FALSE(IsErr(out)) << sql << " -> " << out;
    }
    // Refused, with every needle in the reply.
    void Refused(const std::string& sql, std::initializer_list<std::string> needles) {
        const std::string out = Run(sql);
        EXPECT_TRUE(IsErr(out)) << sql << " -> " << out;
        for (const std::string& needle : needles) {
            EXPECT_NE(out.find(needle), std::string::npos) << sql << " -> " << out
                                                           << "\n  missing: " << needle;
        }
    }

    static constexpr std::size_t kDefaultMaxRows = 1024;

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::CabinStore> cabins_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::unique_ptr<txn::LockTable> locks_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- BW-Q12.1, Q12.6, Q12.8: values land in their named columns -----------

TEST_F(InsertColumnListTest, AListInAnotherOrderLandsEachValueInItsColumn) {
    Ok("CREATE TABLE t (id int64, a int64, b varchar, c int32)");
    Ok("INSERT INTO t (c, b, a) VALUES (3, 'three', 1)");
    Ok("INSERT INTO t (b, c, a) VALUES ('x', 30, 10), ('y', 40, 20)");
    EXPECT_EQ(Run("SELECT * FROM t"), "id,a,b,c\\n1,1,three,3\\n2,10,x,30\\n3,20,y,40");
}

TEST_F(InsertColumnListTest, TheTupleKeyIsIssuedWhenLeftOutOrNullAndPlacedWhenNamed) {
    Ok("CREATE TABLE t (id int64, v int64)");
    Ok("INSERT INTO t (v) VALUES (1)");                   // left out: issued
    Ok("INSERT INTO t (v, id) VALUES (2, 100)");          // named, last position
    Ok("INSERT INTO t (id, v) VALUES (50, 3)");           // named, first position
    Ok("INSERT INTO t (id, v) VALUES (NULL, 4)");         // NULL: issued
    Ok("INSERT INTO t (v, id) VALUES (5, NULL), (6, 7)");  // mixed per row
    // An issued id is the mark above every id placed (invariant 11): 101, 102.
    EXPECT_EQ(Run("SELECT * FROM t"),
              "id,v\\n1,1\\n7,6\\n50,3\\n100,2\\n101,4\\n102,5");

    // The existing key refusals, unchanged, at the value's byte.
    //                                  0         1         2         3
    //                                  0123456789012345678901234567890123
    const std::string dup = Run("INSERT INTO t (v, id) VALUES (9, 100)");
    EXPECT_TRUE(IsErr(dup)) << dup;
    EXPECT_NE(dup.find("byte 33"), std::string::npos) << dup;
    const std::string range = Run("INSERT INTO t (v, id) VALUES (9, 0)");
    EXPECT_TRUE(IsErr(range)) << range;
    EXPECT_NE(range.find("byte 33"), std::string::npos) << range;
}

// A relation of the tuple key alone: a listed `NULL` key is a row with an
// empty body, and a named one is the key and nothing else.
TEST_F(InsertColumnListTest, AKeyOnlyRelationTakesAListedKey) {
    Ok("CREATE TABLE k (id int64)");
    Ok("INSERT INTO k (id) VALUES (NULL)");
    Ok("INSERT INTO k (id) VALUES (9), (NULL)");
    EXPECT_EQ(Run("SELECT * FROM k"), "id\\n1\\n9\\n10");
}

TEST_F(InsertColumnListTest, MaxInsertRowsCountsAListedStatementsRows) {
    Build(/*max_insert_rows=*/2);
    Ok("CREATE TABLE t (id int64, v int64)");
    Ok("INSERT INTO t (v) VALUES (1), (2)");
    Refused("INSERT INTO t (v) VALUES (1), (2), (3)", {"max_insert_rows (2)"});
    // The cap is asked before any name (BW-Q12.12 step 2).
    Refused("INSERT INTO t (nope) VALUES (1), (2), (3)", {"max_insert_rows (2)"});
    EXPECT_EQ(Run("SELECT v FROM t"), "v\\n1\\n2");
}

// ---- BW-Q12.7: omitted columns --------------------------------------------

TEST_F(InsertColumnListTest, AnOmittedNullableColumnIsNullAndAnOmittedNotNullOneIsRefused) {
    Ok("CREATE TABLE t (id int64, a int64, n varchar NULL, m int64 NULL)");
    Ok("INSERT INTO t (a) VALUES (1)");
    Ok("INSERT INTO t (m, a) VALUES (7, 2)");
    EXPECT_EQ(Run("SELECT * FROM t"), "id,a,n,m\\n1,1,NULL,NULL\\n2,2,NULL,7");

    //            0         1         2
    //            0123456789012345678901234
    Refused("INSERT INTO t (n, m) VALUES ('x', 1), ('y', 2)",
            {"'a' is NOT NULL", "leaves it out (byte 14); there is no DEFAULT"});
    EXPECT_EQ(Run("SELECT COUNT(*) FROM t"), "count(*)\\n2") << "nothing written";
}

TEST_F(InsertColumnListTest, AnOmittedTransitionColumnIsRefusedLikeAnyNotNullOne) {
    Ok("CREATE TABLE t (id int64, st varchar, v int64)");
    Ok("CREATE TRANSITION flow ON t(st) ON INSERT ('new') ON UPDATE ('new' => 'done')");
    Refused("INSERT INTO t (v) VALUES (1)", {"'st' is NOT NULL", "(byte 14)"});
    Ok("INSERT INTO t (v, st) VALUES (1, 'new')");
    Refused("INSERT INTO t (v, st) VALUES (2, 'done')", {"transition"});
    EXPECT_EQ(Run("SELECT * FROM t"), "id,st,v\\n1,new,1");
}

// ---- BW-Q12.3, Q12.4: names ---------------------------------------------

TEST_F(InsertColumnListTest, BadNamesAreRefusedAtTheirByte) {
    Ok("CREATE TABLE t (id int64, a int64, d int64 NULL)");
    Ok("ALTER TABLE t DROP COLUMN d");
    //            0         1         2
    //            012345678901234567890123
    Refused("INSERT INTO t (a, x) VALUES (1, 2)", {"unknown column 'x'", "at byte 18"});
    Refused("INSERT INTO t (a, d) VALUES (1, 2)", {"unknown column 'd'", "at byte 18"});
    Refused("INSERT INTO t (a, a) VALUES (1, 2)", {"'a' is listed twice", "at byte 18"});
    // Case is ignored (raft-marks-2026-10-15.md §1, overriding BW-Q12.3):
    // `A` is `a`, and so `(a, A)` lists one column twice.
    Refused("INSERT INTO t (a, A) VALUES (1, 2)", {"'A' is listed twice", "at byte 18"});
    // In written order: the first bad name is the one named.
    Refused("INSERT INTO t (y, a, a) VALUES (1, 2, 3)", {"unknown column 'y'", "at byte 15"});
    EXPECT_EQ(Run("SELECT COUNT(*) FROM t"), "count(*)\\n0");
    Ok("INSERT INTO t (A) VALUES (1)");
    EXPECT_EQ(Run("SELECT a FROM t"), "a\\n1");
}

// Two names that differ only in case cannot be declared any more (column
// names are unique ignoring case); on a relation created before that rule a
// list naming such a pair is refused as ambiguous, which
// `column_name_case_test.cpp` pins.

// ---- BW-Q12.8: row width ---------------------------------------------------

TEST_F(InsertColumnListTest, ARowOfAnotherWidthThanTheListIsRefusedAtItsFirstValue) {
    Ok("CREATE TABLE t (id int64, a int64, b int64)");
    //            0         1         2         3
    //            0123456789012345678901234567890123456789
    Refused("INSERT INTO t (a, b) VALUES (1, 2, 3)",
            {"expected 2 value(s), one per listed column; got 3", "at byte 29"});
    Refused("INSERT INTO t (a, b) VALUES (1, 2), (3)",
            {"expected 2 value(s)", "got 1", "at byte 37", "(row 2)"});
    // No key-less short width as positional rows have: the key is listed.
    Refused("INSERT INTO t (id, a, b) VALUES (1, 2)", {"expected 3 value(s)"});
    EXPECT_EQ(Run("SELECT COUNT(*) FROM t"), "count(*)\\n0");
}

// ---- BW-Q11's scenario: no column change shifts a listed row ---------------

TEST_F(InsertColumnListTest, AfterAnAddAListWithoutTheNewColumnWritesNullThere) {
    Ok("CREATE TABLE t (id int64, v int64) SPARE 2");
    Ok("ALTER TABLE t ADD COLUMN a int64 NULL");
    Ok("INSERT INTO t (v) VALUES (5)");
    Ok("INSERT INTO t (a, v) VALUES (7, 6)");
    EXPECT_EQ(Run("SELECT * FROM t"), "id,v,a\\n1,5,NULL\\n2,6,7");
}

TEST_F(InsertColumnListTest, AfterADropAListedRowLandsInItsColumnsWhereAPositionalOneShifts) {
    Ok("CREATE TABLE t (id int64, a int64, b int64, c int64)");
    Ok("ALTER TABLE t DROP COLUMN a");
    // BW-Q11: a row written for the old columns with its key left out,
    // `(a, b, c)`, has the new full length - positional, it lands with its
    // first value as the key. Listed, every value is in its column.
    Ok("INSERT INTO t (b, c) VALUES (20, 30)");
    Ok("INSERT INTO t (c, b) VALUES (31, 21)");
    EXPECT_EQ(Run("SELECT * FROM t"), "id,b,c\\n1,20,30\\n2,21,31");
}

TEST_F(InsertColumnListTest, AfterADropAndAnAddAListedRowLandsInTheRightColumns) {
    Ok("CREATE TABLE t (id int64, a int64, b int64) SPARE 1");
    Ok("ALTER TABLE t DROP COLUMN a");
    Ok("ALTER TABLE t ADD COLUMN c int64 NULL");
    Ok("INSERT INTO t (b, c) VALUES (2, 3)");
    Ok("INSERT INTO t (c, b) VALUES (5, 4)");
    Ok("INSERT INTO t (b) VALUES (6)");
    EXPECT_EQ(Run("SELECT * FROM t"), "id,b,c\\n1,2,3\\n2,4,5\\n3,6,NULL");
}

// BW-Q12.9: BW-Q9's positional refusals stand, on the same relation.
TEST_F(InsertColumnListTest, PositionalRowsKeepBwQ9sRefusalsBesideAList) {
    Ok("CREATE TABLE t (id int64, v int64) SPARE 2");
    Ok("ALTER TABLE t ADD COLUMN a int64 NULL");
    Refused("INSERT INTO t VALUES (9, 8)", {"changed by ALTER TABLE"});
    Ok("INSERT INTO t VALUES (NULL, 9, 8)");
    Ok("INSERT INTO t (v, a) VALUES (9, 8)");
    EXPECT_EQ(Run("SELECT * FROM t"), "id,v,a\\n1,9,8\\n2,9,8");
}

// ---- Downstream consumers, with a reordered list (BW-Q12.9) ----------------

TEST_F(InsertColumnListTest, AForeignKeyListedOutOfOrderIsCheckedAndAnOmittedNullableOneIsNot) {
    Ok("CREATE TABLE p (id int64, v int64)");
    Ok("INSERT INTO p VALUES (1)");
    Ok("CREATE TABLE c (id int64, w int64, pid int64 NULL REFERENCES p)");
    Ok("INSERT INTO c (pid, w) VALUES (1, 99)");
    const std::string missing = Run("INSERT INTO c (pid, w) VALUES (2, 1)");
    EXPECT_EQ(missing.substr(0, 16), "ERR FK_VIOLATION") << missing;
    // Read positionally, `(2, 1)` would have checked `1` - a parent that
    // exists - and passed. Left out, `pid` is NULL and checks nothing.
    Ok("INSERT INTO c (w) VALUES (2)");
    EXPECT_EQ(Run("SELECT * FROM c"), "id,w,pid\\n1,99,1\\n2,2,NULL");
}

TEST_F(InsertColumnListTest, AnIndexOnAColumnListedOutOfOrderGetsTheRightKey) {
    Ok("CREATE TABLE t (id int64, k int64, c int64)");
    Ok("CREATE INDEX ix_k ON t (k)");
    Ok("INSERT INTO t (c, k) VALUES (100, 7), (200, 8)");
    EXPECT_EQ(Run("SELECT c FROM t WHERE k = 7"), "c\\n100");
    EXPECT_EQ(Run("SELECT c FROM t WHERE k = 8"), "c\\n200");
    EXPECT_EQ(Run("SELECT c FROM t WHERE k = 100"), "c");
}

TEST_F(InsertColumnListTest, AnAssertionAdmitsTheListedBody) {
    Ok("CREATE TABLE q (id int64, v int64, w int64)");
    Ok("CREATE ASSERTION cap ON q GROUP BY (v) CHECK COUNT(*) <= 1");
    Ok("INSERT INTO q (w, v) VALUES (5, 1)");
    Ok("INSERT INTO q (w, v) VALUES (5, 2)");  // v differs; w is the same
    const std::string over = Run("INSERT INTO q (w, v) VALUES (6, 1)");
    EXPECT_TRUE(IsErr(over)) << over;
    EXPECT_NE(over.find("ASSERTION_VIOLATION"), std::string::npos) << over;
    EXPECT_EQ(Run("SELECT * FROM q"), "id,v,w\\n1,1,5\\n2,2,5");
}

TEST_F(InsertColumnListTest, ARolledBackListedInsertLeavesNothing) {
    Ok("CREATE TABLE t (id int64, a int64, b int64 NULL)");
    Session session;
    ASSERT_FALSE(IsErr(Run(session, "BEGIN")));
    ASSERT_FALSE(IsErr(Run(session, "INSERT INTO t (a) VALUES (1), (2)")));
    EXPECT_EQ(Run(session, "SELECT * FROM t"), "id,a,b\\n1,1,NULL\\n2,2,NULL");
    ASSERT_FALSE(IsErr(Run(session, "ROLLBACK")));
    EXPECT_EQ(Run("SELECT * FROM t"), "id,a,b");
}

// ---- BW-Q12.10: the fingerprint --------------------------------------------

TEST_F(InsertColumnListTest, OneListIsOneStatisticsKeyWhateverItsRowsAndTwoOrdersAreTwo) {
    Ok("CREATE TABLE t (id int64, a int64, b int64)");
    stats::StatementStats::ResetForTest();
    Ok("INSERT INTO t (a, b) VALUES (1, 2)");
    Ok("INSERT INTO t (a, b) VALUES (3, 4), (5, 6), (7, 8)");
    Ok("INSERT INTO t (b, a) VALUES (1, 2)");

    const auto ab = parser::FingerprintOf("INSERT INTO t (a, b) VALUES (1, 2)");
    const auto ab3 = parser::FingerprintOf("INSERT INTO t (a, b) VALUES (3, 4), (5, 6), (7, 8)");
    const auto ba = parser::FingerprintOf("INSERT INTO t (b, a) VALUES (1, 2)");
    ASSERT_TRUE(ab.has_value() && ab3.has_value() && ba.has_value());
    EXPECT_EQ(ab->fetch_id, ab3->fetch_id);
    EXPECT_NE(ab->fetch_id, ba->fetch_id);

    const std::string reply =
        Run("SELECT fetch_id, calls FROM sys.statement_stats WHERE kind = 'INSERT'");
    const auto shown = testing_view::ReadViewText(reply);
    ASSERT_EQ(shown.rows.size(), 2u) << reply;
    EXPECT_EQ(shown.Find("fetch_id", std::to_string(ab->fetch_id), "calls"), "2") << reply;
    EXPECT_EQ(shown.Find("fetch_id", std::to_string(ba->fetch_id), "calls"), "1") << reply;
}

}  // namespace
}  // namespace kds::server
