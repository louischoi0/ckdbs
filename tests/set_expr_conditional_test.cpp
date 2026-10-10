// **Boolean values, CASE, the null-handling forms and CAST in a SET value**
// (BJ-S5, BJ-R1 families 2 and 3, BJ-R3, BJ-R4, BJ-Q3, BJ-Q7).
//
// Typing cells run over a WHERE that matches no row (`UPDATED 0` is "it
// typed"); evaluation cells run over a row and read the table back.

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"

namespace kds::server {
namespace {

class SetExprConditionalTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
        Exec("CREATE TABLE t (id int64, i64 int64, i32 int32, i8 int8, u uint64, "
             "d decimal(10,2), e decimal(10,0), s varchar, c char(4), dt date, ts timestamp, "
             "f bool NULL, g bool, n int64 NULL, m int64, o varchar NULL)");
        Exec("INSERT INTO t VALUES (1, 10, 100, 5, 7, '12.50', '3', 'ab', 'cd', '2026-01-02', "
             "'2026-01-02 03:04:05', NULL, 0, NULL, 4, NULL)");
        Exec("INSERT INTO t VALUES (2, -10, -100, -5, 0, '-1.25', '8', '12', 'zw', '2026-03-04', "
             "'2026-03-04 00:00:00', 1, 1, 3, 0, 'x')");
    }

    std::string Run(const std::string& line) { return dispatcher_->Dispatch(line).response; }
    void Exec(const std::string& line) {
        const std::string out = Run(line);
        ASSERT_NE(out.substr(0, 3), "ERR") << line << " -> " << out;
    }
    // One column of one row, as the SELECT prints it.
    std::string At(const std::string& column, int id) {
        const std::string out = Run("SELECT " + column + " FROM t WHERE id = " + std::to_string(id));
        const std::string header = column + "\\n";
        return out.rfind(header, 0) == 0 ? out.substr(header.size()) : out;
    }
    // Typed: over a WHERE that matches no row.
    bool Types(const std::string& set) {
        return Run("UPDATE t SET " + set + " WHERE id = 999").rfind("UPDATED 0", 0) == 0;
    }
    std::string Refusal(const std::string& set) {
        const std::string out = Run("UPDATE t SET " + set + " WHERE id = 999");
        return out.rfind("UPDATED 0", 0) == 0 ? "" : out;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

TEST_F(SetExprConditionalTest, AComparisonIsABoolValue) {
    Exec("UPDATE t SET g = i64 > 5 WHERE id >= 1");
    EXPECT_EQ(At("g", 1), "1");
    EXPECT_EQ(At("g", 2), "0");
    Exec("UPDATE t SET g = s = '12' WHERE id >= 1");
    EXPECT_EQ(At("g", 1), "0");
    EXPECT_EQ(At("g", 2), "1");
    Exec("UPDATE t SET g = dt >= '2026-02-01' WHERE id >= 1");
    EXPECT_EQ(At("g", 1), "0");
    EXPECT_EQ(At("g", 2), "1");
    Exec("UPDATE t SET g = d < 0 WHERE id >= 1");
    EXPECT_EQ(At("g", 1), "0");
    EXPECT_EQ(At("g", 2), "1");
    Exec("UPDATE t SET g = u != 0 WHERE id >= 1");
    EXPECT_EQ(At("g", 1), "1");
    EXPECT_EQ(At("g", 2), "0");
    Exec("UPDATE t SET g = s = c WHERE id = 1");
    EXPECT_EQ(At("g", 1), "0") << "varchar and char compare as text";
}

TEST_F(SetExprConditionalTest, AComparisonWithNullIsNull) {
    // n is NULL on row 1 and 3 on row 2; f is a nullable bool.
    Exec("UPDATE t SET f = n > 1 WHERE id >= 1");
    EXPECT_EQ(At("f", 1), "NULL");
    EXPECT_EQ(At("f", 2), "1");
}

TEST_F(SetExprConditionalTest, TheTruthTablesWithNull) {
    // Row 1: n is NULL, g = 0. Row 2: n = 3, g = 1.
    Exec("UPDATE t SET f = (n > 1) AND g WHERE id >= 1");
    EXPECT_EQ(At("f", 1), "0") << "NULL AND false is false";
    EXPECT_EQ(At("f", 2), "1");
    Exec("UPDATE t SET f = (n > 1) OR g WHERE id >= 1");
    EXPECT_EQ(At("f", 1), "NULL") << "NULL OR false is NULL";
    EXPECT_EQ(At("f", 2), "1");
    Exec("UPDATE t SET f = NOT (n > 1) WHERE id >= 1");
    EXPECT_EQ(At("f", 1), "NULL");
    EXPECT_EQ(At("f", 2), "0");
}

TEST_F(SetExprConditionalTest, IsNullIsNeverNull) {
    Exec("UPDATE t SET g = n IS NULL WHERE id >= 1");
    EXPECT_EQ(At("g", 1), "1");
    EXPECT_EQ(At("g", 2), "0");
    Exec("UPDATE t SET g = n IS NOT NULL WHERE id >= 1");
    EXPECT_EQ(At("g", 1), "0");
    EXPECT_EQ(At("g", 2), "1");
    Exec("UPDATE t SET g = (n + 1) IS NULL WHERE id >= 1");
    EXPECT_EQ(At("g", 1), "1");
}

TEST_F(SetExprConditionalTest, AndStopsAtItsFirstFalseSoAGuardedDivisionIsSafe) {
    // Row 1 has m = 4, row 2 has m = 0: `m != 0 AND i64 / m > 1`.
    Exec("UPDATE t SET g = m != 0 AND i64 / m > 1 WHERE id >= 1");
    EXPECT_EQ(At("g", 1), "1");
    EXPECT_EQ(At("g", 2), "0");
    Exec("UPDATE t SET g = m = 0 OR i64 / m > 1 WHERE id >= 1");
    EXPECT_EQ(At("g", 2), "1");
    const std::string unguarded = Run("UPDATE t SET g = i64 / m > 1 WHERE id >= 1");
    EXPECT_NE(unguarded.find("division by zero"), std::string::npos) << unguarded;
}

TEST_F(SetExprConditionalTest, ComparisonAndBooleanTypingNamesBothTypes) {
    EXPECT_NE(Refusal("g = i64 = 'x'").find("string literal cannot be used as int64"), std::string::npos);
    EXPECT_NE(Refusal("g = i32 > i64").find("int32 and int64"), std::string::npos);
    EXPECT_NE(Refusal("g = i64 AND g").find("operator AND needs a bool but is given int64"),
              std::string::npos);
    EXPECT_NE(Refusal("g = NOT i64").find("operator NOT needs a bool"), std::string::npos);
    EXPECT_NE(Refusal("i64 = i64 > 1").find("bool"), std::string::npos)
        << "a bool is not an int64 (BJ-R3)";
    EXPECT_NE(Refusal("g = dt > 5").find("date"), std::string::npos)
        << "an integer literal is not a date";
    EXPECT_TRUE(Types("g = 1 < 2")) << "two integer literals are int64s";
    EXPECT_TRUE(Types("g = 'a' = 'b'"));
    EXPECT_NE(Refusal("g = 1.5 > 1.2").find("numeric literal"), std::string::npos);
    EXPECT_TRUE(Types("g = NULL = NULL"));
    EXPECT_TRUE(Types("g = NULL AND NULL"));
    EXPECT_TRUE(Types("g = d > 1.5"));
    EXPECT_TRUE(Types("g = g = g"));
}

TEST_F(SetExprConditionalTest, CaseIsSearchedOrSimpleWithNullWhenNothingMatches) {
    Exec("UPDATE t SET i64 = CASE WHEN i64 > 0 THEN 1 WHEN i64 < 0 THEN -1 ELSE 0 END WHERE id >= 1");
    EXPECT_EQ(At("i64", 1), "1");
    EXPECT_EQ(At("i64", 2), "-1");
    Exec("UPDATE t SET m = CASE id WHEN 1 THEN 100 WHEN 2 THEN 200 END WHERE id >= 1");
    EXPECT_EQ(At("m", 1), "100");
    EXPECT_EQ(At("m", 2), "200");
    Exec("UPDATE t SET n = CASE WHEN id = 1 THEN 5 END WHERE id >= 1");
    EXPECT_EQ(At("n", 1), "5");
    EXPECT_EQ(At("n", 2), "NULL") << "no WHEN matched and there is no ELSE";
    // A NULL condition is not true.
    Exec("UPDATE t SET m = CASE WHEN o = 'x' THEN 1 ELSE 2 END WHERE id >= 1");
    EXPECT_EQ(At("m", 1), "2");
    EXPECT_EQ(At("m", 2), "1");
    // Only the chosen branch is evaluated.
    Exec("UPDATE t SET m = CASE WHEN id = 1 THEN 7 ELSE 1 / 0 END WHERE id = 1");
    EXPECT_EQ(At("m", 1), "7");
}

TEST_F(SetExprConditionalTest, CaseBranchesAgreeOnOneTypeAndLiteralsTakeTheTargets) {
    EXPECT_NE(Refusal("i64 = CASE WHEN g THEN 1 ELSE 'x' END").find("integer literal and string"),
              std::string::npos);
    EXPECT_NE(Refusal("i64 = CASE WHEN g THEN i32 ELSE i64 END").find("int32 and int64"),
              std::string::npos);
    EXPECT_NE(Refusal("i64 = CASE WHEN i64 THEN 1 END").find("CASE condition needs a bool"),
              std::string::npos);
    EXPECT_TRUE(Types("i8 = CASE WHEN g THEN 1 ELSE 2 END")) << "the literals take int8";
    EXPECT_NE(Refusal("i8 = CASE WHEN g THEN 1 ELSE 300 END").find("does not fit int8"),
              std::string::npos);
    EXPECT_TRUE(Types("s = CASE WHEN g THEN c ELSE s END")) << "char and varchar are one family";
    EXPECT_TRUE(Types("d = CASE WHEN g THEN 1.5 ELSE d END"));
    EXPECT_TRUE(Types("i64 = CASE WHEN g THEN NULL ELSE 1 END"));
}

TEST_F(SetExprConditionalTest, CoalesceNullifGreatestAndLeast) {
    Exec("UPDATE t SET n = COALESCE(n, m, 0) WHERE id >= 1");
    EXPECT_EQ(At("n", 1), "4");
    EXPECT_EQ(At("n", 2), "3");
    Exec("UPDATE t SET n = NULLIF(n, 3) WHERE id >= 1");
    EXPECT_EQ(At("n", 1), "4");
    EXPECT_EQ(At("n", 2), "NULL");
    Exec("UPDATE t SET n = NULLIF(NULL, 3) WHERE id = 1");
    EXPECT_EQ(At("n", 1), "NULL");
    Exec("UPDATE t SET m = GREATEST(i64, m, 2) WHERE id >= 1");
    EXPECT_EQ(At("m", 1), "10");
    EXPECT_EQ(At("m", 2), "2");
    Exec("UPDATE t SET m = LEAST(i64, m, 100) WHERE id >= 1");
    EXPECT_EQ(At("m", 1), "10");
    EXPECT_EQ(At("m", 2), "-10");
    // NULLs are skipped, and the result is NULL only when every argument is.
    Exec("UPDATE t SET n = GREATEST(n, NULL, 5) WHERE id = 1");
    EXPECT_EQ(At("n", 1), "5");
    Exec("UPDATE t SET n = LEAST(NULL, n) WHERE id = 2");
    EXPECT_EQ(At("n", 2), "NULL");
    Exec("UPDATE t SET s = GREATEST(s, c) WHERE id = 1");
    EXPECT_EQ(At("s", 1), "cd");
    Exec("UPDATE t SET d = LEAST(d, 1.00) WHERE id >= 1");
    EXPECT_EQ(At("d", 1), "1.00");
    EXPECT_EQ(At("d", 2), "-1.25");
    EXPECT_NE(Refusal("i64 = COALESCE(i32, i64)").find("int32 and int64"), std::string::npos);
    EXPECT_NE(Refusal("i64 = GREATEST(i64, 'x')").find("string literal cannot be used as int64"), std::string::npos);
    EXPECT_NE(Refusal("n = NULLIF(i64, s)").find("needs one type"), std::string::npos);
}

TEST_F(SetExprConditionalTest, CastBetweenNumbersRoundsHalfAwayFromZeroAndNeverWraps) {
    Exec("UPDATE t SET e = CAST(d AS decimal(10, 0)) WHERE id >= 1");
    EXPECT_EQ(At("e", 1), "13") << "12.50 rounds away from zero";
    EXPECT_EQ(At("e", 2), "-1");
    Exec("UPDATE t SET d = CAST(i64 AS decimal(10, 2)) WHERE id >= 1");
    EXPECT_EQ(At("d", 1), "10.00");
    EXPECT_EQ(At("d", 2), "-10.00");
    Exec("UPDATE t SET d = 2.50 WHERE id = 1");
    Exec("UPDATE t SET i64 = CAST(d AS int64) WHERE id = 1");
    EXPECT_EQ(At("i64", 1), "3");
    Exec("UPDATE t SET d = -2.50 WHERE id = 1");
    Exec("UPDATE t SET i64 = d::int64 WHERE id = 1");
    EXPECT_EQ(At("i64", 1), "-3");
    Exec("UPDATE t SET d = 2.49 WHERE id = 1");
    Exec("UPDATE t SET i64 = d::int64 WHERE id = 1");
    EXPECT_EQ(At("i64", 1), "2");
    Exec("UPDATE t SET d = 1.005 WHERE id = 99");
    // Narrowing to a type that cannot hold the value is refused, not wrapped.
    Exec("UPDATE t SET i64 = 200 WHERE id = 1");
    const std::string out = Run("UPDATE t SET i8 = CAST(i64 AS int8) WHERE id = 1");
    EXPECT_NE(out.find("outside int8"), std::string::npos) << out;
    EXPECT_EQ(At("i8", 1), "5");
    Exec("UPDATE t SET i32 = i64::int32 WHERE id = 1");
    EXPECT_EQ(At("i32", 1), "200");
    Exec("UPDATE t SET u = CAST(i8 AS uint64) WHERE id = 1");
    EXPECT_EQ(At("u", 1), "5");
    const std::string negative = Run("UPDATE t SET u = CAST(i8 AS uint64) WHERE id = 2");
    EXPECT_NE(negative.find("outside uint64"), std::string::npos) << negative;
}

TEST_F(SetExprConditionalTest, CastToAndFromText) {
    Exec("UPDATE t SET s = CAST(i64 AS varchar) || '/' || CAST(d AS varchar) WHERE id = 1");
    EXPECT_EQ(At("s", 1), "10/12.50");
    Exec("UPDATE t SET i64 = CAST(s AS int64) WHERE id = 2");
    EXPECT_EQ(At("i64", 2), "12");
    Exec("UPDATE t SET d = CAST('  -3.456 ' AS decimal(10, 2)) WHERE id = 1");
    EXPECT_EQ(At("d", 1), "-3.46");
    const std::string bad = Run("UPDATE t SET i64 = CAST(s AS int64) WHERE id = 1");
    EXPECT_NE(bad.find("is not a number"), std::string::npos) << bad;
    EXPECT_NE(Run("UPDATE t SET i64 = CAST('1x' AS int64) WHERE id = 1").find("is not a number"),
              std::string::npos);
    Exec("UPDATE t SET dt = CAST('2027-05-06' AS date) WHERE id = 1");
    EXPECT_EQ(At("dt", 1), "2027-05-06");
    Exec("UPDATE t SET s = CAST(dt AS varchar) WHERE id = 1");
    EXPECT_EQ(At("s", 1), "2027-05-06");
    // char(4) holds four bytes; a longer text is refused, never truncated.
    EXPECT_EQ(Run("UPDATE t SET c = CAST('abcd' AS char(4)) WHERE id = 1").substr(0, 9), "UPDATED 1");
    EXPECT_NE(Run("UPDATE t SET c = CAST('abcde' AS char(4)) WHERE id = 1").find("too long"),
              std::string::npos);
}

TEST_F(SetExprConditionalTest, CastBetweenDatesAndTimestamps) {
    Exec("UPDATE t SET ts = CAST(dt AS timestamp) WHERE id = 1");
    EXPECT_EQ(At("ts", 1), "2026-01-02 00:00:00");
    Exec("UPDATE t SET dt = ts::date WHERE id = 2");
    EXPECT_EQ(At("dt", 2), "2026-03-04");
    Exec("UPDATE t SET ts = '1969-12-31 23:59:59' WHERE id = 2");
    Exec("UPDATE t SET dt = CAST(ts AS date) WHERE id = 2");
    EXPECT_EQ(At("dt", 2), "1969-12-31") << "before the epoch a timestamp floors to its day";
}

TEST_F(SetExprConditionalTest, CastRefusesWhatItCannotConvertAtCompile) {
    EXPECT_NE(Refusal("i64 = CAST(dt AS int64)").find("CAST from date to int64"),
              std::string::npos);
    EXPECT_NE(Refusal("i64 = CAST(g AS int64)").find("not supported"), std::string::npos);
    EXPECT_NE(Refusal("g = CAST(i64 AS bool)").find("not supported"), std::string::npos);
    EXPECT_NE(Refusal("i64 = CAST(i64 AS nonsense)").find("unknown type 'nonsense'"),
              std::string::npos);
    EXPECT_NE(Refusal("i64 = CAST(i64 AS float)").find("float"), std::string::npos);
    EXPECT_NE(Refusal("s = CAST(i64 AS varchar(10))").find("cell width"), std::string::npos);
    EXPECT_NE(Refusal("d = CAST(i64 AS decimal(40, 2))").find("40"), std::string::npos);
    EXPECT_NE(Refusal("i64 = CAST(1.5 + 1.5 AS int64)").find("no type"), std::string::npos);
    EXPECT_TRUE(Types("i64 = CAST(1.5 AS int64)")) << "a lone numeric literal is the decimal it says";
    EXPECT_TRUE(Types("i64 = CAST(NULL AS int64)"));
    EXPECT_TRUE(Types("s = CAST(5 AS varchar)"));
    EXPECT_NE(Refusal("i32 = CAST(i64 AS int64)").find("int64 but column 'i32' is int32"),
              std::string::npos)
        << "a CAST gives the type it names, and the assignment still needs it to be the column's";
}

TEST_F(SetExprConditionalTest, ARowValueReadsTheBeforeImageAndSwaps) {
    Exec("UPDATE t SET (i64, m) = (m, i64) WHERE id = 1");
    EXPECT_EQ(At("i64", 1), "4");
    EXPECT_EQ(At("m", 1), "10");
    Exec("UPDATE t SET (i64, m, n) = (m + 1, i64 + 1, COALESCE(n, 0)) WHERE id = 1");
    EXPECT_EQ(At("i64", 1), "11");
    EXPECT_EQ(At("m", 1), "5");
    EXPECT_EQ(At("n", 1), "0");
    const std::string dup = Run("UPDATE t SET (i64, i64) = (1, 2) WHERE id = 1");
    EXPECT_NE(dup.find("more than once"), std::string::npos) << dup;
}

TEST_F(SetExprConditionalTest, AConstantBoolOrCaseIntoAForeignKeyIsFoldedAndAColumnReadingOneIsRefused) {
    Exec("CREATE TABLE parent (id int64, name varchar)");
    Exec("INSERT INTO parent VALUES (1, 'a')");
    Exec("INSERT INTO parent VALUES (2, 'b')");
    Exec("CREATE TABLE child (id int64, pid int64 REFERENCES parent, v int64)");
    Exec("INSERT INTO child VALUES (1, 1, 10)");
    Exec("UPDATE child SET pid = CASE WHEN 1 < 2 THEN 2 ELSE 1 END WHERE id = 1");
    EXPECT_EQ(Run("SELECT pid FROM child WHERE id = 1"), "pid\\n2");
    EXPECT_NE(Run("UPDATE child SET pid = CASE WHEN 1 < 2 THEN 9 ELSE 1 END WHERE id = 1")
                  .find("FK_VIOLATION"),
              std::string::npos);
    EXPECT_NE(Run("UPDATE child SET pid = COALESCE(v, 1) WHERE id = 1").find("BJ-Q10"),
              std::string::npos);
}

}  // namespace
}  // namespace kds::server
