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

// ---- V04: reserving a word must not move a hash ---------------------------

TEST(FingerprintTest, AReservedWordHashesExactlyAsAnIdentifierDoes) {
    // The invariant V04 is graded on, stated where it can fail loudly.
    // Before V04 the lexer had no keyword token type, so `IN` and
    // `BETWEEN` reached this code as kIdent. If reserving them changed
    // their shape tag - or dropped their text - every pattern_id for a
    // statement containing one would move, and pattern_id is the key to
    // every stored waystone. That is a format break, and it would be
    // caused by a change that alters no statement's meaning.
    //
    // These two hashes were recorded from a build in which `IN` and
    // `BETWEEN` did not exist as keywords - they came off the lexer as
    // plain identifiers. Pinning the literal values is the only assertion
    // that actually witnesses the invariant: a relative comparison between
    // two post-V04 hashes moves in lockstep when the tag changes and
    // notices nothing.
    EXPECT_EQ(Must("SELECT * FROM t WHERE id IN (1, 2)").pattern_id, 0xb87254b3cae4a5b8ull);
    EXPECT_EQ(Must("SELECT * FROM t WHERE id BETWEEN 1 AND 5").pattern_id, 0xaf3f6ac336bb68d8ull);
    EXPECT_EQ(Must("SELECT * FROM t AS a").pattern_id, 0xa91c9b42e1642b7cull);

    // And the text still distinguishes one reserved word from another,
    // case-folded exactly as an identifier is - so reserving a word costs
    // no shape resolution either.
    EXPECT_NE(Must("SELECT * FROM t WHERE a in b").pattern_id,
              Must("SELECT * FROM t WHERE a between b").pattern_id);
    EXPECT_EQ(Must("SELECT * FROM t WHERE a IN b").pattern_id,
              Must("SELECT * FROM t WHERE a in b").pattern_id);
}

TEST(FingerprintTest, AQualifiedNameIsFingerprintableAndTheDotIsShape) {
    // A '.' lexed as kError before V04, and kError means nullopt - so
    // these statements had no fingerprint at all. Gaining one is the
    // transition fingerprint.hpp's bump rule permits without a version
    // bump: nothing already stored changes meaning.
    const auto qualified = FingerprintOf("SELECT * FROM t WHERE t.id = 1");
    ASSERT_TRUE(qualified.has_value());

    // The dot is shape, not an argument: it says where a column lives.
    EXPECT_EQ(qualified->literal_count, 1u);
    EXPECT_NE(qualified->pattern_id, Must("SELECT * FROM t WHERE t id = 1").pattern_id);
}

TEST(FingerprintTest, AnEmptyArgumentStreamHasAFixedHash) {
    // The FNV offset basis, unmodified: a statement with no inline
    // literals still has a well-defined instance key.
    const Fingerprint fp = Must("SELECT * FROM t");
    EXPECT_EQ(fp.literal_count, 0u);
    EXPECT_EQ(fp.param_count, 0u);
    EXPECT_EQ(fp.arg_hash, 14695981039346656037ull);
}

// ---- CREATE PATTERN: the $param fold (spec section 3.2) -------------------

// The done-condition of step 1 of the spec's implementation order, and the
// single property the whole feature rests on. A declaration's body never
// executes; live traffic does, and it carries no declaration. If a declared
// `$param` hashed as anything but the marker a literal and a `?` share, a
// declared pattern would match nothing that ever runs - and it would fail
// *silently*, since there is no error to report when two hashes simply
// differ.
TEST(FingerprintTest, ADeclaredParamHashesAsAValueSoAllThreeFormsAreOnePattern) {
    const Fingerprint declared =
        Must("SELECT id FROM account AS a WHERE a.flag = $flag AND a.name = $name");
    const Fingerprint inlined =
        Must("SELECT id FROM account AS a WHERE a.flag = 42 AND a.name = 'x'");
    const Fingerprint bound =
        Must("SELECT id FROM account AS a WHERE a.flag = ? AND a.name = ?");

    EXPECT_EQ(declared.pattern_id, inlined.pattern_id);
    EXPECT_EQ(declared.pattern_id, bound.pattern_id);

    // A `$param` carries no value - a declaration is not an execution, so
    // instances still arise only from traffic (spec section 3.3). It
    // therefore counts as a parameter, never as a literal, and shares the
    // empty argument stream a fully bound statement has.
    EXPECT_EQ(declared.literal_count, 0u);
    EXPECT_EQ(declared.param_count, 2u);
    EXPECT_EQ(declared.arg_hash, bound.arg_hash);
    EXPECT_NE(declared.arg_hash, inlined.arg_hash);
}

TEST(FingerprintTest, AParamsNameContributesNothingToTheShape) {
    // Names exist for the declaration's readability and for future named
    // binds. Live traffic has no name to contribute, so anything the name
    // added to pattern_id would break the convergence above.
    EXPECT_EQ(Must("SELECT * FROM t WHERE id = $a").pattern_id,
              Must("SELECT * FROM t WHERE id = $b").pattern_id);
    EXPECT_EQ(Must("SELECT * FROM t WHERE id = $a").pattern_id,
              Must("SELECT * FROM t WHERE id = $A").pattern_id);

    // And a parameter is not its name spelled without the sigil: `$a` is a
    // value, a bare `a` is a column. That distinction is the sigil's whole
    // job (spec section 2).
    EXPECT_NE(Must("SELECT * FROM t WHERE id = $a").pattern_id,
              Must("SELECT * FROM t WHERE id = a").pattern_id);
}

TEST(FingerprintTest, TheParamTokenNeededNoFingerprintVersionBump) {
    // `$` lexed as kError before this feature, and kError means nullopt -
    // so a statement containing one had no fingerprint at all. Gaining one
    // is exactly the transition fingerprint.hpp's bump rule names as *not*
    // requiring a version bump: nothing already stored changes meaning.
    //
    // The golden hashes above are the witness, and this is the reminder of
    // what they are witnessing. If a future change to the shape stream does
    // move them, the version has to move with it.
    EXPECT_EQ(kFingerprintVersion, 1u);
    EXPECT_EQ(Must("SELECT * FROM accounts WHERE id = 42").pattern_id, 0xe0fa0b4bc8f0ebe2ull);

    // A bare `$` is still a lexing failure, and a statement that will not
    // lex has no shape worth storing.
    EXPECT_FALSE(FingerprintOf("SELECT * FROM t WHERE id = $").has_value());
    EXPECT_FALSE(FingerprintOf("SELECT * FROM t WHERE id = $ 1").has_value());
}

// ---- The bare numeric literal (TY3 phase 2) -------------------------------

TEST(FingerprintTest, ABareNumericIsTheQuotedStringOfItsSpelling) {
    // Not a shape-only convergence like int-versus-string: the two
    // spellings produce one AST, so they are one statement, and both
    // halves of the fingerprint must agree - pattern_id via the shared
    // kValue marker, arg_hash because the argument stream hashes the same
    // tag and the same bytes for both.
    const Fingerprint bare = Must("SELECT * FROM t WHERE amt = 12.34");
    const Fingerprint quoted = Must("SELECT * FROM t WHERE amt = '12.34'");

    EXPECT_EQ(bare.pattern_id, quoted.pattern_id);
    EXPECT_EQ(bare.arg_hash, quoted.arg_hash);
    EXPECT_EQ(bare.literal_count, 1u);

    // And the value hole is the same hole `?` leaves.
    const Fingerprint bound = Must("SELECT * FROM t WHERE amt = ?");
    EXPECT_EQ(bare.pattern_id, bound.pattern_id);

    // A different spelling of the same number is a different argument -
    // the text is hashed, not the value, exactly as `42` vs `042`.
    const Fingerprint padded = Must("SELECT * FROM t WHERE amt = 12.340");
    EXPECT_EQ(bare.pattern_id, padded.pattern_id);
    EXPECT_NE(bare.arg_hash, padded.arg_hash);
}

TEST(FingerprintTest, TheKeyModeWordNeededNoFingerprintVersionBump) {
    // PK03, and its removal in 2026-08-25 (docs/spec/heap-and-tuple.md §4.1) -
    // one test, because both directions are the easy half of the bump rule.
    // `ASSIGNED` and `EXPLICIT` lexed as ordinary identifiers before either
    // change and hash as ordinary identifiers after: adding them moved no
    // hash because no parsing statement contained one, and refusing
    // `ASSIGNED` moves none because a statement that no longer parses no
    // longer hashes at all. The version therefore stays put through both.
    //
    // This is here so that a later change which *does* move a hash cannot
    // pass by leaving the version alone quietly: the golden corpus pins the
    // hashes, and this pins the version they are relative to.
    EXPECT_EQ(kFingerprintVersion, 1u);
    EXPECT_EQ(Must("SELECT * FROM accounts WHERE id = 42").pattern_id, 0xe0fa0b4bc8f0ebe2ull);

    // And neither word is reserved, so one used as a column name is still
    // an identifier with a shape - which is the property that keeps the
    // hashes above from having moved in the first place.
    EXPECT_TRUE(FingerprintOf("SELECT * FROM t WHERE explicit = 1").has_value());
    EXPECT_TRUE(FingerprintOf("SELECT * FROM t WHERE assigned = 1").has_value());
}

TEST(FingerprintTest, TheNumericTokenNeededNoFingerprintVersionBump) {
    // The subtler case of the bump rule, argued in fingerprint.cpp:
    // `12.34` *did* lex before this token (int, dot, int), so its
    // statement was fingerprintable and its hash has now moved. No bump,
    // because that hash was never storable - int-dot-int parses in no
    // production, and only statements that execute are recorded. The
    // golden corpus pins every pre-existing statement's hash unchanged;
    // this pins the version those pins are relative to.
    EXPECT_EQ(kFingerprintVersion, 1u);
    EXPECT_EQ(Must("SELECT * FROM accounts WHERE id = 42").pattern_id, 0xe0fa0b4bc8f0ebe2ull);
}

}  // namespace
}  // namespace kds::parser
