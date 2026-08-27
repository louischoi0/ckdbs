#include "kds/exec/index_key.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/catalog/well_known.hpp"
#include "kds/exec/row_codec.hpp"

// The index key encoding (docs/spec/index.md §5, workplan IX01).
//
// One property carries the whole design, and it is what these tests exist to
// falsify: **`memcmp` over the encoding must agree with `CompareValues` over
// the values.** If it can disagree for any declarable key type, the tree
// below it routes descents to the wrong page and an index silently loses
// rows - the failure `cabin.md` §5 calls invisible without a baseline.
//
// The second property is the one truncation rests on: a collapse is only
// ever a *false positive*. Two values that encode alike are still separated
// by the read path's re-check; two values that encode out of order are not
// recoverable by anything.

namespace kds::exec {
namespace {

using catalog::kTypeValChar;
using catalog::kTypeValDate;
using catalog::kTypeValDecimal;
using catalog::kTypeValDecimalWide;
using catalog::kTypeValInt16;
using catalog::kTypeValInt32;
using catalog::kTypeValInt64;
using catalog::kTypeValInt8;
using catalog::kTypeValTimestamp;
using catalog::kTypeValUint64;
using catalog::kTypeValVarchar;

catalog::SysColumnRow MakeColumn(std::uint32_t type_val, std::uint32_t len = 0) {
    catalog::SysColumnRow col{};
    col.oid = 1;
    col.rel_id = 1;
    col.pos = 1;
    catalog::SetName(col.name, "k");
    col.type_val = type_val;
    col.len = len;
    return col;
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

parser::AstValue Dec(std::int64_t unscaled, std::uint8_t scale) {
    parser::AstValue out;
    out.type = parser::ValueType::kDecimal;
    out.int_val = unscaled;
    out.scale = scale;
    return out;
}

parser::AstValue WideDec(std::int64_t hi, std::int64_t lo, std::uint8_t scale) {
    parser::AstValue out;
    out.type = parser::ValueType::kDecimalWide;
    out.dec_hi = hi;
    out.int_val = lo;
    out.scale = scale;
    return out;
}

parser::AstValue Uint(std::uint64_t v) {
    parser::AstValue out;
    out.type = parser::ValueType::kInt;
    out.int_val = static_cast<std::int64_t>(v);
    if (v > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        out.raw_int_text = std::to_string(v);
    }
    return out;
}

std::vector<std::byte> Encode(const catalog::SysColumnRow& col, const parser::AstValue& value) {
    auto width = IndexKeyColumnWidth(col);
    EXPECT_TRUE(width.ok()) << width.status().message();
    std::vector<std::byte> out(width.value());
    Status s = EncodeIndexKeyColumn(col, value, out);
    EXPECT_TRUE(s.ok()) << s.message();
    return out;
}

int Sign(int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); }

int MemcmpSign(const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
    EXPECT_EQ(a.size(), b.size());
    return Sign(std::memcmp(a.data(), b.data(), a.size()));
}

// What CompareValues says, as a three-way sign, so the two can be compared
// directly. This is the reference the encoding has to reproduce; if it ever
// gains a case the encoder does not, this is the function that will notice.
int ValueSign(std::uint32_t type_val, const parser::AstValue& a, const parser::AstValue& b) {
    if (CompareValues(type_val, a, b, parser::CompareOp::kLt)) return -1;
    if (CompareValues(type_val, a, b, parser::CompareOp::kGt)) return 1;
    EXPECT_TRUE(CompareValues(type_val, a, b, parser::CompareOp::kEq));
    return 0;
}

// The whole point, applied to one type's ordered sample. Every pair, both
// directions, including each value against itself.
void ExpectOrderAgrees(const catalog::SysColumnRow& col,
                       const std::vector<parser::AstValue>& ascending) {
    std::vector<std::vector<std::byte>> encoded;
    encoded.reserve(ascending.size());
    for (const parser::AstValue& v : ascending) encoded.push_back(Encode(col, v));

    for (std::size_t i = 0; i < ascending.size(); ++i) {
        for (std::size_t j = 0; j < ascending.size(); ++j) {
            const int by_bytes = MemcmpSign(encoded[i], encoded[j]);
            const int by_value = ValueSign(col.type_val, ascending[i], ascending[j]);
            EXPECT_EQ(by_bytes, by_value)
                << "type_val " << col.type_val << ", sample " << i << " vs " << j;
            // The sample is declared ascending, so the encoding must be too.
            const int declared = i < j ? -1 : (i > j ? 1 : 0);
            EXPECT_EQ(by_bytes, declared) << "type_val " << col.type_val << ", sample " << i
                                          << " vs " << j;
        }
    }
}

// ---- The agreement property, per type -----------------------------------

TEST(IndexKeyTest, SignedIntegersEncodeInValueOrder) {
    // The extremes matter more than the middle: a sign flip that forgets to
    // mask leaves INT_MIN encoding above 0 for every width below 8.
    ExpectOrderAgrees(MakeColumn(kTypeValInt8),
                      {Int(-128), Int(-1), Int(0), Int(1), Int(127)});
    ExpectOrderAgrees(MakeColumn(kTypeValInt16),
                      {Int(-32768), Int(-1), Int(0), Int(1), Int(32767)});
    ExpectOrderAgrees(MakeColumn(kTypeValInt32),
                      {Int(-2147483648LL), Int(-70000), Int(-1), Int(0), Int(1), Int(70000),
                       Int(2147483647LL)});
    ExpectOrderAgrees(
        MakeColumn(kTypeValInt64),
        {Int(std::numeric_limits<std::int64_t>::min()), Int(-1), Int(0), Int(1),
         Int(std::numeric_limits<std::int64_t>::max())});
}

TEST(IndexKeyTest, DateAndTimestampRideTheIntegerArm) {
    // A DATE *is* an int32 of epoch days and a TIMESTAMP an int64 of epoch
    // micros (docs/spec/types.md), which is why neither needs code of its
    // own here - and why a pre-epoch value must still order correctly.
    ExpectOrderAgrees(MakeColumn(kTypeValDate), {Int(-719162), Int(-1), Int(0), Int(20672)});
    ExpectOrderAgrees(MakeColumn(kTypeValTimestamp),
                      {Int(-62135596800000000LL), Int(-1), Int(0), Int(1786000000000000LL)});
}

TEST(IndexKeyTest, Uint64UsesTheFullUnsignedRange) {
    // No sign flip here, and the value above INT64_MAX rides in
    // raw_int_text - the one arrangement that lets the top half of the range
    // survive an AstValue at all.
    ExpectOrderAgrees(MakeColumn(kTypeValUint64),
                      {Uint(0), Uint(1), Uint(9223372036854775807ULL),
                       Uint(9223372036854775808ULL), Uint(18446744073709551615ULL)});
}

TEST(IndexKeyTest, DecimalsEncodeInValueOrderAtBothWidths) {
    auto narrow = MakeColumn(kTypeValDecimal, catalog::PackDecimalLen(10, 2));
    ExpectOrderAgrees(narrow, {Dec(-999999, 2), Dec(-1, 2), Dec(0, 2), Dec(1, 2), Dec(1234, 2)});

    auto wide = MakeColumn(kTypeValDecimalWide, catalog::PackDecimalLen(30, 2));
    // Spanning the high half is the case that catches a low-half-first
    // concatenation: -1 and +1 differ only in bits the high half owns.
    ExpectOrderAgrees(wide, {WideDec(-1, 0, 2), WideDec(-1, -1, 2), WideDec(0, 0, 2),
                             WideDec(0, 1, 2), WideDec(1, 0, 2)});
}

TEST(IndexKeyTest, StringsEncodeInByteOrder) {
    ExpectOrderAgrees(MakeColumn(kTypeValVarchar), {Str(""), Str("a"), Str("ab"), Str("b")});
    ExpectOrderAgrees(MakeColumn(kTypeValChar, 8), {Str(""), Str("a"), Str("ab"), Str("b")});
}

// ---- Truncation (spec §6): collapses, never inversions -------------------

TEST(IndexKeyTest, StringsSharingThePrefixCollapseButNeverInvert) {
    const auto col = MakeColumn(kTypeValVarchar);
    const std::string base(kIndexStringKeyBytes, 'x');

    // Same prefix, different tails: one key, which is a false positive the
    // read path's re-check removes.
    EXPECT_EQ(0, MemcmpSign(Encode(col, Str(base + "aaa")), Encode(col, Str(base + "zzz"))));

    // Differing inside the prefix still orders, which is the half that
    // cannot be allowed to collapse.
    std::string lower = base;
    lower[kIndexStringKeyBytes - 1] = 'a';
    EXPECT_EQ(-1, MemcmpSign(Encode(col, Str(lower)), Encode(col, Str(base))));
}

TEST(IndexKeyTest, ZeroPaddingCollapsesAnEmbeddedNul) {
    // "a" and "a\0" encode alike, because a short value is zero-padded and
    // an embedded NUL is not escaped. Stated in the header as benign and
    // pinned here so it stays a known collapse rather than a discovery.
    const auto col = MakeColumn(kTypeValVarchar);
    EXPECT_EQ(0, MemcmpSign(Encode(col, Str("a")), Encode(col, Str(std::string("a\0", 2)))));
}

TEST(IndexKeyTest, ACharColumnSpendsOnlyItsDeclaredWidth) {
    auto narrow = IndexKeyColumnWidth(MakeColumn(kTypeValChar, 8));
    ASSERT_TRUE(narrow.ok());
    EXPECT_EQ(8u + kIndexKeyDiscriminatorSize, narrow.value());

    // ...and never more than the prefix budget.
    auto wide = IndexKeyColumnWidth(MakeColumn(kTypeValChar, 4096));
    ASSERT_TRUE(wide.ok());
    EXPECT_EQ(kIndexStringKeyBytes + kIndexKeyDiscriminatorSize, wide.value());
}

// ---- Composite keys -----------------------------------------------------

TEST(IndexKeyTest, ACompositeKeyOrdersColumnByColumn) {
    const std::vector<catalog::SysColumnRow> cols{MakeColumn(kTypeValInt32),
                                                  MakeColumn(kTypeValVarchar)};
    auto width = IndexKeyWidth(cols);
    ASSERT_TRUE(width.ok());
    EXPECT_EQ(width.value(), (4u + 1u) + (kIndexStringKeyBytes + 1u));

    const auto encode = [&](std::int64_t a, const char* b) {
        std::vector<std::byte> out(width.value());
        const std::vector<parser::AstValue> values{Int(a), Str(b)};
        Status s = EncodeIndexKey(cols, values, out);
        EXPECT_TRUE(s.ok()) << s.message();
        return out;
    };

    // The leading column dominates, whatever the trailing one says.
    EXPECT_EQ(-1, MemcmpSign(encode(1, "zzz"), encode(2, "aaa")));
    // Equal leading column: the second decides.
    EXPECT_EQ(-1, MemcmpSign(encode(2, "aaa"), encode(2, "zzz")));
    EXPECT_EQ(0, MemcmpSign(encode(2, "aaa"), encode(2, "aaa")));
}

TEST(IndexKeyTest, APrefixEncodesAsABytedPrefixOfTheWholeKey) {
    // What makes a prefix seek work with no second code path: encoding the
    // first column alone produces exactly the leading bytes of the full key.
    const std::vector<catalog::SysColumnRow> cols{MakeColumn(kTypeValInt32),
                                                  MakeColumn(kTypeValInt64)};
    auto full_width = IndexKeyWidth(cols);
    ASSERT_TRUE(full_width.ok());

    std::vector<std::byte> full(full_width.value());
    const std::vector<parser::AstValue> both{Int(7), Int(9)};
    ASSERT_TRUE(EncodeIndexKey(cols, both, full).ok());

    auto lead_width = IndexKeyColumnWidth(cols[0]);
    ASSERT_TRUE(lead_width.ok());
    std::vector<std::byte> lead(lead_width.value());
    const std::vector<parser::AstValue> one{Int(7)};
    ASSERT_TRUE(EncodeIndexKey(std::span(cols).subspan(0, 1), one, lead).ok());

    EXPECT_EQ(0, std::memcmp(full.data(), lead.data(), lead.size()));
}

TEST(IndexKeyTest, ZeroBytesSortBelowEveryEncodedValue) {
    // The floor the tree's zero-padded probes rest on (index_page.hpp): the
    // discriminator is 1 for every value that exists, so an all-zero key is
    // below all of them.
    const auto col = MakeColumn(kTypeValInt32);
    const std::vector<parser::AstValue> extremes{Int(std::numeric_limits<std::int32_t>::min()),
                                                 Int(0),
                                                 Int(std::numeric_limits<std::int32_t>::max())};
    auto width = IndexKeyColumnWidth(col);
    ASSERT_TRUE(width.ok());
    const std::vector<std::byte> floor(width.value(), std::byte{0});
    for (const parser::AstValue& v : extremes) {
        EXPECT_EQ(-1, MemcmpSign(floor, Encode(col, v)));
    }
}

// ---- Refusals -----------------------------------------------------------

TEST(IndexKeyTest, AnUncoercedLiteralIsRefusedRatherThanParsed) {
    // The rule that has already cost this engine rows: a written literal
    // reaches a key only through exec::CoerceLiteralToColumn. A second
    // parser here is how the Cabin came to key on one form and read on
    // another (docs/spec/types.md §3.1).
    const auto date = MakeColumn(kTypeValDate);
    std::vector<std::byte> out(IndexKeyColumnWidth(date).value());
    Status s = EncodeIndexKeyColumn(date, Str("2026-08-07"), out);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(StatusCode::kInvalidArgument, s.code());
}

TEST(IndexKeyTest, CoercionIsIdempotentSoAWriteHookMayReCoerceADecodedRow) {
    // Found by workplan IX06, and wider than the index. The date and
    // timestamp arms of CoerceLiteralToColumn have always accepted a value
    // already in storage form; the two decimal arms refused one.
    //
    // A write hook re-coerces a *decoded* row - an UPDATE carries one - so a
    // decimal column arrived as kDecimal and was rejected. The Cabin's hook
    // absorbs a coercion failure by un-observing, so a Cabin on a decimal
    // column was silently destroyed by the first UPDATE touching its
    // relation. The index hook cannot absorb and would have failed the
    // statement.
    const auto narrow = MakeColumn(kTypeValDecimal, catalog::PackDecimalLen(10, 2));
    parser::AstValue value = Dec(1234, 2);
    ASSERT_TRUE(CoerceLiteralToColumn(narrow, value).ok());
    EXPECT_EQ(value.type, parser::ValueType::kDecimal);
    EXPECT_EQ(value.int_val, 1234);
    EXPECT_EQ(value.scale, 2);

    const auto wide = MakeColumn(kTypeValDecimalWide, catalog::PackDecimalLen(30, 4));
    parser::AstValue wide_value = WideDec(1, 2, 4);
    ASSERT_TRUE(CoerceLiteralToColumn(wide, wide_value).ok());
    EXPECT_EQ(wide_value.type, parser::ValueType::kDecimalWide);
    EXPECT_EQ(wide_value.dec_hi, 1);
    EXPECT_EQ(wide_value.int_val, 2);

    // The date arm's behaviour, unchanged and now matched by the two above.
    const auto date = MakeColumn(kTypeValDate);
    parser::AstValue day = Int(20672);
    ASSERT_TRUE(CoerceLiteralToColumn(date, day).ok());
    EXPECT_EQ(day.int_val, 20672);

    // Idempotent, not permissive: a scale that disagrees is still refused
    // rather than rescaled.
    parser::AstValue wrong = Dec(1234, 3);
    EXPECT_FALSE(CoerceLiteralToColumn(narrow, wrong).ok());
}

TEST(IndexKeyTest, ADecimalCarryingTheWrongScaleIsRefusedNeverRescaled) {
    const auto col = MakeColumn(kTypeValDecimal, catalog::PackDecimalLen(10, 2));
    std::vector<std::byte> out(IndexKeyColumnWidth(col).value());
    Status s = EncodeIndexKeyColumn(col, Dec(1234, 3), out);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(std::string::npos, s.message().find("does not rescale"));
}

TEST(IndexKeyTest, AnOutOfRangeIntegerIsRefusedRatherThanTruncated) {
    // Reachable only from a written literal - a decoded row cannot produce
    // one - and a compile-time answer for the caller, never a silent key
    // that probes the wrong bucket.
    const auto col = MakeColumn(kTypeValInt32);
    std::vector<std::byte> out(IndexKeyColumnWidth(col).value());
    Status s = EncodeIndexKeyColumn(col, Int(99999999999LL), out);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(StatusCode::kOutOfRange, s.code());
}

TEST(IndexKeyTest, ANullIsRefusedBecauseItsPositionIsUndecided) {
    const auto col = MakeColumn(kTypeValInt32);
    std::vector<std::byte> out(IndexKeyColumnWidth(col).value());
    parser::AstValue null_value;  // defaults to kNull
    Status s = EncodeIndexKeyColumn(col, null_value, out);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(StatusCode::kUnsupported, s.code());
}

TEST(IndexKeyTest, AFloatColumnIsRefusedAtWidth) {
    auto width = IndexKeyColumnWidth(MakeColumn(catalog::kTypeValFloat));
    EXPECT_FALSE(width.ok());
    EXPECT_EQ(StatusCode::kUnsupported, width.status().code());
}

TEST(IndexKeyTest, AnEmptyKeyIsRefused) {
    auto width = IndexKeyWidth({});
    EXPECT_FALSE(width.ok());
    EXPECT_EQ(StatusCode::kInvalidArgument, width.status().code());
}

}  // namespace
}  // namespace kds::exec
