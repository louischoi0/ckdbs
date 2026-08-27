#include "kds/exec/row_codec.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "alloc_counter.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/type_literals.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/tagged_cell.hpp"

// The row codec's contract after the Keystone change: a tuple payload is
// `[Keystone word][columns 1..n-1]`, the first schema column is the primary
// key and lives only in that word, and the id is the caller's to pass in -
// never something the value list carries (heap-and-tuple.md section 4).

namespace kds::exec {
namespace {

catalog::SysColumnRow Col(std::uint32_t pos, std::string_view name, std::uint32_t type_val,
                          std::uint32_t len) {
    catalog::SysColumnRow col{};
    col.pos = pos;
    catalog::SetName(col.name, name);
    col.type_val = type_val;
    col.len = len;
    col.notnull = true;
    return col;
}

catalog::Schema TwoColumnSchema() {
    catalog::Schema schema;
    schema.columns.push_back(Col(0, "id", catalog::kTypeValInt32, 4));
    schema.columns.push_back(Col(1, "name", catalog::kTypeValVarchar, 0));
    return schema;
}

// The layout every test below encodes against. Built rather than written
// down: the row size is a function of the schema and the instance-pinned
// cell width, and a test that hard-coded it would stop testing the codec
// the moment the default width moved.
catalog::RowLayout LayoutFor(const catalog::Schema& schema) {
    auto layout = catalog::RowLayout::Build(schema, storage::kDefaultInlineCellWidth);
    EXPECT_TRUE(layout.ok()) << layout.status().message();
    return layout.ok() ? layout.value() : catalog::RowLayout{};
}

parser::AstValue Str(std::string s) {
    parser::AstValue v{};
    v.type = parser::ValueType::kStr;
    v.str_val = std::move(s);
    return v;
}

parser::AstValue Int(std::int64_t n) {
    parser::AstValue v{};
    v.type = parser::ValueType::kInt;
    v.int_val = n;
    v.raw_int_text = std::to_string(n);
    return v;
}

TEST(RowCodecKeystoneTest, PayloadStartsWithTheKeystoneWord) {
    auto encoded = EncodeRow(TwoColumnSchema(), LayoutFor(TwoColumnSchema()), /*id=*/7, {Str("alice")});
    ASSERT_TRUE(encoded.ok()) << encoded.status().message();

    // 8-byte word + one tagged cell of the pinned width, whatever the
    // value's length: the row size is a schema constant (invariant 13). The
    // pk is NOT also encoded into the body.
    EXPECT_EQ(encoded.value().size(), kKeystoneWordSize + storage::kDefaultInlineCellWidth);
    EXPECT_EQ(encoded.value().size(), LayoutFor(TwoColumnSchema()).row_size);

    auto id = RowKeystoneId(encoded.value());
    ASSERT_TRUE(id.ok());
    EXPECT_EQ(id.value(), 7u);
}

TEST(RowCodecKeystoneTest, RoundTripsWithThePrimaryKeyFirst) {
    auto encoded = EncodeRow(TwoColumnSchema(), LayoutFor(TwoColumnSchema()), /*id=*/42, {Str("bob")});
    ASSERT_TRUE(encoded.ok());

    auto row = DecodeRow(TwoColumnSchema(), LayoutFor(TwoColumnSchema()), encoded.value());
    ASSERT_TRUE(row.ok()) << row.status().message();
    ASSERT_EQ(row.value().size(), 2u);
    EXPECT_EQ(FormatValue(/*type_val=*/0, row.value()[0]), "42");
    EXPECT_EQ(FormatValue(/*type_val=*/0, row.value()[1]), "bob");
}

TEST(RowCodecKeystoneTest, ValueListCoversEveryColumnButThePrimaryKey) {
    // One value too many - the arity a caller supplying the pk would send.
    auto extra = EncodeRow(TwoColumnSchema(), LayoutFor(TwoColumnSchema()), 1, {Int(1), Str("alice")});
    EXPECT_FALSE(extra.ok());
    EXPECT_EQ(extra.status().code(), StatusCode::kInvalidArgument);

    auto missing = EncodeRow(TwoColumnSchema(), LayoutFor(TwoColumnSchema()), 1, {});
    EXPECT_FALSE(missing.ok());
    EXPECT_EQ(missing.status().code(), StatusCode::kInvalidArgument);
}

TEST(RowCodecKeystoneTest, AnIdBeyondFortyBitsIsRefused) {
    auto out = EncodeRow(TwoColumnSchema(), LayoutFor(TwoColumnSchema()), kMaxKeystoneId + 1, {Str("x")});
    EXPECT_FALSE(out.ok());
    EXPECT_EQ(out.status().code(), StatusCode::kInvalidArgument);

    // The boundary itself is fine.
    EXPECT_TRUE(EncodeRow(TwoColumnSchema(), LayoutFor(TwoColumnSchema()), kMaxKeystoneId, {Str("x")}).ok());
}

TEST(RowCodecKeystoneTest, DistinctIdsProduceDistinctPayloads) {
    auto a = EncodeRow(TwoColumnSchema(), LayoutFor(TwoColumnSchema()), 1, {Str("alice")});
    auto b = EncodeRow(TwoColumnSchema(), LayoutFor(TwoColumnSchema()), 2, {Str("alice")});
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    // Same row values, different key: the payloads must not be identical,
    // which is exactly what stopped two "same id" tuples being writable.
    EXPECT_NE(a.value(), b.value());
}

TEST(RowCodecKeystoneTest, APayloadTooShortForTheWordIsCorruption) {
    std::vector<std::byte> stub(4, std::byte{0});
    auto id = RowKeystoneId(stub);
    EXPECT_FALSE(id.ok());
    EXPECT_EQ(id.status().code(), StatusCode::kCorruption);
}

// ---- Schemas that cannot carry a Keystone id -----------------------------

TEST(RowCodecKeystoneTest, ASchemaWithNoColumnsIsRefused) {
    catalog::Schema empty;
    EXPECT_FALSE(catalog::CheckKeystoneColumn(empty).ok());
    EXPECT_FALSE(EncodeRow(empty, catalog::RowLayout{}, 1, {}).ok());
    EXPECT_FALSE(DecodeRow(empty, catalog::RowLayout{}, {}).ok());
}

TEST(RowCodecKeystoneTest, ANonIntegerFirstColumnIsRefused) {
    catalog::Schema schema;
    schema.columns.push_back(Col(0, "name", catalog::kTypeValVarchar, 0));
    schema.columns.push_back(Col(1, "id", catalog::kTypeValInt64, 8));

    Status s = catalog::CheckKeystoneColumn(schema);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("must be an integer type"), std::string::npos) << s.message();
}

TEST(RowCodecKeystoneTest, EveryIntegerWidthIsAcceptedAsAPrimaryKey) {
    for (std::uint32_t type_val :
         {catalog::kTypeValInt8, catalog::kTypeValInt16, catalog::kTypeValInt32,
          catalog::kTypeValInt64, catalog::kTypeValUint64}) {
        catalog::Schema schema;
        schema.columns.push_back(Col(0, "id", type_val, 8));
        EXPECT_TRUE(catalog::CheckKeystoneColumn(schema).ok()) << "type_val " << type_val;
    }
}

TEST(RowCodecKeystoneTest, ThePrimaryKeyIsNotConstrainedByItsDeclaredWidth) {
    // The id lives in the 40-bit Keystone field, not in an int8 column, so
    // declaring a narrow pk type does not cap the sequence. The declared
    // type is display metadata (DESCRIBE), not the storage.
    catalog::Schema schema;
    schema.columns.push_back(Col(0, "id", catalog::kTypeValInt8, 1));
    schema.columns.push_back(Col(1, "name", catalog::kTypeValVarchar, 0));

    auto encoded = EncodeRow(schema, LayoutFor(schema), /*id=*/100000, {Str("x")});
    ASSERT_TRUE(encoded.ok()) << encoded.status().message();

    auto row = DecodeRow(schema, LayoutFor(schema), encoded.value());
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(FormatValue(/*type_val=*/0, row.value()[0]), "100000");
}

// ---- CompareValues over a uint64 column ---------------------------------
//
// A decoded uint64 carries its value in `raw_int_text` **only when int_val
// cannot hold it** - above INT64_MAX - and leaves the text empty otherwise.
// `ValueAsUint64` is the one place that knows that rule, and its header
// warns that a caller reading the text directly "gets an empty string for
// every ordinary value and silently reads zero, which is how this rule
// breaks". CompareValues was such a caller: it parsed the text on both
// sides and answered false whenever either parse failed, so every
// comparison with an ordinary uint64 operand was a non-match.
//
// Found while building MIN/MAX over uint64 (docs/spec/aggregate.md §3.3),
// which could not descend below INT64_MAX - but the bug was never about
// aggregation: `WHERE big = 5` returned no rows.

TEST(RowCodecCompareTest, AUint64ComparesCorrectlyBelowInt64Max) {
    const parser::AstValue five = Int(5);
    const parser::AstValue nine = Int(9);
    EXPECT_TRUE(CompareValues(catalog::kTypeValUint64, five, nine, parser::CompareOp::kLt));
    EXPECT_FALSE(CompareValues(catalog::kTypeValUint64, nine, five, parser::CompareOp::kLt));
    EXPECT_TRUE(CompareValues(catalog::kTypeValUint64, five, five, parser::CompareOp::kEq));
    EXPECT_TRUE(CompareValues(catalog::kTypeValUint64, nine, five, parser::CompareOp::kGt));
}

TEST(RowCodecCompareTest, AUint64ComparesADecodedValueAgainstALiteral) {
    // The shape a real predicate has: the decoded side carries no text for
    // an ordinary value, the literal side always carries the digits it was
    // written with. Both readings must agree.
    parser::AstValue decoded = Int(5);           // as row_codec produces it
    parser::AstValue literal = Int(5);
    literal.raw_int_text = "5";                  // as the parser produces it
    EXPECT_TRUE(CompareValues(catalog::kTypeValUint64, decoded, literal,
                              parser::CompareOp::kEq));
    EXPECT_TRUE(CompareValues(catalog::kTypeValUint64, literal, decoded,
                              parser::CompareOp::kEq));
}

TEST(RowCodecCompareTest, AUint64AboveInt64MaxOutranksEverySmallValue) {
    // The reason the unsigned path exists at all: a signed reading orders
    // these backwards.
    parser::AstValue big = Int(0);
    big.int_val = static_cast<std::int64_t>(18446744073709551615ULL);
    big.raw_int_text = "18446744073709551615";

    EXPECT_TRUE(CompareValues(catalog::kTypeValUint64, Int(5), big, parser::CompareOp::kLt));
    EXPECT_TRUE(CompareValues(catalog::kTypeValUint64, big, Int(5), parser::CompareOp::kGt));
    EXPECT_FALSE(CompareValues(catalog::kTypeValUint64, big, Int(5), parser::CompareOp::kLt));
}

TEST(RowCodecCompareTest, ANegativeOperandAgainstAUint64IsANonMatch) {
    // Not an error: a type mismatch is a non-match everywhere else in this
    // function, and a negative literal is not a uint64.
    EXPECT_FALSE(CompareValues(catalog::kTypeValUint64, Int(-1), Int(5), parser::CompareOp::kLt));
    EXPECT_FALSE(CompareValues(catalog::kTypeValUint64, Int(-1), Int(5), parser::CompareOp::kGt));
}

// ---- TY04: the three new types through the codec ------------------------
//
// TY7 makes encode the **only gate**, so the property to pin is that what
// comes back is bit-exactly what went in - decode does no validation and
// would not catch a discrepancy.

namespace {

// A Keystone id plus the one column under test. Going through EncodeRow /
// DecodeRow rather than reaching for the per-column arms directly is
// deliberate: those are file-static, and a test-only export of them would
// be a second way into the codec - which is the thing TY7's single gate
// exists to prevent.
catalog::Schema TypedSchema(std::uint32_t type_val, std::uint32_t len) {
    catalog::Schema schema;
    schema.columns.push_back(Col(0, "id", catalog::kTypeValInt64, 8));
    schema.columns.push_back(Col(1, "v", type_val, len));
    return schema;
}

// Encodes one value as a row and reads it straight back.
parser::AstValue RoundTrip(const catalog::Schema& schema, const parser::AstValue& in,
                           Status& status) {
    parser::AstValue out;
    const catalog::RowLayout layout = LayoutFor(schema);

    auto encoded = EncodeRow(schema, layout, /*id=*/1, {in});
    status = encoded.status();
    if (!status.ok()) return out;

    auto row = DecodeRow(schema, layout, encoded.value());
    status = row.status();
    if (!status.ok()) return out;
    // [0] is the pk carried by the Keystone word; [1] is the value.
    return row.value()[1];
}

}  // namespace

TEST(RowCodecTypesTest, ADateRoundTripsBitExactlyAcrossTheRange) {
    const catalog::Schema schema = TypedSchema(catalog::kTypeValDate, 4);
    for (const char* text : {"1900-01-01", "1969-12-31", "1970-01-01", "2026-08-07",
                             "2999-12-31"}) {
        parser::AstValue in;
        in.type = parser::ValueType::kStr;
        in.str_val = text;

        Status s;
        const parser::AstValue out = RoundTrip(schema, in, s);
        ASSERT_TRUE(s.ok()) << text << ": " << s.message();
        // Decoded as an *integer*, not a rendered string (TY5).
        EXPECT_EQ(out.type, parser::ValueType::kInt) << text;
        EXPECT_TRUE(out.raw_int_text.empty()) << text;
        EXPECT_EQ(FormatDate(static_cast<std::int32_t>(out.int_val)), text);
    }
}

TEST(RowCodecTypesTest, ATimestampRoundTripsBitExactly) {
    const catalog::Schema schema = TypedSchema(catalog::kTypeValTimestamp, 8);
    for (const char* text : {"1900-01-01 00:00:00", "1969-12-31 23:59:59.999999",
                             "1970-01-01 00:00:00", "2026-08-07 09:15:00.250000",
                             "2999-12-31 23:59:59.999999"}) {
        parser::AstValue in;
        in.type = parser::ValueType::kStr;
        in.str_val = text;

        Status s;
        const parser::AstValue out = RoundTrip(schema, in, s);
        ASSERT_TRUE(s.ok()) << text << ": " << s.message();
        EXPECT_EQ(out.type, parser::ValueType::kInt) << text;
        EXPECT_EQ(FormatTimestamp(out.int_val), text);
    }
}

TEST(RowCodecTypesTest, ADecimalRoundTripsWithItsScale) {
    const catalog::Schema schema =
        TypedSchema(catalog::kTypeValDecimal, catalog::PackDecimalLen(10, 2));
    for (const char* text : {"12.34", "-12.34", "0.05", "0.00", "99999999.99"}) {
        parser::AstValue in;
        in.type = parser::ValueType::kStr;
        in.str_val = text;

        Status s;
        const parser::AstValue out = RoundTrip(schema, in, s);
        ASSERT_TRUE(s.ok()) << text << ": " << s.message();
        // The one kind that gains a ValueType, because the unscaled integer
        // means nothing without the scale beside it (TY5).
        EXPECT_EQ(out.type, parser::ValueType::kDecimal) << text;
        EXPECT_EQ(out.scale, 2) << text;
        EXPECT_EQ(FormatDecimal(out.int_val, out.scale), text);
    }
}

TEST(RowCodecTypesTest, ADecodedValueReEncodesUnchanged) {
    // What an UPDATE does to every column its SET list did not touch. If
    // encode did not accept decode's output, an untouched column would
    // change when a neighbour was written.
    const catalog::Schema date = TypedSchema(catalog::kTypeValDate, 4);
    const catalog::Schema ts = TypedSchema(catalog::kTypeValTimestamp, 8);
    const catalog::Schema dec =
        TypedSchema(catalog::kTypeValDecimal, catalog::PackDecimalLen(10, 2));

    for (const auto& [schema, text] : {std::pair{date, "2026-08-07"},
                                       std::pair{ts, "2026-08-07 09:15:00.250000"},
                                       std::pair{dec, "12.34"}}) {
        parser::AstValue in;
        in.type = parser::ValueType::kStr;
        in.str_val = text;

        Status s;
        const parser::AstValue once = RoundTrip(schema, in, s);
        ASSERT_TRUE(s.ok()) << text << ": " << s.message();
        const parser::AstValue twice = RoundTrip(schema, once, s);
        ASSERT_TRUE(s.ok()) << text << " (re-encode): " << s.message();
        EXPECT_EQ(twice.int_val, once.int_val) << text;
        EXPECT_EQ(twice.type, once.type) << text;
        EXPECT_EQ(twice.scale, once.scale) << text;
    }
}

TEST(RowCodecTypesTest, DecodingTheNewTypesAllocatesNothing) {
    // TY5's int-only decode, asserted rather than described. A date's value
    // *is* its epoch day, so decoding one must cost no more than decoding
    // an `int32` - the moment an arm here renders text, every rejected row
    // of a scan pays for a string it never emits, which is the regression
    // the int decoder's own comment documents.
    //
    // Decoded into slots the caller already owns (DecodeRowInto), because
    // that is what a chain scan does; DecodeRow allocates its result vector
    // by construction and would measure that instead.
    struct Case {
        const char* what;
        catalog::Schema schema;
        const char* literal;
    };
    const std::vector<Case> cases = {
        {"date", TypedSchema(catalog::kTypeValDate, 4), "2026-08-07"},
        {"timestamp", TypedSchema(catalog::kTypeValTimestamp, 8), "2026-08-07 09:15:00.250000"},
        {"decimal", TypedSchema(catalog::kTypeValDecimal, catalog::PackDecimalLen(10, 2)),
         "12.34"},
    };

    for (const Case& c : cases) {
        const catalog::RowLayout layout = LayoutFor(c.schema);
        parser::AstValue in;
        in.type = parser::ValueType::kStr;
        in.str_val = c.literal;

        auto encoded = EncodeRow(c.schema, layout, /*id=*/1, {in});
        ASSERT_TRUE(encoded.ok()) << c.what << ": " << encoded.status().message();

        // Decode once outside the counter: the slots are reused across
        // rows in a real scan, so the first row's growth is a per-statement
        // cost and not a per-row one.
        std::vector<parser::AstValue> out(c.schema.columns.size());
        ASSERT_TRUE(DecodeRowInto(c.schema, layout, encoded.value(), out).ok()) << c.what;

        test_support::CountAllocations counter;
        const Status s = DecodeRowInto(c.schema, layout, encoded.value(), out);
        const std::size_t allocations = counter.count();

        ASSERT_TRUE(s.ok()) << c.what << ": " << s.message();
        EXPECT_EQ(allocations, 0u) << c.what << " decode allocated";
    }
}

TEST(RowCodecTypesTest, ADecimalWithTheWrongScaleIsRefusedRatherThanRescaled) {
    // TY6 defers cross-scale work whole. Rescaling here would either lose
    // digits or invent them.
    const catalog::Schema schema =
        TypedSchema(catalog::kTypeValDecimal, catalog::PackDecimalLen(10, 2));
    parser::AstValue in;
    in.type = parser::ValueType::kDecimal;
    in.int_val = 1234;
    in.scale = 3;

    Status s;
    RoundTrip(schema, in, s);
    ASSERT_FALSE(s.ok());
    EXPECT_NE(s.message().find("rescale"), std::string::npos) << s.message();
}

// ---- NULL round trips (docs/spec/null.md §2, §3, §6) -----------------------

catalog::SysColumnRow NullableCol(std::uint32_t pos, std::string_view name,
                                  std::uint32_t type_val, std::uint32_t len = 0) {
    catalog::SysColumnRow col = Col(pos, name, type_val, len);
    col.notnull = false;
    return col;
}

parser::AstValue Null() {
    parser::AstValue v{};
    v.type = parser::ValueType::kNull;
    return v;
}

// Every nullable type round-trips NULL, in the first and last nullable
// position, and a present neighbour is untouched by it. The varchar being
// the *last* column is load-bearing: its cell is the one whose span would
// have run into the bitmap under the old to-the-end-of-the-row rule, so
// this test fails if CellOf/MutableCellOf ever regress to `row_size`.
TEST(RowCodecNullTest, NullRoundTripsPerTypeAndPosition) {
    catalog::Schema schema;
    schema.columns.push_back(Col(0, "id", catalog::kTypeValInt64, 8));
    schema.columns.push_back(NullableCol(1, "i", catalog::kTypeValInt32, 4));
    schema.columns.push_back(Col(2, "mid", catalog::kTypeValInt64, 8));
    schema.columns.push_back(NullableCol(3, "s", catalog::kTypeValVarchar));
    const catalog::RowLayout layout = LayoutFor(schema);

    VarHeapSink sink;  // no store: an inline-only row needs none
    auto row = EncodeRow(schema, layout, /*id=*/7, {Null(), Int(42), Null()}, sink);
    ASSERT_TRUE(row.ok()) << row.status().message();
    ASSERT_EQ(row.value().size(), layout.row_size);

    auto decoded = DecodeRow(schema, layout, row.value(), nullptr);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value()[1].type, parser::ValueType::kNull);
    EXPECT_EQ(decoded.value()[2].type, parser::ValueType::kInt);
    EXPECT_EQ(decoded.value()[2].int_val, 42);
    EXPECT_EQ(decoded.value()[3].type, parser::ValueType::kNull);

    // And the same row with both present decodes both - the bit is per
    // row, not per relation.
    auto full = EncodeRow(schema, layout, 8, {Int(1), Int(2), Str("x")}, sink);
    ASSERT_TRUE(full.ok()) << full.status().message();
    auto full_dec = DecodeRow(schema, layout, full.value(), nullptr);
    ASSERT_TRUE(full_dec.ok());
    EXPECT_EQ(full_dec.value()[1].int_val, 1);
    EXPECT_EQ(full_dec.value()[3].str_val, "x");
}

TEST(RowCodecNullTest, ANullIntoANotNullColumnIsRefusedByName) {
    catalog::Schema schema = TwoColumnSchema();
    const catalog::RowLayout layout = LayoutFor(schema);
    VarHeapSink sink;
    auto row = EncodeRow(schema, layout, 7, {Null()}, sink);
    ASSERT_FALSE(row.ok());
    EXPECT_EQ(row.status().code(), StatusCode::kInvalidArgument);
    // Quoted, so this pins "the message names the column" and not merely
    // "the message contains the substring" - the column is literally
    // called `name`, which the unquoted find could never tell apart.
    EXPECT_NE(row.status().message().find("'name'"), std::string::npos)
        << row.status().message();
}

// Spec §6's 8/9 boundary, through the codec and not only the layout: the
// ninth nullable column's bit lives in the second bitmap byte, and both
// bytes must survive the last cell being written.
TEST(RowCodecNullTest, TheNinthNullableColumnRoundTripsFromTheSecondBitmapByte) {
    catalog::Schema schema;
    schema.columns.push_back(Col(0, "id", catalog::kTypeValInt64, 8));
    for (std::uint32_t i = 1; i <= 9; ++i) {
        schema.columns.push_back(
            NullableCol(i, "n" + std::to_string(i), catalog::kTypeValInt32, 4));
    }
    const catalog::RowLayout layout = LayoutFor(schema);
    ASSERT_EQ(layout.null_bitmap_bytes, 2u);

    // Columns 1 and 9 NULL - one bit per bitmap byte - the rest present.
    std::vector<parser::AstValue> values(9, Int(5));
    values[0] = Null();
    values[8] = Null();

    VarHeapSink sink;
    auto row = EncodeRow(schema, layout, 7, values, sink);
    ASSERT_TRUE(row.ok()) << row.status().message();
    auto decoded = DecodeRow(schema, layout, row.value(), nullptr);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value()[1].type, parser::ValueType::kNull);
    EXPECT_EQ(decoded.value()[9].type, parser::ValueType::kNull);
    for (std::size_t i = 2; i <= 8; ++i) {
        EXPECT_EQ(decoded.value()[i].type, parser::ValueType::kInt) << i;
        EXPECT_EQ(decoded.value()[i].int_val, 5) << i;
    }
}

// §3's disagreement is Corruption: a varchar cell hand-tagged kNull while
// the bitmap says present must fail loudly, never read as NULL.
TEST(RowCodecNullTest, ATagBitmapDisagreementIsCorruption) {
    catalog::Schema schema;
    schema.columns.push_back(Col(0, "id", catalog::kTypeValInt64, 8));
    schema.columns.push_back(NullableCol(1, "s", catalog::kTypeValVarchar));
    const catalog::RowLayout layout = LayoutFor(schema);

    VarHeapSink sink;
    auto row = EncodeRow(schema, layout, 7, {Str("x")}, sink);
    ASSERT_TRUE(row.ok());
    // Corrupt: overwrite the cell with the kNull filler, leave the bit clear.
    ASSERT_TRUE(storage::EncodeNullCell(
        std::span<std::byte>(row.value().data() + layout.offsets[1], layout.inline_cell_width)).ok());

    auto decoded = DecodeRow(schema, layout, row.value(), nullptr);
    ASSERT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.status().code(), StatusCode::kCorruption);
}

// The bitmap is after the columns: a NULL row and its all-present twin
// differ only in the bitmap byte, which is the appended-layout property.
TEST(RowCodecNullTest, OnlyTheBitmapByteDistinguishesANullRowFromItsTwin) {
    catalog::Schema schema;
    schema.columns.push_back(Col(0, "id", catalog::kTypeValInt64, 8));
    schema.columns.push_back(NullableCol(1, "i", catalog::kTypeValInt32, 4));
    const catalog::RowLayout layout = LayoutFor(schema);

    VarHeapSink sink;
    auto with_null = EncodeRow(schema, layout, 7, {Null()}, sink);
    auto with_zero = EncodeRow(schema, layout, 7, {Int(0)}, sink);
    ASSERT_TRUE(with_null.ok());
    ASSERT_TRUE(with_zero.ok());
    ASSERT_EQ(with_null.value().size(), with_zero.value().size());
    // Identical everywhere but the bitmap - which is also the proof that
    // NULL and 0 are different rows on disk.
    for (std::size_t i = 0; i + 1 < with_null.value().size(); ++i) {
        EXPECT_EQ(with_null.value()[i], with_zero.value()[i]) << "byte " << i;
    }
    EXPECT_NE(with_null.value().back(), with_zero.value().back());
}

}  // namespace
}  // namespace kds::exec
