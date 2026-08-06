#include "kds/catalog/catalog.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "kds/catalog/well_known.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"

namespace kds::catalog {
namespace {

class CatalogTest : public ::testing::Test {
protected:
    storage::InMemoryPageStore store_{server_first_new_page_id_};
    Catalog catalog_{store_};

    // Matches kds::server::kFirstUserPageId (128) so freshly-created user
    // tables never collide with the fixed catalog pages (4-8).
    static constexpr PageId server_first_new_page_id_ = 128;
};

// Every relation needs a first column that can carry the Keystone id
// (heap-and-tuple.md section 4), so a schema-less CreateTable is no longer
// a legal table to build a fixture on.
Schema MinimalPkSchema() {
    Schema schema;
    SysColumnRow col{};
    col.pos = 0;
    SetName(col.name, "id");
    col.type_val = kTypeValInt64;
    col.len = 8;
    col.notnull = true;
    schema.columns.push_back(col);
    return schema;
}

TEST_F(CatalogTest, BootstrapSucceeds) {
    Status s = catalog_.Bootstrap();
    EXPECT_TRUE(s.ok()) << s.message();
}

TEST_F(CatalogTest, BootstrapRegistersWellKnownObjects) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    const SysObjectRow* sys_ns = catalog_.sys_objects().GetByOid(kNamespaceSys);
    ASSERT_NE(sys_ns, nullptr);
    EXPECT_EQ(NameView(sys_ns->name), "namespaceSys");

    const SysObjectRow* by_name = catalog_.sys_objects().GetByName("type_uint64");
    ASSERT_NE(by_name, nullptr);
    EXPECT_EQ(by_name->oid, kTypeUint64);
}

TEST_F(CatalogTest, BootstrapMakesSysTablesFindableByName) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    auto oid = catalog_.FindTableOidByName("tables");
    ASSERT_TRUE(oid.ok());
    EXPECT_EQ(oid.value(), kSysTablesTable);

    auto row = catalog_.GetSysTableRow(kSysObjectsTable);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(NameView(row.value().name), "objects");
    EXPECT_EQ(row.value().desc_page_id, kCatalogPageObjects);
}

TEST_F(CatalogTest, CreateTableInsertsObjectTableAndColumnRows) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    Schema schema;
    SysColumnRow id_col{};
    id_col.pos = 0;
    SetName(id_col.name, "id");
    id_col.type_val = 5;  // uint64, per Bootstrap()'s placeholder type_val table
    id_col.len = 8;
    id_col.notnull = true;
    schema.columns.push_back(id_col);

    SysColumnRow name_col{};
    name_col.pos = 1;
    SetName(name_col.name, "name");
    name_col.type_val = 9;  // varchar
    name_col.len = 0;
    name_col.notnull = false;
    schema.columns.push_back(name_col);

    auto oid = catalog_.CreateTable(kNamespacePublic, "accounts", schema, ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    auto found_oid = catalog_.FindTableOidByName("accounts");
    ASSERT_TRUE(found_oid.ok());
    EXPECT_EQ(found_oid.value(), oid.value());

    auto table_row = catalog_.GetSysTableRow(oid.value());
    ASSERT_TRUE(table_row.ok());
    EXPECT_EQ(NameView(table_row.value().name), "accounts");
    EXPECT_EQ(table_row.value().namespace_oid, kNamespacePublic);
    EXPECT_EQ(table_row.value().clustered_type, ClusteredType::kHeap);

    auto built_schema = catalog_.BuildSchemaFromColumns(oid.value());
    ASSERT_TRUE(built_schema.ok());
    ASSERT_EQ(built_schema.value().columns.size(), 2u);
    const SysColumnRow* id_found = built_schema.value().FindColumn("id");
    ASSERT_NE(id_found, nullptr);
    EXPECT_EQ(id_found->type_val, 5u);
    EXPECT_TRUE(id_found->notnull);
}

TEST_F(CatalogTest, CreateTableRejectsBtreeClusteredType) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    Schema schema;
    auto oid = catalog_.CreateTable(kNamespacePublic, "t", schema, ClusteredType::kBtree);
    EXPECT_FALSE(oid.ok());
    EXPECT_EQ(oid.status().code(), StatusCode::kInvalidArgument);
}

TEST_F(CatalogTest, InitTableAccessReturnsSchemaAndDescPageId) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    Schema schema;
    SysColumnRow col{};
    col.pos = 0;
    SetName(col.name, "id");
    col.type_val = 5;
    col.len = 8;
    col.notnull = true;
    schema.columns.push_back(col);

    auto oid = catalog_.CreateTable(kNamespacePublic, "widgets", schema, ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok());

    auto access = catalog_.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok()) << access.status().message();
    EXPECT_EQ(access.value()->oid, oid.value());
    EXPECT_EQ(access.value()->clustered_type, ClusteredType::kHeap);
    EXPECT_EQ(access.value()->schema.columns.size(), 1u);
    EXPECT_EQ(access.value()->namespace_oid, kNamespacePublic);

    // Second open is the same cached entry, not a rebuilt copy.
    auto again = catalog_.InitTableAccess(oid.value());
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.value(), access.value());
}

// The pointer InitTableAccess hands out has to survive the statement that
// took it, which for INSERT means surviving its own id allocation: the
// sequence lives in sys.tables next to the row this entry came from, but
// TableAccess does not carry it.
TEST_F(CatalogTest, ATableAccessPointerSurvivesRowIdAllocation) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    Schema schema = MinimalPkSchema();
    auto oid = catalog_.CreateTable(kNamespacePublic, "held", schema, ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok());

    auto access = catalog_.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    const TableAccess* held = access.value();
    const PageId desc_page = held->desc_page_id;

    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(catalog_.AllocateRowId(oid.value()).ok());
    }

    EXPECT_EQ(held->desc_page_id, desc_page);
    EXPECT_EQ(held->schema.columns.size(), 1u);
    EXPECT_EQ(catalog_.InitTableAccess(oid.value()).value(), held);
}

// DDL drops the entry, so the next open rebuilds it from the pages and sees
// the new fact. The pointer is not reused across that boundary.
TEST_F(CatalogTest, DdlInvalidatesACachedTableAccess) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    Schema schema = MinimalPkSchema();
    auto oid = catalog_.CreateTable(kNamespacePublic, "relinked", schema, ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok());

    auto first = catalog_.InitTableAccess(oid.value());
    ASSERT_TRUE(first.ok());
    const PageId old_desc = first.value()->desc_page_id;

    ASSERT_TRUE(catalog_.UpdateRelationDescPage(oid.value(), old_desc + 1000).ok());

    auto second = catalog_.InitTableAccess(oid.value());
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value()->desc_page_id, old_desc + 1000);
}

// A cached schema must not leak into a relation that has none: the
// bootstrap catalog tables have no sys.columns rows, and DESCRIBE reports
// columns=0 for them rather than erroring.
TEST_F(CatalogTest, BuildSchemaFromColumnsStillReportsNotFoundForACatalogTable) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    for (int i = 0; i < 3; ++i) {
        auto schema = catalog_.BuildSchemaFromColumns(kSysTablesTable);
        EXPECT_FALSE(schema.ok());
        EXPECT_EQ(schema.status().code(), StatusCode::kNotFound);
    }
}

TEST_F(CatalogTest, UpdateRelationDescPagePreservesRowIdentity) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    Schema schema = MinimalPkSchema();
    auto oid = catalog_.CreateTable(kNamespacePublic, "movable", schema, ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok());

    auto before = catalog_.GetSysTableRow(oid.value());
    ASSERT_TRUE(before.ok());
    PageId old_desc = before.value().desc_page_id;

    Status s = catalog_.UpdateRelationDescPage(oid.value(), old_desc + 1000);
    ASSERT_TRUE(s.ok()) << s.message();

    auto after = catalog_.GetSysTableRow(oid.value());
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value().desc_page_id, old_desc + 1000);
    EXPECT_EQ(NameView(after.value().name), "movable");
}

TEST_F(CatalogTest, IndexRowsRoundTripAndFilterByTable) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    Oid table_a = catalog_.GenerateUserOid();
    Oid table_b = catalog_.GenerateUserOid();

    ASSERT_TRUE(catalog_.InsertIndexRow(catalog_.GenerateUserOid(), table_a, 0, 5,
                                        kIndexFlagUnique)
                    .ok());
    ASSERT_TRUE(catalog_.InsertIndexRow(catalog_.GenerateUserOid(), table_a, 1, 9, 0).ok());
    ASSERT_TRUE(catalog_.InsertIndexRow(catalog_.GenerateUserOid(), table_b, 0, 5,
                                        kIndexFlagUnique)
                    .ok());

    auto for_a = catalog_.FindIndexesForTable(table_a);
    ASSERT_TRUE(for_a.ok());
    EXPECT_EQ(for_a.value().size(), 2u);

    auto on_col0 = catalog_.FindIndexOnColumn(table_a, 0);
    ASSERT_TRUE(on_col0.ok());
    EXPECT_EQ(on_col0.value().col_type, 5u);
    EXPECT_EQ(on_col0.value().flags, kIndexFlagUnique);

    auto missing = catalog_.FindIndexOnColumn(table_a, 2);
    EXPECT_FALSE(missing.ok());
    EXPECT_EQ(missing.status().code(), StatusCode::kNotFound);
}

// The version counter parser.md I5 / PR20 stamp bound statements with. Its
// contract is monotonic-on-DDL, not "+1 per statement": one CreateTable
// bumps it once per catalog row it writes.
TEST_F(CatalogTest, DdlBumpsTheCatalogVersion) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    const std::uint64_t after_bootstrap = catalog_.catalog_version();
    EXPECT_GT(after_bootstrap, 0u);

    Schema schema = MinimalPkSchema();
    auto oid = catalog_.CreateTable(kNamespacePublic, "versioned", schema, ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    const std::uint64_t after_create = catalog_.catalog_version();
    EXPECT_GT(after_create, after_bootstrap);

    ASSERT_TRUE(catalog_.UpdateRelationDescPage(oid.value(), 9999).ok());
    EXPECT_GT(catalog_.catalog_version(), after_create);
}

// The rule that keeps a statement's cached TableAccess alive across its own
// insert: the id sequence is not a cached fact, so issuing one stales
// nothing. If this ever starts bumping, every bound statement re-parses on
// every insert.
TEST_F(CatalogTest, AllocateRowIdAndReadsDoNotBumpTheCatalogVersion) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    Schema schema = MinimalPkSchema();
    auto oid = catalog_.CreateTable(kNamespacePublic, "seq", schema, ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok());

    const std::uint64_t before = catalog_.catalog_version();

    for (int i = 0; i < 10; ++i) {
        auto id = catalog_.AllocateRowId(oid.value());
        ASSERT_TRUE(id.ok()) << id.status().message();
    }
    ASSERT_TRUE(catalog_.FindTableOidByName("seq").ok());
    ASSERT_TRUE(catalog_.GetSysTableRow(oid.value()).ok());
    ASSERT_TRUE(catalog_.InitTableAccess(oid.value()).ok());
    ASSERT_TRUE(catalog_.ResolveTypeByVal(kTypeValInt64).ok());
    ASSERT_TRUE(catalog_.ListTables().ok());

    EXPECT_EQ(catalog_.catalog_version(), before);
}

// sys.types is written only by Bootstrap(), so one snapshot serves the
// process and an unknown type name is refused without going back to the
// page - which is what takes the scan off DESCRIBE's per-column loop and
// off CREATE TABLE's error path.
TEST_F(CatalogTest, TypeResolutionIsServedFromOneSnapshot) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    auto first = catalog_.ResolveTypeByName("int64");
    ASSERT_TRUE(first.ok()) << first.status().message();
    const std::uint64_t after_first = catalog_.cache_stats().fills;

    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(catalog_.ResolveTypeByName("INT64").ok());  // case-insensitive, still
        ASSERT_TRUE(catalog_.ResolveTypeByVal(kTypeValVarchar).ok());
        auto unknown = catalog_.ResolveTypeByName("no_such_type");
        EXPECT_FALSE(unknown.ok());
        EXPECT_EQ(unknown.status().code(), StatusCode::kNotFound);
    }

    // No refills: the snapshot was loaded once and answered everything,
    // including the misses.
    EXPECT_EQ(catalog_.cache_stats().fills, after_first);
}

// DDL must not drop the type snapshot - it cannot stale it - but it must
// drop the table list, or a freshly created table would be invisible to
// SHOW TABLES.
TEST_F(CatalogTest, DdlRefreshesTheTableListAndKeepsTypes) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    ASSERT_TRUE(catalog_.ResolveTypeByName("int64").ok());

    auto before = catalog_.ListTables();
    ASSERT_TRUE(before.ok());
    const std::size_t count_before = before.value().size();

    Schema schema = MinimalPkSchema();
    ASSERT_TRUE(catalog_.CreateTable(kNamespacePublic, "listed", schema, ClusteredType::kHeap).ok());

    auto after = catalog_.ListTables();
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value().size(), count_before + 1);
    bool found = false;
    for (const SysObjectRow& row : after.value()) {
        if (NameView(row.name) == "listed") found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(CatalogTest, FindTableOidByNameFailsForUnknownName) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    auto oid = catalog_.FindTableOidByName("does_not_exist");
    EXPECT_FALSE(oid.ok());
    EXPECT_EQ(oid.status().code(), StatusCode::kNotFound);
}

// The cache's second payoff, and the one that is deterministic rather than
// timing-dependent: PageStore::Get() hands out a mutable span and therefore
// marks the page dirty (device_page_store.cpp), so before the cache every
// catalog *read* bought its page a rewrite at the next checkpoint. Cached
// reads touch no page, so they dirty none.
TEST(CatalogCacheWriteAmplificationTest, CachedReadsDoNotDirtyCatalogPages) {
    auto device = storage::MemoryPageDevice::Create(/*extent_pages=*/8, /*initial_pages=*/0);
    ASSERT_TRUE(device.ok()) << device.status().message();
    auto store = storage::DevicePageStore::Open(*device.value(), /*first_new_page_id=*/128);
    ASSERT_TRUE(store.ok()) << store.status().message();

    Catalog catalog(*store.value());
    ASSERT_TRUE(catalog.Bootstrap().ok());
    Schema schema = MinimalPkSchema();
    auto oid = catalog.CreateTable(kNamespacePublic, "hot", schema, ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    // Warm the three cached facts, then flush so every frame starts clean.
    ASSERT_TRUE(catalog.FindTableOidByName("hot").ok());
    ASSERT_TRUE(catalog.InitTableAccess(oid.value()).ok());
    ASSERT_TRUE(catalog.ResolveTypeByVal(kTypeValInt64).ok());
    ASSERT_TRUE(catalog.ListTables().ok());
    ASSERT_TRUE(store.value()->Flush().ok());
    ASSERT_TRUE(store.value()->DirtyPageIds().empty());

    // The catalog work a SELECT or an UPDATE does, fifty times over.
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(catalog.FindTableOidByName("hot").ok());
        ASSERT_TRUE(catalog.InitTableAccess(oid.value()).ok());
        ASSERT_TRUE(catalog.ResolveTypeByVal(kTypeValInt64).ok());
        ASSERT_TRUE(catalog.ListTables().ok());
    }

    EXPECT_TRUE(store.value()->DirtyPageIds().empty());

    // An INSERT still dirties exactly one catalog page - sys.tables, where
    // next_id lives. That one is not cached and must not be.
    ASSERT_TRUE(catalog.AllocateRowId(oid.value()).ok());
    EXPECT_EQ(store.value()->DirtyPageIds(), std::vector<PageId>{kCatalogPageTables});
}

// ---- sys.patterns (docs/waystone-concpets.md section 4) -------------------

class PatternCatalogTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(catalog_.Bootstrap().ok()); }

    // Writes a sys.patterns row straight onto the catalog page, bypassing
    // RegisterPattern(). This is how a stale row actually comes to exist -
    // an older build wrote it and then the fingerprint version was bumped -
    // and there is deliberately no API that can produce one, since
    // RegisterPattern() stamps the current version itself.
    void WriteRawPatternRow(std::uint64_t pattern_id, std::uint32_t version) {
        auto bytes = store_.Get(kCatalogPagePatterns);
        ASSERT_TRUE(bytes.ok());
        kds::heap::PageView page(bytes.value());

        SysPatternRow row{};
        row.oid = 900000 + pattern_id;
        row.pattern_id = pattern_id;
        row.fingerprint_version = version;
        row.waystone_root = kInvalidPageId;
        auto encoded = row.Encode();
        ASSERT_TRUE(page.InsertTuple(encoded, kBootstrapXid).ok());
    }

    storage::InMemoryPageStore store_{128};
    Catalog catalog_{store_};

    static constexpr std::uint32_t kVersion = parser::kFingerprintVersion;
    static constexpr std::uint32_t kForeignVersion = parser::kFingerprintVersion + 1;
};

TEST_F(PatternCatalogTest, BootstrapCreatesTheRelation) {
    auto oid = catalog_.FindTableOidByName("patterns");
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    EXPECT_EQ(oid.value(), kSysPatternsTable);

    auto row = catalog_.GetSysTableRow(kSysPatternsTable);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().desc_page_id, kCatalogPagePatterns);
}

TEST_F(PatternCatalogTest, RegisterThenFindRoundTrips) {
    auto registered = catalog_.RegisterPattern(0xABCDEF, kStmtClassUnclassified);
    ASSERT_TRUE(registered.ok()) << registered.status().message();
    EXPECT_EQ(registered.value()->pattern_id, 0xABCDEFu);
    EXPECT_EQ(registered.value()->fingerprint_version, kVersion);
    EXPECT_FALSE(registered.value()->has_waystone_directory());

    auto found = catalog_.FindPattern(0xABCDEF);
    ASSERT_TRUE(found.ok()) << found.status().message();
    EXPECT_EQ(found.value()->oid, registered.value()->oid);

    // Reference-stable, like InitTableAccess(): the caller may hold it.
    EXPECT_EQ(found.value(), registered.value());
}

TEST_F(PatternCatalogTest, AnUnknownPatternIsNotFound) {
    auto found = catalog_.FindPattern(12345);
    EXPECT_FALSE(found.ok());
    EXPECT_EQ(found.status().code(), StatusCode::kNotFound);
}

TEST_F(PatternCatalogTest, RegisteringTwiceIsRefused) {
    ASSERT_TRUE(catalog_.RegisterPattern(7, kStmtClassUnclassified).ok());
    auto again = catalog_.RegisterPattern(7, kStmtClassUnclassified);
    EXPECT_FALSE(again.ok());
    EXPECT_EQ(again.status().code(), StatusCode::kAlreadyExists);
}

TEST_F(PatternCatalogTest, PatternOidsComeFromThePersistentSequence) {
    auto a = catalog_.RegisterPattern(1, kStmtClassUnclassified);
    auto b = catalog_.RegisterPattern(2, kStmtClassUnclassified);
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());

    // Distinct and monotonic, and - unlike GenerateUserOid() - persisted,
    // so a restart does not reissue them onto rows that already exist.
    EXPECT_NE(a.value()->oid, b.value()->oid);
    EXPECT_LT(a.value()->oid, b.value()->oid);

    auto table_row = catalog_.GetSysTableRow(kSysPatternsTable);
    ASSERT_TRUE(table_row.ok());
    EXPECT_GT(table_row.value().next_id, b.value()->oid);
}

// ---- The version gate (the catalog half of P02) ---------------------------

TEST_F(PatternCatalogTest, AForeignVersionResolvesAsAbsentNotAsAnError) {
    WriteRawPatternRow(99, kForeignVersion);

    // The row is on the page, and the lookup still reports the pattern as
    // never seen: its pattern_id was computed under rules this build does
    // not implement, so it names a shape that is not the one it claims.
    // NotFound, not a failure - nothing about a stale row should fail a
    // statement.
    auto found = catalog_.FindPattern(99);
    EXPECT_FALSE(found.ok());
    EXPECT_EQ(found.status().code(), StatusCode::kNotFound);

    // And the same filter applies to the row accessor, which is where it
    // lives. A caller reaching past FindPattern() for heat must not see a
    // stale row either.
    EXPECT_EQ(catalog_.GetSysPatternRow(99).status().code(), StatusCode::kNotFound);
}

TEST_F(PatternCatalogTest, APatternStaleAtOneVersionCanBeReRegisteredAtTheCurrentOne) {
    WriteRawPatternRow(55, kForeignVersion);

    // Not AlreadyExists: as far as this build is concerned the pattern has
    // never been seen, and refusing to record it would leave the shape
    // permanently unlearnable after a version bump.
    auto fresh = catalog_.RegisterPattern(55, kStmtClassUnclassified);
    ASSERT_TRUE(fresh.ok()) << fresh.status().message();

    auto found = catalog_.FindPattern(55);
    ASSERT_TRUE(found.ok());
    EXPECT_EQ(found.value()->fingerprint_version, kVersion);
}

TEST_F(PatternCatalogTest, AStaleRowDoesNotShadowTheCurrentOne) {
    // Both rows on the page at once, stale first - the state a version bump
    // leaves behind, since nothing rewrites the old rows. A lookup that
    // took the first match by pattern_id would return the stale one, which
    // is the defect this ordering exists to catch.
    WriteRawPatternRow(77, kForeignVersion);
    ASSERT_TRUE(catalog_.RegisterPattern(77, kStmtClassUnclassified).ok());

    auto found = catalog_.FindPattern(77);
    ASSERT_TRUE(found.ok());
    EXPECT_EQ(found.value()->fingerprint_version, kVersion);

    auto row = catalog_.GetSysPatternRow(77);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().fingerprint_version, kVersion);
}

TEST_F(PatternCatalogTest, ARegisteredRowAlwaysCarriesTheCurrentVersion) {
    // The version is stamped by RegisterPattern(), not supplied - so there
    // is no call that can write a row this build will not resolve.
    auto registered = catalog_.RegisterPattern(4, kStmtClassUnclassified);
    ASSERT_TRUE(registered.ok());
    EXPECT_TRUE(parser::IsCurrentFingerprintVersion(registered.value()->fingerprint_version));
}

// ---- The root/depth pair --------------------------------------------------

TEST_F(PatternCatalogTest, RootAndDepthAreValidatedAsAPair) {
    ASSERT_TRUE(catalog_.RegisterPattern(10, kStmtClassUnclassified).ok());

    // A root with no depth is unwalkable; a depth with no root has nothing
    // to walk. Both are refused before the page is touched.
    EXPECT_EQ(catalog_.SetPatternWaystoneRoot(10, 4096, 0).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(catalog_.SetPatternWaystoneRoot(10, kInvalidPageId, 1).code(),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(catalog_.SetPatternWaystoneRoot(10, 4096, kMaxPatternDirDepth + 1).code(),
              StatusCode::kInvalidArgument);

    // The two coherent shapes: a directory, and none.
    EXPECT_TRUE(catalog_.SetPatternWaystoneRoot(10, 4096, 1).ok());
    EXPECT_TRUE(catalog_.SetPatternWaystoneRoot(10, kInvalidPageId, 0).ok());
}

TEST_F(PatternCatalogTest, SettingTheRootUpdatesTheCachedEntryInPlace) {
    auto registered = catalog_.RegisterPattern(11, kStmtClassUnclassified);
    ASSERT_TRUE(registered.ok());
    const PatternAccess* held = registered.value();
    ASSERT_FALSE(held->has_waystone_directory());

    ASSERT_TRUE(catalog_.SetPatternWaystoneRoot(11, 4096, 2).ok());

    // The pointer the caller was holding is still valid *and* now reports
    // the new directory - which is the whole reason this is an in-place
    // update rather than an invalidation.
    EXPECT_TRUE(held->has_waystone_directory());
    EXPECT_EQ(held->waystone_root, 4096u);
    EXPECT_EQ(held->dir_depth, 2);

    // And it survives a cache drop, because the page was written too.
    auto row = catalog_.GetSysPatternRow(11);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().waystone_root, 4096u);
    EXPECT_EQ(row.value().dir_depth, 2);
}

TEST_F(PatternCatalogTest, SettingTheRootOfAnUnknownPatternIsNotFound) {
    EXPECT_EQ(catalog_.SetPatternWaystoneRoot(404, 4096, 1).code(), StatusCode::kNotFound);
}

// ---- What registration must not disturb -----------------------------------

TEST_F(PatternCatalogTest, RegisteringAPatternDoesNotInvalidateTableAccess) {
    auto oid = catalog_.CreateTable(kNamespacePublic, "t", MinimalPkSchema(),
                                     ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok());
    auto access = catalog_.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    const TableAccess* held = access.value();
    const std::uint64_t version_before = catalog_.catalog_version();

    ASSERT_TRUE(catalog_.RegisterPattern(0xF00D, kStmtClassUnclassified).ok());

    // The hazard this avoids is the one the deleted per-relation Waystone
    // walked into: a catalog write mid-statement that cleared the cache out
    // from under the `const TableAccess*` the statement was holding.
    // Nothing cached can go stale from a pattern appearing, so nothing is
    // dropped and no version moves.
    EXPECT_EQ(catalog_.catalog_version(), version_before);
    auto again = catalog_.InitTableAccess(oid.value());
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.value(), held);
}

TEST_F(PatternCatalogTest, HeatIsReadFromThePageNotTheCache) {
    ASSERT_TRUE(catalog_.RegisterPattern(21, kStmtClassUnclassified).ok());

    // PatternAccess deliberately has no use_count/last_seen to read: they
    // change on every execution, which is not DDL, so they are not
    // cacheable facts. The row is where they live.
    auto row = catalog_.GetSysPatternRow(21);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().use_count, 0u);
    EXPECT_EQ(row.value().last_seen, 0u);
}

// ---- Relation ownership (docs/workplan-crosscore.md M1) ---------------

class OwnerCoreTest : public ::testing::Test {
protected:
    Schema OneColumnSchema() {
        Schema schema;
        SysColumnRow id{};
        id.pos = 0;
        SetName(id.name, "id");
        id.type_val = kTypeValInt64;
        id.len = 8;
        id.notnull = true;
        schema.columns.push_back(id);
        return schema;
    }

    storage::InMemoryPageStore store_{128};
};

TEST_F(OwnerCoreTest, ASingleCoreInstancePutsEveryRelationOnCoreZero) {
    Catalog catalog(store_, storage::kDefaultInlineCellWidth, /*core_count=*/1);
    ASSERT_TRUE(catalog.Bootstrap().ok());

    auto oid = catalog.CreateTable(kNamespacePublic, "t", OneColumnSchema(),
                                    ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    auto row = catalog.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().owner_core, 0u);
}

TEST_F(OwnerCoreTest, CreateTableRecordsAnOwnerAndTableAccessCarriesIt) {
    // The path that matters: the planner reads TableAccess, not the row, so
    // an owner recorded on disk and lost on the way into the cache would be
    // invisible until something compared it against execution.
    Catalog catalog(store_, storage::kDefaultInlineCellWidth, /*core_count=*/4);
    ASSERT_TRUE(catalog.Bootstrap().ok());

    auto oid = catalog.CreateTable(kNamespacePublic, "t", OneColumnSchema(),
                                    ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    auto row = catalog.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    // The system core, even at cores=4: DDL allocates the relation's pages
    // from the system core's free map, and a relation must be owned by the
    // core that can fault its pages (core_placement.hpp).
    EXPECT_EQ(row.value().owner_core, kSystemCore);

    auto access = catalog.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok()) << access.status().message();
    EXPECT_EQ(access.value()->owner_core, row.value().owner_core);
}

TEST_F(OwnerCoreTest, EveryRelationIsReachableFromTheCoreThatCreatedIt) {
    // The property the round-robin broke, and which nothing checked until
    // the affinity guard existed: placement and execution have to agree, or
    // the relation cannot be read by anyone.
    Catalog catalog(store_, storage::kDefaultInlineCellWidth, /*core_count=*/3);
    ASSERT_TRUE(catalog.Bootstrap().ok());

    for (int i = 0; i < 6; ++i) {
        auto oid = catalog.CreateTable(kNamespacePublic, "t" + std::to_string(i),
                                        OneColumnSchema(), ClusteredType::kHeap);
        ASSERT_TRUE(oid.ok()) << oid.status().message();
        auto row = catalog.GetSysTableRow(oid.value());
        ASSERT_TRUE(row.ok());
        EXPECT_EQ(row.value().owner_core, kSystemCore)
            << "relation t" << i << " was placed where nothing can reach it";
    }
}

TEST_F(OwnerCoreTest, OwnershipSurvivesAReopen) {
    // It is a catalog fact, so it has to come back off the page - not be
    // re-derived, which workplan guideline 4 forbids outright.
    std::uint32_t assigned = 0;
    Oid oid = 0;
    {
        Catalog catalog(store_, storage::kDefaultInlineCellWidth, /*core_count=*/4);
        ASSERT_TRUE(catalog.Bootstrap().ok());
        auto created = catalog.CreateTable(kNamespacePublic, "t", OneColumnSchema(),
                                            ClusteredType::kHeap);
        ASSERT_TRUE(created.ok());
        oid = created.value();
        auto row = catalog.GetSysTableRow(oid);
        ASSERT_TRUE(row.ok());
        assigned = row.value().owner_core;
    }

    Catalog reopened(store_, storage::kDefaultInlineCellWidth, /*core_count=*/4);
    auto row = reopened.GetSysTableRow(oid);
    ASSERT_TRUE(row.ok()) << row.status().message();
    EXPECT_EQ(row.value().owner_core, assigned);
}

// ---- The catalog relations chain (docs/keystoneid-k0-findings.md) --------
//
// `sys.columns` used to be one fixed 8 KB page that did not chain, so the
// whole instance held ~68 column rows and the CREATE TABLE that needed the
// 69th failed with "heap page has no room for this tuple". These tests
// cross that boundary on purpose: the interesting row is the first one on
// the *second* page, because everything about reading it - the scan, the
// lookups built on the scan, the mutators that find a row and write it
// back - used to stop at the end of page one.

namespace {

// Wide enough that a handful of relations exhaust one page, and named so
// the arithmetic is visible: a page holds about 68 column rows.
Schema WideSchema(int columns) {
    Schema schema;
    for (int i = 0; i < columns; ++i) {
        SysColumnRow col{};
        col.pos = static_cast<std::uint32_t>(i);
        SetName(col.name, "c" + std::to_string(i));
        col.type_val = kTypeValInt64;
        col.len = 8;
        col.notnull = true;
        schema.columns.push_back(col);
    }
    return schema;
}

}  // namespace

TEST(CatalogChain, ColumnsBeyondOnePageAreStoredAndReadBack) {
    storage::InMemoryPageStore store{server::kFirstUserPageId};
    Catalog catalog(store, storage::kDefaultInlineCellWidth);
    ASSERT_TRUE(catalog.Bootstrap().ok());

    // 12 relations x 20 columns = 240 column rows, comfortably past the
    // ~68 one page holds.
    std::vector<Oid> oids;
    for (int i = 0; i < 12; ++i) {
        auto oid = catalog.CreateTable(kNamespacePublic, "wide" + std::to_string(i),
                                       WideSchema(20), ClusteredType::kHeap);
        ASSERT_TRUE(oid.ok()) << "relation " << i << ": " << oid.status().message();
        oids.push_back(oid.value());
    }

    // Every relation's schema still reads back whole - including the ones
    // whose rows landed on a later page.
    for (std::size_t i = 0; i < oids.size(); ++i) {
        auto schema = catalog.BuildSchemaFromColumns(oids[i]);
        ASSERT_TRUE(schema.ok()) << "relation " << i << ": " << schema.status().message();
        EXPECT_EQ(schema.value().columns.size(), 20u) << "relation " << i;
        EXPECT_EQ(NameView(schema.value().columns[19].name), "c19") << "relation " << i;
    }
}

TEST(CatalogChain, TheChainIsALinkedListOfPagesInTheReservedRange) {
    storage::InMemoryPageStore store{server::kFirstUserPageId};
    Catalog catalog(store, storage::kDefaultInlineCellWidth);
    ASSERT_TRUE(catalog.Bootstrap().ok());

    for (int i = 0; i < 12; ++i) {
        ASSERT_TRUE(catalog.CreateTable(kNamespacePublic, "wide" + std::to_string(i),
                                        WideSchema(20), ClusteredType::kHeap)
                        .ok());
    }

    // Follow sys.columns' links: more than one page, and every page after
    // the root inside the reserved range - which is what keeps a peer able
    // to fault it (MayFault admits only low pages read-only).
    PageId at = kCatalogPageColumns;
    int pages = 0;
    while (at != kInvalidPageId) {
        ++pages;
        if (at != kCatalogPageColumns) {
            EXPECT_GE(at, kCatalogOverflowFirst) << "page " << at << " is below the range";
            EXPECT_LT(at, kCatalogOverflowLimit) << "page " << at << " is a user page";
        }
        auto bytes = store.GetForRead(at);
        ASSERT_TRUE(bytes.ok());
        at = heap::PageView(bytes.value()).next_page_id();
        ASSERT_LE(pages, 64) << "the chain does not terminate";
    }
    EXPECT_GT(pages, 1) << "240 column rows should not fit on one page";
}

// The mutators walk too. `AllocateRowId` finds a sys.tables row and writes
// it back; a row on a later page used to be invisible to it, which would
// have made the relation un-insertable rather than merely un-listable.
TEST(CatalogChain, ASequenceOnALaterPageStillIssuesIds) {
    storage::InMemoryPageStore store{server::kFirstUserPageId};
    Catalog catalog(store, storage::kDefaultInlineCellWidth);
    ASSERT_TRUE(catalog.Bootstrap().ok());

    // Enough relations that sys.tables itself needs a second page: its rows
    // are wider than a column row, so this takes fewer of them.
    std::vector<Oid> oids;
    for (int i = 0; i < 60; ++i) {
        auto oid = catalog.CreateTable(kNamespacePublic, "t" + std::to_string(i),
                                       WideSchema(1), ClusteredType::kHeap);
        ASSERT_TRUE(oid.ok()) << "relation " << i << ": " << oid.status().message();
        oids.push_back(oid.value());
    }

    // The last relation created is the furthest into the chain.
    const Oid last = oids.back();
    auto first_id = catalog.AllocateRowId(last);
    ASSERT_TRUE(first_id.ok()) << first_id.status().message();
    auto second_id = catalog.AllocateRowId(last);
    ASSERT_TRUE(second_id.ok()) << second_id.status().message();
    EXPECT_EQ(second_id.value(), first_id.value() + 1);

    // And the bump persisted, which is the half that needs the *write* to
    // have found the right page.
    auto row = catalog.GetSysTableRow(last);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().next_id, second_id.value() + 1);
}

TEST(CatalogChain, NameLookupFindsARelationOnALaterPage) {
    storage::InMemoryPageStore store{server::kFirstUserPageId};
    Catalog catalog(store, storage::kDefaultInlineCellWidth);
    ASSERT_TRUE(catalog.Bootstrap().ok());

    for (int i = 0; i < 60; ++i) {
        ASSERT_TRUE(catalog.CreateTable(kNamespacePublic, "t" + std::to_string(i),
                                        WideSchema(1), ClusteredType::kHeap)
                        .ok());
    }
    auto oid = catalog.FindTableOidByName("t59");
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    // The 60 user relations plus the bootstrap ones - the point is that the
    // listing walks past the end of the root page, not the exact total.
    auto listed = catalog.ListTables();
    ASSERT_TRUE(listed.ok());
    EXPECT_GE(listed.value().size(), 60u);
    bool found_last = false;
    for (const SysObjectRow& obj : listed.value()) {
        if (NameView(obj.name) == "t59") found_last = true;
    }
    EXPECT_TRUE(found_last) << "the relation furthest into the chain is missing from the list";
}

}  // namespace
}  // namespace kds::catalog
