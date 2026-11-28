#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/txn/read_view.hpp"

#include "act_on_fetch_store.hpp"
#include "armed_race.hpp"

// **EM's C4: a qualified name's namespace from the catalog cache**
// (`instructions/v3.2.0/workorder-em-front-end-reuse-without-quiet-wrong.md`,
// EM-Q9 (b), EM-Q12 (a)). `Catalog::ResolveNamespace` answers a statement's
// qualifier from `CatalogCache`'s namespace map on the unfiltered route, and
// only while the memo is the instance word's. These cells pin that a cached
// answer is the scan's: across another core's drop and re-create of the
// namespace and its relation between a statement's head and its resolution,
// under a view, and for the reserved spellings.

namespace kds::catalog {
namespace {

Schema TupleKeyAndV() {
    Schema schema;
    for (const char* name : {"id", "v"}) {
        SysColumnRow col{};
        col.pos = static_cast<std::uint32_t>(schema.columns.size());
        SetName(col.name, name);
        col.type_val = kTypeValInt64;
        col.len = 8;
        col.notnull = true;
        schema.columns.push_back(col);
    }
    return schema;
}

// Two cores' catalogs over one store and one set of instance words.
struct TwoCores {
    std::unique_ptr<storage::MemoryPageDevice> device;
    std::unique_ptr<storage::DevicePageStore> store;
    InstanceWords words;
    std::array<std::unique_ptr<Catalog>, 2> cores;

    TwoCores() {
        store = testing_race::ArmedStore(device, /*first_new_page_id=*/128);
        for (auto& c : cores) {
            c = std::make_unique<Catalog>(*store);
            c->SetInstanceWords(&words);
        }
        const Status booted = cores[0]->Bootstrap();
        EXPECT_TRUE(booted.ok()) << booted.message();
    }
    Catalog& operator[](int core) { return *cores[static_cast<std::size_t>(core)]; }
    // What the pages answer: a catalog that has cached nothing.
    StatusOr<Oid> Scanned(std::string_view ns, std::string_view name) const {
        return Catalog(*store).ResolveRelation(ns, name, kNamespacePublic);
    }
};

Oid MustCreate(Catalog& c, std::string_view ns, std::string_view name) {
    auto space = c.FindNamespaceOidByName(ns);
    EXPECT_TRUE(space.ok()) << space.status().message();
    auto oid = c.CreateTable(space.value(), name, TupleKeyAndV());
    EXPECT_TRUE(oid.ok()) << oid.status().message();
    return oid.value();
}

TEST(CatalogNamespaceCacheTest, AQualifiedNameResolvedAcrossAnotherCoresDropAndRecreateIsTheScans) {
    // Core 1 caches `ns1 -> X` and `(X, t)`, then passes a statement head.
    // Core 0 drops `ns1.t` and `ns1` and creates both again before core 1
    // resolves `ns1.t` in that statement. A map read past the head would
    // answer the dropped relation; the scan answers the new one.
    //
    // **Mutation**: drop `MemoIsCurrent()` from `ResolveNamespace`'s test -
    // core 1 answers the dropped oid.
    TwoCores cores;
    ASSERT_TRUE(cores[0].CreateNamespace("ns1").ok());
    const Oid first = MustCreate(cores[0], "ns1", "t");

    cores[1].Revalidate();
    auto cached = cores[1].ResolveRelation("ns1", "t", kNamespacePublic);
    ASSERT_TRUE(cached.ok()) << cached.status().message();
    ASSERT_EQ(cached.value(), first);
    cores[1].Revalidate();  // the statement head: the memo is the word's

    std::vector<std::uint64_t> cabins;
    ASSERT_TRUE(cores[0].DropTable(first, cabins).ok());
    auto old_space = cores[0].FindNamespaceOidByName("ns1");
    ASSERT_TRUE(old_space.ok());
    ASSERT_TRUE(cores[0].DropNamespace(old_space.value()).ok());
    ASSERT_TRUE(cores[0].CreateNamespace("ns1").ok());
    const Oid second = MustCreate(cores[0], "ns1", "t");
    ASSERT_NE(first, second);

    auto mid_statement = cores[1].ResolveRelation("ns1", "t", kNamespacePublic);
    ASSERT_TRUE(mid_statement.ok()) << mid_statement.status().message();
    EXPECT_EQ(mid_statement.value(), second);
    EXPECT_EQ(mid_statement.value(), cores.Scanned("ns1", "t").value());

    cores[1].Revalidate();  // the next head drops the map
    auto next = cores[1].ResolveRelation("ns1", "t", kNamespacePublic);
    ASSERT_TRUE(next.ok());
    EXPECT_EQ(next.value(), second);
}

TEST(CatalogNamespaceCacheTest, AQualifierIsScannedOnceAndThenServedFromTheMap) {
    TwoCores cores;
    ASSERT_TRUE(cores[0].CreateNamespace("ns1").ok());
    const Oid t = MustCreate(cores[0], "ns1", "t");
    cores[1].Revalidate();
    ASSERT_EQ(cores[1].ResolveRelation("ns1", "t", kNamespacePublic).value(), t);
    const CatalogCache::Stats before = cores[1].cache_stats();
    ASSERT_EQ(cores[1].ResolveRelation("ns1", "t", kNamespacePublic).value(), t);
    const CatalogCache::Stats after = cores[1].cache_stats();
    EXPECT_EQ(after.hits - before.hits, 2u) << "the namespace and the relation, both from memory";
    EXPECT_EQ(after.misses, before.misses);
    EXPECT_EQ(after.fills, before.fills);
}

TEST(CatalogNamespaceCacheTest, ALookupUnderAViewNeitherReadsNorFillsTheMap) {
    TwoCores cores;
    ASSERT_TRUE(cores[0].CreateNamespace("ns1").ok());
    const Oid t = MustCreate(cores[0], "ns1", "t");
    cores[1].Revalidate();
    const txn::ReadView view;
    const CatalogCache::Stats before = cores[1].cache_stats();
    for (int i = 0; i < 2; ++i) {
        auto got = cores[1].ResolveRelation("ns1", "t", kNamespacePublic, 0, &view);
        ASSERT_TRUE(got.ok()) << got.status().message();
        EXPECT_EQ(got.value(), t);
    }
    const CatalogCache::Stats after = cores[1].cache_stats();
    EXPECT_EQ(after.hits, before.hits);
    EXPECT_EQ(after.misses, before.misses);
    EXPECT_EQ(after.fills, before.fills);
}

TEST(CatalogNamespaceCacheTest, TheReservedSpellingsAreAnsweredBeforeTheMap) {
    TwoCores cores;
    const Oid t = MustCreate(cores[0], "public", "t");
    cores[1].Revalidate();
    ASSERT_EQ(cores[1].ResolveRelation("public", "t", kNamespacePublic).value(), t);
    const CatalogCache::Stats before = cores[1].cache_stats();
    ASSERT_EQ(cores[1].ResolveRelation("public", "t", kNamespacePublic).value(), t);
    const CatalogCache::Stats after = cores[1].cache_stats();
    EXPECT_EQ(after.hits - before.hits, 1u) << "the relation only: `public` asks no map";
    auto sys = cores[1].ResolveRelation("sys", "tables", kNamespacePublic);
    EXPECT_EQ(sys.status().code(), StatusCode::kUnsupported);
}

TEST(CatalogNamespaceCacheTest, ADroppedNamespaceIsNotFoundAtTheNextHead) {
    TwoCores cores;
    ASSERT_TRUE(cores[0].CreateNamespace("ns1").ok());
    const Oid t = MustCreate(cores[0], "ns1", "t");
    cores[1].Revalidate();
    ASSERT_EQ(cores[1].ResolveRelation("ns1", "t", kNamespacePublic).value(), t);
    std::vector<std::uint64_t> cabins;
    ASSERT_TRUE(cores[0].DropTable(t, cabins).ok());
    ASSERT_TRUE(cores[0].DropNamespace(cores[0].FindNamespaceOidByName("ns1").value()).ok());
    cores[1].Revalidate();
    auto gone = cores[1].ResolveRelation("ns1", "t", kNamespacePublic);
    EXPECT_EQ(gone.status().code(), StatusCode::kNotFound) << gone.status().message();
    // The namespace's own refusal, not a relation's under a stale namespace.
    EXPECT_NE(gone.status().message().find("no namespace named 'ns1'"), std::string::npos)
        << gone.status().message();
    EXPECT_EQ(gone.status().code(), cores.Scanned("ns1", "t").status().code());
}

}  // namespace
}  // namespace kds::catalog
