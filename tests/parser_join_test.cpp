#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "kds/parser/parser.hpp"

// V05 - joins and aliases (docs/inflight/in-progress/parser-v2-workplan.md).
//
// The corpus (tests/testdata/parser_corpus.txt) pins the verdict of every
// form here; this file pins the *shape* the accepted ones parse into,
// which the corpus deliberately does not record. Two properties carry the
// weight:
//
//   written order   spec section 1 makes it a client contract - written
//                   order is execution order, and nothing reorders it.
//                   Asserting it at the AST is the earliest place it can
//                   be checked, and the only place until a step chain
//                   exists to check it at.
//
//   bindings        every relation in a FROM list has a distinct name.
//                   The refusal is what makes a self-join expressible: a
//                   parser that silently accepted `FROM t JOIN t` would
//                   have to guess which `t` a predicate meant.

namespace kds::parser {
namespace {

const SelectStmt& MustSelect(const StatusOr<Statement>& parsed) {
    EXPECT_TRUE(parsed.ok()) << parsed.status().message();
    return std::get<SelectStmt>(parsed.value());
}

TEST(ParserJoinTest, ASingleRelationStatementHasNoJoins) {
    auto parsed = Parse("SELECT * FROM accounts");
    const SelectStmt& sel = MustSelect(parsed);
    EXPECT_EQ(sel.from.table_name, "accounts");
    EXPECT_TRUE(sel.joins.empty());
    EXPECT_EQ(sel.relation_count(), 1u);
    // With no alias the binding is the table name itself.
    EXPECT_EQ(sel.from.binding(), "accounts");
}

TEST(ParserJoinTest, AnAliasBecomesTheBindingAndTheTableNameSurvives) {
    auto parsed = Parse("SELECT * FROM accounts AS a");
    const SelectStmt& sel = MustSelect(parsed);
    EXPECT_EQ(sel.from.table_name, "accounts") << "the catalog is still looked up by this";
    EXPECT_EQ(sel.from.alias, "a");
    EXPECT_EQ(sel.from.binding(), "a") << "but predicates refer to it by this";
}

TEST(ParserJoinTest, WrittenOrderIsPreservedAcrossAChainOfJoins) {
    // The contract: three relations, and the AST holds them in the order
    // the client wrote them. A parser that normalized or reordered this -
    // by relation name, by estimated size, by anything - would break the
    // one guarantee spec section 1 offers about execution.
    auto parsed = Parse(
        "SELECT a.id, c.note FROM a JOIN b ON a.id = b.a_id JOIN c ON b.id = c.b_id");
    const SelectStmt& sel = MustSelect(parsed);

    ASSERT_EQ(sel.relation_count(), 3u);
    EXPECT_EQ(sel.from.table_name, "a");
    ASSERT_EQ(sel.joins.size(), 2u);
    EXPECT_EQ(sel.joins[0].relation.table_name, "b");
    EXPECT_EQ(sel.joins[1].relation.table_name, "c");

    // Each join keeps the predicate that attached it, sides in written
    // order - `a.id = b.a_id` must not come back as `b.a_id = a.id`.
    EXPECT_EQ(sel.joins[0].left.qualifier, "a");
    EXPECT_EQ(sel.joins[0].left.name, "id");
    EXPECT_EQ(sel.joins[0].right.qualifier, "b");
    EXPECT_EQ(sel.joins[0].right.name, "a_id");
    EXPECT_EQ(sel.joins[1].left.qualifier, "b");
    EXPECT_EQ(sel.joins[1].right.qualifier, "c");
}

TEST(ParserJoinTest, ASelfJoinParsesWhenEachOccurrenceHasItsOwnAlias) {
    auto parsed = Parse("SELECT a.id FROM t AS a JOIN t AS b ON a.parent_id = b.id");
    const SelectStmt& sel = MustSelect(parsed);

    ASSERT_EQ(sel.joins.size(), 1u);
    // One table, two relations: the whole point of aliases.
    EXPECT_EQ(sel.from.table_name, "t");
    EXPECT_EQ(sel.joins[0].relation.table_name, "t");
    EXPECT_EQ(sel.from.binding(), "a");
    EXPECT_EQ(sel.joins[0].relation.binding(), "b");
}

TEST(ParserJoinTest, OneRelationNamedTwiceWithoutAliasesIsUnsupportedNotAmbiguous) {
    auto parsed = Parse("SELECT * FROM t JOIN t ON t.id = t.parent_id");
    ASSERT_FALSE(parsed.ok());
    // Unsupported, not InvalidArgument: this is a well-formed statement
    // with a meaning in standard SQL that this engine declines to guess
    // at. The message has to say how to fix it, since "t is named twice"
    // is not actionable on its own.
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_NE(parsed.status().message().find("alias"), std::string::npos)
        << parsed.status().message();
}

TEST(ParserJoinTest, TwoRelationsSharingAnAliasIsRefusedForTheSameReason) {
    // Distinct tables, one binding - the ambiguity is in the binding, not
    // in the table name, so the check has to be on binding().
    auto parsed = Parse("SELECT * FROM accounts AS x JOIN trades AS x ON x.id = x.id");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
}

TEST(ParserJoinTest, AnAliasCollidingWithAnUnaliasedTableNameIsRefused) {
    // `t` the table and `t` the alias of `u` are one binding. Catching
    // this needs the comparison to be over bindings rather than over
    // aliases and table names separately.
    auto parsed = Parse("SELECT * FROM t JOIN u AS t ON t.id = t.id");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
}

TEST(ParserJoinTest, BindingComparisonIsCaseInsensitive) {
    // Names are compared case-insensitively everywhere else in the engine,
    // so `T` and `t` are one binding here too. Getting this wrong would
    // let an ambiguous statement through, which is the failure that
    // matters - the other direction only costs an alias.
    auto parsed = Parse("SELECT * FROM t JOIN T ON t.id = T.id");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
}

// ---- Outer joins: reserved, refused, positioned ---------------------------

TEST(ParserJoinTest, OuterJoinKeywordsAreUnsupportedWithTheirOwnPosition) {
    struct Case {
        const char* sql;
        std::size_t keyword_at;
    };
    const Case cases[] = {
        //                     0123456789012345678
        {"SELECT * FROM t LEFT JOIN u ON t.id = u.id", 16},
        {"SELECT * FROM t RIGHT JOIN u ON t.id = u.id", 16},
        {"SELECT * FROM t FULL JOIN u ON t.id = u.id", 16},
        {"SELECT * FROM t OUTER JOIN u ON t.id = u.id", 16},
    };
    for (const Case& c : cases) {
        auto parsed = Parse(c.sql);
        ASSERT_FALSE(parsed.ok()) << c.sql;
        EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported) << c.sql;
        // The position is the keyword's own, not "wherever the parser
        // gave up" - which is the whole reason these words are reserved
        // before they are implementable (spec I9).
        EXPECT_NE(parsed.status().message().find("byte " + std::to_string(c.keyword_at)),
                  std::string::npos)
            << c.sql << ": " << parsed.status().message();
    }
}

TEST(ParserJoinTest, InnerIsNotASynonymAndIsNotSilentlyAccepted) {
    // Spec I9 reserves only the outer keywords. `INNER JOIN` is therefore
    // trailing garbage after `FROM t`, and rejected - which is a truthful
    // answer, not an oversight: accepting it would be a grammar addition
    // no task has asked for.
    auto parsed = Parse("SELECT * FROM t INNER JOIN u ON t.id = u.id");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument);
}

// ---- Malformed joins ------------------------------------------------------

TEST(ParserJoinTest, AJoinWithoutOnIsRejected) {
    // A JOIN with no ON is a cross join, and this grammar has no cross
    // join to mean - so it is an error rather than a silent product.
    auto parsed = Parse("SELECT * FROM t JOIN u");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(parsed.status().message().find("ON"), std::string::npos)
        << parsed.status().message();
}

TEST(ParserJoinTest, AnUnqualifiedOnColumnIsUnsupportedWithItsPosition) {
    //                 0123456789012345678901234567890
    auto parsed = Parse("SELECT * FROM t JOIN u ON id = u.id");
    ASSERT_FALSE(parsed.ok());
    // Not InvalidArgument: `ON id = u.id` is well-formed SQL whose
    // resolution this parser does not do. When V14's compiler resolves
    // unqualified names against the FROM list this can loosen; until then
    // refusing beats guessing which relation owns `id`.
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_NE(parsed.status().message().find("byte 26"), std::string::npos)
        << parsed.status().message();
}

TEST(ParserJoinTest, AnIncompleteJoinPredicateIsRejected) {
    for (auto sql : {"SELECT * FROM t JOIN u ON t.id", "SELECT * FROM t JOIN u ON t.id =",
                     "SELECT * FROM t JOIN u ON t. = u.id", "SELECT * FROM t JOIN ON t.id = u.id"}) {
        auto parsed = Parse(sql);
        EXPECT_FALSE(parsed.ok()) << sql;
    }
}

TEST(ParserJoinTest, AnAliasMustBeAnIdentifier) {
    for (auto sql : {"SELECT * FROM t AS", "SELECT * FROM t AS 42", "SELECT * FROM t AS *"}) {
        auto parsed = Parse(sql);
        EXPECT_FALSE(parsed.ok()) << sql;
        EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument) << sql;
    }
}

TEST(ParserJoinTest, AWhereClauseStillParsesAfterAJoin) {
    // What matters here is that the join loop stops at WHERE instead of
    // consuming it. V06 made `SELECT *` illegal over two relations, so
    // the statement names its columns.
    auto parsed = Parse("SELECT t.id FROM t JOIN u ON t.id = u.id WHERE t.id = 1");
    const SelectStmt& sel = MustSelect(parsed);
    ASSERT_EQ(sel.joins.size(), 1u);
    ASSERT_EQ(sel.where.size(), 1u);
    EXPECT_EQ(sel.where[0].col.qualifier, "t");
    EXPECT_EQ(sel.where[0].col.name, "id");
}

TEST(ParserJoinTest, JoinSyntaxIsCaseInsensitive) {
    auto parsed = Parse("select a.id from t as a join u as b on a.id = b.id");
    const SelectStmt& sel = MustSelect(parsed);
    ASSERT_EQ(sel.joins.size(), 1u);
    EXPECT_EQ(sel.from.binding(), "a");
    EXPECT_EQ(sel.joins[0].relation.binding(), "b");
}

}  // namespace
}  // namespace kds::parser
