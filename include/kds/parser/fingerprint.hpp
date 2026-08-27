#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "kds/parser/token.hpp"

// Query-template fingerprinting: the parse-time reduction of a statement
// to its function form - `patternX(a, b)` - as a pair of integers
// (docs/spec/waystone-concpets.md sections 1 and 3, docs/parser.md I1). This is
// the gate every other Waystone task sits behind: without a stable
// pattern identity there is nothing to key a waystone on.
//
// One pass over the token stream produces both halves:
//
//   pattern_id   hash of the *shape* - every token in order, with each
//                value literal replaced by a single parameter marker.
//   arg_hash     hash of the *arguments* - the inline literal values, in
//                the order they appeared.
//
// ---- The property that makes this worth building -------------------------
//
// `WHERE id = 42` and `WHERE id = ?` reduce to the **same** pattern_id.
// A client that inlines its literals and one that binds parameters share a
// waystone, which is the whole reason the fingerprint is taken from the
// token stream rather than from the finished AST: the AST has already
// thrown away the distinction between "a literal was here" and "a value
// belongs here", and recovering it afterwards is a second pass that this
// design specifically does not want (I1).
//
// The two forms differ in arg_hash, and they must. A `?` contributes
// nothing to the argument stream, because its value does not exist yet -
// it arrives at BIND. `param_count` is what a future BIND stage uses to
// know how many bound values to fold in before the arg_hash is final; see
// "What this is not, yet" below.
//
// ---- What is shape and what is argument ---------------------------------
//
// **Identifiers are shape.** `SELECT * FROM accounts WHERE id = 1` and
// `SELECT * FROM trades WHERE id = 1` are different patterns, because
// they read different relations and a trail from one is worthless to the
// other. Keywords are hashed as identifiers - same tag, same folded text -
// which makes SELECT and UPDATE differ for free.
//
// That the lexer now has a keyword token type (V04) changes nothing here,
// and must not: those seven words were identifiers to it before, so
// tagging them differently would move the pattern_id of every stored
// statement containing one. Reserving a word is a grammar change, never a
// fingerprint change.
//
// Identifiers are folded to lower case, so `SELECT`/`select` and
// `accounts`/`ACCOUNTS` converge - matching the case-insensitive
// comparison the rest of the engine already does with names. The fold is
// written out by hand rather than through `std::tolower`, which is
// locale-dependent: this hash is persisted in `sys.patterns` and must not
// depend on the locale the server happened to boot in.
//
// **Value literals are arguments.** Integers and strings become one
// shared marker in the shape - the same marker a `?` produces - and their
// text goes to the argument stream tagged with its type. One marker for
// both types is deliberate: a bind parameter's type is not known at parse,
// so distinguishing int-shaped from string-shaped holes would break the
// convergence above. The type is not lost, it moves to arg_hash.
//
// **NULL is shape.** It carries no value to bind, and a statement with an
// inline NULL is a genuinely different shape from one with a value in that
// position - `IS NULL` semantics are not `= <value>` semantics. It gets
// its own marker and contributes nothing to the argument stream.
//
// A trailing semicolon is skipped, so `SELECT * FROM t;` and
// `SELECT * FROM t` are one pattern.
//
// ---- Literal text, not literal value ------------------------------------
//
// The argument stream hashes each literal's **text**, not its decoded
// value. That trades one harmless failure for one dangerous one: two
// spellings of the same number (`42` and `042`) hash differently and miss
// each other, costing a replay; whereas hashing the decoded `int_val`
// would let two genuinely different literals collide, because the lexer's
// decode silently wraps past 64 bits. A miss costs a descent. A collision
// costs a wrong location, and while the trail's own validation would catch
// even that (below), there is no reason to manufacture the case.
//
// ---- Collisions are survivable by construction --------------------------
//
// The hash is FNV-1a/64: stable, dependency-free, value-based, and not
// cryptographic. Two distinct shapes can in principle land on one
// pattern_id. At an application's realistic scale - hundreds of distinct
// statement shapes, not billions - the birthday bound puts that at around
// 1e-15, but the reason it is *safe* is structural rather than
// statistical: a colliding pattern's trail names tuples that fail the
// replay validation in waystone-concpets.md section 2 (wrong `rel_oid`,
// wrong Keystone id at the recorded slot), so it degrades to a miss and a
// fall-through. A collision can cost performance. It cannot produce a row.
//
// ---- Stability is a hard requirement ------------------------------------
//
// pattern_id is persisted in `sys.patterns` and is the key to stored
// waystones, so it must be a pure function of the statement text. No
// pointer values, no addresses, no container iteration order, no locale,
// no `std::hash` (whose output is explicitly not stable across runs or
// implementations). The unit tests pin exact values for that reason.
//
// A change to the algorithm is therefore a *format* change, and
// `kFingerprintVersion` below is how it is survived without a migration.
//
// ---- Where the numbers come from ----------------------------------------
//
// **Folded into the parse (2026-08-03).** `FingerprintAccumulator` below is
// fed by the lexer as it produces tokens, so a parse yields the fingerprint
// with no second pass over the text. `FingerprintOf()` - which does lex
// independently - is now for callers holding a string and no parse, and is
// implemented on the same accumulator so there is one set of rules.
//
// This was the cost of being a bolt-on and it was not small: on the
// statement path the second lex measured ~13% of a point join's latency,
// three times what the Waystone recording it existed for actually cost, and
// it was the whole of replay's B+ tree regression
// (bench/results-waystone-v2.md). What survives the blueprint-parser rewrite
// is this file's *contract* - a statement in, two integers out - not how the
// numbers are produced. Nothing downstream should depend on the latter.
//
// **Lexing now allocates nothing at all** (2026-08-03): `Token::text` is a
// view into the statement (token.hpp), and the fold below hashes case as it
// goes rather than building a folded copy per identifier. Those were the two
// remaining per-token allocations, and the second one mattered here
// specifically - a fingerprint that built a `std::string` per token would
// have undone zero-copy tokens for exactly the tokens that motivated them.
//
// arg_hash covers inline literals only. When a BIND stage exists it must
// fold the bound values in on top, in parameter order, to reach the final
// instance key; `param_count` is how it knows how many to expect.

namespace kds::parser {

// ---- The version, and the one rule that governs it ------------------------
//
// Which revision of the fingerprinting algorithm produced a stored
// `pattern_id`. Persisted on every `sys.patterns` row, and the reason the
// blueprint parser (docs/parser.md I1) can replace this file without a
// migration: it will not reproduce these hashes, and it does not have to.
// It bumps this, and every pattern recorded under the old rules stops
// resolving.
//
// **The rule for bumping, stated once:** bump whenever an *already
// fingerprintable* statement would hash differently than it does today.
// Concretely, that is a change to any of - the token stream a statement
// reduces to (a new token type appearing in a shape, a change to what is
// skipped), the shape or argument tag values, the framing of a hashed
// field, the case-folding rule, or the hash function itself.
//
// The distinction that is easy to get wrong: making a statement
// fingerprintable that previously was not - adding `DELETE` to the
// patternable leading words, say - **does not** require a bump. Nothing
// already stored changes meaning; a shape that hashed to X still hashes to
// X. Only a change that moves an existing statement's hash invalidates
// what is on disk.
//
// One refinement, earned by the bare-numeric token (TY3 phase 2):
// "already fingerprintable" means *already storable* - a statement that
// lexes but cannot parse produces a hash, but that hash can never reach
// `sys.patterns`, because nothing records a statement that does not
// execute and a CREATE PATTERN body must itself parse. Fusing
// `12.34`'s three tokens into one moved the hash of exactly such
// statements and nothing else, so the version did not move. The claim a
// change like it must argue: the only token sequences whose hashing
// changes appear in no statement any production accepts.
//
// Never 0. A `sys.patterns` row read out of a zeroed or never-written page
// decodes to version 0, and that must not be mistaken for a current row -
// which is exactly the mistake `IsCurrentFingerprintVersion()` exists to
// make impossible.
inline constexpr std::uint32_t kFingerprintVersion = 1;
static_assert(kFingerprintVersion != 0, "0 is reserved for an unset/zeroed row");

// Whether a stored pattern's `fingerprint_version` was produced by the
// running build, and therefore whether its `pattern_id` means anything
// here.
//
// A mismatch is a **miss, not an error**: the pattern resolves as though
// it had never been recorded, the statement executes normally, and a new
// row is recorded under the current version. Nothing fails, nothing is
// migrated, and the stale rows and their waystones are garbage for
// retention to reclaim (workplan P15).
//
// Deliberately an exact comparison rather than `>=` or `<=`. A newer build
// must not accept an older row - its `pattern_id`s were computed under
// different rules and name shapes that are not the ones they claim - and
// an older build must not accept a newer one for the same reason. There is
// no ordering here, only identity, and an inequality would silently
// resurrect exactly the trails this constant exists to retire.
constexpr bool IsCurrentFingerprintVersion(std::uint32_t stored) noexcept {
    return stored == kFingerprintVersion;
}

// The function form of one statement: which pattern, and with what
// arguments. Both halves are meaningless on their own - a waystone is
// keyed on the pair (docs/spec/waystone-concpets.md section 5).
struct Fingerprint {
    // Identifies the statement's shape. The key of a `sys.patterns` row.
    std::uint64_t pattern_id = 0;

    // Identifies the arguments bound into that shape. Selects a waystone
    // within the pattern's directory. Covers inline literals only.
    std::uint64_t arg_hash = 0;

    // Inline literals folded into arg_hash.
    std::uint32_t literal_count = 0;

    // `?` placeholders whose values arrive at BIND. Non-zero means
    // arg_hash is incomplete, not that it is wrong.
    std::uint32_t param_count = 0;
};

// Reduces `sql` to its function form, or reports that it has none.
//
// Returns nullopt - deliberately, rather than a Status - for every
// statement that is not patternable, because none of them is an error and
// a caller must do the same thing in all of them (record nothing, replay
// nothing, execute normally):
//
//   - DDL and session/admin statements. `CREATE TABLE`, and anything
//     whose leading keyword is not one of the three below. A pattern is a
//     thing worth *repeating*; DDL is not, and SET has no result to trail
//     (docs/parser.md I8 excludes them from fingerprinting explicitly).
//   - Empty or whitespace-only input.
//   - Anything the lexer cannot tokenize. A statement that will fail to
//     parse has no shape worth remembering, and refusing to guess one
//     keeps a malformed statement from occupying a catalog row.
//
// Never fails, never allocates beyond the lexer's own token strings, and
// never touches the catalog: this is a pure function of its argument, and
// the tests depend on it staying that way.
//
// **This lexes `sql` from scratch.** On the statement path that is a second
// lex on top of the parser's own - see `FingerprintAccumulator` below, which
// is how the parser avoids it. This entry point remains for callers with a
// string and no parse: the golden corpus, which fingerprints statements that
// deliberately do not parse, and `CREATE PATTERN`, which fingerprints a
// *substring* of the statement it just parsed (its body) and is not on any
// hot path.
std::optional<Fingerprint> FingerprintOf(std::string_view sql);

// ---- The same reduction, driven by someone else's token stream -----------
//
// Fed one token at a time, in the order the lexer produced them. This is
// what lets the fingerprint ride along with the parse instead of costing a
// second pass: `Lexer` owns one of these and feeds it from the single place
// every token is produced, so the parser gets a fingerprint for free and
// nothing lexes twice.
//
// **`FingerprintOf()` above is implemented on top of this**, and that is
// load-bearing rather than tidy. Two implementations of a hash that is
// persisted in `sys.patterns` and keys every stored waystone would be two
// chances to disagree, and a disagreement would not fail - it would quietly
// give the same statement two different pattern_ids depending on which path
// computed it, retiring trails at random. There is one set of rules and both
// entry points run it.
//
// The contract on feeding, which the lexer satisfies by construction:
//
//   - tokens arrive **in order** and **exactly once** each. `Lexer::Peek()`
//     caches, so a peeked token is produced once and consumed later; that
//     is why the hook is in `ReadToken()` and not in `Next()`.
//   - the stream is **complete** only once `kEof` has been fed. A parse that
//     stopped early has fed a prefix, and a prefix of a hash is not a hash
//     of a prefix - `Result()` answers nullopt rather than something
//     plausible.
class FingerprintAccumulator {
public:
    // `tok` is the next token the lexer produced. Feeding `kEof` ends the
    // stream; feeding anything after that is ignored.
    void Feed(const Token& tok) noexcept;

    // The fingerprint of everything fed so far, or nullopt when the stream
    // is not a pattern: an unpatternable leading word, a token the lexer
    // could not read, or a stream that never reached `kEof`.
    std::optional<Fingerprint> Result() const noexcept;

    // Whether `kEof` has been fed. Exposed because "did the parse consume
    // the whole statement" is a question the parser can answer more cheaply
    // this way than by re-examining its own state.
    bool complete() const noexcept { return complete_; }

    void Reset() noexcept;

private:
    // FNV-1a states, not a helper type: this is a header on the hot path and
    // the algorithm is two integers and a loop. The loop lives in the .cpp
    // with the tag constants it feeds, which are format and belong together.
    std::uint64_t shape_ = 0;
    std::uint64_t args_ = 0;

    std::uint32_t literal_count_ = 0;
    std::uint32_t param_count_ = 0;

    bool started_ = false;   // the leading word has been seen
    bool valid_ = false;     // ...and it was patternable, and nothing failed to lex
    bool complete_ = false;  // kEof has been fed

    // BI5's suppression (docs/spec/bulkinsert.md §2.4): an INSERT's shape
    // is its **first row's**. Once the first top-level paren group of an
    // INSERT-headed stream has closed, later tokens fold literals into
    // arg_hash and nothing into the shape - so `VALUES (1)` and
    // `VALUES (1), (2), (3)` are one pattern_id, and row count never
    // fragments sys.patterns. Safe for every *stored* hash: a storable
    // 1-row statement has nothing but `;`/EOF after its group, which
    // folded nothing before either.
    bool insert_head_ = false;        // the leading word was INSERT
    bool first_group_closed_ = false; // ...and its first () group has ended
    std::uint32_t paren_depth_ = 0;
};

}  // namespace kds::parser
