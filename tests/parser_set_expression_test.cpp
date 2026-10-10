// **A SET value is a value expression** (BJ-S2,
// `instructions/v3.0.0/workorder-bj-expression-update.md` BJ-R1 family 1,
// BJ-R6, BJ-Q9).
//
// What this file pins: the arithmetic tokens, the minus rule (a `-` before a
// digit stays part of the literal and the parser reads it as a minus in
// binary position), the precedence of the family-1 operators, the shape rule
// (operators and columns are the statement's shape, a literal is a value),
// and the refusal of every form a later stage builds - by name, at its byte.

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "kds/parser/fingerprint.hpp"
#include "kds/parser/lexer.hpp"
#include "kds/parser/parser.hpp"

namespace kds::parser {
namespace {

std::vector<TokenType> Types(std::string_view sql) {
    Lexer lex(sql);
    std::vector<TokenType> out;
    for (;;) {
        const Token t = lex.Next();
        out.push_back(t.type);
        if (t.type == TokenType::kEof) break;
    }
    return out;
}

// The one assignment of `UPDATE t SET a = <value>`.
StatusOr<Assignment> SetOf(const std::string& value) {
    auto parsed = Parse("UPDATE t SET a = " + value);
    if (!parsed.ok()) return parsed.status();
    return std::get<UpdateStmt>(parsed.value()).assignments.at(0);
}

// A tree rendered as a string, parenthesised, so precedence is read off one
// line: `a + b * 2` is `(a + (b * 2))`.
std::string Render(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::kLiteral:
            return e.literal.type == ValueType::kInt
                       ? e.literal.raw_int_text
                       : (e.literal.type == ValueType::kNull ? "NULL"
                                                             : "'" + e.literal.str_val + "'");
        case Expr::Kind::kColumn: return e.column;
        case Expr::Kind::kUnary:
            return std::string(e.op == ExprOp::kNeg ? "(-" : "(+") + Render(*e.lhs) + ")";
        case Expr::Kind::kBinary: {
            static constexpr const char* kOps[] = {"+", "-", "*", "/", "%", "||"};
            return "(" + Render(*e.lhs) + " " + kOps[static_cast<int>(e.op)] + " " +
                   Render(*e.rhs) + ")";
        }
    }
    return "?";
}

std::string Shape(const std::string& value) {
    auto a = SetOf(value);
    EXPECT_TRUE(a.ok()) << value << ": " << a.status().message();
    if (!a.ok()) return "<error>";
    EXPECT_NE(a.value().expr, nullptr) << value << " should be an expression";
    return a.value().expr == nullptr ? "<bare>" : Render(*a.value().expr);
}

Fingerprint Must(const std::string& sql) {
    auto fp = FingerprintOf(sql);
    EXPECT_TRUE(fp.has_value()) << sql;
    return fp.value_or(Fingerprint{});
}

TEST(SetExpressionLexTest, TheArithmeticOperatorsAreTokens) {
    EXPECT_EQ(Types("a + b - c * d / e % f || g"),
              (std::vector<TokenType>{
                  TokenType::kIdent, TokenType::kPlus, TokenType::kIdent, TokenType::kMinus,
                  TokenType::kIdent, TokenType::kStar, TokenType::kIdent, TokenType::kSlash,
                  TokenType::kIdent, TokenType::kPercent, TokenType::kIdent, TokenType::kConcat,
                  TokenType::kIdent, TokenType::kEof}));
}

TEST(SetExpressionLexTest, AMinusBeforeADigitStaysPartOfTheLiteral) {
    // BJ-Q9 (a): the lexer is unchanged for every statement that parses
    // today, so no stored pattern_id moves.
    EXPECT_EQ(Types("a -1"),
              (std::vector<TokenType>{TokenType::kIdent, TokenType::kIntLit, TokenType::kEof}));
    EXPECT_EQ(Types("a-1"),
              (std::vector<TokenType>{TokenType::kIdent, TokenType::kIntLit, TokenType::kEof}));
    EXPECT_EQ(Types("a - 1"),
              (std::vector<TokenType>{TokenType::kIdent, TokenType::kMinus, TokenType::kIntLit,
                                      TokenType::kEof}));
    // `--` is a comment, so `a--1` is `a`.
    EXPECT_EQ(Types("a--1"), (std::vector<TokenType>{TokenType::kIdent, TokenType::kEof}));
}

TEST(SetExpressionLexTest, ALoneBarIsStillAnError) {
    EXPECT_EQ(Types("a | b")[1], TokenType::kError);
}

TEST(SetExpressionParseTest, PrecedenceIsPostgresqlsLoosestFirst) {
    EXPECT_EQ(Shape("a + b * 2"), "(a + (b * 2))");
    EXPECT_EQ(Shape("a * 2 + b % 3 / 4"), "((a * 2) + ((b % 3) / 4))");
    EXPECT_EQ(Shape("(a + b) * 2"), "((a + b) * 2)");
    // `||` is looser than `+`.
    EXPECT_EQ(Shape("s || a + 1"), "(s || (a + 1))");
    // Left associative.
    EXPECT_EQ(Shape("a - b - c"), "((a - b) - c)");
    EXPECT_EQ(Shape("a / b / c"), "((a / b) / c)");
    EXPECT_EQ(Shape("s || 'x' || t"), "((s || 'x') || t)");
}

TEST(SetExpressionParseTest, AUnarySignBindsTighterThanAMultiplication) {
    EXPECT_EQ(Shape("-a * 2"), "((-a) * 2)");
    EXPECT_EQ(Shape("- 5"), "(-5)");
    EXPECT_EQ(Shape("a * - b"), "(a * (-b))");
    EXPECT_EQ(Shape("+a"), "(+a)");
}

TEST(SetExpressionParseTest, ASignedLiteralInBinaryPositionIsAMinus) {
    // BJ-Q9 (a). `a -1` is `a - 1`, and what follows binds as it would
    // after an explicit minus.
    EXPECT_EQ(Shape("a -1"), "(a - 1)");
    EXPECT_EQ(Shape("a-1"), "(a - 1)");
    EXPECT_EQ(Shape("a -1 * 2"), "(a - (1 * 2))");
    // After an explicit operator the sign belongs to the literal.
    EXPECT_EQ(Shape("a - -1"), "(a - -1)");
    EXPECT_EQ(Shape("a + -1"), "(a + -1)");
}

TEST(SetExpressionParseTest, ASignedLiteralKeepsItsFullMagnitude) {
    // Read from the digits, never from the fused value, so the magnitude of
    // INT64_MIN survives.
    auto a = SetOf("a -9223372036854775808");
    ASSERT_TRUE(a.ok()) << a.status().message();
    ASSERT_NE(a.value().expr, nullptr);
    ASSERT_EQ(a.value().expr->kind, Expr::Kind::kBinary);
    EXPECT_EQ(a.value().expr->op, ExprOp::kSub);
    EXPECT_EQ(a.value().expr->rhs->literal.raw_int_text, "9223372036854775808");
}

TEST(SetExpressionParseTest, ASignedDecimalInBinaryPositionIsAMinusAndItsMagnitude) {
    auto a = SetOf("a -1.5");
    ASSERT_TRUE(a.ok()) << a.status().message();
    ASSERT_NE(a.value().expr, nullptr);
    EXPECT_EQ(a.value().expr->op, ExprOp::kSub);
    EXPECT_EQ(a.value().expr->rhs->literal.str_val, "1.5");
    EXPECT_TRUE(a.value().expr->rhs->bare_numeric);
}

TEST(SetExpressionParseTest, ABareNumberIsToldFromAString) {
    auto n = SetOf("a + 1.5");
    auto s = SetOf("a + '1.5'");
    ASSERT_TRUE(n.ok() && s.ok());
    EXPECT_TRUE(n.value().expr->rhs->bare_numeric);
    EXPECT_FALSE(s.value().expr->rhs->bare_numeric);
}

TEST(SetExpressionParseTest, OneBareLiteralKeepsTodaysShape) {
    // BJ-Q3 (c): such an assignment keeps the per-row gate at the encode,
    // and every reader of `val` stays right.
    for (const char* lit : {"5", "-5", "'x'", "NULL", "1.5"}) {
        auto a = SetOf(lit);
        ASSERT_TRUE(a.ok()) << lit << ": " << a.status().message();
        EXPECT_EQ(a.value().expr, nullptr) << lit;
    }
    EXPECT_EQ(SetOf("5").value().val.int_val, 5);
    EXPECT_EQ(SetOf("-5").value().val.int_val, -5);
}

TEST(SetExpressionParseTest, AParenthesisedLiteralIsAnExpression) {
    auto a = SetOf("(5)");
    ASSERT_TRUE(a.ok()) << a.status().message();
    EXPECT_NE(a.value().expr, nullptr);
}

TEST(SetExpressionParseTest, AColumnAloneIsAnExpression) {
    auto a = SetOf("b");
    ASSERT_TRUE(a.ok()) << a.status().message();
    ASSERT_NE(a.value().expr, nullptr);
    EXPECT_EQ(a.value().expr->kind, Expr::Kind::kColumn);
    EXPECT_EQ(a.value().expr->column, "b");
}

TEST(SetExpressionParseTest, EveryNodeCarriesItsByte) {
    //                0         1         2
    //                012345678901234567890123456789
    const std::string sql = "UPDATE t SET a = b + 12 * c";
    auto parsed = Parse(sql);
    ASSERT_TRUE(parsed.ok());
    const Expr& sum = *std::get<UpdateStmt>(parsed.value()).assignments.at(0).expr;
    EXPECT_EQ(sum.byte_offset, sql.find("b + 12"));
    EXPECT_EQ(sum.op_byte_offset, sql.find('+'));
    EXPECT_EQ(sum.rhs->op_byte_offset, sql.find('*'));
    EXPECT_EQ(sum.rhs->lhs->byte_offset, sql.find("12"));
}

TEST(SetExpressionParseTest, AFormALaterStageBuildsIsRefusedByNameAtItsByte) {
    struct Case {
        const char* value;
        const char* names;
        std::size_t at;
    };
    const std::string head = "UPDATE t SET a = ";
    const std::vector<Case> cases = {
        {"abs(a)", "BJ-S6", 0},          {"a + abs(b)", "BJ-S6", 4},
        {"a = 1", "BJ-S5", 2},           {"a < 1", "BJ-S5", 2},
        {"a AND b", "BJ-S5", 2},         {"a IS NULL", "BJ-S5", 2},
        {"NOT a", "BJ-S5", 0},           {"CASE WHEN a THEN 1 END", "BJ-S5", 0},
        {"(SELECT 1)", "BJ-S7", 0},      {"1 + (SELECT 1)", "BJ-S7", 4},
        {"t.b", "qualified", 0},
        {"CAST(a AS int)", "BJ-S5", 0},  {"COALESCE(a, 1)", "BJ-S5", 0},
        {"a + GREATEST(a, 1)", "BJ-S5", 4},
        {"a IN (1)", "BJ-S5", 2},        {"a BETWEEN 1 AND 2", "BJ-S5", 2},
        {"a::int", "BJ-S5", 1},          {"CASE 1 WHEN 1 THEN 2 END", "BJ-S5", 0},
        {"CASE -1 WHEN 1 THEN 2 END", "BJ-S5", 0},
    };
    for (const Case& c : cases) {
        auto parsed = Parse(head + c.value);
        ASSERT_FALSE(parsed.ok()) << c.value;
        EXPECT_EQ(parsed.status().code(), StatusCode::kNotImplemented)
            << c.value << ": " << parsed.status().message();
        EXPECT_NE(parsed.status().message().find(c.names), std::string::npos)
            << c.value << ": " << parsed.status().message();
        EXPECT_NE(parsed.status().message().find("byte " + std::to_string(head.size() + c.at)),
                  std::string::npos)
            << c.value << ": " << parsed.status().message();
    }
}

TEST(SetExpressionParseTest, AColumnNamedCaseIsStillAColumn) {
    // `CASE` is not reserved (CLAUDE.md: nothing new is reserved lightly).
    auto a = SetOf("case");
    ASSERT_TRUE(a.ok()) << a.status().message();
    ASSERT_NE(a.value().expr, nullptr);
    EXPECT_EQ(a.value().expr->column, "case");
    auto with_where = Parse("UPDATE t SET a = case WHERE id = 1");
    EXPECT_TRUE(with_where.ok()) << with_where.status().message();
}

TEST(SetExpressionParseTest, AMalformedExpressionIsInvalidNotNotImplemented) {
    for (const char* bad : {"a +", "(a + 1", "a + * 2", "* 2", ")"}) {
        auto parsed = Parse(std::string("UPDATE t SET a = ") + bad);
        ASSERT_FALSE(parsed.ok()) << bad;
        EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument)
            << bad << ": " << parsed.status().message();
    }
}

TEST(SetExpressionParseTest, NestingIsBounded) {
    const std::string deep = std::string(200, '(') + "a" + std::string(200, ')');
    auto parsed = Parse("UPDATE t SET a = " + deep);
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported) << parsed.status().message();
    // `--` opens a comment, so spell the signs apart.
    std::string spaced;
    for (int i = 0; i < 200; ++i) spaced += "- ";
    auto chain = Parse("UPDATE t SET a = " + spaced + "a");
    ASSERT_FALSE(chain.ok());
    EXPECT_EQ(chain.status().code(), StatusCode::kUnsupported) << chain.status().message();
}

TEST(SetExpressionParseTest, ALongChainIsBoundedByItsHeightNotOnlyItsNesting) {
    // `1+1+1+...` leans entirely left, so no parenthesis or sign ever counts
    // it, and a tree a million levels tall crashed the process in its
    // destructor (BJ-S2's review). 200 terms is fine; 10,000 is refused.
    const auto Chain = [](int terms, std::string_view op) {
        std::string sql = "UPDATE t SET a = a";
        for (int i = 0; i < terms; ++i) sql += std::string(" ") + std::string(op) + " 1";
        return sql;
    };
    EXPECT_TRUE(Parse(Chain(200, "+")).ok());
    for (std::string_view op : {"+", "*", "||"}) {
        auto parsed = Parse(Chain(10000, op));
        ASSERT_FALSE(parsed.ok()) << op;
        EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported) << op;
    }
    EXPECT_EQ(Parse(Chain(1000000, "+")).status().code(), StatusCode::kUnsupported)
        << "a megabyte of operators must be refused, not walked";
}

TEST(SetExpressionShapeTest, ALiteralIsAValueButAnOperatorIsShape) {
    // BJ-R6: `v + 1` and `v + 2` share a pattern and differ in their
    // arguments; `v + 1` and `v - 1` are two patterns.
    const Fingerprint plus1 = Must("UPDATE t SET v = v + 1 WHERE id = 7");
    const Fingerprint plus2 = Must("UPDATE t SET v = v + 2 WHERE id = 7");
    const Fingerprint minus1 = Must("UPDATE t SET v = v - 1 WHERE id = 7");
    EXPECT_EQ(plus1.pattern_id, plus2.pattern_id);
    EXPECT_NE(plus1.arg_hash, plus2.arg_hash);
    EXPECT_NE(plus1.pattern_id, minus1.pattern_id);
}

TEST(SetExpressionShapeTest, EveryOperatorAndEveryReferencedColumnIsShape) {
    const char* const kStatements[] = {
        "UPDATE t SET v = v + 1",  "UPDATE t SET v = v - 1",  "UPDATE t SET v = v * 1",
        "UPDATE t SET v = v / 1",  "UPDATE t SET v = v % 1",  "UPDATE t SET v = v || 1",
        "UPDATE t SET v = w + 1",  "UPDATE t SET v = (v + 1)", "UPDATE t SET v = -v",
        "UPDATE t SET v = v + 1 + 1",
    };
    std::vector<std::uint64_t> seen;
    for (const char* sql : kStatements) {
        const std::uint64_t id = Must(sql).pattern_id;
        for (std::uint64_t other : seen) EXPECT_NE(id, other) << sql;
        seen.push_back(id);
    }
}

TEST(SetExpressionShapeTest, TheTwoSpellingsOfAMinusAreTwoShapesAndOneMeaning) {
    // BJ-Q9 (a)'s stated cost: `v -1` fuses the sign into the literal and
    // `v - 1` does not, so the shapes differ. The meaning does not.
    EXPECT_NE(Must("UPDATE t SET v = v -1").pattern_id,
              Must("UPDATE t SET v = v - 1").pattern_id);
    EXPECT_EQ(Shape("v -1"), Shape("v - 1"));
}

TEST(SetExpressionShapeTest, NoStoredShapeMovedWithTheNewTokens) {
    // The statements that parsed before BJ-S2 and carry a `-` are the ones a
    // changed minus rule could move. They keep one `kValue` per signed
    // literal.
    EXPECT_EQ(Must("SELECT * FROM t WHERE x = -7").pattern_id,
              Must("SELECT * FROM t WHERE x = 7").pattern_id);
    EXPECT_EQ(Must("SELECT * FROM t WHERE x BETWEEN -5 AND -1").pattern_id,
              Must("SELECT * FROM t WHERE x BETWEEN 5 AND 1").pattern_id);
}

}  // namespace
}  // namespace kds::parser
