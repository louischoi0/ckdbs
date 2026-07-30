#include "kds/catalog/catalog.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "kds/catalog/well_known.hpp"
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

}  // namespace
}  // namespace kds::catalog
