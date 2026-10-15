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
    ExpectRefused("INSERT INTO w VALUES (1, 'a', 'EU', 1) ON CONFLICT (id) DO UPDATE SET tier = 2",
                  "NOT_IMPLEMENTED");
    ExpectRefused("INSERT INTO w VALUES (1, 'a', 'EU', 1) ON CONFLICT (name) DO NOTHING",
                  "the tuple key is the one conflict");
    ExpectRefused("INSERT INTO w VALUES (1, 'a', 'EU', 1) ON CONFLICT (nosuch) DO NOTHING",
                  "");
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

}  // namespace
}  // namespace kds::server
