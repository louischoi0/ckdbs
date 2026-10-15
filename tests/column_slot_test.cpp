#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/tagged_cell.hpp"
#include "kds/txn/manager.hpp"

// BW-S1 (`instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`):
// a relation's reserved 16-byte column slots. `CREATE TABLE ... SPARE n`
// fixes the row size with them (BW-R2), a vacant slot is NULL in every row
// (BW-R3), and a relation with none lays out exactly as before.

namespace kds {
namespace {

using catalog::RowLayout;
using catalog::Schema;
using catalog::SysColumnRow;

constexpr std::uint32_t kW = storage::kDefaultInlineCellWidth;

SysColumnRow Col(std::uint32_t pos, std::string_view name, std::uint32_t type_val,
                 bool notnull = true, std::uint32_t len = 0) {
    SysColumnRow col{};
    col.pos = pos;
    catalog::SetName(col.name, name);
    col.type_val = type_val;
    col.len = len;
    col.notnull = notnull;
    return col;
}

SysColumnRow SlotCol(std::uint32_t pos, std::string_view name, std::uint32_t type_val,
                     std::uint32_t len = 0) {
    SysColumnRow col = Col(pos, name, type_val, /*notnull=*/false, len);
    col.flags = SysColumnRow::kColumnInSlot;
    return col;
}

RowLayout LayoutOf(const Schema& schema) {
    auto layout = RowLayout::Build(schema, kW);
    EXPECT_TRUE(layout.ok()) << layout.status().message();
    return layout.ok() ? layout.value() : RowLayout{};
}

parser::AstValue Int(std::int64_t n) {
    parser::AstValue v{};
    v.type = parser::ValueType::kInt;
    v.int_val = n;
    v.raw_int_text = std::to_string(n);
    return v;
}

parser::AstValue Null() {
    parser::AstValue v{};
    v.type = parser::ValueType::kNull;
    return v;
}

// ---- The layout (BW-R2) ----------------------------------------------------

TEST(ColumnSlotLayoutTest, NoSlotsIsByteIdenticalToTheLayoutBeforeSlots) {
    Schema schema;
    schema.columns = {Col(0, "id", catalog::kTypeValInt64), Col(1, "a", catalog::kTypeValInt32),
                      Col(2, "b", catalog::kTypeValInt16, /*notnull=*/false)};
    const RowLayout layout = LayoutOf(schema);
    EXPECT_EQ(layout.row_size, kKeystoneWordSize + 4 + 2 + 1);
    EXPECT_EQ(layout.vacant_slot_bit_begin, layout.vacant_slot_bit_end);
}

TEST(ColumnSlotLayoutTest, SlotsSitBetweenTheDeclaredColumnsAndTheBitmap) {
    Schema schema;
    schema.spare_slots = 3;
    schema.columns = {Col(0, "id", catalog::kTypeValInt64),
                      Col(1, "a", catalog::kTypeValInt32, /*notnull=*/false),
                      Col(2, "b", catalog::kTypeValInt16),
                      SlotCol(3, "c", catalog::kTypeValInt64)};
    const RowLayout layout = LayoutOf(schema);

    const std::uint32_t declared_end = kKeystoneWordSize + 4 + 2;
    ASSERT_EQ(layout.offsets.size(), 4u);
    EXPECT_EQ(layout.offsets[3], declared_end);  // slot 0
    // One declared nullable bit plus a bit per slot, held or not.
    EXPECT_EQ(layout.null_bitmap_bytes, 1u);
    EXPECT_EQ(layout.row_size, declared_end + 3 * catalog::kColumnSlotWidth + 1);
    EXPECT_EQ(layout.null_bits[1], 0u);
    EXPECT_EQ(layout.null_bits[2], catalog::kNoNullBit);
    EXPECT_EQ(layout.null_bits[3], 1u);
    // Slots 1 and 2 are vacant: bits 2 and 3.
    EXPECT_EQ(layout.vacant_slot_bit_begin, 2u);
    EXPECT_EQ(layout.vacant_slot_bit_end, 4u);
}

TEST(ColumnSlotLayoutTest, TakingASlotMovesNeitherTheRowSizeNorAnyOffset) {
    Schema before;
    before.spare_slots = 2;
    before.columns = {Col(0, "id", catalog::kTypeValInt64), Col(1, "a", catalog::kTypeValVarchar)};
    Schema after = before;
    after.columns.push_back(SlotCol(2, "c", catalog::kTypeValVarchar, 16));
    Schema dropped = after;
    dropped.columns[1].flags = SysColumnRow::kColumnDropped;

    const RowLayout a = LayoutOf(before);
    const RowLayout b = LayoutOf(after);
    const RowLayout c = LayoutOf(dropped);
    EXPECT_EQ(a.row_size, b.row_size);
    EXPECT_EQ(b.row_size, c.row_size);
    EXPECT_EQ(a.offsets[1], b.offsets[1]);
    EXPECT_EQ(b.offsets, c.offsets);
    EXPECT_EQ(b.null_bits, c.null_bits);
    EXPECT_EQ(a.null_bitmap_bytes, b.null_bitmap_bytes);
}

TEST(ColumnSlotLayoutTest, ASlotColumnWiderThanASlotIsCorruption) {
    Schema schema;
    schema.spare_slots = 1;
    schema.columns = {Col(0, "id", catalog::kTypeValInt64),
                      SlotCol(1, "s", catalog::kTypeValVarchar, 64)};
    auto layout = RowLayout::Build(schema, kW);
    ASSERT_FALSE(layout.ok());
    EXPECT_EQ(layout.status().code(), StatusCode::kCorruption);
}

TEST(ColumnSlotLayoutTest, MoreSlotColumnsThanSlotsIsCorruption) {
    Schema schema;
    schema.spare_slots = 1;
    schema.columns = {Col(0, "id", catalog::kTypeValInt64), SlotCol(1, "a", catalog::kTypeValInt8),
                      SlotCol(2, "b", catalog::kTypeValInt8)};
    auto layout = RowLayout::Build(schema, kW);
    ASSERT_FALSE(layout.ok());
    EXPECT_EQ(layout.status().code(), StatusCode::kCorruption);
}

TEST(ColumnSlotLayoutTest, ADeclaredColumnAfterASlotColumnIsCorruption) {
    Schema schema;
    schema.spare_slots = 1;
    schema.columns = {Col(0, "id", catalog::kTypeValInt64), SlotCol(1, "a", catalog::kTypeValInt8),
                      Col(2, "b", catalog::kTypeValInt8)};
    auto layout = RowLayout::Build(schema, kW);
    ASSERT_FALSE(layout.ok());
    EXPECT_EQ(layout.status().code(), StatusCode::kCorruption);
}

TEST(ColumnSlotLayoutTest, EveryCellIsItsColumnsWidthNotTheGapToTheNextColumn) {
    // The last declared column is followed by the slot region, and a slot
    // column by vacant slots: a cell sized "to the next offset or the
    // bitmap" would take those bytes, so a char would accept a longer value
    // and a varchar would inline past its width - and both would read back
    // differently once an `ADD` placed a column after them.
    Schema schema;
    schema.spare_slots = 3;
    schema.columns = {Col(0, "id", catalog::kTypeValInt64),
                      Col(1, "s", catalog::kTypeValVarchar),
                      SlotCol(2, "c", catalog::kTypeValChar, 4)};
    const RowLayout layout = LayoutOf(schema);
    ASSERT_EQ(layout.cell_ends.size(), 3u);
    EXPECT_EQ(layout.cell_ends[0], kKeystoneWordSize);
    EXPECT_EQ(layout.cell_ends[1], layout.offsets[1] + kW);
    EXPECT_EQ(layout.cell_ends[2], layout.offsets[2] + 4);

    // A value past the varchar's own inline capacity must spill (and with no
    // var-heap sink, be refused), never inline into the slot region.
    const std::string long_value(storage::InlineCapacity(kW) + 1, 'x');
    parser::AstValue str{};
    str.type = parser::ValueType::kStr;
    str.str_val = long_value;
    auto spilled = exec::EncodeRow(schema, layout, 1, {str, Null()});
    ASSERT_FALSE(spilled.ok());
    EXPECT_EQ(spilled.status().code(), StatusCode::kUnsupported) << spilled.status().message();

    // A char(4) in a slot takes four bytes, not the slot's sixteen.
    parser::AstValue five{};
    five.type = parser::ValueType::kStr;
    five.str_val = "abcde";
    parser::AstValue short_str{};
    short_str.type = parser::ValueType::kStr;
    short_str.str_val = "a";
    auto wide = exec::EncodeRow(schema, layout, 2, {short_str, five});
    ASSERT_FALSE(wide.ok());
    EXPECT_EQ(wide.status().code(), StatusCode::kInvalidArgument) << wide.status().message();
}

// ---- A vacant slot is NULL in every row (BW-R3, [quiet-wrong]) ------------

TEST(ColumnSlotEncodeTest, EveryVacantSlotsBitIsSetAndAHeldSlotsFollowsItsValue) {
    Schema schema;
    schema.spare_slots = 3;
    schema.columns = {Col(0, "id", catalog::kTypeValInt64), Col(1, "a", catalog::kTypeValInt32),
                      SlotCol(2, "c", catalog::kTypeValInt64)};
    const RowLayout layout = LayoutOf(schema);

    auto present = exec::EncodeRow(schema, layout, 1, {Int(5), Int(9)});
    ASSERT_TRUE(present.ok()) << present.status().message();
    // The held slot (bit 0) is present; the two vacant ones (bits 1, 2) are NULL.
    const std::uint8_t bits = std::to_integer<std::uint8_t>(present.value().back());
    EXPECT_EQ(bits, 0b110u);

    auto null = exec::EncodeRow(schema, layout, 2, {Int(5), Null()});
    ASSERT_TRUE(null.ok()) << null.status().message();
    EXPECT_EQ(std::to_integer<std::uint8_t>(null.value().back()), 0b111u);
}

TEST(ColumnSlotEncodeTest, AnUpdateCarriesTheVacantBits) {
    Schema schema;
    schema.spare_slots = 2;
    schema.columns = {Col(0, "id", catalog::kTypeValInt64), Col(1, "a", catalog::kTypeValInt32)};
    const RowLayout layout = LayoutOf(schema);

    auto row = exec::EncodeRow(schema, layout, 1, {Int(5)});
    ASSERT_TRUE(row.ok());
    const parser::AstValue seven = Int(7);
    const exec::AssignedCell cells[] = {{1, &seven}};
    auto updated = exec::EncodeAssigned(schema, layout, 1, row.value(), cells);
    ASSERT_TRUE(updated.ok()) << updated.status().message();
    EXPECT_EQ(std::to_integer<std::uint8_t>(updated.value().back()), 0b11u);
}

// ---- The grammar (BW-R1) ---------------------------------------------------

TEST(ColumnSlotParseTest, SpareTakesACountAndSitsBesideTheStorageWord) {
    for (const char* sql : {"CREATE TABLE t (id int64) SPARE 4",
                            "CREATE TABLE t (id int64) BTREE SPARE 4",
                            "CREATE TABLE t (id int64) spare 4 BTREE"}) {
        auto parsed = parser::Parse(sql);
        ASSERT_TRUE(parsed.ok()) << sql << ": " << parsed.status().message();
        EXPECT_EQ(std::get<parser::CreateTableStmt>(parsed.value()).spare_slots, 4u) << sql;
    }
    auto none = parser::Parse("CREATE TABLE t (id int64)");
    ASSERT_TRUE(none.ok());
    EXPECT_EQ(std::get<parser::CreateTableStmt>(none.value()).spare_slots, 0u);
}

TEST(ColumnSlotParseTest, AnOutOfRangeRepeatedOrMissingCountIsRefusedWithItsByte) {
    auto wide = parser::Parse("CREATE TABLE t (id int64) SPARE 65");
    ASSERT_FALSE(wide.ok());
    EXPECT_EQ(wide.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(wide.status().message().find("byte 32"), std::string::npos)
        << wide.status().message();

    auto twice = parser::Parse("CREATE TABLE t (id int64) SPARE 1 SPARE 2");
    ASSERT_FALSE(twice.ok());
    EXPECT_NE(twice.status().message().find("byte 34"), std::string::npos)
        << twice.status().message();

    for (const char* sql : {"CREATE TABLE t (id int64) SPARE", "CREATE TABLE t (id int64) SPARE x",
                            "CREATE TABLE t (id int64) SPARE -1"}) {
        auto bad = parser::Parse(sql);
        ASSERT_FALSE(bad.ok()) << sql;
        EXPECT_EQ(bad.status().code(), StatusCode::kInvalidArgument) << sql;
    }
}

// ---- End to end ------------------------------------------------------------

class ColumnSlotDispatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
        cabins_.emplace();
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, &*cabins_, &*mgr_);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    void Ok(const std::string& sql) {
        const std::string out = Run(sql);
        EXPECT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::CabinStore> cabins_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<server::CommandDispatcher> dispatcher_;
};

TEST_F(ColumnSlotDispatchTest, SpareIsRecordedAndShownOnSysTables) {
    Ok("CREATE TABLE t (id int64, v int32) SPARE 4");
    Ok("CREATE TABLE u (id int64, v int32)");
    EXPECT_EQ(Run("SELECT name, spare_slots, spare_free FROM sys.tables WHERE name = 't'"),
              "name,spare_slots,spare_free\\nt,4,4");
    EXPECT_EQ(Run("SELECT name, spare_slots, spare_free FROM sys.tables WHERE name = 'u'"),
              "name,spare_slots,spare_free\\nu,0,0");

    // Rows go in and come out as they would with no slots.
    Ok("INSERT INTO t VALUES (7)");
    EXPECT_EQ(Run("SELECT * FROM t"), "id,v\\n1,7");

    const auto oid = boot_->catalog.FindTableOidByName("t");
    ASSERT_TRUE(oid.ok());
    auto access = boot_->catalog.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    EXPECT_EQ(access.value()->schema.spare_slots, 4u);
    EXPECT_EQ(access.value()->layout.row_size,
              kKeystoneWordSize + 4 + 4 * catalog::kColumnSlotWidth + 1);
    // BW-R10: the var-heap chain exists for an `ADD` of a varchar(16) to use.
    EXPECT_NE(access.value()->varheap_page_id, kInvalidPageId);
}

}  // namespace
}  // namespace kds
