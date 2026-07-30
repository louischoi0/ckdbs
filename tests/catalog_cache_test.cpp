#include "kds/catalog/catalog_cache.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "kds/catalog/well_known.hpp"

namespace kds::catalog {
namespace {

TableAccess MakeAccess(Oid oid, std::size_t ncols) {
    TableAccess access{};
    access.namespace_oid = kNamespacePublic;
    access.oid = oid;
    access.desc_page_id = static_cast<PageId>(1000 + oid);
    access.clustered_type = ClusteredType::kHeap;
    for (std::size_t i = 0; i < ncols; ++i) {
        SysColumnRow col{};
        col.rel_id = oid;
        col.pos = static_cast<std::uint32_t>(i);
        SetName(col.name, "c" + std::to_string(i));
        col.type_val = kTypeValInt64;
        col.len = 8;
        access.schema.columns.push_back(col);
    }
    return access;
}

SysTypeRow MakeType(Oid oid, std::string_view name, std::uint32_t type_val) {
    SysTypeRow row{};
    row.oid = oid;
    SetName(row.name, name);
    row.type_val = type_val;
    row.len = 8;
    return row;
}

TEST(CatalogCacheTest, NameToOidRoundTrips) {
    CatalogCache cache;

    EXPECT_EQ(cache.FindOidByName("accounts"), nullptr);
    cache.PutOidByName("accounts", 4000);

    const Oid* found = cache.FindOidByName("accounts");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(*found, 4000u);

    // Exact match only: the catalog's name lookup has always been
    // case-sensitive (only sys.types matching is not).
    EXPECT_EQ(cache.FindOidByName("ACCOUNTS"), nullptr);
}

TEST(CatalogCacheTest, PutOidByNameKeepsTheFirstValue) {
    CatalogCache cache;
    cache.PutOidByName("t", 4000);
    const Oid* first = cache.FindOidByName("t");
    ASSERT_NE(first, nullptr);

    cache.PutOidByName("t", 9999);

    // Same address, same value: a re-fill must not move or rewrite an entry
    // a caller may be holding.
    EXPECT_EQ(cache.FindOidByName("t"), first);
    EXPECT_EQ(*first, 4000u);
    EXPECT_EQ(cache.name_entries(), 1u);
}

TEST(CatalogCacheTest, TableAccessRoundTripsAndReportsItsEntry) {
    CatalogCache cache;

    EXPECT_EQ(cache.FindTableAccess(4000), nullptr);

    const TableAccess* put = cache.PutTableAccess(MakeAccess(4000, 3));
    ASSERT_NE(put, nullptr);
    EXPECT_EQ(put->oid, 4000u);
    EXPECT_EQ(put->desc_page_id, 5000u);
    EXPECT_EQ(put->schema.columns.size(), 3u);

    // The pointer a fill returns and the one a later lookup returns are the
    // same entry, not two copies.
    EXPECT_EQ(cache.FindTableAccess(4000), put);
}

// The property the whole pointer-returning API rests on: a statement holds a
// const TableAccess* while it works, so growing the map must never move an
// existing entry. std::unordered_map is node-based, which is why it is the
// container here.
TEST(CatalogCacheTest, TableAccessPointersSurviveRehash) {
    CatalogCache cache;

    const TableAccess* first = cache.PutTableAccess(MakeAccess(4000, 2));
    ASSERT_NE(first, nullptr);

    // Enough inserts to force several rehashes past any plausible initial
    // bucket count.
    for (Oid oid = 4001; oid < 4400; ++oid) {
        ASSERT_NE(cache.PutTableAccess(MakeAccess(oid, 2)), nullptr);
    }

    EXPECT_EQ(cache.FindTableAccess(4000), first);
    EXPECT_EQ(first->oid, 4000u);
    EXPECT_EQ(first->schema.columns.size(), 2u);
    EXPECT_EQ(cache.table_access_entries(), 400u);
}

TEST(CatalogCacheTest, TypesAreStoredWholeAndNotMatchedHere) {
    CatalogCache cache;

    EXPECT_FALSE(cache.types_loaded());
    EXPECT_EQ(cache.FindTypes(), nullptr);

    std::vector<SysTypeRow> types;
    types.push_back(MakeType(kTypeInt64, "int64", kTypeValInt64));
    types.push_back(MakeType(kTypeVarchar, "varchar", kTypeValVarchar));
    cache.PutTypes(std::move(types));

    ASSERT_TRUE(cache.types_loaded());
    const std::vector<SysTypeRow>* cached = cache.FindTypes();
    ASSERT_NE(cached, nullptr);
    ASSERT_EQ(cached->size(), 2u);
    EXPECT_EQ(NameView((*cached)[0].name), "int64");
}

TEST(CatalogCacheTest, TableListRoundTrips) {
    CatalogCache cache;
    EXPECT_EQ(cache.FindTableList(), nullptr);

    std::vector<SysObjectRow> tables;
    SysObjectRow row{};
    row.oid = 4000;
    row.type_oid = kTypeTable;
    SetName(row.name, "accounts");
    tables.push_back(row);
    cache.PutTableList(std::move(tables));

    const std::vector<SysObjectRow>* cached = cache.FindTableList();
    ASSERT_NE(cached, nullptr);
    ASSERT_EQ(cached->size(), 1u);
    EXPECT_EQ(NameView((*cached)[0].name), "accounts");
}

TEST(CatalogCacheTest, InvalidateDropsDdlFactsAndKeepsTypes) {
    CatalogCache cache;
    cache.PutOidByName("accounts", 4000);
    cache.PutTableAccess(MakeAccess(4000, 1));
    cache.PutTableList(std::vector<SysObjectRow>{});
    cache.PutTypes(std::vector<SysTypeRow>{MakeType(kTypeInt64, "int64", kTypeValInt64)});

    cache.Invalidate();

    EXPECT_EQ(cache.FindOidByName("accounts"), nullptr);
    EXPECT_EQ(cache.FindTableAccess(4000), nullptr);
    EXPECT_EQ(cache.FindTableList(), nullptr);
    EXPECT_EQ(cache.table_access_entries(), 0u);
    EXPECT_EQ(cache.name_entries(), 0u);

    // sys.types is written only by Bootstrap(), so DDL cannot stale it.
    EXPECT_TRUE(cache.types_loaded());
    EXPECT_NE(cache.FindTypes(), nullptr);
}

TEST(CatalogCacheTest, StatsCountHitsMissesFillsAndInvalidations) {
    CatalogCache cache;

    EXPECT_EQ(cache.FindTableAccess(4000), nullptr);  // miss
    cache.PutTableAccess(MakeAccess(4000, 1));        // fill
    EXPECT_NE(cache.FindTableAccess(4000), nullptr);  // hit
    EXPECT_NE(cache.FindTableAccess(4000), nullptr);  // hit

    cache.PutOidByName("accounts", 4000);             // fill
    cache.PutOidByName("accounts", 4000);             // already present: no fill
    EXPECT_NE(cache.FindOidByName("accounts"), nullptr);  // hit
    EXPECT_EQ(cache.FindOidByName("nope"), nullptr);       // miss

    cache.Invalidate();

    const CatalogCache::Stats& stats = cache.stats();
    EXPECT_EQ(stats.hits, 3u);
    EXPECT_EQ(stats.misses, 2u);
    EXPECT_EQ(stats.fills, 2u);
    EXPECT_EQ(stats.invalidations, 1u);
}

}  // namespace
}  // namespace kds::catalog
