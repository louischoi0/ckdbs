#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <variant>

#include <gtest/gtest.h>

#include "kds/parser/parser.hpp"

// V09 - the pagination tail (docs/spec/parser-v2.md I11,
// docs/inflight/in-progress/parser-v2-workplan.md V09).
//
// Three things this file holds down, in descending order of how expensive
// they would be to get wrong:
//
//   1. **Nothing is reserved.** `ORDER`, `BY`, `ASC`, `DESC`, `LIMIT` and
//      `OFFSET` are ordinary identifiers matched by text at clause
//      position, so a column or table may carry any of those names, every
//      previously-parsing statement lexes to the same token stream, and
//      `kFingerprintVersion` does not move. The golden corpus pins the
//      hashes; this file pins the grammar that keeps them true.
//
//   2. **The counts are slots** (I11): `LIMIT 10` and `LIMIT 20` share a
//      pattern_id and differ in arg_hash, so a limited statement is one
//      pattern rather than one per count - which is what lets a Waystone
//      trail serve it per instance.
//
//   3. **Every refusal carries a position**: `DESC`, an aggregated
//      statement's `LIMIT`, a subquery's tail, a count that is not a
//      non-negative integer. `[AMENDED 2026-08-24 — HV4]` An aggregated
//      statement's `ORDER BY` is no longer among them: it parses, with an
//      aggregate admitted as a key.
//
// What the parser deliberately does not decide: whether `order_by` names
// the driving relation's pk. Pk-ness is catalog knowledge, so that check -
// and the acceptance of `ORDER BY <pk> [ASC]` as the validated no-op it is
// - lives in the compiler (V09's execution half), pointed at the byte the
// AST stored here.

namespace kds::parser {
namespace {

// By value, for the reason parser_projection_test.cpp gives: the StatusOr
// is a temporary and a reference into it dangles at the call site.
SelectStmt MustSelect(const StatusOr<Statement>& parsed) {
    EXPECT_TRUE(parsed.ok()) << parsed.status().message();
    if (!parsed.ok()) return SelectStmt{};
    return std::get<SelectStmt>(parsed.value());
}

// A refusal's message must name the byte the offending token starts at.
// Checked as a substring rather than by parsing the message, because the
// wording is allowed to improve and the position is not.
::testing::AssertionResult MentionsByte(const Status& status, std::uint32_t offset) {
    const std::string& msg = status.message();
    const std::string want = "byte " + std::to_string(offset);
    if (msg.find(want) != std::string::npos) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << "expected the message to name '" << want << "', got: " << msg;
}

// The byte a word starts at, so a test's expected position comes from the
// statement itself rather than from a hand count that rots when the
// statement is edited.
std::uint32_t ByteOf(std::string_view sql, std::string_view word) {
    const auto at = sql.find(word);
    EXPECT_NE(at, std::string_view::npos) << word << " not in: " << sql;
    return static_cast<std::uint32_t>(at);
}

// ---- Accepted forms -------------------------------------------------------

TEST(ParserPaginationTest, LimitAlone) {
    const SelectStmt sel = MustSelect(Parse("SELECT a FROM t LIMIT 10"));
    ASSERT_TRUE(sel.limit.has_value());
    EXPECT_EQ(sel.limit.value(), 10u);
    EXPECT_EQ(sel.offset, 0u);
    EXPECT_TRUE(sel.order_by.empty());
}

TEST(ParserPaginationTest, OffsetAlone) {
    const SelectStmt sel = MustSelect(Parse("SELECT a FROM t OFFSET 5"));
    EXPECT_FALSE(sel.limit.has_value());
    EXPECT_EQ(sel.offset, 5u);
}

TEST(ParserPaginationTest, LimitThenOffset) {
    const SelectStmt sel = MustSelect(Parse("SELECT a FROM t LIMIT 10 OFFSET 5"));
    ASSERT_TRUE(sel.limit.has_value());
    EXPECT_EQ(sel.limit.value(), 10u);
    EXPECT_EQ(sel.offset, 5u);
}

// LIMIT 0 is a legal statement that emits nothing - a real value, not an
// absent clause, which is the whole reason the AST field is optional.
TEST(ParserPaginationTest, LimitZeroIsAValueNotAnAbsence) {
    const SelectStmt sel = MustSelect(Parse("SELECT a FROM t LIMIT 0"));
    ASSERT_TRUE(sel.limit.has_value());
    EXPECT_EQ(sel.limit.value(), 0u);
}

TEST(ParserPaginationTest, OrderByBareColumn) {
    const SelectStmt sel = MustSelect(Parse("SELECT a FROM t ORDER BY id"));
    ASSERT_EQ(sel.order_by.size(), 1u);
    EXPECT_EQ(sel.order_by[0].key.column.name, "id");
    EXPECT_FALSE(sel.order_by[0].key.column.qualified());
    EXPECT_FALSE(sel.order_by[0].descending);
}

TEST(ParserPaginationTest, OrderByQualifiedWithAsc) {
    const SelectStmt sel = MustSelect(Parse("SELECT a FROM t ORDER BY t.id ASC"));
    ASSERT_EQ(sel.order_by.size(), 1u);
    EXPECT_EQ(sel.order_by[0].key.column.qualifier, "t");
    EXPECT_EQ(sel.order_by[0].key.column.name, "id");
    EXPECT_FALSE(sel.order_by[0].descending);
}

// ---- OB1: what the clause accepts now -------------------------------------

// `DESC` used to be refused because "every chain links forward only". An
// output sort does not walk - it orders what the walk emitted - so the
// reason is gone rather than outvoted.
TEST(ParserPaginationTest, DescReachesTheAst) {
    const SelectStmt sel = MustSelect(Parse("SELECT a FROM t ORDER BY id DESC"));
    ASSERT_EQ(sel.order_by.size(), 1u);
    EXPECT_EQ(sel.order_by[0].key.column.name, "id");
    EXPECT_TRUE(sel.order_by[0].descending);
}

// Written order is significant and each key carries its own direction:
// `ORDER BY x DESC, y` is descending on x and *ascending* on y, which is
// the standard's reading and the one a client writing it expects.
TEST(ParserPaginationTest, MultipleKeysKeepWrittenOrderAndPerKeyDirection) {
    const SelectStmt sel = MustSelect(Parse("SELECT a FROM t ORDER BY x DESC, t.y, z ASC"));
    ASSERT_EQ(sel.order_by.size(), 3u);
    EXPECT_EQ(sel.order_by[0].key.column.name, "x");
    EXPECT_TRUE(sel.order_by[0].descending);
    EXPECT_EQ(sel.order_by[1].key.column.qualifier, "t");
    EXPECT_EQ(sel.order_by[1].key.column.name, "y");
    EXPECT_FALSE(sel.order_by[1].descending);
    EXPECT_EQ(sel.order_by[2].key.column.name, "z");
    EXPECT_FALSE(sel.order_by[2].descending);
}

TEST(ParserPaginationTest, EightKeysAreAccepted) {
    const SelectStmt sel =
        MustSelect(Parse("SELECT a FROM t ORDER BY c1, c2, c3, c4, c5, c6, c7, c8"));
    EXPECT_EQ(sel.order_by.size(), 8u);
}

// The cap refuses at the byte of the key that crossed it - the ninth - and
// not at the clause, which would leave the client counting commas.
TEST(ParserPaginationTest, TheKeyCountCapRefusesAtTheOffendingKey) {
    const std::string_view sql = "SELECT a FROM t ORDER BY c1, c2, c3, c4, c5, c6, c7, c8, c9";
    const auto parsed = Parse(sql);
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_TRUE(MentionsByte(parsed.status(), ByteOf(sql, "c9")));
}

TEST(ParserPaginationTest, TheFullTail) {
    const std::string_view sql =
        "SELECT a FROM t WHERE a = 1 ORDER BY id ASC LIMIT 10 OFFSET 5";
    const SelectStmt sel = MustSelect(Parse(sql));
    ASSERT_EQ(sel.order_by.size(), 1u);
    EXPECT_EQ(sel.order_by[0].key.column.name, "id");
    ASSERT_TRUE(sel.limit.has_value());
    EXPECT_EQ(sel.limit.value(), 10u);
    EXPECT_EQ(sel.offset, 5u);
    EXPECT_EQ(sel.where.size(), 1u);
}

TEST(ParserPaginationTest, TailAfterAJoin) {
    const SelectStmt sel = MustSelect(
        Parse("SELECT a.x, b.y FROM a JOIN b ON a.id = b.aid LIMIT 3"));
    ASSERT_TRUE(sel.limit.has_value());
    EXPECT_EQ(sel.limit.value(), 3u);
    EXPECT_EQ(sel.relation_count(), 2u);
}

// The count is decoded from its digits, so the full unsigned range is
// representable - int_val's signed decode could not judge this literal.
TEST(ParserPaginationTest, LimitTakesTheFullUnsignedRange) {
    const SelectStmt sel =
        MustSelect(Parse("SELECT a FROM t LIMIT 18446744073709551615"));
    ASSERT_TRUE(sel.limit.has_value());
    EXPECT_EQ(sel.limit.value(), std::numeric_limits<std::uint64_t>::max());
}

// ---- Nothing is reserved --------------------------------------------------

TEST(ParserPaginationTest, ColumnsMayBeNamedLimitAndOffset) {
    const SelectStmt sel = MustSelect(Parse("SELECT limit, offset FROM t"));
    ASSERT_EQ(sel.projection.size(), 2u);
    EXPECT_EQ(sel.projection[0].name, "limit");
    EXPECT_EQ(sel.projection[1].name, "offset");
}

TEST(ParserPaginationTest, ATableMayBeNamedOrder) {
    const SelectStmt sel = MustSelect(Parse("SELECT a FROM order LIMIT 1"));
    EXPECT_EQ(sel.from.table_name, "order");
}

TEST(ParserPaginationTest, AWhereColumnMayBeNamedLimit) {
    const SelectStmt sel = MustSelect(Parse("SELECT a FROM t WHERE limit = 1"));
    ASSERT_EQ(sel.where.size(), 1u);
    EXPECT_EQ(sel.where[0].col.name, "limit");
}

// ---- Refusals: expressions and ordinals -----------------------------------

TEST(ParserPaginationTest, OrderByAnExpressionIsUnsupported) {
    const std::string_view sql = "SELECT a FROM t ORDER BY count(x)";
    const auto parsed = Parse(sql);
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_TRUE(MentionsByte(parsed.status(), ByteOf(sql, "count")));
}

// `ORDER BY 1` stays refused after OB1 lifted the DESC and multi-key
// refusals beside it, and for a reason neither of those had: the ordinal
// names a select-list position, which is a second spelling of something
// that already has one. An output sort existing changes nothing about it.
TEST(ParserPaginationTest, AnOrdinalOrderByIsUnsupported) {
    const std::string_view sql = "SELECT a FROM t ORDER BY 1";
    const auto parsed = Parse(sql);
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_TRUE(MentionsByte(parsed.status(), ByteOf(sql, "1")));
}

// ---- Refusals: aggregated output ------------------------------------------

TEST(ParserPaginationTest, LimitOverAnAggregateIsUnsupported) {
    const std::string_view sql = "SELECT b, COUNT(*) FROM t GROUP BY b LIMIT 5";
    const auto parsed = Parse(sql);
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_TRUE(MentionsByte(parsed.status(), ByteOf(sql, "LIMIT")));
}

TEST(ParserPaginationTest, OffsetOverAnAggregateIsUnsupported) {
    const std::string_view sql = "SELECT COUNT(*) FROM t OFFSET 2";
    const auto parsed = Parse(sql);
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_TRUE(MentionsByte(parsed.status(), ByteOf(sql, "OFFSET")));
}

// `SELECT b FROM t GROUP BY b` names no function and is still a fold - the
// tail's aggregated refusal keys on the same test the items landing does.
TEST(ParserPaginationTest, AGroupByAloneIsAggregatedForTheTail) {
    const auto parsed = Parse("SELECT b FROM t GROUP BY b LIMIT 1");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
}

// `[AMENDED 2026-08-24 — docs/inflight/in-progress/workplan-having.md HV4]` This was a refusal
// for as long as the sort could not order what a fold emits. It parses now,
// and the parser stores what was written: which relation a name belongs to,
// and whether it is a grouping key, stay the compiler's.
TEST(ParserPaginationTest, OrderByOverAnAggregateParses) {
    const SelectStmt sel =
        MustSelect(Parse("SELECT b, COUNT(*) FROM t GROUP BY b ORDER BY b DESC"));
    ASSERT_EQ(sel.order_by.size(), 1u);
    EXPECT_FALSE(sel.order_by[0].key.is_aggregate);
    EXPECT_EQ(sel.order_by[0].key.column.name, "b");
    EXPECT_TRUE(sel.order_by[0].descending);
}

// The key an aggregated statement can name that no column reference can.
TEST(ParserPaginationTest, OrderByAnAggregateOverAFoldParses) {
    const SelectStmt sel = MustSelect(
        Parse("SELECT b, COUNT(*) FROM t GROUP BY b ORDER BY COUNT(*) DESC, b"));
    ASSERT_EQ(sel.order_by.size(), 2u);
    EXPECT_TRUE(sel.order_by[0].key.is_aggregate);
    EXPECT_EQ(sel.order_by[0].key.func, AggFunc::kCount);
    EXPECT_TRUE(sel.order_by[0].key.star_arg);
    EXPECT_TRUE(sel.order_by[0].descending);
    EXPECT_FALSE(sel.order_by[1].key.is_aggregate);
    EXPECT_EQ(sel.order_by[1].key.column.name, "b");
}

// An aggregate not in the select list is a *hidden item* at compile (HV-2),
// which is a compiler question - the parser's job is to carry it through.
TEST(ParserPaginationTest, OrderByAnAggregateTheSelectListDoesNotNameParses) {
    const SelectStmt sel =
        MustSelect(Parse("SELECT b FROM t GROUP BY b ORDER BY SUM(DISTINCT qty)"));
    ASSERT_EQ(sel.order_by.size(), 1u);
    EXPECT_TRUE(sel.order_by[0].key.is_aggregate);
    EXPECT_EQ(sel.order_by[0].key.func, AggFunc::kSum);
    EXPECT_TRUE(sel.order_by[0].key.distinct);
    EXPECT_EQ(sel.order_by[0].key.column.name, "qty");
}

// Without a fold there is nothing for an aggregate key to be the answer of,
// so OB1's refusal stands unchanged - wording, code and byte.
TEST(ParserPaginationTest, OrderByAnAggregateWithoutAFoldIsStillUnsupported) {
    const std::string_view sql = "SELECT a FROM t ORDER BY COUNT(*)";
    const auto parsed = Parse(sql);
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_TRUE(MentionsByte(parsed.status(), ByteOf(sql, "COUNT")));
}

// A call this grammar has no function for, over a fold: refused at the
// key's own byte rather than left to fail as trailing garbage past it.
TEST(ParserPaginationTest, OrderByANonAggregateCallOverAFoldIsUnsupported) {
    const std::string_view sql = "SELECT b, COUNT(*) FROM t GROUP BY b ORDER BY foo(b)";
    const auto parsed = Parse(sql);
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_TRUE(MentionsByte(parsed.status(), ByteOf(sql, "foo")));
}

// ---- Refusals: subquery position ------------------------------------------

TEST(ParserPaginationTest, LimitInASubqueryIsUnsupported) {
    const std::string_view sql =
        "SELECT a FROM t WHERE id IN (SELECT id FROM u LIMIT 3)";
    const auto parsed = Parse(sql);
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_TRUE(MentionsByte(parsed.status(), ByteOf(sql, "LIMIT")));
}

TEST(ParserPaginationTest, OrderByInASubqueryIsUnsupported) {
    const std::string_view sql =
        "SELECT a FROM t WHERE EXISTS (SELECT x FROM u ORDER BY id)";
    const auto parsed = Parse(sql);
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_TRUE(MentionsByte(parsed.status(), ByteOf(sql, "ORDER")));
}

// The depth rule reaches every WHERE, not just a SELECT's: UPDATE and
// DELETE parse their predicates through the same production.
TEST(ParserPaginationTest, DeleteWhereSubqueryTailIsRefusedToo) {
    const auto parsed =
        Parse("DELETE FROM t WHERE EXISTS (SELECT x FROM u LIMIT 1)");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
}

// ---- Refusals: the count --------------------------------------------------

TEST(ParserPaginationTest, ANegativeLimitIsInvalid) {
    const auto parsed = Parse("SELECT a FROM t LIMIT -1");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument);
}

// `1.5` lexes as one numeric token since TY10, so this is a typed refusal
// naming the literal, not a syntax error at the dot.
TEST(ParserPaginationTest, ADecimalLimitIsInvalid) {
    const auto parsed = Parse("SELECT a FROM t LIMIT 1.5");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument);
}

TEST(ParserPaginationTest, AStringOffsetIsInvalid) {
    const auto parsed = Parse("SELECT a FROM t OFFSET 'x'");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument);
}

TEST(ParserPaginationTest, AMissingCountIsInvalid) {
    const auto parsed = Parse("SELECT a FROM t LIMIT");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument);
}

TEST(ParserPaginationTest, ABindParameterCountIsInvalid) {
    const auto parsed = Parse("SELECT a FROM t LIMIT ?");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument);
}

// One past uint64 max: refused, never wrapped - TY11's lesson one layer
// down, and the message names the digits the client wrote.
TEST(ParserPaginationTest, AnOverflowingLimitRefusesRatherThanWraps) {
    const auto parsed = Parse("SELECT a FROM t LIMIT 18446744073709551616");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(parsed.status().message().find("does not fit"), std::string::npos)
        << parsed.status().message();
}

// ---- Refusals: clause order -----------------------------------------------

// The clause order is the grammar (I11): OFFSET before LIMIT is trailing
// garbage, not an alternate spelling. Dialect compatibility is a non-goal
// (I13), and the MySQL comma form falls out the same way.
TEST(ParserPaginationTest, OffsetBeforeLimitIsTrailingGarbage) {
    const auto parsed = Parse("SELECT a FROM t OFFSET 5 LIMIT 10");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument);
}

TEST(ParserPaginationTest, TheMysqlCommaFormIsRefused) {
    const auto parsed = Parse("SELECT a FROM t LIMIT 10, 5");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument);
}

// ---- The counts are slots (I11) -------------------------------------------

std::optional<Fingerprint> FingerprintFor(std::string_view sql) {
    Parser parser(sql);
    auto parsed = parser.Parse();
    EXPECT_TRUE(parsed.ok()) << parsed.status().message();
    return parser.fingerprint();
}

TEST(ParserPaginationTest, LimitCountsShareAPattern) {
    const auto ten = FingerprintFor("SELECT a FROM t LIMIT 10");
    const auto twenty = FingerprintFor("SELECT a FROM t LIMIT 20");
    ASSERT_TRUE(ten.has_value());
    ASSERT_TRUE(twenty.has_value());
    EXPECT_EQ(ten->pattern_id, twenty->pattern_id);
    EXPECT_NE(ten->arg_hash, twenty->arg_hash);
}

TEST(ParserPaginationTest, OffsetCountsShareAPatternToo) {
    const auto five = FingerprintFor("SELECT a FROM t LIMIT 10 OFFSET 5");
    const auto six = FingerprintFor("SELECT a FROM t LIMIT 10 OFFSET 6");
    ASSERT_TRUE(five.has_value());
    ASSERT_TRUE(six.has_value());
    EXPECT_EQ(five->pattern_id, six->pattern_id);
    EXPECT_NE(five->arg_hash, six->arg_hash);
}

// Writing the clause is part of the shape: a limited statement and its
// unlimited twin are different patterns, so a trail recorded under one is
// never consulted for the other.
TEST(ParserPaginationTest, WritingLimitChangesThePattern) {
    const auto limited = FingerprintFor("SELECT a FROM t LIMIT 10");
    const auto unlimited = FingerprintFor("SELECT a FROM t");
    ASSERT_TRUE(limited.has_value());
    ASSERT_TRUE(unlimited.has_value());
    EXPECT_NE(limited->pattern_id, unlimited->pattern_id);
}

}  // namespace
}  // namespace kds::parser
