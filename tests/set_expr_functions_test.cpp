// **Scalar functions, date arithmetic and decimal division in a SET value**
// (BJ-S6, BJ-Q3 (a), BJ-Q4 (a), BJ-Q5).
//
// The clock is pinned, so `NOW()` and `CURRENT_DATE` are values a cell can
// read: 2026-10-10 12:00:00 UTC.

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/functions.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"

namespace kds::server {
namespace {

constexpr std::int64_t kNoon = 1791633600000000;  // 2026-10-10 12:00:00 UTC, day 20736

class SetExprFunctionsTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
        Exec("CREATE TABLE t (id int64, i64 int64, i32 int32, i8 int8, d decimal(10,2), "
             "e decimal(10,0), w decimal(30,4), s varchar, c char(8), dt date, d2 date, "
             "ts timestamp, n int64 NULL, o varchar NULL, f decimal(10,1))");
        Exec("INSERT INTO t VALUES (1, -7, 100, 5, '12.50', '3', '3.1415', '  Hello World  ', "
             "'abc', '2026-03-04', '2026-03-01', '2026-03-04 05:06:07', NULL, NULL, '2.5')");
        Exec("INSERT INTO t VALUES (2, 0, -100, -5, '-1.25', '8', '-0.5000', 'x', 'zz', "
             "'2026-12-31', '2027-01-02', '1969-12-31 23:00:00', 3, 'y', '-2.5')");
    }

    std::string Run(const std::string& line) { return dispatcher_->Dispatch(line).response; }
    void Exec(const std::string& line) {
        const std::string out = Run(line);
        ASSERT_NE(out.substr(0, 3), "ERR") << line << " -> " << out;
    }
    std::string At(const std::string& column, int id) {
        const std::string out = Run("SELECT " + column + " FROM t WHERE id = " + std::to_string(id));
        const std::string header = column + "\\n";
        return out.rfind(header, 0) == 0 ? out.substr(header.size()) : out;
    }
    bool Types(const std::string& set) {
        return Run("UPDATE t SET " + set + " WHERE id = 999").rfind("UPDATED 0", 0) == 0;
    }
    std::string Refusal(const std::string& set) {
        const std::string out = Run("UPDATE t SET " + set + " WHERE id = 999");
        return out.rfind("UPDATED 0", 0) == 0 ? "" : out;
    }

    exec::ScopedStatementClockForTest clock_{kNoon};
    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

TEST_F(SetExprFunctionsTest, AbsAndSignKeepTheirArgumentsType) {
    Exec("UPDATE t SET i64 = ABS(i64) WHERE id = 1");
    EXPECT_EQ(At("i64", 1), "7");
    Exec("UPDATE t SET d = ABS(d) WHERE id = 2");
    EXPECT_EQ(At("d", 2), "1.25");
    Exec("UPDATE t SET d = SIGN(d) WHERE id >= 1");
    EXPECT_EQ(At("d", 1), "1.00");
    EXPECT_EQ(At("d", 2), "1.00");
    Exec("UPDATE t SET i32 = SIGN(i32) WHERE id >= 1");
    EXPECT_EQ(At("i32", 2), "-1");
    Exec("UPDATE t SET i64 = ABS(-5) WHERE id = 2");
    EXPECT_EQ(At("i64", 2), "5") << "a literal argument is an int64";
    Exec("UPDATE t SET i8 = -128 WHERE id = 1");
    const std::string out = Run("UPDATE t SET i8 = ABS(i8) WHERE id = 1");
    EXPECT_NE(out.find("is outside int8"), std::string::npos) << out;
    EXPECT_EQ(At("i8", 1), "-128");
}

TEST_F(SetExprFunctionsTest, RoundTruncCeilAndFloorYieldTheScaleTheyNameAndTakeTheContextsPrecision) {
    Exec("UPDATE t SET e = ROUND(d, 0) WHERE id >= 1");
    EXPECT_EQ(At("e", 1), "13") << "12.50 rounds away from zero";
    EXPECT_EQ(At("e", 2), "-1");
    Exec("UPDATE t SET e = TRUNC(d) WHERE id >= 1");
    EXPECT_EQ(At("e", 1), "12");
    EXPECT_EQ(At("e", 2), "-1");
    Exec("UPDATE t SET e = CEIL(d) WHERE id >= 1");
    EXPECT_EQ(At("e", 1), "13");
    EXPECT_EQ(At("e", 2), "-1");
    Exec("UPDATE t SET e = FLOOR(d) WHERE id >= 1");
    EXPECT_EQ(At("e", 1), "12");
    EXPECT_EQ(At("e", 2), "-2") << "FLOOR of a negative goes down, TRUNC goes toward zero";
    Exec("UPDATE t SET d = ROUND(w, 2) WHERE id >= 1");
    EXPECT_EQ(At("d", 1), "3.14");
    EXPECT_EQ(At("d", 2), "-0.50");
    Exec("UPDATE t SET w = ROUND(d, 4) WHERE id = 1");
    EXPECT_EQ(At("w", 1), "3.1400") << "more places than the argument has pads";
    Exec("UPDATE t SET f = TRUNC(w, 1) WHERE id = 1");
    EXPECT_EQ(At("f", 1), "3.1");
    Exec("UPDATE t SET w = 3.1415 WHERE id = 1");
    Exec("UPDATE t SET w = CAST(ROUND(w, 3) AS decimal(30, 4)) WHERE id = 1");
    EXPECT_EQ(At("w", 1), "3.1420") << "3.1415 rounds up at the third place";
    Exec("UPDATE t SET w = 3.1415 WHERE id = 1");
    Exec("UPDATE t SET w = CAST(TRUNC(w, 3) AS decimal(30, 4)) WHERE id = 1");
    EXPECT_EQ(At("w", 1), "3.1410") << "TRUNC drops what ROUND would have rounded up";
    Exec("UPDATE t SET d = ROUND(d, 2) WHERE id = 1");
    EXPECT_EQ(At("d", 1), "3.14") << "the same scale is the same type";
    Exec("UPDATE t SET i64 = ROUND(i64, 2) WHERE id = 1");
    EXPECT_EQ(At("i64", 1), "-7");
    Exec("UPDATE t SET d = -2.50 WHERE id = 1");
    Exec("UPDATE t SET e = ROUND(d) WHERE id = 1");
    EXPECT_EQ(At("e", 1), "-3") << "a negative tie rounds away from zero";
    // A scaled result adapts to the precision of what it is assigned to or
    // compared with, never to another scale.
    EXPECT_NE(Refusal("d = ROUND(w, 3)").find("scale 3 but is used as decimal(10,2)"),
              std::string::npos);
    EXPECT_NE(Refusal("i64 = ROUND(d, 0)").find("but is used as int64"), std::string::npos);
    EXPECT_TRUE(Types("d = ROUND(w, 2) + d"));
    EXPECT_TRUE(Types("d = ROUND(w, 2) + 1"));
    EXPECT_TRUE(Types("i8 = CASE WHEN ROUND(d, 0) > 12 THEN 1 ELSE 0 END"));
    EXPECT_NE(Refusal("d = ROUND(w, 2) * ROUND(w, 2)").find("has no type of its own"),
              std::string::npos);
    EXPECT_NE(Refusal("e = ROUND(d, i32)").find("whole-number literal"), std::string::npos);
    EXPECT_NE(Refusal("e = ROUND(d, -1)").find("negative"), std::string::npos);
    EXPECT_NE(Refusal("e = ROUND(d, 39)").find("0 to 38"), std::string::npos);
    EXPECT_TRUE(Types("e = ROUND(ROUND(w, 3), 0)"));
}

TEST_F(SetExprFunctionsTest, ARoundedValueTooWideForItsPrecisionIsRefusedAtTheRow) {
    Exec("UPDATE t SET w = 99999999999.9999 WHERE id = 1");
    const std::string out = Run("UPDATE t SET f = ROUND(w, 1) WHERE id = 1");
    EXPECT_NE(out.find("is outside decimal(10,1)"), std::string::npos) << out;
    EXPECT_EQ(At("f", 1), "2.5");
}

TEST_F(SetExprFunctionsTest, ModIsThePercentOperator) {
    Exec("UPDATE t SET i64 = MOD(i64, 4) WHERE id = 1");
    EXPECT_EQ(At("i64", 1), "-3") << "the dividend's sign";
    Exec("UPDATE t SET d = MOD(d, 5.00) WHERE id = 1");
    EXPECT_EQ(At("d", 1), "2.50");
    EXPECT_NE(Run("UPDATE t SET i64 = MOD(i64, 0) WHERE id = 1").find("division by zero"),
              std::string::npos);
    EXPECT_NE(Refusal("i64 = MOD(i64, i32)").find("int64 and int32"), std::string::npos);
}

TEST_F(SetExprFunctionsTest, ADecimalQuotientHasMoreDigitsAndIsNarrowedByRoundOrCast) {
    Exec("UPDATE t SET d = ROUND(d / 3, 2) WHERE id >= 1");
    EXPECT_EQ(At("d", 1), "4.17");
    EXPECT_EQ(At("d", 2), "-0.42") << "-0.41666... rounds away from zero";
    Exec("UPDATE t SET d = 12.50 WHERE id = 1");
    Exec("UPDATE t SET e = CAST(d / 3 AS decimal(10, 0)) WHERE id = 1");
    EXPECT_EQ(At("e", 1), "4") << "12.50 / 3 = 4.1666...";
    Exec("UPDATE t SET e = CAST(d / d AS decimal(10, 0)) WHERE id = 2");
    EXPECT_EQ(At("e", 2), "1");
    Exec("UPDATE t SET d = 12.52 WHERE id = 1");
    Exec("UPDATE t SET d = CAST(d / 8 AS decimal(10, 2)) WHERE id = 1");
    EXPECT_EQ(At("d", 1), "1.57") << "12.52 / 8 = 1.565, a tie: half away from zero, not half even";
    const std::string zero = Run("UPDATE t SET d = ROUND(d / 0, 2) WHERE id = 1");
    EXPECT_NE(zero.find("division by zero"), std::string::npos) << zero;
    EXPECT_NE(Run("UPDATE t SET d = ROUND(d / (d - d), 2) WHERE id = 1").find("division by zero"),
              std::string::npos);
    // `/` between two decimals needs one type, as every operator does.
    EXPECT_NE(Refusal("d = ROUND(d / e, 2)").find("decimal(10,2) and decimal(10,0)"),
              std::string::npos);
    EXPECT_NE(Refusal("d = d / 2").find("but column 'd' is decimal(10,2)"), std::string::npos);
}

TEST_F(SetExprFunctionsTest, ADivisionIsRoundedOnceNotTwice) {
    // BJ-S6's review: the quotient was rounded to its own scale and then again
    // by ROUND / TRUNC / CAST, so a value just under a tie was pushed over it.
    Exec("CREATE TABLE q (id int64, a decimal(18,2), r decimal(18,4), a38 decimal(38,2), "
         "b38 decimal(38,2), r38 decimal(38,2), c decimal(10,4), n0 decimal(10,0))");
    Exec("INSERT INTO q VALUES (1, '10000000000000.00', '1', '10000000000000.00', "
         "'2000000000000000.01', '1', '1', '7')");
    Exec("UPDATE q SET r = ROUND(a / 200000000000000001, 4) WHERE id = 1");
    EXPECT_EQ(Run("SELECT r FROM q WHERE id = 1"), "r\\n0.0000") << "exactly 0.0000499999...";
    Exec("UPDATE q SET a = 20000000000000.00 WHERE id = 1");
    Exec("UPDATE q SET r = TRUNC(a / 200000000000000001, 4) WHERE id = 1");
    EXPECT_EQ(Run("SELECT r FROM q WHERE id = 1"), "r\\n0.0000");
    Exec("UPDATE q SET c = CAST(a / 200000000000000001 AS decimal(10, 4)) WHERE id = 1");
    EXPECT_EQ(Run("SELECT c FROM q WHERE id = 1"), "c\\n0.0001")
        << "0.0000999999... is just under 0.0001 at four places: it rounds to 0.0001 once";
    Exec("UPDATE q SET r38 = ROUND(a38 / b38, 2) WHERE id = 1");
    EXPECT_EQ(Run("SELECT r38 FROM q WHERE id = 1"), "r38\\n0.00");
    // CEIL and FLOOR of a quotient keep the inexact remainder.
    Exec("UPDATE q SET n0 = CEIL(a38 / b38) WHERE id = 1");
    EXPECT_EQ(Run("SELECT n0 FROM q WHERE id = 1"), "n0\\n1");
    Exec("UPDATE q SET n0 = FLOOR(a38 / (0 - b38)) WHERE id = 1");
    EXPECT_EQ(Run("SELECT n0 FROM q WHERE id = 1"), "n0\\n-1");
}

TEST_F(SetExprFunctionsTest, AQuotientKeepsRoomForItsIntegerDigits) {
    // BJ-S6's review: decimal(30,22) / 2 had no integer digits left and failed
    // for any value. The 16 extra places are as many as leave room.
    Exec("CREATE TABLE z (id int64, x decimal(30,22), y decimal(38,10), r decimal(38,30))");
    Exec("INSERT INTO z VALUES (1, '4', '9999999999999999999999999999.0000000001', '1')");
    Exec("UPDATE z SET x = ROUND(x / 2, 22) WHERE id = 1");
    EXPECT_EQ(Run("SELECT x FROM z WHERE id = 1"), "x\\n2.0000000000000000000000");
    Exec("UPDATE z SET r = x / 3 WHERE id = 1");
    EXPECT_EQ(Run("SELECT r FROM z WHERE id = 1"),
              "r\\n0.666666666666666666666666666667")
        << "decimal(30,22) / 3 is decimal(38,30): eight integer digits and thirty places";
    EXPECT_NE(Run("UPDATE z SET x = x / 2 WHERE id = 1").find("decimal(38,30) but column 'x'"),
              std::string::npos);
    // decimal(38,10) has 28 integer digits to keep: no extra places fit.
    Exec("UPDATE z SET y = y / 2 WHERE id = 1");
    EXPECT_EQ(Run("SELECT y FROM z WHERE id = 1"),
              "y\\n4999999999999999999999999999.5000000001");
}

TEST_F(SetExprFunctionsTest, ScaledDecimalsAdaptWhereverTheyAreUsed) {
    Exec("CREATE TABLE sc (id int64, w decimal(30,4), d53 decimal(5,3), d52 decimal(5,2), "
         "d102 decimal(12,2), d22 decimal(2,2), b bool, i8 int8)");
    Exec("INSERT INTO sc VALUES (1, '123456789.1234', '12.345', '1.00', '0.00', '0.50', 0, 0)");
    const auto Sc = [&](const std::string& what) {
        return Run("SELECT " + what + " FROM sc WHERE id = 1");
    };
    // B3: SIGN of a decimal whose precision is its scale.
    Exec("UPDATE sc SET b = SIGN(d22) > 0 WHERE id = 1");
    EXPECT_EQ(Sc("b"), "b\\n1");
    Exec("UPDATE sc SET i8 = CAST(SIGN(d22) AS int8) WHERE id = 1");
    EXPECT_EQ(Sc("i8"), "i8\\n1");
    Exec("UPDATE sc SET d102 = SIGN(d22) WHERE id = 1");
    EXPECT_EQ(Sc("d102"), "d102\\n1.00");
    // B4: two scaled decimals take the wider precision, whichever is first.
    Exec("UPDATE sc SET b = ROUND(w, 2) = ROUND(d53, 2) WHERE id = 1");
    EXPECT_EQ(Sc("b"), "b\\n0");
    Exec("UPDATE sc SET b = ROUND(d53, 2) = ROUND(w, 2) WHERE id = 1");
    EXPECT_EQ(Sc("b"), "b\\n0");
    Exec("UPDATE sc SET b = COALESCE(ROUND(w, 2), ROUND(d53, 2)) > 0 WHERE id = 1");
    EXPECT_EQ(Sc("b"), "b\\n1");
    // B5: compared with a decimal of the same scale, it takes the wider precision.
    Exec("UPDATE sc SET b = ROUND(w, 2) > d52 WHERE id = 1");
    EXPECT_EQ(Sc("b"), "b\\n1");
    // B6: every rounding form over a decimal adopts its context's precision.
    Exec("UPDATE sc SET d102 = ROUND(d52, 2) WHERE id = 1");
    EXPECT_EQ(Sc("d102"), "d102\\n1.00");
    Exec("UPDATE sc SET d102 = ABS(ROUND(w, 2)) WHERE id = 1");
    EXPECT_EQ(Sc("d102"), "d102\\n123456789.12");
    Exec("UPDATE sc SET d102 = ROUND(ROUND(w, 3), 2) WHERE id = 1");
    EXPECT_EQ(Sc("d102"), "d102\\n123456789.12");
    // A value too wide for the precision it lands in is the row's refusal.
    const std::string wide = Run("UPDATE sc SET d52 = ROUND(w, 2) WHERE id = 1");
    EXPECT_NE(wide.find("is outside decimal(5,2)"), std::string::npos) << wide;
    // ABS and ROUND of NULL are NULL of any type (BJ-R4).
    Exec("CREATE TABLE nn (id int64, d decimal(10,2) NULL, n int64 NULL)");
    Exec("INSERT INTO nn VALUES (1, '1.00', 1)");
    Exec("UPDATE nn SET d = ROUND(NULL, 2) WHERE id = 1");
    EXPECT_EQ(Run("SELECT d FROM nn WHERE id = 1"), "d\\nNULL");
    Exec("UPDATE nn SET d = '3.00' WHERE id = 1");
    Exec("UPDATE nn SET d = ABS(NULL) WHERE id = 1");
    EXPECT_EQ(Run("SELECT d FROM nn WHERE id = 1"), "d\\nNULL");
}

TEST_F(SetExprFunctionsTest, ARowErrorNamesTheFunctionNotTheOperator) {
    Exec("UPDATE t SET i8 = -128 WHERE id = 1");
    const std::string out = Run("UPDATE t SET i8 = ABS(i8) WHERE id = 1");
    EXPECT_NE(out.find("the result of function abs is outside int8"), std::string::npos) << out;
    EXPECT_NE(Refusal("i64 = MOD(i64)").find("'MOD' takes 2 argument(s)"), std::string::npos);
    EXPECT_NE(Refusal("i64 = MOD(i64, 2, 3)").find("'MOD' takes 2 argument(s)"),
              std::string::npos);
    EXPECT_NE(Refusal("e = ROUND(d, 39)").find("0 to 38"), std::string::npos);
    EXPECT_NE(Refusal("e = ROUND(d, 99999999999999999999)").find("0 to 38"), std::string::npos);
}

TEST_F(SetExprFunctionsTest, TextFunctions) {
    Exec("UPDATE t SET c = UPPER(c) WHERE id = 1");
    EXPECT_EQ(At("c", 1), "ABC");
    Exec("UPDATE t SET c = LOWER(c) WHERE id = 1");
    EXPECT_EQ(At("c", 1), "abc");
    Exec("UPDATE t SET i32 = LENGTH('\xc3\xa9') WHERE id = 2");
    EXPECT_EQ(At("i32", 2), "2") << "LENGTH counts bytes";
    Exec("UPDATE t SET s = UPPER('\xc3\xa9" "a') WHERE id = 2");
    EXPECT_EQ(At("s", 2), "\xc3\xa9" "A") << "UPPER touches ASCII letters only";
    Exec("UPDATE t SET s = 'x' WHERE id = 2");
    Exec("UPDATE t SET i32 = LENGTH(s) WHERE id >= 1");
    EXPECT_EQ(At("i32", 1), "15");
    EXPECT_EQ(At("i32", 2), "1");
    Exec("UPDATE t SET s = TRIM(s) WHERE id = 1");
    EXPECT_EQ(At("s", 1), "Hello World");
    Exec("UPDATE t SET s = SUBSTRING(s, 1, 5) WHERE id = 1");
    EXPECT_EQ(At("s", 1), "Hello");
    Exec("UPDATE t SET s = 'Hello World' WHERE id = 1");
    Exec("UPDATE t SET s = SUBSTRING(s, 7) WHERE id = 1");
    EXPECT_EQ(At("s", 1), "World");
    Exec("UPDATE t SET s = SUBSTRING('abcdef', -1, 4) WHERE id = 1");
    EXPECT_EQ(At("s", 1), "ab") << "positions before the first count against the length";
    Exec("UPDATE t SET s = SUBSTRING('abc', 5) || SUBSTRING('abc', 2, 0) || 'z' WHERE id = 1");
    EXPECT_EQ(At("s", 1), "z");
    EXPECT_NE(Run("UPDATE t SET s = SUBSTRING(s, 1, -1) WHERE id = 1").find("negative substring"),
              std::string::npos);
    Exec("UPDATE t SET s = '--x--' WHERE id = 1");
    Exec("UPDATE t SET s = LTRIM(s, '-') || '|' || RTRIM(s, '-') WHERE id = 1");
    EXPECT_EQ(At("s", 1), "x--|--x");
    Exec("UPDATE t SET i32 = POSITION('lo' IN 'hello') WHERE id = 1");
    EXPECT_EQ(At("i32", 1), "4");
    Exec("UPDATE t SET i32 = POSITION('zz' IN 'hello') WHERE id = 1");
    EXPECT_EQ(At("i32", 1), "0");
    Exec("UPDATE t SET s = REPLACE('a-b-c', '-', '+') WHERE id = 1");
    EXPECT_EQ(At("s", 1), "a+b+c");
    Exec("UPDATE t SET s = REPLACE('abc', '', 'x') WHERE id = 1");
    EXPECT_EQ(At("s", 1), "abc") << "an empty search string replaces nothing";
    Exec("UPDATE t SET s = LPAD('7', 3, '0') || RPAD('ab', 5, 'xy') || LPAD('abcdef', 3) WHERE id = 1");
    EXPECT_EQ(At("s", 1), "007abxyxabc");
    Exec("UPDATE t SET s = LPAD('x', 0) || RPAD('x', -3) WHERE id = 1");
    EXPECT_EQ(At("s", 1), "") << "a length of nothing or less is the empty string";
    EXPECT_NE(Run("UPDATE t SET s = LPAD('x', 2000000000) WHERE id = 1").find("builds more than"),
              std::string::npos);
    Exec("UPDATE t SET s = CONCAT('a', c, 'b') WHERE id = 1");
    EXPECT_EQ(At("s", 1), "aabcb");
}

TEST_F(SetExprFunctionsTest, FunctionsAreNullForANullArgumentExceptConcat) {
    Exec("UPDATE t SET n = CAST(LENGTH(o) AS int64) WHERE id >= 1");
    EXPECT_EQ(At("n", 1), "NULL");
    EXPECT_EQ(At("n", 2), "1");
    Exec("UPDATE t SET o = UPPER(o) WHERE id >= 1");
    EXPECT_EQ(At("o", 1), "NULL");
    EXPECT_EQ(At("o", 2), "Y");
    Exec("UPDATE t SET o = CONCAT(o, 'k', NULL) WHERE id = 1");
    EXPECT_EQ(At("o", 1), "k") << "CONCAT skips NULLs";
    Exec("UPDATE t SET o = SUBSTRING(o, NULL) WHERE id = 1");
    EXPECT_EQ(At("o", 1), "NULL");
    Exec("UPDATE t SET n = ABS(n) WHERE id >= 1");
    EXPECT_EQ(At("n", 1), "NULL");
    Exec("UPDATE t SET n = CAST(POSITION('a' IN o) AS int64) WHERE id = 1");
    EXPECT_EQ(At("n", 1), "NULL");
}

TEST_F(SetExprFunctionsTest, ASignatureThatDoesNotFitNamesTheArgument) {
    EXPECT_NE(Refusal("s = UPPER(i64)").find("upper needs text but is given int64"),
              std::string::npos);
    EXPECT_NE(Refusal("s = SUBSTRING(s, 'x')").find("substring needs a whole number"),
              std::string::npos);
    EXPECT_NE(Refusal("s = SUBSTRING(s, 1.5)").find("whole number"), std::string::npos);
    EXPECT_NE(Refusal("i64 = ABS(s)").find("abs needs a number but is given varchar"),
              std::string::npos);
    EXPECT_NE(Refusal("i64 = LENGTH(i64)").find("length needs text"), std::string::npos);
    EXPECT_NE(Refusal("i64 = LENGTH(s)").find("is int32 but column 'i64' is int64"),
              std::string::npos)
        << "LENGTH is an int32 (BJ-R3: no implicit widening)";
    EXPECT_NE(Refusal("s = nosuchfunction(s)").find("unknown function 'nosuchfunction'"),
              std::string::npos);
    EXPECT_NE(Refusal("s = UPPER(s, s)").find("'upper' takes 1 argument(s)"), std::string::npos);
    EXPECT_NE(Refusal("s = SUBSTRING(s)").find("'substring' takes 2 to 3"), std::string::npos);
    EXPECT_NE(Refusal("s = NOW()").find("is timestamp but column 's' is varchar"),
              std::string::npos);
    EXPECT_NE(Refusal("ts = NOW(1)").find("'now' takes 0 argument(s)"), std::string::npos);
    EXPECT_TRUE(Types("s = UPPER(NULL)"));
    EXPECT_TRUE(Types("s = UPPER('x')"));
}

TEST_F(SetExprFunctionsTest, NowAndCurrentDateAreOneValuePerStatement) {
    // A timestamp from before the epoch floors to its own day.
    Exec("UPDATE t SET dt = DATE(ts) WHERE id = 2");
    EXPECT_EQ(At("dt", 2), "1969-12-31");
    Exec("UPDATE t SET ts = NOW() WHERE id >= 1");
    EXPECT_EQ(At("ts", 1), "2026-10-10 12:00:00");
    EXPECT_EQ(At("ts", 2), "2026-10-10 12:00:00");
    Exec("UPDATE t SET dt = CURRENT_DATE WHERE id >= 1");
    EXPECT_EQ(At("dt", 1), "2026-10-10");
    Exec("UPDATE t SET dt = DATE(NOW()) WHERE id = 2");
    EXPECT_EQ(At("dt", 2), "2026-10-10");
    Exec("UPDATE t SET dt = DATE(ts) WHERE id = 1");
    EXPECT_EQ(At("dt", 1), "2026-10-10");
    clock_.Set(kNoon + 86400LL * 1000000LL);
    Exec("UPDATE t SET d2 = CURRENT_DATE + 1 WHERE id = 1");
    EXPECT_EQ(At("d2", 1), "2026-10-12") << "the clock moved a day between the statements";
}

TEST_F(SetExprFunctionsTest, ACurrentDateColumnWinsOverTheFunction) {
    Exec("CREATE TABLE cd (id int64, current_date date)");
    Exec("INSERT INTO cd VALUES (1, '2001-02-03')");
    Exec("UPDATE cd SET current_date = current_date + 1 WHERE id = 1");
    EXPECT_EQ(Run("SELECT current_date FROM cd WHERE id = 1"), "current_date\\n2001-02-04");
}

TEST_F(SetExprFunctionsTest, DateArithmeticIsDaysAndDifferences) {
    Exec("UPDATE t SET dt = dt + 7 WHERE id = 1");
    EXPECT_EQ(At("dt", 1), "2026-03-11");
    Exec("UPDATE t SET dt = 30 + dt WHERE id = 1");
    EXPECT_EQ(At("dt", 1), "2026-04-10");
    Exec("UPDATE t SET dt = dt - 10 WHERE id = 1");
    EXPECT_EQ(At("dt", 1), "2026-03-31");
    Exec("UPDATE t SET dt = dt + i8 WHERE id = 1");
    EXPECT_EQ(At("dt", 1), "2026-04-05");
    Exec("UPDATE t SET i32 = d2 - dt WHERE id >= 1");
    EXPECT_EQ(At("i32", 1), "-35");
    EXPECT_EQ(At("i32", 2), "2");
    Exec("UPDATE t SET dt = '2026-02-28' WHERE id = 1");
    Exec("UPDATE t SET dt = dt + 1 WHERE id = 1");
    EXPECT_EQ(At("dt", 1), "2026-03-01") << "the calendar, not 30-day months";
    Exec("UPDATE t SET dt = '1900-01-01' WHERE id = 1");
    const std::string below = Run("UPDATE t SET dt = dt - 1 WHERE id = 1");
    EXPECT_NE(below.find("outside the supported range"), std::string::npos) << below;
    EXPECT_EQ(At("dt", 1), "1900-01-01");
    Exec("UPDATE t SET dt = '2999-12-31' WHERE id = 1");
    const std::string out = Run("UPDATE t SET dt = dt + 1 WHERE id = 1");
    EXPECT_NE(out.find("outside the supported range"), std::string::npos) << out;
    EXPECT_EQ(At("dt", 1), "2999-12-31");
    EXPECT_NE(Refusal("dt = dt + dt").find("cannot apply to date and date"), std::string::npos);
    EXPECT_NE(Refusal("dt = dt * 2").find("cannot apply to date"), std::string::npos);
    EXPECT_NE(Refusal("dt = dt + 1.5").find("cannot apply to date"), std::string::npos);
    EXPECT_NE(Refusal("dt = dt + d").find("cannot apply to date and decimal(10,2)"),
              std::string::npos);
    EXPECT_NE(Refusal("dt = -dt").find("operator -"), std::string::npos);
    const std::string ts = Refusal("ts = ts + 1");
    EXPECT_NE(ts.find("INTERVAL"), std::string::npos) << ts;
    EXPECT_NE(Refusal("ts = dt + ts").find("INTERVAL"), std::string::npos);
}

}  // namespace
}  // namespace kds::server
