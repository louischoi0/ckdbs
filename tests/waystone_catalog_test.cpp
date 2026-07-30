#include <cstdint>

#include <gtest/gtest.h>

#include "kds/catalog/catalog.hpp"
#include "kds/catalog/rows.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/stats/waystone_dir.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// The catalog side of Waystone (waystone-concpets.md section 7, spec test
// 12-6's first half; workplan T04/T12). Three things have to hold before
// the hooks and the probe can be built on top:
//
//   - a relation that never asked for Waystone is untouched by it;
//   - the three fields survive a round trip through a catalog page, since
//     they are what a directory walk is reconstructed from after a
//     restart;
//   - the triple can never be written inconsistently, because every
//     reader downstream is entitled to trust it without re-checking.

namespace kds::catalog {
namespace {

class WaystoneCatalogTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Bootstrap().ok());
        auto oid = catalog_.CreateTable(kNamespacePublic, "accounts", PkSchema(),
                                        ClusteredType::kHeap);
        ASSERT_TRUE(oid.ok()) << oid.status().message();
        oid_ = oid.value();
    }

    static Schema PkSchema() {
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

    storage::InMemoryPageStore store_{128};
    Catalog catalog_{store_};
    Oid oid_ = 0;
};

// ---- Defaults -----------------------------------------------------------

TEST_F(WaystoneCatalogTest, ANewRelationHasNoWaystone) {
    auto row = catalog_.GetSysTableRow(oid_);
    ASSERT_TRUE(row.ok()) << row.status().message();
    EXPECT_EQ(row.value().waystone_state, WaystoneState::kDisabled);
    EXPECT_EQ(row.value().waystone_dir_root, kInvalidPageId);
    EXPECT_EQ(row.value().waystone_dir_depth, 0);
    EXPECT_FALSE(WaystoneActive(row.value().waystone_state));
}

TEST_F(WaystoneCatalogTest, TheDefaultsReachTableAccessToo) {
    auto access = catalog_.InitTableAccess(oid_);
    ASSERT_TRUE(access.ok()) << access.status().message();
    EXPECT_EQ(access.value()->waystone_state, WaystoneState::kDisabled);
    EXPECT_EQ(access.value()->waystone_dir_root, kInvalidPageId);
    EXPECT_EQ(access.value()->waystone_dir_depth, 0);
}

TEST_F(WaystoneCatalogTest, NothingEnablesWaystoneImplicitly) {
    // A second relation through the same CreateTable path is just as
    // untouched: enabling is always a separate, explicit DDL step.
    auto other = catalog_.CreateTable(kNamespacePublic, "orders", PkSchema(),
                                      ClusteredType::kHeap);
    ASSERT_TRUE(other.ok()) << other.status().message();

    auto row = catalog_.GetSysTableRow(other.value());
    ASSERT_TRUE(row.ok()) << row.status().message();
    EXPECT_EQ(row.value().waystone_state, WaystoneState::kDisabled);
}

// ---- Round trip ---------------------------------------------------------

TEST_F(WaystoneCatalogTest, TheTripleSurvivesTheCatalogPage) {
    auto root = stats::CreateDirPage(store_);
    ASSERT_TRUE(root.ok());

    ASSERT_TRUE(catalog_
                    .SetWaystoneDirectory(oid_, WaystoneState::kCovered, root.value(),
                                          /*dir_depth=*/1)
                    .ok());

    auto row = catalog_.GetSysTableRow(oid_);
    ASSERT_TRUE(row.ok()) << row.status().message();
    EXPECT_EQ(row.value().waystone_state, WaystoneState::kCovered);
    EXPECT_EQ(row.value().waystone_dir_root, root.value());
    EXPECT_EQ(row.value().waystone_dir_depth, 1);

    // And every other field of the row is where it was: the encoding grew
    // by three fields and must not have shifted the ones before them.
    EXPECT_EQ(row.value().oid, oid_);
    EXPECT_EQ(NameView(row.value().name), "accounts");
    EXPECT_EQ(row.value().clustered_type, ClusteredType::kHeap);
    EXPECT_EQ(row.value().next_id, kFirstRowId);
}

TEST_F(WaystoneCatalogTest, ASecondCatalogOverTheSameStoreSeesIt) {
    // The fields are persisted, not cached state: a directory has to be
    // walkable after a restart, and this is the closest thing the tests
    // have to one.
    auto root = stats::CreateDirPage(store_);
    ASSERT_TRUE(root.ok());
    ASSERT_TRUE(
        catalog_.SetWaystoneDirectory(oid_, WaystoneState::kCovered, root.value(), 2).ok());

    Catalog reopened(store_);
    auto row = reopened.GetSysTableRow(oid_);
    ASSERT_TRUE(row.ok()) << row.status().message();
    EXPECT_EQ(row.value().waystone_state, WaystoneState::kCovered);
    EXPECT_EQ(row.value().waystone_dir_root, root.value());
    EXPECT_EQ(row.value().waystone_dir_depth, 2);
}

TEST_F(WaystoneCatalogTest, DisablingClearsTheDirectoryFields) {
    auto root = stats::CreateDirPage(store_);
    ASSERT_TRUE(root.ok());
    ASSERT_TRUE(
        catalog_.SetWaystoneDirectory(oid_, WaystoneState::kCovered, root.value(), 1).ok());

    ASSERT_TRUE(
        catalog_.SetWaystoneDirectory(oid_, WaystoneState::kDisabled, kInvalidPageId, 0).ok());

    auto row = catalog_.GetSysTableRow(oid_);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().waystone_state, WaystoneState::kDisabled);
    EXPECT_EQ(row.value().waystone_dir_root, kInvalidPageId);
}

TEST_F(WaystoneCatalogTest, BackfillingIsActiveButNotCovered) {
    auto root = stats::CreateDirPage(store_);
    ASSERT_TRUE(root.ok());
    ASSERT_TRUE(
        catalog_.SetWaystoneDirectory(oid_, WaystoneState::kBackfilling, root.value(), 1).ok());

    auto row = catalog_.GetSysTableRow(oid_);
    ASSERT_TRUE(row.ok());
    // The distinction the middle state exists for: the insert path
    // maintains entries, but a probe miss does not yet mean "no such row".
    EXPECT_TRUE(WaystoneActive(row.value().waystone_state));
    EXPECT_NE(row.value().waystone_state, WaystoneState::kCovered);
}

// ---- The triple is written as one fact ----------------------------------

TEST_F(WaystoneCatalogTest, AnEnabledWaystoneWithoutARootIsRefused) {
    EXPECT_EQ(
        catalog_.SetWaystoneDirectory(oid_, WaystoneState::kCovered, kInvalidPageId, 1).code(),
        StatusCode::kInvalidArgument);
}

TEST_F(WaystoneCatalogTest, ADepthOutsideTheWalkableRangeIsRefused) {
    auto root = stats::CreateDirPage(store_);
    ASSERT_TRUE(root.ok());

    // Depth 0 with a root would be unwalkable; past kMaxDirDepth would
    // mask pk digits away and alias entries.
    EXPECT_EQ(catalog_.SetWaystoneDirectory(oid_, WaystoneState::kCovered, root.value(), 0).code(),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(catalog_
                  .SetWaystoneDirectory(oid_, WaystoneState::kCovered, root.value(),
                                        stats::kMaxDirDepth + 1)
                  .code(),
              StatusCode::kInvalidArgument);
}

TEST_F(WaystoneCatalogTest, ADisabledWaystoneCarryingADirectoryIsRefused) {
    auto root = stats::CreateDirPage(store_);
    ASSERT_TRUE(root.ok());
    EXPECT_EQ(
        catalog_.SetWaystoneDirectory(oid_, WaystoneState::kDisabled, root.value(), 1).code(),
        StatusCode::kInvalidArgument);
}

TEST_F(WaystoneCatalogTest, ARefusedChangeLeavesTheRowAlone) {
    auto root = stats::CreateDirPage(store_);
    ASSERT_TRUE(root.ok());
    ASSERT_TRUE(
        catalog_.SetWaystoneDirectory(oid_, WaystoneState::kCovered, root.value(), 1).ok());

    EXPECT_FALSE(catalog_.SetWaystoneDirectory(oid_, WaystoneState::kCovered, kInvalidPageId, 1).ok());

    auto row = catalog_.GetSysTableRow(oid_);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().waystone_dir_root, root.value()) << "a refused write must change nothing";
    EXPECT_EQ(row.value().waystone_dir_depth, 1);
}

TEST_F(WaystoneCatalogTest, AnUnknownOidIsNotFound) {
    auto root = stats::CreateDirPage(store_);
    ASSERT_TRUE(root.ok());
    EXPECT_EQ(catalog_.SetWaystoneDirectory(999999, WaystoneState::kCovered, root.value(), 1).code(),
              StatusCode::kNotFound);
}

// ---- Cache coherency ----------------------------------------------------

TEST_F(WaystoneCatalogTest, ChangingTheDirectoryInvalidatesACachedTableAccess) {
    auto before = catalog_.InitTableAccess(oid_);
    ASSERT_TRUE(before.ok());
    ASSERT_EQ(before.value()->waystone_state, WaystoneState::kDisabled);
    const std::uint64_t version_before = catalog_.catalog_version();

    auto root = stats::CreateDirPage(store_);
    ASSERT_TRUE(root.ok());
    ASSERT_TRUE(
        catalog_.SetWaystoneDirectory(oid_, WaystoneState::kCovered, root.value(), 1).ok());

    EXPECT_GT(catalog_.catalog_version(), version_before)
        << "TableAccess caches all three fields, so the change must bump";

    // Re-acquired, per the contract in catalog.hpp - the earlier pointer
    // is dangling now, which is exactly why the header says so.
    auto after = catalog_.InitTableAccess(oid_);
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value()->waystone_state, WaystoneState::kCovered);
    EXPECT_EQ(after.value()->waystone_dir_root, root.value());
    EXPECT_EQ(after.value()->waystone_dir_depth, 1);
}

TEST_F(WaystoneCatalogTest, AllocatingARowIdDoesNotDisturbTheWaystoneFields) {
    // next_id lives in the same row and is bumped per insert without a
    // version bump (catalog_cache.hpp). It must not carry the Waystone
    // fields away with it.
    auto root = stats::CreateDirPage(store_);
    ASSERT_TRUE(root.ok());
    ASSERT_TRUE(
        catalog_.SetWaystoneDirectory(oid_, WaystoneState::kCovered, root.value(), 2).ok());

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(catalog_.AllocateRowId(oid_).ok());
    }

    auto row = catalog_.GetSysTableRow(oid_);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().next_id, kFirstRowId + 5);
    EXPECT_EQ(row.value().waystone_state, WaystoneState::kCovered);
    EXPECT_EQ(row.value().waystone_dir_root, root.value());
    EXPECT_EQ(row.value().waystone_dir_depth, 2);
}

TEST_F(WaystoneCatalogTest, ADescPageRelinkDoesNotDisturbThemEither) {
    auto root = stats::CreateDirPage(store_);
    ASSERT_TRUE(root.ok());
    ASSERT_TRUE(
        catalog_.SetWaystoneDirectory(oid_, WaystoneState::kCovered, root.value(), 1).ok());

    ASSERT_TRUE(catalog_.UpdateRelationDescPage(oid_, 4096).ok());

    auto row = catalog_.GetSysTableRow(oid_);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().desc_page_id, 4096u);
    EXPECT_EQ(row.value().waystone_dir_root, root.value());
}

}  // namespace
}  // namespace kds::catalog
