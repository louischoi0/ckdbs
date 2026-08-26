#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "kds/parser/parser.hpp"

// V07 - predicate-position subqueries (docs/inflight/in-progress/parser-v2-workplan.md).
//
// Four forms are in scope, and they are in scope *only* in predicate
// position: scalar comparison, `IN`/`NOT IN`, `EXISTS`/`NOT EXISTS`.
// Everything else that looks like nesting - a derived table, a CTE - is
// out, and the reason is structural rather than a matter of effort: a
// derived table's result must become a relation with a schema,
// materialized and probed by something other than a pk, which breaks
// pk-direct probing into the next step and puts a temporary relation in
// the storage layer.
//
// The rule that enforces it, from spec section 2: **the relation-reference
// production must never reach the statement production**, and `WITH` must
// not lex as a statement head. Both are tested below by their *answer* -
// Unsupported with an exact position, never a bare syntax error and never
// an accepted parse - because that is what a client sees.

namespace kds::parser {
namespace {

SelectStmt MustSelect(const StatusOr<Statement>& parsed) {
    EXPECT_TRUE(parsed.ok()) << parsed.status().message();
    if (!parsed.ok()) return SelectStmt{};
    return std::get<SelectStmt>(parsed.value());
}

// ---- The four accepted forms ----------------------------------------------

TEST(ParserSubqueryTest, AScalarComparisonSubqueryParses) {
    const SelectStmt sel =
        MustSelect(Parse("SELECT * FROM t WHERE id = (SELECT id FROM u WHERE flag = 1)"));
    ASSERT_EQ(sel.where.size(), 1u);
    EXPECT_EQ(sel.where[0].kind, PredicateKind::kCompareSubquery);
    EXPECT_EQ(sel.where[0].op, CompareOp::kEq);
    EXPECT_EQ(sel.where[0].col.name, "id");
    ASSERT_TRUE(sel.where[0].has_subquery());

    // The nested block is a full query block, not a fragment.
    const SelectStmt& inner = *sel.where[0].subquery;
    EXPECT_EQ(inner.from.table_name, "u");
    ASSERT_EQ(inner.projection.size(), 1u);
    EXPECT_EQ(inner.projection[0].name, "id");
    ASSERT_EQ(inner.where.size(), 1u);
    EXPECT_EQ(inner.where[0].col.name, "flag");
}

TEST(ParserSubqueryTest, AScalarSubqueryWorksWithEveryComparisonOperator) {
    // Cardinality is not provable here (spec section 2), so none of these
    // is a parse error; more than one row is a runtime
    // CardinalityViolation. What the parser owes is the operator.
    struct Case {
        const char* sql;
        CompareOp op;
    };
    const Case cases[] = {
        {"SELECT * FROM t WHERE id = (SELECT id FROM u)", CompareOp::kEq},
        {"SELECT * FROM t WHERE id != (SELECT id FROM u)", CompareOp::kNeq},
        {"SELECT * FROM t WHERE id < (SELECT id FROM u)", CompareOp::kLt},
        {"SELECT * FROM t WHERE id >= (SELECT id FROM u)", CompareOp::kGte},
    };
    for (const Case& c : cases) {
        const SelectStmt sel = MustSelect(Parse(c.sql));
        ASSERT_EQ(sel.where.size(), 1u) << c.sql;
        EXPECT_EQ(sel.where[0].kind, PredicateKind::kCompareSubquery) << c.sql;
        EXPECT_EQ(sel.where[0].op, c.op) << c.sql;
    }
}

TEST(ParserSubqueryTest, InAndNotInParseToDistinctKinds) {
    const SelectStmt in = MustSelect(Parse("SELECT * FROM t WHERE id IN (SELECT t_id FROM u)"));
    ASSERT_EQ(in.where.size(), 1u);
    EXPECT_EQ(in.where[0].kind, PredicateKind::kInSubquery);
    EXPECT_EQ(in.where[0].col.name, "id");

    // NOT IN is a different predicate, not IN with a flag: it compiles to
    // NotExists, which is search-class and never trail-replayable, where
    // IN compiles to Exists and is replayable on a positive result. The
    // AST has to keep them apart for that to be decidable later.
    const SelectStmt not_in =
        MustSelect(Parse("SELECT * FROM t WHERE id NOT IN (SELECT t_id FROM u)"));
    ASSERT_EQ(not_in.where.size(), 1u);
    EXPECT_EQ(not_in.where[0].kind, PredicateKind::kNotInSubquery);
    EXPECT_EQ(not_in.where[0].col.name, "id");
}

TEST(ParserSubqueryTest, ExistsAndNotExistsCarryNoColumn) {
    const SelectStmt ex = MustSelect(Parse("SELECT * FROM t WHERE EXISTS (SELECT * FROM u)"));
    ASSERT_EQ(ex.where.size(), 1u);
    EXPECT_EQ(ex.where[0].kind, PredicateKind::kExists);
    EXPECT_TRUE(ex.where[0].col.name.empty()) << "EXISTS has nothing on its left";
    ASSERT_TRUE(ex.where[0].has_subquery());

    const SelectStmt nex =
        MustSelect(Parse("SELECT * FROM t WHERE NOT EXISTS (SELECT * FROM u)"));
    ASSERT_EQ(nex.where.size(), 1u);
    EXPECT_EQ(nex.where[0].kind, PredicateKind::kNotExists);
}

TEST(ParserSubqueryTest, ACorrelatedSubqueryParsesLikeAnyOther) {
    // Correlation is a property the compiler discovers (V15), not a
    // syntax: the parser's only job is to keep the qualified reference.
    const SelectStmt sel = MustSelect(
        Parse("SELECT * FROM t WHERE EXISTS (SELECT * FROM u WHERE u.t_id = 1)"));
    const SelectStmt& inner = *sel.where[0].subquery;
    ASSERT_EQ(inner.where.size(), 1u);
    EXPECT_EQ(inner.where[0].col.qualifier, "u");
}

TEST(ParserSubqueryTest, SubqueriesCombineWithAndAndWithOrdinaryPredicates) {
    const SelectStmt sel = MustSelect(
        Parse("SELECT * FROM t WHERE id = 1 AND EXISTS (SELECT * FROM u) AND name = 'x'"));
    ASSERT_EQ(sel.where.size(), 3u);
    EXPECT_EQ(sel.where[0].kind, PredicateKind::kCompareValue);
    EXPECT_EQ(sel.where[1].kind, PredicateKind::kExists);
    EXPECT_EQ(sel.where[2].kind, PredicateKind::kCompareValue);
    // Written order, as everywhere else.
    EXPECT_EQ(sel.where[0].col.name, "id");
    EXPECT_EQ(sel.where[2].col.name, "name");
}

TEST(ParserSubqueryTest, AnUpdateWhereTakesSubqueriesToo) {
    auto parsed = Parse("UPDATE t SET a = 1 WHERE id IN (SELECT t_id FROM u)");
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    const auto& upd = std::get<UpdateStmt>(parsed.value());
    ASSERT_EQ(upd.where.size(), 1u);
    EXPECT_EQ(upd.where[0].kind, PredicateKind::kInSubquery);
}

TEST(ParserSubqueryTest, ASubqueryMayItselfJoinAndProject) {
    const SelectStmt sel = MustSelect(Parse(
        "SELECT * FROM t WHERE id IN (SELECT a.id FROM u AS a JOIN v AS b ON a.id = b.id)"));
    const SelectStmt& inner = *sel.where[0].subquery;
    EXPECT_EQ(inner.relation_count(), 2u);
    ASSERT_EQ(inner.projection.size(), 1u);
    EXPECT_EQ(inner.projection[0].qualifier, "a");
}

// ---- The depth cap --------------------------------------------------------

// Builds `SELECT * FROM t WHERE EXISTS (` * n + `SELECT * FROM t` + `)` * n,
// i.e. a statement with exactly `n` nested query blocks under the outer one.
std::string NestedTo(int n) {
    std::string sql = "SELECT * FROM t";
    for (int i = 0; i < n; ++i) sql += " WHERE EXISTS (SELECT * FROM t";
    for (int i = 0; i < n; ++i) sql += ")";
    return sql;
}

TEST(ParserSubqueryTest, NestingAtTheCapParsesAndOneDeeperIsUnsupported) {
    // The boundary, from both sides. A cap that is never reached is not a
    // cap, and one that is off by one silently forbids a legal statement.
    const std::string at_cap = NestedTo(static_cast<int>(kMaxSubqueryDepth));
    auto ok = Parse(at_cap);
    EXPECT_TRUE(ok.ok()) << at_cap << ": " << ok.status().message();

    const std::string past_cap = NestedTo(static_cast<int>(kMaxSubqueryDepth) + 1);
    auto too_deep = Parse(past_cap);
    ASSERT_FALSE(too_deep.ok()) << past_cap;
    EXPECT_EQ(too_deep.status().code(), StatusCode::kUnsupported);
    EXPECT_NE(too_deep.status().message().find(std::to_string(kMaxSubqueryDepth)),
              std::string::npos)
        << too_deep.status().message();
}

TEST(ParserSubqueryTest, DeepNestingIsRefusedRatherThanOverflowingTheStack) {
    // The reason the cap exists at all: the parser recurses per level, so
    // an uncapped nest is a stack overflow reachable from one client
    // string. This must return a Status, not die.
    auto parsed = Parse(NestedTo(500));
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
}

TEST(ParserSubqueryTest, DepthUnwindsSoSiblingSubqueriesEachGetTheFullBudget) {
    // Two subqueries side by side are each at depth 1, not 1 and 2. A
    // depth counter kept as parser state rather than as a parameter is
    // exactly how this breaks - it would have to be restored by hand on
    // every path out.
    auto parsed = Parse(
        "SELECT * FROM t WHERE EXISTS (SELECT * FROM u) AND EXISTS (SELECT * FROM v)");
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    const SelectStmt sel = MustSelect(parsed);
    ASSERT_EQ(sel.where.size(), 2u);
    EXPECT_EQ(sel.where[0].subquery->from.table_name, "u");
    EXPECT_EQ(sel.where[1].subquery->from.table_name, "v");
}

// ---- Nesting outside predicate position -----------------------------------

TEST(ParserSubqueryTest, ASubqueryInFromIsUnsupportedWithItsPosition) {
    //                  01234567890123
    auto parsed = Parse("SELECT * FROM (SELECT * FROM u)");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported)
        << "a derived table must not read as a syntax error";
    EXPECT_NE(parsed.status().message().find("byte 14"), std::string::npos)
        << parsed.status().message();
    EXPECT_NE(parsed.status().message().find("FROM"), std::string::npos)
        << parsed.status().message();
}

TEST(ParserSubqueryTest, ASubqueryInAJoinsRelationPositionIsUnsupportedToo) {
    // The rule is on the relation-reference production, so it holds
    // wherever a relation reference appears - not just after FROM.
    auto parsed = Parse("SELECT t.id FROM t JOIN (SELECT * FROM u) ON t.id = u.id");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
}

TEST(ParserSubqueryTest, ACteIsUnsupportedRatherThanAnUnknownKeyword) {
    //                  0123
    auto parsed = Parse("WITH x AS (SELECT * FROM u) SELECT * FROM x");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported)
        << "WITH is declined, not unrecognized";
    EXPECT_NE(parsed.status().message().find("byte 0"), std::string::npos)
        << parsed.status().message();
}

TEST(ParserSubqueryTest, ASubqueryInAnInsertOrUpdateValuePositionIsRejected) {
    // Spec section 2 lists these as out of scope, `[OPEN: revisit]`. They
    // are refused because no value production reaches a SELECT - which is
    // the same structural rule, and keeps the option open.
    EXPECT_FALSE(Parse("INSERT INTO t VALUES ((SELECT id FROM u))").ok());
    EXPECT_FALSE(Parse("UPDATE t SET a = (SELECT id FROM u)").ok());
}

// ---- Malformed subqueries -------------------------------------------------

TEST(ParserSubqueryTest, AnUnclosedOrEmptySubqueryIsRejected) {
    for (auto sql : {"SELECT * FROM t WHERE EXISTS (SELECT * FROM u",
                     "SELECT * FROM t WHERE EXISTS ()", "SELECT * FROM t WHERE EXISTS (SELECT)",
                     "SELECT * FROM t WHERE EXISTS", "SELECT * FROM t WHERE id IN ()",
                     "SELECT * FROM t WHERE id = (SELECT)"}) {
        auto parsed = Parse(sql);
        EXPECT_FALSE(parsed.ok()) << sql;
    }
}

TEST(ParserSubqueryTest, AnInValueListIsStillRejectedAndSaysWhat) {
    // V08's, not V07's. The message has to name what it wanted, since
    // `IN (1, 2)` is the spelling a client reaches for first.
    auto parsed = Parse("SELECT * FROM t WHERE id IN (1, 2)");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(parsed.status().message().find("SELECT"), std::string::npos)
        << parsed.status().message();
}

TEST(ParserSubqueryTest, BareNotIsRefusedWithAnExplanation) {
    // `NOT` is reserved but negation is only available in the two forms
    // spec I10 admits; there is no expression tree for a bare NOT to
    // negate. Refused by name rather than as a mystery syntax error.
    for (auto sql : {"SELECT * FROM t WHERE NOT id = 1", "SELECT * FROM t WHERE id NOT = 1"}) {
        auto parsed = Parse(sql);
        ASSERT_FALSE(parsed.ok()) << sql;
        EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported) << sql;
        EXPECT_NE(parsed.status().message().find("NOT"), std::string::npos)
            << sql << ": " << parsed.status().message();
    }
}

TEST(ParserSubqueryTest, ASemicolonDoesNotEndANestedBlock) {
    // Only the outermost block may be followed by one. Swallowing a ';'
    // inside a subquery would accept a statement boundary where there is
    // none.
    EXPECT_FALSE(Parse("SELECT * FROM t WHERE EXISTS (SELECT * FROM u;)").ok());
    EXPECT_TRUE(Parse("SELECT * FROM t WHERE EXISTS (SELECT * FROM u);").ok());
}

}  // namespace
}  // namespace kds::parser
