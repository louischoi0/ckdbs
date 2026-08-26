#include "kds/bootstrap/bootstrap.hpp"

#include <gtest/gtest.h>

#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/tagged_cell.hpp"

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


// ---- The pinned inline_cell_width (rule-fixed-length-tuple.md section 4) --
//
// The width decides on-disk tuple layout, so a database opened under a
// different one would not fail - it would decode every row at the wrong
// offsets. That is why the mismatch is caught at mount and not left to
// produce wrong answers later.

TEST(BootstrapTest, AFreshDatabasePinsTheConfiguredCellWidth) {
    storage::InMemoryPageStore store(server::kFirstUserPageId);

    auto result = BootstrapDatabase(store, 1000, /*inline_cell_width=*/128);
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(result.value().superblock.inline_cell_width(), 128u);

    // And the catalog builds its row layouts for that width, not the
    // default - the pin is worthless if nothing downstream honours it.
    catalog::Schema schema;
    catalog::SysColumnRow id{};
    id.pos = 0;
    catalog::SetName(id.name, "id");
    id.type_val = catalog::kTypeValInt64;
    catalog::SysColumnRow s{};
    s.pos = 1;
    catalog::SetName(s.name, "s");
    s.type_val = catalog::kTypeValVarchar;
    schema.columns = {id, s};

    auto oid = result.value().catalog.CreateTable(catalog::kNamespacePublic, "t", schema,
                                                  catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto access = result.value().catalog.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok()) << access.status().message();
    EXPECT_EQ(access.value()->layout.inline_cell_width, 128u);
    EXPECT_EQ(access.value()->layout.row_size, 8u + 128u);
}

TEST(BootstrapTest, RemountingWithADifferentCellWidthIsRefusedNamingBoth) {
    storage::InMemoryPageStore store(server::kFirstUserPageId);
    ASSERT_TRUE(BootstrapDatabase(store, 1000, /*inline_cell_width=*/64).ok());

    auto second = BootstrapDatabase(store, 2000, /*inline_cell_width=*/128);
    ASSERT_FALSE(second.ok());
    EXPECT_EQ(second.status().code(), StatusCode::kInvalidArgument);

    // Both numbers, because the operator's next question is always "which
    // one is the database's?".
    EXPECT_NE(second.status().message().find("64"), std::string::npos)
        << second.status().message();
    EXPECT_NE(second.status().message().find("128"), std::string::npos)
        << second.status().message();
}

TEST(BootstrapTest, RemountingWithTheSameCellWidthIsFine) {
    storage::InMemoryPageStore store(server::kFirstUserPageId);
    ASSERT_TRUE(BootstrapDatabase(store, 1000, /*inline_cell_width=*/128).ok());

    auto second = BootstrapDatabase(store, 2000, /*inline_cell_width=*/128);
    ASSERT_TRUE(second.ok()) << second.status().message();
    EXPECT_EQ(second.value().superblock.inline_cell_width(), 128u);
}

TEST(BootstrapTest, AnIllegalCellWidthIsRefusedBeforeAnythingIsCreated) {
    storage::InMemoryPageStore store(server::kFirstUserPageId);

    auto result = BootstrapDatabase(store, 1000, /*inline_cell_width=*/3);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), StatusCode::kInvalidArgument);

    // Nothing was written: a bad setting must not leave a half-made
    // database behind for the next run to inherit.
    EXPECT_FALSE(store.Get(server::kSuperBlockPageId).ok());
}

// ---- The pinned core count (docs/inflight/in-progress/workplan-crosscore.md M6) -------------
//
// Same shape as the width above and for a reason of the same weight: WAL
// streams are per core, so the count decides how many streams the database
// has. Mounting under a different one would leave streams with nothing to
// replay them, and recovery under a changed count is [OPEN] (wal.md §3) -
// the refusal is what stops this path from settling it by accident.

TEST(BootstrapTest, AFreshDatabasePinsTheConfiguredCoreCount) {
    storage::InMemoryPageStore store(server::kFirstUserPageId);

    auto result = BootstrapDatabase(store, 1000, storage::kDefaultInlineCellWidth, /*cores=*/4);
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(result.value().superblock.core_count(), 4u);
}

TEST(BootstrapTest, RemountingWithADifferentCoreCountIsRefusedNamingBoth) {
    storage::InMemoryPageStore store(server::kFirstUserPageId);
    ASSERT_TRUE(BootstrapDatabase(store, 1000, storage::kDefaultInlineCellWidth, /*cores=*/1).ok());

    auto second = BootstrapDatabase(store, 2000, storage::kDefaultInlineCellWidth, /*cores=*/2);
    ASSERT_FALSE(second.ok());
    EXPECT_EQ(second.status().code(), StatusCode::kInvalidArgument);

    EXPECT_NE(second.status().message().find("cores 2"), std::string::npos)
        << second.status().message();
    EXPECT_NE(second.status().message().find("the 1"), std::string::npos)
        << second.status().message();
}

TEST(BootstrapTest, RemountingWithTheSameCoreCountIsFine) {
    storage::InMemoryPageStore store(server::kFirstUserPageId);
    ASSERT_TRUE(BootstrapDatabase(store, 1000, storage::kDefaultInlineCellWidth, /*cores=*/4).ok());

    auto second = BootstrapDatabase(store, 2000, storage::kDefaultInlineCellWidth, /*cores=*/4);
    ASSERT_TRUE(second.ok()) << second.status().message();
    EXPECT_EQ(second.value().superblock.core_count(), 4u);
}

TEST(BootstrapTest, AnIllegalCoreCountIsRefusedBeforeAnythingIsCreated) {
    storage::InMemoryPageStore store(server::kFirstUserPageId);

    auto result = BootstrapDatabase(store, 1000, storage::kDefaultInlineCellWidth, /*cores=*/0);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), StatusCode::kInvalidArgument);
    EXPECT_FALSE(store.Get(server::kSuperBlockPageId).ok());
}

TEST(BootstrapTest, SystemRelationsAreOwnedByTheSystemCore) {
    // M5: core 0 owns the superblock, the free map, file growth and the
    // catalog pages, so every relation that lives on those pages is its.
    storage::InMemoryPageStore store(server::kFirstUserPageId);
    auto result = BootstrapDatabase(store, 1000, storage::kDefaultInlineCellWidth, /*cores=*/4);
    ASSERT_TRUE(result.ok()) << result.status().message();

    for (const catalog::Oid oid : {catalog::kSysTablesTable, catalog::kSysColumnsTable,
                                    catalog::kSysObjectsTable, catalog::kSysTypesTable}) {
        auto row = result.value().catalog.GetSysTableRow(oid);
        ASSERT_TRUE(row.ok()) << row.status().message();
        EXPECT_EQ(row.value().owner_core, catalog::kSystemCore) << "oid " << oid;
    }
}

}  // namespace
}  // namespace kds::bootstrap
