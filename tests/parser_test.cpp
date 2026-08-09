#include "kds/parser/parser.hpp"

#include <gtest/gtest.h>

namespace kds::parser {
namespace {

TEST(ParserTest, CreateTableDefaultsToHeap) {
    auto stmt = Parse("CREATE TABLE accounts (id uint64, name varchar)");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();

    auto* ct = std::get_if<CreateTableStmt>(&stmt.value());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->table_name, "accounts");
    ASSERT_EQ(ct->columns.size(), 2u);
    EXPECT_EQ(ct->columns[0].name, "id");
    EXPECT_EQ(ct->columns[0].type_name, "uint64");
    EXPECT_EQ(ct->columns[1].name, "name");
    EXPECT_EQ(ct->columns[1].type_name, "varchar");
    EXPECT_EQ(ct->clustered, catalog::ClusteredType::kHeap);
}

TEST(ParserTest, CreateTableExplicitBtree) {
    auto stmt = Parse("CREATE TABLE t (id int64) BTREE");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();
    auto* ct = std::get_if<CreateTableStmt>(&stmt.value());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->clustered, catalog::ClusteredType::kBtree);
}

TEST(ParserTest, CreateTableRequiresAtLeastOneColumn) {
    auto stmt = Parse("CREATE TABLE t ()");
    EXPECT_FALSE(stmt.ok());
}

TEST(ParserTest, CreateTableIsCaseInsensitiveForKeywords) {
    auto stmt = Parse("create table t (id int64) heap");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();
}

TEST(ParserTest, InsertParsesMixedValueTypes) {
    auto stmt = Parse("INSERT INTO accounts VALUES (1, 'alice', NULL, -9)");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();

    auto* ins = std::get_if<InsertStmt>(&stmt.value());
    ASSERT_NE(ins, nullptr);
    EXPECT_EQ(ins->table_name, "accounts");
    ASSERT_EQ(ins->values.size(), 4u);
    EXPECT_EQ(ins->values[0].type, ValueType::kInt);
    EXPECT_EQ(ins->values[0].int_val, 1);
    EXPECT_EQ(ins->values[1].type, ValueType::kStr);
    EXPECT_EQ(ins->values[1].str_val, "alice");
    EXPECT_EQ(ins->values[2].type, ValueType::kNull);
    EXPECT_EQ(ins->values[3].type, ValueType::kInt);
    EXPECT_EQ(ins->values[3].int_val, -9);
}

TEST(ParserTest, ABareNumericLiteralIsTheQuotedStringOfItsSpelling) {
    // TY3 phase 2: `12.34` produces exactly the AstValue `'12.34'` would -
    // kStr, spelling preserved - so every stage past the parser has one
    // case, and the column's type gives it meaning at the usual gates.
    auto stmt = Parse("INSERT INTO t VALUES (12.34, -0.5)");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();
    auto* ins = std::get_if<InsertStmt>(&stmt.value());
    ASSERT_NE(ins, nullptr);
    ASSERT_EQ(ins->values.size(), 2u);
    EXPECT_EQ(ins->values[0].type, ValueType::kStr);
    EXPECT_EQ(ins->values[0].str_val, "12.34");
    EXPECT_EQ(ins->values[1].type, ValueType::kStr);
    EXPECT_EQ(ins->values[1].str_val, "-0.5");

    // The byte offset is the literal's own first byte - what lets a later
    // coercion failure point at what the client wrote (TY05).
    //                            0123456789012345678901234567
    auto sel = Parse("SELECT * FROM t WHERE amt = 12.34");
    ASSERT_TRUE(sel.ok()) << sel.status().message();
    auto* s = std::get_if<SelectStmt>(&sel.value());
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(s->where.size(), 1u);
    EXPECT_EQ(s->where[0].val.type, ValueType::kStr);
    EXPECT_EQ(s->where[0].val.str_val, "12.34");
    EXPECT_EQ(s->where[0].val.byte_offset, 28u);
}

TEST(ParserTest, InsertPreservesRawIntTextForLargeLiterals) {
    auto stmt = Parse("INSERT INTO t VALUES (18446744073709551615)");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();
    auto* ins = std::get_if<InsertStmt>(&stmt.value());
    ASSERT_NE(ins, nullptr);
    EXPECT_EQ(ins->values[0].raw_int_text, "18446744073709551615");
}

TEST(ParserTest, InsertRequiresAtLeastOneValue) {
    auto stmt = Parse("INSERT INTO t VALUES ()");
    EXPECT_FALSE(stmt.ok());
}

TEST(ParserTest, SelectStarNoWhere) {
    auto stmt = Parse("SELECT * FROM accounts");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();

    auto* sel = std::get_if<SelectStmt>(&stmt.value());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->from.table_name, "accounts");
    EXPECT_TRUE(sel->from.alias.empty());
    EXPECT_TRUE(sel->joins.empty());
    EXPECT_EQ(sel->relation_count(), 1u);
    EXPECT_TRUE(sel->where.empty());
}

TEST(ParserTest, SelectWithSingleWhereCondition) {
    auto stmt = Parse("SELECT * FROM accounts WHERE id = 5");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();

    auto* sel = std::get_if<SelectStmt>(&stmt.value());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->where.size(), 1u);
    EXPECT_EQ(sel->where[0].col.name, "id");
    EXPECT_EQ(sel->where[0].op, CompareOp::kEq);
    EXPECT_EQ(sel->where[0].val.int_val, 5);
}

TEST(ParserTest, SelectWithMultipleAndConditions) {
    auto stmt = Parse("SELECT * FROM t WHERE a = 1 AND b != 'x' AND c >= 3");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();

    auto* sel = std::get_if<SelectStmt>(&stmt.value());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->where.size(), 3u);
    EXPECT_EQ(sel->where[0].op, CompareOp::kEq);
    EXPECT_EQ(sel->where[1].op, CompareOp::kNeq);
    EXPECT_EQ(sel->where[1].val.str_val, "x");
    EXPECT_EQ(sel->where[2].op, CompareOp::kGte);
}

TEST(ParserTest, SelectAcceptsAnExplicitProjection) {
    // Reversed by V06: this was "only SELECT * is supported". The list
    // parses now; executing it is V17's, and the dispatcher says so
    // rather than emitting every column.
    auto stmt = Parse("SELECT id FROM t");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();

    auto* sel = std::get_if<SelectStmt>(&stmt.value());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->projection.size(), 1u);
    EXPECT_EQ(sel->projection[0].name, "id");
    EXPECT_FALSE(sel->star());
}

TEST(ParserTest, UpdateSingleAssignmentNoWhere) {
    auto stmt = Parse("UPDATE accounts SET name = 'bob'");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();

    auto* upd = std::get_if<UpdateStmt>(&stmt.value());
    ASSERT_NE(upd, nullptr);
    EXPECT_EQ(upd->table_name, "accounts");
    ASSERT_EQ(upd->assignments.size(), 1u);
    EXPECT_EQ(upd->assignments[0].col_name, "name");
    EXPECT_EQ(upd->assignments[0].val.str_val, "bob");
    EXPECT_TRUE(upd->where.empty());
}

TEST(ParserTest, UpdateMultipleAssignmentsWithWhere) {
    auto stmt = Parse("UPDATE t SET a = 1, b = 2 WHERE id = 9");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();

    auto* upd = std::get_if<UpdateStmt>(&stmt.value());
    ASSERT_NE(upd, nullptr);
    ASSERT_EQ(upd->assignments.size(), 2u);
    EXPECT_EQ(upd->assignments[0].col_name, "a");
    EXPECT_EQ(upd->assignments[1].col_name, "b");
    ASSERT_EQ(upd->where.size(), 1u);
    EXPECT_EQ(upd->where[0].col.name, "id");
}

TEST(ParserTest, TrailingSemicolonIsOptional) {
    EXPECT_TRUE(Parse("SELECT * FROM t;").ok());
    EXPECT_TRUE(Parse("SELECT * FROM t").ok());
}

TEST(ParserTest, EmptyStatementIsError) {
    auto stmt = Parse("");
    EXPECT_FALSE(stmt.ok());
    EXPECT_EQ(stmt.status().code(), StatusCode::kInvalidArgument);
}

TEST(ParserTest, BindParameterIsRejected) {
    // `?` lexes (token.hpp kParam) so that fingerprinting can see the
    // placeholder, but no production accepts it: this protocol has no BIND
    // stage to supply a value. Giving it a token type must not have made
    // it executable by accident.
    auto stmt = Parse("SELECT * FROM t WHERE id = ?");
    EXPECT_FALSE(stmt.ok());
    EXPECT_EQ(stmt.status().code(), StatusCode::kInvalidArgument);
}

TEST(ParserTest, UnknownKeywordIsError) {
    auto stmt = Parse("DROP TABLE t");
    EXPECT_FALSE(stmt.ok());
}

TEST(ParserTest, TrailingGarbageAfterValidStatementIsError) {
    auto stmt = Parse("SELECT * FROM t garbage");
    EXPECT_FALSE(stmt.ok());
}

TEST(ParserTest, MissingClosingParenIsError) {
    auto stmt = Parse("INSERT INTO t VALUES (1, 2");
    EXPECT_FALSE(stmt.ok());
}

TEST(ParserTest, StatementTypeNameMatchesVariant) {
    EXPECT_STREQ(StatementTypeName(Parse("SELECT * FROM t").value()), "SELECT");
    EXPECT_STREQ(StatementTypeName(Parse("INSERT INTO t VALUES (1)").value()), "INSERT");
    EXPECT_STREQ(StatementTypeName(Parse("UPDATE t SET a = 1").value()), "UPDATE");
    EXPECT_STREQ(StatementTypeName(Parse("CREATE TABLE t (a int64)").value()), "CREATE TABLE");
}

}  // namespace
}  // namespace kds::parser
