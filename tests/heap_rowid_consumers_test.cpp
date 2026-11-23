#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "sim/instance.hpp"
#include "view_text.hpp"

// DN-S3 (`instructions/v3.2.0/workorder-dn-heap-rowid.md` §5): the paths
// that find a heap's row by its Keystone - a foreign key to a heap whose
// Keystone is its declared primary key (DN-Q5 (a)), `PURGE` by that Keystone
// (DN-Q6 (a)), the ordered index walk resolved at an entry's rowid, and a
// Cabin and a Waystone over a heap (DN-Q10 (a): their hint is the rowid).

namespace kds::server {
namespace {

class HeapConsumersTest : public ::testing::Test {
protected:
    void SetUp() override {
        sim::SimInstance::Options options;
        options.lock_table = true;
        auto created = sim::SimInstance::Create(options);
        ASSERT_TRUE(created.ok()) << created.status().message();
        db_ = std::move(created.value());
    }

    std::string Run(const std::string& sql) { return db_->Execute(sql); }
    testing_view::ViewText Rows(const std::string& sql) {
        return testing_view::ReadViewText(Run(sql));
    }
    void Ok(const std::string& sql, const char* prefix) {
        const std::string reply = Run(sql);
        ASSERT_EQ(reply.rfind(prefix, 0), 0u) << sql << " -> " << reply;
    }
    void Refused(const std::string& sql, const std::string& why) {
        const std::string reply = Run(sql);
        EXPECT_EQ(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
        EXPECT_NE(reply.find(why), std::string::npos) << sql << " -> " << reply;
    }

    std::unique_ptr<sim::SimInstance> db_;
};

// DN-Q5 (a): a heap whose Keystone is its declared int64 primary key is a
// parent; a hidden Keystone's is not.
TEST_F(HeapConsumersTest, ADeclaredKeystoneHeapIsAForeignKeyParent) {
    Ok("CREATE TABLE acct (id int64 PRIMARY KEY, owner int64) HEAP", "CREATED");
    Ok("CREATE TABLE xfer (id int64, acct_id int64 REFERENCES acct)", "CREATED");
    Ok("INSERT INTO acct VALUES (10, 1)", "INSERTED");
    Ok("INSERT INTO xfer VALUES (1, 10)", "INSERTED");
    Refused("INSERT INTO xfer VALUES (2, 11)", "FK_VIOLATION");
    Refused("DELETE FROM acct WHERE id = 10", "");
    Ok("DELETE FROM xfer WHERE id = 1", "DELETED");
    Ok("DELETE FROM acct WHERE id = 10", "DELETED");

    // A hidden Keystone's heap is a parent too since DR-S3c, by its
    // `keyidentity` (`primary_key_test.cpp`).
    Ok("CREATE TABLE hid (v int64) HEAP", "CREATED");
    Ok("CREATE TABLE child (id int64, hid_id int64 REFERENCES hid)", "CREATED");
}

// DN-Q6 (a): PURGE by Keystone on a declared-Keystone heap frees the key,
// which an INSERT names again; the hidden Keystone's heap refuses PURGE.
TEST_F(HeapConsumersTest, PurgeFreesADeclaredKeystoneHeapsKey) {
    Ok("CREATE TABLE p (id int64 PRIMARY KEY, v int64) HEAP", "CREATED");
    Ok("CREATE INDEX pv ON p (v)", "CREATED");
    for (int i = 1; i <= 40; ++i) {
        Ok("INSERT INTO p VALUES (" + std::to_string(i) + ", " + std::to_string(i % 4) + ")",
           "INSERTED");
    }
    Ok("DELETE FROM p WHERE id >= 10 AND id < 20", "DELETED");
    Refused("INSERT INTO p VALUES (15, 0)", "duplicate tuple key");
    Ok("PURGE FROM p WHERE id >= 12 AND id < 18", "PURGED 6");
    Ok("INSERT INTO p VALUES (15, 3)", "INSERTED");
    Refused("INSERT INTO p VALUES (11, 0)", "duplicate tuple key");
    EXPECT_EQ(Rows("SELECT v FROM p WHERE id = 15").rows,
              (std::vector<std::vector<std::string>>{{"3"}}));
    // The index probe finds the new row and none of the purged.
    std::vector<std::string> ids;
    for (const auto& row : Rows("SELECT id FROM p WHERE v = 3").rows) ids.push_back(row.at(0));
    std::sort(ids.begin(), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), "15"), ids.end());
    EXPECT_EQ(std::count(ids.begin(), ids.end(), "15"), 1);

    Ok("CREATE TABLE h (v int64) HEAP", "CREATED");
    Refused("PURGE FROM h WHERE keyidentity = 1", "heap");
}

// A heap's PURGE walk is in page order: placed in descending key order, a
// window's keys lie on both sides of keys past `hi`, and every one is judged
// once - the walk does not stop at the first key past the window.
TEST_F(HeapConsumersTest, PurgeJudgesAHeapsWindowWhateverItsPageOrder) {
    Ok("CREATE TABLE p (id int64 PRIMARY KEY, v int64) HEAP", "CREATED");
    for (int i = 600; i >= 1; --i) {
        Ok("INSERT INTO p VALUES (" + std::to_string(i) + ", " + std::to_string(i % 7) + ")",
           "INSERTED");
    }
    Ok("DELETE FROM p WHERE id >= 100 AND id < 500", "DELETED 400");
    Ok("PURGE FROM p WHERE id >= 200 AND id < 400", "PURGED 200");
    Ok("PURGE FROM p WHERE id >= 200 AND id < 400", "PURGED 0");
    Ok("PURGE FROM p WHERE id = 450", "PURGED 1");
    Refused("PURGE FROM p WHERE id = 50", "is live");
    for (const int key : {199, 400, 451}) {
        Refused("INSERT INTO p VALUES (" + std::to_string(key) + ", 0)", "duplicate tuple key");
    }
    for (const int key : {200, 399, 450}) {
        Ok("INSERT INTO p VALUES (" + std::to_string(key) + ", 0)", "INSERTED");
    }
    EXPECT_EQ(Rows("SELECT COUNT(*) FROM p").rows.at(0).at(0), "203");
}

// **A key PURGE freed and an INSERT named again never meets its old
// entries** (DN-R3): before the slot release the Keystone index holds the
// purged row's entry beside the new one, and after it the purged slot is
// freed and taken by another key - in both, every probe of the key answers
// the new row alone, and the key is bound again.
TEST_F(HeapConsumersTest, APurgedKeyNamedAgainMeetsNoStaleEntryAcrossTheRelease) {
    Ok("CREATE TABLE p (id int64 PRIMARY KEY, v int64) HEAP", "CREATED");
    Ok("CREATE INDEX pv ON p (v)", "CREATED");
    for (int i = 1; i <= 300; ++i) {
        Ok("INSERT INTO p VALUES (" + std::to_string(i) + ", 1)", "INSERTED");
    }
    Ok("DELETE FROM p WHERE id >= 100 AND id < 200", "DELETED 100");
    Ok("PURGE FROM p WHERE id >= 100 AND id < 200", "PURGED 100");
    const auto check = [&](const char* when, const char* total) {
        SCOPED_TRACE(when);
        for (const int key : {100, 150, 199}) {
            EXPECT_EQ(Rows("SELECT v FROM p WHERE id = " + std::to_string(key)).rows,
                      (std::vector<std::vector<std::string>>{{"2"}}));
            Refused("INSERT INTO p VALUES (" + std::to_string(key) + ", 9)",
                    "duplicate tuple key");
        }
        EXPECT_EQ(Rows("SELECT COUNT(*) FROM p WHERE v = 1").rows.at(0).at(0), "200");
        EXPECT_EQ(Rows("SELECT COUNT(*) FROM p WHERE v = 2").rows.at(0).at(0), "100");
        EXPECT_EQ(Rows("SELECT COUNT(*) FROM p").rows.at(0).at(0), total);
    };
    for (int i = 100; i < 200; ++i) {
        Ok("INSERT INTO p VALUES (" + std::to_string(i) + ", 2)", "INSERTED");
    }
    // Keys the purged slots can take once released.
    for (int i = 1000; i < 1100; ++i) {
        Ok("INSERT INTO p VALUES (" + std::to_string(i) + ", 3)", "INSERTED");
    }
    check("before the release", "400");
    for (int i = 0; i < 100000; ++i) {
        auto owed = db_->dispatcher().StepHeapRelease(16);
        ASSERT_TRUE(owed.ok()) << owed.status().message();
        if (!owed.value()) break;
    }
    EXPECT_GE(db_->dispatcher().last_heap_release().freed, 90u)
        << "the purged slots are freed once no entry names them";
    for (int i = 2000; i < 2100; ++i) {
        Ok("INSERT INTO p VALUES (" + std::to_string(i) + ", 4)", "INSERTED");
    }
    check("after the release and the slots' reuse", "500");
    EXPECT_EQ(Rows("SELECT COUNT(*) FROM p WHERE v = 4").rows.at(0).at(0), "100");
}

// A heap parent's key PURGE freed is a parent again once named again, and
// D9(a)'s `S` on it fences a parent DELETE as on a btree; the reverse check
// over a heap child answers from the walk and from a Cabin.
TEST_F(HeapConsumersTest, AHeapParentHoldsUnderTheForeignKeysRules) {
    Ok("CREATE TABLE acct (id int64 PRIMARY KEY, owner int64) HEAP", "CREATED");
    Ok("CREATE TABLE xfer (id int64 PRIMARY KEY, acct_id int64 REFERENCES acct) HEAP",
       "CREATED");
    for (int i = 1; i <= 20; ++i) {
        Ok("INSERT INTO acct VALUES (" + std::to_string(i) + ", 0)", "INSERTED");
    }
    Ok("INSERT INTO xfer VALUES (1, 5)", "INSERTED");
    Refused("DELETE FROM acct WHERE id = 5", "FK_VIOLATION");

    // Deleted, purged and named again: a parent again.
    Ok("DELETE FROM acct WHERE id = 7", "DELETED 1");
    Refused("INSERT INTO xfer VALUES (2, 7)", "FK_VIOLATION");
    Ok("PURGE FROM acct WHERE id = 7", "PURGED 1");
    Refused("INSERT INTO xfer VALUES (2, 7)", "FK_VIOLATION");
    Ok("INSERT INTO acct VALUES (7, 1)", "INSERTED");
    Ok("INSERT INTO xfer VALUES (2, 7)", "INSERTED");
    Refused("DELETE FROM acct WHERE id = 7", "FK_VIOLATION");

    // D9(a): an open child insert's `S` on its parent row refuses the
    // parent's DELETE, and its commit leaves the reference standing.
    Session child;
    ASSERT_EQ(db_->dispatcher().Dispatch("BEGIN", &child).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(db_->dispatcher()
                  .Dispatch("INSERT INTO xfer VALUES (3, 9)", &child)
                  .response.rfind("INSERTED", 0),
              0u);
    EXPECT_EQ(Run("DELETE FROM acct WHERE id = 9").rfind("ERR", 0), 0u);
    ASSERT_EQ(db_->dispatcher().Dispatch("COMMIT", &child).response.rfind("COMMIT", 0), 0u);
    Refused("DELETE FROM acct WHERE id = 9", "FK_VIOLATION");

    // Moved off and deleted: the parents go.
    Ok("UPDATE xfer SET acct_id = 1 WHERE id = 3", "UPDATED 1");
    Ok("DELETE FROM xfer WHERE id = 2", "DELETED 1");
    Ok("DELETE FROM acct WHERE id = 9", "DELETED 1");
    Ok("DELETE FROM acct WHERE id = 7", "DELETED 1");
}

TEST(HeapConsumersCabinTest, ACabinOnAHeapChildAnswersTheReverseCheck) {
    sim::SimInstance::Options options;
    options.lock_table = true;
    options.cabins = true;
    auto created = sim::SimInstance::Create(options);
    ASSERT_TRUE(created.ok()) << created.status().message();
    sim::SimInstance& db = *created.value();
    const auto ok = [&](const std::string& sql, const char* prefix) {
        const std::string reply = db.Execute(sql);
        EXPECT_EQ(reply.rfind(prefix, 0), 0u) << sql << " -> " << reply;
    };
    ok("CREATE TABLE acct (id int64 PRIMARY KEY, owner int64) HEAP", "CREATED");
    ok("CREATE TABLE xfer (id int64 PRIMARY KEY, acct_id int64 REFERENCES acct) HEAP",
       "CREATED");
    ok("CREATE CABIN ON xfer(acct_id)", "CREATED");
    for (int i = 1; i <= 4; ++i) {
        ok("INSERT INTO acct VALUES (" + std::to_string(i) + ", 0)", "INSERTED");
    }
    ok("INSERT INTO xfer VALUES (1, 1)", "INSERTED");
    // Observed, so the sets answer: 2 has none, 1 has one.
    ok("SELECT * FROM xfer WHERE acct_id = 1", "");
    ok("SELECT * FROM xfer WHERE acct_id = 2", "");
    ok("DELETE FROM acct WHERE id = 2", "DELETED 1");
    EXPECT_EQ(db.Execute("DELETE FROM acct WHERE id = 1").rfind("ERR FK_VIOLATION", 0), 0u);
    // The child moved off 1 keeps its key in 1's set, a surplus the
    // re-check subtracts; deleted, purged and its key named again under
    // another parent, the set's stale key is a row that names 3.
    ok("UPDATE xfer SET acct_id = 3 WHERE id = 1", "UPDATED 1");
    ok("DELETE FROM xfer WHERE id = 1", "DELETED 1");
    ok("PURGE FROM xfer WHERE id = 1", "PURGED 1");
    ok("INSERT INTO xfer VALUES (1, 4)", "INSERTED");
    ok("DELETE FROM acct WHERE id = 1", "DELETED 1");
    ok("DELETE FROM acct WHERE id = 3", "DELETED 1");
    EXPECT_EQ(db.Execute("DELETE FROM acct WHERE id = 4").rfind("ERR FK_VIOLATION", 0), 0u);
}

// The ordered index walk (CV) over a heap resolves at each entry's rowid
// and answers what the sort does.
TEST_F(HeapConsumersTest, AnOrderedIndexWalkOverAHeapMatchesTheSort) {
    Ok("CREATE TABLE h (a int64, b int64) HEAP", "CREATED");
    Ok("CREATE INDEX hb ON h (b)", "CREATED");
    for (int i = 0; i < 500; ++i) {
        Ok("INSERT INTO h VALUES (" + std::to_string(i) + ", " + std::to_string((i * 37) % 101) +
               ")",
           "INSERTED");
    }
    const auto walked = Rows("SELECT b FROM h ORDER BY b LIMIT 25");
    const auto sorted = Rows("SELECT b FROM h ORDER BY b + 0 LIMIT 25");
    EXPECT_EQ(walked.rows, sorted.rows);
    const auto walked_desc = Rows("SELECT b FROM h ORDER BY b DESC LIMIT 25");
    const auto sorted_desc = Rows("SELECT b FROM h ORDER BY b + 0 DESC LIMIT 25");
    EXPECT_EQ(walked_desc.rows, sorted_desc.rows);
}

}  // namespace
}  // namespace kds::server
