#include "kds/exec/type_literals.hpp"

#include <string>

#include <gtest/gtest.h>

// TY01 - the three literal parsers (docs/spec/types.md TY3/TY6/TY7,
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

// ---- The wide decimal (TY2's separate int128 type, 2026-08-07) -----------

TEST(TypeLiteralsTest, AWideDecimalParsesBeyondInt64AndRendersBack) {
    // 24 significant digits - unrepresentable in the 8-byte type by
    // construction - through the shared digit walk and back through the
    // hand-peeled renderer.
    auto unscaled = ParseDecimalLiteralWide("12345678901234567890.1234", 24, 4);
    ASSERT_TRUE(unscaled.ok()) << unscaled.status().message();
    EXPECT_EQ(FormatDecimalWide(unscaled.value(), 4), "12345678901234567890.1234");

    auto negative = ParseDecimalLiteralWide("-0.0001", 24, 4);
    ASSERT_TRUE(negative.ok());
    EXPECT_EQ(FormatDecimalWide(negative.value(), 4), "-0.0001");
}

TEST(TypeLiteralsTest, TheWidestWideDecimalIs38Nines) {
    // p = 38 because 10^38 - 1 < 2^127; one more digit does not fit and is
    // refused by the digit cap, not by a wrapped value.
    const std::string nines(38, '9');
    auto unscaled = ParseDecimalLiteralWide(nines, 38, 0);
    ASSERT_TRUE(unscaled.ok()) << unscaled.status().message();
    EXPECT_EQ(FormatDecimalWide(unscaled.value(), 0), nines);
    EXPECT_EQ(FormatDecimalWide(-unscaled.value(), 0), "-" + nines);

    EXPECT_FALSE(ParseDecimalLiteralWide(std::string(39, '9'), 38, 0).ok());
}

TEST(TypeLiteralsTest, WideBoundsAreExclusiveOfTheNarrowOnes) {
    // One declaration selects exactly one type: the wide checker refuses
    // p <= 18 toward `decimal(p, s)` and p > 38 outright, and the scale
    // rule is the same rule at either width.
    EXPECT_FALSE(CheckDecimalWidePrecisionScale(18, 0).ok());
    EXPECT_FALSE(CheckDecimalWidePrecisionScale(39, 0).ok());
    EXPECT_FALSE(CheckDecimalWidePrecisionScale(24, 25).ok());
    EXPECT_TRUE(CheckDecimalWidePrecisionScale(19, 0).ok());
    EXPECT_TRUE(CheckDecimalWidePrecisionScale(38, 38).ok());
}

TEST(TypeLiteralsTest, TheWideParserSharesTheNarrowRules) {
    // Same body, wider register: scale overflow, precision overflow and
    // malformed input answer exactly as the narrow parser answers them.
    EXPECT_FALSE(ParseDecimalLiteralWide("1.234", 24, 2).ok());   // scale
    EXPECT_FALSE(ParseDecimalLiteralWide(std::string(25, '1'), 24, 0).ok());  // precision
    EXPECT_FALSE(ParseDecimalLiteralWide("12.3.4", 24, 2).ok());  // malformed
    // And a short literal scales up exactly.
    auto scaled = ParseDecimalLiteralWide("5", 20, 3);
    ASSERT_TRUE(scaled.ok());
    EXPECT_EQ(FormatDecimalWide(scaled.value(), 3), "5.000");
}

}  // namespace
}  // namespace kds::exec

// ---- TY03: the DDL -------------------------------------------------------

#include <variant>

#include "kds/parser/parser.hpp"

namespace kds::parser {
namespace {

const ColumnDef& ColumnOf(const StatusOr<Statement>& parsed, std::size_t at) {
    static const ColumnDef kEmpty;
    if (!parsed.ok()) return kEmpty;
    const auto& ct = std::get<CreateTableStmt>(parsed.value());
    if (at >= ct.columns.size()) return kEmpty;
    return ct.columns[at];
}

TEST(TypeDdlTest, TheThreeTypeNamesParse) {
    for (const char* type : {"date", "timestamp"}) {
        const std::string sql = std::string("CREATE TABLE t (id int64, x ") + type + ")";
        auto parsed = Parse(sql);
        ASSERT_TRUE(parsed.ok()) << sql << ": " << parsed.status().message();
        EXPECT_EQ(ColumnOf(parsed, 1).type_name, type);
        EXPECT_FALSE(ColumnOf(parsed, 1).has_precision);
    }
}

TEST(TypeDdlTest, DecimalCarriesItsPrecisionAndScale) {
    auto parsed = Parse("CREATE TABLE t (id int64, price decimal(10, 2))");
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    const ColumnDef& col = ColumnOf(parsed, 1);
    EXPECT_TRUE(col.has_precision);
    EXPECT_EQ(col.precision, 10u);
    EXPECT_EQ(col.scale, 2u);
}

TEST(TypeDdlTest, ABareDecimalIsRefusedWithItsPosition) {
    // **Never defaulted.** A default scale is a silent decision about what
    // a stored value means, and the parser is the last layer that can tell
    // "said nothing" from "said zero".
    auto parsed = Parse("CREATE TABLE t (id int64, price decimal)");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(parsed.status().message().find("byte 32"), std::string::npos)
        << parsed.status().message();
    EXPECT_NE(parsed.status().message().find("no default scale"), std::string::npos)
        << parsed.status().message();
}

TEST(TypeDdlTest, AHalfWrittenDecimalIsASyntaxError) {
    for (const char* sql : {"CREATE TABLE t (id int64, p decimal(10))",
                            "CREATE TABLE t (id int64, p decimal(10,))",
                            "CREATE TABLE t (id int64, p decimal(,2))",
                            "CREATE TABLE t (id int64, p decimal(10 2))",
                            "CREATE TABLE t (id int64, p decimal(-1, 2))",
                            "CREATE TABLE t (id int64, p decimal('a', 2))"}) {
        EXPECT_FALSE(Parse(sql).ok()) << sql;
    }
}

TEST(TypeDdlTest, ATypeThatTakesNoArgumentsRefusesThem) {
    auto parsed = Parse("CREATE TABLE t (id int64, x int64(10, 2))");
    ASSERT_FALSE(parsed.ok());
    EXPECT_NE(parsed.status().message().find("takes no arguments"), std::string::npos)
        << parsed.status().message();
}

TEST(TypeDdlTest, AColumnMayStillBeNamedDate) {
    // The type names are unreserved, like every keyword this parser matches
    // by text - so `date` stays available as a column name everywhere it
    // was, which is the same argument the aggregate grammar rests on.
    auto parsed = Parse("CREATE TABLE t (id int64, date varchar, timestamp int64)");
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    EXPECT_EQ(ColumnOf(parsed, 1).name, "date");
    EXPECT_EQ(ColumnOf(parsed, 2).name, "timestamp");

    EXPECT_TRUE(Parse("SELECT date FROM t WHERE date = 'x'").ok());
    EXPECT_TRUE(Parse("SELECT COUNT(*) FROM t GROUP BY date").ok());
}

TEST(TypeDdlTest, ADecimalColumnMayBeNamedDecimal) {
    auto parsed = Parse("CREATE TABLE t (id int64, decimal decimal(4, 1))");
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    EXPECT_EQ(ColumnOf(parsed, 1).name, "decimal");
    EXPECT_EQ(ColumnOf(parsed, 1).precision, 4u);
}

}  // namespace
}  // namespace kds::parser
