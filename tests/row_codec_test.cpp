#include "kds/exec/row_codec.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/catalog/well_known.hpp"
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
    EXPECT_EQ(FormatValue(row.value()[0]), "42");
    EXPECT_EQ(FormatValue(row.value()[1]), "bob");
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
    EXPECT_EQ(FormatValue(row.value()[0]), "100000");
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
// Found while building MIN/MAX over uint64 (docs/feat-aggregate.md §3.3),
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

}  // namespace
}  // namespace kds::exec
