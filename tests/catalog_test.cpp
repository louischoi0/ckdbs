#include "kds/catalog/catalog.hpp"

#include <gtest/gtest.h>

#include "kds/catalog/well_known.hpp"
#include "kds/storage/in_memory_page_store.hpp"

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

    auto access = catalog_.InitTableAccess(kNamespacePublic, oid.value());
    ASSERT_TRUE(access.ok()) << access.status().message();
    EXPECT_EQ(access.value().oid, oid.value());
    EXPECT_EQ(access.value().clustered_type, ClusteredType::kHeap);
    EXPECT_EQ(access.value().schema.columns.size(), 1u);
}

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

TEST_F(CatalogTest, FindTableOidByNameFailsForUnknownName) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    auto oid = catalog_.FindTableOidByName("does_not_exist");
    EXPECT_FALSE(oid.ok());
    EXPECT_EQ(oid.status().code(), StatusCode::kNotFound);
}

}  // namespace
}  // namespace kds::catalog
