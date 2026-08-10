#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "kds/parser/parser.hpp"

// AG01 - the aggregate grammar (docs/feat-aggregate.md §2,
// docs/workplan-aggregate.md).
//
// Three things this file is here to hold down, in descending order of how
// expensive they would be to get wrong:
//
//   1. **Nothing is reserved.** A function head is an unqualified name from
//      the set *followed by a paren*, and no production in this grammar puts
//      a paren after a column reference. So a column may still be named
//      `count` or `group`, every previously-accepted statement lexes to the
//      same token stream, and `kFingerprintVersion` does not move. The
//      golden corpus is the evidence for the hashes; this file is the
//      evidence for the grammar that makes it true.
//
//   2. **Every refusal carries a position.** §2's table is a list of forms
//      this engine understands and declines, and a client that gets
//      "not supported, byte 23" is told where to look, where a bare syntax
//      error sends them hunting for a typo that is not there.
//
//   3. **A non-aggregated statement is unchanged.** Items are staged during
//      the parse and collapse back into `projection` when no aggregate and
//      no GROUP BY was written, so a plain SELECT produces the same AST it
//      produced before this clause existed - which is what makes AG1's
//      "the chain is byte-identical" claim reachable at all.

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

// ---- The four functions parse --------------------------------------------

TEST(ParserAggregateTest, CountStarParses) {
    const SelectStmt sel = MustSelect(Parse("SELECT COUNT(*) FROM t"));
    ASSERT_TRUE(sel.aggregated());
    ASSERT_EQ(sel.agg_items.size(), 1u);
    EXPECT_TRUE(sel.agg_items[0].is_aggregate);
    EXPECT_EQ(sel.agg_items[0].func, AggFunc::kCount);
    EXPECT_TRUE(sel.agg_items[0].star_arg);
    EXPECT_FALSE(sel.agg_items[0].distinct);
    // An aggregated statement is never a star, however empty its
    // projection is - which is the trap `star()` exists to avoid.
    EXPECT_FALSE(sel.star());
    EXPECT_TRUE(sel.projection.empty());
}

TEST(ParserAggregateTest, EachFunctionOverAColumnParses) {
    struct Case {
        const char* sql;
        AggFunc func;
    };
    const Case cases[] = {
        {"SELECT COUNT(x) FROM t", AggFunc::kCount},
        {"SELECT SUM(x) FROM t", AggFunc::kSum},
        {"SELECT MIN(x) FROM t", AggFunc::kMin},
        {"SELECT MAX(x) FROM t", AggFunc::kMax},
    };
    for (const Case& c : cases) {
        const SelectStmt sel = MustSelect(Parse(c.sql));
        ASSERT_EQ(sel.agg_items.size(), 1u) << c.sql;
        EXPECT_TRUE(sel.agg_items[0].is_aggregate) << c.sql;
        EXPECT_EQ(sel.agg_items[0].func, c.func) << c.sql;
        EXPECT_FALSE(sel.agg_items[0].star_arg) << c.sql;
        EXPECT_EQ(sel.agg_items[0].column.name, "x") << c.sql;
    }
}

TEST(ParserAggregateTest, LowerCaseAndQualifiedArgumentsParse) {
    const SelectStmt sel = MustSelect(Parse("SELECT sum(a.qty) FROM orders AS a"));
    ASSERT_EQ(sel.agg_items.size(), 1u);
    EXPECT_EQ(sel.agg_items[0].func, AggFunc::kSum);
    EXPECT_EQ(sel.agg_items[0].column.qualifier, "a");
    EXPECT_EQ(sel.agg_items[0].column.name, "qty");
}

TEST(ParserAggregateTest, DistinctIsRecordedPerItem) {
    const SelectStmt sel =
        MustSelect(Parse("SELECT COUNT(DISTINCT x), SUM(y) FROM t"));
    ASSERT_EQ(sel.agg_items.size(), 2u);
    EXPECT_TRUE(sel.agg_items[0].distinct);
    EXPECT_FALSE(sel.agg_items[1].distinct);
}

TEST(ParserAggregateTest, ItemsKeepTheirWrittenOrder) {
    // The select list decides the order values come back in, aggregated or
    // not. A fold that reordered its own output would be answering a
    // different statement.
    const SelectStmt sel =
        MustSelect(Parse("SELECT b, COUNT(*), SUM(x) FROM t GROUP BY b"));
    ASSERT_EQ(sel.agg_items.size(), 3u);
    EXPECT_FALSE(sel.agg_items[0].is_aggregate);
    EXPECT_EQ(sel.agg_items[0].column.name, "b");
    EXPECT_EQ(sel.agg_items[1].func, AggFunc::kCount);
    EXPECT_EQ(sel.agg_items[2].func, AggFunc::kSum);
}

// ---- GROUP BY -------------------------------------------------------------

TEST(ParserAggregateTest, GroupByKeysKeepTheirWrittenOrder) {
    const SelectStmt sel =
        MustSelect(Parse("SELECT a, b, COUNT(*) FROM t GROUP BY a, b"));
    ASSERT_EQ(sel.group_by.size(), 2u);
    EXPECT_EQ(sel.group_by[0].name, "a");
    EXPECT_EQ(sel.group_by[1].name, "b");
}

TEST(ParserAggregateTest, GroupByWithoutAnAggregateIsStillAFold) {
    // It names no function and still emits one row per group rather than
    // one per row, which is a fold by every property that matters.
    const SelectStmt sel = MustSelect(Parse("SELECT b FROM t GROUP BY b"));
    EXPECT_TRUE(sel.aggregated());
    ASSERT_EQ(sel.agg_items.size(), 1u);
    EXPECT_FALSE(sel.agg_items[0].is_aggregate);
    EXPECT_TRUE(sel.projection.empty());
}

TEST(ParserAggregateTest, GroupByFollowsTheWhereClause) {
    const SelectStmt sel =
        MustSelect(Parse("SELECT b, COUNT(*) FROM t WHERE q = 1 GROUP BY b"));
    ASSERT_EQ(sel.where.size(), 1u);
    ASSERT_EQ(sel.group_by.size(), 1u);
    EXPECT_EQ(sel.group_by[0].name, "b");
}

TEST(ParserAggregateTest, AQualifiedGroupingKeyParses) {
    const SelectStmt sel = MustSelect(
        Parse("SELECT a.b, COUNT(*) FROM t AS a JOIN u AS c ON a.id = c.id GROUP BY a.b"));
    ASSERT_EQ(sel.group_by.size(), 1u);
    EXPECT_EQ(sel.group_by[0].qualifier, "a");
    EXPECT_EQ(sel.group_by[0].name, "b");
}

// ---- Nothing is reserved --------------------------------------------------

TEST(ParserAggregateTest, AColumnMayStillBeNamedCount) {
    const SelectStmt sel = MustSelect(Parse("SELECT count FROM t"));
    EXPECT_FALSE(sel.aggregated());
    ASSERT_EQ(sel.projection.size(), 1u);
    EXPECT_EQ(sel.projection[0].name, "count");
}

TEST(ParserAggregateTest, AColumnNamedCountIsUsableInEveryPosition) {
    const SelectStmt sel =
        MustSelect(Parse("SELECT count, sum FROM t WHERE count = 1 AND min = 2"));
    EXPECT_FALSE(sel.aggregated());
    ASSERT_EQ(sel.projection.size(), 2u);
    EXPECT_EQ(sel.projection[1].name, "sum");
    ASSERT_EQ(sel.where.size(), 2u);
    EXPECT_EQ(sel.where[0].col.name, "count");
}

TEST(ParserAggregateTest, AColumnMayStillBeNamedGroup) {
    // `GROUP` is read as a clause head only past the WHERE, where no column
    // reference can stand. Everywhere else it is an identifier.
    const SelectStmt sel = MustSelect(Parse("SELECT group FROM t WHERE group = 1"));
    EXPECT_FALSE(sel.aggregated());
    ASSERT_EQ(sel.projection.size(), 1u);
    EXPECT_EQ(sel.projection[0].name, "group");
}

TEST(ParserAggregateTest, AQualifiedNameFromTheFunctionSetIsAColumn) {
    // `a.count` is not a function head: a head is *unqualified*, and a
    // qualified name is a column reference in every production there is.
    const SelectStmt sel = MustSelect(Parse("SELECT a.count FROM t AS a"));
    EXPECT_FALSE(sel.aggregated());
    ASSERT_EQ(sel.projection.size(), 1u);
    EXPECT_EQ(sel.projection[0].qualifier, "a");
    EXPECT_EQ(sel.projection[0].name, "count");
}

TEST(ParserAggregateTest, APlainSelectIsUnchangedByTheClauseExisting) {
    // The whole of AG1 rests on this: a statement that does not aggregate
    // produces the AST it produced before, so its chain and its fingerprint
    // cannot have moved either.
    const SelectStmt sel = MustSelect(Parse("SELECT a, b FROM t WHERE id = 1"));
    EXPECT_FALSE(sel.aggregated());
    EXPECT_TRUE(sel.agg_items.empty());
    EXPECT_TRUE(sel.group_by.empty());
    ASSERT_EQ(sel.projection.size(), 2u);
    EXPECT_EQ(sel.projection[0].name, "a");
    EXPECT_EQ(sel.projection[1].name, "b");
}

TEST(ParserAggregateTest, StarIsStillAStar) {
    const SelectStmt sel = MustSelect(Parse("SELECT * FROM t"));
    EXPECT_TRUE(sel.star());
    EXPECT_FALSE(sel.aggregated());
}

TEST(ParserAggregateTest, AMultiRelationNamedListIsNotMisreadAsAStar) {
    // The items are staged in a local while the FROM list is read, so the
    // ambiguity check must test what was *written* rather than whether the
    // projection is empty - otherwise every multi-relation SELECT that
    // spells its columns out is refused as an ambiguous star.
    const SelectStmt sel =
        MustSelect(Parse("SELECT a.x, b.y FROM a JOIN b ON a.id = b.id"));
    EXPECT_FALSE(sel.star());
    EXPECT_EQ(sel.projection.size(), 2u);
}

// ---- §2's refusal table ---------------------------------------------------

TEST(ParserAggregateTest, StarWithGroupByIsRefused) {
    const StatusOr<Statement> parsed = Parse("SELECT * FROM t GROUP BY a");
    ASSERT_FALSE(parsed.ok());
    // Which columns `*` folds was never written; naming them is not a
    // feature request, so this is InvalidArgument rather than Unsupported.
    EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument);
    EXPECT_TRUE(MentionsByte(parsed.status(), 7));
}

TEST(ParserAggregateTest, AvgParsesLikeAnyOtherAggregate) {
    // This test's predecessor pinned the refusal; the flip is the AVG
    // decision landing (feat-aggregate.md §3.4, 2026-08-07). The *grammar*
    // half is now ordinary - the type half (decimal columns only) is the
    // compiler's, tested where the other per-type rules are.
    const StatusOr<Statement> parsed = Parse("SELECT AVG(x) FROM t");
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    const auto* sel = std::get_if<SelectStmt>(&parsed.value());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->agg_items.size(), 1u);
    EXPECT_EQ(sel->agg_items[0].func, AggFunc::kAvg);
    EXPECT_FALSE(sel->agg_items[0].distinct);
    EXPECT_EQ(sel->agg_items[0].column.name, "x");

    const StatusOr<Statement> distinct = Parse("SELECT AVG(DISTINCT x) FROM t");
    ASSERT_TRUE(distinct.ok()) << distinct.status().message();
    EXPECT_TRUE(std::get_if<SelectStmt>(&distinct.value())->agg_items[0].distinct);

    // AVG(*) rides the same refusal every non-COUNT head has.
    const StatusOr<Statement> star = Parse("SELECT AVG(*) FROM t");
    ASSERT_FALSE(star.ok());
    EXPECT_EQ(star.status().code(), StatusCode::kInvalidArgument);
}

TEST(ParserAggregateTest, StarIsOnlyAnArgumentOfCount) {
    for (const char* sql : {"SELECT SUM(*) FROM t", "SELECT MIN(*) FROM t",
                            "SELECT MAX(*) FROM t"}) {
        const StatusOr<Statement> parsed = Parse(sql);
        ASSERT_FALSE(parsed.ok()) << sql;
        EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument) << sql;
        EXPECT_TRUE(MentionsByte(parsed.status(), 11)) << sql;
    }
}

TEST(ParserAggregateTest, CountDistinctStarIsRefused) {
    const StatusOr<Statement> parsed = Parse("SELECT COUNT(DISTINCT *) FROM t");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument);
    EXPECT_TRUE(MentionsByte(parsed.status(), 22));
}

TEST(ParserAggregateTest, HavingIsRefusedWithItsOwnPosition) {
    const StatusOr<Statement> parsed =
        Parse("SELECT b, COUNT(*) FROM t GROUP BY b HAVING COUNT(*) > 1");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_TRUE(MentionsByte(parsed.status(), 37));
}

TEST(ParserAggregateTest, OrderByOverAggregatedOutputIsRefused) {
    const StatusOr<Statement> parsed =
        Parse("SELECT b, COUNT(*) FROM t GROUP BY b ORDER BY b");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_TRUE(MentionsByte(parsed.status(), 37));
}

TEST(ParserAggregateTest, ANonAggregatedOrderByParsesSinceV09) {
    // This test used to pin the opposite: ORDER BY was not supported
    // anywhere, and only the aggregated refusal was this clause's
    // business. V09 made the non-aggregated form parse - the verdict
    // flipped with its task, exactly as the corpus comment schedules it -
    // while the aggregated refusal above kept its answer. The tail's own
    // grammar lives in parser_pagination_test.cpp; what this file still
    // holds down is the boundary between the two forms.
    const StatusOr<Statement> parsed = Parse("SELECT * FROM t ORDER BY id");
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
}

TEST(ParserAggregateTest, AnAggregateInsideASubqueryIsRefused) {
    const StatusOr<Statement> parsed =
        Parse("SELECT * FROM t WHERE id = (SELECT COUNT(*) FROM u)");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_TRUE(MentionsByte(parsed.status(), 35));
}

TEST(ParserAggregateTest, AGroupByInsideASubqueryIsRefused) {
    const StatusOr<Statement> parsed =
        Parse("SELECT * FROM t WHERE id IN (SELECT id FROM u GROUP BY id)");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_TRUE(MentionsByte(parsed.status(), 55));
}

TEST(ParserAggregateTest, GroupByTakesColumnReferencesOnly) {
    const StatusOr<Statement> parsed =
        Parse("SELECT COUNT(*) FROM t GROUP BY count(x)");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_TRUE(MentionsByte(parsed.status(), 32));
}

TEST(ParserAggregateTest, AnUnclosedAggregateArgumentIsASyntaxError) {
    const StatusOr<Statement> parsed = Parse("SELECT COUNT(x FROM t");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace kds::parser
