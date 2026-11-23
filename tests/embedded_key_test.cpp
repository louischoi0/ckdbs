#include "kds/exec/embedded_key.hpp"

#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "kds/catalog/well_known.hpp"
#include "kds/storage/keystone.hpp"

// DR-S3e: the embedded primary key's code is a persisted format (DR-R12) -
// every row of an embedded relation sits at the id it computes - so it is
// pinned here by value. A cell that moves is a format event (DR-R5), not a
// refactor.

namespace kds::exec {
namespace {

using catalog::kTypeValBool;
using catalog::kTypeValChar;
using catalog::kTypeValDate;
using catalog::kTypeValInt16;
using catalog::kTypeValInt32;
using catalog::kTypeValInt64;
using catalog::kTypeValInt8;
using catalog::kTypeValVarchar;

catalog::SysColumnRow Column(std::uint32_t type_val, std::uint16_t pos) {
    catalog::SysColumnRow col{};
    col.oid = pos;
    col.rel_id = 1;
    col.pos = pos;
    catalog::SetName(col.name, "k" + std::to_string(pos));
    col.type_val = type_val;
    col.notnull = true;
    return col;
}

// A relation as `CREATE TABLE` makes an embedded one: the hidden Keystone at
// position 0, then the key's columns in key order.
catalog::Schema Embedded(std::vector<std::uint32_t> types) {
    catalog::Schema schema;
    schema.columns.push_back(catalog::HiddenTupleKeyColumn());
    for (std::size_t i = 0; i < types.size(); ++i) {
        schema.columns.push_back(Column(types[i], static_cast<std::uint16_t>(i + 1)));
        schema.embedded_key.push_back(static_cast<std::uint16_t>(i + 1));
    }
    return schema;
}

parser::AstValue Int(std::int64_t v) {
    parser::AstValue out;
    out.type = parser::ValueType::kInt;
    out.int_val = v;
    return out;
}

std::optional<EmbeddedKeyWindow> Window(const catalog::Schema& schema,
                                        std::span<const parser::AstValue> prefix,
                                        const parser::AstValue* low,
                                        const parser::AstValue* high) {
    auto window = EmbeddedKeyWindowOf(schema, prefix, low, high);
    EXPECT_TRUE(window.ok()) << window.status().message();
    return window.ok() ? window.value() : std::nullopt;
}

std::uint64_t IdOf(const catalog::Schema& schema, std::vector<std::int64_t> key) {
    std::vector<parser::AstValue> values;
    for (std::int64_t v : key) values.push_back(Int(v));
    auto id = EmbeddedKeyId(schema, values);
    EXPECT_TRUE(id.ok()) << id.status().message();
    return id.ok() ? id.value() : 0;
}

constexpr std::int64_t kInt32Min = std::numeric_limits<std::int32_t>::min();
constexpr std::int64_t kInt32Max = std::numeric_limits<std::int32_t>::max();

TEST(EmbeddedKeyTest, WhichKeysEmbed) {
    const auto embeds = [](std::vector<std::uint32_t> types) {
        std::vector<catalog::SysColumnRow> cols;
        for (std::size_t i = 0; i < types.size(); ++i) {
            cols.push_back(Column(types[i], static_cast<std::uint16_t>(i + 1)));
        }
        return KeyEmbeds(cols);
    };
    EXPECT_TRUE(embeds({kTypeValInt32}));
    EXPECT_TRUE(embeds({kTypeValDate}));
    EXPECT_TRUE(embeds({kTypeValInt32, kTypeValInt16}));       // 48 bits exactly
    EXPECT_TRUE(embeds({kTypeValDate, kTypeValInt8, kTypeValBool}));
    EXPECT_TRUE(embeds({kTypeValInt16, kTypeValInt16, kTypeValInt16}));
    EXPECT_FALSE(embeds({kTypeValInt32, kTypeValInt32}));      // 64 bits
    EXPECT_FALSE(embeds({kTypeValInt32, kTypeValInt16, kTypeValInt8}));  // 56 bits
    EXPECT_FALSE(embeds({kTypeValInt64}));
    EXPECT_FALSE(embeds({kTypeValVarchar}));
    EXPECT_FALSE(embeds({kTypeValChar}));  // DR-Q7 (a) as revised
    EXPECT_FALSE(embeds({}));
}

// The format's vectors: id = code + 1, a signed column's sign bit flipped,
// columns most significant first.
TEST(EmbeddedKeyTest, TheIdsOfPinnedKeys) {
    const catalog::Schema one = Embedded({kTypeValInt32});
    EXPECT_EQ(IdOf(one, {kInt32Min}), 1u);
    EXPECT_EQ(IdOf(one, {-1}), 0x80000000ull);
    EXPECT_EQ(IdOf(one, {0}), 0x80000001ull);
    EXPECT_EQ(IdOf(one, {1}), 0x80000002ull);
    EXPECT_EQ(IdOf(one, {kInt32Max}), 0x100000000ull);

    const catalog::Schema two = Embedded({kTypeValInt32, kTypeValInt16});
    EXPECT_EQ(IdOf(two, {1, 2}), 0x800000018003ull);
    EXPECT_EQ(IdOf(two, {0, 0}), 0x800000008001ull);
    EXPECT_EQ(IdOf(two, {kInt32Min, -32768}), 1u);

    const catalog::Schema day = Embedded({kTypeValDate, kTypeValInt8, kTypeValBool});
    // 2026-01-02 is epoch day 20455 (0x4FE7): 0x80004FE7, then int8 3 as
    // 0x83, then bool 1 as 0x01.
    EXPECT_EQ(IdOf(day, {20455, 3, 1}), 0x80004FE78302ull);
}

TEST(EmbeddedKeyTest, IdsAscendAsKeysDo) {
    const catalog::Schema two = Embedded({kTypeValInt32, kTypeValInt16});
    const std::vector<std::vector<std::int64_t>> ascending = {
        {kInt32Min, -32768}, {-5, 32767}, {-4, -32768}, {-1, -1}, {-1, 0}, {0, -1},
        {0, 0},              {0, 1},      {1, -32768},  {7, 9},   {7, 10}, {kInt32Max, 32766},
    };
    std::uint64_t last = 0;
    for (const auto& key : ascending) {
        const std::uint64_t id = IdOf(two, key);
        EXPECT_GT(id, last) << key[0] << "," << key[1];
        EXPECT_LE(id, kMaxKeystoneId);
        last = id;
    }
}

// A key filling all 48 bits has one combination past the id space.
TEST(EmbeddedKeyTest, TheLargestKeyOfAFullWidthFormIsRefused) {
    const catalog::Schema two = Embedded({kTypeValInt32, kTypeValInt16});
    EXPECT_EQ(IdOf(two, {kInt32Max, 32766}), kMaxKeystoneId);
    std::vector<parser::AstValue> largest = {Int(kInt32Max), Int(32767)};
    auto id = EmbeddedKeyId(two, largest);
    ASSERT_FALSE(id.ok());
    EXPECT_EQ(id.status().code(), StatusCode::kOutOfRange);
    // A narrower form has no such key.
    const catalog::Schema one = Embedded({kTypeValInt32});
    EXPECT_EQ(IdOf(one, {kInt32Max}), 0x100000000ull);
}

TEST(EmbeddedKeyTest, AValueTheColumnCannotHoldIsRefused) {
    const catalog::Schema one = Embedded({kTypeValInt32});
    std::vector<parser::AstValue> wide = {Int(5000000000)};
    EXPECT_EQ(EmbeddedKeyId(one, wide).status().code(), StatusCode::kOutOfRange);
    parser::AstValue text;
    text.type = parser::ValueType::kStr;
    text.str_val = "x";
    std::vector<parser::AstValue> wrong = {text};
    EXPECT_FALSE(EmbeddedKeyId(one, wrong).ok());
    std::vector<parser::AstValue> short_key;
    EXPECT_FALSE(EmbeddedKeyId(one, short_key).ok());
}

TEST(EmbeddedKeyTest, AWindowHoldsExactlyTheKeysItsBoundsAdmit) {
    const catalog::Schema two = Embedded({kTypeValInt32, kTypeValInt16});
    // A bare prefix: every key beginning with it.
    const std::vector<parser::AstValue> seven = {Int(7)};
    auto prefix = Window(two, seven, nullptr, nullptr);
    ASSERT_TRUE(prefix.has_value());
    EXPECT_EQ(prefix->lo, IdOf(two, {7, -32768}));
    EXPECT_EQ(prefix->hi, IdOf(two, {7, 32767}));
    // A prefix and a range on the next column.
    const parser::AstValue low = Int(-2);
    const parser::AstValue high = Int(5);
    auto ranged = Window(two, seven, &low, &high);
    ASSERT_TRUE(ranged.has_value());
    EXPECT_EQ(ranged->lo, IdOf(two, {7, -2}));
    EXPECT_EQ(ranged->hi, IdOf(two, {7, 5}));
    // A range on the first column, no prefix: the columns after it whole.
    auto leading = Window(two, {}, &low, &high);
    ASSERT_TRUE(leading.has_value());
    EXPECT_EQ(leading->lo, IdOf(two, {-2, -32768}));
    EXPECT_EQ(leading->hi, IdOf(two, {5, 32767}));
    // The top of the id space: the one unnameable code is clipped.
    const parser::AstValue top = Int(kInt32Max);
    auto last = Window(two, {}, &top, &top);
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->hi, kMaxKeystoneId);
    // The whole key as a prefix is one id.
    const std::vector<parser::AstValue> whole = {Int(7), Int(9)};
    auto point = Window(two, whole, nullptr, nullptr);
    ASSERT_TRUE(point.has_value());
    EXPECT_EQ(point->lo, IdOf(two, {7, 9}));
    EXPECT_EQ(point->hi, point->lo);
}

// A window no key can fill is empty; one that cannot be told is a failure,
// never an empty answer: a value that does not encode is not known to match
// no row.
TEST(EmbeddedKeyTest, AnEmptyWindowIsToldFromOneThatCannotBeRead) {
    const catalog::Schema two = Embedded({kTypeValInt32, kTypeValInt16});
    const parser::AstValue five = Int(5);
    const parser::AstValue two_v = Int(2);
    auto inverted = EmbeddedKeyWindowOf(two, {}, &five, &two_v);
    ASSERT_TRUE(inverted.ok()) << inverted.status().message();
    EXPECT_FALSE(inverted.value().has_value());
    // The one key past the id space, alone.
    const std::vector<parser::AstValue> largest = {Int(kInt32Max), Int(32767)};
    auto unnameable = EmbeddedKeyWindowOf(two, largest, nullptr, nullptr);
    ASSERT_TRUE(unnameable.ok()) << unnameable.status().message();
    EXPECT_FALSE(unnameable.value().has_value());

    EXPECT_FALSE(EmbeddedKeyWindowOf(two, {}, &five, nullptr).ok());     // one end alone
    EXPECT_FALSE(EmbeddedKeyWindowOf(two, {}, nullptr, nullptr).ok());   // nothing bound
    const parser::AstValue wide = Int(5000000000);
    EXPECT_EQ(EmbeddedKeyWindowOf(two, {}, &five, &wide).status().code(),
              StatusCode::kOutOfRange);
    const std::vector<parser::AstValue> three = {Int(1), Int(2), Int(3)};
    EXPECT_FALSE(EmbeddedKeyWindowOf(two, three, nullptr, nullptr).ok());
}

}  // namespace
}  // namespace kds::exec
