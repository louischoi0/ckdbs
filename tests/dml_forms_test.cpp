#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"

// BU-S8 (instructions/v3.0.0/workorder-bu-basic-query-surface.md): the DML
// forms - expressions in VALUES, INSERT ... SELECT, ON CONFLICT DO NOTHING
// (BU-Q18), UPDATE ... FROM and DELETE ... USING (BU-Q16), RETURNING
// (BU-Q17). The dataset is anti-kdbs F004's, `w` a copy of `cust` each test
// writes, and each expected answer the one PostgreSQL gives (BU-R1).

namespace kds::server {
namespace {

class DmlFormsTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        // A multi-row write unwinds through the manager (bulkinsert.md BI4).
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, /*cabins=*/nullptr, &*mgr_);

        for (const char* table : {"cust", "w"}) {
            Ok(std::string("CREATE TABLE ") + table +
               " (id int64, name varchar(32), region varchar(16), tier int64 NULL)");
            Ok(std::string("INSERT INTO ") + table +
               " VALUES (1, 'alice', 'EU', 1), (2, 'bob', 'US', 2), (3, 'carol', 'EU', NULL), "
               "(4, 'dave', 'APAC', 3), (5, 'erin', 'US', NULL), (6, 'frank', 'EU', 2)");
        }
        Ok("CREATE TABLE ord (id int64, cust_id int64, status varchar(16), amount int64)");
        Ok("INSERT INTO ord VALUES (10, 1, 'open', 100), (11, 1, 'paid', 250), "
           "(12, 2, 'paid', 75), (13, 2, 'cancel', 300), (14, 4, 'open', 50), "
           "(15, 4, 'paid', 500), (16, 6, 'open', 120), (17, 99, 'open', 10)");
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    void Ok(const std::string& sql) {
        const std::string reply = Run(sql);
        ASSERT_NE(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
    }
    static std::vector<std::string> Lines(const std::string& reply) {
        std::vector<std::string> out;
        std::size_t at = 0;
        for (;;) {
            const std::size_t next = reply.find("\\n", at);
            out.push_back(reply.substr(at, next == std::string::npos ? std::string::npos : next - at));
            if (next == std::string::npos) break;
            at = next + 2;
        }
        return out;
    }
    // A SELECT's header and rows, the rows in byte order.
    void ExpectRows(const std::string& sql, std::vector<std::string> want) {
        const std::string reply = Run(sql);
        ASSERT_NE(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
        std::vector<std::string> got = Lines(reply);
        if (got.size() > 1) std::sort(got.begin() + 1, got.end());
        if (want.size() > 1) std::sort(want.begin() + 1, want.end());
        EXPECT_EQ(got, want) << sql;
    }
    void ExpectReply(const std::string& sql, const std::string& want) {
        EXPECT_EQ(Run(sql), want) << sql;
    }
    void ExpectRefused(const std::string& sql, const std::string& needle) {
        const std::string reply = Run(sql);
        EXPECT_EQ(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
        EXPECT_NE(reply.find(needle), std::string::npos) << sql << " -> " << reply;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- Expressions in VALUES --------------------------------------------------

TEST_F(DmlFormsTest, AValuesExpressionIsTypedForItsColumnAndFoldedOnce) {
    Ok("INSERT INTO w VALUES (30, 'h' || 'al', 'US', 1 + 1)");
    ExpectRows("SELECT * FROM w WHERE id = 30", {"id,name,region,tier", "30,hal,US,2"});
    // The tuple key's value too, and a listed column's.
    Ok("INSERT INTO w (region, name, id) VALUES (UPPER('eu'), 'x' || 'y', 10 * 4)");
    ExpectRows("SELECT * FROM w WHERE id = 40", {"id,name,region,tier", "40,xy,EU,NULL"});
    // Several rows, an expression in each.
    Ok("INSERT INTO w VALUES (50, 'a', 'US', 2 * 3), (51, 'b' || 'b', 'EU', NULL)");
    ExpectRows("SELECT id, name, tier FROM w WHERE id >= 50",
               {"id,name,tier", "50,a,6", "51,bb,NULL"});
}

TEST_F(DmlFormsTest, AValuesExpressionIsRefusedForWhatItCannotBe) {
    ExpectRefused("INSERT INTO w VALUES (31, name, 'US', 1)", "reads no column");
    ExpectRefused("INSERT INTO w VALUES (32, 'a', 'US', 'x' || 'y')", "not int64");
    ExpectRefused("INSERT INTO w VALUES (33, 'a', 'US', 1 / 0)", "division by zero");
    ExpectRefused("INSERT INTO w VALUES (34, 'a', 'US', (SELECT MAX(tier) FROM cust))",
                  "NOT_IMPLEMENTED");
    ExpectRows("SELECT COUNT(*) FROM w", {"count(*)", "6"});
}

// ---- An UPDATE's or DELETE's WHERE ---------------------------------------------

// The WHERE is the native grammar's (parser-v2.md I20): what only a SELECT's
// query layer reads is declined `NotImplemented` with its byte, nothing is
// written, and the same WHERE in a SELECT still answers.
TEST_F(DmlFormsTest, AnExpressionInADmlWhereIsNotImplementedAndWritesNothing) {
    ExpectRows("SELECT id FROM w WHERE tier + 1 = 3", {"id", "2", "6"});
    ExpectRefused("UPDATE w SET tier = 9 WHERE tier + 1 = 3", "NOT_IMPLEMENTED");
    ExpectRefused("UPDATE w SET tier = 9 WHERE tier + 1 = 3", "(byte 33)");
    ExpectRefused("DELETE FROM w WHERE 1 = 0", "NOT_IMPLEMENTED");
    ExpectRefused("DELETE FROM w USING ord WHERE w.id = ord.cust_id + 0", "NOT_IMPLEMENTED");
    ExpectRows("SELECT id, tier FROM w WHERE tier = 9", {"id,tier"});
    ExpectRows("SELECT COUNT(*) FROM w", {"count(*)", "6"});
}

// ---- INSERT ... SELECT --------------------------------------------------------

TEST_F(DmlFormsTest, InsertSelectPlacesTheRowsItsSourceReturns) {
    ExpectReply("INSERT INTO w SELECT id + 100, name, region, tier FROM cust WHERE region = 'US'",
                "INSERTED oid=4005 rows=2 first_id=102 last_id=105");
    ExpectRows("SELECT * FROM w WHERE id > 100",
               {"id,name,region,tier", "102,bob,US,2", "105,erin,US,NULL"});
    // ORDER BY and LIMIT shape the source.
    Ok("INSERT INTO w SELECT id + 200, name, region, tier FROM cust ORDER BY id LIMIT 2");
    ExpectRows("SELECT id FROM w WHERE id > 200", {"id", "201", "202"});
    // A listed target, the tuple key left out and issued.
    Ok("INSERT INTO w (name, region) SELECT name, region FROM cust WHERE id = 3");
    ExpectRows("SELECT COUNT(*) FROM w WHERE name = 'carol'", {"count(*)", "2"});
    ExpectReply("INSERT INTO w SELECT id, name, region, tier FROM cust WHERE id > 99",
                "INSERTED oid=4005 rows=0");
}

// The source is read whole before a row is placed, so a statement reading
// the relation it writes reads only the rows it found (the Halloween problem).
TEST_F(DmlFormsTest, InsertSelectFromItsOwnRelationReadsOnlyWhatWasThere) {
    ExpectReply("INSERT INTO w SELECT id + 1000, name, region, tier FROM w",
                "INSERTED oid=4005 rows=6 first_id=1001 last_id=1006");
    ExpectRows("SELECT COUNT(*) FROM w", {"count(*)", "12"});
}

TEST_F(DmlFormsTest, InsertSelectIsTypedAndAtomic) {
    ExpectRefused("INSERT INTO w SELECT id + 300, tier, region, tier FROM cust",
                  "no implicit conversion");
    ExpectRefused("INSERT INTO w SELECT id + 300, name FROM cust", "name no row");
    // A duplicate key on the third row unwinds the first two.
    ExpectRefused("INSERT INTO w SELECT id + 400, name, region, tier FROM cust "
                  "UNION ALL SELECT 401, 'dup', 'EU', 1",
                  "");
    ExpectRows("SELECT COUNT(*) FROM w", {"count(*)", "6"});
}

// ---- ON CONFLICT DO NOTHING (BU-Q18) ----------------------------------------

TEST_F(DmlFormsTest, OnConflictDoNothingSkipsARowWhoseTupleKeyIsPlaced) {
    ExpectReply("INSERT INTO w VALUES (1, 'alice', 'EU', 1) ON CONFLICT DO NOTHING",
                "INSERTED oid=4005 rows=0");
    // A placed key and a key named twice in the statement skip; the rest is placed.
    ExpectReply("INSERT INTO w VALUES (1, 'x', 'EU', 1), (60, 'g', 'EU', 1), (60, 'h', 'EU', 1) "
                "ON CONFLICT (id) DO NOTHING",
                "INSERTED oid=4005 rows=1 first_id=60 last_id=60");
    ExpectRows("SELECT id, name FROM w WHERE id = 1 OR id = 60", {"id,name", "1,alice", "60,g"});
    // A delete-marked key is placed (BD-R5): its row is the tombstone.
    Ok("DELETE FROM w WHERE id = 2");
    ExpectRefused("INSERT INTO w VALUES (2, 'x', 'US', 1)", "duplicate tuple key");
    ExpectReply("INSERT INTO w VALUES (2, 'x', 'US', 1) ON CONFLICT DO NOTHING",
                "INSERTED oid=4005 rows=0");
    // A row that leaves its key out has no conflict.
    Ok("INSERT INTO w (name, region) VALUES ('n', 'EU') ON CONFLICT DO NOTHING");
    ExpectRows("SELECT COUNT(*) FROM w WHERE name = 'n'", {"count(*)", "1"});
    // INSERT ... SELECT: ids 6..11, of which 6 is placed.
    ExpectReply("INSERT INTO w SELECT id + 5, name, region, tier FROM cust ON CONFLICT DO NOTHING",
                "INSERTED oid=4005 rows=5 first_id=7 last_id=11");
}

TEST_F(DmlFormsTest, OnConflictRefusesWhatItDoesNotDo) {
    ExpectRefused("INSERT INTO w VALUES (1, 'a', 'EU', 1) ON CONFLICT (name) DO NOTHING",
                  "the arbiter is the tuple key or the one column of a unique index");
    ExpectRefused("INSERT INTO w VALUES (1, 'a', 'EU', 1) ON CONFLICT (nosuch) DO NOTHING",
                  "");
}

// ---- ON CONFLICT DO UPDATE (CG) ---------------------------------------------

// **The tuple key arbiter**: a placed key's row is updated through UPDATE's
// per-row path, an absent key is inserted, one statement does both and
// counts both (CG-Q7 (a)), `EXCLUDED.<col>` is the proposed row and a bare
// column the holder's before-image.
TEST_F(DmlFormsTest, OnConflictDoUpdateUpdatesAPlacedKeyAndInsertsAnAbsentOne) {
    ExpectReply("INSERT INTO w VALUES (1, 'al', 'EU', 7) ON CONFLICT (id) DO UPDATE "
                "SET name = EXCLUDED.name, tier = tier + EXCLUDED.tier",
                "INSERTED oid=4005 rows=1");
    ExpectRows("SELECT * FROM w WHERE id = 1", {"id,name,region,tier", "1,al,EU,8"});
    // The target-less form arbitrates on the tuple key too.
    ExpectReply("INSERT INTO w VALUES (2, 'b2', 'US', 5), (70, 'new', 'EU', 1) "
                "ON CONFLICT DO UPDATE SET name = EXCLUDED.name",
                "INSERTED oid=4005 rows=2");
    ExpectRows("SELECT id, name, tier FROM w WHERE id = 2 OR id = 70",
               {"id,name,tier", "2,b2,2", "70,new,1"});
    // The target's own name qualifies its before-image.
    Ok("INSERT INTO w VALUES (4, 'd', 'APAC', 10) ON CONFLICT (id) DO UPDATE "
       "SET tier = w.tier * 2");
    ExpectRows("SELECT tier FROM w WHERE id = 4", {"tier", "6"});
    // RETURNING answers both arms' rows, in the order they were written.
    ExpectRows("INSERT INTO w VALUES (5, 'e', 'US', 1), (71, 'f', 'EU', 2) "
               "ON CONFLICT (id) DO UPDATE SET region = 'XX' RETURNING id, region",
               {"id,region", "5,XX", "71,EU", "INSERTED oid=4005 rows=2"});
}

// **What the arm refuses, each at its byte**: a delete-marked key, committed
// (CG-Q2 (a)) or this transaction's own (CG-Q3 (a)); one key twice in a
// statement (CG-Q4 (a)); a WHERE on the arm (CG-Q5 (a)); a SET of the tuple
// key (K2); a qualifier that names neither EXCLUDED nor the target.
TEST_F(DmlFormsTest, OnConflictDoUpdateRefusesWhatItDoesNotDo) {
    Ok("DELETE FROM w WHERE id = 2");
    ExpectRefused("INSERT INTO w VALUES (2, 'x', 'US', 1) ON CONFLICT (id) DO UPDATE "
                  "SET name = EXCLUDED.name",
                  "not yet purged; PURGE frees it (byte 22)");
    ExpectRefused("INSERT INTO w VALUES (3, 'x', 'EU', 1), (3, 'y', 'EU', 1) "
                  "ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name",
                  "would update one row twice: an earlier row of this statement names the same "
                  "tuple key (byte 41) (row 2)");
    ExpectRows("SELECT name FROM w WHERE id = 3", {"name", "carol"});
    ExpectRefused("INSERT INTO w VALUES (3, 'x', 'EU', 1) ON CONFLICT (id) DO UPDATE "
                  "SET name = 'q' WHERE tier = 1",
                  "DO UPDATE ... WHERE is not supported");
    ExpectRefused("INSERT INTO w VALUES (3, 'x', 'EU', 1) ON CONFLICT (id) DO UPDATE SET id = 9",
                  "tuple key column 'id' cannot be updated at byte 70");
    ExpectRefused("INSERT INTO w VALUES (3, 'x', 'EU', 1) ON CONFLICT (id) DO UPDATE "
                  "SET name = other.name",
                  "names neither EXCLUDED nor the target");
    ExpectRefused("INSERT INTO w VALUES (3, 'x', 'EU', 1) ON CONFLICT (id) DO UPDATE "
                  "SET name = EXCLUDED.nosuch",
                  "EXCLUDED has no column 'nosuch' (byte 77)");
    // This transaction's own delete is not revived.
    Ok("BEGIN");
    Ok("DELETE FROM w WHERE id = 4");
    ExpectRefused("INSERT INTO w VALUES (4, 'x', 'EU', 1) ON CONFLICT (id) DO UPDATE "
                  "SET name = EXCLUDED.name",
                  "was deleted by this transaction");
    Ok("ROLLBACK");
    ExpectRows("SELECT name FROM w WHERE id = 4", {"name", "dave"});
}

// **The unique-index arbiter** (CG-Q1 (b)): `ON CONFLICT (col)` on the one
// column of a unique index updates the row holding the value, or inserts
// when none does; `DO NOTHING` takes the same arbiter.
TEST_F(DmlFormsTest, OnConflictArbitratesOnTheOneColumnOfAUniqueIndex) {
    Ok("CREATE UNIQUE INDEX w_name ON w (name)");
    ExpectReply("INSERT INTO w (name, region, tier) VALUES ('alice', 'US', 9) "
                "ON CONFLICT (name) DO UPDATE SET region = EXCLUDED.region, tier = EXCLUDED.tier",
                "INSERTED oid=4005 rows=1");
    ExpectRows("SELECT id, region, tier FROM w WHERE name = 'alice'", {"id,region,tier", "1,US,9"});
    ExpectRows("SELECT COUNT(*) FROM w", {"count(*)", "6"});
    Ok("INSERT INTO w (name, region) VALUES ('zoe', 'EU') ON CONFLICT (name) DO UPDATE "
       "SET region = EXCLUDED.region");
    ExpectRows("SELECT COUNT(*) FROM w WHERE name = 'zoe'", {"count(*)", "1"});
    ExpectReply("INSERT INTO w (name, region) VALUES ('bob', 'XX') ON CONFLICT (name) DO NOTHING",
                "INSERTED oid=4005 rows=0");
    ExpectRows("SELECT region FROM w WHERE name = 'bob'", {"region", "US"});
    // `EXCLUDED` of the tuple key is the key the row named, on this arbiter
    // too.
    Ok("INSERT INTO w VALUES (60, 'carol', 'EU', 1) ON CONFLICT (name) DO UPDATE "
       "SET tier = EXCLUDED.id");
    ExpectRows("SELECT id, tier FROM w WHERE name = 'carol'", {"id,tier", "3,60"});
    // A row an earlier row of the statement moved onto a later row's value is
    // one row met twice (CG-Q4 (a)), which the rows' own values do not show.
    ExpectRefused("INSERT INTO w (name, region) VALUES ('frank', 'fred'), ('fred', 'X') "
                  "ON CONFLICT (name) DO UPDATE SET name = EXCLUDED.region",
                  "tuple key 6 was written by an earlier row of this statement");
    ExpectRows("SELECT name FROM w WHERE id = 6", {"name", "frank"});
    // A non-unique or multi-column index is no arbiter.
    Ok("CREATE INDEX w_region ON w (region)");
    ExpectRefused("INSERT INTO w (name, region) VALUES ('q', 'EU') ON CONFLICT (region) DO NOTHING",
                  "the arbiter is the tuple key or the one column of a unique index");
}

// ---- UPDATE ... FROM and DELETE ... USING (BU-Q16) ---------------------------

TEST_F(DmlFormsTest, UpdateFromWritesTheRowsThatMatchExactlyOneFromRow) {
    Ok("UPDATE w SET tier = 7 FROM ord WHERE ord.cust_id = w.id AND ord.amount >= 500");
    ExpectRows("SELECT id, tier FROM w WHERE tier = 7", {"id,tier", "4,7"});
    // An uncorrelated FROM: every row matches the one order above 400.
    Ok("UPDATE w SET tier = tier + 10 FROM ord WHERE ord.amount > 400 AND w.id < 3");
    ExpectRows("SELECT id, tier FROM w WHERE id < 3", {"id,tier", "1,11", "2,12"});
    // The relation itself, under an alias.
    Ok("UPDATE w SET tier = 9 FROM w AS x WHERE x.id = w.id AND x.region = 'APAC'");
    ExpectRows("SELECT id, tier FROM w WHERE region = 'APAC'", {"id,tier", "4,9"});
}

// BU-Q16 [quiet-wrong]: a row matching two FROM rows has no one value to
// take, so the statement is refused and writes nothing.
TEST_F(DmlFormsTest, UpdateFromRefusesARowMatchingTwoFromRows) {
    ExpectRefused("UPDATE w SET tier = 8 FROM ord WHERE ord.cust_id = w.id", "more than one");
    ExpectRefused("UPDATE w SET tier = 8 FROM ord WHERE ord.amount > 200 AND w.id = 6",
                  "more than one");
    ExpectRows("SELECT COUNT(*) FROM w WHERE tier = 8", {"count(*)", "0"});
}

TEST_F(DmlFormsTest, DeleteUsingDeletesEachMatchedRowOnce) {
    Ok("DELETE FROM w USING ord WHERE ord.cust_id = w.id AND ord.status = 'cancel'");
    ExpectRows("SELECT id FROM w", {"id", "1", "3", "4", "5", "6"});
    // Customer 1 has two orders and is deleted once.
    Ok("DELETE FROM w USING ord WHERE ord.cust_id = w.id");
    ExpectRows("SELECT id FROM w", {"id", "3", "5"});
}

TEST_F(DmlFormsTest, FromAndUsingAreOneNameSpaceWithTheStatement) {
    // `id` is both relations': ambiguous, not the FROM relation's.
    ExpectRefused("UPDATE w SET tier = 1 FROM cust WHERE id = 3", "ambiguous");
    ExpectRefused("DELETE FROM w USING cust WHERE region = 'EU'", "ambiguous");
    ExpectRefused("UPDATE w SET tier = 1 FROM w WHERE w.id = 3", "bound twice");
    ExpectRefused("UPDATE w SET tier = 1 FROM ord, cust WHERE ord.cust_id = w.id",
                  "more than one relation");
    ExpectRows("SELECT COUNT(*) FROM w WHERE tier = 1", {"count(*)", "1"});
}

// ---- RETURNING (BU-Q17) -------------------------------------------------------

// The text surface answers the rows, then the statement's own tag.
TEST_F(DmlFormsTest, ReturningAnswersTheRowsThenTheTag) {
    ExpectReply("UPDATE w SET tier = 5 WHERE id = 1 RETURNING id, tier",
                "id,tier\\n1,5\\nUPDATED 1");
    ExpectReply("UPDATE w SET tier = tier + 1 WHERE id = 6 RETURNING id, tier * 10 AS t10",
                "id,t10\\n6,30\\nUPDATED 1");
    ExpectReply("UPDATE w SET tier = 1 WHERE id = 999 RETURNING id", "id\\nUPDATED 0");
    // A DELETE answers its before-images.
    ExpectReply("DELETE FROM w WHERE region = 'US' RETURNING id, name",
                "id,name\\n2,bob\\n5,erin\\nDELETED 2");
    // An INSERT's, with the issued key.
    const std::string issued = Run("INSERT INTO w (name, region) VALUES ('q', 'US') RETURNING id, name");
    EXPECT_EQ(issued.rfind("id,name\\n7,q\\nINSERTED ", 0), 0u) << issued;
    const std::string star = Run("INSERT INTO w VALUES (70, 'z', 'EU', NULL) RETURNING *");
    EXPECT_EQ(star.rfind("id,name,region,tier\\n70,z,EU,NULL\\nINSERTED ", 0), 0u) << star;
}

TEST_F(DmlFormsTest, ReturningRidesWithTheOtherForms) {
    ExpectReply("INSERT INTO w VALUES (1, 'x', 'EU', 1), (71, 'y', 'EU', 1) "
                "ON CONFLICT DO NOTHING RETURNING id",
                "id\\n71\\nINSERTED oid=4005 rows=1 first_id=71 last_id=71");
    ExpectReply("UPDATE w SET tier = 7 FROM ord WHERE ord.cust_id = w.id AND ord.amount >= 500 "
                "RETURNING w.id, tier",
                "w.id,tier\\n4,7\\nUPDATED 1");
    ExpectReply("INSERT INTO w SELECT id + 100, name, region, tier FROM cust WHERE id = 3 "
                "RETURNING id, tier",
                "id,tier\\n103,NULL\\nINSERTED oid=4005 id=103 page=135 slot=7");
}

// RETURNING is read before the statement ends, so its failure fails it.
TEST_F(DmlFormsTest, AFailingReturningUndoesTheStatement) {
    ExpectRefused("UPDATE w SET tier = 0 WHERE id = 4 RETURNING 10 / tier", "division by zero");
    ExpectRows("SELECT tier FROM w WHERE id = 4", {"tier", "3"});
    ExpectRefused("UPDATE w SET tier = 1 WHERE id = 4 RETURNING nosuch", "nosuch");
    ExpectRefused("UPDATE w SET tier = 1 WHERE id = 4 RETURNING COUNT(*)", "aggregate");
    ExpectRefused("UPDATE w SET tier = 1 FROM ord WHERE ord.cust_id = w.id AND ord.amount >= 500 "
                  "RETURNING ord.amount",
                  "the written relation's columns only");
    ExpectRows("SELECT tier FROM w WHERE id = 4", {"tier", "3"});
}


// ---- Review fixes -------------------------------------------------------------

// `NOW()` and `CURRENT_DATE` read no column: one value per statement, folded
// with the rest of the row (the column test, not the constant test).
TEST_F(DmlFormsTest, AValuesExpressionMayCallNowAndCurrentDate) {
    Ok("CREATE TABLE ev (id int64, d date, ts timestamp)");
    Ok("INSERT INTO ev VALUES (1, CURRENT_DATE, NOW())");
    ExpectRows("SELECT COUNT(*) FROM ev WHERE d IS NOT NULL", {"count(*)", "1"});
}

// An integer literal settles as `int64` with no stored width; an `int64`
// column carries one. Only a decimal's `len` is part of its type.
TEST_F(DmlFormsTest, InsertSelectTakesALiteralForAColumnOfItsType) {
    ExpectReply("INSERT INTO w SELECT 90, name, region, tier FROM cust WHERE id = 1",
                "INSERTED oid=4005 id=90 page=135 slot=6");
}

// A decimal is matched to its column by scale, so its precision is checked
// per row: the encoder would store any unscaled value of the right scale.
TEST_F(DmlFormsTest, InsertSelectRefusesADecimalWiderThanItsColumn) {
    Ok("CREATE TABLE px (id int64, m decimal(10,2))");
    Ok("INSERT INTO px VALUES (1, 12.34)");
    Ok("CREATE TABLE py (id int64, m decimal(10,2))");
    Ok("INSERT INTO py SELECT id, m * 2 FROM px");
    ExpectRefused("INSERT INTO py SELECT 2, CAST(123456789.55 AS decimal(12,2)) FROM px",
                  "does not fit decimal(10,2)");
    ExpectRows("SELECT * FROM py", {"id,m", "1,24.68"});
}

// Without a lock table every borrow is granted, so the pre-check's granted
// branch must not read another transaction's undecided insert as placed: the
// row is not skipped (its rollback would free the key), and once that
// transaction rolls back the key is placed.
TEST_F(DmlFormsTest, OnConflictDoesNotSkipAKeyAnUndecidedInsertHolds) {
    Session a;
    ASSERT_NE(dispatcher_->Dispatch("BEGIN", &a).response.rfind("ERR", 0), 0u);
    ASSERT_NE(dispatcher_->Dispatch("INSERT INTO w VALUES (90, 'a', 'EU', 1)", &a)
                  .response.rfind("ERR", 0),
              0u);
    EXPECT_NE(Run("INSERT INTO w VALUES (90, 'b', 'EU', 1) ON CONFLICT DO NOTHING"),
              "INSERTED oid=4005 rows=0");
    ASSERT_NE(dispatcher_->Dispatch("ROLLBACK", &a).response.rfind("ERR", 0), 0u);
    ExpectReply("INSERT INTO w VALUES (90, 'b', 'EU', 1) ON CONFLICT DO NOTHING",
                "INSERTED oid=4005 rows=1 first_id=90 last_id=90");
}

}  // namespace
}  // namespace kds::server
