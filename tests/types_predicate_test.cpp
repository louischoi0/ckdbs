#include "kds/exec/step_compiler.hpp"

#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/type_literals.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// TY05 - comparison and compile-time coercion (docs/spec/types.md §3.1,
// §3.2; docs/workplan-types.md).
//
// The property under test is that **the coercion happens at compile**, and
// it is checked by inspecting the compiled chain rather than by observing
// which rows come back. Those are not the same assertion: a statement that
// returned the right rows while parsing its literal per row would pass an
// end-to-end test and fail this one, and §3.1 is a statement about cost as
// much as about meaning - per-row evaluation must stay an int64 compare.
//
// Tables are created through the **dispatcher**, not by assembling a schema
// by hand, because `DECIMAL(p, s)` packs its precision and scale into
// `SysColumnRow::len` (§4a) and a hand-built row would be free to get that
// packing wrong in the same direction as the code under test.

namespace kds::exec {
namespace {

class TypesPredicateTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));

        ASSERT_EQ(Run("CREATE TABLE ev (id int64, d date, ts timestamp, amt decimal(10, 2))")
                      .substr(0, 7),
                  "CREATED");
        // Decimal columns of matching and differing scale, for the
        // column-column rule - in one relation, so comparing them needs no
        // join.
        //
        // Note the store above is constructed with `kFirstUserPageId`, not
        // defaulted. `InMemoryPageStore`'s default first page is 1, which
        // is inside the catalog's fixed pages (0..13) and the reserved
        // catalog-overflow range (14..127), so allocation hands out ids
        // already in use and the *third* `CREATE TABLE` fails with "page id
        // already in use" - at any bootstrap page count, with the whole
        // store free. Every other dispatcher-level fixture passes the
        // constant; this one did not, which is the entire bug.
        ASSERT_EQ(Run("CREATE TABLE money (id int64, two decimal(10, 2), "
                      "alsotwo decimal(10, 2), three decimal(10, 3))")
                      .substr(0, 7),
                  "CREATED");
    }

    std::string Run(const std::string& sql) {
        server::CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
        return d.Dispatch(sql).response;
    }

    StatusOr<StepChain> CompileSql(const std::string& sql) {
        auto parsed = parser::Parse(sql);
        if (!parsed.ok()) return parsed.status();
        return Compile(boot_->catalog, std::get<parser::SelectStmt>(parsed.value()));
    }

    StepChain MustCompile(const std::string& sql) {
        auto chain = CompileSql(sql);
        EXPECT_TRUE(chain.ok()) << sql << ": " << chain.status().message();
        return chain.ok() ? std::move(chain.value()) : StepChain{};
    }

    // The single residual of a single-step chain.
    const StepPredicate& OnlyResidual(const StepChain& chain) {
        EXPECT_EQ(chain.steps.size(), 1u);
        EXPECT_EQ(chain.steps[0].residual.size(), 1u);
        return chain.steps[0].residual[0];
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
};

// ---- The compiled right-hand side is already an integer -----------------

TEST_F(TypesPredicateTest, ADateLiteralCompilesToItsEpochDay) {
    const StepChain chain = MustCompile("SELECT id FROM ev WHERE d = '2026-08-07'");
    const StepPredicate& pred = OnlyResidual(chain);

    ASSERT_EQ(pred.rhs.kind, OperandKind::kLiteral);
    // kInt, not kStr: the string is gone by the time the chain exists.
    EXPECT_EQ(pred.rhs.literal.type, parser::ValueType::kInt);
    EXPECT_TRUE(pred.rhs.literal.str_val.empty());
    // Derived through the same parser rather than written down, so this
    // asserts the compiler used it and not that 20672 is a magic number.
    auto expected = ParseDateLiteral("2026-08-07");
    ASSERT_TRUE(expected.ok());
    EXPECT_EQ(pred.rhs.literal.int_val, expected.value());
}

TEST_F(TypesPredicateTest, ATimestampLiteralCompilesToItsEpochMicros) {
    const StepChain chain =
        MustCompile("SELECT id FROM ev WHERE ts < '2026-08-07 09:15:00.250000'");
    const StepPredicate& pred = OnlyResidual(chain);

    EXPECT_EQ(pred.op, parser::CompareOp::kLt);
    EXPECT_EQ(pred.rhs.literal.type, parser::ValueType::kInt);
    auto expected = ParseTimestampLiteral("2026-08-07 09:15:00.250000");
    ASSERT_TRUE(expected.ok());
    EXPECT_EQ(pred.rhs.literal.int_val, expected.value());
}

TEST_F(TypesPredicateTest, ADecimalLiteralCompilesToItsScaledInteger) {
    const StepChain chain = MustCompile("SELECT id FROM ev WHERE amt = '12.34'");
    const StepPredicate& pred = OnlyResidual(chain);

    // The one kind that keeps a ValueType of its own, because the unscaled
    // integer means nothing without the scale beside it.
    EXPECT_EQ(pred.rhs.literal.type, parser::ValueType::kDecimal);
    EXPECT_EQ(pred.rhs.literal.int_val, 1234);
    EXPECT_EQ(pred.rhs.literal.scale, 2);
}

TEST_F(TypesPredicateTest, AnIntegerLiteralAgainstADecimalColumnIsScaled) {
    // `amt = 12` is exact and means 12.00. Scaling it goes through
    // ParseDecimalLiteral rather than a multiply here, so the precision
    // rules have one owner.
    const StepChain chain = MustCompile("SELECT id FROM ev WHERE amt = 12");
    const StepPredicate& pred = OnlyResidual(chain);

    EXPECT_EQ(pred.rhs.literal.type, parser::ValueType::kDecimal);
    EXPECT_EQ(pred.rhs.literal.int_val, 1200);
    EXPECT_EQ(pred.rhs.literal.scale, 2);
}

TEST_F(TypesPredicateTest, BothBoundsOfABetweenAreCoerced) {
    // BETWEEN lowers to two ordinary conjuncts, so it is two chances to
    // forget - and the high bound is the one a single-site fix misses.
    const StepChain chain =
        MustCompile("SELECT id FROM ev WHERE d BETWEEN '2026-01-01' AND '2026-12-31'");
    ASSERT_EQ(chain.steps.size(), 1u);
    ASSERT_EQ(chain.steps[0].residual.size(), 2u);

    auto low = ParseDateLiteral("2026-01-01");
    auto high = ParseDateLiteral("2026-12-31");
    ASSERT_TRUE(low.ok() && high.ok());

    for (const StepPredicate& pred : chain.steps[0].residual) {
        EXPECT_EQ(pred.rhs.literal.type, parser::ValueType::kInt);
    }
    EXPECT_EQ(chain.steps[0].residual[0].rhs.literal.int_val, low.value());
    EXPECT_EQ(chain.steps[0].residual[1].rhs.literal.int_val, high.value());
}

// ---- A literal that does not parse is a positioned compile error --------

TEST_F(TypesPredicateTest, AMalformedLiteralNamesTheByteItStartsAt) {
    // The offset is computed from the statement rather than written down,
    // so the assertion survives anyone rewording the SQL.
    const std::string sql = "SELECT id FROM ev WHERE d = '2026-02-30'";
    const std::size_t at = sql.find('\'');
    ASSERT_NE(at, std::string::npos);

    auto chain = CompileSql(sql);
    ASSERT_FALSE(chain.ok()) << "a date that does not exist must not compile";
    EXPECT_NE(chain.status().message().find("byte " + std::to_string(at)),
              std::string::npos)
        << chain.status().message();
}

TEST_F(TypesPredicateTest, ADecimalLiteralBeyondItsScaleIsRefusedAtCompile) {
    // Refused rather than rounded: '1.234' against DECIMAL(10,2) is a
    // value the column cannot hold, and silently dropping the digit would
    // answer a question the client did not ask.
    auto chain = CompileSql("SELECT id FROM ev WHERE amt = '1.234'");
    ASSERT_FALSE(chain.ok());
    EXPECT_EQ(chain.status().code(), StatusCode::kInvalidArgument) << chain.status().message();
}

TEST_F(TypesPredicateTest, AMalformedLiteralIsRefusedInAnUpdateToo) {
    // The write filter is the second lowering site. A literal that means
    // one thing in a SELECT and another in an UPDATE's WHERE is exactly
    // the drift one shared coercion helper exists to prevent.
    const std::string reply = Run("UPDATE ev SET id = 1 WHERE d = 'not a date'");
    EXPECT_EQ(reply.substr(0, 3), "ERR") << reply;
}

// ---- Column against column: scales must agree ---------------------------

TEST_F(TypesPredicateTest, ComparingDecimalColumnsOfDifferentScaleIsRefused) {
    auto chain = CompileSql("SELECT id FROM money WHERE two = three");
    ASSERT_FALSE(chain.ok());
    EXPECT_EQ(chain.status().code(), StatusCode::kUnsupported) << chain.status().message();
    EXPECT_NE(chain.status().message().find("rescale"), std::string::npos)
        << chain.status().message();
}

TEST_F(TypesPredicateTest, ComparingDecimalColumnsOfTheSameScaleIsFine) {
    auto chain = CompileSql("SELECT id FROM money WHERE two = alsotwo");
    EXPECT_TRUE(chain.ok()) << chain.status().message();
}

// ---- Comparison itself --------------------------------------------------

TEST_F(TypesPredicateTest, DecimalsCompareOnTheirUnscaledIntegers) {
    parser::AstValue a;
    a.type = parser::ValueType::kDecimal;
    a.int_val = 1234;
    a.scale = 2;

    parser::AstValue b;
    b.type = parser::ValueType::kDecimal;
    b.int_val = 1250;
    b.scale = 2;

    EXPECT_TRUE(CompareValues(catalog::kTypeValDecimal, a, b, parser::CompareOp::kLt));
    EXPECT_FALSE(CompareValues(catalog::kTypeValDecimal, a, b, parser::CompareOp::kEq));
    EXPECT_TRUE(CompareValues(catalog::kTypeValDecimal, a, a, parser::CompareOp::kEq));
}

TEST_F(TypesPredicateTest, DecimalsOfDisagreeingScaleAreANonMatchRatherThanRescaled) {
    // The compiler makes this unreachable; the arm answers a non-match
    // anyway, because rescaling inside a per-row predicate is the worst
    // possible place to decide what rescaling means.
    parser::AstValue a;
    a.type = parser::ValueType::kDecimal;
    a.int_val = 1500;
    a.scale = 3;  // 1.500

    parser::AstValue b;
    b.type = parser::ValueType::kDecimal;
    b.int_val = 150;
    b.scale = 2;  // 1.50 - the same number

    EXPECT_FALSE(CompareValues(catalog::kTypeValDecimal, a, b, parser::CompareOp::kEq));
}

TEST_F(TypesPredicateTest, DatesCompareAsTheIntegersTheyAre) {
    auto early = ParseDateLiteral("2026-01-01");
    auto late = ParseDateLiteral("2026-12-31");
    ASSERT_TRUE(early.ok() && late.ok());

    parser::AstValue a;
    a.type = parser::ValueType::kInt;
    a.int_val = early.value();

    parser::AstValue b;
    b.type = parser::ValueType::kInt;
    b.int_val = late.value();

    EXPECT_TRUE(CompareValues(catalog::kTypeValDate, a, b, parser::CompareOp::kLt));
    EXPECT_FALSE(CompareValues(catalog::kTypeValDate, b, a, parser::CompareOp::kLt));
}

// ---- SUM's type rules (spec §3.2) ---------------------------------------

TEST_F(TypesPredicateTest, SumOverADateOrTimestampIsRefused) {
    for (const char* sql : {"SELECT SUM(d) FROM ev", "SELECT SUM(ts) FROM ev"}) {
        auto chain = CompileSql(sql);
        ASSERT_FALSE(chain.ok()) << sql;
        EXPECT_EQ(chain.status().code(), StatusCode::kInvalidArgument) << sql;
    }
}

TEST_F(TypesPredicateTest, SumOverADecimalCompilesAndCarriesTheScale) {
    const StepChain chain = MustCompile("SELECT SUM(amt) FROM ev");
    ASSERT_TRUE(chain.aggregate.has_value());
    ASSERT_EQ(chain.aggregate->items.size(), 1u);
    EXPECT_EQ(chain.aggregate->items[0].type_val, catalog::kTypeValDecimal);
    // Carried at compile because the fold sits outside the executor and
    // has no catalog to ask when it re-attaches the scale.
    EXPECT_EQ(chain.aggregate->items[0].scale, 2);
}

TEST_F(TypesPredicateTest, MinAndMaxOverEveryNewTypeCompile) {
    for (const char* sql : {"SELECT MIN(d) FROM ev", "SELECT MAX(d) FROM ev",
                            "SELECT MIN(ts) FROM ev", "SELECT MAX(amt) FROM ev"}) {
        auto chain = CompileSql(sql);
        EXPECT_TRUE(chain.ok()) << sql << ": " << chain.status().message();
    }
}

// ---- TY06: rendering happens at the boundary ----------------------------
//
// §3.3 and contract item 7. Two things are being pinned: the formats
// themselves, and that `type_val = 0` renders exactly as `FormatValue`
// did before the parameter existed - the compatibility half, which is
// what every non-column caller (a plan's literal, a catalog view, a
// group label in an error) depends on.

TEST_F(TypesPredicateTest, ADateRendersAsItsLiteralAndNotItsEpochDay) {
    ASSERT_EQ(Run("INSERT INTO ev VALUES ('2026-08-07', '2026-08-07 09:15:00.250000', '12.34')")
                  .substr(0, 8),
              "INSERTED");

    const std::string read = Run("SELECT d, ts, amt FROM ev");
    EXPECT_NE(read.find("2026-08-07"), std::string::npos) << read;
    EXPECT_NE(read.find("2026-08-07 09:15:00.250000"), std::string::npos) << read;
    EXPECT_NE(read.find("12.34"), std::string::npos) << read;
    // The stored form must not leak through anywhere in the reply.
    EXPECT_EQ(read.find("20672"), std::string::npos) << read;
}

TEST_F(TypesPredicateTest, StarRendersTheNewTypesToo) {
    // A different code path from a named projection: `SELECT *` has no
    // `projection_types` and renders from the schema the dispatcher
    // already resolved. Both paths have to agree.
    ASSERT_EQ(Run("INSERT INTO ev VALUES ('2026-08-07', '2026-08-07 09:15:00.250000', '12.34')")
                  .substr(0, 8),
              "INSERTED");
    const std::string star = Run("SELECT * FROM ev");
    EXPECT_NE(star.find("2026-08-07 09:15:00.250000"), std::string::npos) << star;
    EXPECT_EQ(star.find("20672"), std::string::npos) << star;
}

TEST_F(TypesPredicateTest, ATimestampWithNoFractionShowsNone) {
    // §3.3's `[PROPOSED]` rule: six fractional digits when non-zero, none
    // when zero. Pinned so that changing it is a decision rather than a
    // drift.
    ASSERT_EQ(Run("INSERT INTO ev VALUES ('2026-08-07', '2026-08-07 09:15:00', '1.00')")
                  .substr(0, 8),
              "INSERTED");
    const std::string read = Run("SELECT ts FROM ev");
    EXPECT_NE(read.find("2026-08-07 09:15:00"), std::string::npos) << read;
    EXPECT_EQ(read.find("09:15:00."), std::string::npos) << read;
}

TEST_F(TypesPredicateTest, ADecimalKeepsItsTrailingZeros) {
    // `12.30`, not `12.3` - the scale is part of the value's meaning, and
    // a client that declared two places asked for two places.
    ASSERT_EQ(Run("INSERT INTO ev VALUES ('2026-08-07', '2026-08-07 09:15:00', '12.30')")
                  .substr(0, 8),
              "INSERTED");
    const std::string read = Run("SELECT amt FROM ev");
    EXPECT_NE(read.find("12.30"), std::string::npos) << read;
}

TEST_F(TypesPredicateTest, AggregatedOutputRendersThroughItsItemType) {
    // The fold's output goes out through a different site than a
    // projection's, so it is its own chance to render an epoch day.
    // MIN(d) is a date; COUNT(*) is a plain integer beside it.
    ASSERT_EQ(Run("INSERT INTO ev VALUES ('2026-08-07', '2026-08-07 09:15:00', '1.00')")
                  .substr(0, 8),
              "INSERTED");
    ASSERT_EQ(Run("INSERT INTO ev VALUES ('2026-01-01', '2026-01-01 00:00:00', '2.50')")
                  .substr(0, 8),
              "INSERTED");

    // The whole row, not four independent substring hits: `find("2")` for
    // the count would match inside `2026` and pin nothing.
    const std::string read = Run("SELECT MIN(d), MAX(d), COUNT(*), SUM(amt) FROM ev");
    EXPECT_NE(read.find("2026-01-01,2026-08-07,2,3.50"), std::string::npos) << read;
}

TEST_F(TypesPredicateTest, TypeValZeroRendersAsItAlwaysDid) {
    // The compatibility half of the sweep. Every caller that has no column
    // type passes 0, and 0 must mean "what this function did before types
    // existed" for every kind that predates them.
    parser::AstValue i;
    i.type = parser::ValueType::kInt;
    i.int_val = 20672;
    EXPECT_EQ(FormatValue(0, i), "20672");

    parser::AstValue s;
    s.type = parser::ValueType::kStr;
    s.str_val = "alice";
    EXPECT_EQ(FormatValue(0, s), "alice");

    parser::AstValue n;
    n.type = parser::ValueType::kNull;
    EXPECT_EQ(FormatValue(0, n), "NULL");

    parser::AstValue p;
    p.type = parser::ValueType::kParam;
    p.str_val = "flag";
    EXPECT_EQ(FormatValue(0, p), "$flag");

    // A uint64 above INT64_MAX keeps its digit text, which is the rule
    // `raw_int_text` exists for and the one most easily broken by a
    // rewrite of this function.
    parser::AstValue big;
    big.type = parser::ValueType::kInt;
    big.raw_int_text = "18446744073709551615";
    EXPECT_EQ(FormatValue(0, big), "18446744073709551615");
}

TEST_F(TypesPredicateTest, ADecimalRendersWithoutAnyColumnType) {
    // The one kind that ignores `type_val` entirely, which is what lets a
    // SUM's folded output and a column read render identically without
    // the caller knowing which it holds.
    parser::AstValue d;
    d.type = parser::ValueType::kDecimal;
    d.int_val = 1230;
    d.scale = 2;
    EXPECT_EQ(FormatValue(0, d), "12.30");
    EXPECT_EQ(FormatValue(catalog::kTypeValDecimal, d), "12.30");
}

TEST_F(TypesPredicateTest, AValueThatIsNotAnIntegerIgnoresADateColumnType) {
    // Guarded on the value as well as the column, so a caller passing a
    // type_val that does not match what it holds gets the value rather
    // than a nonsense date.
    parser::AstValue s;
    s.type = parser::ValueType::kStr;
    s.str_val = "not a date";
    EXPECT_EQ(FormatValue(catalog::kTypeValDate, s), "not a date");
}

}  // namespace
}  // namespace kds::exec

