#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "kds/catalog/schema.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/tagged_cell.hpp"

// catalog::RowLayout - invariant 13 made computable. A relation's row size
// is a schema constant, and these are the cases where "computable from the
// schema alone" has to be defended: a column whose width nobody has decided
// (float/decimal), and a schema whose row could never fit a page.
//
// Both refusals happen at CREATE TABLE rather than at the first INSERT,
// which is the same argument CheckKeystoneColumn() already makes: a
// relation no row can be written to is not a relation worth creating.

namespace kds::catalog {
namespace {

constexpr std::uint32_t kW = storage::kDefaultInlineCellWidth;

SysColumnRow Col(std::uint32_t pos, std::string_view name, std::uint32_t type_val,
                 std::uint32_t len = 0) {
    SysColumnRow col{};
    col.pos = pos;
    SetName(col.name, name);
    col.type_val = type_val;
    col.len = len;
    col.notnull = true;
    return col;
}

Schema SchemaOf(std::initializer_list<SysColumnRow> columns) {
    Schema schema;
    for (const SysColumnRow& col : columns) schema.columns.push_back(col);
    return schema;
}

// ---- The constant --------------------------------------------------------

TEST(RowLayoutTest, RowSizeIsTheSumOfFixedColumnWidths) {
    Schema schema = SchemaOf({Col(0, "id", kTypeValInt64), Col(1, "n", kTypeValInt32),
                              Col(2, "b", kTypeValBool), Col(3, "s", kTypeValVarchar)});

    auto layout = RowLayout::Build(schema, kW);
    ASSERT_TRUE(layout.ok()) << layout.status().message();

    // Keystone word + int32 + bool + one tagged cell.
    EXPECT_EQ(layout.value().row_size, kKeystoneWordSize + 4 + 1 + kW);
    EXPECT_EQ(layout.value().inline_cell_width, kW);
}

TEST(RowLayoutTest, OffsetsStartAtTheKeystoneWordAndRunInColumnOrder) {
    Schema schema = SchemaOf({Col(0, "id", kTypeValInt64), Col(1, "a", kTypeValInt16),
                              Col(2, "b", kTypeValVarchar), Col(3, "c", kTypeValInt8)});

    auto layout = RowLayout::Build(schema, kW);
    ASSERT_TRUE(layout.ok());
    ASSERT_EQ(layout.value().offsets.size(), 4u);

    // The pk lives *only* in the Keystone word, so column 0 is that word
    // and the body starts after it (invariant 11).
    EXPECT_EQ(layout.value().offsets[0], 0u);
    EXPECT_EQ(layout.value().offsets[1], kKeystoneWordSize);
    EXPECT_EQ(layout.value().offsets[2], kKeystoneWordSize + 2);
    EXPECT_EQ(layout.value().offsets[3], kKeystoneWordSize + 2 + kW);
    EXPECT_EQ(layout.value().row_size, kKeystoneWordSize + 2 + kW + 1);
}

TEST(RowLayoutTest, AVarcharCostsTheSameWhateverTheWidthIsSetTo) {
    Schema schema = SchemaOf({Col(0, "id", kTypeValInt64), Col(1, "s", kTypeValVarchar)});

    for (std::uint32_t width : {storage::kMinInlineCellWidth, 32u, kW, 256u}) {
        auto layout = RowLayout::Build(schema, width);
        ASSERT_TRUE(layout.ok()) << "width " << width;
        EXPECT_EQ(layout.value().row_size, kKeystoneWordSize + width) << "width " << width;
    }
}

TEST(RowLayoutTest, ACharColumnKeepsItsDeclaredWidth) {
    // `char` was already fixed-width by declaration - the one variable
    // type that never needed a tagged cell.
    Schema schema = SchemaOf({Col(0, "id", kTypeValInt64), Col(1, "c", kTypeValChar, 12)});

    auto layout = RowLayout::Build(schema, kW);
    ASSERT_TRUE(layout.ok());
    EXPECT_EQ(layout.value().row_size, kKeystoneWordSize + 12);
}

// ---- Refusals ------------------------------------------------------------

TEST(RowLayoutTest, AFloatColumnIsUnsupported) {
    // Float and decimal used to be refused together, for one reason: no
    // decided width. `docs/spec/types.md` TY1 splits them. Decimal is a
    // scaled int64 and has a width now; float stays out on the merits -
    // IEEE comparison and aggregation semantics conflict with this
    // engine's exactness discipline - which is a product decision rather
    // than an undecided encoding.
    Schema schema = SchemaOf({Col(0, "id", kTypeValInt64), Col(1, "x", kTypeValFloat)});
    auto layout = RowLayout::Build(schema, kW);

    ASSERT_FALSE(layout.ok());
    EXPECT_EQ(layout.status().code(), StatusCode::kUnsupported);
    EXPECT_NE(layout.status().message().find("float"), std::string::npos)
        << layout.status().message();
}

TEST(RowLayoutTest, ADecimalColumnHasAWidthNow) {
    // Eight bytes, the unscaled int64 (TY2). A width is not an encoding:
    // `CheckDeclarableColumnTypes` is what still refuses the *column* until
    // the DDL can carry (p, s), and this only says how wide it will be.
    Schema schema = SchemaOf({Col(0, "id", kTypeValInt64), Col(1, "x", kTypeValDecimal)});
    auto layout = RowLayout::Build(schema, kW);

    ASSERT_TRUE(layout.ok()) << layout.status().message();
    auto width = RowLayout::ColumnWidth(Col(1, "x", kTypeValDecimal), kW);
    ASSERT_TRUE(width.ok());
    EXPECT_EQ(width.value(), 8u);
}

TEST(RowLayoutTest, DateAndTimestampHaveTheirSpecifiedWidths) {
    // TY4: a date is int32 epoch days, a timestamp int64 UTC micros. Both
    // fixed-width, which is the only reason invariant 13 is untouched.
    auto date = RowLayout::ColumnWidth(Col(1, "d", kTypeValDate), kW);
    ASSERT_TRUE(date.ok()) << date.status().message();
    EXPECT_EQ(date.value(), 4u);

    auto ts = RowLayout::ColumnWidth(Col(1, "t", kTypeValTimestamp), kW);
    ASSERT_TRUE(ts.ok()) << ts.status().message();
    EXPECT_EQ(ts.value(), 8u);
}

TEST(RowLayoutTest, ARowTooWideForAPageIsUnsupported) {
    // Enough tagged cells to overrun a page. Refused here rather than at
    // the first INSERT, which would otherwise be the only place anyone
    // found out.
    Schema schema;
    schema.columns.push_back(Col(0, "id", kTypeValInt64));
    const std::uint32_t columns = (heap::kMaxTuplePayloadSize / kW) + 2;
    for (std::uint32_t i = 0; i < columns; ++i) {
        schema.columns.push_back(Col(i + 1, "c" + std::to_string(i), kTypeValVarchar));
    }

    auto layout = RowLayout::Build(schema, kW);
    ASSERT_FALSE(layout.ok());
    EXPECT_EQ(layout.status().code(), StatusCode::kUnsupported);
}

TEST(RowLayoutTest, TheWidestRowThatStillFitsAPageIsAccepted) {
    // The other side of the boundary, so the check is a limit rather than
    // a taste.
    Schema schema;
    schema.columns.push_back(Col(0, "id", kTypeValInt64));
    const std::uint32_t body = heap::kMaxTuplePayloadSize - kKeystoneWordSize;
    schema.columns.push_back(Col(1, "c", kTypeValChar, body));

    auto layout = RowLayout::Build(schema, kW);
    ASSERT_TRUE(layout.ok()) << layout.status().message();
    EXPECT_EQ(layout.value().row_size, heap::kMaxTuplePayloadSize);

    schema.columns[1].len = body + 1;
    EXPECT_FALSE(RowLayout::Build(schema, kW).ok());
}

TEST(RowLayoutTest, AZeroWidthColumnIsRefused) {
    // Two columns at one offset is an ambiguous layout, not merely a
    // useless one. `char` with len 0 is the only way to spell it.
    Schema schema = SchemaOf({Col(0, "id", kTypeValInt64), Col(1, "c", kTypeValChar, 0)});
    auto layout = RowLayout::Build(schema, kW);

    ASSERT_FALSE(layout.ok());
    EXPECT_EQ(layout.status().code(), StatusCode::kInvalidArgument);
}

TEST(RowLayoutTest, ASchemaWithNoKeystoneColumnIsRefused) {
    EXPECT_FALSE(RowLayout::Build(Schema{}, kW).ok());
    EXPECT_FALSE(RowLayout::Build(SchemaOf({Col(0, "s", kTypeValVarchar)}), kW).ok());
}

TEST(RowLayoutTest, AnOutOfRangeCellWidthIsRefused) {
    Schema schema = SchemaOf({Col(0, "id", kTypeValInt64), Col(1, "s", kTypeValVarchar)});

    EXPECT_FALSE(RowLayout::Build(schema, 0).ok());
    EXPECT_FALSE(RowLayout::Build(schema, storage::kMinInlineCellWidth - 1).ok());
    EXPECT_FALSE(RowLayout::Build(schema, storage::kMaxInlineCellWidth + 1).ok());
}

// ---- The null bitmap (null.md §2, §6) --------------------------------

SysColumnRow Nullable(std::uint32_t pos, std::string_view name, std::uint32_t type_val,
                      std::uint32_t len = 0) {
    SysColumnRow col = Col(pos, name, type_val, len);
    col.notnull = false;
    return col;
}

// §2.2's whole claim, asserted rather than believed: an all-NOT NULL schema
// pays zero bitmap bytes and a byte-identical row_size - which is every
// relation in existence, and why no data file needs rewriting.
TEST(RowLayoutTest, AnAllNotNullSchemaPaysNoBitmapAndItsRowSizeIsUnmoved) {
    Schema schema = SchemaOf({Col(0, "id", kTypeValInt64), Col(1, "n", kTypeValInt32),
                              Col(2, "s", kTypeValVarchar)});
    auto layout = RowLayout::Build(schema, kW);
    ASSERT_TRUE(layout.ok());
    EXPECT_EQ(layout.value().null_bitmap_bytes, 0u);
    EXPECT_EQ(layout.value().row_size, 8u + 4u + kW);
    for (const std::uint16_t bit : layout.value().null_bits) {
        EXPECT_EQ(bit, kNoNullBit);
    }
}

TEST(RowLayoutTest, NullableColumnsTakeAscendingBitsAndNotNullColumnsTakeNone) {
    Schema schema = SchemaOf({Col(0, "id", kTypeValInt64), Nullable(1, "a", kTypeValInt32),
                              Col(2, "b", kTypeValInt32), Nullable(3, "c", kTypeValVarchar)});
    auto layout = RowLayout::Build(schema, kW);
    ASSERT_TRUE(layout.ok());
    EXPECT_EQ(layout.value().null_bitmap_bytes, 1u);
    EXPECT_EQ(layout.value().null_bits[0], kNoNullBit);
    EXPECT_EQ(layout.value().null_bits[1], 0u);
    EXPECT_EQ(layout.value().null_bits[2], kNoNullBit);
    EXPECT_EQ(layout.value().null_bits[3], 1u);
    // The bitmap is appended: every column offset matches the all-NOT NULL
    // twin's, and row_size grows by exactly the bitmap.
    Schema twin = SchemaOf({Col(0, "id", kTypeValInt64), Col(1, "a", kTypeValInt32),
                            Col(2, "b", kTypeValInt32), Col(3, "c", kTypeValVarchar)});
    auto twin_layout = RowLayout::Build(twin, kW);
    ASSERT_TRUE(twin_layout.ok());
    EXPECT_EQ(layout.value().offsets, twin_layout.value().offsets);
    EXPECT_EQ(layout.value().row_size, twin_layout.value().row_size + 1u);
}

// The byte boundary §6 names: 8 nullable columns fit one byte, 9 take two.
TEST(RowLayoutTest, TheBitmapGrowsAByteAtTheNinthNullableColumn) {
    auto with_nullable = [](std::size_t n) {
        Schema schema;
        schema.columns.push_back(Col(0, "id", kTypeValInt64));
        for (std::size_t i = 0; i < n; ++i) {
            schema.columns.push_back(
                Nullable(static_cast<std::uint32_t>(i + 1), "c" + std::to_string(i),
                         kTypeValInt32));
        }
        return schema;
    };
    auto eight = RowLayout::Build(with_nullable(8), kW);
    auto nine = RowLayout::Build(with_nullable(9), kW);
    ASSERT_TRUE(eight.ok());
    ASSERT_TRUE(nine.ok());
    EXPECT_EQ(eight.value().null_bitmap_bytes, 1u);
    EXPECT_EQ(nine.value().null_bitmap_bytes, 2u);
}

// Invariant 11's layout-level defense: the first column carries the
// Keystone word, which has no NULL encoding.
TEST(RowLayoutTest, ANullableFirstColumnIsRefused) {
    Schema schema = SchemaOf({Nullable(0, "id", kTypeValInt64), Col(1, "v", kTypeValInt32)});
    auto layout = RowLayout::Build(schema, kW);
    ASSERT_FALSE(layout.ok());
    EXPECT_EQ(layout.status().code(), StatusCode::kInvalidArgument);
}

// The two bit helpers, against a hand-built payload: set and test agree,
// and a zero-filled payload reads all-present.
TEST(RowLayoutTest, TheBitHelpersRoundTripAndZeroMeansPresent) {
    Schema schema = SchemaOf({Col(0, "id", kTypeValInt64), Nullable(1, "a", kTypeValInt32),
                              Nullable(2, "b", kTypeValInt32)});
    auto layout = RowLayout::Build(schema, kW);
    ASSERT_TRUE(layout.ok());
    std::vector<std::byte> payload(layout.value().row_size, std::byte{0});
    EXPECT_FALSE(NullBitIsSet(payload, layout.value(), 1));
    EXPECT_FALSE(NullBitIsSet(payload, layout.value(), 2));
    SetNullBit(payload, layout.value(), 2);
    EXPECT_FALSE(NullBitIsSet(payload, layout.value(), 1));
    EXPECT_TRUE(NullBitIsSet(payload, layout.value(), 2));
    EXPECT_FALSE(NullBitIsSet(payload, layout.value(), 0)) << "kNoNullBit is never NULL";
}

}  // namespace
}  // namespace kds::catalog
