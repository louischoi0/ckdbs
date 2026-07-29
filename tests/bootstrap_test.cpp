#include "kds/bootstrap/bootstrap.hpp"

#include <gtest/gtest.h>

#include "kds/storage/in_memory_page_store.hpp"

namespace kds::bootstrap {
namespace {

TEST(BootstrapTest, FreshDatabaseCreatesSuperBlockAndCatalog) {
    storage::InMemoryPageStore store(server::kFirstUserPageId);

    auto result = BootstrapDatabase(store, 1000);
    ASSERT_TRUE(result.ok()) << result.status().message();

    EXPECT_EQ(result.value().superblock.version(), server::kSuperBlockVersion);
    EXPECT_EQ(result.value().superblock.create_time(), 1000u);

    auto tables_oid = result.value().catalog.FindTableOidByName("tables");
    ASSERT_TRUE(tables_oid.ok());
    EXPECT_EQ(tables_oid.value(), catalog::kSysTablesTable);
}

TEST(BootstrapTest, SecondBootstrapLoadsExistingSuperBlockWithoutReRunningCatalog) {
    storage::InMemoryPageStore store(server::kFirstUserPageId);

    auto first = BootstrapDatabase(store, 1000);
    ASSERT_TRUE(first.ok());

    // Create a user table on the first "boot" so we can tell the second
    // bootstrap call did NOT wipe the catalog (Catalog::Bootstrap() would
    // fail outright trying to re-CreateAt the fixed pages anyway, but this
    // also proves data survives across the boundary).
    // A relation's first column is its mandatory Keystone primary key
    // (heap-and-tuple.md section 4), so the table needs one to exist.
    catalog::Schema schema;
    catalog::SysColumnRow col{};
    col.pos = 0;
    catalog::SetName(col.name, "id");
    col.type_val = catalog::kTypeValInt64;
    col.len = 8;
    col.notnull = true;
    schema.columns.push_back(col);

    auto table_oid =
        first.value().catalog.CreateTable(catalog::kNamespacePublic, "widgets", schema,
                                           catalog::ClusteredType::kHeap);
    ASSERT_TRUE(table_oid.ok());

    auto second = BootstrapDatabase(store, 2000);
    ASSERT_TRUE(second.ok()) << second.status().message();

    // Loaded the same superblock (create_time unchanged), but mount time
    // advanced to the second call's clock reading.
    EXPECT_EQ(second.value().superblock.create_time(), 1000u);
    EXPECT_EQ(second.value().superblock.last_mount_time(), 2000u);

    // The table created before the second bootstrap call is still there.
    auto found = second.value().catalog.FindTableOidByName("widgets");
    ASSERT_TRUE(found.ok());
    EXPECT_EQ(found.value(), table_oid.value());
}

TEST(BootstrapTest, FailsIfSuperBlockPageHoldsUndecodableData) {
    storage::InMemoryPageStore store(server::kFirstUserPageId);

    // Occupy the superblock's fixed page id with something that isn't a
    // valid SuperBlock image (all-zero, magic mismatch), simulating disk
    // corruption or an unrelated page landing there.
    ASSERT_TRUE(store.CreateAt(server::kSuperBlockPageId).ok());

    auto result = BootstrapDatabase(store, 1000);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), StatusCode::kCorruption);
}

}  // namespace
}  // namespace kds::bootstrap
