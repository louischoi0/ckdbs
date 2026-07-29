#include "kds/exec/row_codec.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/catalog/well_known.hpp"
#include "kds/storage/keystone.hpp"

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
    auto encoded = EncodeRow(TwoColumnSchema(), /*id=*/7, {Str("alice")});
    ASSERT_TRUE(encoded.ok()) << encoded.status().message();

    // 8-byte word + 2-byte varchar length prefix + 5 bytes of text. The pk
    // is NOT also encoded into the body.
    EXPECT_EQ(encoded.value().size(), kKeystoneWordSize + 2 + 5);

    auto id = RowKeystoneId(encoded.value());
    ASSERT_TRUE(id.ok());
    EXPECT_EQ(id.value(), 7u);
}

TEST(RowCodecKeystoneTest, RoundTripsWithThePrimaryKeyFirst) {
    auto encoded = EncodeRow(TwoColumnSchema(), /*id=*/42, {Str("bob")});
    ASSERT_TRUE(encoded.ok());

    auto row = DecodeRow(TwoColumnSchema(), encoded.value());
    ASSERT_TRUE(row.ok()) << row.status().message();
    ASSERT_EQ(row.value().size(), 2u);
    EXPECT_EQ(FormatValue(row.value()[0]), "42");
    EXPECT_EQ(FormatValue(row.value()[1]), "bob");
}

TEST(RowCodecKeystoneTest, ValueListCoversEveryColumnButThePrimaryKey) {
    // One value too many - the arity a caller supplying the pk would send.
    auto extra = EncodeRow(TwoColumnSchema(), 1, {Int(1), Str("alice")});
    EXPECT_FALSE(extra.ok());
    EXPECT_EQ(extra.status().code(), StatusCode::kInvalidArgument);

    auto missing = EncodeRow(TwoColumnSchema(), 1, {});
    EXPECT_FALSE(missing.ok());
    EXPECT_EQ(missing.status().code(), StatusCode::kInvalidArgument);
}

TEST(RowCodecKeystoneTest, AnIdBeyondFortyBitsIsRefused) {
    auto out = EncodeRow(TwoColumnSchema(), kMaxKeystoneId + 1, {Str("x")});
    EXPECT_FALSE(out.ok());
    EXPECT_EQ(out.status().code(), StatusCode::kInvalidArgument);

    // The boundary itself is fine.
    EXPECT_TRUE(EncodeRow(TwoColumnSchema(), kMaxKeystoneId, {Str("x")}).ok());
}

TEST(RowCodecKeystoneTest, DistinctIdsProduceDistinctPayloads) {
    auto a = EncodeRow(TwoColumnSchema(), 1, {Str("alice")});
    auto b = EncodeRow(TwoColumnSchema(), 2, {Str("alice")});
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
    EXPECT_FALSE(EncodeRow(empty, 1, {}).ok());
    EXPECT_FALSE(DecodeRow(empty, {}).ok());
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

    auto encoded = EncodeRow(schema, /*id=*/100000, {Str("x")});
    ASSERT_TRUE(encoded.ok()) << encoded.status().message();

    auto row = DecodeRow(schema, encoded.value());
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(FormatValue(row.value()[0]), "100000");
}

}  // namespace
}  // namespace kds::exec
