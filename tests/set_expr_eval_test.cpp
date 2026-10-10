// **A SET expression is evaluated against the row's before-image and then
// written** (BJ-S4, BJ-R2, BJ-R3, BJ-R4, BJ-R5, BJ-R10, BJ-Q10).
//
// `SetExprTypingTest` pins what compiles; these cells run the statement over
// rows that match and read the table back, so a wrong value, a wrapped
// overflow, a second application or an assignment that read another's result
// is what fails.

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"

namespace kds::server {
namespace {

class SetExprEvalTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
        Exec("CREATE TABLE t (id int64, i8 int8, i32 int32, i64 int64, u uint64, "
             "d decimal(10,2), w decimal(30,4), s varchar, c char(8), n int64 NULL, "
             "m int64)");
        Exec("INSERT INTO t VALUES (1, 10, 100, 1000, 7, '12.50', '3.0000', 'ab', 'cd', 5, 3)");
        Exec("INSERT INTO t VALUES (2, -10, -100, -1000, 0, '-1.25', '0.5000', 'xy', 'zw', NULL, 4)");
    }

    std::string Run(const std::string& line) { return dispatcher_->Dispatch(line).response; }

    // A statement that must not be refused.
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

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

TEST_F(SetExprEvalTest, IntegerArithmeticReadsTheRowAndWritesTheResult) {
    EXPECT_EQ(Run("UPDATE t SET i64 = i64 + 1 WHERE id = 1").substr(0, 9), "UPDATED 1");
    EXPECT_EQ(At("i64", 1), "1001");
    Exec("UPDATE t SET i64 = i64 * 3 - 5 WHERE id = 1");
    EXPECT_EQ(At("i64", 1), "2998");
    // `/` truncates toward zero and `%` takes the dividend's sign (BJ-R3).
    Exec("UPDATE t SET i64 = i64 / 7 WHERE id = 2");
    EXPECT_EQ(At("i64", 2), "-142");
    Exec("UPDATE t SET i32 = i32 % 7 WHERE id = 2");
    EXPECT_EQ(At("i32", 2), "-2");
    Exec("UPDATE t SET i64 = -i64 WHERE id = 1");
    EXPECT_EQ(At("i64", 1), "-2998");
    Exec("UPDATE t SET i64 = (i64 + 2) * (i64 - 2) WHERE id = 1");
    EXPECT_EQ(At("i64", 1), "8988000");
}

TEST_F(SetExprEvalTest, TheWholeSetListReadsTheBeforeImage) {
    // BJ-R2: the swap. A later assignment never sees an earlier one's result.
    Exec("UPDATE t SET i64 = m, m = i64 WHERE id = 1");
    EXPECT_EQ(At("i64", 1), "3");
    EXPECT_EQ(At("m", 1), "1000");
    Exec("UPDATE t SET i64 = i64 + 1, m = i64 + 1 WHERE id = 1");
    EXPECT_EQ(At("i64", 1), "4");
    EXPECT_EQ(At("m", 1), "4") << "m read i64's before-image, 3, not 4";
}

TEST_F(SetExprEvalTest, EveryMatchingRowGetsItsOwnValueOnce) {
    EXPECT_EQ(Run("UPDATE t SET i64 = i64 + m WHERE id >= 1").substr(0, 9), "UPDATED 2");
    EXPECT_EQ(At("i64", 1), "1003");
    EXPECT_EQ(At("i64", 2), "-996");
}

TEST_F(SetExprEvalTest, TheKeyIsReadAndNeverWritten) {
    Exec("UPDATE t SET m = id * 10 WHERE id >= 1");
    EXPECT_EQ(At("m", 1), "10");
    EXPECT_EQ(At("m", 2), "20");
}

TEST_F(SetExprEvalTest, AnOverflowIsRefusedAtTheRowAndNothingIsStored) {
    const std::string out = Run("UPDATE t SET i8 = i8 * 100 WHERE id = 1");
    EXPECT_EQ(out.substr(0, 3), "ERR") << out;
    EXPECT_NE(out.find("outside int8"), std::string::npos) << out;
    EXPECT_NE(out.find("row id 1"), std::string::npos) << out;
    EXPECT_NE(out.find("in the expression at byte"), std::string::npos) << out;
    EXPECT_EQ(At("i8", 1), "10");
    // An intermediate is judged in its own type: 100 + 100 does not fit int8
    // even though the final result would.
    Exec("UPDATE t SET i8 = 100 WHERE id = 1");
    EXPECT_NE(Run("UPDATE t SET i8 = i8 + 100 - 100 WHERE id = 1").find("outside int8"),
              std::string::npos);
    EXPECT_EQ(At("i8", 1), "100");
    // int64 does not wrap either.
    Exec("UPDATE t SET i64 = 9223372036854775807 WHERE id = 1");
    EXPECT_NE(Run("UPDATE t SET i64 = i64 + 1 WHERE id = 1").find("outside int64"),
              std::string::npos);
    EXPECT_EQ(At("i64", 1), "9223372036854775807");
    Exec("UPDATE t SET i64 = -9223372036854775807 - 1 WHERE id = 1");
    EXPECT_NE(Run("UPDATE t SET i64 = -i64 WHERE id = 1").find("outside int64"),
              std::string::npos);
    EXPECT_NE(Run("UPDATE t SET i64 = i64 / -1 WHERE id = 1").find("outside int64"),
              std::string::npos);
}

TEST_F(SetExprEvalTest, UnsignedArithmeticIsUnsignedAndNeverGoesNegative) {
    Exec("UPDATE t SET u = u + 1 WHERE id = 1");
    EXPECT_EQ(At("u", 1), "8");
    EXPECT_NE(Run("UPDATE t SET u = u - 8 - 1 WHERE id = 1").find("outside uint64"),
              std::string::npos);
    EXPECT_EQ(At("u", 1), "8");
    // Above INT64_MAX: carried in the digits, not in a signed word.
    Exec("UPDATE t SET u = 18446744073709551615 WHERE id = 1");
    Exec("UPDATE t SET u = u - 1 WHERE id = 1");
    EXPECT_EQ(At("u", 1), "18446744073709551614");
    EXPECT_NE(Run("UPDATE t SET u = u + 2 WHERE id = 1").find("outside uint64"),
              std::string::npos);
    Exec("UPDATE t SET u = u / 2 WHERE id = 1");
    EXPECT_EQ(At("u", 1), "9223372036854775807");
    Exec("UPDATE t SET u = u + 1 WHERE id = 1");
    EXPECT_EQ(At("u", 1), "9223372036854775808");
}

TEST_F(SetExprEvalTest, ADivisionByZeroIsRefusedAndNullDividesToNull) {
    const std::string out = Run("UPDATE t SET i64 = i64 / 0 WHERE id = 1");
    EXPECT_NE(out.find("division by zero"), std::string::npos) << out;
    EXPECT_NE(out.find("row id 1"), std::string::npos) << out;
    EXPECT_NE(Run("UPDATE t SET i64 = i64 % (i64 - i64) WHERE id = 1").find("division by zero"),
              std::string::npos);
    EXPECT_EQ(At("i64", 1), "1000");
    // `NULL / 0` is NULL, not an error (BJ-R4).
    Exec("UPDATE t SET n = n / 0 WHERE id = 2");
    EXPECT_EQ(At("n", 2), "NULL") << "n stayed NULL";
}

TEST_F(SetExprEvalTest, NullPropagatesAndIsCountedAsUpdated) {
    // BJ-R4: `SET counter = counter + 1` over a NULL leaves it NULL and
    // counts the row.
    EXPECT_EQ(Run("UPDATE t SET n = n + 1 WHERE id >= 1").substr(0, 9), "UPDATED 2");
    EXPECT_EQ(At("n", 1), "6");
    EXPECT_EQ(At("n", 2), "NULL");
    Exec("UPDATE t SET n = NULL + 1 WHERE id = 1");
    EXPECT_EQ(At("n", 1), "NULL");
}

TEST_F(SetExprEvalTest, ANullIntoANotNullColumnIsRefusedAtTheRow) {
    const std::string out = Run("UPDATE t SET m = n + 1 WHERE id = 2");
    EXPECT_EQ(out.substr(0, 3), "ERR") << out;
    EXPECT_NE(out.find("'m'"), std::string::npos) << out;
    EXPECT_EQ(At("m", 2), "4");
    // The same expression over the row whose n is not NULL is fine.
    Exec("UPDATE t SET m = n + 1 WHERE id = 1");
    EXPECT_EQ(At("m", 1), "6");
}

TEST_F(SetExprEvalTest, DecimalsKeepTheirScaleAndRefuseWhatDoesNotFit) {
    Exec("UPDATE t SET d = d + d WHERE id = 1");
    EXPECT_EQ(At("d", 1), "25.00");
    Exec("UPDATE t SET d = d * 3 WHERE id = 1");
    EXPECT_EQ(At("d", 1), "75.00");
    Exec("UPDATE t SET d = 2 * d - 1.50 WHERE id = 1");
    EXPECT_EQ(At("d", 1), "148.50");
    Exec("UPDATE t SET d = -d WHERE id = 2");
    EXPECT_EQ(At("d", 2), "1.25");
    Exec("UPDATE t SET d = d % 1.00 WHERE id = 1");
    EXPECT_EQ(At("d", 1), "0.50");
    // `d * NULL` is NULL of d's type (BJ-R4); the column is NOT NULL, so the
    // row refuses it.
    EXPECT_EQ(Run("UPDATE t SET d = d * NULL WHERE id = 1").substr(0, 3), "ERR");
    // decimal(10,2) holds eight integer digits.
    Exec("UPDATE t SET d = 99999999.99 WHERE id = 1");
    const std::string out = Run("UPDATE t SET d = d + 0.01 WHERE id = 1");
    EXPECT_NE(out.find("outside decimal(10,2)"), std::string::npos) << out;
    EXPECT_EQ(At("d", 1), "99999999.99");
}

TEST_F(SetExprEvalTest, AWideProductMustBeNarrowedAndASameScaleSumIsExact) {
    Exec("UPDATE t SET w = w + w WHERE id = 1");
    EXPECT_EQ(At("w", 1), "6.0000");
    Exec("UPDATE t SET w = w * 2 - w WHERE id = 1");
    EXPECT_EQ(At("w", 1), "6.0000");
    // A product of two decimal(30,4) is decimal(38,8): refused at compile.
    EXPECT_NE(Run("UPDATE t SET w = w * w WHERE id = 1").find("decimal(38,8)"),
              std::string::npos);
}

TEST_F(SetExprEvalTest, TextConcatenatesAndALongValueSpills) {
    Exec("UPDATE t SET s = s || '-' || s WHERE id = 1");
    EXPECT_EQ(At("s", 1), "ab-ab");
    Exec("UPDATE t SET s = c || s WHERE id = 1");
    EXPECT_EQ(At("s", 1), "cdab-ab");
    // BJ-R3: nothing is truncated. A `char(8)` result that is too long is
    // refused at the row; a `varchar`'s value spills.
    const std::string refused = Run("UPDATE t SET c = c || 'efghijkl' WHERE id = 1");
    EXPECT_EQ(refused.substr(0, 3), "ERR") << refused;
    EXPECT_EQ(At("c", 1), "cd");
    std::string big(3000, 'q');
    Exec("UPDATE t SET s = '" + big + "' WHERE id = 1");
    Exec("UPDATE t SET s = s || s WHERE id = 1");
    EXPECT_EQ(At("s", 1), big + big);
    EXPECT_EQ(Run("UPDATE t SET s = s || NULL WHERE id = 1").substr(0, 3), "ERR")
        << "s is NOT NULL, so a NULL result is refused at the row";
}

TEST_F(SetExprEvalTest, ARowThatDoesNotMatchIsNeverEvaluated) {
    // The expression would divide by zero for every row; no row qualifies.
    EXPECT_EQ(Run("UPDATE t SET i64 = i64 / 0 WHERE id = 99").substr(0, 9), "UPDATED 0");
}

TEST_F(SetExprEvalTest, AFailureInALaterRowNamesThatRowAndWritesNothingOfIt) {
    // Row 1 evaluates, row 2 overflows int8 (BJ-R10): the statement is
    // refused at row 2, naming its pk. (This dispatcher has no transaction
    // manager, so rolling row 1 back is the transactional cells' to pin.)
    Exec("UPDATE t SET i8 = -100 WHERE id = 2");
    const std::string out = Run("UPDATE t SET i8 = i8 * 2 WHERE id >= 1");
    EXPECT_EQ(out.substr(0, 3), "ERR") << out;
    EXPECT_NE(out.find("outside int8"), std::string::npos) << out;
    EXPECT_NE(out.find("row id 2"), std::string::npos) << out;
    EXPECT_EQ(At("i8", 2), "-100");
}

// ---- A foreign-key column (BJ-Q10) ----------------------------------------

class SetExprForeignKeyTest : public SetExprEvalTest {
protected:
    void SetUp() override {
        SetExprEvalTest::SetUp();
        Exec("CREATE TABLE parent (id int64, name varchar)");
        Exec("INSERT INTO parent VALUES (1, 'a')");
        Exec("INSERT INTO parent VALUES (2, 'b')");
        Exec("CREATE TABLE child (id int64, pid int64 REFERENCES parent, v int64)");
        Exec("INSERT INTO child VALUES (1, 1, 10)");
    }
};

TEST_F(SetExprForeignKeyTest, ANonConstantExpressionIntoAForeignKeyColumnIsRefused) {
    const std::string sql = "UPDATE child SET pid = v / 10 WHERE id = 1";
    const std::string out = Run(sql);
    EXPECT_NE(out.find("BJ-Q10"), std::string::npos) << out;
    EXPECT_NE(out.find("NOT_IMPLEMENTED"), std::string::npos) << out;
    EXPECT_NE(out.find("byte " + std::to_string(sql.find("v / 10"))), std::string::npos) << out;
    EXPECT_EQ(Run("SELECT pid FROM child WHERE id = 1"), "pid\\n1");
}

TEST_F(SetExprForeignKeyTest, AConstantExpressionIsFoldedAndCheckedAgainstItsValue) {
    Exec("UPDATE child SET pid = 1 + 1 WHERE id = 1");
    EXPECT_EQ(Run("SELECT pid FROM child WHERE id = 1"), "pid\\n2");
    const std::string absent = Run("UPDATE child SET pid = 2 + 5 WHERE id = 1");
    EXPECT_EQ(absent.substr(0, 3), "ERR") << absent;
    EXPECT_NE(absent.find("FK_VIOLATION"), std::string::npos) << absent;
    EXPECT_NE(absent.find("id=7"), std::string::npos) << "the folded value was the one checked";
    EXPECT_EQ(Run("SELECT pid FROM child WHERE id = 1"), "pid\\n2");
    // The value the row writes is the one that was checked, so no row is
    // pointed at an absent parent, and an UPDATE matching nothing is still
    // `UPDATED 0` for a parent that does not exist.
    EXPECT_EQ(Run("UPDATE child SET pid = 20 + 30 WHERE id = 99").substr(0, 9), "UPDATED 0");
}

TEST_F(SetExprForeignKeyTest, AnExpressionOverAnotherColumnIsFineWhereNoForeignKeyIsSet) {
    Exec("UPDATE child SET v = v + 1 WHERE id = 1");
    EXPECT_EQ(Run("SELECT v FROM child WHERE id = 1"), "v\\n11");
}

}  // namespace
}  // namespace kds::server
