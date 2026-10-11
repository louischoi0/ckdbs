#include "kds/storage/keystone.hpp"

#include <limits>
#include <type_traits>

#include <gtest/gtest.h>

namespace kds {
namespace {

TEST(KeystoneTest, EncodeDecodeRoundTrip) {
    auto word = Keystone::Encode(42, 7, 123);
    ASSERT_TRUE(word.ok());

    Keystone k = Keystone::Decode(word.value());
    EXPECT_EQ(k.id, 42u);
    EXPECT_EQ(k.flags, 7u);
    EXPECT_EQ(k.reserved, 123u);
}

// BK-R1: `reserved` is as wide as its field by type, at the struct and at
// Encode, so a wider value is narrowed at the call rather than shifted out of
// the word inside Encode.
static_assert(std::numeric_limits<decltype(Keystone::reserved)>::digits == kKeystoneReservedBits);
static_assert(std::is_same_v<decltype(&Keystone::Encode),
                             StatusOr<std::uint64_t> (*)(std::uint64_t, std::uint8_t,
                                                         std::uint8_t)>);

TEST(KeystoneTest, ZeroValuesRoundTrip) {
    auto word = Keystone::Encode(0, 0, 0);
    ASSERT_TRUE(word.ok());
    EXPECT_EQ(word.value(), 0u);

    Keystone k = Keystone::Decode(0);
    EXPECT_EQ(k.id, 0u);
    EXPECT_EQ(k.flags, 0u);
    EXPECT_EQ(k.reserved, 0u);
}

TEST(KeystoneTest, RejectsIdBeyondTheIdSpace) {
    auto word = Keystone::Encode(kMaxKeystoneId + 1, 0, 0);
    EXPECT_FALSE(word.ok());
    EXPECT_EQ(word.status().code(), StatusCode::kInvalidArgument);
}

TEST(KeystoneTest, FieldsDoNotBleedIntoEachOther) {
    // id=0 but flags/reserved saturated: decoding must not leak bits
    // from the flags/reserved region back into id.
    auto word = Keystone::Encode(0, 0xFF, 0xFF);
    ASSERT_TRUE(word.ok());

    Keystone k = Keystone::Decode(word.value());
    EXPECT_EQ(k.id, 0u);

    // id saturated but flags/reserved zero: no leakage the other way.
    auto word2 = Keystone::Encode(kMaxKeystoneId, 0, 0);
    ASSERT_TRUE(word2.ok());
    Keystone k2 = Keystone::Decode(word2.value());
    EXPECT_EQ(k2.flags, 0u);
    EXPECT_EQ(k2.reserved, 0u);
}

}  // namespace
}  // namespace kds
