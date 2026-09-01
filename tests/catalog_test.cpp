#include "kds/catalog/catalog.hpp"

#include "kds/storage/anchor_page.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "kds/catalog/well_known.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/txn/manager.hpp"

namespace kds::catalog {
namespace {

class CatalogTest : public ::testing::Test {
protected:
    storage::InMemoryPageStore store_{server_first_new_page_id_};
    Catalog catalog_{store_};

    // Matches kds::server::kFirstUserPageId (128) so freshly-created user
    // tables never collide with the fixed catalog pages (4-15).
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

// RD1 (instructions/v2.4.0/range-foundation.md §5 RA2): sys.ranges exists
// from bootstrap and is empty. Empty is the whole product at this version
// - the row format is RD2's - so what this pins is the relation's
// existence on its fixed root and the emptiness every path relies on: a
// relation with no rows here is one range owned by sys.tables.owner_core
// (docs/spec/crosscore.md CC9).
TEST_F(CatalogTest, SysRangesExistsAndIsEmptyAtBootstrap) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    auto oid = catalog_.FindTableOidByName("ranges");
    ASSERT_TRUE(oid.ok());
    EXPECT_EQ(oid.value(), kSysRangesTable);

    auto row = catalog_.GetSysTableRow(kSysRangesTable);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(NameView(row.value().name), "ranges");
    EXPECT_EQ(row.value().desc_page_id, kCatalogPageRanges);

    // Empty means empty on the device, not just unread: no tuple on the
    // root, and no chain ever grew from it.
    auto bytes = store_.Get(kCatalogPageRanges);
    ASSERT_TRUE(bytes.ok());
    heap::PageView page(bytes.value().bytes());
    EXPECT_EQ(page.slot_count(), 0u);
    EXPECT_EQ(page.next_page_id(), kInvalidPageId);
}

// ---- RD2: the directory row, and the door its two rules live at --------

TEST_F(CatalogTest, ARangeRowRoundTripsThroughItsCodec) {
    SysRangeRow row{};
    row.range_id = 77;
    row.lo = 0x00FF'FFFF'FFFFull;  // a 40-bit Keystone id at its ceiling
    row.rel_oid = 4000;
    row.owner_core = 3;
    row.entry_page = 987;

    const auto bytes = row.Encode();
    auto back = SysRangeRow::Decode(bytes);
    ASSERT_TRUE(back.ok()) << back.status().message();
    EXPECT_EQ(back.value().range_id, row.range_id);
    EXPECT_EQ(back.value().lo, row.lo);
    EXPECT_EQ(back.value().rel_oid, row.rel_oid);
    EXPECT_EQ(back.value().owner_core, row.owner_core);
    EXPECT_EQ(back.value().entry_page, row.entry_page);

    // Short bytes are refused rather than read past, like every other row.
    EXPECT_FALSE(SysRangeRow::Decode(std::span(bytes).first(bytes.size() - 1)).ok());
}

TEST_F(CatalogTest, AnUnsplitRelationHasNoRangeRowsAndThatIsNotAnError) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    // The ordinary answer for every relation in this engine today: empty,
    // ok. A relation with no rows here is one range owned by
    // `sys.tables.owner_core`, and an empty *error* would make the unsplit
    // path handle a failure that never happens.
    auto ranges = catalog_.RangesOf(4000);
    ASSERT_TRUE(ranges.ok()) << ranges.status().message();
    EXPECT_TRUE(ranges.value().empty());
}

TEST_F(CatalogTest, TheFirstRangeRowMustDescribeTheWholeIdSpace) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    // CC9's rule 1, refused at the door. Opening a directory at a boundary
    // above 0 publishes a partition with a hole below it, and every later
    // insert inherits the hole.
    SysRangeRow above{};
    above.rel_oid = 4000;
    above.lo = 4096;
    above.owner_core = 1;
    above.entry_page = 500;
    Status refused = catalog_.InsertRangeRow(above);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(refused.message().find("lo = 0"), std::string::npos) << refused.message();

    // And nothing landed - a refused insert is not a partial one.
    auto after = catalog_.RangesOf(4000);
    ASSERT_TRUE(after.ok());
    EXPECT_TRUE(after.value().empty());

    SysRangeRow first{};
    first.rel_oid = 4000;
    first.lo = 0;
    first.owner_core = 0;
    first.entry_page = 400;
    ASSERT_TRUE(catalog_.InsertRangeRow(first).ok());
    ASSERT_TRUE(catalog_.InsertRangeRow(above).ok()) << "the second row was refused";
}

TEST_F(CatalogTest, RangeRowsComeBackInLoOrderWhateverOrderTheyWereWritten) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    // Page order is insertion order and neither is `lo` order, which is
    // what RD7 concatenates in and what makes `hi` derivable.
    const std::uint64_t los[] = {0, 8192, 4096, 12288};
    for (std::size_t i = 0; i < 4; ++i) {
        SysRangeRow row{};
        row.rel_oid = 4000;
        row.lo = los[i];
        row.owner_core = static_cast<std::uint32_t>(i);
        row.entry_page = static_cast<PageId>(400 + i);
        ASSERT_TRUE(catalog_.InsertRangeRow(row).ok()) << "row " << i;
    }
    // A second relation's rows share the heap and must not appear.
    SysRangeRow other{};
    other.rel_oid = 4001;
    other.lo = 0;
    other.owner_core = 2;
    other.entry_page = 600;
    ASSERT_TRUE(catalog_.InsertRangeRow(other).ok());

    auto ranges = catalog_.RangesOf(4000);
    ASSERT_TRUE(ranges.ok()) << ranges.status().message();
    ASSERT_EQ(ranges.value().size(), 4u);
    EXPECT_EQ(ranges.value()[0].lo, 0u);
    EXPECT_EQ(ranges.value()[1].lo, 4096u);
    EXPECT_EQ(ranges.value()[2].lo, 8192u);
    EXPECT_EQ(ranges.value()[3].lo, 12288u);

    auto theirs = catalog_.RangesOf(4001);
    ASSERT_TRUE(theirs.ok());
    ASSERT_EQ(theirs.value().size(), 1u);
    EXPECT_EQ(theirs.value()[0].owner_core, 2u);
}

TEST_F(CatalogTest, TwoRangesMayNotClaimOneBoundary) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    SysRangeRow first{};
    first.rel_oid = 4000;
    first.lo = 0;
    first.owner_core = 0;
    first.entry_page = 400;
    ASSERT_TRUE(catalog_.InsertRangeRow(first).ok());

    SysRangeRow dup = first;
    dup.owner_core = 1;
    dup.entry_page = 401;
    Status refused = catalog_.InsertRangeRow(dup);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), StatusCode::kInvalidArgument);

    // The same boundary on *another* relation is a different range and is
    // admitted - the rule is per relation, not per id.
    SysRangeRow elsewhere = first;
    elsewhere.rel_oid = 4001;
    EXPECT_TRUE(catalog_.InsertRangeRow(elsewhere).ok());
}

TEST_F(CatalogTest, ADirectoryWithNoLoZeroRowIsCorruptionAndNotAnAnswer) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    // **The set the door refuses on the read side, reached the only way it
    // can be**: past `InsertRangeRow`, which refuses to create it. It is
    // not hypothetical - a directory whose `lo = 0` row and whose later
    // boundaries were written by different transactions can be left in
    // exactly this shape by a mount that undoes only some of them, which is
    // the residue RB2 owns (`Catalog::InsertRangeRow`'s note).
    //
    // The bytes go straight onto page 15, which is what makes this a test
    // of the reader rather than of the writer.
    SysRangeRow orphan{};
    orphan.range_id = 1;
    orphan.rel_oid = 4000;
    orphan.lo = 4096;
    orphan.owner_core = 1;
    orphan.entry_page = 500;
    {
        auto bytes = store_.Get(kCatalogPageRanges);
        ASSERT_TRUE(bytes.ok());
        heap::PageView page(bytes.value().bytes());
        const auto encoded = orphan.Encode();
        ASSERT_TRUE(page.InsertTuple(encoded, kBootstrapXid).ok());
    }

    auto refused = catalog_.RangesOf(4000);
    EXPECT_FALSE(refused.ok()) << "a directory with a hole under it was served";
    EXPECT_EQ(refused.status().code(), StatusCode::kCorruption);
    EXPECT_NE(refused.status().message().find("lo = 0"), std::string::npos)
        << refused.status().message();

    // And it is that relation's directory that is refused, not the page:
    // another relation's rows on the same heap still resolve.
    SysRangeRow ok_row{};
    ok_row.rel_oid = 4001;
    ok_row.lo = 0;
    ok_row.owner_core = 0;
    ok_row.entry_page = 600;
    ASSERT_TRUE(catalog_.InsertRangeRow(ok_row).ok());
    auto healthy = catalog_.RangesOf(4001);
    ASSERT_TRUE(healthy.ok()) << healthy.status().message();
    EXPECT_EQ(healthy.value().size(), 1u);
}

TEST_F(CatalogTest, ARangeBoundaryAboveTheKeystoneCeilingIsRefused) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    // `lo` is a Keystone id (invariant 7). A boundary above the 40-bit
    // ceiling is one no id could ever fall in, so the range it opens could
    // never hold a row.
    SysRangeRow first{};
    first.rel_oid = 4000;
    first.lo = 0;
    first.owner_core = 0;
    first.entry_page = 400;
    ASSERT_TRUE(catalog_.InsertRangeRow(first).ok());

    SysRangeRow beyond{};
    beyond.rel_oid = 4000;
    beyond.lo = kMaxKeystoneId + 1;
    beyond.owner_core = 1;
    beyond.entry_page = 401;
    Status refused = catalog_.InsertRangeRow(beyond);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), StatusCode::kInvalidArgument);

    // The ceiling itself is a legal boundary, so the refusal is off-by-one
    // free.
    beyond.lo = kMaxKeystoneId;
    EXPECT_TRUE(catalog_.InsertRangeRow(beyond).ok());
}

TEST_F(CatalogTest, TheReaderRefusesAnAboveCeilingBoundaryTheWriterNeverWrote) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    // The same ceiling on the **read** side, and it is RD3 that made it
    // load-bearing rather than symmetric: `hi` is derived as the next row's
    // `lo` with `kIdSpaceEnd` on the last, so a stored boundary above the
    // space derives a `RangeTarget` with `lo > hi` - a value that struct
    // says cannot exist, and one RB3 would read an `entry_page` off. The
    // writer's refusal above is not enough, because these bytes can reach
    // page 15 without passing it.
    SysRangeRow first{};
    first.rel_oid = 4000;
    first.lo = 0;
    first.owner_core = 0;
    first.entry_page = 400;
    ASSERT_TRUE(catalog_.InsertRangeRow(first).ok());

    SysRangeRow beyond{};
    beyond.range_id = 99;
    beyond.rel_oid = 4000;
    beyond.lo = kMaxKeystoneId + 4;
    beyond.owner_core = 1;
    beyond.entry_page = 401;
    {
        auto bytes = store_.Get(kCatalogPageRanges);
        ASSERT_TRUE(bytes.ok());
        heap::PageView page(bytes.value().bytes());
        const auto encoded = beyond.Encode();
        ASSERT_TRUE(page.InsertTuple(encoded, kBootstrapXid).ok());
    }

    auto refused = catalog_.RangesOf(4000);
    EXPECT_FALSE(refused.ok()) << "an inverted range was served";
    EXPECT_EQ(refused.status().code(), StatusCode::kCorruption);
}

TEST_F(CatalogTest, EveryRangeRowCarriesAnIdOfItsOwnInTheIdentitySlot) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    // `CatalogRowRef`'s contract: every catalog row carries its oid in the
    // first eight bytes, which is where `Compensate` re-reads it. `lo` was
    // in that slot in this row's first draft and is **not unique** - every
    // split relation's opening row is at 0 - so the ids are what keep two
    // relations' opening rows distinguishable there.
    SysRangeRow a{};
    a.rel_oid = 4000;
    a.lo = 0;
    a.owner_core = 0;
    a.entry_page = 400;
    ASSERT_TRUE(catalog_.InsertRangeRow(a).ok());

    SysRangeRow b = a;
    b.rel_oid = 4001;
    b.entry_page = 401;
    ASSERT_TRUE(catalog_.InsertRangeRow(b).ok());

    auto first = catalog_.RangesOf(4000);
    auto second = catalog_.RangesOf(4001);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(first.value().size(), 1u);
    ASSERT_EQ(second.value().size(), 1u);
    EXPECT_NE(first.value()[0].range_id, 0u) << "the row was written with no id";
    EXPECT_NE(first.value()[0].range_id, second.value()[0].range_id)
        << "two rows at lo = 0 share an identity";
}

TEST_F(CatalogTest, DroppingARelationRetiresItsRangeRowsAndNoOthers) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    auto doomed =
        catalog_.CreateTable(kNamespacePublic, "doomed", MinimalPkSchema(), ClusteredType::kHeap);
    ASSERT_TRUE(doomed.ok()) << doomed.status().message();

    SysRangeRow mine{};
    mine.rel_oid = doomed.value();
    mine.lo = 0;
    mine.owner_core = 0;
    mine.entry_page = 400;
    ASSERT_TRUE(catalog_.InsertRangeRow(mine).ok());
    SysRangeRow second = mine;
    second.lo = 4096;
    second.owner_core = 1;
    second.entry_page = 401;
    ASSERT_TRUE(catalog_.InsertRangeRow(second).ok());

    SysRangeRow survivor{};
    survivor.rel_oid = doomed.value() + 1;
    survivor.lo = 0;
    survivor.owner_core = 2;
    survivor.entry_page = 500;
    ASSERT_TRUE(catalog_.InsertRangeRow(survivor).ok());

    std::vector<std::uint64_t> dropped_cabins;
    ASSERT_TRUE(catalog_.DropTable(doomed.value(), dropped_cabins).ok());

    auto gone = catalog_.RangesOf(doomed.value());
    ASSERT_TRUE(gone.ok()) << gone.status().message();
    EXPECT_TRUE(gone.value().empty()) << "a dropped relation kept its ranges";

    auto kept = catalog_.RangesOf(survivor.rel_oid);
    ASSERT_TRUE(kept.ok());
    EXPECT_EQ(kept.value().size(), 1u) << "the sweep took another relation's range";
}

// ---- AF-T0: a user namespace is not the system namespace ----------------
//
// `instructions/v2.8.0/ratification-af-namespace.md` AF-P3. Three DDL
// refusals asked `namespace_oid != kNamespacePublic`, which is true of
// **every** namespace but `public` - so the first relation created outside
// it would have been undroppable and unrenamable, refused by a message
// calling it a system relation. Both cells below fail on that spelling and
// pass on `IsSystemNamespace`.
//
// No syntax is needed to reach the case: `CreateTable` has always taken
// the namespace as its first argument, and only the dispatcher's two call
// sites hard-code `kNamespacePublic`.
constexpr Oid kAfTestNamespace = kUserOidStart + 1;

TEST_F(CatalogTest, ARelationInAUserNamespaceIsRenamableAndDroppable) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    auto oid = catalog_.CreateTable(kAfTestNamespace, "orders_line", MinimalPkSchema(),
                                    ClusteredType::kHeap);
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

TEST_F(CatalogTest, ACachedAccessCarriesTheRelationsRangesAndTheyRepublishOnInsert) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    auto oid =
        catalog_.CreateTable(kNamespacePublic, "split_me", MinimalPkSchema(), ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    // The ordinary answer for every relation this engine has today, and
    // the branch RD3's zero-cost invariant is read from.
    auto unsplit = catalog_.InitTableAccess(oid.value());
    ASSERT_TRUE(unsplit.ok()) << unsplit.status().message();
    EXPECT_TRUE(unsplit.value()->ranges.empty());

    SysRangeRow first{};
    first.rel_oid = oid.value();
    first.lo = 0;
    first.owner_core = unsplit.value()->owner_core;
    first.entry_page = unsplit.value()->desc_page_id;
    ASSERT_TRUE(catalog_.InsertRangeRow(first).ok());
    SysRangeRow second = first;
    second.lo = 4096;
    second.owner_core = 1;
    second.entry_page = 900;
    ASSERT_TRUE(catalog_.InsertRangeRow(second).ok());

    // Publication (§2b): `InsertRangeRow` ends in `BumpVersion`, so the
    // entry filled above is gone and the next fill carries the boundary.
    // A cached routing answer that survived this is the bug that section
    // exists to prevent.
    auto split = catalog_.InitTableAccess(oid.value());
    ASSERT_TRUE(split.ok()) << split.status().message();
    ASSERT_EQ(split.value()->ranges.size(), 2u);
    EXPECT_EQ(split.value()->ranges[0].hi, 4096u);
    EXPECT_EQ(split.value()->ranges[1].hi, kIdSpaceEnd);

    // And the resolver reads that vector, which is the storage §2c's shape
    // argument is about (`range_directory.hpp`).
    auto routed = ResolveRanges(split.value()->ranges, PkSpan::Equality(5000));
    ASSERT_TRUE(routed.ok()) << routed.status().message();
    ASSERT_EQ(routed.value().size(), 1u);
    EXPECT_EQ(routed.value()[0].owner_core, 1u);
    EXPECT_EQ(routed.value()[0].entry_page, 900u);
}

TEST_F(CatalogTest, ARelationWhoseDirectoryIsTornCannotBeOpenedAtAll) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    auto torn =
        catalog_.CreateTable(kNamespacePublic, "torn", MinimalPkSchema(), ClusteredType::kHeap);
    ASSERT_TRUE(torn.ok()) << torn.status().message();
    auto healthy =
        catalog_.CreateTable(kNamespacePublic, "healthy", MinimalPkSchema(), ClusteredType::kHeap);
    ASSERT_TRUE(healthy.ok()) << healthy.status().message();

    // The shape `InsertRangeRow` refuses to create and a partial mount-time
    // undo can still leave (its own note): rows, and no `lo = 0` row.
    // Written straight onto page 15, past the writer's door, because that
    // door is shut against it.
    SysRangeRow orphan{};
    orphan.range_id = 1;
    orphan.rel_oid = torn.value();
    orphan.lo = 4096;
    orphan.owner_core = 1;
    orphan.entry_page = 401;
    {
        auto bytes = store_.Get(kCatalogPageRanges);
        ASSERT_TRUE(bytes.ok());
        heap::PageView page(bytes.value().bytes());
        const auto encoded = orphan.Encode();
        ASSERT_TRUE(page.InsertTuple(encoded, kBootstrapXid).ok());
    }

    // Fatal to opening *that* relation: an empty `ranges` means "one range
    // at owner_core" by CC9, so serving the fill would route every
    // statement to the lower range's chain head and answer from there - a
    // wrong answer with nothing logged.
    auto refused = catalog_.InitTableAccess(torn.value());
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kCorruption);

    // And scoped to it. Reading the whole directory as one unit would have
    // made one relation's torn rows an unusable instance.
    auto fine = catalog_.InitTableAccess(healthy.value());
    EXPECT_TRUE(fine.ok()) << fine.status().message();
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

    ASSERT_TRUE(catalog_.UpdateRelationDescPage(oid.value(), old_desc + 1000,
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

TEST_F(CatalogTest, UpdateRelationDescPagePreservesRowIdentity) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());

    Schema schema = MinimalPkSchema();
    auto oid = catalog_.CreateTable(kNamespacePublic, "movable", schema, ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok());

    auto before = catalog_.GetSysTableRow(oid.value());
    ASSERT_TRUE(before.ok());
    PageId old_desc = before.value().desc_page_id;

    Status s = catalog_.UpdateRelationDescPage(oid.value(), old_desc + 1000,
                                               before.value().anchor_page_id);
    ASSERT_TRUE(s.ok()) << s.message();

    auto after = catalog_.GetSysTableRow(oid.value());
    ASSERT_TRUE(after.ok());
    // PW2-3: the row is CREATE-fixed - the move landed in the anchor, and
    // a fresh fill resolves it from there.
    EXPECT_EQ(after.value().desc_page_id, old_desc);
    EXPECT_EQ(NameView(after.value().name), "movable");
    {
        auto anchor = store_.GetForRead(after.value().anchor_page_id);
        ASSERT_TRUE(anchor.ok());
        EXPECT_EQ(storage::AnchorClusteredRoot(anchor.value().bytes()), old_desc + 1000);
    }
}

// ---- Secondary indexes (docs/spec/index.md §12, workplan IX03) ----------

// Three columns, so an index can be declared on something that is neither
// the primary key nor the only other column.
Schema IndexableSchema() {
    Schema schema = MinimalPkSchema();
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

    auto table_a = catalog_.CreateTable(kNamespacePublic, "a", IndexableSchema(),
                                        ClusteredType::kBtree);
    ASSERT_TRUE(table_a.ok()) << table_a.status().message();
    auto table_b = catalog_.CreateTable(kNamespacePublic, "b", IndexableSchema(),
                                        ClusteredType::kBtree);
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
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema(),
                                      ClusteredType::kBtree);
    ASSERT_TRUE(table.ok());

    Catalog::IndexDef def = SimpleIndex(table.value(), "composite", 1);
    def.key_cols = {1, 2};
    ASSERT_TRUE(catalog_.CreateIndex(def).ok());

    EXPECT_TRUE(catalog_.FindIndexOnColumn(table.value(), 1).ok());
    EXPECT_FALSE(catalog_.FindIndexOnColumn(table.value(), 2).ok());
}

TEST_F(CatalogTest, AHeapClusteredRelationTakesNoIndex) {
    // Spec IX3: an entry resolves through the primary key, which a heap
    // relation has no index for - so every probe would be a chain scan.
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto table = catalog_.CreateTable(kNamespacePublic, "h", IndexableSchema(),
                                      ClusteredType::kHeap);
    ASSERT_TRUE(table.ok());

    auto created = catalog_.CreateIndex(SimpleIndex(table.value(), "h_owner", 1));
    EXPECT_FALSE(created.ok());
    EXPECT_NE(created.status().message().find("heap-clustered"), std::string::npos);
}

TEST_F(CatalogTest, CreateIndexRefusesRatherThanTruncatingOrGuessing) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema(),
                                      ClusteredType::kBtree);
    ASSERT_TRUE(table.ok());

    // The primary key: the clustered tree already indexes it.
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

    // UNIQUE (spec IX11): v1 is a read accelerator that cannot fail a write
    // for a reason of its own.
    Catalog::IndexDef unique = SimpleIndex(table.value(), "unique", 1);
    unique.flags = kIndexFlagUnique;
    auto refused = catalog_.CreateIndex(unique);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kNotImplemented);

    // A name already in use.
    ASSERT_TRUE(catalog_.CreateIndex(SimpleIndex(table.value(), "taken", 1)).ok());
    auto again = catalog_.CreateIndex(SimpleIndex(table.value(), "taken", 2));
    EXPECT_FALSE(again.ok());
    EXPECT_EQ(again.status().code(), StatusCode::kAlreadyExists);
}

TEST_F(CatalogTest, ADroppedIndexIsRetiredSoTheNameIsFreeAgain) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema(),
                                      ClusteredType::kBtree);
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
    // records it - the counterpart of UpdateRelationDescPage, but *not* the
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
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema(),
                                      ClusteredType::kBtree);
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
    ASSERT_TRUE(catalog_.UpdateIndexRoot(table.value(), oid.value(), 4242,
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
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema(),
                                      ClusteredType::kBtree);
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
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema(),
                                      ClusteredType::kBtree);
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
    // Bit 0 is always clear: CreateIndex refuses the primary key.
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
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema(),
                                      ClusteredType::kBtree);
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
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema(),
                                      ClusteredType::kBtree);
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
    ASSERT_TRUE(catalog_.UpdateIndexRoot(table.value(), oid.value().index_oid, 7777,
                                         after.value()->anchor_page_id)
                    .ok());

    auto relinked = catalog_.InitTableAccess(table.value());
    ASSERT_TRUE(relinked.ok());
    ASSERT_EQ(relinked.value()->indexes.size(), 1u);
    EXPECT_EQ(relinked.value()->indexes[0].root_page_id, 7777u);
}

TEST_F(CatalogTest, ADroppedIndexLeavesTheRelationWithNoneAgain) {
    ASSERT_TRUE(catalog_.Bootstrap().ok());
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema(),
                                      ClusteredType::kBtree);
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
    auto a = catalog_.CreateTable(kNamespacePublic, "a", IndexableSchema(),
                                  ClusteredType::kBtree);
    auto b = catalog_.CreateTable(kNamespacePublic, "b", IndexableSchema(),
                                  ClusteredType::kBtree);
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
    auto table = catalog_.CreateTable(kNamespacePublic, "t", IndexableSchema(),
                                      ClusteredType::kBtree);
    ASSERT_TRUE(table.ok());

    const std::uint64_t before_create = catalog_.catalog_version();
    auto oid = catalog_.CreateIndex(SimpleIndex(table.value(), "ix", 1));
    ASSERT_TRUE(oid.ok());
    const std::uint64_t after_create = catalog_.catalog_version();
    EXPECT_GT(after_create, before_create);

    ASSERT_TRUE(catalog_.DropIndex(oid.value()).ok());
    EXPECT_GT(catalog_.catalog_version(), after_create);
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

    auto vrow = catalog_.GetSysTableRow(oid.value());
    ASSERT_TRUE(vrow.ok());
    ASSERT_TRUE(
        catalog_.UpdateRelationDescPage(oid.value(), 9999, vrow.value().anchor_page_id).ok());
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

// ---- sys.patterns (docs/spec/waystone-concpets.md section 4) -------------------

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
        kds::heap::PageView page(bytes.value().bytes());

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

// ---- Relation ownership (docs/inflight/in-progress/workplan-crosscore.md M1) ---------------

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

// ---- Key order (docs/spec/heap-and-tuple.md §4.1) -----------------------------
//
// There is no key *mode* to test any more - `CreateTable` takes no such
// parameter and refuses no storage pairing for one. What replaced it is an
// **observation**: every relation starts ascending and stays there until an
// id is admitted below its high-water mark, which only a btree relation can
// do. So these tests are about what `AdmitExplicitRowId` admits, what it
// refuses, and what the flag says afterwards.

using KeyOrderTest = OwnerCoreTest;

TEST_F(KeyOrderTest, EveryNewRelationStartsAscendingAndSurvivesAReopen) {
    Oid heap_oid = 0;
    Oid btree_oid = 0;
    {
        Catalog catalog(store_, storage::kDefaultInlineCellWidth, /*core_count=*/1);
        ASSERT_TRUE(catalog.Bootstrap().ok());

        auto h = catalog.CreateTable(kNamespacePublic, "chained", OneColumnSchema(),
                                      ClusteredType::kHeap);
        ASSERT_TRUE(h.ok()) << h.status().message();
        heap_oid = h.value();

        // The pairing that was refused until 2026-08-25 is now unremarkable:
        // nothing about naming keys is declared at CREATE at all.
        auto b = catalog.CreateTable(kNamespacePublic, "clustered", OneColumnSchema(),
                                      ClusteredType::kBtree);
        ASSERT_TRUE(b.ok()) << b.status().message();
        btree_oid = b.value();
    }

    Catalog reopened(store_, storage::kDefaultInlineCellWidth, /*core_count=*/1);

    auto h_row = reopened.GetSysTableRow(heap_oid);
    ASSERT_TRUE(h_row.ok()) << h_row.status().message();
    EXPECT_EQ(h_row.value().key_order, KeyOrder::kAscending);

    auto b_row = reopened.GetSysTableRow(btree_oid);
    ASSERT_TRUE(b_row.ok()) << b_row.status().message();
    EXPECT_EQ(b_row.value().key_order, KeyOrder::kAscending);
}

TEST_F(KeyOrderTest, AHeapRelationTakesASuppliedKeyAtOrAboveTheMark) {
    // The capability the removal bought: a heap-clustered relation may be
    // told its keys. What it may not be told is a key that goes backwards -
    // its chain's tail append, its page-wise ordering and its tail-page-only
    // duplicate check are all the ascent (§3.1b), and the mark is the ascent
    // written as one number.
    Catalog catalog(store_, storage::kDefaultInlineCellWidth, /*core_count=*/1);
    ASSERT_TRUE(catalog.Bootstrap().ok());

    auto oid = catalog.CreateTable(kNamespacePublic, "chained", OneColumnSchema(),
                                    ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    ASSERT_TRUE(catalog.AdmitExplicitRowId(oid.value(), 500).ok());
    ASSERT_TRUE(catalog.AdmitExplicitRowId(oid.value(), 600).ok());

    auto row = catalog.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().next_id, 601u);
    // Never unordered, whatever it was told: the refusal below is what keeps
    // that true, so the flag is a consequence rather than a second rule.
    EXPECT_EQ(row.value().key_order, KeyOrder::kAscending);

    auto refused = catalog.AdmitExplicitRowId(oid.value(), 550);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), StatusCode::kOutOfRange);
    EXPECT_NE(refused.message().find("must ascend"), std::string::npos) << refused.message();

    // And the refusal wrote nothing: a refused id burns no mark.
    auto after = catalog.GetSysTableRow(oid.value());
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value().next_id, 601u);
    EXPECT_EQ(after.value().key_order, KeyOrder::kAscending);
}

TEST_F(KeyOrderTest, ABtreeRelationTakesABelowMarkKeyAndTurnsUnordered) {
    Catalog catalog(store_, storage::kDefaultInlineCellWidth, /*core_count=*/1);
    ASSERT_TRUE(catalog.Bootstrap().ok());

    auto oid = catalog.CreateTable(kNamespacePublic, "clustered", OneColumnSchema(),
                                    ClusteredType::kBtree);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    ASSERT_TRUE(catalog.AdmitExplicitRowId(oid.value(), 600).ok());
    {
        auto row = catalog.GetSysTableRow(oid.value());
        ASSERT_TRUE(row.ok());
        EXPECT_EQ(row.value().key_order, KeyOrder::kAscending) << "600 was above the mark";
    }

    // Below the mark: admitted, because the descent - not this function -
    // proves the key unused. The mark does not move; the flag does.
    ASSERT_TRUE(catalog.AdmitExplicitRowId(oid.value(), 550).ok());
    auto row = catalog.GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().next_id, 601u) << "a below-mark id must not walk the mark backwards";
    EXPECT_EQ(row.value().key_order, KeyOrder::kUnordered);

    // A second below-mark id changes nothing - the flip is guarded, so a
    // backfill of old ids does not write the catalog page once per row.
    ASSERT_TRUE(catalog.AdmitExplicitRowId(oid.value(), 549).ok());
    auto again = catalog.GetSysTableRow(oid.value());
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.value().key_order, KeyOrder::kUnordered);
    EXPECT_EQ(again.value().next_id, 601u);
}

TEST_F(KeyOrderTest, ATableAccessCarriesTheOrderAndIsInvalidatedByTheFlip) {
    // The compiler reads it from here and never from sys.tables. A cache
    // left saying kAscending after the flip would let an ORDER BY <pk> be
    // discarded on a relation whose pages are no longer in key order - a
    // wrong answer, which is why the flip bumps the catalog version.
    Catalog catalog(store_, storage::kDefaultInlineCellWidth, /*core_count=*/1);
    ASSERT_TRUE(catalog.Bootstrap().ok());

    auto oid = catalog.CreateTable(kNamespacePublic, "clustered", OneColumnSchema(),
                                    ClusteredType::kBtree);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    ASSERT_TRUE(catalog.AdmitExplicitRowId(oid.value(), 600).ok());
    {
        auto access = catalog.InitTableAccess(oid.value());
        ASSERT_TRUE(access.ok()) << access.status().message();
        EXPECT_EQ(access.value()->key_order, KeyOrder::kAscending);
    }

    ASSERT_TRUE(catalog.AdmitExplicitRowId(oid.value(), 550).ok());
    auto access = catalog.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok()) << access.status().message();
    EXPECT_EQ(access.value()->key_order, KeyOrder::kUnordered)
        << "the cached access outlived the flip that made it wrong";
}

TEST_F(KeyOrderTest, AnIssuedIdRisesAboveEverySuppliedOne) {
    // The two id sources share one mark, which is what keeps them from
    // colliding: AllocateRowId used to refuse an explicit relation outright,
    // and now both run on every relation.
    Catalog catalog(store_, storage::kDefaultInlineCellWidth, /*core_count=*/1);
    ASSERT_TRUE(catalog.Bootstrap().ok());

    auto oid = catalog.CreateTable(kNamespacePublic, "mixed", OneColumnSchema(),
                                    ClusteredType::kHeap);
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

TEST_F(KeyOrderTest, ACatalogRelationStartsAscending) {
    // Not a tautology worth skipping: sys.tables rows for the bootstrap
    // relations go through InsertRelationRow, which sets the field rather
    // than taking it, so this is the check that it sets what it claims.
    Catalog catalog(store_, storage::kDefaultInlineCellWidth, /*core_count=*/1);
    ASSERT_TRUE(catalog.Bootstrap().ok());

    auto row = catalog.GetSysTableRow(kSysTablesTable);
    ASSERT_TRUE(row.ok()) << row.status().message();
    EXPECT_EQ(row.value().key_order, KeyOrder::kAscending);
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
    auto oid = catalog_.CreateTable(kNamespacePublic, "stamped", MinimalPkSchema(),
                                    ClusteredType::kHeap, kDdlTrx);
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
    auto oid = catalog_.CreateTable(kNamespacePublic, "unstamped", MinimalPkSchema(),
                                    ClusteredType::kHeap);
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
    auto oid = catalog_.CreateTable(kNamespacePublic, "pending", MinimalPkSchema(),
                                    ClusteredType::kHeap, kCreator);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    // A concurrent reader: the creating transaction is live, so it is in
    // this view's in-flight set and everything it wrote is invisible.
    txn::ReadView other;
    other.up_to_trx_id = 1000;
    ASSERT_TRUE(other.AddInFlight(kCreator).ok());

    EXPECT_EQ(catalog_.FindTableOidByName("pending", &other).status().code(),
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
    own.up_to_trx_id = 1000;
    own.own_trx_id = kCreator;
    ASSERT_TRUE(own.AddInFlight(kCreator).ok());
    auto mine = catalog_.FindTableOidByName("pending", &own);
    ASSERT_TRUE(mine.ok()) << "a transaction cannot see its own CREATE TABLE";
    EXPECT_EQ(mine.value(), oid.value());

    // And an internal read - bootstrap, recovery, anything with no reader -
    // still sees everything, which is every pre-DT3 caller.
    auto unfiltered = catalog_.FindTableOidByName("pending");
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
    ASSERT_TRUE(catalog_.CreateTable(kNamespacePublic, "half_open", MinimalPkSchema(),
                                     ClusteredType::kHeap, kCreator)
                    .ok());

    txn::ReadView own;
    own.up_to_trx_id = 1000;
    own.own_trx_id = kCreator;
    ASSERT_TRUE(own.AddInFlight(kCreator).ok());
    ASSERT_TRUE(catalog_.FindTableOidByName("half_open", &own).ok());  // creator sees it

    // Nothing about that lookup may have leaked into the shared cache.
    txn::ReadView other;
    other.up_to_trx_id = 1000;
    ASSERT_TRUE(other.AddInFlight(kCreator).ok());
    EXPECT_EQ(catalog_.FindTableOidByName("half_open", &other).status().code(),
              StatusCode::kNotFound)
        << "the creator's lookup published its uncommitted relation through the cache";
}

// ---- DT3a: a rolled-back CREATE TABLE leaves no relation ---------------

TEST_F(CatalogTest, ARolledBackCreateTableLeavesNoRelationEvenToALaterReader) {
    // **The view is not what makes this work, and that is the point.**
    // `ReadView::Visible` answers "below the high-water mark and not
    // in-flight" - it has no notion of "aborted" - so a reader minted
    // *after* the rollback would see the creating id as committed. The
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
    auto oid = catalog_.CreateTable(kNamespacePublic, "doomed", MinimalPkSchema(),
                                    ClusteredType::kHeap,
                                    txn.value()->id(), &written);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    // sys.objects + sys.tables + one column.
    ASSERT_EQ(written.size(), 3u) << "not every catalog row was reported";

    for (const CatalogRowRef& row : written) {
        mgr.NoteInsert(*txn.value(), /*rel_oid=*/0, row.page_id, row.slot, row.oid);
    }
    ASSERT_TRUE(mgr.Abort(*txn.value()).ok());

    // A reader minted after the abort: it considers the creating id
    // committed, and must still not find the relation - because the rows
    // are gone, not because they are hidden.
    txn::ReadView later;
    later.up_to_trx_id = 1u << 20;
    EXPECT_EQ(catalog_.FindTableOidByName("doomed", &later).status().code(),
              StatusCode::kNotFound)
        << "the rolled-back relation survived its own rollback";

    // And an unfiltered read - the one that sees literally everything -
    // agrees, which is what proves the rows were retired rather than
    // merely filtered.
    EXPECT_EQ(catalog_.FindTableOidByName("doomed").status().code(), StatusCode::kNotFound)
        << "the rows are still on the page; the rollback only hid them";
}

}  // namespace
}  // namespace kds::catalog
