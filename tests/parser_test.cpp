#include "kds/parser/parser.hpp"

#include <cstddef>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace kds::parser {
namespace {

TEST(ParserTest, CreateTableDefaultsToBtree) {
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
    // No storage word: a relation whose first column is its tuple key.
    EXPECT_FALSE(ct->heap);
}

TEST(ParserTest, CreateTableExplicitBtree) {
    auto stmt = Parse("CREATE TABLE t (id int64) BTREE");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();
    auto* ct = std::get_if<CreateTableStmt>(&stmt.value());
    ASSERT_NE(ct, nullptr);
    EXPECT_FALSE(ct->heap);
}

// BZ-R1: `HEAP` asks for the engine's hidden tuple key, and SUS-1's refusal
// is gone - the first declared column is an ordinary one, of any type.
TEST(ParserTest, CreateTableHeapAsksForAHiddenTupleKey) {
    auto stmt = Parse("CREATE TABLE t (name varchar, n int64) HEAP");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();
    auto* ct = std::get_if<CreateTableStmt>(&stmt.value());
    ASSERT_NE(ct, nullptr);
    EXPECT_TRUE(ct->heap);
    ASSERT_EQ(ct->columns.size(), 2u);
    EXPECT_EQ(ct->columns[0].name, "name");
}

// ---- The key-mode word (PK03, docs/spec/heap-and-tuple.md §4.1) ---------------

TEST(ParserTest, CreateTableDefaultsToAssigned) {
    // A statement that names no mode means what every statement written
    // before the amendment meant, and that is what makes this additive.
    auto stmt = Parse("CREATE TABLE t (id int64)");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();
    auto* ct = std::get_if<CreateTableStmt>(&stmt.value());
    ASSERT_NE(ct, nullptr);
    EXPECT_FALSE(ct->heap);
}

TEST(ParserTest, CreateTableStillTakesEXPLICITInEitherOrderAndItDoesNothing) {
    // The word outlived the mode it selected (docs/spec/heap-and-tuple.md §4.1).
    // Accepted so written SQL keeps working, and it says nothing false -
    // every relation takes a caller-supplied key now. The only thing to
    // assert is that it changes no field, storage least of all.
    for (std::string_view sql : {"CREATE TABLE t (id int64) BTREE EXPLICIT",
                                 "CREATE TABLE t (id int64) EXPLICIT BTREE",
                                 "CREATE TABLE t (id int64) explicit btree"}) {
        auto stmt = Parse(sql);
        ASSERT_TRUE(stmt.ok()) << sql << ": " << stmt.status().message();
        auto* ct = std::get_if<CreateTableStmt>(&stmt.value());
        ASSERT_NE(ct, nullptr) << sql;
        EXPECT_FALSE(ct->heap) << sql;
    }

    // Bare EXPLICIT does not move storage either - the old grammar's
    // explicit-implies-btree resolution went with the refusal it fed.
    auto bare = Parse("CREATE TABLE t (id int64) EXPLICIT");
    ASSERT_TRUE(bare.ok()) << bare.status().message();
    auto* bare_ct = std::get_if<CreateTableStmt>(&bare.value());
    ASSERT_NE(bare_ct, nullptr);
    EXPECT_FALSE(bare_ct->heap);
}

TEST(ParserTest, CreateTableRefusesASSIGNEDWithItsByte) {
    // Not ignored, which would be accepting a spelling and enforcing
    // something else: the word means "supplying a tuple key is refused", and on the
    // relation this statement makes, supplying one is admitted.
    auto stmt = Parse("CREATE TABLE t (id int64) HEAP ASSIGNED");
    ASSERT_FALSE(stmt.ok());
    EXPECT_EQ(stmt.status().code(), StatusCode::kUnsupported);
    EXPECT_NE(stmt.status().message().find("no longer exists"), std::string::npos)
        << stmt.status().message();
    EXPECT_NE(stmt.status().message().find(
                  "byte " + std::to_string(std::string_view("CREATE TABLE t (id int64) HEAP ")
                                                .size())),
              std::string::npos)
        << stmt.status().message();
}

TEST(ParserTest, CreateTableRefusesARepeatedWordInEitherCategory) {
    struct Case {
        std::string_view sql;
        std::size_t offending_byte;
    };
    // `ASSIGNED EXPLICIT` is no longer one of these: the first word is
    // refused outright, before there is a second to be a repeat of.
    for (const Case& c : {Case{"CREATE TABLE t (id int64) HEAP BTREE", 31},
                          Case{"CREATE TABLE t (id int64) BTREE BTREE", 32},
                          Case{"CREATE TABLE t (id int64) EXPLICIT EXPLICIT", 35}}) {
        auto stmt = Parse(c.sql);
        ASSERT_FALSE(stmt.ok()) << c.sql;
        EXPECT_EQ(stmt.status().code(), StatusCode::kInvalidArgument) << c.sql;
        // The byte names the *second* word, which is the one that could not
        // be honored - pointing at the first would blame the word that was
        // read.
        EXPECT_NE(stmt.status().message().find("byte " + std::to_string(c.offending_byte)),
                  std::string::npos)
            << c.sql << ": " << stmt.status().message();
    }
}

TEST(ParserTest, CreateTableLeavesAnUnknownTrailingWordToTheGarbageCheck) {
    // The loop must stop at a word it does not know rather than consuming
    // it, so the pre-existing refusal is what an unknown word still gets -
    // before this task and after it.
    const std::string bare_sql = "CREATE TABLE t (id int64) FOO";
    auto bare = Parse(bare_sql);
    ASSERT_FALSE(bare.ok());
    EXPECT_EQ(bare.status().code(), StatusCode::kInvalidArgument);

    const std::string both_sql = "CREATE TABLE t (id int64) BTREE EXPLICIT FOO";
    auto after_both = Parse(both_sql);
    ASSERT_FALSE(after_both.ok());
    EXPECT_EQ(after_both.status().code(), StatusCode::kInvalidArgument);
    // Word for word the same refusal but for its byte: the two known words
    // are consumed and the unknown one lands where it always did.
    const auto garbage = [](const std::string& sql) {
        return "unexpected token 'FOO' after end of statement (byte " +
               std::to_string(sql.find("FOO")) + ")";
    };
    EXPECT_EQ(bare.status().message(), garbage(bare_sql));
    EXPECT_EQ(after_both.status().message(), garbage(both_sql));
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
    ASSERT_EQ(ins->rows[0].size(), 4u);
    EXPECT_EQ(ins->rows[0][0].type, ValueType::kInt);
    EXPECT_EQ(ins->rows[0][0].int_val, 1);
    EXPECT_EQ(ins->rows[0][1].type, ValueType::kStr);
    EXPECT_EQ(ins->rows[0][1].str_val, "alice");
    EXPECT_EQ(ins->rows[0][2].type, ValueType::kNull);
    EXPECT_EQ(ins->rows[0][3].type, ValueType::kInt);
    EXPECT_EQ(ins->rows[0][3].int_val, -9);
}

TEST(ParserTest, ABareNumericLiteralIsTheQuotedStringOfItsSpelling) {
    // TY3 phase 2: `12.34` produces exactly the AstValue `'12.34'` would -
    // kStr, spelling preserved - so every stage past the parser has one
    // case, and the column's type gives it meaning at the usual gates.
    auto stmt = Parse("INSERT INTO t VALUES (12.34, -0.5)");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();
    auto* ins = std::get_if<InsertStmt>(&stmt.value());
    ASSERT_NE(ins, nullptr);
    ASSERT_EQ(ins->rows[0].size(), 2u);
    EXPECT_EQ(ins->rows[0][0].type, ValueType::kStr);
    EXPECT_EQ(ins->rows[0][0].str_val, "12.34");
    EXPECT_EQ(ins->rows[0][1].type, ValueType::kStr);
    EXPECT_EQ(ins->rows[0][1].str_val, "-0.5");

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
    EXPECT_EQ(ins->rows[0][0].raw_int_text, "18446744073709551615");
}

TEST(ParserTest, InsertRequiresAtLeastOneValue) {
    auto stmt = Parse("INSERT INTO t VALUES ()");
    EXPECT_FALSE(stmt.ok());
}

// BW-S6 (BW-Q12.1 of `instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`):
// a column list, each name with its byte, and the list's `(` byte. A
// positional statement has none.
TEST(ParserTest, InsertTakesAColumnListWithItsBytes) {
    //                 0         1         2         3
    //                 0123456789012345678901234567890123456
    auto stmt = Parse("INSERT INTO t (b, id) VALUES (1, 2), (3, 4)");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();
    auto* ins = std::get_if<InsertStmt>(&stmt.value());
    ASSERT_NE(ins, nullptr);
    EXPECT_EQ(ins->columns_byte_offset, 14u);
    ASSERT_EQ(ins->columns.size(), 2u);
    EXPECT_EQ(ins->columns[0].name, "b");
    EXPECT_EQ(ins->columns[0].byte_offset, 15u);
    EXPECT_EQ(ins->columns[1].name, "id");
    EXPECT_EQ(ins->columns[1].byte_offset, 18u);
    ASSERT_EQ(ins->rows.size(), 2u);
    EXPECT_EQ(ins->rows[1][0].int_val, 3);

    auto positional = Parse("INSERT INTO t VALUES (1)");
    ASSERT_TRUE(positional.ok());
    EXPECT_TRUE(std::get<InsertStmt>(positional.value()).columns.empty());
}

// BW-Q12.2, BW-Q12.5, BW-Q12.11, BW-Q12.12 step 1: every parse refusal of the
// list is `InvalidArgument` at its token's byte.
TEST(ParserTest, AColumnListsParseRefusalsNameTheirByte) {
    struct Case {
        const char* sql;
        const char* at;
    };
    const Case cases[] = {
        {"INSERT INTO t () VALUES (1)", "at byte 15"},       // empty list: the `)`
        {"INSERT INTO t (1) VALUES (1)", "at byte 15"},      // not an identifier
        {"INSERT INTO t (?) VALUES (1)", "at byte 15"},      // a parameter is no name
        {"INSERT INTO t (a,) VALUES (1)", "at byte 17"},     // a trailing comma
        {"INSERT INTO t (a, b VALUES (1, 2)", "at byte 20"},  // the missing `)`
        {"INSERT INTO t (a, b) (1, 2)", "at byte 21"},       // the missing VALUES
    };
    for (const Case& c : cases) {
        auto stmt = Parse(c.sql);
        ASSERT_FALSE(stmt.ok()) << c.sql;
        EXPECT_EQ(stmt.status().code(), StatusCode::kInvalidArgument) << c.sql;
        EXPECT_NE(stmt.status().message().find(c.at), std::string::npos)
            << c.sql << " -> " << stmt.status().message();
    }
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

// K-M3's parser half, and it is a *negative* obligation: assigning the
// tuple key must reach the compiler, because which column is the tuple key is
// catalog knowledge and the parser has no catalog. A parser that guessed
// - by the name `id`, say - would refuse a legal statement on a relation
// whose second column happens to be called that.
TEST(ParserTest, AssigningTheTupleKeyParses) {
    auto stmt = Parse("UPDATE accounts SET id = 99");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();

    auto* upd = std::get_if<UpdateStmt>(&stmt.value());
    ASSERT_NE(upd, nullptr);
    ASSERT_EQ(upd->assignments.size(), 1u);
    EXPECT_EQ(upd->assignments[0].col_name, "id");
}

// The byte the compiler's refusal reports comes from here, so it is pinned
// here: the column's own first byte, not the token after it.
TEST(ParserTest, EachAssignmentRecordsItsColumnsByteOffset) {
    const std::string sql = "UPDATE t SET a = 1, bb = 2";
    auto stmt = Parse(sql);
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();

    auto* upd = std::get_if<UpdateStmt>(&stmt.value());
    ASSERT_NE(upd, nullptr);
    ASSERT_EQ(upd->assignments.size(), 2u);
    EXPECT_EQ(upd->assignments[0].byte_offset, sql.find("a = 1"));
    EXPECT_EQ(upd->assignments[1].byte_offset, sql.find("bb = 2"));
}

// An UPDATE's or DELETE's WHERE is the native grammar's alone (parser-v2.md
// I10), with no query layer behind it: a form only the query grammar reads
// is declined `NotImplemented`, naming the byte the native parse stopped at
// (anti-kdbs QA at 4f175ba9: these were byte-less `InvalidArgument`s).
TEST(ParserTest, AnExpressionInADmlWhereIsNotImplementedAtItsByte) {
    struct Case {
        std::string sql;
        std::string_view at;  // the token the native parse stopped at
    };
    const Case cases[] = {
        {"UPDATE t SET n = 10 WHERE n + 1 = 3", "+ 1"},
        {"DELETE FROM t WHERE n + 1 = 3", "+ 1"},
        {"UPDATE t SET n = 10 WHERE 1 = 0", "0"},
        {"DELETE FROM t WHERE 1 = 0;", "0;"},
        {"UPDATE t SET n = 10 WHERE n = 1 + 1 RETURNING id", "+ 1"},
        {"UPDATE t SET n = 10 FROM u WHERE t.n + 1 = u.m", "+ 1"},
        {"DELETE FROM t WHERE id IN (SELECT id FROM t WHERE n * 2 = 4)", "* 2"},
        // A subquery's aggregate is its own, not the WHERE's.
        {"DELETE FROM t WHERE n + 0 = (SELECT MAX(n) FROM t)", "+ 0"},
    };
    for (const Case& c : cases) {
        auto stmt = Parse(c.sql);
        ASSERT_FALSE(stmt.ok()) << c.sql;
        EXPECT_EQ(stmt.status().code(), StatusCode::kNotImplemented) << c.sql;
        EXPECT_NE(stmt.status().message().find("byte " + std::to_string(c.sql.rfind(c.at))),
                  std::string::npos)
            << c.sql << " -> " << stmt.status().message();
    }
}

// What neither grammar reads stays the native `InvalidArgument`, and a
// native `NotImplemented` keeps its own words.
TEST(ParserTest, AWrongDmlWhereKeepsItsNativeRefusal) {
    for (const char* sql : {"UPDATE t SET n = 1 WHERE n = = 3", "DELETE FROM t WHERE n +",
                            "UPDATE t SET n = 1 WHERE id = ?", "DELETE FROM t WHERE n = 1 FOO",
                            // An aggregate in a WHERE is wrong, not declined.
                            "DELETE FROM t WHERE SUM(n) = 1",
                            "UPDATE t SET n = 1 WHERE n = 1 OR COUNT(*) > 0"}) {
        auto stmt = Parse(sql);
        ASSERT_FALSE(stmt.ok()) << sql;
        EXPECT_EQ(stmt.status().code(), StatusCode::kInvalidArgument) << sql;
        EXPECT_NE(stmt.status().message().find("byte "), std::string::npos)
            << sql << " -> " << stmt.status().message();
    }
    // The refusal is about the WHERE: an error before it is never re-read.
    auto set = Parse("UPDATE t SET n = WHERE n + 1 = 3");
    ASSERT_FALSE(set.ok());
    EXPECT_EQ(set.status().code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(Parse("UPDATE t SET n = 1 WHERE UPPER('a') = 'A'").status().code(),
              StatusCode::kNotImplemented);
    EXPECT_EQ(Parse("UPDATE t SET n = 1 WHERE UPPER('a') = 'A'").status().message().find(
                  "an expression in"),
              std::string::npos);
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
    // `DROP TABLE` parses since DT01, so the probe is a head the grammar
    // has never heard of - the case this test was always about.
    auto stmt = Parse("TRUNCATE t");
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

// ---- Nullability (docs/spec/null.md §2.3, D1) ------------------------------

TEST(ParserTest, AColumnIsNotNullUnlessDeclaredNull) {
    auto stmt = Parse("CREATE TABLE t (id int64, a int64 NULL, b int64 NOT NULL, c varchar)");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();
    auto* ct = std::get_if<CreateTableStmt>(&stmt.value());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->columns.size(), 4u);
    EXPECT_TRUE(ct->columns[0].notnull) << "saying nothing means NOT NULL (D1)";
    EXPECT_FALSE(ct->columns[1].notnull);
    EXPECT_GT(ct->columns[1].null_byte_offset, 0u);
    EXPECT_TRUE(ct->columns[2].notnull) << "NOT NULL spells the default and parses";
    EXPECT_TRUE(ct->columns[3].notnull);
}

TEST(ParserTest, NullabilityPrecedesTheOtherColumnSuffixes) {
    auto stmt = Parse("CREATE TABLE t (id int64, p int64 NULL REFERENCES parent)");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();
    auto* ct = std::get_if<CreateTableStmt>(&stmt.value());
    ASSERT_NE(ct, nullptr);
    EXPECT_FALSE(ct->columns[1].notnull);
    EXPECT_EQ(ct->columns[1].references_table, "parent");
}

TEST(ParserTest, NotWithoutNullInAColumnIsRefusedWithTheByte) {
    auto stmt = Parse("CREATE TABLE t (id int64, a int64 NOT)");
    ASSERT_FALSE(stmt.ok());
    EXPECT_NE(stmt.status().message().find("byte"), std::string::npos);
}

}  // namespace
}  // namespace kds::parser
