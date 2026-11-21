#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/catalog/well_known.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "sim/instance.hpp"
#include "view_text.hpp"

// DN's heap (`instructions/v3.2.0/workorder-dn-heap-rowid.md`): data pages
// addressed by rowid, every key access through an index naming the rowid,
// a full scan that reads the data pages. These cells pin the surface DN-S2
// built - the primary key forms (DN-D3, DN-D5, DN-Q13 (c)), `keyidentity`
// (DN-D6, DN-R5), the Keystone's binding (invariant 11, DN-Q14 (a)) - and
// the write paths over a heap: insert, update in place, delete, rollback,
// an index probe resolved at its rowid, and a crash across page growth.

namespace kds::server {
namespace {

// A lock table, which the slot release's fence needs (DD-R8).
sim::SimInstance::Options WithLocks() {
    sim::SimInstance::Options options;
    options.lock_table = true;
    return options;
}

class HeapRowidTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto created = sim::SimInstance::Create(WithLocks());
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
    // Refused, the reply naming `why` (the text port carries no code).
    void Refused(const std::string& sql, const std::string& why) {
        const std::string reply = Run(sql);
        EXPECT_EQ(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
        EXPECT_NE(reply.find(why), std::string::npos) << sql << " -> " << reply;
    }
    // A column's values, sorted: a heap's walk promises no order.
    std::vector<std::string> Sorted(const std::string& sql) {
        std::vector<std::string> out;
        for (const auto& row : Rows(sql).rows) out.push_back(row.at(0));
        std::sort(out.begin(), out.end());
        return out;
    }

    std::unique_ptr<sim::SimInstance> db_;
};

TEST_F(HeapRowidTest, AHeapIsStoredAsAHeapAndHasAKeystoneIndex) {
    Ok("CREATE TABLE h (a int64, b int64) HEAP", "CREATED");
    const auto tables = Rows("SELECT name, clustered_type FROM sys.tables");
    EXPECT_EQ(tables.Find("name", "h", "clustered_type"), "HEAP");
    const auto indexes = Rows("SELECT name FROM sys.indexes");
    bool keystone = false;
    for (const auto& row : indexes.rows) keystone |= row.at(0).rfind("$keystone_", 0) == 0;
    EXPECT_TRUE(keystone) << "no Keystone index was built with the heap";
}

// DN-D5, DN-Q13 (c): a single int64 primary key declared first is the
// Keystone - visible, named by INSERT, unique, never updated.
TEST_F(HeapRowidTest, ASingleInt64PrimaryKeyDeclaredFirstIsTheKeystone) {
    Ok("CREATE TABLE p (id int64 PRIMARY KEY, v int64) HEAP", "CREATED");
    Ok("INSERT INTO p VALUES (7, 70)", "INSERTED");
    Ok("INSERT INTO p VALUES (3, 30)", "INSERTED");
    EXPECT_EQ(Rows("DESCRIBE p").Column("name"), (std::vector<std::string>{"id", "v"}));
    EXPECT_EQ(Rows("SELECT v FROM p WHERE id = 7").rows,
              (std::vector<std::vector<std::string>>{{"70"}}));
    EXPECT_EQ(Rows("SELECT keyidentity FROM p WHERE v = 30").rows,
              (std::vector<std::vector<std::string>>{{"3"}}));
    Refused("INSERT INTO p VALUES (7, 71)", "duplicate tuple key");
    Refused("UPDATE p SET id = 8 WHERE id = 7", "");
}

TEST_F(HeapRowidTest, AnInt64PrimaryKeyNotDeclaredFirstIsRefused) {
    Refused("CREATE TABLE p (v int64, id int64 PRIMARY KEY) HEAP", "declared first");
    Refused("CREATE TABLE p (v int64, id int64, PRIMARY KEY (id)) HEAP", "declared first");
}

TEST_F(HeapRowidTest, PrimaryKeyIsAHeapsAlone) {
    Refused("CREATE TABLE b (id int64 PRIMARY KEY, v int64) BTREE", "PRIMARY KEY is a heap's");
    Refused("CREATE TABLE b (id int64, v int64, PRIMARY KEY (id))", "PRIMARY KEY is a heap's");
}

// DN-D3: any other primary key is a unique index beside a hidden Keystone.
TEST_F(HeapRowidTest, AnotherPrimaryKeyIsAUniqueIndexBesideAHiddenKeystone) {
    Ok("CREATE TABLE q (code varchar, region int64, n int64, PRIMARY KEY (code, region)) HEAP",
       "CREATED");
    EXPECT_EQ(Rows("DESCRIBE q").Column("name"),
              (std::vector<std::string>{"code", "region", "n"}));
    Ok("INSERT INTO q VALUES ('a', 1, 10)", "INSERTED");
    Ok("INSERT INTO q VALUES ('a', 2, 20)", "INSERTED");
    Refused("INSERT INTO q VALUES ('a', 1, 30)", "already held");
    EXPECT_EQ(Rows("SELECT n FROM q WHERE code = 'a' AND region = 2").rows,
              (std::vector<std::vector<std::string>>{{"20"}}));
}

TEST_F(HeapRowidTest, OnePrimaryKeyARelationAndNeverNull) {
    Refused("CREATE TABLE p (id int64 PRIMARY KEY, v int64 PRIMARY KEY) HEAP", "one primary key");
    Refused("CREATE TABLE p (id int64 PRIMARY KEY, v int64, PRIMARY KEY (v)) HEAP",
            "one primary key");
    Refused("CREATE TABLE p (v int64 NULL, PRIMARY KEY (v)) HEAP", "NULL");
    Refused("CREATE TABLE p (c varchar NULL, v int64, PRIMARY KEY (c, v)) HEAP", "never NULL");
    Refused("CREATE TABLE p (v int64, PRIMARY KEY (w)) HEAP", "does not declare");
}

// DN-D6, DN-R5: `keyidentity` reads the Keystone on every relation and names
// nothing that is declared.
TEST_F(HeapRowidTest, KeyidentityReadsTheKeystoneAndIsReserved) {
    Ok("CREATE TABLE h (a int64, b int64) HEAP", "CREATED");
    Ok("INSERT INTO h VALUES (1, 2)", "INSERTED");
    const auto star = Rows("SELECT * FROM h");
    EXPECT_EQ(star.columns, (std::vector<std::string>{"a", "b"})) << "* includes no Keystone";
    const auto key = Rows("SELECT keyidentity, a FROM h");
    ASSERT_EQ(key.rows.size(), 1u);
    EXPECT_NE(key.rows[0][0], "") << "the hidden Keystone reads by name";
    Ok("CREATE TABLE b (id int64, v int64)", "CREATED");
    Ok("INSERT INTO b VALUES (42, 1)", "INSERTED");
    EXPECT_EQ(Rows("SELECT keyidentity FROM b").rows,
              (std::vector<std::vector<std::string>>{{"42"}}));
    Refused("CREATE TABLE keyidentity (a int64)", "is reserved");
    Refused("CREATE TABLE c (keyidentity int64)", "is reserved");
    Refused("CREATE INDEX KeyIdentity ON h (b)", "is reserved");
    Refused("SELECT * FROM h AS keyidentity", "is reserved");
    Refused("ALTER TABLE h RENAME COLUMN a TO keyidentity", "is reserved");
}

// DN-Q14 (a): a hidden-Keystone heap names a key only above its mark, so a
// deleted key is never named again; a declared-PK heap keeps a deleted
// key bound (its Keystone entry stays until PURGE).
TEST_F(HeapRowidTest, ADeletedKeyIsNeverBoundAgain) {
    Ok("CREATE TABLE h (a int64) HEAP", "CREATED");
    Ok("INSERT INTO h VALUES (1)", "INSERTED");
    Ok("INSERT INTO h VALUES (2)", "INSERTED");
    const std::string first = Rows("SELECT keyidentity FROM h WHERE a = 1").rows.at(0).at(0);
    Ok("DELETE FROM h WHERE a = 1", "DELETED");
    Refused("INSERT INTO h (keyidentity, a) VALUES (" + first + ", 9)", "issue mark");
    Ok("INSERT INTO h (keyidentity, a) VALUES (1000, 9)", "INSERTED");

    Ok("CREATE TABLE p (id int64 PRIMARY KEY, v int64) HEAP", "CREATED");
    Ok("INSERT INTO p VALUES (5, 1)", "INSERTED");
    Ok("DELETE FROM p WHERE id = 5", "DELETED");
    Refused("INSERT INTO p VALUES (5, 2)", "duplicate tuple key");
    EXPECT_TRUE(Rows("SELECT v FROM p").rows.empty());
}

TEST_F(HeapRowidTest, UpdateDeleteAndRollbackOverAHeap) {
    Ok("CREATE TABLE h (a int64, b int64) HEAP", "CREATED");
    for (int i = 0; i < 2000; ++i) {
        Ok("INSERT INTO h VALUES (" + std::to_string(i) + ", " + std::to_string(i % 7) + ")",
           "INSERTED");
    }
    Ok("CREATE INDEX hb ON h (b)", "CREATED");
    Ok("UPDATE h SET b = 100 WHERE a = 1500", "UPDATED");
    EXPECT_EQ(Sorted("SELECT a FROM h WHERE b = 100"), (std::vector<std::string>{"1500"}));
    Ok("DELETE FROM h WHERE b = 3", "DELETED");
    EXPECT_TRUE(Rows("SELECT a FROM h WHERE b = 3").rows.empty());
    EXPECT_EQ(Rows("SELECT a FROM h WHERE a = 11").rows,
              (std::vector<std::vector<std::string>>{{"11"}}));
    Ok("BEGIN", "BEGIN");
    Ok("INSERT INTO h VALUES (5000, 100)", "INSERTED");
    Ok("UPDATE h SET b = 100 WHERE a = 1", "UPDATED");
    Ok("DELETE FROM h WHERE a = 2", "DELETED");
    Ok("ROLLBACK", "ROLLBACK");
    EXPECT_EQ(Sorted("SELECT a FROM h WHERE b = 100"), (std::vector<std::string>{"1500"}));
    EXPECT_EQ(Rows("SELECT b FROM h WHERE a = 2").rows,
              (std::vector<std::vector<std::string>>{{"2"}}));
    // 2000 rows, 285 or 286 of them with b = 3 deleted.
    const auto count = Rows("SELECT COUNT(*) FROM h");
    EXPECT_EQ(count.rows.at(0).at(0), std::to_string(2000 - (2000 / 7 + (2000 % 7 > 3 ? 1 : 0))));
}

// The index probe resolves at the entry's rowid and agrees with the walk.
TEST_F(HeapRowidTest, AnIndexProbeAndTheWalkAnswerTheSameRows) {
    Ok("CREATE TABLE h (a int64, b int64) HEAP", "CREATED");
    Ok("CREATE INDEX hb ON h (b)", "CREATED");
    for (int i = 0; i < 1000; ++i) {
        Ok("INSERT INTO h VALUES (" + std::to_string(i) + ", " + std::to_string(i % 10) + ")",
           "INSERTED");
    }
    EXPECT_EQ(Sorted("SELECT a FROM h WHERE b = 4"),
              Sorted("SELECT a FROM h WHERE b + 0 = 4"));
    EXPECT_EQ(Sorted("SELECT a FROM h WHERE b >= 2 AND b <= 3").size(), 200u);
}

TEST(HeapRowidCrashTest, AGrownHeapAndItsIndexesSurviveACrash) {
    auto created = sim::SimInstance::Create();
    ASSERT_TRUE(created.ok()) << created.status().message();
    std::unique_ptr<sim::SimInstance> db = std::move(created.value());
    ASSERT_EQ(db->Execute("CREATE TABLE h (a int64, b int64) HEAP").substr(0, 7), "CREATED");
    ASSERT_EQ(db->Execute("CREATE INDEX hb ON h (b)").substr(0, 7), "CREATED");
    for (int i = 0; i < 1500; ++i) {
        ASSERT_EQ(db->Execute("INSERT INTO h VALUES (" + std::to_string(i) + ", " +
                              std::to_string(i % 5) + ")")
                      .substr(0, 8),
                  "INSERTED");
    }
    ASSERT_EQ(db->Execute("DELETE FROM h WHERE a < 10").substr(0, 7), "DELETED");
    const std::string all = db->Execute("SELECT a, b FROM h ORDER BY a");
    const std::string probed = db->Execute("SELECT a FROM h WHERE b = 2 ORDER BY a");
    db->Crash();
    ASSERT_TRUE(db->Reboot().ok());
    EXPECT_EQ(db->Execute("SELECT a, b FROM h ORDER BY a"), all);
    EXPECT_EQ(db->Execute("SELECT a FROM h WHERE b = 2 ORDER BY a"), probed);
    ASSERT_EQ(db->Execute("INSERT INTO h VALUES (9999, 2)").substr(0, 8), "INSERTED");
    EXPECT_NE(db->Execute("SELECT a FROM h WHERE b = 2 ORDER BY a").find("9999"),
              std::string::npos);
}

// ---- The slot release (DN-R2; DD-S4's driver, DN-S2) ----------------------

// Every step the driver owes, run to the end as core 0's tick would.
exec::HeapReleaseCounts ReleaseAll(sim::SimInstance& db) {
    for (int i = 0; i < 100000; ++i) {
        auto owed = db.dispatcher().StepHeapRelease(16);
        EXPECT_TRUE(owed.ok()) << owed.status().message();
        if (!owed.ok() || !owed.value()) break;
    }
    return db.dispatcher().last_heap_release();
}

// A heap's data page count, by the btree's leaf count, which walks a heap's
// chain as a leaf chain.
std::size_t DataPages(sim::SimInstance& db, const std::string& table) {
    auto oid = db.catalog().ResolveRelation("", table, catalog::kNamespacePublic);
    EXPECT_TRUE(oid.ok());
    auto access = db.catalog().InitTableAccess(oid.value());
    EXPECT_TRUE(access.ok());
    auto leaves = btree::BtreeLeafCount(db.store(), access.value()->desc_page_id);
    EXPECT_TRUE(leaves.ok());
    return leaves.value();
}

TEST_F(HeapRowidTest, ADeletedRowsSlotIsFreedAndTakenAgain) {
    Ok("CREATE TABLE h (a int64, b int64) HEAP", "CREATED");
    Ok("CREATE INDEX hb ON h (b)", "CREATED");
    for (int i = 0; i < 1500; ++i) {
        Ok("INSERT INTO h VALUES (" + std::to_string(i) + ", " + std::to_string(i % 9) + ")",
           "INSERTED");
    }
    const std::size_t pages = DataPages(*db_, "h");
    Ok("DELETE FROM h WHERE a >= 500 AND a < 1000", "DELETED");
    const exec::HeapReleaseCounts counts = ReleaseAll(*db_);
    EXPECT_EQ(counts.retired, 500u);
    EXPECT_GE(counts.freed, 490u) << "at most one slot a leaf of each index stays named";
    EXPECT_EQ(counts.retired, counts.freed + counts.named);
    for (int i = 0; i < 400; ++i) {
        Ok("INSERT INTO h VALUES (" + std::to_string(10000 + i) + ", 7)", "INSERTED");
    }
    EXPECT_EQ(DataPages(*db_, "h"), pages) << "the inserts took the freed slots";
    EXPECT_EQ(Rows("SELECT COUNT(*) FROM h").rows.at(0).at(0), "1400");
    EXPECT_TRUE(Rows("SELECT a FROM h WHERE a >= 500 AND a < 1000").rows.empty());
    EXPECT_EQ(Sorted("SELECT a FROM h WHERE b = 7"),
              Sorted("SELECT a FROM h WHERE b + 0 = 7"));
}

// A declared Keystone keeps its tombstone - the key stays bound - so its
// slot waits for a PURGE; a rolled-back insert's slot is freed.
TEST_F(HeapRowidTest, ADeclaredKeystonesTombstoneWaitsAndARolledBackSlotIsFreed) {
    Ok("CREATE TABLE p (id int64 PRIMARY KEY, v int64) HEAP", "CREATED");
    for (int i = 1; i <= 50; ++i) {
        Ok("INSERT INTO p VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")",
           "INSERTED");
    }
    Ok("DELETE FROM p WHERE id <= 10", "DELETED");
    Ok("BEGIN", "BEGIN");
    for (int i = 100; i < 120; ++i) {
        Ok("INSERT INTO p VALUES (" + std::to_string(i) + ", 0)", "INSERTED");
    }
    Ok("ROLLBACK", "ROLLBACK");
    const exec::HeapReleaseCounts counts = ReleaseAll(*db_);
    EXPECT_EQ(counts.retired, 0u) << "a declared Keystone's tombstone is kept";
    EXPECT_EQ(counts.freed + counts.named, 20u) << "the rolled-back inserts' slots";
    EXPECT_GE(counts.freed, 18u);
    Refused("INSERT INTO p VALUES (3, 3)", "duplicate tuple key");
    Ok("INSERT INTO p VALUES (110, 1)", "INSERTED");
    EXPECT_EQ(Rows("SELECT v FROM p WHERE id = 110").rows,
              (std::vector<std::vector<std::string>>{{"1"}}));
}

// **A tombstone the release could not yet retire is asked again.** The
// delete queues its relation while its deleter is still in flight, and an
// older reader holds the horizon past it; a job that ran then found every
// tombstone owed and ended, and nothing queued the relation again, so the
// rows' slots stayed taken for good - here an explicit transaction's
// delete and a reader's view. A job that defers one queues its relation
// again.
TEST_F(HeapRowidTest, ATombstoneTheReleaseHadToDeferIsReleasedLater) {
    Ok("CREATE TABLE h (a int64, b int64) HEAP", "CREATED");
    for (int i = 0; i < 300; ++i) {
        Ok("INSERT INTO h VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")",
           "INSERTED");
    }
    // Delete-marked slots no job has retired yet.
    const auto tombstones = [&]() {
        auto oid = db_->catalog().ResolveRelation("", "h", catalog::kNamespacePublic);
        EXPECT_TRUE(oid.ok());
        auto access = db_->catalog().InitTableAccess(oid.value());
        EXPECT_TRUE(access.ok());
        int marked = 0;
        for (PageId at = access.value()->desc_page_id; at != kInvalidPageId;) {
            auto ref = db_->store().GetForRead(at);
            EXPECT_TRUE(ref.ok());
            if (!ref.ok()) return -1;
            heap::PageView page(ref.value().bytes());
            for (std::uint16_t i = 0; i < page.slot_count(); ++i) {
                auto tuple = page.ReadTuple(i);
                if (tuple.ok() && tuple.value().deleted) ++marked;
            }
            at = page.next_page_id();
        }
        return marked;
    };
    const auto steps = [&](int n) {
        for (int i = 0; i < n; ++i) {
            auto owed = db_->dispatcher().StepHeapRelease(16);
            ASSERT_TRUE(owed.ok()) << owed.status().message();
        }
    };
    // In flight: the deleter is not yet resolved when the job runs.
    Ok("BEGIN", "BEGIN");
    Ok("DELETE FROM h WHERE a >= 50 AND a < 100", "DELETED");
    steps(50);
    Ok("COMMIT", "COMMIT");
    ReleaseAll(*db_);
    EXPECT_EQ(tombstones(), 0) << "a delete committed after the job ran is never retired";

    // An older reader's view: the horizon holds the deleter.
    Session reader;
    ASSERT_EQ(db_->dispatcher().Dispatch("BEGIN ISOLATION LEVEL REPEATABLE READ", &reader).response.rfind("BEGIN", 0),
              0u);
    const std::string before = db_->dispatcher().Dispatch("SELECT COUNT(*) FROM h", &reader).response;
    Ok("DELETE FROM h WHERE a >= 150 AND a < 200", "DELETED");
    steps(50);
    EXPECT_EQ(tombstones(), 50) << "retired under a reader that sees them";
    EXPECT_EQ(db_->dispatcher().Dispatch("SELECT COUNT(*) FROM h", &reader).response, before);
    ASSERT_EQ(db_->dispatcher().Dispatch("COMMIT", &reader).response.rfind("COMMIT", 0), 0u);
    ReleaseAll(*db_);
    EXPECT_EQ(tombstones(), 0) << "a tombstone a reader held is never retired once it ends";
    EXPECT_EQ(Rows("SELECT COUNT(*) FROM h").rows.at(0).at(0), "200");
}

// **A rolled-back insert alone queues its relation**: a rollback's
// compensation retiring a heap slot reports it (`TransactionManager`'s
// heap-retire observer, `HeapSpace::NoteDead`), so its slots are released
// with no later delete on the relation; a mount queues every heap.
TEST_F(HeapRowidTest, ARolledBackInsertAloneQueuesItsRelation) {
    Ok("CREATE TABLE p (id int64 PRIMARY KEY, v int64) HEAP", "CREATED");
    for (int i = 1; i <= 50; ++i) {
        Ok("INSERT INTO p VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")",
           "INSERTED");
    }
    Ok("BEGIN", "BEGIN");
    for (int i = 100; i < 120; ++i) {
        Ok("INSERT INTO p VALUES (" + std::to_string(i) + ", 0)", "INSERTED");
    }
    Ok("ROLLBACK", "ROLLBACK");
    auto owed = db_->dispatcher().StepHeapRelease(16);
    ASSERT_TRUE(owed.ok()) << owed.status().message();
    EXPECT_TRUE(owed.value()) << "the rollback queued nothing";
    const exec::HeapReleaseCounts counts = ReleaseAll(*db_);
    EXPECT_EQ(counts.freed + counts.named, 20u);
}

TEST(HeapRowidCrashTest, AReleasedAndReusedSlotReplays) {
    auto created = sim::SimInstance::Create(WithLocks());
    ASSERT_TRUE(created.ok()) << created.status().message();
    std::unique_ptr<sim::SimInstance> db = std::move(created.value());
    ASSERT_EQ(db->Execute("CREATE TABLE h (a int64, b varchar) HEAP").substr(0, 7), "CREATED");
    ASSERT_EQ(db->Execute("CREATE INDEX ha ON h (a)").substr(0, 7), "CREATED");
    for (int i = 0; i < 600; ++i) {
        ASSERT_EQ(db->Execute("INSERT INTO h VALUES (" + std::to_string(i) + ", '" +
                              std::string(40, static_cast<char>('a' + i % 26)) + "')")
                      .substr(0, 8),
                  "INSERTED");
    }
    ASSERT_EQ(db->Execute("DELETE FROM h WHERE a >= 100 AND a < 300").substr(0, 7), "DELETED");
    const exec::HeapReleaseCounts counts = ReleaseAll(*db);
    ASSERT_GT(counts.freed, 0u);
    for (int i = 0; i < 150; ++i) {
        ASSERT_EQ(db->Execute("INSERT INTO h VALUES (" + std::to_string(5000 + i) + ", 'new')")
                      .substr(0, 8),
                  "INSERTED");
    }
    const std::string all = db->Execute("SELECT a, b FROM h ORDER BY a");
    const std::string probed = db->Execute("SELECT b FROM h WHERE a = 5100");
    ASSERT_EQ(probed, "b\\nnew");
    db->Crash();
    ASSERT_TRUE(db->Reboot().ok());
    EXPECT_EQ(db->Execute("SELECT a, b FROM h ORDER BY a"), all);
    EXPECT_EQ(db->Execute("SELECT b FROM h WHERE a = 5100"), probed);
    EXPECT_TRUE(testing_view::ReadViewText(db->Execute("SELECT a FROM h WHERE a = 150"))
                    .rows.empty());
}

}  // namespace
}  // namespace kds::server
