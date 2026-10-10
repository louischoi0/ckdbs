// **A SET expression is typed at compile** (BJ-S3, BJ-R3, BJ-R4, BJ-R5,
// BJ-Q2 (b), BJ-Q3).
//
// Until BJ-S4 evaluates an expression, a statement whose expression types is
// refused `NotImplemented (BJ-S4)` and one that does not is refused for what
// is wrong with it - so each cell reads which of the two it got. The point
// is the second kind: a mistyped expression is refused whether or not a row
// matches, naming both types and the byte.

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"

namespace kds::server {
namespace {

class SetExprTypingTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
        const std::string created = Run(
            "CREATE TABLE t (id int64, i8 int8, i32 int32, i64 int64, u uint64, "
            "d decimal(10,2), e decimal(10,2), w decimal(12,4), s varchar, c char(8), "
            "dt date, ts timestamp, n int64 NULL)");
        ASSERT_EQ(created.substr(0, 7), "CREATED") << created;
        ASSERT_EQ(Run("INSERT INTO t VALUES (1, 1, 1, 1, 1, '1.00', '1.00', '1.0000', 'x', 'y', "
                      "'2026-01-01', '2026-01-01 00:00:00', NULL)")
                      .substr(0, 8),
                  "INSERTED");
    }

    std::string Run(const std::string& line) { return dispatcher_->Dispatch(line).response; }

    // The statement typed and reached BJ-S4's refusal.
    bool Types(const std::string& set) {
        const std::string out = Run("UPDATE t SET " + set + " WHERE id = 1");
        return out.find("(BJ-S4)") != std::string::npos;
    }
    // The refusal, or the empty string when the statement typed.
    std::string Refusal(const std::string& set) {
        const std::string out = Run("UPDATE t SET " + set + " WHERE id = 1");
        return out.find("(BJ-S4)") != std::string::npos ? "" : out;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

TEST_F(SetExprTypingTest, AMistypedExpressionIsRefusedEvenWhenNoRowMatches) {
    // §1.3: today a mistyped bare literal answers `UPDATED 0` over a WHERE
    // that matches nothing. An expression is typed at compile instead.
    const std::string out = Run("UPDATE t SET i32 = i32 + i64 WHERE id = 999");
    EXPECT_NE(out.find("int32 and int64"), std::string::npos) << out;
    EXPECT_EQ(out.find("UPDATED"), std::string::npos) << out;
}

TEST_F(SetExprTypingTest, SameTypeOperandsType) {
    EXPECT_TRUE(Types("i64 = i64 + 1"));
    EXPECT_TRUE(Types("i32 = i32 * i32 - 7"));
    EXPECT_TRUE(Types("i8 = i8 % 3"));
    EXPECT_TRUE(Types("u = u / 2 + u"));
    EXPECT_TRUE(Types("d = d + e - d"));
    EXPECT_TRUE(Types("i64 = -i64"));
    EXPECT_TRUE(Types("d = +d"));
}

TEST_F(SetExprTypingTest, TheTwoWidthsOfOneFamilyAreTwoTypes) {
    // BJ-Q2 (b), adopted over (a): W3's letter.
    const std::string r = Refusal("i64 = i32 + i64");
    EXPECT_NE(r.find("int32 and int64"), std::string::npos) << r;
    EXPECT_NE(r.find("no implicit conversion"), std::string::npos) << r;
    EXPECT_NE(Refusal("d = d + w").find("decimal(10,2) and decimal(12,4)"), std::string::npos);
}

TEST_F(SetExprTypingTest, TheExpressionMustBeItsTargetsType) {
    const std::string r = Refusal("i64 = i32 + 1");
    EXPECT_NE(r.find("the expression is int32 but column 'i64' is int64"), std::string::npos) << r;
    EXPECT_NE(r.find("CAST is the only one"), std::string::npos) << r;
    EXPECT_NE(Refusal("u = i64 + 1").find("is int64 but column 'u' is uint64"), std::string::npos);
}

TEST_F(SetExprTypingTest, ALiteralTakesTheTypeItsContextGives) {
    EXPECT_TRUE(Types("i8 = i8 + 100"));
    EXPECT_TRUE(Types("d = d + 1")) << "an integer literal beside a decimal is that decimal's";
    EXPECT_TRUE(Types("d = 2 * d"));
    EXPECT_TRUE(Types("i64 = 1 + 2"));
    EXPECT_TRUE(Types("u = 18446744073709551615 - u"));
    EXPECT_TRUE(Types("d = d + 1.5"));
}

TEST_F(SetExprTypingTest, ALiteralThatDoesNotFitIsAPositionedCompileError) {
    const std::string sql = "UPDATE t SET i8 = i8 + 300 WHERE id = 999";
    const std::string out = Run(sql);
    EXPECT_NE(out.find("byte " + std::to_string(sql.find("300"))), std::string::npos) << out;
    EXPECT_EQ(out.find("UPDATED"), std::string::npos) << out;
    // A scale the decimal cannot hold is refused, never rounded (TY6).
    EXPECT_NE(Refusal("d = d + 1.555").find("byte"), std::string::npos);
}

TEST_F(SetExprTypingTest, ALiteralOfAnotherFamilyIsRefused) {
    EXPECT_NE(Refusal("i64 = i64 + 'x'").find("string literal cannot be used as int64"),
              std::string::npos);
    EXPECT_NE(Refusal("i64 = i64 + 1.5").find("numeric literal cannot be used as int64"),
              std::string::npos);
    EXPECT_NE(Refusal("s = s || 1").find("integer literal cannot be used as varchar"),
              std::string::npos);
}

TEST_F(SetExprTypingTest, ADecimalProductAddsScalesAndMustBeNarrowedToItsTarget) {
    // BJ-Q3 (a): an implicit narrowing is refused, `*` of two decimals adds
    // the scales, and `price * 2` keeps the decimal's own.
    const std::string r = Refusal("d = d * e");
    EXPECT_NE(r.find("decimal128(20,4) but column 'd' is decimal(10,2)"), std::string::npos) << r;
    EXPECT_TRUE(Types("d = d * 2"));
    EXPECT_NE(Refusal("d = d * i64").find("decimal(10,2) and int64"), std::string::npos)
        << "only a literal scales a decimal; an int64 column is another type";
    // Division needs an explicit narrowing, which BJ-S5/S6 build.
    EXPECT_NE(Run("UPDATE t SET d = d / 2 WHERE id = 1").find("BJ-S5"), std::string::npos);
    EXPECT_NE(Run("UPDATE t SET d = d / e WHERE id = 1").find("NOT_IMPLEMENTED"),
              std::string::npos);
}

TEST_F(SetExprTypingTest, TextConcatenatesAcrossVarcharAndChar) {
    EXPECT_TRUE(Types("s = s || 'x' || c"));
    EXPECT_TRUE(Types("c = s || c")) << "varchar and char are one family; the length is a row's check";
    EXPECT_NE(Refusal("s = s || i64").find("varchar and int64"), std::string::npos);
    EXPECT_NE(Refusal("i64 = s || s").find("is varchar but column 'i64' is int64"),
              std::string::npos);
}

TEST_F(SetExprTypingTest, ArithmeticOnTextBoolDateAndTimestampIsRefused) {
    EXPECT_NE(Refusal("s = s + 1").find("integer literal cannot be used as varchar"),
              std::string::npos);
    EXPECT_NE(Refusal("s = s + s").find("operator + cannot apply to varchar and varchar"),
              std::string::npos);
    EXPECT_NE(Refusal("s = -s").find("operator -"), std::string::npos);
    EXPECT_NE(Run("UPDATE t SET dt = dt + 1 WHERE id = 1").find("BJ-S6"), std::string::npos);
    const std::string ts = Run("UPDATE t SET ts = ts + 1 WHERE id = 1");
    EXPECT_NE(ts.find("INTERVAL"), std::string::npos) << ts;
    EXPECT_NE(ts.find("UNSUPPORTED"), std::string::npos) << ts;
}

TEST_F(SetExprTypingTest, AnUnsignedValueHasNoNegation) {
    const std::string r = Refusal("u = -u");
    EXPECT_NE(r.find("uint64"), std::string::npos) << r;
    EXPECT_NE(r.find("no negation"), std::string::npos) << r;
    EXPECT_TRUE(Types("u = +u"));
}

TEST_F(SetExprTypingTest, NullTypesAsWhatItsContextIs) {
    // BJ-R4: whether a NULL result fits a NOT NULL target is the row's
    // verdict, not the compile's.
    EXPECT_TRUE(Types("n = n + 1"));
    EXPECT_TRUE(Types("i64 = NULL + 1"));
    EXPECT_TRUE(Types("s = s || NULL"));
    EXPECT_NE(Refusal("s = NULL + 1").find("byte"), std::string::npos);
}

TEST_F(SetExprTypingTest, ThePkIsReadNeverWritten) {
    // BJ-R5: `id` may be read inside an expression and still cannot be a
    // target, refused before the expression is looked at.
    EXPECT_TRUE(Types("i64 = id * 10"));
    const std::string out = Run("UPDATE t SET id = id + 1 WHERE id = 1");
    EXPECT_NE(out.find("UNSUPPORTED"), std::string::npos) << out;
    EXPECT_NE(out.find("primary-key column"), std::string::npos) << out;
}

TEST_F(SetExprTypingTest, AnUnknownColumnInAnExpressionIsPositioned) {
    const std::string sql = "UPDATE t SET i64 = i64 + nope WHERE id = 1";
    const std::string out = Run(sql);
    EXPECT_NE(out.find("unknown column 'nope'"), std::string::npos) << out;
    EXPECT_NE(out.find("byte " + std::to_string(sql.find("nope"))), std::string::npos) << out;
}

TEST_F(SetExprTypingTest, AnExpressionNeverWritesBeforeBjS4) {
    // The compile refusal is what keeps the foreign-key hoist and the apply
    // loop, which read `Assignment::val`, from meeting an expression.
    const std::string before = Run("SELECT i64 FROM t WHERE id = 1");
    const std::string out = Run("UPDATE t SET i64 = i64 + 1 WHERE id = 1");
    EXPECT_NE(out.find("NOT_IMPLEMENTED"), std::string::npos) << out;
    EXPECT_EQ(out.find("UPDATED"), std::string::npos) << out;
    EXPECT_EQ(Run("SELECT i64 FROM t WHERE id = 1"), before);
}

TEST_F(SetExprTypingTest, AColumnReferenceAloneMustBeTheTargetsType) {
    EXPECT_TRUE(Types("i64 = id"));
    EXPECT_NE(Refusal("i64 = i32").find("is int32 but column 'i64' is int64"), std::string::npos);
}

}  // namespace
}  // namespace kds::server
