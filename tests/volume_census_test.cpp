#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/server/volume_census.hpp"
#include "kds/storage/varheap.hpp"
#include "sim/instance.hpp"

// DV (`instructions/v3.2.0/workorder-dv-unreachable-page-reclaim.md`): the
// volume census over volumes each leak class of the order's §1 produces.
// DV-S0 measures: each cell prints what the census found, and asserts only
// that the mark was whole (DV-R1) and that a clean volume reads zero.

namespace kds::server {
namespace {

class VolumeCensusTest : public ::testing::Test {
protected:
    void Boot(bool waystone = false) {
        sim::SimInstance::Options options;
        options.waystone = waystone;
        options.lock_table = true;
        auto created = sim::SimInstance::Create(options);
        ASSERT_TRUE(created.ok()) << created.status().message();
        db_ = std::move(created.value());
    }

    std::string Run(const std::string& sql) { return db_->Execute(sql); }
    void Ok(const std::string& sql, const char* prefix) {
        const std::string reply = Run(sql);
        ASSERT_EQ(reply.rfind(prefix, 0), 0u) << sql << " -> " << reply;
    }

    VolumeCensus Census() {
        auto census = db_->RunCensus();
        EXPECT_TRUE(census.ok()) << census.status().message();
        if (!census.ok()) return {};
        EXPECT_TRUE(census.value().complete)
            << (census.value().refusals.empty() ? "" : census.value().refusals.front());
        return census.value();
    }

    static void Print(const char* cell, const VolumeCensus& c) {
        std::string classes;
        for (const auto& [key, count] : c.unmarked_by_class) {
            classes += " " + key.page_class + "/" + std::to_string(key.owner) + ":" +
                       std::to_string(count);
        }
        std::printf("[census] %s allocated=%llu marked=%llu unmarked=%llu unprovable=%llu%s\n",
                    cell, static_cast<unsigned long long>(c.allocated),
                    static_cast<unsigned long long>(c.marked),
                    static_cast<unsigned long long>(c.unmarked),
                    static_cast<unsigned long long>(c.unprovable), classes.c_str());
    }

    void Reboot() {
        ASSERT_TRUE(db_->CleanShutdown().ok());
        Status s = db_->Reboot();
        ASSERT_TRUE(s.ok()) << s.message();
        ASSERT_TRUE(db_->SettleReclaim().ok());
    }

    std::unique_ptr<sim::SimInstance> db_;
};

// Every root kind on one volume, nothing leaked: the census reads zero.
TEST_F(VolumeCensusTest, ACleanVolumeReadsZero) {
    Boot(/*waystone=*/true);
    Ok("CREATE TABLE t (id int64, v int64, s varchar)", "CREATED");
    Ok("CREATE TABLE h (id int64 PRIMARY KEY, v int64) HEAP", "CREATED");
    Ok("CREATE INDEX tv ON t (v)", "CREATED");
    Ok("CREATE INDEX hv ON h (v)", "CREATED");
    Ok("CREATE ASSERTION cap ON t GROUP BY (v) CHECK COUNT(*) <= 1000000", "CREATED ASSERTION");
    for (int i = 1; i <= 400; ++i) {
        Ok("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i % 7) + ", '" +
               std::string(200, 'a' + i % 26) + "')",
           "INSERTED");
        Ok("INSERT INTO h VALUES (" + std::to_string(i) + ", " + std::to_string(i % 5) + ")",
           "INSERTED");
    }
    Ok("WAYSTONE ON SELECT v FROM t WHERE id = 5", "OK WAYSTONE ON");
    for (int i = 1; i <= 20; ++i) Run("SELECT v FROM t WHERE id = " + std::to_string(i));
    const VolumeCensus c = Census();
    Print("clean", c);
    EXPECT_EQ(c.unmarked, 0u);
    for (const char* kind : {"catalog", "catalog_varheap", "relation", "assertion", "waystone"}) {
        EXPECT_TRUE(c.roots_walked.contains(kind)) << kind << " was never walked";
    }
}

// DV-R1: a root the mark cannot walk whole makes the census no proof - a
// live relation's var-heap chain cut after its root.
TEST_F(VolumeCensusTest, AChainThatDoesNotWalkIsARefusal) {
    Boot();
    Ok("CREATE TABLE t (id int64, s varchar)", "CREATED");
    for (int i = 1; i <= 200; ++i) {
        Ok("INSERT INTO t VALUES (" + std::to_string(i) + ", '" + std::string(600, 'q') + "')",
           "INSERTED");
    }
    auto oid = db_->catalog().FindTableOidByName(catalog::kNamespacePublic, "t");
    ASSERT_TRUE(oid.ok());
    auto row = db_->catalog().GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    PageId second = kInvalidPageId;
    {
        auto head = db_->store().GetForRead(row.value().varheap_page_id);
        ASSERT_TRUE(head.ok());
        second = varheap::PageNextPageId(head.value().bytes());
    }
    ASSERT_NE(second, kInvalidPageId) << "the chain never grew past its root";
    ASSERT_TRUE(db_->store().FreePage(second).ok());
    auto census = db_->RunCensus();
    ASSERT_TRUE(census.ok()) << census.status().message();
    EXPECT_FALSE(census.value().complete);
    ASSERT_FALSE(census.value().refusals.empty());
    EXPECT_NE(census.value().refusals.front().find("relation oid=" + std::to_string(oid.value())),
              std::string::npos)
        << census.value().refusals.front();
}

// §1's Waystone class: every mount renews an opted row's directory and
// strands the one it replaces.
TEST_F(VolumeCensusTest, WaystoneMountsStrandDirectories) {
    Boot(/*waystone=*/true);
    Ok("CREATE TABLE t (id int64, v int64)", "CREATED");
    for (int i = 1; i <= 50; ++i) {
        Ok("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")",
           "INSERTED");
    }
    Ok("WAYSTONE ON SELECT v FROM t WHERE id = 5", "OK WAYSTONE ON");
    Ok("WAYSTONE ON SELECT id FROM t WHERE id = 6", "OK WAYSTONE ON");
    constexpr int kMounts = 100;
    std::uint64_t at_10 = 0;
    for (int mount = 1; mount <= kMounts; ++mount) {
        for (int i = 1; i <= 8; ++i) {
            Run("SELECT v FROM t WHERE id = " + std::to_string(i));
            Run("SELECT id FROM t WHERE id = " + std::to_string(i));
        }
        Reboot();
        if (HasFatalFailure()) return;
        if (mount == 10) at_10 = Census().unmarked;
    }
    const VolumeCensus c = Census();
    Print("waystone x100 mounts", c);
    std::printf("[census] waystone: %llu unmarked after 10 mounts, %llu after %d\n",
                static_cast<unsigned long long>(at_10),
                static_cast<unsigned long long>(c.unmarked), kMounts);
    EXPECT_GT(c.unmarked, at_10);
}

// §1's Waystone class at `OFF`.
TEST_F(VolumeCensusTest, WaystoneOffStrandsItsDirectory) {
    Boot(/*waystone=*/true);
    Ok("CREATE TABLE t (id int64, v int64)", "CREATED");
    for (int i = 1; i <= 20; ++i) {
        Ok("INSERT INTO t VALUES (" + std::to_string(i) + ", 1)", "INSERTED");
    }
    for (int round = 0; round < 10; ++round) {
        Ok("WAYSTONE ON SELECT v FROM t WHERE id = 5", "OK WAYSTONE ON");
        for (int i = 1; i <= 8; ++i) Run("SELECT v FROM t WHERE id = " + std::to_string(i));
        Ok("WAYSTONE OFF SELECT v FROM t WHERE id = 5", "OK WAYSTONE OFF");
    }
    const VolumeCensus c = Census();
    Print("waystone on/off x10", c);
}

// §1: a rolled-back `CREATE TABLE`'s pages.
TEST_F(VolumeCensusTest, RolledBackCreateTable) {
    Boot();
    for (int i = 0; i < 10; ++i) {
        Ok("BEGIN", "BEGIN");
        Ok("CREATE TABLE r" + std::to_string(i) + " (id int64, v int64, s varchar)", "CREATED");
        Ok("INSERT INTO r" + std::to_string(i) + " VALUES (1, 1, '" + std::string(300, 'x') + "')",
           "INSERTED");
        Ok("ROLLBACK", "ROLLBACK");
    }
    const VolumeCensus c = Census();
    Print("rolled-back create table x10", c);
    Reboot();
    if (HasFatalFailure()) return;
    Print("rolled-back create table x10, after a mount", Census());
}

// §1: a failed `CREATE INDEX`'s pages - a unique build over duplicates.
TEST_F(VolumeCensusTest, FailedCreateIndex) {
    Boot();
    Ok("CREATE TABLE t (id int64, v int64)", "CREATED");
    for (int i = 1; i <= 2000; ++i) {
        Ok("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i % 1000) + ")",
           "INSERTED");
    }
    for (int i = 0; i < 10; ++i) {
        const std::string reply = Run("CREATE UNIQUE INDEX tv" + std::to_string(i) + " ON t (v)");
        EXPECT_EQ(reply.rfind("ERR", 0), 0u) << reply;
        ASSERT_TRUE(db_->SettleReclaim().ok());
    }
    const VolumeCensus c = Census();
    Print("failed unique create index x10", c);
    Reboot();
    if (HasFatalFailure()) return;
    Print("failed unique create index x10, after a mount", Census());
}

// A dropped table, reclaimed: nothing of it stays.
TEST_F(VolumeCensusTest, DroppedTableReclaimed) {
    Boot();
    for (int i = 0; i < 5; ++i) {
        const std::string t = "d" + std::to_string(i);
        Ok("CREATE TABLE " + t + " (id int64, v int64, s varchar)", "CREATED");
        Ok("CREATE INDEX " + t + "v ON " + t + " (v)", "CREATED");
        for (int k = 1; k <= 300; ++k) {
            Ok("INSERT INTO " + t + " VALUES (" + std::to_string(k) + ", " + std::to_string(k) +
                   ", '" + std::string(150, 'y') + "')",
               "INSERTED");
        }
        Ok("DROP TABLE " + t, "DROPPED");
    }
    Reboot();
    if (HasFatalFailure()) return;
    const VolumeCensus c = Census();
    Print("dropped tables x5, reclaimed", c);
    EXPECT_EQ(c.unmarked, 0u);
}

}  // namespace
}  // namespace kds::server
