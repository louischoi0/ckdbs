#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <gtest/gtest.h>

#include "kds/parser/fingerprint.hpp"
#include "kds/parser/parser.hpp"

// `CREATE ASSERTION` / `DROP ASSERTION` — the grammar (docs/spec/assertion.md
// §3, workplan AST02).
//
// Three things this file is really about, and none of them is "does the
// parser accept the happy path".
//
// **The grammar is the supported predicate class** (AS2). SQL-92's
// `CREATE ASSERTION` takes a free-form search condition, and that generality
// is one of the two reasons no major DBMS ships the statement. Here the shape
// is four facts — a relation, a group-column list, one of two aggregates, an
// integer upper bound — so *every* form outside the class is refused by the
// parser with the byte that caused it, rather than accepted and later found
// unenforceable. The refusal tests below are the specification; the accepting
// ones only check the four facts survive.
//
// **A reserved form answers `Unsupported`, not a syntax error.** `>`, `>=`,
// `DEFERRABLE`, `NOT VALID`, `MIN`/`MAX`/`AVG`, `COUNT(<column>)` and
// `DISTINCT` all parse. Each is a form this engine understands and declines,
// so its answer names the decision (AS11, AS3, AS7, §10) at its own position.
// That is also what keeps the grammar from shifting on the day one of them
// lands.
//
// **Nothing is reserved.** `ASSERTION`, `CHECK` and `GROUP` reach the lexer as
// ordinary identifiers — matched by text where the grammar expects them, like
// `COVERING` and `DISTINCT` before them — so a relation or a column may still
// be named any of them, no token sequence lexes differently than it did, and
// `kFingerprintVersion` does not move. The golden corpus
// (tests/testdata/parser_corpus.txt) is the evidence that no DML fingerprint
// moved; this file covers the other half, that the words are still usable as
// names.

namespace kds::parser {
namespace {

StatusOr<Statement> ParseSql(std::string_view sql) {
    Parser parser(sql);
    return parser.Parse();
}

const AssertionStmt& MustAssertion(const StatusOr<Statement>& parsed) {
    EXPECT_TRUE(parsed.ok()) << parsed.status().message();
    return std::get<AssertionStmt>(parsed.value());
}

// Every refusal is checked as (code, offset-bearing message), because a
// position is half of what makes these errors worth anything: the whole
// argument for reserving a form rather than leaving it a syntax error is that
// the client is told *which byte* they wrote that this engine declines.
void ExpectRefusal(std::string_view sql, StatusCode code, std::string_view mentions) {
    auto parsed = ParseSql(sql);
    ASSERT_FALSE(parsed.ok()) << sql;
    EXPECT_EQ(parsed.status().code(), code) << sql << " -> " << parsed.status().message();
    EXPECT_NE(parsed.status().message().find(std::string(mentions)), std::string::npos)
        << sql << " -> " << parsed.status().message();
}

// ---- The accepted forms -------------------------------------------------

TEST(AssertionDdlTest, TheSpecsOwnExampleParsesIntoItsFourFacts) {
    // docs/spec/assertion.md §3.2, verbatim.
    auto parsed = ParseSql(
        "CREATE ASSERTION user_product_purchase_limit ON purchases "
        "GROUP BY (user_id, product_id) CHECK COUNT(*) <= 5");
    const AssertionStmt& stmt = MustAssertion(parsed);

    EXPECT_FALSE(stmt.drop);
    EXPECT_EQ(stmt.name, "user_product_purchase_limit");
    EXPECT_EQ(stmt.table_name, "purchases");
    ASSERT_EQ(stmt.group_columns.size(), 2u);
    EXPECT_EQ(stmt.group_columns[0].name, "user_id");
    EXPECT_EQ(stmt.group_columns[1].name, "product_id");
    EXPECT_EQ(stmt.func, AggFunc::kCount);
    EXPECT_TRUE(stmt.sum_column.name.empty());
    EXPECT_EQ(stmt.op, CompareOp::kLte);
    EXPECT_EQ(stmt.bound, 5);
}

TEST(AssertionDdlTest, ASumAssertionCarriesTheColumnItSums) {
    auto parsed = ParseSql("CREATE ASSERTION credit ON orders GROUP BY (user_id) "
                            "CHECK SUM(amount) <= 1000");
    const AssertionStmt& stmt = MustAssertion(parsed);

    EXPECT_EQ(stmt.func, AggFunc::kSum);
    EXPECT_EQ(stmt.sum_column.name, "amount");
    ASSERT_EQ(stmt.group_columns.size(), 1u);
    EXPECT_EQ(stmt.group_columns[0].name, "user_id");
    EXPECT_EQ(stmt.bound, 1000);
}

// The one piece of arithmetic in the grammar, and it is here rather than in
// two later stages: the enforced invariant is `aggregate <= N` for both
// accepted operators, so `<` is not one more case for the write path to be
// right about. Getting this wrong by one is the difference between a bound of
// 5 admitting five rows and admitting four.
//
// Both mappings are exact - neither reinterprets what was written. That is
// the property `=` could not have, and why AS11 was revised to refuse it
// rather than fold it in here as a third row of this table.
TEST(AssertionDdlTest, EveryOperatorReducesToOneEnforcedCeiling) {
    struct Case {
        const char* sql;
        CompareOp op;
        std::int64_t enforced;
    };
    const Case cases[] = {
        {"CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) <= 5", CompareOp::kLte, 5},
        {"CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) < 5", CompareOp::kLt, 4},
    };
    for (const Case& c : cases) {
        auto parsed = ParseSql(c.sql);
        const AssertionStmt& stmt = MustAssertion(parsed);
        EXPECT_EQ(stmt.op, c.op) << c.sql;
        EXPECT_EQ(stmt.enforced_max(), c.enforced) << c.sql;
    }
}

TEST(AssertionDdlTest, TheDeclarationIsKeptVerbatimForTheCatalog) {
    // AS10 stores the declaration's text and no per-column table beside it,
    // which is also what makes an uncapped GROUP BY list storable in a
    // fixed-width catalog row. So the text has to be the operator's, not a
    // rebuild from the AST that could drift from what they wrote.
    const std::string sql =
        "create  assertion Lim on T group by ( a , b ) check count(*) <= 3 ;";
    auto parsed = ParseSql(sql);
    const AssertionStmt& stmt = MustAssertion(parsed);

    EXPECT_EQ(stmt.source_text, "create  assertion Lim on T group by ( a , b ) check count(*) <= 3");
    // Case is preserved in the text and folded nowhere else: the names are
    // stored as written, exactly as every other declaration keeps them.
    EXPECT_EQ(stmt.name, "Lim");
    EXPECT_EQ(stmt.table_name, "T");
}

TEST(AssertionDdlTest, TheGrammarIsCaseInsensitiveAndTakesATrailingSemicolon) {
    for (std::string_view sql : {
             "CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) <= 1",
             "create assertion a on t group by (x) check count(*) <= 1",
             "CrEaTe AsSeRtIoN a On t GrOuP bY (x) ChEcK cOuNt(*) <= 1",
             "CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) <= 1;",
         }) {
        auto parsed = ParseSql(sql);
        const AssertionStmt& stmt = MustAssertion(parsed);
        EXPECT_EQ(stmt.name, "a") << sql;
        EXPECT_EQ(stmt.bound, 1) << sql;
    }
}

TEST(AssertionDdlTest, DropNamesTheAssertionAndNothingElse) {
    auto parsed = ParseSql("DROP ASSERTION user_product_purchase_limit");
    const AssertionStmt& stmt = MustAssertion(parsed);

    EXPECT_TRUE(stmt.drop);
    EXPECT_EQ(stmt.name, "user_product_purchase_limit");
    // An assertion's name is unique instance-wide, so DROP naming its
    // relation again would be a second identity to keep in step with the
    // first - IndexStmt's rule, and the reason the CREATE-only fields stay
    // empty here rather than being filled in with something plausible.
    EXPECT_TRUE(stmt.table_name.empty());
    EXPECT_TRUE(stmt.group_columns.empty());
}

TEST(AssertionDdlTest, TheGroupByListHasNoCapAndAcceptsALongOne) {
    // §3 declares no ceiling on the list, and this task deliberately invents
    // none: the catalog stores `source_text`, so a long list costs text and
    // not a widened row. Eleven columns is past every `[PROPOSED]` cap in the
    // engine, which is the point - none of them applies here.
    auto parsed = ParseSql(
        "CREATE ASSERTION a ON t GROUP BY (c0, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10) "
        "CHECK COUNT(*) <= 1");
    const AssertionStmt& stmt = MustAssertion(parsed);
    ASSERT_EQ(stmt.group_columns.size(), 11u);
    EXPECT_EQ(stmt.group_columns[10].name, "c10");
}

TEST(AssertionDdlTest, EveryColumnCarriesWhereItWasWritten) {
    // The offsets are what let the *catalog's* errors carry a position: this
    // is the only layer that knows where a name was written, and "relation t
    // has no column q" is AST03's answer, not this one's.
    const std::string sql = "CREATE ASSERTION a ON t GROUP BY (x, y) CHECK SUM(amt) <= 1";
    auto parsed = ParseSql(sql);
    const AssertionStmt& stmt = MustAssertion(parsed);

    EXPECT_EQ(sql.substr(stmt.byte_offset, 1), "a");
    EXPECT_EQ(sql.substr(stmt.table_byte_offset, 1), "t");
    ASSERT_EQ(stmt.group_columns.size(), 2u);
    EXPECT_EQ(sql.substr(stmt.group_columns[0].byte_offset, 1), "x");
    EXPECT_EQ(sql.substr(stmt.group_columns[1].byte_offset, 1), "y");
    EXPECT_EQ(sql.substr(stmt.sum_column.byte_offset, 3), "amt");
    EXPECT_EQ(sql.substr(stmt.bound_byte_offset, 1), "1");
}

// ---- Nothing is reserved -----------------------------------------------

TEST(AssertionDdlTest, TheNewWordsAreStillUsableAsNames) {
    // The whole reason `ASSERTION` and `CHECK` are matched by text rather
    // than lexed as keywords: a statement that parsed before this task still
    // parses, and hashes identically, because no token sequence changed. A
    // word moved into the Keyword enum would be a language change, not a
    // lexer change (token.hpp).
    auto assertion_named_assertion = ParseSql(
        "CREATE ASSERTION assertion ON check GROUP BY (assertion, check) "
        "CHECK COUNT(*) <= 1");
    const AssertionStmt& stmt = MustAssertion(assertion_named_assertion);
    EXPECT_EQ(stmt.name, "assertion");
    EXPECT_EQ(stmt.table_name, "check");
    ASSERT_EQ(stmt.group_columns.size(), 2u);
    EXPECT_EQ(stmt.group_columns[1].name, "check");

    // And the DML side, which is what actually carries a `pattern_id`.
    EXPECT_TRUE(ParseSql("SELECT assertion FROM check WHERE assertion = 1").ok());
    EXPECT_TRUE(ParseSql("SELECT check, assertion FROM t GROUP BY check").ok());
    EXPECT_TRUE(ParseSql("INSERT INTO assertion VALUES (1)").ok());
    EXPECT_TRUE(ParseSql("UPDATE check SET assertion = 1 WHERE id = 1").ok());
}

TEST(AssertionDdlTest, AssertionDdlIsNotFingerprintedAndTheVersionDidNotMove) {
    // Assertion DDL carries no pattern: its leading word is CREATE, so the
    // accumulator answers nothing, exactly as `CREATE INDEX` and `CREATE
    // TABLE` do. A `pattern_id` for a statement that is executed once and
    // never repeated would be an entry nothing could ever match.
    Parser parser("CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) <= 1");
    EXPECT_TRUE(parser.Parse().ok());
    EXPECT_FALSE(parser.fingerprint().has_value());

    // The version is pinned here as well as by the corpus, because this is
    // the assertion the workplan asks for in as many words: DML fingerprints
    // are unaffected by this change.
    EXPECT_EQ(kFingerprintVersion, 1u);
}

// ---- Reserved and refused (AS11, AS3, AS7, §10) -------------------------

// **AS11 as revised 2026-08-08.** `=` parsed and was documented as meaning
// `aggregate <= N`, for syntactic familiarity. It is refused now, and the
// reason is truthfulness rather than cost: the engine would have enforced
// something other than what the operator wrote. Enforcing real equality means
// enforcing the lower-bound half, which is the DELETE and decreasing-UPDATE
// write path v1 excludes - so `=` costs exactly what `>=` costs.
TEST(AssertionDdlTest, EqualityIsUnsupportedBecauseReadingItAsAnUpperBoundWouldLie) {
    ExpectRefusal("CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) = 5",
                  StatusCode::kUnsupported, "equality assertions (=)");
    ExpectRefusal("CREATE ASSERTION a ON t GROUP BY (x) CHECK SUM(v) = 100",
                  StatusCode::kUnsupported, "enforcing a lower bound");

    // Refused at the operator, before the bound is even read - so the
    // degenerate-predicate check below never sees an `=`, and `= 0` is now
    // answered by this rule rather than by that one.
    ExpectRefusal("CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) = 0",
                  StatusCode::kUnsupported, "equality assertions (=)");
}

TEST(AssertionDdlTest, ALowerBoundIsUnsupportedAtItsOwnOperator) {
    // AS11, and the one refusal that pays for a whole write path: a lower
    // bound has to be re-checked on DELETE and on every decreasing UPDATE,
    // which is exactly why v1 can leave DELETE uninstrumented (§4.2).
    ExpectRefusal("CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) > 5",
                  StatusCode::kUnsupported, "lower-bound");
    ExpectRefusal("CREATE ASSERTION a ON t GROUP BY (x) CHECK SUM(v) >= 5",
                  StatusCode::kUnsupported, "lower-bound");
    // The byte is the operator's, not the statement's.
    auto parsed = ParseSql("CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) > 5");
    ASSERT_FALSE(parsed.ok());
    EXPECT_NE(parsed.status().message().find("byte 52"), std::string::npos)
        << parsed.status().message();
}

TEST(AssertionDdlTest, InequalityIsNotABoundAtAll) {
    // Distinct from `>` on purpose. `>` is reserved grammar - a form that
    // could be enforced and is not, so it says which decision it waits on.
    // `!=` names no ceiling in any direction, so it is simply wrong.
    ExpectRefusal("CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) != 5",
                  StatusCode::kInvalidArgument, "not a bound");
    // And the message names only what v1 takes, which is now two operators.
    auto parsed = ParseSql("CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) != 5");
    ASSERT_FALSE(parsed.ok());
    EXPECT_NE(parsed.status().message().find("is < or <="), std::string::npos)
        << parsed.status().message();
}

TEST(AssertionDdlTest, ConstraintTimingClausesAreReservedAndRefused) {
    // AS3 and AS7. `NOT DEFERRABLE` is refused too, even though it names the
    // behaviour v1 actually has: accepting it would turn it into a promise,
    // and the decision reserves the whole timing clause rather than half of
    // it.
    for (std::string_view sql : {
             "CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) <= 1 DEFERRABLE",
             "CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) <= 1 NOT DEFERRABLE",
             "CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) <= 1 NOT VALID",
             "CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) <= 1 INITIALLY DEFERRED",
         }) {
        auto parsed = ParseSql(sql);
        ASSERT_FALSE(parsed.ok()) << sql;
        EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported) << sql;
        EXPECT_NE(parsed.status().message().find("statement time"), std::string::npos)
            << sql << " -> " << parsed.status().message();
    }
}

TEST(AssertionDdlTest, TheThreeUnmaintainableAggregatesAreRefusedByName) {
    // §10: MIN and MAX are not incrementally maintainable under deletion
    // without extra structure, and AVG is not a bound. Refused by name so the
    // answer says which decision it is waiting on, rather than "expected
    // COUNT or SUM".
    for (std::string_view sql : {
             "CREATE ASSERTION a ON t GROUP BY (x) CHECK MIN(v) <= 1",
             "CREATE ASSERTION a ON t GROUP BY (x) CHECK MAX(v) <= 1",
             "CREATE ASSERTION a ON t GROUP BY (x) CHECK AVG(v) <= 1",
         }) {
        ExpectRefusal(sql, StatusCode::kUnsupported, "out of scope for assertions");
    }
    // A word that is no aggregate at all is a different answer: there is no
    // decision to wait on.
    ExpectRefusal("CREATE ASSERTION a ON t GROUP BY (x) CHECK TOTAL(v) <= 1",
                  StatusCode::kInvalidArgument, "is not an aggregate");
}

TEST(AssertionDdlTest, CountTakesStarAndSumTakesAColumnAndNeitherIsCoerced) {
    // `COUNT(<column>)` counts non-NULLs, which is a *different* aggregate;
    // reading it as `COUNT(*)` would enforce a bound the operator did not
    // write, so it is refused rather than folded.
    ExpectRefusal("CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(v) <= 1",
                  StatusCode::kUnsupported, "COUNT(*)");
    ExpectRefusal("CREATE ASSERTION a ON t GROUP BY (x) CHECK SUM(*) <= 1",
                  StatusCode::kInvalidArgument, "SUM takes a column");
    ExpectRefusal("CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(DISTINCT v) <= 1",
                  StatusCode::kUnsupported, "DISTINCT");
}

// ---- Wrong, rather than reserved ---------------------------------------

TEST(AssertionDdlTest, TheBoundIsANonNegativeIntegerLiteral) {
    // §3.1: literals only, no expressions (TY3 conservatism), and
    // non-negative. One test because one predicate answers both, which is why
    // the message names both halves.
    for (std::string_view sql : {
             "CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) <= -1",
             "CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) <= 'five'",
             "CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) <= 1.5",
             "CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) <= n",
             "CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) <=",
         }) {
        ExpectRefusal(sql, StatusCode::kInvalidArgument, "non-negative integer literal");
    }
}

TEST(AssertionDdlTest, ADegenerateCountBoundCanNeverAdmitARowAndIsRefused) {
    // §3.1's named case, and its two spellings. A group *exists* only because
    // it holds at least one row, so its count is at least 1 and any ceiling
    // below 1 declares a relation that may never be written to again.
    // `= 0`, the spelling §3.1 named, is now refused one step earlier by the
    // operator itself - see the equality test above. What is left is the two
    // spellings an accepted operator can still produce.
    for (std::string_view sql : {
             "CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) <= 0",
             "CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) < 1",
             "CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) < 0",
         }) {
        ExpectRefusal(sql, StatusCode::kInvalidArgument, "can never admit a row");
    }

    // One above each is the smallest legal bound, and is accepted.
    EXPECT_TRUE(ParseSql("CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) <= 1").ok());
    EXPECT_TRUE(ParseSql("CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) < 2").ok());
}

TEST(AssertionDdlTest, ASumBoundOfZeroIsNotDegenerateBecauseIntegersGoNegative) {
    // The asymmetry with COUNT is a proof, not an oversight. A sum has no
    // floor of 1 - an int64 column may hold negative values - so no
    // non-negative bound is provably unsatisfiable, and refusing one would be
    // inventing a restriction the spec does not state.
    for (std::string_view sql : {
             "CREATE ASSERTION a ON t GROUP BY (x) CHECK SUM(v) <= 0",
             "CREATE ASSERTION a ON t GROUP BY (x) CHECK SUM(v) < 0",
         }) {
        auto parsed = ParseSql(sql);
        EXPECT_TRUE(parsed.ok()) << sql << " -> " << parsed.status().message();
    }
}

TEST(AssertionDdlTest, EveryMandatoryClauseIsMandatory) {
    // Each of these names the clause that is missing, which is the whole
    // difference between a grammar that encodes a class and one that guesses:
    // an assertion with no grouping would be a whole-relation bound, a
    // different structure (one header, no directory) that v1 did not build,
    // so it is refused rather than silently read as a single group.
    ExpectRefusal("CREATE ASSERTION a", StatusCode::kInvalidArgument, "names the relation");
    ExpectRefusal("CREATE ASSERTION a ON t CHECK COUNT(*) <= 1", StatusCode::kInvalidArgument,
                  "requires a GROUP BY list");
    ExpectRefusal("CREATE ASSERTION a ON t GROUP BY (x)", StatusCode::kInvalidArgument,
                  "requires a CHECK clause");
    ExpectRefusal("CREATE ASSERTION a ON t GROUP BY () CHECK COUNT(*) <= 1",
                  StatusCode::kInvalidArgument, "expected identifier");
    ExpectRefusal("CREATE ASSERTION a ON t GROUP BY x CHECK COUNT(*) <= 1",
                  StatusCode::kInvalidArgument, "expected '('");
    // `ON` is a reserved keyword, so a missing name is caught by ParseIdent
    // before the relation clause is ever reached - the answer names the token
    // that is not an identifier rather than the clause it belongs to.
    ExpectRefusal("CREATE ASSERTION ON t GROUP BY (x) CHECK COUNT(*) <= 1",
                  StatusCode::kInvalidArgument, "expected identifier");
    ExpectRefusal("DROP ASSERTION", StatusCode::kInvalidArgument, "expected identifier");
    ExpectRefusal("CREATE ASSERTION a ON t GROUP BY (x) CHECK COUNT(*) <= 1 garbage",
                  StatusCode::kInvalidArgument, "after end of statement");
}

// ---- The shared column-list production ---------------------------------

TEST(AssertionDdlTest, TheIndexCapsStillRefuseAndStillNameTheirByte) {
    // The GROUP BY list reuses the index declaration's column-list
    // production, with the cap turned off. This pins that turning it off did
    // not turn it off for the index: a cap refuses and never truncates
    // (docs/spec/index.md §13), which is what keeps a truncated index from
    // being declared complete.
    ExpectRefusal("CREATE INDEX ix ON t (a, b, c, d, e)", StatusCode::kUnsupported,
                  "index key columns");
    ExpectRefusal("CREATE INDEX ix ON t (a) COVERING (b, c, d, e, f, g, h, i, j)",
                  StatusCode::kUnsupported, "covered columns");
}

}  // namespace
}  // namespace kds::parser
