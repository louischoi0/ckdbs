#include "kds/parser/fingerprint.hpp"

#include <gtest/gtest.h>

#include <string>

namespace kds::parser {
namespace {

Fingerprint Must(std::string_view sql) {
    auto fp = FingerprintOf(sql);
    EXPECT_TRUE(fp.has_value()) << "expected a pattern for: " << sql;
    return fp.value_or(Fingerprint{});
}

// ---- The property the whole design rests on -------------------------------

TEST(FingerprintTest, InlineLiteralAndBindParameterShareOnePatternId) {
    const Fingerprint inlined = Must("SELECT * FROM accounts WHERE id = 42");
    const Fingerprint bound = Must("SELECT * FROM accounts WHERE id = ?");

    EXPECT_EQ(inlined.pattern_id, bound.pattern_id);
    // They must not share an arg_hash: one supplied a value, the other did
    // not, and treating them as the same instance would hand a client's
    // bound query the trail of somebody's literal 42.
    EXPECT_NE(inlined.arg_hash, bound.arg_hash);

    EXPECT_EQ(inlined.literal_count, 1u);
    EXPECT_EQ(inlined.param_count, 0u);
    EXPECT_EQ(bound.literal_count, 0u);
    EXPECT_EQ(bound.param_count, 1u);
}

TEST(FingerprintTest, IntAndStringLiteralsShareAShapeButNotAnArgHash) {
    // The shape cannot distinguish them - a bind parameter's type is not
    // known at parse - so the argument stream must.
    const Fingerprint as_int = Must("SELECT * FROM t WHERE c = 1");
    const Fingerprint as_str = Must("SELECT * FROM t WHERE c = '1'");

    EXPECT_EQ(as_int.pattern_id, as_str.pattern_id);
    EXPECT_NE(as_int.arg_hash, as_str.arg_hash);
}

TEST(FingerprintTest, SameShapeDifferentValuesShareOnePatternId) {
    const Fingerprint a = Must("SELECT * FROM t WHERE id = 1");
    const Fingerprint b = Must("SELECT * FROM t WHERE id = 999999");

    EXPECT_EQ(a.pattern_id, b.pattern_id);
    EXPECT_NE(a.arg_hash, b.arg_hash);
}

TEST(FingerprintTest, IdenticalStatementsHashIdentically) {
    const Fingerprint a = Must("UPDATE t SET c = 5 WHERE id = 7");
    const Fingerprint b = Must("UPDATE t SET c = 5 WHERE id = 7");

    EXPECT_EQ(a.pattern_id, b.pattern_id);
    EXPECT_EQ(a.arg_hash, b.arg_hash);
}

// ---- What is shape --------------------------------------------------------

TEST(FingerprintTest, DifferentRelationsAreDifferentPatterns) {
    // A trail recorded against `accounts` is worthless to `trades`, so
    // they must not share a waystone.
    EXPECT_NE(Must("SELECT * FROM accounts WHERE id = 1").pattern_id,
              Must("SELECT * FROM trades WHERE id = 1").pattern_id);
}

TEST(FingerprintTest, DifferentColumnsAndOperatorsAreDifferentPatterns) {
    const std::uint64_t base = Must("SELECT * FROM t WHERE a = 1").pattern_id;
    EXPECT_NE(base, Must("SELECT * FROM t WHERE b = 1").pattern_id);
    EXPECT_NE(base, Must("SELECT * FROM t WHERE a > 1").pattern_id);
    EXPECT_NE(base, Must("SELECT * FROM t WHERE a >= 1").pattern_id);
    EXPECT_NE(base, Must("SELECT * FROM t WHERE a != 1").pattern_id);
}

TEST(FingerprintTest, StatementKindsDoNotCollide) {
    EXPECT_NE(Must("SELECT * FROM t WHERE id = 1").pattern_id,
              Must("UPDATE t SET c = 1 WHERE id = 1").pattern_id);
}

TEST(FingerprintTest, PredicateCountIsPartOfTheShape) {
    EXPECT_NE(Must("SELECT * FROM t WHERE a = 1").pattern_id,
              Must("SELECT * FROM t WHERE a = 1 AND b = 2").pattern_id);
}

TEST(FingerprintTest, AdjacentIdentifiersCannotBleedTogether) {
    // Guards the length prefix in Fnv1a::Field(). Without it these two
    // token streams flatten to the same bytes.
    EXPECT_NE(Must("SELECT * FROM ab WHERE c = 1").pattern_id,
              Must("SELECT * FROM a WHERE bc = 1").pattern_id);
}

TEST(FingerprintTest, NullIsShapeNotAnArgument) {
    const Fingerprint with_null = Must("UPDATE t SET c = NULL WHERE id = 1");
    const Fingerprint with_value = Must("UPDATE t SET c = 2 WHERE id = 1");

    // A NULL assignment is a different shape from a value assignment, and
    // it contributes nothing to bind.
    EXPECT_NE(with_null.pattern_id, with_value.pattern_id);
    EXPECT_EQ(with_null.literal_count, 1u);  // the WHERE literal only
}

// ---- What is normalized away ----------------------------------------------

TEST(FingerprintTest, KeywordAndIdentifierCaseIsFolded) {
    const std::uint64_t lower = Must("select * from accounts where id = 1").pattern_id;
    EXPECT_EQ(lower, Must("SELECT * FROM ACCOUNTS WHERE ID = 1").pattern_id);
    EXPECT_EQ(lower, Must("SeLeCt * FrOm Accounts WhErE Id = 1").pattern_id);
}

TEST(FingerprintTest, WhitespaceAndCommentsAreNormalizedAway) {
    const std::uint64_t plain = Must("SELECT * FROM t WHERE id = 1").pattern_id;
    EXPECT_EQ(plain, Must("SELECT   *\n  FROM t\tWHERE id=1").pattern_id);
    EXPECT_EQ(plain, Must("SELECT * FROM t -- a comment\n WHERE id = 1").pattern_id);
}

TEST(FingerprintTest, TrailingSemicolonIsNormalizedAway) {
    const Fingerprint bare = Must("SELECT * FROM t WHERE id = 1");
    const Fingerprint terminated = Must("SELECT * FROM t WHERE id = 1;");

    EXPECT_EQ(bare.pattern_id, terminated.pattern_id);
    EXPECT_EQ(bare.arg_hash, terminated.arg_hash);
}

TEST(FingerprintTest, StringLiteralCaseAndContentAreNotFolded) {
    // Identifiers fold; values do not. 'Alice' and 'alice' are different
    // arguments to the same pattern.
    const Fingerprint upper = Must("SELECT * FROM t WHERE c = 'Alice'");
    const Fingerprint lower = Must("SELECT * FROM t WHERE c = 'alice'");

    EXPECT_EQ(upper.pattern_id, lower.pattern_id);
    EXPECT_NE(upper.arg_hash, lower.arg_hash);
}

TEST(FingerprintTest, ArgumentOrderMatters) {
    const Fingerprint ab = Must("SELECT * FROM t WHERE a = 1 AND b = 2");
    const Fingerprint ba = Must("SELECT * FROM t WHERE a = 2 AND b = 1");

    EXPECT_EQ(ab.pattern_id, ba.pattern_id);
    EXPECT_NE(ab.arg_hash, ba.arg_hash);
}

// ---- Statements with no pattern -------------------------------------------

TEST(FingerprintTest, DdlHasNoPattern) {
    EXPECT_FALSE(FingerprintOf("CREATE TABLE t (id int64, c int64)").has_value());
    EXPECT_FALSE(FingerprintOf("create table t (id int64) BTREE").has_value());
}

TEST(FingerprintTest, SessionAndAdminStatementsHaveNoPattern) {
    EXPECT_FALSE(FingerprintOf("SET DURABILITY STRICT").has_value());
    EXPECT_FALSE(FingerprintOf("SHOW META").has_value());
    EXPECT_FALSE(FingerprintOf("LIST TABLES").has_value());
    EXPECT_FALSE(FingerprintOf("DESCRIBE t").has_value());
    EXPECT_FALSE(FingerprintOf("SYNC").has_value());
}

TEST(FingerprintTest, UnknownLeadingWordHasNoPattern) {
    // The allow-list's whole point: a word this grammar has never heard of
    // is not patternable, rather than being patternable by default.
    EXPECT_FALSE(FingerprintOf("DELETE FROM t WHERE id = 1").has_value());
    EXPECT_FALSE(FingerprintOf("VACUUM").has_value());
}

TEST(FingerprintTest, EmptyAndUnlexableInputHaveNoPattern) {
    EXPECT_FALSE(FingerprintOf("").has_value());
    EXPECT_FALSE(FingerprintOf("   \n\t ").has_value());
    EXPECT_FALSE(FingerprintOf("SELECT * FROM t WHERE id @ 1").has_value());
    // A leading token that is not an identifier cannot start a statement.
    EXPECT_FALSE(FingerprintOf("42 SELECT").has_value());
}

// ---- Stability ------------------------------------------------------------

// These pin the algorithm. They are not testing arithmetic - they are the
// only thing that can catch an *accidental* change to a value that is
// persisted in sys.patterns and keys every stored waystone.
//
// The version is asserted here, beside the hashes, on purpose: the two
// move together. Whoever changes the algorithm sees these values fail, and
// the failure is the reminder that `kFingerprintVersion` has to be bumped
// so stored patterns retire instead of resolving trails recorded under
// different rules.
TEST(FingerprintTest, PatternIdAndArgHashAreStableAcrossBuilds) {
    const Fingerprint fp = Must("SELECT * FROM accounts WHERE id = 42");
    EXPECT_EQ(fp.pattern_id, 0xe0fa0b4bc8f0ebe2ull);
    EXPECT_EQ(fp.arg_hash, 0x182b9abf546ab5c4ull);
    EXPECT_EQ(kFingerprintVersion, 1u);
}

// ---- Versioning (P02) -----------------------------------------------------

TEST(FingerprintTest, OnlyTheRunningBuildsVersionIsCurrent) {
    EXPECT_TRUE(IsCurrentFingerprintVersion(kFingerprintVersion));

    // Exact identity, not an ordering. An older row's pattern_ids were
    // computed under different rules and name shapes that are not the ones
    // they claim, so a newer build must not accept them - and the mirror
    // case, an older build meeting a newer row, is wrong for the same
    // reason. A `>=` here would silently resurrect every trail this
    // constant exists to retire.
    EXPECT_FALSE(IsCurrentFingerprintVersion(kFingerprintVersion - 1));
    EXPECT_FALSE(IsCurrentFingerprintVersion(kFingerprintVersion + 1));
    EXPECT_FALSE(IsCurrentFingerprintVersion(0xFFFFFFFFu));
}

TEST(FingerprintTest, AZeroedRowIsNeverCurrent) {
    // A sys.patterns row read out of a zeroed or never-written page
    // decodes to version 0. It must not pass for a current row, which is
    // why 0 is reserved and the constant is never allowed to take it.
    EXPECT_FALSE(IsCurrentFingerprintVersion(0));
    EXPECT_NE(kFingerprintVersion, 0u);
}

TEST(FingerprintTest, AForeignVersionIsAMissNotAnError) {
    // The catalog-level form of this - a stored row whose version does not
    // match resolving as "no pattern" rather than failing the statement -
    // lands with the sys.patterns lookup in P04. What is pinnable today is
    // the decision it rests on: the predicate answers, it never fails, so
    // there is no error path for a caller to propagate by mistake.
    static_assert(noexcept(IsCurrentFingerprintVersion(0)));
    static_assert(IsCurrentFingerprintVersion(kFingerprintVersion));
    static_assert(!IsCurrentFingerprintVersion(0));

    // And a statement is fingerprinted the same way regardless: version
    // gates whether a *stored* row is usable, never whether a statement
    // has a pattern.
    EXPECT_TRUE(FingerprintOf("SELECT * FROM t WHERE id = 1").has_value());
}

TEST(FingerprintTest, AnEmptyArgumentStreamHasAFixedHash) {
    // The FNV offset basis, unmodified: a statement with no inline
    // literals still has a well-defined instance key.
    const Fingerprint fp = Must("SELECT * FROM t");
    EXPECT_EQ(fp.literal_count, 0u);
    EXPECT_EQ(fp.param_count, 0u);
    EXPECT_EQ(fp.arg_hash, 14695981039346656037ull);
}

}  // namespace
}  // namespace kds::parser
