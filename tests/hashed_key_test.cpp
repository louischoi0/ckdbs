#include "kds/exec/hashed_key.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/catalog/well_known.hpp"

// DR-S3f: a hashed primary key's home is a persisted format (DR-R12) - a row
// is placed at the id it computes and a read goes there first - so the hash
// and the canonical bytes are pinned here by value, each cell computed
// outside the engine. A cell that moves is a format event (DR-R5): every
// existing row would be off its home, slower and never wrong (DR-R11).

namespace kds::exec {
namespace {

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

// The hidden Keystone at position 0, then the key's columns in key order.
catalog::Schema Keyed(std::vector<std::uint32_t> types) {
    catalog::Schema schema;
    schema.columns.push_back(catalog::HiddenTupleKeyColumn());
    for (std::size_t i = 0; i < types.size(); ++i) {
        schema.columns.push_back(Column(types[i], static_cast<std::uint16_t>(i + 1)));
    }
    schema.key_form = catalog::kKeyFormHashed;
    return schema;
}

parser::AstValue Int(std::int64_t v) {
    parser::AstValue out;
    out.type = parser::ValueType::kInt;
    out.int_val = v;
    return out;
}

parser::AstValue Str(std::string v) {
    parser::AstValue out;
    out.type = parser::ValueType::kStr;
    out.str_val = std::move(v);
    return out;
}

std::span<const std::byte> Bytes(const std::string& text) {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

TEST(HashedKeyTest, TheHashIsPinned) {
    EXPECT_EQ(Hash47({}), 34500308043975u);
    EXPECT_EQ(Hash47(Bytes("abc")), 52334844194197u);
}

// Canonical bytes: per column a tag, the length in four little-endian
// bytes, the value - an int64's index key encoding (discriminator 1, the
// sign bit flipped, big-endian), a string's whole text.
TEST(HashedKeyTest, AKeysHomeIsPinnedByValue) {
    const catalog::Schema one = Keyed({catalog::kTypeValInt64});
    const std::vector<std::uint16_t> k1 = {1};
    const std::vector<parser::AstValue> five = {Int(5)};
    EXPECT_EQ(HashedKeyHome(one, k1, five), 88900192993118u);

    const catalog::Schema two = Keyed({catalog::kTypeValVarchar, catalog::kTypeValInt64});
    const std::vector<std::uint16_t> k2 = {1, 2};
    const std::vector<parser::AstValue> ann = {Str("ann"), Int(2)};
    EXPECT_EQ(HashedKeyHome(two, k2, ann), 21032303123057u);
    const std::vector<parser::AstValue> empty = {Str(""), Int(-1)};
    EXPECT_EQ(HashedKeyHome(two, k2, empty), 111758754108443u);
}

// A string is hashed whole, never by the 32 bytes an index entry keeps
// (DR-R12): keys that differ past the prefix have homes of their own.
TEST(HashedKeyTest, AStringIsHashedPastTheIndexPrefix) {
    const catalog::Schema schema = Keyed({catalog::kTypeValVarchar});
    const std::vector<std::uint16_t> key = {1};
    const std::string prefix(40, 'x');
    const std::vector<parser::AstValue> a = {Str(prefix + "a")};
    const std::vector<parser::AstValue> b = {Str(prefix + "b")};
    const std::uint64_t home_a = HashedKeyHome(schema, key, a);
    const std::uint64_t home_b = HashedKeyHome(schema, key, b);
    ASSERT_NE(home_a, 0u);
    ASSERT_NE(home_b, 0u);
    EXPECT_NE(home_a, home_b);
}

TEST(HashedKeyTest, EveryHomeIsBelowTheIssuedIds) {
    const catalog::Schema schema = Keyed({catalog::kTypeValInt64});
    const std::vector<std::uint16_t> key = {1};
    std::set<std::uint64_t> homes;
    for (std::int64_t v = -2000; v < 2000; ++v) {
        const std::vector<parser::AstValue> value = {Int(v)};
        const std::uint64_t home = HashedKeyHome(schema, key, value);
        EXPECT_GE(home, 1u);
        EXPECT_LT(home, catalog::kHashedKeyIssuedBase);
        homes.insert(home);
    }
    EXPECT_EQ(homes.size(), 4000u);  // no two of these keys share a home
}

// A key with no home is no error: 0, which is no id. Its row is issued an
// id and its read is the index's.
TEST(HashedKeyTest, AKeyThatDoesNotEncodeHasNoHome) {
    const catalog::Schema schema = Keyed({catalog::kTypeValInt8, catalog::kTypeValInt64});
    const std::vector<std::uint16_t> key = {1, 2};
    const std::vector<parser::AstValue> wide = {Int(1000), Int(1)};  // past an int8
    EXPECT_EQ(HashedKeyHome(schema, key, wide), 0u);
    parser::AstValue null;
    const std::vector<parser::AstValue> with_null = {null, Int(1)};
    EXPECT_EQ(HashedKeyHome(schema, key, with_null), 0u);
    const std::vector<parser::AstValue> short_row = {Int(1)};
    EXPECT_EQ(HashedKeyHome(schema, key, short_row), 0u);
    const std::vector<std::uint16_t> past = {1, 9};
    const std::vector<parser::AstValue> fits = {Int(1), Int(1)};
    EXPECT_EQ(HashedKeyHome(schema, past, fits), 0u);
    EXPECT_NE(HashedKeyHome(schema, key, fits), 0u);
}

}  // namespace
}  // namespace kds::exec
