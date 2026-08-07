#include "kds/exec/type_literals.hpp"

#include <string>

#include <gtest/gtest.h>

// TY01 - the three literal parsers (docs/spec-types.md TY3/TY6/TY7,
// docs/workplan-types.md).
//
// These are the **only gate** (TY7): a value is proven here, once, and
// decode never re-validates. So the interesting cases are all the ones that
// must be refused - a parser that accepts `'2026-02-30'` stores a day that
// does not exist and nothing downstream will ever notice.
//
// They are also called from **two** places, the encoder and the compiler's
// literal coercion, which is the reason they are free functions rather than
// arms of the encoder. A predicate that accepted a literal the encoder
// rejected would make `WHERE d = '2026-02-30'` and an INSERT of the same
// text disagree about what the database contains.

namespace kds::exec {
namespace {

// ---- DATE ---------------------------------------------------------------

TEST(TypeLiteralsTest, TheEpochIsDayZero) {
    auto day = ParseDateLiteral("1970-01-01");
    ASSERT_TRUE(day.ok()) << day.status().message();
    EXPECT_EQ(day.value(), 0);
}

TEST(TypeLiteralsTest, DatesRoundTripThroughTheirRendering) {
    for (const char* text : {"1900-01-01", "1969-12-31", "1970-01-01", "2000-02-29",
                             "2026-08-07", "2999-12-31"}) {
        auto day = ParseDateLiteral(text);
        ASSERT_TRUE(day.ok()) << text << ": " << day.status().message();
        EXPECT_EQ(FormatDate(day.value()), text);
    }
}

TEST(TypeLiteralsTest, TheRangeEdgesAreTheProposedOnes) {
    // §6.1's `[PROPOSED]` window, pinned so moving it is a visible change
    // rather than a silent one.
    EXPECT_EQ(ParseDateLiteral("1900-01-01").value(), kMinEpochDay);
    EXPECT_EQ(ParseDateLiteral("2999-12-31").value(), kMaxEpochDay);
    EXPECT_EQ(ParseDateLiteral("1899-12-31").status().code(), StatusCode::kOutOfRange);
    EXPECT_EQ(ParseDateLiteral("3000-01-01").status().code(), StatusCode::kOutOfRange);
}

TEST(TypeLiteralsTest, LeapYearsAreRealLeapYears) {
    EXPECT_TRUE(ParseDateLiteral("2024-02-29").ok());   // divisible by 4
    EXPECT_TRUE(ParseDateLiteral("2000-02-29").ok());   // divisible by 400
    EXPECT_FALSE(ParseDateLiteral("1900-02-29").ok());  // divisible by 100, not 400
    EXPECT_FALSE(ParseDateLiteral("2023-02-29").ok());
}

TEST(TypeLiteralsTest, AnImpossibleCalendarDateIsRefused) {
    // Not silently rolled into the next month, which is the behaviour that
    // makes a bad import invisible.
    for (const char* text : {"2026-02-30", "2026-04-31", "2026-13-01", "2026-00-01",
                             "2026-01-00", "2026-01-32"}) {
        EXPECT_FALSE(ParseDateLiteral(text).ok()) << text;
    }
}

TEST(TypeLiteralsTest, TheDateShapeIsExact) {
    // Zero-padding required, no time part, no slashes, nothing trailing.
    for (const char* text : {"2026-8-07", "2026-08-7", "26-08-07", "2026/08/07",
                             "2026-08-07 ", "2026-08-07T00:00:00", "", "notadate"}) {
        EXPECT_FALSE(ParseDateLiteral(text).ok()) << "'" << text << "'";
    }
}

// ---- TIMESTAMP ----------------------------------------------------------

TEST(TypeLiteralsTest, TimestampsRoundTrip) {
    for (const char* text : {"1970-01-01 00:00:00", "2026-08-07 09:15:00",
                             "2026-08-07 09:15:00.250000", "1969-12-31 23:59:59",
                             "2999-12-31 23:59:59.999999"}) {
        auto micros = ParseTimestampLiteral(text);
        ASSERT_TRUE(micros.ok()) << text << ": " << micros.status().message();
        EXPECT_EQ(FormatTimestamp(micros.value()), text);
    }
}

TEST(TypeLiteralsTest, AShortFractionScalesUpBecauseItIsPositional) {
    // `.5` is half a second, not five microseconds.
    EXPECT_EQ(ParseTimestampLiteral("1970-01-01 00:00:00.5").value(), 500'000);
    EXPECT_EQ(ParseTimestampLiteral("1970-01-01 00:00:00.05").value(), 50'000);
    EXPECT_EQ(ParseTimestampLiteral("1970-01-01 00:00:00.000001").value(), 1);
}

TEST(TypeLiteralsTest, MoreThanSixFractionalDigitsIsRefused) {
    // Truncating would store a different instant than the one written.
    EXPECT_FALSE(ParseTimestampLiteral("1970-01-01 00:00:00.1234567").ok());
    EXPECT_FALSE(ParseTimestampLiteral("1970-01-01 00:00:00.").ok());
}

TEST(TypeLiteralsTest, TimeFieldsAreRangeChecked) {
    for (const char* text : {"2026-08-07 24:00:00", "2026-08-07 00:60:00",
                             "2026-08-07 00:00:60"}) {
        EXPECT_FALSE(ParseTimestampLiteral(text).ok()) << text;
    }
}

TEST(TypeLiteralsTest, ADateIsNotAcceptedAsATimestamp) {
    // Deliberately not promoted to midnight: the two are different columns,
    // and widening one literal into the other hides a schema mismatch.
    EXPECT_FALSE(ParseTimestampLiteral("2026-08-07").ok());
}

TEST(TypeLiteralsTest, ABeforeEpochTimestampRendersInTheDayItFallsIn) {
    // Floor division, not truncation toward zero - otherwise an instant a
    // second before the epoch renders as the day after it.
    auto micros = ParseTimestampLiteral("1969-12-31 23:59:59");
    ASSERT_TRUE(micros.ok());
    EXPECT_LT(micros.value(), 0);
    EXPECT_EQ(FormatTimestamp(micros.value()), "1969-12-31 23:59:59");
}

// ---- DECIMAL ------------------------------------------------------------

TEST(TypeLiteralsTest, ADecimalIsItsUnscaledInteger) {
    EXPECT_EQ(ParseDecimalLiteral("12.34", 10, 2).value(), 1234);
    EXPECT_EQ(ParseDecimalLiteral("-12.34", 10, 2).value(), -1234);
    EXPECT_EQ(ParseDecimalLiteral("0.05", 10, 2).value(), 5);
    EXPECT_EQ(ParseDecimalLiteral("100", 10, 2).value(), 10000);
}

TEST(TypeLiteralsTest, AShorterLiteralIsExactAndScalesUp) {
    // The scale is part of the value's meaning, so '12.3' at scale 2 is
    // 12.30 and equals '12.30' - §6.2's pinned equality.
    EXPECT_EQ(ParseDecimalLiteral("12.3", 10, 2).value(),
              ParseDecimalLiteral("12.30", 10, 2).value());
    EXPECT_EQ(ParseDecimalLiteral("12", 10, 2).value(), 1200);
}

TEST(TypeLiteralsTest, MoreFractionalDigitsThanTheScaleIsRefused) {
    // **The rule that matters most in this file.** Rounding a literal so it
    // fits is a silent wrong answer about money.
    auto refused = ParseDecimalLiteral("12.345", 10, 2);
    ASSERT_FALSE(refused.ok());
    EXPECT_NE(refused.status().message().find("round"), std::string::npos)
        << refused.status().message();
}

TEST(TypeLiteralsTest, DecimalsRoundTripThroughTheirRendering) {
    struct Case { const char* text; std::uint8_t p; std::uint8_t s; };
    for (const Case& c : {Case{"12.34", 10, 2}, Case{"-12.34", 10, 2}, Case{"0.05", 10, 2},
                          Case{"999999999999999999", 18, 0}, Case{"0.000001", 10, 6}}) {
        auto unscaled = ParseDecimalLiteral(c.text, c.p, c.s);
        ASSERT_TRUE(unscaled.ok()) << c.text << ": " << unscaled.status().message();
        EXPECT_EQ(FormatDecimal(unscaled.value(), c.s), c.text) << c.text;
    }
}

TEST(TypeLiteralsTest, TrailingZerosAreRenderedBecauseTheScaleIsDeclared) {
    EXPECT_EQ(FormatDecimal(1230, 2), "12.30");
    EXPECT_EQ(FormatDecimal(1200, 2), "12.00");
    EXPECT_EQ(FormatDecimal(5, 2), "0.05");
    EXPECT_EQ(FormatDecimal(-5, 2), "-0.05");
    EXPECT_EQ(FormatDecimal(1234, 0), "1234");
}

TEST(TypeLiteralsTest, LeadingZerosAreNotSignificant) {
    // '0.05' is two significant digits and fits a decimal(2,2).
    EXPECT_TRUE(ParseDecimalLiteral("0.05", 2, 2).ok());
    EXPECT_TRUE(ParseDecimalLiteral("0.00", 2, 2).ok());
}

TEST(TypeLiteralsTest, AValueTooLargeForItsPrecisionIsRefused) {
    EXPECT_FALSE(ParseDecimalLiteral("1000", 3, 0).ok());
    EXPECT_FALSE(ParseDecimalLiteral("12.34", 3, 2).ok());
    EXPECT_TRUE(ParseDecimalLiteral("9.99", 3, 2).ok());
}

TEST(TypeLiteralsTest, PrecisionAndScaleBoundsAreTheSpecs) {
    // TY2: 1 <= p <= 18, 0 <= s <= p. Beyond 18 is a *future separate
    // type* carrying an int128, never a widening of this one.
    EXPECT_TRUE(CheckDecimalPrecisionScale(1, 0).ok());
    EXPECT_TRUE(CheckDecimalPrecisionScale(18, 18).ok());
    EXPECT_FALSE(CheckDecimalPrecisionScale(0, 0).ok());
    EXPECT_FALSE(CheckDecimalPrecisionScale(19, 0).ok());
    EXPECT_FALSE(CheckDecimalPrecisionScale(5, 6).ok());

    auto refused = CheckDecimalPrecisionScale(19, 0);
    EXPECT_NE(refused.message().find("int128"), std::string::npos) << refused.message();
}

TEST(TypeLiteralsTest, MalformedDecimalsAreRefused) {
    for (const char* text : {"", ".", "12.3.4", "1a", "--1", "12,34"}) {
        EXPECT_FALSE(ParseDecimalLiteral(text, 10, 2).ok()) << "'" << text << "'";
    }
}

TEST(TypeLiteralsTest, TheWidestDecimalRoundTrips) {
    // 18 nines, which is what p = 18 means and comfortably inside int64.
    auto unscaled = ParseDecimalLiteral("999999999999999999", 18, 0);
    ASSERT_TRUE(unscaled.ok()) << unscaled.status().message();
    EXPECT_EQ(unscaled.value(), 999'999'999'999'999'999LL);
}

}  // namespace
}  // namespace kds::exec
