#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/server/restore.hpp"
#include "kds/server/volume_census.hpp"
#include "kds/stats/waystone_dir.hpp"
#include "kds/storage/varheap.hpp"
#include "pattern_rows.hpp"
#include "sim/instance.hpp"

#include <fstream>
#include <sstream>

#include "file_rig_crash.hpp"
#include "kds/base/current_core.hpp"
#include "kds/server/expeditor.hpp"
#include "reclaim_census.hpp"
#include "view_text.hpp"

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

// §1's Waystone class, closed at its source (DV-S2, DV-Q4 (a)): every mount
// renews an opted row's directory, and the one it replaces is taken back -
// its trails freed, its root reused - so a hundred mounts strand nothing.
TEST_F(VolumeCensusTest, WaystoneMountsStrandNothing) {
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
        // Twice each: an instance records at its second sighting.
        for (int pass = 0; pass < 2; ++pass) {
            for (int i = 1; i <= 8; ++i) {
                Run("SELECT v FROM t WHERE id = " + std::to_string(i));
                Run("SELECT id FROM t WHERE id = " + std::to_string(i));
            }
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
    EXPECT_EQ(at_10, 0u);
    EXPECT_EQ(c.unmarked, 0u);
}

// DV-R1 `[quiet-wrong]` for the renewal's harvest: a stale directory naming a
// live relation's logged pages - what a crash makes of a child whose bit
// never reached the device and which the cursor handed on - frees none of
// them. Only its own trails, which still read `kWaystone`, go.
TEST_F(VolumeCensusTest, AStaleDirectoryNamingLivePagesLeavesThem) {
    Boot(/*waystone=*/true);
    Ok("CREATE TABLE t (id int64, v int64, s varchar)", "CREATED");
    for (int i = 1; i <= 50; ++i) {
        Ok("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ", '" +
               std::string(300, 'k') + "')",
           "INSERTED");
    }
    Ok("WAYSTONE ON SELECT v FROM t WHERE id = 5", "OK WAYSTONE ON");
    // Twice each: the first sighting counts, the second records (n = 2).
    for (int round = 0; round < 2; ++round) {
        for (int i = 1; i <= 8; ++i) Run("SELECT v FROM t WHERE id = " + std::to_string(i));
    }
    auto oid = db_->catalog().FindTableOidByName(catalog::kNamespacePublic, "t");
    ASSERT_TRUE(oid.ok());
    auto table = db_->catalog().GetSysTableRow(oid.value());
    ASSERT_TRUE(table.ok());
    const std::vector<PageId> live{table.value().desc_page_id, table.value().anchor_page_id,
                                   table.value().varheap_page_id, catalog::kCatalogPageTables};
    auto rows = db_->catalog().ListPatterns();
    ASSERT_TRUE(rows.ok());
    PageId root = kInvalidPageId;
    for (const catalog::SysPatternRow& row : rows.value()) {
        if (catalog::HasWaystoneDirectory(row)) root = row.waystone_root;
    }
    ASSERT_NE(root, kInvalidPageId) << "the opted row was given no directory";
    ASSERT_GT(kds::testing_patterns::DirChildren(db_->store(), root), 0) << "nothing recorded";
    {
        // The last slots, which the eight recorded keys are vanishingly
        // unlikely to hash to: each now names a live logged page.
        auto page = db_->store().Get(root);
        ASSERT_TRUE(page.ok());
        for (std::size_t i = 0; i < live.size(); ++i) {
            const std::size_t slot = stats::kDirFanout - 1 - i;
            ASSERT_EQ(stats::LoadDirChild(page.value().bytes(), slot), stats::kEmptyDirSlot);
            std::memcpy(page.value().bytes().data() + slot * sizeof(PageId), &live[i],
                        sizeof(PageId));
        }
    }
    AdvisoryDiscard discard;
    ASSERT_TRUE(DiscardAdvisoryState(db_->catalog(), /*waystone_recording=*/true, &db_->store(),
                                     &discard)
                    .ok());
    EXPECT_GT(discard.trail_pages_freed, 0u) << "the directory's own trails were not taken";
    for (const PageId id : live) EXPECT_TRUE(db_->store().IsAllocated(id)) << "page " << id;
    const std::string all = Run("SELECT s FROM t WHERE id = 50");
    EXPECT_NE(all.find(std::string(300, 'k')), std::string::npos) << all;
    const VolumeCensus c = Census();
    EXPECT_EQ(c.unmarked, 0u);
}

// §1's Waystone class at `OFF`: left (DV-Q4 as revised at DV-S2) - the
// directory an `OFF` clears is freed by nothing, about eight pages each.
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

// Left with `OFF`'s (DV-Q4'): a committed `DROP TABLE` ends the opt-ins
// naming the relation, clearing their roots as an `OFF` does, and the
// directory it clears is freed by nothing.
TEST_F(VolumeCensusTest, ADropTablesEndOfAnOptInStrandsItsDirectory) {
    Boot(/*waystone=*/true);
    Ok("CREATE TABLE t (id int64, v int64)", "CREATED");
    for (int i = 1; i <= 20; ++i) {
        Ok("INSERT INTO t VALUES (" + std::to_string(i) + ", 1)", "INSERTED");
    }
    Ok("WAYSTONE ON SELECT v FROM t WHERE id = 5", "OK WAYSTONE ON");
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 1; i <= 8; ++i) Run("SELECT v FROM t WHERE id = " + std::to_string(i));
    }
    EXPECT_EQ(Census().unmarked, 0u);
    Ok("DROP TABLE t", "DROPPED");
    Reboot();
    if (HasFatalFailure()) return;
    const VolumeCensus c = Census();
    Print("drop table ending an opt-in", c);
    std::uint64_t waystone = 0;
    for (const auto& [key, n] : c.unmarked_by_class) {
        EXPECT_TRUE(key.page_class == "waystone" || key.page_class == "waystone_dir")
            << key.page_class;
        waystone += n;
    }
    EXPECT_GT(waystone, 0u) << "the premise moved: a drop's end of an opt-in strands nothing";
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
    // The sim runs no in-run reclaim (its dispatcher has no reclaim queue,
    // BF-R9's), so the rollback's own job is the rig's to show
    // (`InRunReclaimTest.ARolledBackCreateIsFreedInTheRun`); here the pages
    // stand until a mount, and a clean mount owes nothing for a decided
    // transaction.
    Print("rolled-back create table x10", Census());
}

// DV-S2's mount half: a crash loser's `CREATE TABLE`, its `sys.tables` row
// retired by the mount's undo, is walked from its anchor and freed.
TEST_F(VolumeCensusTest, ACrashLosersCreateIsFreedAtTheMount) {
    Boot();
    Ok("CREATE TABLE keep (id int64, v int64)", "CREATED");
    Ok("BEGIN", "BEGIN");
    for (int i = 0; i < 4; ++i) {
        const std::string t = "l" + std::to_string(i);
        Ok("CREATE TABLE " + t + " (id int64 PRIMARY KEY, v int64, s varchar)" +
               (i % 2 == 0 ? std::string(" HEAP") : std::string()),
           "CREATED");
        for (int k = 1; k <= 60; ++k) {
            Ok("INSERT INTO " + t + " VALUES (" + std::to_string(k) + ", 1, '" +
                   std::string(400, 'z') + "')",
               "INSERTED");
        }
    }
    ASSERT_EQ(Run("SYNC"), "OK synced");
    db_->Crash();
    Status s = db_->Reboot();
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(db_->recovery().undone_creates.size(), 4u);
    ASSERT_TRUE(db_->SettleReclaim().ok());
    EXPECT_EQ(db_->reclaim_counters().refused.load(), 0u);
    const VolumeCensus c = Census();
    Print("crash loser's create table x4, after the mount", c);
    for (const auto& [key, n] : c.unmarked_by_class) {
        EXPECT_LT(key.owner, catalog::kUserOidStart)
            << key.page_class << " owned by " << key.owner << " survived: " << n;
    }
}

// `[quiet-wrong]` if it failed: **a burned oid is issued again after a
// restart** (`page.md` §2a), so a relation created before the mount's
// reclaim runs can carry the loser's oid on every page. The job walks from
// the loser's own anchor, by links, never by owner, so it frees the loser's
// pages and none of the new relation's.
TEST_F(VolumeCensusTest, ACrashLosersReclaimSparesARelationThatTookItsOid) {
    Boot();
    Ok("BEGIN", "BEGIN");
    Ok("CREATE TABLE loser (id int64, v int64, s varchar)", "CREATED");
    for (int k = 1; k <= 300; ++k) {
        Ok("INSERT INTO loser VALUES (" + std::to_string(k) + ", 1, '" + std::string(400, 'z') +
               "')",
           "INSERTED");
    }
    ASSERT_EQ(Run("SYNC"), "OK synced");
    db_->Crash();
    Status s = db_->Reboot();
    ASSERT_TRUE(s.ok()) << s.message();
    ASSERT_EQ(db_->recovery().undone_creates.size(), 1u);
    const catalog::Oid burned = db_->recovery().undone_creates.front().oid;

    // Before the mount's reclaim has run a step: the same oid, new pages.
    Ok("CREATE TABLE taker (id int64, v int64, s varchar)", "CREATED");
    auto taker = db_->catalog().FindTableOidByName(catalog::kNamespacePublic, "taker");
    ASSERT_TRUE(taker.ok());
    ASSERT_EQ(taker.value(), burned) << "the premise moved: the burned oid was not issued again";
    for (int k = 1; k <= 300; ++k) {
        Ok("INSERT INTO taker VALUES (" + std::to_string(k) + ", " + std::to_string(k) + ", '" +
               std::string(400, 'y') + "')",
           "INSERTED");
    }
    ASSERT_TRUE(db_->SettleReclaim().ok());
    EXPECT_EQ(db_->reclaim_counters().refused.load(), 0u);
    EXPECT_GT(db_->store().pages_freed(), 0u) << "the loser's pages were not freed";

    const VolumeCensus c = Census();
    Print("a loser's reclaim beside a relation with its oid", c);
    EXPECT_EQ(c.unmarked, 0u);
    for (int k : {1, 150, 300}) {
        const std::string reply = Run("SELECT v, s FROM taker WHERE id = " + std::to_string(k));
        EXPECT_NE(reply.find(std::string(400, 'y')), std::string::npos) << reply.substr(0, 80);
    }
    const std::string count = Run("SELECT COUNT(*) FROM taker");
    EXPECT_NE(count.find("300"), std::string::npos) << count;
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


// ---- DV-S1: the mount's census, asked by a boot key ---------------------------

StatusOr<std::unique_ptr<Expeditor>> MountWithCensus(const std::filesystem::path& dir,
                                                     bool census) {
    Expeditor::Config config;
    config.buffer_pool_frames = 4096;
    config.data_file = (dir / "kds.db").string();
    config.wal_dir = (dir / "wal").string();
    config.log_file = (dir / "kds.log").string();
    config.cores = 1;
    config.debug_text_port = 0;
    config.checkpoint_interval_ns = 0;
    config.volume_census_at_mount = census;
    return Expeditor::Open(config, /*now_unix_seconds=*/1000);
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::stringstream all;
    all << in.rdbuf();
    return all.str();
}

// DV-Q3 (a), DV-Q9 (a): off by default, the mount counts nothing and the view
// is empty; asked, the mount logs its census and the view carries its rows -
// here an `OFF`'s stranded directory, which DV leaves (DV-Q4').
TEST(VolumeCensusMountTest, TheBootKeyAsksTheMountForItsCensus) {
    crash_rig::TempDir dir;
    const std::string sql = "SELECT v FROM ws WHERE id = 5";
    {
        auto opened = MountWithCensus(dir.path, /*census=*/false);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        CurrentCoreGuard as(0);
        Expeditor& db = *opened.value();
        for (const std::string& line :
             {std::string("CREATE TABLE ws (id int64, v int64)"),
              std::string("INSERT INTO ws VALUES (5, 1)"), "WAYSTONE ON " + sql, sql, sql,
              "WAYSTONE OFF " + sql}) {
            const std::string reply = db.dispatcher().Dispatch(line).response;
            ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << line << ": " << reply;
        }
        EXPECT_TRUE(testing_view::ReadViewText(
                        db.dispatcher().Dispatch("SELECT * FROM sys.volume_census").response)
                        .rows.empty());
        EXPECT_EQ(ReadFile(dir.path / "kds.log").find("[census]"), std::string::npos);
        reclaim_census::CleanShutdownCheckpoint(db);
    }
    auto opened = MountWithCensus(dir.path, /*census=*/true);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    CurrentCoreGuard as(0);
    Expeditor& db = *opened.value();
    const std::string log = ReadFile(dir.path / "kds.log");
    EXPECT_NE(log.find("reached by no root"), std::string::npos) << log;
    EXPECT_NE(log.find("the mark is whole"), std::string::npos) << log;
    const testing_view::ViewText view = testing_view::ReadViewText(
        db.dispatcher().Dispatch("SELECT page_class, pages, complete FROM sys.volume_census")
            .response);
    // The census's own row first, then what it found unreached.
    ASSERT_GE(view.rows.size(), 2u) << "the OFF's directory was not counted";
    EXPECT_EQ(view.rows[0][0], "allocated");
    for (std::size_t i = 0; i < view.rows.size(); ++i) {
        const auto& row = view.rows[i];
        if (i != 0) EXPECT_TRUE(row[0] == "waystone" || row[0] == "waystone_dir") << row[0];
        EXPECT_EQ(row[2], "1");
    }
}

}  // namespace
}  // namespace kds::server
