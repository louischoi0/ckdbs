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

    Ok("CREATE TABLE hid (v int64) HEAP", "CREATED");
    Refused("CREATE TABLE child (id int64, hid_id int64 REFERENCES hid)", "heap");
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
