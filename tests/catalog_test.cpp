#include "kds/catalog/catalog.hpp"

#include "kds/storage/anchor_page.hpp"

#include <atomic>
#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/sched/clock.hpp"
#include "kds/txn/manager.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/record.hpp"

namespace kds::catalog {
namespace {

class CatalogTest : public ::testing::Test {
protected:
    storage::InMemoryPageStore store_{server_first_new_page_id_};
    Catalog catalog_{store_};
    // The schema word the AT-S2 cells share between this catalog and a
    // reader (a fixture member, so it outlives every catalog pointing at it).
    std::atomic<std::uint64_t> word{0};

    // Matches kds::server::kFirstUserPageId (128) so freshly-created pages -
    // user tables' and a catalog chain's growth alike - never collide with
    // the fixed catalog roots (4-16).
    static constexpr PageId server_first_new_page_id_ = 128;
};

// Every relation needs a first column that can carry the Keystone id
// (heap-and-tuple.md section 4), so a schema-less CreateTable is no longer
// a legal table to build a fixture on.
Schema MinimalTupleKeySchema() {
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

    auto oid = catalog_.FindTableOidByName(::kds::catalog::kNamespaceSys, "tables");
    ASSERT_TRUE(oid.ok());
    EXPECT_EQ(oid.value(), kSysTablesTable);

    auto row = catalog_.GetSysTableRow(kSysObjectsTable);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().desc_page_id, kCatalogPageObjects);
}

// The split relation is retired (`raft-marks-2026-09-30.md` §9): bootstrap
// no longer creates `sys.ranges`, so no catalog relation answers the name.
TEST_F(CatalogTest, BootstrapCreatesNoRangeDirectory) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    auto oid = catalog_.FindTableOidByName(::kds::catalog::kNamespacePublic, "ranges");
    EXPECT_EQ(oid.status().code(), StatusCode::kNotFound) << oid.status().message();
}

// ---- RD2: the directory row, and the door its two rules live at --------

// ---- AF-T0: a user namespace is not the system namespace ----------------
//
// `instructions/v2.8.0/ratification-af-namespace.md` AF-P3. Three DDL
// refusals asked `namespace_oid != kNamespacePublic`, which is true of
// **every** namespace but `public` - so the first relation created outside
// it would have been undroppable and unrenamable, refused by a message
// calling it a system relation. Both cells below fail on that spelling and
// pass on `IsSystemNamespace`.
//
// No syntax is needed to reach the case: `CreateTable` takes the namespace
// as its first argument. It must name one that exists since AT-S17b, which
// checks under page 6's hold - these cells used to pass a bare oid.
TEST_F(CatalogTest, ARelationInAUserNamespaceIsRenamableAndDroppable) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto ns = catalog_.CreateNamespace("af_test");
    ASSERT_TRUE(ns.ok()) << ns.status().message();

    auto oid = catalog_.CreateTable(ns.value(), "orders_line", MinimalTupleKeySchema());
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    Status renamed = catalog_.RenameTable(oid.value(), "orders_item");
    EXPECT_TRUE(renamed.ok())
        << "a user relation outside `public` was refused a rename: " << renamed.message();

    std::vector<std::uint64_t> dropped_cabins;
    Status dropped = catalog_.DropTable(oid.value(), dropped_cabins);
    EXPECT_TRUE(dropped.ok())
        << "a user relation outside `public` was refused a drop: " << dropped.message();
}

TEST_F(CatalogTest, ACatalogsOwnRelationIsStillRefusedARenameAndADrop) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    // The half AF-T0 must not lose: narrowing the predicate is only safe
    // if the thing it was written for still trips it.
    Status renamed = catalog_.RenameTable(kSysTablesTable, "not_tables");
    EXPECT_FALSE(renamed.ok()) << "sys.tables was renamed";
    EXPECT_NE(renamed.message().find("system relation"), std::string::npos) << renamed.message();

    std::vector<std::uint64_t> dropped_cabins;
    Status dropped = catalog_.DropTable(kSysTablesTable, dropped_cabins);
    EXPECT_FALSE(dropped.ok()) << "sys.tables was dropped";
    EXPECT_NE(dropped.message().find("system relation"), std::string::npos) << dropped.message();
}

// ---- AF-T1: the namespace as a catalog object ---------------------------
//
// `instructions/v2.8.0/ratification-af-namespace.md` AF-T1. A namespace is
// a `sys.objects` row of `kTypeNamespace` whose `namespace_oid` is its own
// oid - no new relation, no new page, no format change.

TEST_F(CatalogTest, ANamespaceIsAnObjectRowThatOwnsItself) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    auto ns = catalog_.CreateNamespace("orders");
    ASSERT_TRUE(ns.ok()) << ns.status().message();
    EXPECT_GE(ns.value(), kUserOidStart) << "a namespace took a bootstrap oid";

    auto found = catalog_.FindNamespaceOidByName("orders");
    ASSERT_TRUE(found.ok()) << found.status().message();
    EXPECT_EQ(found.value(), ns.value());

    auto all = catalog_.ListNamespaces();
    ASSERT_TRUE(all.ok()) << all.status().message();
    ASSERT_EQ(all.value().size(), 1u) << "the two bootstrap namespaces are registry-only";
    EXPECT_EQ(all.value()[0].oid, ns.value());
    // The convention `InitWellKnownObjects` set for the two that bootstrap,
    // so a user namespace and a well-known one decode the same way.
    EXPECT_EQ(all.value()[0].namespace_oid, ns.value())
        << "a namespace's own namespace is not itself";
    // And therefore not a system relation, without anything being told so.
    EXPECT_FALSE(IsSystemNamespace(all.value()[0].namespace_oid));
}

TEST_F(CatalogTest, ANamespaceNameIsUniqueAndTheTwoReservedSpellingsAreRefused) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    ASSERT_TRUE(catalog_.CreateNamespace("orders").ok());

    auto again = catalog_.CreateNamespace("orders");
    EXPECT_FALSE(again.ok());
    EXPECT_EQ(again.status().code(), StatusCode::kAlreadyExists) << again.status().message();

    // `sys` is already a qualifier in the grammar (exec::kCatalogSchema)
    // and `public` is the namespace every relation is created in, so
    // admitting either as a user namespace would create an ambiguity
    // before AF-T3 has ruled on what a qualifier means.
    for (const char* reserved : {"sys", "public"}) {
        auto refused = catalog_.CreateNamespace(reserved);
        EXPECT_FALSE(refused.ok()) << reserved << " was admitted as a user namespace";
        EXPECT_EQ(refused.status().code(), StatusCode::kInvalidArgument)
            << refused.status().message();
    }
    // …and both still resolve, to the oids that existed before any DDL.
    auto sys_ns = catalog_.FindNamespaceOidByName("sys");
    ASSERT_TRUE(sys_ns.ok());
    EXPECT_EQ(sys_ns.value(), kNamespaceSys);
    auto public_ns = catalog_.FindNamespaceOidByName("public");
    ASSERT_TRUE(public_ns.ok());
    EXPECT_EQ(public_ns.value(), kNamespacePublic);

    // A well-known object's own stored name is taken too - two objects
    // answering one GetByName is a collision the registry cannot report.
    auto clash = catalog_.CreateNamespace("namespacePublic");
    EXPECT_FALSE(clash.ok());
    EXPECT_EQ(clash.status().code(), StatusCode::kAlreadyExists) << clash.status().message();
}

TEST_F(CatalogTest, DroppingANamespaceIsRestrictedByTheRelationsInIt) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    auto ns = catalog_.CreateNamespace("orders");
    ASSERT_TRUE(ns.ok()) << ns.status().message();

    auto rel = catalog_.CreateTable(ns.value(), "orders_line", MinimalTupleKeySchema());
    ASSERT_TRUE(rel.ok()) << rel.status().message();

    Status restricted = catalog_.DropNamespace(ns.value());
    EXPECT_FALSE(restricted.ok()) << "a namespace holding a relation was dropped";
    EXPECT_NE(restricted.message().find("orders_line"), std::string::npos)
        << "the refusal did not name the relation that blocked it: " << restricted.message();

    // Empty it, and the drop goes through.
    std::vector<std::uint64_t> dropped_cabins;
    ASSERT_TRUE(catalog_.DropTable(rel.value(), dropped_cabins).ok());
    Status dropped = catalog_.DropNamespace(ns.value());
    EXPECT_TRUE(dropped.ok()) << dropped.message();

    // The name is free the instant the retype lands, and the row is not:
    // it is GenerateUserOid()'s floor evidence, so a reissued oid could
    // make a live relation read as a member of a namespace that is gone.
    EXPECT_EQ(catalog_.FindNamespaceOidByName("orders").status().code(), StatusCode::kNotFound);
    auto live = catalog_.ListNamespaces();
    ASSERT_TRUE(live.ok());
    EXPECT_TRUE(live.value().empty()) << "a dropped namespace still lists as live";
    auto next = catalog_.GenerateUserOid();
    ASSERT_TRUE(next.ok());
    EXPECT_GT(next.value(), ns.value()) << "the dropped namespace's oid was reissued";
}

TEST_F(CatalogTest, TheTwoReservedNamespacesCannotBeDropped) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    for (const Oid reserved : {kNamespaceSys, kNamespacePublic}) {
        Status s = catalog_.DropNamespace(reserved);
        EXPECT_FALSE(s.ok()) << "reserved namespace oid " << reserved << " was dropped";
        EXPECT_EQ(s.code(), StatusCode::kInvalidArgument) << s.message();
    }
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

    auto oid = catalog_.CreateTable(kNamespacePublic, "accounts", schema);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    auto found_oid = catalog_.FindTableOidByName(::kds::catalog::kNamespacePublic, "accounts");
    ASSERT_TRUE(found_oid.ok());
    EXPECT_EQ(found_oid.value(), oid.value());

    auto table_row = catalog_.GetSysTableRow(oid.value());
    ASSERT_TRUE(table_row.ok());
    EXPECT_EQ(table_row.value().namespace_oid, kNamespacePublic);
    EXPECT_EQ(table_row.value().clustered_type, ClusteredType::kBtree);

    auto built_schema = catalog_.BuildSchemaFromColumns(oid.value());
    ASSERT_TRUE(built_schema.ok());
    ASSERT_EQ(built_schema.value().columns.size(), 2u);
    auto id_lookup = built_schema.value().FindColumn("id", /*byte_offset=*/0);
    ASSERT_TRUE(id_lookup.ok());
    const SysColumnRow* id_found = id_lookup.value();
    ASSERT_NE(id_found, nullptr);
    EXPECT_EQ(id_found->type_val, 5u);
    EXPECT_TRUE(id_found->notnull);
}

TEST_F(CatalogTest, CreateTableRejectsARelationWithNoColumns) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    Schema schema;
    auto oid = catalog_.CreateTable(kNamespacePublic, "t", schema);
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

    auto oid = catalog_.CreateTable(kNamespacePublic, "widgets", schema);
    ASSERT_TRUE(oid.ok());

    auto access = catalog_.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok()) << access.status().message();
    EXPECT_EQ(access.value()->oid, oid.value());
    EXPECT_EQ(access.value()->clustered_type, ClusteredType::kBtree);
    EXPECT_EQ(access.value()->schema.columns.size(), 1u);

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
    Schema schema = MinimalTupleKeySchema();
    auto oid = catalog_.CreateTable(kNamespacePublic, "held", schema);
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
    Schema schema = MinimalTupleKeySchema();
    auto oid = catalog_.CreateTable(kNamespacePublic, "relinked", schema);
    ASSERT_TRUE(oid.ok());

    auto first = catalog_.InitTableAccess(oid.value());
    ASSERT_TRUE(first.ok());
    const PageId old_desc = first.value()->desc_page_id;

    ASSERT_TRUE(catalog_.PublishRoot(oid.value(), /*index_oid=*/0, old_desc + 1000,
                                                first.value()->anchor_page_id)
                    .ok());

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

TEST_F(CatalogTest, PublishRootPreservesRowIdentity) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    Schema schema = MinimalTupleKeySchema();
    auto oid = catalog_.CreateTable(kNamespacePublic, "movable", schema);
    ASSERT_TRUE(oid.ok());

    auto before = catalog_.GetSysTableRow(oid.value());
    ASSERT_TRUE(before.ok());
    PageId old_desc = before.value().desc_page_id;

    Status s = catalog_.PublishRoot(oid.value(), /*index_oid=*/0, old_desc + 1000,
                                               before.value().anchor_page_id);
    ASSERT_TRUE(s.ok()) << s.message();

    auto after = catalog_.GetSysTableRow(oid.value());
    ASSERT_TRUE(after.ok());
    // PW2-3: the row is CREATE-fixed - the move landed in the anchor, and
    // a fresh fill resolves it from there.
    EXPECT_EQ(after.value().desc_page_id, old_desc);
    {
        auto anchor = store_.GetForRead(after.value().anchor_page_id);
        ASSERT_TRUE(anchor.ok());
        EXPECT_EQ(storage::AnchorClusteredRoot(anchor.value().bytes()), old_desc + 1000);
    }
}

// ---- Secondary indexes (docs/spec/index.md §12, workplan IX03) ----------

// Three columns, so an index can be declared on something that is neither
// the tuple key nor the only other column.
Schema IndexableSchema() {
    Schema schema = MinimalTupleKeySchema();
    for (const char* name : {"owner", "amount"}) {
        SysColumnRow col{};
        col.pos = static_cast<std::uint32_t>(schema.columns.size());
        SetName(col.name, name);
        col.type_val = kTypeValInt64;
        col.len = 8;
        schema.columns.push_back(col);
    }
    return schema;
}

Catalog::IndexDef SimpleIndex(Oid table_oid, std::string name, std::uint16_t col) {
    Catalog::IndexDef def;
    def.table_oid = table_oid;
    def.name = std::move(name);
    def.root_page_id = 1000;
    def.key_width = 9;
    def.entry_width = 17;
    def.key_cols = {col};
    return def;
}

TEST_F(CatalogTest, IndexRowsRoundTripAndFilterByTable) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    auto table_a = catalog_.CreateTable(kNamespacePublic, "a", IndexableSchema());
    ASSERT_TRUE(table_a.ok()) << table_a.status().message();
    auto table_b = catalog_.CreateTable(kNamespacePublic, "b", IndexableSchema());
    ASSERT_TRUE(table_b.ok());

    auto by_owner = catalog_.CreateIndex(SimpleIndex(table_a.value(), "a_owner", 1));
    ASSERT_TRUE(by_owner.ok()) << by_owner.status().message();
    ASSERT_TRUE(catalog_.CreateIndex(SimpleIndex(table_a.value(), "a_amount", 2)).ok());
    ASSERT_TRUE(catalog_.CreateIndex(SimpleIndex(table_b.value(), "b_owner", 1)).ok());

    auto for_a = catalog_.FindIndexesForTable(table_a.value());
    ASSERT_TRUE(for_a.ok());
    EXPECT_EQ(for_a.value().size(), 2u);

    auto by_name = catalog_.FindIndexByName("a_owner");
    ASSERT_TRUE(by_name.ok());
    EXPECT_EQ(by_name.value().index_oid, by_owner.value());
    EXPECT_EQ(by_name.value().table_oid, table_a.value());
    EXPECT_EQ(by_name.value().nkeys, 1u);
    EXPECT_EQ(by_name.value().key_cols[0], 1u);

    auto on_col1 = catalog_.FindIndexOnColumn(table_a.value(), 1);
    ASSERT_TRUE(on_col1.ok());
    EXPECT_EQ(NameView(on_col1.value().name), "a_owner");

    auto missing = catalog_.FindIndexOnColumn(table_a.value(), 99);
    EXPECT_FALSE(missing.ok());
    EXPECT_EQ(missing.status().code(), StatusCode::kNotFound);
}

// "Leading", not "contains". An index on (a, b) can serve an equality on a
// and cannot serve one on b, so answering yes for b would stop the compiler
// calling that step a filter scan while leaving it exactly as slow.
TEST_F(CatalogTest, FindIndexOnColumnAnswersForTheLeadingKeyColumnOnly) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema());
    ASSERT_TRUE(table.ok());

    Catalog::IndexDef def = SimpleIndex(table.value(), "composite", 1);
    def.key_cols = {1, 2};
    ASSERT_TRUE(catalog_.CreateIndex(def).ok());

    EXPECT_TRUE(catalog_.FindIndexOnColumn(table.value(), 1).ok());
    EXPECT_FALSE(catalog_.FindIndexOnColumn(table.value(), 2).ok());
}

TEST_F(CatalogTest, CreateIndexRefusesRatherThanTruncatingOrGuessing) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema());
    ASSERT_TRUE(table.ok());

    // The tuple key: the clustered tree already indexes it.
    EXPECT_FALSE(catalog_.CreateIndex(SimpleIndex(table.value(), "on_pk", 0)).ok());

    // A column the relation does not have.
    EXPECT_FALSE(catalog_.CreateIndex(SimpleIndex(table.value(), "off_end", 7)).ok());

    // An empty key.
    Catalog::IndexDef empty = SimpleIndex(table.value(), "empty", 1);
    empty.key_cols.clear();
    EXPECT_FALSE(catalog_.CreateIndex(empty).ok());

    // The same column twice - two encodings of one value, ordered against
    // itself.
    Catalog::IndexDef repeated = SimpleIndex(table.value(), "repeated", 1);
    repeated.key_cols = {1, 1};
    EXPECT_FALSE(catalog_.CreateIndex(repeated).ok());

    // Over the cap. A cap refuses and never truncates (spec §11): a
    // truncated index declared complete is a wrong answer with a right
    // answer's shape.
    Catalog::IndexDef too_wide = SimpleIndex(table.value(), "too_wide", 1);
    too_wide.key_cols.assign(kMaxIndexKeyColumns + 1, 1);
    EXPECT_FALSE(catalog_.CreateIndex(too_wide).ok());
    Catalog::IndexDef too_covered = SimpleIndex(table.value(), "too_covered", 1);
    too_covered.covered_cols.assign(kMaxIndexCoveredColumns + 1, 2);
    EXPECT_FALSE(catalog_.CreateIndex(too_covered).ok());

    // UNIQUE is admitted since BX (spec IX11); a flag bit nothing defines is
    // not.
    Catalog::IndexDef unknown = SimpleIndex(table.value(), "unknown", 1);
    unknown.flags = kIndexFlagUnique << 1;
    auto refused = catalog_.CreateIndex(unknown);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kInvalidArgument);
    Catalog::IndexDef unique = SimpleIndex(table.value(), "unique", 1);
    unique.flags = kIndexFlagUnique;
    EXPECT_TRUE(catalog_.CreateIndex(unique).ok());

    // A name already in use.
    ASSERT_TRUE(catalog_.CreateIndex(SimpleIndex(table.value(), "taken", 1)).ok());
    auto again = catalog_.CreateIndex(SimpleIndex(table.value(), "taken", 2));
    EXPECT_FALSE(again.ok());
    EXPECT_EQ(again.status().code(), StatusCode::kAlreadyExists);
}

TEST_F(CatalogTest, ADroppedIndexIsRetiredSoTheNameIsFreeAgain) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema());
    ASSERT_TRUE(table.ok());

    auto oid = catalog_.CreateIndex(SimpleIndex(table.value(), "ix", 1));
    ASSERT_TRUE(oid.ok());
    ASSERT_TRUE(catalog_.DropIndex(oid.value()).ok());

    EXPECT_FALSE(catalog_.FindIndexByName("ix").ok());
    EXPECT_TRUE(catalog_.FindIndexesForTable(table.value()).value().empty());

    // Retired rather than delete-marked, so a re-creation does not collide
    // with a row nobody can see (DropCabin's argument).
    EXPECT_TRUE(catalog_.CreateIndex(SimpleIndex(table.value(), "ix", 1)).ok());

    EXPECT_EQ(catalog_.DropIndex(999999).code(), StatusCode::kNotFound);
}

TEST_F(CatalogTest, AnIndexRootMovesInPlaceRatherThanInvalidatingTheCache) {
    // A root split reports a new root and someone above the storage layer
    // records it - the counterpart of a clustered root move, but *not* the
    // counterpart of its invalidation.
    //
    // This asserted a version bump when IX04 landed, and IX06 changed it
    // deliberately. A split happens inside an ordinary INSERT, so a global
    // drop would dangle the `const TableAccess*` the running statement
    // holds - and a multi-row UPDATE would be holding it across every later
    // row. The fact belongs to one index and is read by nothing else, which
    // is the same test the two pattern in-place updates pass
    // (catalog_cache.hpp).
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema());
    ASSERT_TRUE(table.ok());
    auto oid = catalog_.CreateIndex(SimpleIndex(table.value(), "ix", 1));
    ASSERT_TRUE(oid.ok());

    // The pointer a caller would be holding across an index insert.
    auto held = catalog_.InitTableAccess(table.value());
    ASSERT_TRUE(held.ok());
    const catalog::TableAccess* ta = held.value();
    ASSERT_EQ(ta->indexes.size(), 1u);

    const std::uint64_t before = catalog_.catalog_version();
    const PageId original_root = ta->indexes[0].root_page_id;
    ASSERT_TRUE(catalog_.PublishRoot(table.value(), oid.value(), 4242,
                                         ta->anchor_page_id)
                    .ok());
    EXPECT_EQ(catalog_.catalog_version(), before) << "a root move must not drop the cache";

    // Still valid, and already showing the new root.
    EXPECT_EQ(ta->indexes[0].root_page_id, 4242u);

    // PW2-3: the sys.indexes row is CREATE-fixed - the durable half moved
    // to the anchor slot, and a fresh fill resolves it from there (the
    // f5686f8 review's C4.1, the previously unpinned write).
    auto row = catalog_.FindIndexByName("ix");
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().root_page_id, original_root);
    EXPECT_EQ(row.value().key_cols[0], 1u);  // the rest of the row survived
    {
        auto rel = catalog_.GetSysTableRow(table.value());
        ASSERT_TRUE(rel.ok());
        auto anchor = store_.GetForRead(rel.value().anchor_page_id);
        ASSERT_TRUE(anchor.ok());
        auto slot = storage::AnchorIndexRoot(anchor.value().bytes(), oid.value());
        ASSERT_TRUE(slot.ok());
        EXPECT_EQ(slot.value(), 4242u);
    }

    // The NotFound probe went with the sys.indexes scan (the 96b0343
    // review's C1): existence is the caller's fact now - MaintainIndexes
    // iterates access.indexes, which is the proof.
}

// ---- TableAccess::indexes / index_mask (workplan IX04) -----------------

TEST_F(CatalogTest, TableAccessCarriesTheRelationsIndexesInCreationOrder) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema());
    ASSERT_TRUE(table.ok());

    Catalog::IndexDef composite = SimpleIndex(table.value(), "composite", 1);
    composite.key_cols = {2, 1};
    composite.covered_cols = {1};
    composite.root_page_id = 555;
    composite.key_width = 18;
    composite.entry_width = 34;
    auto first = catalog_.CreateIndex(composite);
    ASSERT_TRUE(first.ok()) << first.status().message();
    auto second = catalog_.CreateIndex(SimpleIndex(table.value(), "single", 1));
    ASSERT_TRUE(second.ok());

    auto access = catalog_.InitTableAccess(table.value());
    ASSERT_TRUE(access.ok()) << access.status().message();
    ASSERT_EQ(access.value()->indexes.size(), 2u);

    // Sorted by index_oid, which is creation order - so §9's lowest-oid
    // tie-break is a property of the list and not of how the rows happened
    // to land on the catalog page.
    EXPECT_EQ(access.value()->indexes[0].index_oid, first.value());
    EXPECT_EQ(access.value()->indexes[1].index_oid, second.value());

    const TableAccess::IndexRef& ix = access.value()->indexes[0];
    EXPECT_EQ(ix.root_page_id, 555u);
    EXPECT_EQ(ix.key_width, 18u);
    EXPECT_EQ(ix.entry_width, 34u);
    ASSERT_EQ(ix.keys().size(), 2u);
    // Declared order, not sorted: it is the order the key encoding
    // concatenates them in.
    EXPECT_EQ(ix.keys()[0], 2u);
    EXPECT_EQ(ix.keys()[1], 1u);
    ASSERT_EQ(ix.covered().size(), 1u);
    EXPECT_EQ(ix.covered()[0], 1u);
}

TEST_F(CatalogTest, TheIndexMaskNamesLeadingKeyColumnsOnly) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema());
    ASSERT_TRUE(table.ok());

    Catalog::IndexDef def = SimpleIndex(table.value(), "composite", 1);
    def.key_cols = {1, 2};
    ASSERT_TRUE(catalog_.CreateIndex(def).ok());

    auto access = catalog_.InitTableAccess(table.value());
    ASSERT_TRUE(access.ok());

    // Column 1 leads the index; column 2 is in it and cannot be entered by
    // an equality, so a bit for it would stop the compiler calling that
    // step a filter scan while leaving it exactly as slow.
    EXPECT_NE(access.value()->index_mask & (std::uint64_t{1} << 1), 0u);
    EXPECT_EQ(access.value()->index_mask & (std::uint64_t{1} << 2), 0u);
    // Bit 0 is always clear: CreateIndex refuses the tuple key.
    EXPECT_EQ(access.value()->index_mask & 1u, 0u);

    ASSERT_NE(access.value()->IndexOn(1), nullptr);
    EXPECT_EQ(NameView(catalog_.FindIndexByName("composite").value().name), "composite");
    EXPECT_EQ(access.value()->IndexOn(2), nullptr);
    EXPECT_EQ(access.value()->IndexOn(0), nullptr);
}

TEST_F(CatalogTest, IndexOnPicksTheLowestOidWhenTwoIndexesShareALeadingColumn) {
    // Spec §9's tie-break, which exists so the same statement compiles the
    // same way whatever the data did.
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema());
    ASSERT_TRUE(table.ok());

    auto first = catalog_.CreateIndex(SimpleIndex(table.value(), "one", 1));
    ASSERT_TRUE(first.ok());
    Catalog::IndexDef wider = SimpleIndex(table.value(), "two", 1);
    wider.key_cols = {1, 2};
    ASSERT_TRUE(catalog_.CreateIndex(wider).ok());

    auto access = catalog_.InitTableAccess(table.value());
    ASSERT_TRUE(access.ok());
    ASSERT_NE(access.value()->IndexOn(1), nullptr);
    EXPECT_EQ(access.value()->IndexOn(1)->index_oid, first.value());
}

TEST_F(CatalogTest, ACachedTableAccessSeesAnIndexCreatedAfterItWasFilled) {
    // The reason CreateIndex bumps: an index appearing stales index_mask on
    // every held entry for the relation, and the compiler reads it.
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema());
    ASSERT_TRUE(table.ok());

    auto before = catalog_.InitTableAccess(table.value());
    ASSERT_TRUE(before.ok());
    EXPECT_EQ(before.value()->index_mask, 0u);

    ASSERT_TRUE(catalog_.CreateIndex(SimpleIndex(table.value(), "ix", 1)).ok());

    auto after = catalog_.InitTableAccess(table.value());
    ASSERT_TRUE(after.ok());
    EXPECT_NE(after.value()->index_mask, 0u);

    // ...and a root that moved. This is the one field on TableAccess that
    // can change without DDL, which is why a caller holding the pointer
    // across an index insert that grows a level is holding a dangling one.
    auto oid = catalog_.FindIndexByName("ix");
    ASSERT_TRUE(oid.ok());
    ASSERT_TRUE(catalog_.PublishRoot(table.value(), oid.value().index_oid, 7777,
                                         after.value()->anchor_page_id)
                    .ok());

    auto relinked = catalog_.InitTableAccess(table.value());
    ASSERT_TRUE(relinked.ok());
    ASSERT_EQ(relinked.value()->indexes.size(), 1u);
    EXPECT_EQ(relinked.value()->indexes[0].root_page_id, 7777u);
}

TEST_F(CatalogTest, ADroppedIndexLeavesTheRelationWithNoneAgain) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema());
    ASSERT_TRUE(table.ok());
    auto oid = catalog_.CreateIndex(SimpleIndex(table.value(), "ix", 1));
    ASSERT_TRUE(oid.ok());
    ASSERT_NE(catalog_.InitTableAccess(table.value()).value()->index_mask, 0u);

    ASSERT_TRUE(catalog_.DropIndex(oid.value()).ok());

    auto after = catalog_.InitTableAccess(table.value());
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value()->index_mask, 0u);
    EXPECT_TRUE(after.value()->indexes.empty());
}

TEST_F(CatalogTest, OneRelationsIndexesDoNotAppearOnAnother) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto a = catalog_.CreateTable(kNamespacePublic, "a", IndexableSchema());
    auto b = catalog_.CreateTable(kNamespacePublic, "b", IndexableSchema());
    ASSERT_TRUE(a.ok() && b.ok());
    ASSERT_TRUE(catalog_.CreateIndex(SimpleIndex(a.value(), "a_ix", 1)).ok());

    EXPECT_EQ(catalog_.InitTableAccess(a.value()).value()->indexes.size(), 1u);
    EXPECT_TRUE(catalog_.InitTableAccess(b.value()).value()->indexes.empty());
    EXPECT_EQ(catalog_.InitTableAccess(b.value()).value()->index_mask, 0u);
}

// Where InsertIndexRow() deliberately did not bump: that comment was true
// while nothing cached anything derived from sys.indexes, and IX04 makes it
// false.
TEST_F(CatalogTest, CreatingAndDroppingAnIndexBumpsTheCatalogVersion) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema());
    ASSERT_TRUE(table.ok());

    const std::uint64_t before_create = catalog_.catalog_version();
    auto oid = catalog_.CreateIndex(SimpleIndex(table.value(), "ix", 1));
    ASSERT_TRUE(oid.ok());
    const std::uint64_t after_create = catalog_.catalog_version();
    EXPECT_GT(after_create, before_create);

    ASSERT_TRUE(catalog_.DropIndex(oid.value()).ok());
    EXPECT_GT(catalog_.catalog_version(), after_create);
}

// ---- AT-S2: the schema version word ------------------------------------

TEST_F(CatalogTest, EveryDdlBumpsTheSchemaWordAndTheWriterKeepsItsOwnCache) {
    // One atomic for the instance (`Expeditor` owns it; a fixture stands in).
    // `BumpVersion()` advances it at every catalog write - which is before
    // the DDL's relation `X` is released, D21's order - and the writer's own
    // cache, dropped by `BumpVersion()` itself, is not dropped a second
    // time by its own bump: `cache_built_at_` moves with the word.
    catalog_.SetSchemaWord(&word);
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    const std::uint64_t after_bootstrap = word.load();
    EXPECT_GT(after_bootstrap, 0u) << "bootstrap writes catalog rows and bumps";

    auto oid = catalog_.CreateTable(kNamespacePublic, "w", MinimalTupleKeySchema());
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    EXPECT_GT(word.load(), after_bootstrap);

    // The writer's cache holds across its own bumps: two resolves, one fill.
    const auto stats_before = catalog_.cache_stats();
    ASSERT_TRUE(catalog_.InitTableAccess(oid.value()).ok());
    ASSERT_TRUE(catalog_.InitTableAccess(oid.value()).ok());
    EXPECT_EQ(catalog_.cache_stats().fills, stats_before.fills + 1)
        << "the writer dropped its own cache on its own bump";
}

TEST_F(CatalogTest, AReaderDropsOnlyAtItsBoundaryNeverInsideARead) {
    // The defect AT-S2's review found and the rule that closes it: a drop
    // frees every `const TableAccess*` a running statement holds, so the
    // word is asked at a task boundary and never inside a cached read. A
    // reader that filled `a`, saw the word move, and then read `b` - as a
    // join's second bind does - still holds `a`; its next `Revalidate()` is
    // what drops.
    catalog_.SetSchemaWord(&word);
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto a = catalog_.CreateTable(kNamespacePublic, "a", MinimalTupleKeySchema());
    auto b = catalog_.CreateTable(kNamespacePublic, "b", MinimalTupleKeySchema());
    ASSERT_TRUE(a.ok() && b.ok());

    Catalog reader(store_, storage::kDefaultInlineCellWidth);
    reader.SetSchemaWord(&word);
    reader.Revalidate();
    auto held = reader.InitTableAccess(a.value());
    ASSERT_TRUE(held.ok());
    const std::uint64_t drops = reader.cache_stats().invalidations;

    ASSERT_TRUE(catalog_.CreateTable(kNamespacePublic, "c", MinimalTupleKeySchema()).ok());
    ASSERT_TRUE(reader.InitTableAccess(b.value()).ok());
    // ASSERT, not EXPECT: under the mutant this cell exists to kill the
    // next line reads through a freed entry and would crash the binary
    // instead of failing this cell.
    ASSERT_EQ(reader.cache_stats().invalidations, drops)
        << "a cached read dropped the cache under a pointer the statement holds";
    EXPECT_EQ(held.value()->oid, a.value()) << "the held entry was freed";

    reader.Revalidate();
    EXPECT_EQ(reader.cache_stats().invalidations, drops + 1) << "the boundary did not drop";
}

TEST_F(CatalogTest, ABumpFromACacheThatIsBehindDoesNotSwallowTheOneItMissed) {
    // `BumpWord`'s guard, the half no other cell reaches: a catalog whose
    // cache is behind the word bumps it - a peer's own write, once AT-S3
    // admits one - and must **not** adopt the new value, or the bump it
    // never revalidated against is swallowed and the stale memo stands for
    // good. The next boundary must still drop.
    catalog_.SetSchemaWord(&word);
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto oid = catalog_.CreateTable(kNamespacePublic, "s", MinimalTupleKeySchema());
    ASSERT_TRUE(oid.ok());

    Catalog reader(store_, storage::kDefaultInlineCellWidth);
    reader.SetSchemaWord(&word);
    reader.Revalidate();
    ASSERT_TRUE(reader.InitTableAccess(oid.value()).ok());
    const std::uint64_t drops = reader.cache_stats().invalidations;

    // The reader falls behind, then bumps while behind.
    ASSERT_TRUE(catalog_.CreateTable(kNamespacePublic, "s2", MinimalTupleKeySchema()).ok());
    reader.InvalidateAfterCompensation();
    // Its own bump dropped its cache once; the boundary must drop again for
    // the change it missed, which is what a swallowed bump would hide.
    const std::uint64_t after_own = reader.cache_stats().invalidations;
    EXPECT_EQ(after_own, drops + 1);
    reader.Revalidate();
    EXPECT_EQ(reader.cache_stats().invalidations, after_own + 1)
        << "the reader's own bump adopted a word it had never revalidated against";
}

TEST_F(CatalogTest, AReaderDropsItsCacheWhenTheWordMovesAndOnlyThen) {
    // A second catalog over the same store, sharing the word: what a peer
    // is. It fills its memo, the word moves, and its next cached read
    // drops the memo and refills - with nothing sent. While the word is
    // still, the memo is served.
    catalog_.SetSchemaWord(&word);
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto oid = catalog_.CreateTable(kNamespacePublic, "r", MinimalTupleKeySchema());
    ASSERT_TRUE(oid.ok());

    Catalog reader(store_, storage::kDefaultInlineCellWidth);
    reader.SetSchemaWord(&word);
    reader.Revalidate();
    ASSERT_TRUE(reader.InitTableAccess(oid.value()).ok());
    const auto filled = reader.cache_stats();
    reader.Revalidate();
    ASSERT_TRUE(reader.InitTableAccess(oid.value()).ok());
    EXPECT_EQ(reader.cache_stats().fills, filled.fills) << "the word is still and the memo went";

    // A DDL on the writer: the reader's next boundary drops, its next read
    // refills.
    ASSERT_TRUE(catalog_.CreateTable(kNamespacePublic, "r2", MinimalTupleKeySchema()).ok());
    reader.Revalidate();
    ASSERT_TRUE(reader.InitTableAccess(oid.value()).ok());
    EXPECT_EQ(reader.cache_stats().fills, filled.fills + 1)
        << "the word moved and the reader served its stale memo";
}

TEST_F(CatalogTest, InvalidateAfterCompensationBumpsTheWordAndDropCacheDoesNot) {
    // `InvalidateAfterCompensation()` is the call both endings of a
    // catalog-writing transaction make (`TxnSessionTest` runs the endings
    // themselves); it bumps. `DropCache()` is the mount's post-redo drop;
    // it bumps nothing, since nothing on this instance's numbering changed.
    catalog_.SetSchemaWord(&word);
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    const std::uint64_t before = word.load();
    catalog_.InvalidateAfterCompensation();
    EXPECT_EQ(word.load(), before + 1);
    catalog_.DropCache();
    EXPECT_EQ(word.load(), before + 1);
}

// The version counter parser.md I5 / PR20 stamp bound statements with. Its
// contract is monotonic-on-DDL, not "+1 per statement": one CreateTable
// bumps it once per catalog row it writes.
TEST_F(CatalogTest, DdlBumpsTheCatalogVersion) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    const std::uint64_t after_bootstrap = catalog_.catalog_version();
    EXPECT_GT(after_bootstrap, 0u);

    Schema schema = MinimalTupleKeySchema();
    auto oid = catalog_.CreateTable(kNamespacePublic, "versioned", schema);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    const std::uint64_t after_create = catalog_.catalog_version();
    EXPECT_GT(after_create, after_bootstrap);

    auto vrow = catalog_.GetSysTableRow(oid.value());
    ASSERT_TRUE(vrow.ok());
    ASSERT_TRUE(
        catalog_.PublishRoot(oid.value(), /*index_oid=*/0, 9999, vrow.value().anchor_page_id).ok());
    // PW2-4 reversed the old pin here: a root move is not DDL - the cached
    // entry updates in place, and the version must NOT bump (the running
    // INSERT holds the entry, and a peer's move must not broadcast).
    EXPECT_EQ(catalog_.catalog_version(), after_create);
}

// The rule that keeps a statement's cached TableAccess alive across its own
// insert: the id sequence is not a cached fact, so issuing one stales
// nothing. If this ever starts bumping, every bound statement re-parses on
// every insert.
TEST_F(CatalogTest, AllocateRowIdAndReadsDoNotBumpTheCatalogVersion) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    Schema schema = MinimalTupleKeySchema();
    auto oid = catalog_.CreateTable(kNamespacePublic, "seq", schema);
    ASSERT_TRUE(oid.ok());

    const std::uint64_t before = catalog_.catalog_version();

    for (int i = 0; i < 10; ++i) {
        auto id = catalog_.AllocateRowId(oid.value());
        ASSERT_TRUE(id.ok()) << id.status().message();
    }
    ASSERT_TRUE(catalog_.FindTableOidByName(::kds::catalog::kNamespacePublic, "seq").ok());
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

    Schema schema = MinimalTupleKeySchema();
    ASSERT_TRUE(catalog_.CreateTable(kNamespacePublic, "listed", schema).ok());

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

    auto oid = catalog_.FindTableOidByName(::kds::catalog::kNamespacePublic, "does_not_exist");
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
    auto store = storage::DevicePageStore::Open(*device.value(), ::kds::storage::FrameCapacity{4096}, /*first_new_page_id=*/128);
    ASSERT_TRUE(store.ok()) << store.status().message();

    Catalog catalog(*store.value());
    ASSERT_TRUE(catalog.Bootstrap().ok());
    Schema schema = MinimalTupleKeySchema();
    auto oid = catalog.CreateTable(kNamespacePublic, "hot", schema);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    // Warm the three cached facts, then flush so every frame starts clean.
    ASSERT_TRUE(catalog.FindTableOidByName(::kds::catalog::kNamespacePublic, "hot").ok());
    ASSERT_TRUE(catalog.InitTableAccess(oid.value()).ok());
    ASSERT_TRUE(catalog.ResolveTypeByVal(kTypeValInt64).ok());
    ASSERT_TRUE(catalog.ListTables().ok());
    ASSERT_TRUE(store.value()->Flush().ok());
    ASSERT_TRUE(store.value()->DirtyPageIds().empty());

    // The catalog work a SELECT or an UPDATE does, fifty times over.
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(catalog.FindTableOidByName(::kds::catalog::kNamespacePublic, "hot").ok());
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

// ---- sys.patterns (docs/spec/waystone-concpets.md section 4) -------------------

// A logger over memory, for the renewal's lapse line.
class CapturedLog {
public:
    CapturedLog() : logger_(&sink_, clock_, LogLevel::kTrace) {}
    Logger* get() noexcept { return &logger_; }
    // Whether a line carries "[<component>]" and every one of `needles`.
    bool Has(const std::string& component, const std::vector<std::string>& needles) const {
        for (const std::string& line : sink_.lines) {
            if (line.find("[" + component + "]") == std::string::npos) continue;
            if (std::all_of(needles.begin(), needles.end(), [&](const std::string& n) {
                    return line.find(n) != std::string::npos;
                })) {
                return true;
            }
        }
        return false;
    }

private:
    MemoryLogSink sink_;
    ManualWallClock clock_{1'700'000'000};
    Logger logger_;
};

class PatternCatalogTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(catalog_.Bootstrap().ok()); }

    // Writes a sys.patterns row straight onto the catalog page, bypassing
    // SetPatternWaystone(). This is how a stale row actually comes to exist
    // - an older build wrote it and then the fingerprint version was bumped
    // - and there is deliberately no API that can produce one, since
    // SetPatternWaystone() stamps the current version itself.
    void WriteRawPatternRow(std::uint64_t fetch_id, std::uint32_t version,
                            std::uint16_t flags = 0, PageId root = kInvalidPageId) {
        auto bytes = store_.Get(kCatalogPagePatterns);
        ASSERT_TRUE(bytes.ok());
        kds::heap::PageView page(bytes.value().bytes());

        SysPatternRow row{};
        row.oid = 900000 + fetch_id;
        row.fetch_id = fetch_id;
        row.fingerprint_version = version;
        row.waystone_root = root;
        row.dir_depth = root != kInvalidPageId ? 1 : 0;
        row.flags = flags;
        auto encoded = row.Encode();
        ASSERT_TRUE(page.InsertTuple(encoded, kBootstrapXid).ok());
    }

    // `WAYSTONE ON`/`OFF`'s write, answering whether it wrote.
    bool On(std::uint64_t fetch_id, PageId root = kInvalidPageId) {
        auto wrote = catalog_.SetPatternWaystone(fetch_id, kStmtClassUnclassified, true, root);
        EXPECT_TRUE(wrote.ok()) << wrote.status().message();
        return wrote.ok() && wrote.value().written;
    }
    bool Off(std::uint64_t fetch_id) {
        auto wrote = catalog_.SetPatternWaystone(fetch_id, 0, false, kInvalidPageId);
        EXPECT_TRUE(wrote.ok()) << wrote.status().message();
        return wrote.ok() && wrote.value().written;
    }

    // Every row carrying `fetch_id`, of any version, in page order.
    std::vector<SysPatternRow> RowsOf(std::uint64_t fetch_id) {
        std::vector<SysPatternRow> out;
        auto rows = catalog_.ListPatterns();
        EXPECT_TRUE(rows.ok());
        if (!rows.ok()) return out;
        for (const SysPatternRow& row : rows.value()) {
            if (row.fetch_id == fetch_id) out.push_back(row);
        }
        return out;
    }

    storage::InMemoryPageStore store_{128};
    Catalog catalog_{store_};

    static constexpr std::uint32_t kVersion = parser::kFingerprintVersion;
    static constexpr std::uint32_t kForeignVersion = parser::kFingerprintVersion + 1;
};

TEST_F(PatternCatalogTest, BootstrapCreatesTheRelation) {
    auto oid = catalog_.FindTableOidByName(::kds::catalog::kNamespaceSys, "patterns");
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    EXPECT_EQ(oid.value(), kSysPatternsTable);

    auto row = catalog_.GetSysTableRow(kSysPatternsTable);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().desc_page_id, kCatalogPagePatterns);
}

// ---- WAYSTONE ON / OFF's one writer (CF-S1b) ---------------------------------

TEST_F(PatternCatalogTest, OnInsertsAnOptedRowThatFindRoundTrips) {
    ASSERT_TRUE(On(0xABCDEF));
    auto found = catalog_.FindPattern(0xABCDEF);
    ASSERT_TRUE(found.ok()) << found.status().message();
    EXPECT_EQ(found.value()->fetch_id, 0xABCDEFu);
    EXPECT_EQ(found.value()->fingerprint_version, kVersion);
    EXPECT_NE(found.value()->flags & kPatternWaystone, 0);
    EXPECT_FALSE(found.value()->has_waystone_directory()) << "no root was passed";

    auto row = catalog_.GetSysPatternRow(0xABCDEF);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().origin, 0) << "the reserved byte is written 0";
}

TEST_F(PatternCatalogTest, AnUnknownPatternIsNotFound) {
    auto found = catalog_.FindPattern(12345);
    EXPECT_FALSE(found.ok());
    EXPECT_EQ(found.status().code(), StatusCode::kNotFound);
}

TEST_F(PatternCatalogTest, OnTwiceIsUnchangedAndInsertsOneRow) {
    ASSERT_TRUE(On(7));
    EXPECT_FALSE(On(7)) << "a second ON wrote";
    EXPECT_EQ(RowsOf(7).size(), 1u);
}

TEST_F(PatternCatalogTest, OnWritesTheRootItCarriesOnlyWhereTheRowHasNone) {
    ASSERT_TRUE(On(5));
    // Opted with no root, then a root: written, at depth 1.
    EXPECT_TRUE(On(5, 4096));
    auto row = catalog_.GetSysPatternRow(5);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().waystone_root, 4096u);
    EXPECT_EQ(row.value().dir_depth, 1);
    // A row already rooted keeps its root; the page carried is unused.
    EXPECT_FALSE(On(5, 8192));
    EXPECT_EQ(catalog_.GetSysPatternRow(5).value().waystone_root, 4096u);
}

TEST_F(PatternCatalogTest, OffClearsTheBitAndTheRootAndKeepsTheRow) {
    ASSERT_TRUE(On(9, 4096));
    EXPECT_TRUE(Off(9));
    const auto rows = RowsOf(9);
    ASSERT_EQ(rows.size(), 1u) << "OFF deleted the row";
    EXPECT_EQ(rows[0].flags & kPatternWaystone, 0);
    EXPECT_FALSE(HasWaystoneDirectory(rows[0]));
    EXPECT_FALSE(Off(9)) << "a second OFF wrote";
    EXPECT_FALSE(Off(404)) << "OFF of a pattern no row names wrote";
    // And ON again re-opts the kept row rather than inserting a second.
    EXPECT_TRUE(On(9));
    EXPECT_EQ(RowsOf(9).size(), 1u);
}

TEST_F(PatternCatalogTest, PatternOidsComeFromThePersistentSequence) {
    ASSERT_TRUE(On(1));
    ASSERT_TRUE(On(2));
    auto a = catalog_.GetSysPatternRow(1);
    auto b = catalog_.GetSysPatternRow(2);
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());

    // Distinct and monotonic, and - unlike GenerateUserOid() - persisted,
    // so a restart does not reissue them onto rows that already exist.
    EXPECT_LT(a.value().oid, b.value().oid);
    auto table_row = catalog_.GetSysTableRow(kSysPatternsTable);
    ASSERT_TRUE(table_row.ok());
    EXPECT_GT(table_row.value().next_id, b.value().oid);
}

// The opt-in is the fact a peer's pattern entry caches, so it moves the
// schema word - after the write, and only when something was written.
TEST_F(PatternCatalogTest, AnOptInMovesTheCatalogVersionAndUnchangedDoesNot) {
    const std::uint64_t before = catalog_.catalog_version();
    ASSERT_TRUE(On(31));
    const std::uint64_t after_on = catalog_.catalog_version();
    EXPECT_GT(after_on, before);
    EXPECT_FALSE(On(31));
    EXPECT_EQ(catalog_.catalog_version(), after_on) << "an unchanged ON moved the version";
    ASSERT_TRUE(Off(31));
    EXPECT_GT(catalog_.catalog_version(), after_on);
}

// ---- The version gate (the catalog half of P02) ---------------------------

TEST_F(PatternCatalogTest, AForeignVersionResolvesAsAbsentNotAsAnError) {
    WriteRawPatternRow(99, kForeignVersion);

    // The row is on the page, and the lookup still reports the pattern as
    // never seen: its fetch_id was computed under rules this build does
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

TEST_F(PatternCatalogTest, APatternStaleAtOneVersionCanBeOptedInAtTheCurrentOne) {
    WriteRawPatternRow(55, kForeignVersion, kPatternWaystone);

    // As far as this build is concerned the pattern has never been opted:
    // ON inserts a current row beside the lapsed one rather than writing
    // through it.
    ASSERT_TRUE(On(55));
    auto found = catalog_.FindPattern(55);
    ASSERT_TRUE(found.ok());
    EXPECT_EQ(found.value()->fingerprint_version, kVersion);
    EXPECT_EQ(RowsOf(55).size(), 2u);
}

TEST_F(PatternCatalogTest, AStaleRowDoesNotShadowTheCurrentOne) {
    // Both rows on the page at once, stale first - the state a version bump
    // leaves behind, since nothing rewrites the old rows. A lookup that
    // took the first match by fetch_id would return the stale one, which
    // is the defect this ordering exists to catch.
    WriteRawPatternRow(77, kForeignVersion);
    ASSERT_TRUE(On(77));

    auto found = catalog_.FindPattern(77);
    ASSERT_TRUE(found.ok());
    EXPECT_EQ(found.value()->fingerprint_version, kVersion);

    auto row = catalog_.GetSysPatternRow(77);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().fingerprint_version, kVersion);
}

// **OFF clears every row of its `fetch_id`, of any version** (§8.3 Storage,
// OFF): a `fetch_id` is not salted by `kFingerprintVersion`, so after a bump
// a lapsed opted row and a re-issued ON's current row share it. Stale first
// on the page, so a first-match OFF would clear the lapsed row and leave
// the current one recording.
TEST_F(PatternCatalogTest, OffClearsALapsedRowAndTheCurrentRowOfOneFetchId) {
    WriteRawPatternRow(88, kForeignVersion, kPatternWaystone, 4096);
    ASSERT_TRUE(On(88, 8192));
    ASSERT_EQ(RowsOf(88).size(), 2u);

    EXPECT_TRUE(Off(88));
    for (const SysPatternRow& row : RowsOf(88)) {
        EXPECT_EQ(row.flags & kPatternWaystone, 0) << "version " << row.fingerprint_version;
        EXPECT_FALSE(HasWaystoneDirectory(row)) << "version " << row.fingerprint_version;
    }
    EXPECT_FALSE(Off(88)) << "nothing was left opted";
}

// A lapsed row alone: OFF by number reaches it although no lookup does.
TEST_F(PatternCatalogTest, OffReachesALapsedRowNoLookupReturns) {
    WriteRawPatternRow(66, kForeignVersion, kPatternWaystone);
    ASSERT_FALSE(catalog_.FindPattern(66).ok());
    EXPECT_TRUE(Off(66));
    const auto rows = RowsOf(66);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].flags & kPatternWaystone, 0);
}

TEST_F(PatternCatalogTest, OffTakesNoRoot) {
    EXPECT_EQ(catalog_.SetPatternWaystone(1, 0, false, 4096).status().code(),
              StatusCode::kInvalidArgument);
}

// ---- The mount's renewal (§8.3 Mount) ---------------------------------------

TEST_F(PatternCatalogTest, TheRenewalRootsOptedRowsAndClearsEveryOtherRoot) {
    ASSERT_TRUE(On(1, 4096));   // opted, rooted at the previous run's page
    ASSERT_TRUE(On(2));         // opted, never rooted
    ASSERT_TRUE(On(3, 4096));
    ASSERT_TRUE(Off(3));        // opted out
    WriteRawPatternRow(4, kForeignVersion, kPatternWaystone, 4096);  // lapsed, rooted
    WriteRawPatternRow(5, kForeignVersion, 0, 4096);                 // other version, rooted

    CapturedLog log;
    catalog_.SetLogger(log.get());
    auto renewed = catalog_.RenewPatternDirectories(/*waystone_recording=*/true);
    ASSERT_TRUE(renewed.ok()) << renewed.status().message();
    EXPECT_EQ(renewed.value(), 2u) << "the opted current rows";

    for (const std::uint64_t fetch_id : {1u, 2u}) {
        auto row = catalog_.GetSysPatternRow(fetch_id);
        ASSERT_TRUE(row.ok());
        EXPECT_TRUE(HasWaystoneDirectory(row.value())) << fetch_id;
        EXPECT_EQ(row.value().dir_depth, 1) << fetch_id;
        EXPECT_NE(row.value().waystone_root, 4096u) << fetch_id << " kept its stale root";
        EXPECT_NE(row.value().flags & kPatternWaystone, 0) << fetch_id;
    }
    for (const std::uint64_t fetch_id : {3u, 4u, 5u}) {
        for (const SysPatternRow& row : RowsOf(fetch_id)) {
            EXPECT_FALSE(HasWaystoneDirectory(row)) << fetch_id << " kept a root";
        }
    }
    // Fetch id 4's lapse is reported, at its version; 5 was never opted.
    EXPECT_TRUE(log.Has("waystone", {"1 opt-ins lapsed at fingerprint version " +
                                     std::to_string(kForeignVersion)}));
}

// A lapse the operator already re-issued is not reported at every mount.
TEST_F(PatternCatalogTest, ARenewalReportsALapseOnlyWhileItIsUnreplaced) {
    WriteRawPatternRow(44, kForeignVersion, kPatternWaystone);
    ASSERT_TRUE(On(44));
    CapturedLog log;
    catalog_.SetLogger(log.get());
    ASSERT_TRUE(catalog_.RenewPatternDirectories(true).ok());
    EXPECT_FALSE(log.Has("waystone", {"lapsed"}));
}

// `waystone_recording` off: nothing would record into a directory, so the
// renewal clears every root and creates none, and the bits stay.
TEST_F(PatternCatalogTest, TheRenewalWithRecordingOffCreatesNoDirectory) {
    ASSERT_TRUE(On(1, 4096));
    ASSERT_TRUE(On(2));
    auto renewed = catalog_.RenewPatternDirectories(/*waystone_recording=*/false);
    ASSERT_TRUE(renewed.ok()) << renewed.status().message();
    EXPECT_EQ(renewed.value(), 2u);
    for (const std::uint64_t fetch_id : {1u, 2u}) {
        auto row = catalog_.GetSysPatternRow(fetch_id);
        ASSERT_TRUE(row.ok());
        EXPECT_FALSE(HasWaystoneDirectory(row.value())) << fetch_id;
        EXPECT_NE(row.value().flags & kPatternWaystone, 0) << fetch_id;
    }
    EXPECT_EQ(catalog_.CountOptedPatterns().value(), 2u);
}

TEST_F(PatternCatalogTest, CountOptedPatternsCountsCurrentOptedRowsOnly) {
    EXPECT_EQ(catalog_.CountOptedPatterns().value(), 0u);
    ASSERT_TRUE(On(1));
    ASSERT_TRUE(On(2));
    ASSERT_TRUE(On(3));
    ASSERT_TRUE(Off(3));
    WriteRawPatternRow(4, kForeignVersion, kPatternWaystone);
    EXPECT_EQ(catalog_.CountOptedPatterns().value(), 2u);
}

// ---- What an opt-in must not disturb ---------------------------------------

TEST_F(PatternCatalogTest, HeatIsReadFromThePageNotTheCache) {
    ASSERT_TRUE(On(21));

    // PatternAccess deliberately has no use_count/last_seen to read: heat
    // moves without DDL, so it is not a cacheable fact (schema.hpp). The
    // row is where they live, reserved and 0 (rows.hpp).
    auto row = catalog_.GetSysPatternRow(21);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().use_count, 0u);
    EXPECT_EQ(row.value().last_seen, 0u);
}

// ---- Key order (docs/spec/heap-and-tuple.md §4.1) -----------------------------
//
// There is no key *mode* to test any more - `CreateTable` takes no such
// parameter and refuses no storage pairing for one. What replaced it is an
// **observation**: every relation starts ascending, and since BB-R3 (on
// BB-Q8 (b)) stays there - an id below its high-water mark is refused on
// every relation, where a btree used to admit one and flip. So these tests
// are about what `AdmitExplicitRowId` admits and what it refuses.

class RowIdAdmissionTest : public ::testing::Test {
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

TEST_F(RowIdAdmissionTest, EveryNewRelationWritesTheRetiredKeyOrderByteZero) {
    // BB-R10: the byte `KeyOrder` held stays at its offset, written 0 by
    // every row, because the mount reads it for one thing - a relation still
    // marked with the deleted `kUnordered` (BB-R11). Read back after a
    // reopen, off the page.
    Oid heap_oid = 0;
    Oid btree_oid = 0;
    {
        Catalog catalog(store_, storage::kDefaultInlineCellWidth);
        ASSERT_TRUE(catalog.Bootstrap().ok());

        auto h = catalog.CreateTable(kNamespacePublic, "chained", OneColumnSchema());
        ASSERT_TRUE(h.ok()) << h.status().message();
        heap_oid = h.value();

        // The pairing that was refused until 2026-08-25 is now unremarkable:
        // nothing about naming keys is declared at CREATE at all.
        auto b = catalog.CreateTable(kNamespacePublic, "clustered", OneColumnSchema());
        ASSERT_TRUE(b.ok()) << b.status().message();
        btree_oid = b.value();
    }

    Catalog reopened(store_, storage::kDefaultInlineCellWidth);

    auto h_row = reopened.GetSysTableRow(heap_oid);
    ASSERT_TRUE(h_row.ok()) << h_row.status().message();
    EXPECT_EQ(h_row.value().retired_key_order, 0u);

    auto b_row = reopened.GetSysTableRow(btree_oid);
    ASSERT_TRUE(b_row.ok()) << b_row.status().message();
    EXPECT_EQ(b_row.value().retired_key_order, 0u);
}

TEST_F(RowIdAdmissionTest, ABtreeAdmissionAdvancesTheMarkOrLeavesItAndNeverRefuses) {
    // BD-R6/BD-R7 (`instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`):
    // a btree leaf places a key where it sorts, so its admission is
    // advance-or-nothing - a key at or above the mark moves it past the key,
    // one below leaves it where it is, and neither is refused. The heap's
    // refusal is `AHeapRelationTakesASuppliedKeyAtOrAboveTheMark`'s.
    Catalog catalog(store_, storage::kDefaultInlineCellWidth);
    ASSERT_TRUE(catalog.Bootstrap().ok());

    auto oid = catalog.CreateTable(kNamespacePublic, "clustered", OneColumnSchema());
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    ASSERT_TRUE(catalog.AdmitExplicitRowId(oid.value(), 600).ok());
    // Below the mark, in a gap and twice over: admitted, and the mark stays.
    EXPECT_TRUE(catalog.AdmitExplicitRowId(oid.value(), 550).ok());
    EXPECT_TRUE(catalog.AdmitExplicitRowId(oid.value(), 549).ok());

    auto row = catalog.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().next_id, 601u) << "a below-mark id must not walk the mark backwards";
    auto issued = catalog.AllocateRowId(oid.value());
    ASSERT_TRUE(issued.ok()) << issued.status().message();
    EXPECT_EQ(issued.value(), 601u);
}

TEST_F(RowIdAdmissionTest, AnIssuedIdRisesAboveEverySuppliedOne) {
    // The two id sources share one mark, which is what keeps them from
    // colliding: AllocateRowId used to refuse an explicit relation outright,
    // and now both run on every relation.
    Catalog catalog(store_, storage::kDefaultInlineCellWidth);
    ASSERT_TRUE(catalog.Bootstrap().ok());

    auto oid = catalog.CreateTable(kNamespacePublic, "mixed", OneColumnSchema());
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    ASSERT_TRUE(catalog.AdmitExplicitRowId(oid.value(), 900).ok());
    auto issued = catalog.AllocateRowId(oid.value());
    ASSERT_TRUE(issued.ok()) << issued.status().message();
    EXPECT_EQ(issued.value(), 901u);

    // And the reverse order: an issued id moves the mark a supplied one is
    // then measured against.
    ASSERT_TRUE(catalog.AdmitExplicitRowId(oid.value(), 902).ok());
    auto next = catalog.AllocateRowId(oid.value());
    ASSERT_TRUE(next.ok()) << next.status().message();
    EXPECT_EQ(next.value(), 903u);
}

TEST_F(RowIdAdmissionTest, ACatalogRelationWritesTheRetiredKeyOrderByteZero) {
    // Not a tautology worth skipping: sys.tables rows for the bootstrap
    // relations go through InsertRelationRow too, and a bootstrap row the
    // mount found marked would refuse every volume.
    Catalog catalog(store_, storage::kDefaultInlineCellWidth);
    ASSERT_TRUE(catalog.Bootstrap().ok());

    auto row = catalog.GetSysTableRow(kSysTablesTable);
    ASSERT_TRUE(row.ok()) << row.status().message();
    EXPECT_EQ(row.value().retired_key_order, 0u);
}

// ---- The catalog relations chain (docs/rules/keystoneid-k0-findings.md) --------
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
                                       WideSchema(20));
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

TEST(CatalogChain, TheChainIsALinkedListOfOwnedPages) {
    storage::InMemoryPageStore store{server::kFirstUserPageId};
    Catalog catalog(store, storage::kDefaultInlineCellWidth);
    ASSERT_TRUE(catalog.Bootstrap().ok());

    for (int i = 0; i < 12; ++i) {
        ASSERT_TRUE(catalog.CreateTable(kNamespacePublic, "wide" + std::to_string(i),
                                        WideSchema(20))
                        .ok());
    }

    // Follow sys.columns' links: more than one page, the chain ends, and
    // every page after the root is an allocated heap page from the free
    // map's general supply - no id below the first user page, since the
    // reserved overflow range went (`crosscore.md` CC12/CR3).
    //
    // A growth page says whose it is (page.md §2a), as a user relation's
    // does through `heap::ChainInsert`: it carries the catalog relation's
    // oid, and the root keeps bootstrap's 0. The `sys.access_stats` discard
    // tells its own pages from another relation's by exactly this
    // (access_stats_test.cpp).
    PageId at = kCatalogPageColumns;
    int pages = 0;
    while (at != kInvalidPageId) {
        ++pages;
        if (at != kCatalogPageColumns) {
            EXPECT_GE(at, server::kFirstUserPageId) << "page " << at << " has a reserved id";
        }
        auto bytes = store.GetForRead(at);
        ASSERT_TRUE(bytes.ok()) << "page " << at << " is not allocated";
        EXPECT_EQ(storage::RawPageType(bytes.value().bytes()),
                  static_cast<std::uint8_t>(PageType::kHeap))
            << "page " << at;
        EXPECT_EQ(storage::GetOwnerOid(bytes.value().bytes()),
                  at == kCatalogPageColumns ? 0u : kSysColumnsTable)
            << "page " << at;
        at = heap::PageView(bytes.value().bytes()).next_page_id();
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
                                       WideSchema(1));
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
                                        WideSchema(1))
                        .ok());
    }
    auto oid = catalog.FindTableOidByName(::kds::catalog::kNamespacePublic, "t59");
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

// ---- Transactional DDL, DT2: the stamp reaches the rows -----------------

// Reads every live tuple's trx_id off one catalog page chain.
std::vector<std::uint64_t> StampsOn(storage::PageStore& store, PageId root) {
    std::vector<std::uint64_t> stamps;
    (void)heap::ChainVisit(
        store, root, storage::PageAccess::kRead,
        [&](PageId, heap::PageView& page,
            std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            auto tuple = page.ReadTuple(slot);
            if (tuple.ok()) stamps.push_back(tuple.value().trx_id);
            return storage::VisitControl::kContinue;
        });
    return stamps;
}

TEST_F(CatalogTest, CreateTableStampsItsRowsWithTheTransactionIdItWasGiven) {
    // DT2's whole content: the id a caller supplies reaches the rows.
    // Nothing *reads* it yet - catalog scans do not filter by visibility
    // until DT3 - so this is a seam test, and the suite passing unchanged
    // beside it is the other half of the claim.
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    constexpr std::uint64_t kDdlTrx = 4242;
    auto oid = catalog_.CreateTable(kNamespacePublic, "stamped", MinimalTupleKeySchema(), kDdlTrx);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    // All three of a relation's rows carry the same stamp: a reader that
    // could see the table row but not its columns would see a relation
    // with no schema.
    for (PageId page : {kCatalogPageObjects, kCatalogPageTables, kCatalogPageColumns}) {
        const auto stamps = StampsOn(store_, page);
        EXPECT_NE(std::find(stamps.begin(), stamps.end(), kDdlTrx), stamps.end())
            << "no row on catalog page " << page << " carries the supplied id";
    }
}

TEST_F(CatalogTest, ADefaultedCreateTableStillStampsBootstrapAndBootstrapRowsAlwaysDo) {
    // The other half of DT2, and the one that would break quietly: every
    // caller that does not pass an id - bootstrap, recovery, every test -
    // must still get kBootstrapXid, because those rows have to stay
    // visible to a read view minted before any transaction existed
    // (ddl-transactional.md §3).
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto oid = catalog_.CreateTable(kNamespacePublic, "unstamped", MinimalTupleKeySchema());
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    for (PageId page : {kCatalogPageObjects, kCatalogPageTables, kCatalogPageColumns}) {
        const auto stamps = StampsOn(store_, page);
        ASSERT_FALSE(stamps.empty());
        for (std::uint64_t stamp : stamps) {
            EXPECT_EQ(stamp, kBootstrapXid)
                << "a row on catalog page " << page << " left the bootstrap stamp";
        }
    }
}

// ---- DT3: a catalog read answers what the reader's view can see --------

TEST_F(CatalogTest, ARelationCreatedByAnUnseenTransactionDoesNotExistForThatReader) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    constexpr std::uint64_t kCreator = 500;
    auto oid = catalog_.CreateTable(kNamespacePublic, "pending", MinimalTupleKeySchema(), kCreator);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    // A concurrent reader: the creating transaction is live, so the window
    // holds no commit for it and everything it wrote is invisible.
    txn::InstanceVisibility vis;
    txn::ReadView other;
    other.visibility = &vis;
    other.snapshot_lsn = 1000;

    EXPECT_EQ(catalog_.FindTableOidByName(::kds::catalog::kNamespacePublic, "pending", &other).status().code(),
              StatusCode::kNotFound)
        << "a reader saw a relation whose creating transaction it cannot see";

    auto listed = catalog_.ListTables(&other);
    ASSERT_TRUE(listed.ok());
    for (const SysObjectRow& row : listed.value()) {
        EXPECT_NE(NameView(row.name), "pending") << "the invisible relation was listed";
    }
    // ...and the bootstrap relations are still there, because kBootstrapXid
    // is visible to every view forever (ddl-transactional.md §3). A
    // filter that hid those would pass the assertion above and be useless.
    EXPECT_FALSE(listed.value().empty()) << "the filter hid the bootstrap catalog too";

    // The creating transaction itself sees its own work, uncommitted.
    txn::ReadView own;
    own.visibility = &vis;
    own.snapshot_lsn = 1000;
    own.own_trx_id = kCreator;
    auto mine = catalog_.FindTableOidByName(::kds::catalog::kNamespacePublic, "pending", &own);
    ASSERT_TRUE(mine.ok()) << "a transaction cannot see its own CREATE TABLE";
    EXPECT_EQ(mine.value(), oid.value());

    // And an internal read - bootstrap, recovery, anything with no reader -
    // still sees everything, which is every pre-DT3 caller.
    auto unfiltered = catalog_.FindTableOidByName(::kds::catalog::kNamespacePublic, "pending");
    ASSERT_TRUE(unfiltered.ok());
    EXPECT_EQ(unfiltered.value(), oid.value());
}

TEST_F(CatalogTest, ATransactionalLookupNeitherReadsNorFillsTheSharedCache) {
    // The half that would break quietly: the cache is one map for the whole
    // instance and knows nothing about who is asking, so a filtered lookup
    // that filled it would publish an uncommitted relation to every later
    // reader - and one that read it would be served that publication.
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    constexpr std::uint64_t kCreator = 700;
    ASSERT_TRUE(catalog_.CreateTable(kNamespacePublic, "half_open", MinimalTupleKeySchema(), kCreator)
                    .ok());

    txn::InstanceVisibility vis;
    txn::ReadView own;
    own.visibility = &vis;
    own.snapshot_lsn = 1000;
    own.own_trx_id = kCreator;
    ASSERT_TRUE(catalog_.FindTableOidByName(::kds::catalog::kNamespacePublic, "half_open", &own).ok());  // creator sees it

    // Nothing about that lookup may have leaked into the shared cache.
    txn::ReadView other;
    other.visibility = &vis;
    other.snapshot_lsn = 1000;
    EXPECT_EQ(catalog_.FindTableOidByName(::kds::catalog::kNamespacePublic, "half_open", &other).status().code(),
              StatusCode::kNotFound)
        << "the creator's lookup published its uncommitted relation through the cache";
}

// ---- DT3a: a rolled-back CREATE TABLE leaves no relation ---------------

TEST_F(CatalogTest, ARolledBackCreateTableLeavesNoRelationEvenToALaterReader) {
    // **The view is not what makes this work, and that is the point.**
    // `ReadView::Visible` answers "committed" for anything below the
    // instance's floor - it has no notion of "aborted" there - so a reader
    // whose floor has passed the creating id sees it as committed. The
    // engine hides aborted work by compensation, so DDL has to put its
    // rows on the trail like every other write
    // (ddl-transactional.md §2's correction).
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    server::SuperBlock sb = server::SuperBlock::CreateFresh(/*now_unix_seconds=*/0);
    txn::TrxIdSequence ids(sb);
    txn::UndoLog undo(store_, /*wal=*/nullptr);
    txn::TransactionManager mgr(ids, undo, store_, /*wal=*/nullptr);

    auto txn = mgr.Begin(txn::IsolationLevel::kReadCommitted);
    ASSERT_TRUE(txn.ok()) << txn.status().message();

    std::vector<CatalogRowRef> written;
    auto oid = catalog_.CreateTable(kNamespacePublic, "doomed", MinimalTupleKeySchema(),
                                    txn.value()->id(), &written);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    // sys.objects + sys.tables + one column.
    ASSERT_EQ(written.size(), 3u) << "not every catalog row was reported";

    for (const CatalogRowRef& row : written) {
        mgr.NoteInsert(*txn.value(), /*rel_oid=*/0, row.page_id, row.slot, row.oid);
    }
    ASSERT_TRUE(mgr.Abort(*txn.value()).ok());

    // A reader whose floor is above the creating id: it considers that id
    // committed, and must still not find the relation - because the rows
    // are gone, not because they are hidden.
    txn::InstanceVisibility resolved;
    resolved.PublishBounds(/*core=*/0, txn::kUnboundedBound, /*cursor=*/1u << 20);
    resolved.Reclaim();
    ASSERT_GT(resolved.Floor(), txn.value()->id());
    txn::ReadView later;
    later.visibility = &resolved;
    EXPECT_EQ(catalog_.FindTableOidByName(::kds::catalog::kNamespacePublic, "doomed", &later).status().code(),
              StatusCode::kNotFound)
        << "the rolled-back relation survived its own rollback";

    // And an unfiltered read - the one that sees literally everything -
    // agrees, which is what proves the rows were retired rather than
    // merely filtered.
    EXPECT_EQ(catalog_.FindTableOidByName(::kds::catalog::kNamespacePublic, "doomed").status().code(), StatusCode::kNotFound)
        << "the rows are still on the page; the rollback only hid them";
}

// ---- AZ-S1: a row placed and then not reported ----------------------------
//
// `InsertRow` places a row, fires the DDL undo hook, then logs the row's
// `HEAP_INSERT`. A failure between placement and the record left the row on
// the page with `where` unset, so the caller had nothing to register and the
// rollback nothing to retire: a live catalog row for a DDL that failed
// (`workorder-az-ay-carry-forward.md` item 2).

// One page of a catalog chain as the slot directory records it.
struct PageShape {
    PageId page_id = kInvalidPageId;
    std::uint16_t slots = 0;
    std::uint16_t live = 0;
    std::uint16_t lower = 0;
    std::uint16_t upper = 0;
    bool operator==(const PageShape&) const = default;
};

std::vector<PageShape> ChainShape(storage::PageStore& store, PageId root) {
    std::vector<PageShape> out;
    for (PageId at = root; at != kInvalidPageId;) {
        auto bytes = store.Get(at);
        EXPECT_TRUE(bytes.ok()) << bytes.status().message();
        if (!bytes.ok()) break;
        heap::PageView page(bytes.value().bytes());
        PageShape shape{at, page.slot_count(), 0, page.lower(), page.upper()};
        for (std::uint16_t i = 0; i < shape.slots; ++i) {
            auto info = page.DebugSlotInfo(i);
            if (info.ok() && !info.value().dead) ++shape.live;
        }
        out.push_back(shape);
        at = page.next_page_id();
    }
    return out;
}

TEST_F(CatalogTest, AnUndoHookThatFailsOnTheTailLeavesTheChainAsItWas) {
    // The first catalog write a create makes is its `sys.objects` row, and
    // after bootstrap that chain's tail has room: the tail arm.
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    const std::vector<PageShape> before = ChainShape(store_, kCatalogPageObjects);
    ASSERT_FALSE(before.empty());

    PageId hooked = kInvalidPageId;
    catalog_.SetDdlUndoHook([&](const Catalog::DdlUndoEvent& event) {
        hooked = event.page_id;
        return Status::IoError("injected: the undo record could not be appended");
    });
    std::vector<CatalogRowRef> written;
    auto created = catalog_.CreateTable(kNamespacePublic, "unhooked", MinimalTupleKeySchema(), /*trx_id=*/7, &written);
    catalog_.SetDdlUndoHook(nullptr);

    ASSERT_EQ(created.status().code(), StatusCode::kIoError) << created.status().message();
    ASSERT_EQ(hooked, before.back().page_id) << "the cell meant the tail arm";
    EXPECT_TRUE(written.empty());
    // Nothing was logged and nothing is reported, so the page is as it was:
    // not a dead slot, which a later logged insert on this page would sit
    // past - a record redo cannot apply to an image the dead slot never
    // reached (`heap_page.cpp`'s dense-slot refusal).
    EXPECT_EQ(ChainShape(store_, kCatalogPageObjects), before)
        << "the row the failed create placed is still on the page";
    EXPECT_EQ(catalog_.FindTableOidByName(::kds::catalog::kNamespacePublic, "unhooked").status().code(), StatusCode::kNotFound);
}

TEST_F(CatalogTest, AnUndoHookThatFailsOnANewPageLeavesItEmptyUnlinkedAndUnreported) {
    // The new-page arm, through `CreateIndex`: the caller whose `where` is
    // read even when the create fails (`CREATE INDEX`'s `created_row`,
    // command_dispatcher.cpp), where `CreateTable` reports only the rows
    // that succeeded. Indexes are created until `sys.indexes` grows, and
    // the hook refuses the first event on a page its chain did not have.
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto chain_pages = [&] {
        std::set<PageId> pages;
        for (const PageShape& page : ChainShape(store_, kCatalogPageIndexes)) {
            pages.insert(page.page_id);
        }
        return pages;
    };

    std::set<PageId> known;
    PageId fresh = kInvalidPageId;
    CatalogRowRef where;
    Status last = Status::OK();
    for (int i = 0; i < 1024 && fresh == kInvalidPageId; ++i) {
        auto table = catalog_.CreateTable(kNamespacePublic, "t" + std::to_string(i),
                                          IndexableSchema());
        ASSERT_TRUE(table.ok()) << table.status().message();
        known = chain_pages();
        catalog_.SetDdlUndoHook([&](const Catalog::DdlUndoEvent& event) {
            if (known.count(event.page_id) != 0) return Status::OK();
            fresh = event.page_id;
            return Status::IoError("injected: the undo record could not be appended");
        });
        where = CatalogRowRef{};
        last = catalog_.CreateIndex(SimpleIndex(table.value(), "i" + std::to_string(i), 1),
                                    /*trx_id=*/7, &where)
                   .status();
        catalog_.SetDdlUndoHook(nullptr);
    }

    ASSERT_NE(fresh, kInvalidPageId) << "sys.indexes never grew; the cell tested nothing";
    EXPECT_EQ(last.code(), StatusCode::kIoError) << last.message();
    EXPECT_EQ(where.page_id, kInvalidPageId) << "a row the failed create took back is reported";
    EXPECT_EQ(chain_pages(), known) << "the new page was linked into the chain";
    auto bytes = store_.Get(fresh);
    ASSERT_TRUE(bytes.ok()) << bytes.status().message();
    EXPECT_EQ(heap::PageView(bytes.value().bytes()).slot_count(), 0)
        << "the row the failed create placed is still on the new page";
}

TEST_F(CatalogTest, ARowWhoseRecordCannotBeAppendedIsRetiredWhereItLies) {
    // The log refuses after the undo hook has succeeded: the transaction's
    // undo holds an insert naming the slot, so the slot is retired rather
    // than taken back - a retired slot is never handed out again, so
    // recovery's undo meets it dead ("nothing to retire", recovery_undo.cpp)
    // and never another row.
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto device = wal::MemoryLogDevice::Create(/*segment_size=*/1u << 20);
    ASSERT_TRUE(device.ok()) << device.status().message();
    sched::ManualClock clock;
    wal::WalManagerConfig config;
    config.ring_capacity = wal::kMinRingCapacity;
    auto opened = wal::WalManager::Open(device.value().get(), clock, /*core_id=*/0, config);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    wal::WalManager& log = *opened.value();
    catalog_.SetWal(&log);

    const std::vector<PageShape> before = ChainShape(store_, kCatalogPageObjects);
    ASSERT_FALSE(before.empty());

    // At the hook, the ring is filled to less than one smallest record and
    // the device made to refuse: the row's append then finds no room, and
    // the drain it forces fails. Each fill ends at a refused drain, which
    // spends that injection - hence one per fill and one for the row.
    bool armed = false;
    catalog_.SetDdlUndoHook([&](const Catalog::DdlUndoEvent&) {
        if (armed) return Status::OK();
        armed = true;
        for (const std::size_t size : {std::size_t{1024}, std::size_t{1}}) {
            device.value()->FailNextWrite(Status::IoError("injected: the drain failed"));
            const std::vector<std::byte> filler(size, std::byte{0x5a});
            Status fill = Status::OK();
            for (int i = 0; i < 1 << 16 && fill.ok(); ++i) {
                fill = log.Append({wal::RecordType::kHeapInsert, wal::kNoTxnId, kInvalidPageId, 0},
                                  filler)
                           .status();
            }
            EXPECT_EQ(fill.code(), StatusCode::kIoError) << "the ring never filled";
        }
        device.value()->FailNextWrite(Status::IoError("injected: the row's drain failed"));
        return Status::OK();
    });
    std::vector<CatalogRowRef> written;
    auto created = catalog_.CreateTable(kNamespacePublic, "unlogged", MinimalTupleKeySchema(), /*trx_id=*/7, &written);
    catalog_.SetDdlUndoHook(nullptr);
    catalog_.SetWal(nullptr);

    ASSERT_TRUE(armed);
    ASSERT_EQ(created.status().code(), StatusCode::kIoError) << created.status().message();
    EXPECT_TRUE(written.empty());
    const std::vector<PageShape> after = ChainShape(store_, kCatalogPageObjects);
    ASSERT_EQ(after.size(), before.size());
    EXPECT_EQ(after.back().slots, before.back().slots + 1) << "the slot was taken back";
    EXPECT_EQ(after.back().live, before.back().live)
        << "the row the failed create placed is still live on the page";
    EXPECT_EQ(catalog_.FindTableOidByName(::kds::catalog::kNamespacePublic, "unlogged").status().code(), StatusCode::kNotFound);
}

}  // namespace
}  // namespace kds::catalog
