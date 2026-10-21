// **The catalog page ceiling, lifted** (superblock 31). A catalog chain's
// pages past its fixed root come from the free map's general supply
// (`InsertRow` -> `CreateNew`), stamped with the catalog relation's oid,
// where they used to come from the reserved range 17..127 - 111 pages for
// every catalog relation together, about 800 relations of 8 columns.
//
// - **A catalog past the old range** (unit): 1,000 relations of 8 columns
//   directly through the catalog, so `sys.columns` alone outgrows the 111
//   pages; every chain walks to its end on owned heap pages, and the last
//   relation is still resolvable and insertable.
// - **A clean restart** and **a crash** over the same fill: production's
//   mount (`Expeditor`, its advisory discard included), and the
//   simulation's crash, whose redo rebuilds every growth page a lost
//   writeback took from its `PAGE_INIT`.
// - **A grown catalog page is an ordinary frame** (D2): not a pinned
//   class, evicted clean by the sweep and faulted back with its rows, while
//   the fixed root stays resident.
// - **Reclaim passes a catalog page by**: a dropped relation's pages are
//   freed by owner, so a catalog growth page - a catalog oid, in the same
//   id range - is never freed, and a freed id may become one; a reclaim
//   re-driven after a crash passes the catalog page its cut run's free
//   handed to a chain.
// - **A long `sys.patterns` at mount**: the discard clears every Waystone
//   root in one walk, whatever the chain's length.
//
// A peer reading a grown catalog page is `core_runtime_test.cpp`'s
// `APeerResolvesARelationWhoseCatalogRowsSpilledOntoAGrownPage`; the
// version refusal is `superblock_test.cpp`'s `AVersion30VolumeDoesNotMount`.
//
// **The large cells run only with `KDS_TEST_LARGE_CATALOG` set**: the push
// gate's `ctest` excludes no label in this tree (`scripts/test.sh`), so a
// cell too slow for the gate skips itself instead.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "catalog_chain_pages.hpp"
#include "reclaim_census.hpp"
#include "sim/instance.hpp"
#include "view_text.hpp"

#include "kds/base/current_core.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/server/expeditor.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_header.hpp"

namespace kds::server {
namespace {

namespace fs = std::filesystem;

// The gate's size (D7): past the old range in `sys.columns` alone.
constexpr int kTables = 1000;
constexpr int kColumns = 8;
// The excluded size: three times past it.
constexpr int kLargeTables = 3000;
// What the reserved range held, 17..127, for every catalog relation.
constexpr std::size_t kOldRangePages = 111;

bool LargeCellsEnabled() { return std::getenv("KDS_TEST_LARGE_CATALOG") != nullptr; }

std::string TableName(int i) { return "t" + std::to_string(i); }

// `id` and `c1`..`c7`, every one int64.
catalog::Schema EightColumns() {
    catalog::Schema schema;
    for (int i = 0; i < kColumns; ++i) {
        catalog::SysColumnRow col{};
        col.pos = static_cast<std::uint32_t>(i);
        catalog::SetName(col.name, i == 0 ? std::string("id") : "c" + std::to_string(i));
        col.type_val = catalog::kTypeValInt64;
        col.len = 8;
        col.notnull = true;
        schema.columns.push_back(col);
    }
    return schema;
}

std::string CreateStatement(int i) {
    std::string sql = "CREATE TABLE " + TableName(i) + " (id int64";
    for (int c = 1; c < kColumns; ++c) sql += ", c" + std::to_string(c) + " int64";
    return sql + ")";
}

// What the walk of every catalog chain found.
struct ChainCensus {
    std::size_t columns_pages = 0;  // sys.columns' chain, root included
    std::size_t grown_pages = 0;    // every chain's pages past its root
};

// Every catalog chain walks to its end, and every page past a root is an
// allocated heap page of the general supply owned by the chain's relation.
template <typename Store>
ChainCensus ExpectOwnedChains(Store& store) {
    ChainCensus census;
    for (const catalog::CatalogRoot& root : catalog::kCatalogRoots) {
        auto chain = testing_catalog::CatalogChainPages(store, root.page);
        EXPECT_TRUE(chain.ok()) << chain.status().message();
        if (!chain.ok()) continue;
        if (root.page == catalog::kCatalogPageColumns) census.columns_pages = chain.value().size();
        census.grown_pages += chain.value().size() - 1;
        for (std::size_t at = 1; at < chain.value().size(); ++at) {
            const PageId id = chain.value()[at];
            EXPECT_GE(id, kFirstUserPageId) << "root " << root.page << ": page " << id;
            if constexpr (requires { store.IsAllocated(id); }) {
                EXPECT_TRUE(store.IsAllocated(id)) << "root " << root.page << ": page " << id;
            }
            auto bytes = store.GetForRead(id);
            EXPECT_TRUE(bytes.ok()) << "root " << root.page << ": page " << id;
            if (!bytes.ok()) continue;
            EXPECT_EQ(storage::RawPageType(bytes.value().bytes()),
                      static_cast<std::uint8_t>(PageType::kHeap))
                << "root " << root.page << ": page " << id;
            EXPECT_EQ(storage::GetOwnerOid(bytes.value().bytes()), root.oid)
                << "root " << root.page << ": page " << id;
        }
    }
    return census;
}

// ---- A catalog past the old range, directly ------------------------------

void FillTheCatalogDirectly(int tables) {
    storage::InMemoryPageStore store{kFirstUserPageId};
    catalog::Catalog catalog(store, storage::kDefaultInlineCellWidth);
    ASSERT_TRUE(catalog.Bootstrap().ok());

    std::vector<catalog::Oid> oids;
    for (int i = 0; i < tables; ++i) {
        auto oid = catalog.CreateTable(catalog::kNamespacePublic, TableName(i), EightColumns());
        ASSERT_TRUE(oid.ok()) << "relation " << i << ": " << oid.status().message();
        oids.push_back(oid.value());
    }

    // The last relation's rows are the furthest along every chain.
    const catalog::Oid last = oids.back();
    auto found = catalog.FindTableOidByName(catalog::kNamespacePublic, TableName(tables - 1));
    ASSERT_TRUE(found.ok()) << found.status().message();
    EXPECT_EQ(found.value(), last);
    auto schema = catalog.BuildSchemaFromColumns(last);
    ASSERT_TRUE(schema.ok()) << schema.status().message();
    ASSERT_EQ(schema.value().columns.size(), static_cast<std::size_t>(kColumns));
    EXPECT_EQ(catalog::NameView(schema.value().columns.back().name), "c7");
    auto first_id = catalog.AllocateRowId(last);
    ASSERT_TRUE(first_id.ok()) << first_id.status().message();
    auto second_id = catalog.AllocateRowId(last);
    ASSERT_TRUE(second_id.ok()) << second_id.status().message();
    EXPECT_EQ(second_id.value(), first_id.value() + 1);

    const ChainCensus census = ExpectOwnedChains(store);
    EXPECT_GT(census.columns_pages, kOldRangePages)
        << "sys.columns alone should outgrow what the reserved range held";
}

TEST(CatalogPageCeilingTest, AThousandRelationsOfEightColumnsOutgrowTheOldRange) {
    FillTheCatalogDirectly(kTables);
}

TEST(CatalogPageCeilingTest, ThreeThousandRelationsOfEightColumns) {
    if (!LargeCellsEnabled()) GTEST_SKIP() << "set KDS_TEST_LARGE_CATALOG to run";
    FillTheCatalogDirectly(kLargeTables);
}

// ---- Through a mount ------------------------------------------------------

struct TempDir {
    fs::path path;
    TempDir() {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("kds_ceiling_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// Production's mount (`reclaim_census.hpp`'s), relaxed so a fill of a
// thousand DDLs does not wait on a thousand syncs: the clean stop is what
// makes the run durable.
StatusOr<std::unique_ptr<Expeditor>> Mount(const fs::path& dir, bool waystone_recording = true) {
    return reclaim_census::Mount(dir, /*cores=*/1, wal::DurabilityClass::kRelaxed,
                                 waystone_recording);
}

// A `sys.patterns` row, which only an opt-in writes since CF-S1b.
Status OptIn(catalog::Catalog& catalog, std::uint64_t fetch_id, PageId root = kInvalidPageId) {
    return catalog.SetPatternWaystone(fetch_id, catalog::kStmtClassUnclassified, /*on=*/true, root)
        .status();
}

using reclaim_census::CleanShutdownCheckpoint;
using reclaim_census::CrashImage;
using reclaim_census::Settle;

using Sql = std::function<std::string(const std::string&)>;

void CreateTables(const Sql& run, int from, int to) {
    for (int i = from; i < to; ++i) {
        const std::string reply = run(CreateStatement(i));
        ASSERT_EQ(reply.substr(0, 7), "CREATED") << TableName(i) << ": " << reply;
    }
}

// One row into each of `tables`, the tuple key issued.
void InsertOneRow(const Sql& run, const std::vector<int>& tables, int value) {
    for (const int i : tables) {
        std::string sql = "INSERT INTO " + TableName(i) + " VALUES (";
        for (int c = 1; c < kColumns; ++c) {
            if (c > 1) sql += ", ";
            sql += std::to_string(value);
        }
        const std::string reply = run(sql + ")");
        ASSERT_EQ(reply.substr(0, 8), "INSERTED") << TableName(i) << ": " << reply;
    }
}

// Each of `tables` describes with its eight columns and holds `rows` rows
// whose `c7` is `values`, in insertion order.
void ExpectTables(const Sql& run, const std::vector<int>& tables,
                  const std::vector<std::string>& values) {
    std::vector<std::string> names{"id"};
    for (int c = 1; c < kColumns; ++c) names.push_back("c" + std::to_string(c));
    for (const int i : tables) {
        const auto described = testing_view::ReadViewText(run("DESCRIBE " + TableName(i)));
        EXPECT_EQ(described.Column("name"), names) << TableName(i);
        const auto rows = testing_view::ReadViewText(run("SELECT c7 FROM " + TableName(i)));
        EXPECT_EQ(rows.Column("c7"), values) << TableName(i);
    }
}

const std::vector<int> kProbed{0, kTables / 2, kTables - 1};

TEST(CatalogPageCeilingTest, ACleanRestartReadsACatalogPastTheOldRange) {
    TempDir dir;
    {
        auto opened = Mount(dir.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        const Sql run = [&](const std::string& sql) { return db.dispatcher().Dispatch(sql).response; };
        CreateTables(run, 0, kTables);
        InsertOneRow(run, kProbed, 1);
        EXPECT_GT(ExpectOwnedChains(db.store()).columns_pages, kOldRangePages);
        CleanShutdownCheckpoint(db);
    }
    auto opened = Mount(dir.path);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    const Sql run = [&](const std::string& sql) { return db.dispatcher().Dispatch(sql).response; };
    EXPECT_GT(ExpectOwnedChains(db.store()).columns_pages, kOldRangePages);
    ExpectTables(run, kProbed, {"1"});
    InsertOneRow(run, kProbed, 2);
    ExpectTables(run, kProbed, {"1", "2"});
}

TEST(CatalogPageCeilingTest, ACrashRecoversACatalogPastTheOldRange) {
    auto created = sim::SimInstance::Create();
    ASSERT_TRUE(created.ok()) << created.status().message();
    std::unique_ptr<sim::SimInstance> instance = std::move(created.value());
    const Sql run = [&](const std::string& sql) { return instance->Execute(sql); };
    // Most of the fill relaxed, a `SYNC` per hundred: the harness runs no
    // writeback of its own, so the sync is what cleans the pool, which is
    // smaller than the fill and evicts catalog pages as it goes.
    constexpr int kSynced = kTables - 100;
    ASSERT_EQ(run("SET DURABILITY RELAXED"), "SET durability=relaxed");
    for (int from = 0; from < kSynced; from += 100) {
        CreateTables(run, from, from + 100);
        ASSERT_EQ(run("SYNC"), "OK synced");
    }
    // The rest strict: each commit's log is durable and its pages are not,
    // so the crash loses the growth pages these relations' rows landed on
    // and redo rebuilds them from their `PAGE_INIT`s.
    ASSERT_EQ(run("SET DURABILITY STRICT"), "SET durability=strict");
    const std::size_t synced_pages = ExpectOwnedChains(instance->store()).grown_pages;
    CreateTables(run, kSynced, kTables);
    ASSERT_GT(ExpectOwnedChains(instance->store()).grown_pages, synced_pages)
        << "no catalog chain grew after the last sync; the crash would rebuild none";
    InsertOneRow(run, kProbed, 1);
    // Unsynced past this point, so lost to the crash.
    ASSERT_EQ(run("SET DURABILITY RELAXED"), "SET durability=relaxed");
    InsertOneRow(run, kProbed, 2);

    instance->Crash();
    ASSERT_TRUE(instance->Reboot().ok());
    EXPECT_GT(instance->recovery().redo_applied, 0u) << "the crash left redo nothing to do";

    EXPECT_GT(ExpectOwnedChains(instance->store()).columns_pages, kOldRangePages);
    ExpectTables(run, kProbed, {"1"});
    ASSERT_EQ(run("SET DURABILITY RELAXED"), "SET durability=relaxed");
    InsertOneRow(run, kProbed, 3);
    ExpectTables(run, kProbed, {"1", "3"});
}

// ---- A grown catalog page is an ordinary frame ---------------------------

TEST(CatalogPageCeilingTest, AGrownCatalogPageIsEvictedAndFaultedBack) {
    // D2: only the fixed system range is resident by class. A regression
    // that pinned the growth again - a raised resident limit, a kind-based
    // pin - would keep every catalog page in the pool and pass every other
    // cell here.
    auto device = storage::MemoryPageDevice::Create(/*extent_pages=*/8, /*initial_pages=*/0);
    ASSERT_TRUE(device.ok()) << device.status().message();
    auto opened = storage::DevicePageStore::Open(*device.value(), storage::FrameCapacity{4096},
                                                 /*first_new_page_id=*/kFirstUserPageId);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    storage::DevicePageStore& store = *opened.value();
    // The floor production installs (`Expeditor::Open`).
    store.SetResidentLimit(kFirstUserPageId);
    catalog::Catalog catalog(store, storage::kDefaultInlineCellWidth);
    ASSERT_TRUE(catalog.Bootstrap().ok());

    // Until `sys.columns` has two pages past its root; the last relation's
    // rows end on the tail.
    catalog::Oid last = 0;
    std::vector<PageId> chain;
    for (int i = 0; i < 100 && chain.size() < 3; ++i) {
        auto oid = catalog.CreateTable(catalog::kNamespacePublic, TableName(i), EightColumns());
        ASSERT_TRUE(oid.ok()) << "relation " << i << ": " << oid.status().message();
        last = oid.value();
        auto walked = testing_catalog::CatalogChainPages(store, catalog::kCatalogPageColumns);
        ASSERT_TRUE(walked.ok()) << walked.status().message();
        chain = walked.value();
    }
    ASSERT_GE(chain.size(), 3u) << "sys.columns never grew; the cell tests nothing";
    EXPECT_TRUE(store.IsPinnedClass(catalog::kCatalogPageColumns)) << "the fixed root is evictable";
    for (std::size_t at = 1; at < chain.size(); ++at) {
        EXPECT_FALSE(store.IsPinnedClass(chain[at])) << "grown page " << chain[at] << " is pinned";
    }

    // Clean, then swept for more laps than the usage cap: every clean
    // unpinned frame falls.
    ASSERT_TRUE(store.Sync().ok());
    for (int lap = 0; lap < 2 * storage::DevicePageStore::kClockUsageCap + 2; ++lap) {
        store.EvictColdFrames(store.resident_pages());
    }

    // The walk faults every grown page back off the device, checksum
    // checked, and none of the fixed root.
    device.value()->ClearTrace();
    auto walked = testing_catalog::CatalogChainPages(store, catalog::kCatalogPageColumns);
    ASSERT_TRUE(walked.ok()) << walked.status().message();
    EXPECT_EQ(walked.value(), chain);
    const auto read_back = [&](PageId id) {
        for (const storage::MemoryPageDevice::TraceEntry& op : device.value()->trace()) {
            if (op.kind == storage::MemoryPageDevice::OpKind::kRead && op.first_page_id <= id &&
                id < op.first_page_id + op.nr_pages) {
                return true;
            }
        }
        return false;
    };
    EXPECT_FALSE(read_back(catalog::kCatalogPageColumns)) << "the fixed root was evicted";
    for (std::size_t at = 1; at < chain.size(); ++at) {
        EXPECT_TRUE(read_back(chain[at])) << "grown page " << chain[at] << " was never evicted";
    }
    auto schema = catalog.BuildSchemaFromColumns(last);
    ASSERT_TRUE(schema.ok()) << schema.status().message();
    ASSERT_EQ(schema.value().columns.size(), static_cast<std::size_t>(kColumns));
    EXPECT_EQ(catalog::NameView(schema.value().columns.back().name), "c7");
}

// ---- Reclaim passes a catalog page by -------------------------------------

TEST(CatalogPageCeilingTest, AReclaimNeverFreesACatalogPageAndAFreedIdMayBecomeOne) {
    // Relations created in turn interleave their pages with the catalog's
    // growth, so the dropped relations' ids sit among catalog pages, and
    // the reclaim's owner test is all that tells them apart.
    constexpr int kCreated = 200;
    constexpr int kDropped = 20;
    TempDir dir;
    std::set<PageId> catalog_pages;
    std::set<PageId> dropped_pages;
    {
        auto opened = Mount(dir.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        const Sql run = [&](const std::string& sql) { return db.dispatcher().Dispatch(sql).response; };
        CreateTables(run, 0, kCreated);
        std::set<std::uint64_t> dropped_oids;
        for (int i = 0; i < kDropped; ++i) {
            dropped_oids.insert(reclaim_census::DescribeField(db.dispatcher(), TableName(i), "oid"));
        }
        for (const auto& [id, owner] : reclaim_census::AllocatedPages(db.store())) {
            if (dropped_oids.count(owner) != 0) dropped_pages.insert(id);
        }
        ASSERT_GE(dropped_pages.size(), dropped_oids.size());
        for (int i = 0; i < kDropped; ++i) {
            const std::string reply = run("DROP TABLE " + TableName(i));
            ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
        }
        auto every = testing_catalog::EveryCatalogPage(db.store());
        ASSERT_TRUE(every.ok()) << every.status().message();
        catalog_pages = every.value();
        ASSERT_GT(ExpectOwnedChains(db.store()).grown_pages, 0u);
        CleanShutdownCheckpoint(db);
    }

    auto opened = Mount(dir.path);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    const Sql run = [&](const std::string& sql) { return db.dispatcher().Dispatch(sql).response; };
    Settle(db);
    EXPECT_EQ(db.reclaim_counters().refused.load(), 0u);
    for (const PageId id : dropped_pages) {
        EXPECT_FALSE(db.store().IsAllocated(id)) << "page " << id << " of a dropped relation";
    }
    for (const PageId id : catalog_pages) {
        EXPECT_TRUE(db.store().IsAllocated(id)) << "catalog page " << id << " was freed";
    }
    ExpectOwnedChains(db.store());

    // The freed ids are handed out ahead of the cursor, so the catalog's
    // next growth takes one: a dropped user id becoming a catalog page is a
    // new creation (invariant 2), stamped with the catalog relation's oid.
    bool reused = false;
    for (int i = kCreated; i < kCreated + 200 && !reused; ++i) {
        CreateTables(run, i, i + 1);
        auto every = testing_catalog::EveryCatalogPage(db.store());
        ASSERT_TRUE(every.ok()) << every.status().message();
        for (const PageId id : every.value()) {
            if (catalog_pages.count(id) == 0 && dropped_pages.count(id) != 0) reused = true;
        }
    }
    EXPECT_TRUE(reused) << "no catalog chain grew onto an id a dropped relation held";
    ExpectOwnedChains(db.store());
    ExpectTables(run, {kDropped, kCreated - 1}, {});
}

TEST(CatalogPageCeilingTest, AReclaimRedrivenAfterACrashPassesACatalogPageOnAnIdItFreed) {
    // The crash the owner check is all that stands in front of (D6): a
    // reclaim cut after its first free, the freed id already a catalog
    // growth page, and the next mount's re-drive walking from the same roots
    // to it. `FreePage`'s system floor covers no id past 128, so the
    // walk's tests of what it meets - owner, then type, then level - are
    // what keep the catalog page.
    //
    // `sys.patterns` grows the catalog here because it allocates nothing
    // else: the freed id is the only one listed and the lowest is handed out
    // first, so the chain's next page is that id.
    TempDir dir;
    std::set<PageId> dropped_pages;
    {
        auto opened = Mount(dir.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        const Sql run = [&](const std::string& sql) { return db.dispatcher().Dispatch(sql).response; };
        ASSERT_EQ(run("CREATE TABLE big (id int64, v varchar)").substr(0, 7), "CREATED");
        reclaim_census::Fill(db.dispatcher(), "big", 1500);
        dropped_pages = reclaim_census::PagesOf(
            db.store(), reclaim_census::DescribeField(db.dispatcher(), "big", "oid"));
        ASSERT_GE(dropped_pages.size(), 3u);
        ASSERT_EQ(run("DROP TABLE big").rfind("ERR", 0), std::string::npos);
        CleanShutdownCheckpoint(db);
    }

    // Two copies of one crash image: one mounted with recording off, one
    // with it on (below).
    TempDir image, image_recording;
    PageId reused = kInvalidPageId;
    {
        auto opened = Mount(dir.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        db.SetReclaimCutForTest([&](const char* what) {
            if (reused != kInvalidPageId || std::string(what) != "after a free") return;
            PageId freed = kInvalidPageId;
            for (const PageId id : dropped_pages) {
                if (!db.store().IsAllocated(id)) freed = id;
            }
            ASSERT_NE(freed, kInvalidPageId) << "the cut came before any free";
            for (std::uint64_t fetch_id = 0x6000'0000; reused == kInvalidPageId; ++fetch_id) {
                ASSERT_LT(fetch_id, 0x6000'0000u + 20000) << "sys.patterns never took id " << freed;
                ASSERT_TRUE(OptIn(db.catalog(), fetch_id).ok());
                auto chain =
                    testing_catalog::CatalogChainPages(db.store(), catalog::kCatalogPagePatterns);
                ASSERT_TRUE(chain.ok()) << chain.status().message();
                for (const PageId id : chain.value()) {
                    if (id == freed) reused = id;
                }
            }
            ASSERT_TRUE(db.Sync().ok());
            CrashImage(dir.path, image.path);
            CrashImage(dir.path, image_recording.path);
        });
        Settle(db);
        db.SetReclaimCutForTest(nullptr);
        ASSERT_NE(reused, kInvalidPageId);
    }

    // The re-drive on each image. With recording off the mount creates no
    // page, so every id the cut reclaim left is the re-drive's to free. With
    // it on - the default - the renewal runs after recovery and before the
    // re-drive (CF-S1b), giving each opted row a fresh headerless directory,
    // which can take an id the cut reclaim freed: the re-drive must then
    // walk past it on its owner bytes and free none of the renewed roots.
    for (const bool recording : {false, true}) {
        SCOPED_TRACE(recording ? "waystone_recording on" : "waystone_recording off");
        auto mounted =
            Mount(recording ? image_recording.path : image.path, /*waystone_recording=*/recording);
        ASSERT_TRUE(mounted.ok()) << mounted.status().message();
        Expeditor& db = *mounted.value();
        CurrentCoreGuard as(0);
        std::set<PageId> renewed;
        if (recording) {
            auto rows = db.catalog().ListPatterns();
            ASSERT_TRUE(rows.ok()) << rows.status().message();
            for (const catalog::SysPatternRow& row : rows.value()) {
                if ((row.flags & catalog::kPatternWaystone) == 0) continue;
                ASSERT_TRUE(catalog::HasWaystoneDirectory(row)) << "fetch_id " << row.fetch_id;
                renewed.insert(row.waystone_root);
            }
            ASSERT_FALSE(renewed.empty()) << "the premise moved: nothing was renewed";
        }
        ASSERT_EQ(db.reclaim_counters().pending.load(), 1u) << "the reclaim was not cut";
        Settle(db);
        EXPECT_EQ(db.reclaim_counters().pending.load(), 0u);
        EXPECT_EQ(db.reclaim_counters().refused.load(), 0u);
        EXPECT_GT(db.reclaim_counters().skipped.load(), 0u)
            << "the re-drive never met the catalog page, so the owner check was not exercised";
        EXPECT_TRUE(db.store().IsAllocated(reused)) << "catalog page " << reused << " was freed";
        for (const PageId id : renewed) {
            EXPECT_TRUE(db.store().IsAllocated(id)) << "renewed directory " << id << " was freed";
        }
        for (const PageId id : dropped_pages) {
            if (id == reused || renewed.count(id) != 0) continue;
            EXPECT_FALSE(db.store().IsAllocated(id)) << "page " << id << " of big";
        }
        auto chain = testing_catalog::CatalogChainPages(db.store(), catalog::kCatalogPagePatterns);
        ASSERT_TRUE(chain.ok()) << chain.status().message();
        EXPECT_NE(std::find(chain.value().begin(), chain.value().end(), reused),
                  chain.value().end())
            << "page " << reused << " left sys.patterns' chain";
        ExpectOwnedChains(db.store());
    }
}

// ---- A long sys.patterns at mount -----------------------------------------

// `patterns` opted in, every tenth given a Waystone directory, then a
// clean restart with `waystone_recording` off: the mount's renewal clears
// every root and creates none, keeps every opt-in, and the mount succeeds
// whatever the chain's length. Recording off, because a renewal with it on
// creates a directory per opted row, which at 17,000 rows is a cost this
// cell does not measure; the renewal's create is
// `RenewingEveryWaystoneRootDropsTheCachedPatterns`'s.
void ExpectTheMountClearsEveryPattern(int patterns, std::size_t min_pages) {
    constexpr std::uint64_t kFirstFetchId = 0x5000'0000;
    TempDir dir;
    {
        auto opened = Mount(dir.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        for (int k = 0; k < patterns; ++k) {
            const std::uint64_t fetch_id = kFirstFetchId + static_cast<std::uint64_t>(k);
            // Never read before the renewal clears it, so any id serves.
            const Status opted =
                OptIn(db.catalog(), fetch_id, k % 10 == 0 ? kFirstUserPageId : kInvalidPageId);
            ASSERT_TRUE(opted.ok()) << k << ": " << opted.message();
        }
        auto chain = testing_catalog::CatalogChainPages(db.store(), catalog::kCatalogPagePatterns);
        ASSERT_TRUE(chain.ok()) << chain.status().message();
        ASSERT_GT(chain.value().size(), min_pages);
        CleanShutdownCheckpoint(db);
    }
    auto opened = Mount(dir.path, /*waystone_recording=*/false);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    auto listed = db.catalog().ListPatterns();
    ASSERT_TRUE(listed.ok()) << listed.status().message();
    std::size_t ours = 0;
    for (const catalog::SysPatternRow& row : listed.value()) {
        if (row.fetch_id < kFirstFetchId) continue;
        ++ours;
        EXPECT_FALSE(catalog::HasWaystoneDirectory(row)) << "fetch_id " << row.fetch_id;
        EXPECT_EQ(row.waystone_root, kInvalidPageId) << "fetch_id " << row.fetch_id;
        EXPECT_NE(row.flags & catalog::kPatternWaystone, 0) << "fetch_id " << row.fetch_id;
        auto cached = db.catalog().FindPattern(row.fetch_id);
        ASSERT_TRUE(cached.ok()) << cached.status().message();
        EXPECT_EQ(cached.value()->waystone_root, kInvalidPageId) << "fetch_id " << row.fetch_id;
    }
    EXPECT_EQ(ours, static_cast<std::size_t>(patterns));
    ExpectOwnedChains(db.store());
}

// The renewal's two walks over a chain of several pages, the cache
// included `[quiet-wrong]`: a pattern cached before the renewal answers the
// renewed root after it, never the previous run's, whose children could be
// ids the cursor has handed to logged pages (CF-S1b, §8.3 Mount).
TEST(CatalogPageCeilingTest, RenewingEveryWaystoneRootDropsTheCachedPatterns) {
    storage::InMemoryPageStore store{kFirstUserPageId};
    catalog::Catalog catalog(store, storage::kDefaultInlineCellWidth);
    ASSERT_TRUE(catalog.Bootstrap().ok());
    // Opted with no directory, so the stale roots below are the only ones.
    for (std::uint64_t fetch_id = 1; fetch_id <= 300; ++fetch_id) {
        ASSERT_TRUE(OptIn(catalog, fetch_id).ok());
    }
    // One on the root page, one on the chain's last page, each rooted at an
    // id that stands for the previous run's directory, and cached.
    for (const std::uint64_t fetch_id : {std::uint64_t{1}, std::uint64_t{300}}) {
        ASSERT_TRUE(OptIn(catalog, fetch_id, kFirstUserPageId).ok());
        auto cached = catalog.FindPattern(fetch_id);
        ASSERT_TRUE(cached.ok()) << cached.status().message();
        ASSERT_EQ(cached.value()->waystone_root, kFirstUserPageId);
    }
    auto chain = testing_catalog::CatalogChainPages(store, catalog::kCatalogPagePatterns);
    ASSERT_TRUE(chain.ok()) << chain.status().message();
    ASSERT_GT(chain.value().size(), 1u);

    auto renewed = catalog.RenewPatternDirectories(/*waystone_recording=*/true);
    ASSERT_TRUE(renewed.ok()) << renewed.status().message();
    EXPECT_EQ(renewed.value(), 300u);
    std::set<PageId> roots;
    for (std::uint64_t fetch_id = 1; fetch_id <= 300; ++fetch_id) {
        auto cached = catalog.FindPattern(fetch_id);
        ASSERT_TRUE(cached.ok()) << cached.status().message();
        EXPECT_NE(cached.value()->waystone_root, kFirstUserPageId) << fetch_id;
        EXPECT_EQ(cached.value()->dir_depth, 1u) << fetch_id;
        auto row = catalog.GetSysPatternRow(fetch_id);
        ASSERT_TRUE(row.ok()) << row.status().message();
        EXPECT_EQ(row.value().waystone_root, cached.value()->waystone_root) << fetch_id;
        roots.insert(row.value().waystone_root);
    }
    EXPECT_EQ(roots.size(), 300u) << "two opted rows share one fresh directory";

    // Recording off: every root cleared and none created, the bits kept.
    auto cleared = catalog.RenewPatternDirectories(/*waystone_recording=*/false);
    ASSERT_TRUE(cleared.ok()) << cleared.status().message();
    EXPECT_EQ(cleared.value(), 300u);
    for (const std::uint64_t fetch_id : {std::uint64_t{1}, std::uint64_t{300}}) {
        auto cached = catalog.FindPattern(fetch_id);
        ASSERT_TRUE(cached.ok()) << cached.status().message();
        EXPECT_FALSE(cached.value()->has_waystone_directory()) << fetch_id;
        EXPECT_NE(cached.value()->flags & catalog::kPatternWaystone, 0) << fetch_id;
    }
}

TEST(CatalogPageCeilingTest, AMountClearsEveryWaystoneRootOfALongPatternChain) {
    ExpectTheMountClearsEveryPattern(/*patterns=*/1500, /*min_pages=*/8);
}

TEST(CatalogPageCeilingTest, AMountClearsEveryWaystoneRootOfAPatternChainPastTheOldBound) {
    // Past 128 pages: the old walk bound, at which every mount refused.
    if (!LargeCellsEnabled()) GTEST_SKIP() << "set KDS_TEST_LARGE_CATALOG to run";
    ExpectTheMountClearsEveryPattern(/*patterns=*/17000, /*min_pages=*/128);
}

}  // namespace
}  // namespace kds::server
