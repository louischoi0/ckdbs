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
// - **Reclaim passes a catalog page by**: a dropped relation's pages are
//   freed by owner, so a catalog growth page - a catalog oid, in the same
//   id range - is never freed, and a freed id may become one.
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
#include "kds/storage/in_memory_page_store.hpp"
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

// Production's mount (`drop_table_reclaim_test.cpp`'s), relaxed so a fill
// of a thousand DDLs does not wait on a thousand syncs: the clean stop
// below is what makes the run durable.
StatusOr<std::unique_ptr<Expeditor>> Mount(const fs::path& dir) {
    Expeditor::Config config;
    config.buffer_pool_frames = 4096;  // required (BE-R3)
    config.data_file = (dir / "kds.db").string();
    config.wal_dir = (dir / "wal").string();
    config.log_file = {};
    config.cores = 1;
    config.debug_text_port = 0;
    config.checkpoint_interval_ns = 0;
    config.durability = wal::DurabilityClass::kRelaxed;
    return Expeditor::Open(config, /*now_unix_seconds=*/1000);
}

// `CoreRuntime::ShutdownCheckpoint`'s order: pages, then the checkpoint.
void CleanShutdownCheckpoint(Expeditor& db) {
    ASSERT_TRUE(db.wal().Flush().ok());
    ASSERT_TRUE(db.store().Sync().ok());
    ASSERT_TRUE(db.Checkpoint().ok());
}

// The mount's pending reclaim, driven by hand (`drop_table_reclaim_test.cpp`).
void Settle(Expeditor& db) {
    for (int step = 0; step < 100000; ++step) {
        if (db.reclaim_counters().pending.load() == 0 ||
            db.reclaim_counters().refused.load() != 0) {
            break;
        }
        ASSERT_TRUE(db.ReclaimStep().ok());
    }
    EXPECT_EQ(db.reclaim_counters().refused.load(), 0u);
}

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

// ---- A long sys.patterns at mount -----------------------------------------

// `patterns` registered, every tenth given a Waystone directory, then a
// clean restart: the mount's discard clears every root in one walk, and
// the mount succeeds whatever the chain's length.
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
            auto registered = db.catalog().RegisterPattern(fetch_id, catalog::kStmtClassUnclassified);
            ASSERT_TRUE(registered.ok()) << k << ": " << registered.status().message();
            if (k % 10 != 0) continue;
            // Never read before the discard clears it, so any id serves.
            auto claimed = db.catalog().ClaimPatternWaystoneRoot(fetch_id, kFirstUserPageId, 1);
            ASSERT_TRUE(claimed.ok()) << k << ": " << claimed.status().message();
        }
        auto chain = testing_catalog::CatalogChainPages(db.store(), catalog::kCatalogPagePatterns);
        ASSERT_TRUE(chain.ok()) << chain.status().message();
        ASSERT_GT(chain.value().size(), min_pages);
        CleanShutdownCheckpoint(db);
    }
    auto opened = Mount(dir.path);
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
        auto cached = db.catalog().FindPattern(row.fetch_id);
        ASSERT_TRUE(cached.ok()) << cached.status().message();
        EXPECT_EQ(cached.value()->waystone_root, kInvalidPageId) << "fetch_id " << row.fetch_id;
    }
    EXPECT_EQ(ours, static_cast<std::size_t>(patterns));
    ExpectOwnedChains(db.store());
}

// The one walk writes what `ClaimPatternWaystoneRoot`'s clearing arm
// writes, the cache included: a cached pattern no longer names the root.
TEST(CatalogPageCeilingTest, ClearingEveryWaystoneRootUpdatesTheCachedPatterns) {
    storage::InMemoryPageStore store{kFirstUserPageId};
    catalog::Catalog catalog(store, storage::kDefaultInlineCellWidth);
    ASSERT_TRUE(catalog.Bootstrap().ok());
    for (std::uint64_t fetch_id = 1; fetch_id <= 300; ++fetch_id) {
        ASSERT_TRUE(catalog.RegisterPattern(fetch_id, catalog::kStmtClassUnclassified).ok());
    }
    // One on the root page, one on the chain's last page.
    for (const std::uint64_t fetch_id : {std::uint64_t{1}, std::uint64_t{300}}) {
        ASSERT_TRUE(catalog.ClaimPatternWaystoneRoot(fetch_id, kFirstUserPageId, 1).ok());
        auto cached = catalog.FindPattern(fetch_id);
        ASSERT_TRUE(cached.ok()) << cached.status().message();
        ASSERT_EQ(cached.value()->waystone_root, kFirstUserPageId);
    }
    auto chain = testing_catalog::CatalogChainPages(store, catalog::kCatalogPagePatterns);
    ASSERT_TRUE(chain.ok()) << chain.status().message();
    ASSERT_GT(chain.value().size(), 1u);

    ASSERT_TRUE(catalog.ClearEveryPatternWaystoneRoot().ok());
    for (const std::uint64_t fetch_id : {std::uint64_t{1}, std::uint64_t{300}}) {
        auto cached = catalog.FindPattern(fetch_id);
        ASSERT_TRUE(cached.ok()) << cached.status().message();
        EXPECT_EQ(cached.value()->waystone_root, kInvalidPageId) << fetch_id;
        EXPECT_EQ(cached.value()->dir_depth, 0u) << fetch_id;
        auto row = catalog.GetSysPatternRow(fetch_id);
        ASSERT_TRUE(row.ok()) << row.status().message();
        EXPECT_FALSE(catalog::HasWaystoneDirectory(row.value())) << fetch_id;
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
