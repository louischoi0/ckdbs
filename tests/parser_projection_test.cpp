#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "kds/parser/parser.hpp"

// V06 - projection and qualified names (docs/inflight/in-progress/parser-v2-workplan.md).
//
// Two rules, and the second is the one with teeth:
//
//   1. An explicit select list parses: `SELECT a.x, b.y`. Column names may
//      be written qualified or bare anywhere a column appears, not just in
//      an ON clause.
//
//   2. `SELECT *` is refused once there is more than one relation. Which
//      columns it means, and in what order, would be a property of how the
//      relations were joined - and spec section 1 makes written order the
//      client's to choose, so there is no answer the parser is entitled to
//      pick. Unsupported, not InvalidArgument: the statement is
//      well-formed and naming the columns is the fix.
//
// Projection shape must never affect the statement's *class*. Two
// statements differing only in which columns they name read the same rows
// by the same access path. Nothing tags a class yet (V14 does), so what is
// checkable here is the parser's half: the projection is a separate field
// and no other part of the AST changes shape with it.

namespace kds::parser {
namespace {

// By value, deliberately. Returning a reference would dangle at every
// `MustSelect(Parse(...))` call site here: the StatusOr is a temporary
// that dies at the end of the full expression, taking the statement with
// it. A SelectStmt copy costs nothing in a test and cannot be held wrong.
SelectStmt MustSelect(const StatusOr<Statement>& parsed) {
    EXPECT_TRUE(parsed.ok()) << parsed.status().message();
    if (!parsed.ok()) return SelectStmt{};
    return std::get<SelectStmt>(parsed.value());
}

// ---- The select list ------------------------------------------------------

TEST(ParserProjectionTest, StarLeavesTheProjectionEmpty) {
    const SelectStmt sel = MustSelect(Parse("SELECT * FROM t"));
    EXPECT_TRUE(sel.star());
    EXPECT_TRUE(sel.projection.empty());
}

TEST(ParserProjectionTest, ASingleUnqualifiedColumnParses) {
    const SelectStmt sel = MustSelect(Parse("SELECT name FROM t"));
    EXPECT_FALSE(sel.star());
    ASSERT_EQ(sel.projection.size(), 1u);
    EXPECT_EQ(sel.projection[0].name, "name");
    EXPECT_FALSE(sel.projection[0].qualified());
}

TEST(ParserProjectionTest, ColumnsKeepTheirWrittenOrder) {
    // The select list is a client-visible order too: it decides the order
    // values come back in. Sorting or deduplicating it would silently
    // change the shape of every result row.
    const SelectStmt sel = MustSelect(Parse("SELECT c, a, b, a FROM t"));
    ASSERT_EQ(sel.projection.size(), 4u);
    EXPECT_EQ(sel.projection[0].name, "c");
    EXPECT_EQ(sel.projection[1].name, "a");
    EXPECT_EQ(sel.projection[2].name, "b");
    EXPECT_EQ(sel.projection[3].name, "a") << "a repeated column is the client's business";
}

TEST(ParserProjectionTest, QualifiedAndBareColumnsMixInOneList) {
    const SelectStmt sel =
        MustSelect(Parse("SELECT a.x, y, b.z FROM t AS a JOIN u AS b ON a.id = b.id"));
    ASSERT_EQ(sel.projection.size(), 3u);

    EXPECT_EQ(sel.projection[0].qualifier, "a");
    EXPECT_EQ(sel.projection[0].name, "x");
    // Bare in a multi-relation statement is accepted here and resolved
    // later: spec's resolution rule is "resolves iff exactly one visible
    // relation has that column", which needs a catalog the parser does
    // not have. V14 owns it; refusing here would forbid a form the
    // language allows.
    EXPECT_FALSE(sel.projection[1].qualified());
    EXPECT_EQ(sel.projection[1].name, "y");
    EXPECT_EQ(sel.projection[2].qualifier, "b");
    EXPECT_EQ(sel.projection[2].name, "z");
}

TEST(ParserProjectionTest, AMalformedSelectListIsRejected) {
    for (auto sql : {"SELECT FROM t", "SELECT a, FROM t", "SELECT a b FROM t",
                     "SELECT a., b FROM t", "SELECT 42 FROM t", "SELECT a, * FROM t",
                     "SELECT *, a FROM t"}) {
        auto parsed = Parse(sql);
        EXPECT_FALSE(parsed.ok()) << sql;
    }
}

// ---- Star over more than one relation -------------------------------------

TEST(ParserProjectionTest, StarIsUnsupportedOnceThereIsMoreThanOneRelation) {
    //                  0123456
    auto parsed = Parse("SELECT * FROM t JOIN u ON t.id = u.id");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    // The position is the star's own - that is the token the client has
    // to replace.
    EXPECT_NE(parsed.status().message().find("byte 7"), std::string::npos)
        << parsed.status().message();
    EXPECT_NE(parsed.status().message().find("2 relations"), std::string::npos)
        << parsed.status().message();
}

TEST(ParserProjectionTest, StarSurvivesForASingleRelationEvenWithAnAlias) {
    // One relation is never ambiguous, alias or not, and `SELECT *` is
    // what nearly every statement in the corpus says. Refusing it here
    // would break them for no gain.
    EXPECT_TRUE(Parse("SELECT * FROM t").ok());
    EXPECT_TRUE(Parse("SELECT * FROM t AS a").ok());
    EXPECT_TRUE(Parse("SELECT * FROM t WHERE id = 1").ok());
}

TEST(ParserProjectionTest, AnExplicitListMakesAJoinParse) {
    const SelectStmt sel =
        MustSelect(Parse("SELECT t.id, u.note FROM t JOIN u ON t.id = u.t_id"));
    EXPECT_EQ(sel.relation_count(), 2u);
    ASSERT_EQ(sel.projection.size(), 2u);
    EXPECT_EQ(sel.projection[0].qualifier, "t");
    EXPECT_EQ(sel.projection[1].qualifier, "u");
}

TEST(ParserProjectionTest, AMoreSpecificRefusalWinsOverTheStarRule) {
    // Three relations, an outer join and a duplicate binding all in one
    // statement. The star rule is checked last on purpose: "outer joins
    // are not supported" and "t is named twice" tell a client what to do,
    // where "SELECT * is ambiguous" would send them to fix the wrong
    // thing first.
    auto outer = Parse("SELECT * FROM t LEFT JOIN u ON t.id = u.id");
    ASSERT_FALSE(outer.ok());
    EXPECT_NE(outer.status().message().find("outer join"), std::string::npos)
        << outer.status().message();

    auto dup = Parse("SELECT * FROM t JOIN t ON t.id = t.id");
    ASSERT_FALSE(dup.ok());
    EXPECT_NE(dup.status().message().find("alias"), std::string::npos) << dup.status().message();
}

// ---- Qualified names in WHERE ---------------------------------------------

TEST(ParserProjectionTest, AWhereColumnMayBeQualified) {
    const SelectStmt sel =
        MustSelect(Parse("SELECT t.id FROM t JOIN u ON t.id = u.id WHERE u.status = 'open'"));
    ASSERT_EQ(sel.where.size(), 1u);
    EXPECT_EQ(sel.where[0].col.qualifier, "u");
    EXPECT_EQ(sel.where[0].col.name, "status");
}

TEST(ParserProjectionTest, ABareWhereColumnStillParsesUnchanged) {
    // The spelling every existing statement uses. It must reach the same
    // ColumnName, with no qualifier, or the whole corpus moves.
    const SelectStmt sel = MustSelect(Parse("SELECT * FROM t WHERE id = 1 AND name = 'x'"));
    ASSERT_EQ(sel.where.size(), 2u);
    EXPECT_FALSE(sel.where[0].col.qualified());
    EXPECT_EQ(sel.where[0].col.name, "id");
    EXPECT_EQ(sel.where[1].col.name, "name");
}

TEST(ParserProjectionTest, AQualifiedWhereColumnCarriesItsPosition) {
    //                  0123456789012345678901234
    const SelectStmt sel = MustSelect(Parse("SELECT * FROM t WHERE t.id = 1"));
    ASSERT_EQ(sel.where.size(), 1u);
    // The offset is the qualifier's, which is what an "names no relation"
    // message has to point at.
    EXPECT_EQ(sel.where[0].col.byte_offset, 22u);
}

TEST(ParserProjectionTest, AnUpdateWhereMayAlsoBeQualified) {
    auto parsed = Parse("UPDATE t SET a = 1 WHERE t.id = 2");
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    const auto& upd = std::get<UpdateStmt>(parsed.value());
    ASSERT_EQ(upd.where.size(), 1u);
    EXPECT_EQ(upd.where[0].col.qualifier, "t");
}

TEST(ParserProjectionTest, AnUpdateSetTargetStaysUnqualified) {
    // UPDATE reads one relation, so a qualifier on a SET target says
    // nothing the statement does not already say. Not accepted rather
    // than accepted-and-ignored.
    EXPECT_FALSE(Parse("UPDATE t SET t.a = 1").ok());
}

}  // namespace
}  // namespace kds::parser
