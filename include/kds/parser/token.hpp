#pragma once

#include <cstdint>
#include <string_view>

// Lexer token types.
//
// ---- A token owns nothing: `text` is a view into the SQL ----------------
//
// `Token::text` is a `std::string_view` **into the statement the lexer was
// given**, not a copy of it. A `Token` is therefore trivially copyable and
// costs nothing to move around, and lexing a statement allocates nothing at
// all - where it used to allocate one `std::string` per identifier or
// literal longer than the small-string limit, which on a statement with
// real relation names is most of them.
//
// **The rule that makes this safe: a token must not outlive the SQL it was
// lexed from.** In practice tokens never escape a parse - the AST copies
// every name and value it keeps into its own `std::string` (docs/parser.md
// I4's copy-at-the-boundary rule), so a `Statement` is self-contained and
// the source can go away the moment `Parse()` returns. Anything that
// collects `Token`s into a container and outlives the source is a
// use-after-free, and there is nothing here that can catch it for you.

namespace kds::parser {

// Words the grammar reserves (docs/inflight/in-progress/parser-v2-workplan.md V04). Reserved
// means a statement cannot use one as a column or table name - `SELECT *
// FROM t WHERE in = 1` stops parsing here rather than reading `in` as an
// identifier.
//
// The list is exactly what the v2 grammar needs to *see*; needing to see a
// word is not the same as being able to execute it. `NOT` and `EXISTS` are
// reserved by V04 and answer with a real error until V07 implements them,
// which is the point - a truthful "not supported yet, at byte 23" beats a
// syntax error pointing at the wrong thing.
//
// Not reserved, deliberately: `SELECT`, `FROM`, `WHERE`, `AND`, `INSERT`,
// `UPDATE`, `SET`, `VALUES`, `CREATE`, `TABLE`, `HEAP`, `BTREE`. Those are
// matched by text where the grammar expects them (Parser::ExpectKeyword),
// which is what lets a column be named `values` today. Moving one of them
// in here is a language change, not a lexer change, and it would break
// statements that parse now.
enum class Keyword : std::uint8_t {
    kJoin,
    kOn,
    kAs,
    kIn,
    kExists,
    kNot,
    kBetween,

    // Reserved by V05 and rejected by it: an outer join answers
    // `Unsupported` with the keyword's own position (spec I9). Reserved
    // *now*, before they are implementable, so a client gets a truthful
    // "not supported, here" instead of a syntax error pointing somewhere
    // else - and so the grammar does not shift when they land.
    //
    // Costs nothing to the fingerprint: they were identifiers to the lexer
    // before, and a keyword hashes exactly as an identifier does. That is
    // the property V04 bought by giving all keywords one token type.
    kLeft,
    kRight,
    kFull,
    kOuter,
};

enum class TokenType {
    kEof,

    // literals
    kIdent,    // table/column name, or an unreserved keyword
    kIntLit,   // 42, -7
    kStrLit,   // 'hello' (quotes stripped)

    // A bare numeric literal with a decimal point: `12.34`, `-0.5`. Both
    // sides of the point must have digits - `12.` and `.5` lex as they
    // always did (an integer and a dot; a dot and an integer), so a
    // qualified name's grammar does not move.
    //
    // **A bare numeric is sugar for the quoted string of its spelling,
    // exactly** (docs/spec/types.md TY3 phase 2): the parser produces the
    // same AstValue `'12.34'` produces, and the fingerprint hashes the same
    // argument bytes, so `= 12.34` and `= '12.34'` are one statement
    // everywhere - one pattern, one arg_hash, one meaning. The column's
    // type decides the interpretation at the same single gate the quoted
    // form goes through (`CoerceLiteralToColumn` / `EncodeOneValue`), which
    // is what keeps this a lexer convenience rather than a type-system
    // event. `text` is the full spelling, sign and point included;
    // `int_val` and `digits()` are meaningless for it and stay unset.
    kNumLit,
    kNullLit,  // NULL

    // A reserved word (see Keyword above); which one is in Token::kw.
    //
    // One token type for all of them rather than one per word, for a
    // reason that outlives the parser: the fingerprint must hash a keyword
    // exactly as it hashes an identifier, or every stored pattern_id
    // containing one of these words moves and every waystone recorded
    // under it is retired (fingerprint.hpp's bump rule). With one type
    // that is one line in ShapeTagOf() which cannot be forgotten when the
    // next keyword is reserved; with seven it is seven chances to get it
    // wrong, growing with the grammar.
    kKeyword,

    // Bind-parameter placeholder: `?`. Lexed but not accepted by any
    // production - the parser rejects it exactly as it rejected the
    // kError this used to be, because there is no BIND stage in the
    // newline protocol to supply a value. It exists so fingerprinting can
    // see the placeholder (fingerprint.hpp): `WHERE id = 42` and
    // `WHERE id = ?` must reduce to one pattern_id, and a token type is
    // the only way to tell a placeholder from a lexing failure.
    kParam,

    // A named parameter: `$flag`. `Token::text` carries the name with the
    // sigil stripped, **as written** - like an identifier, and for the same
    // reason: folding is the consumer's job, so an error message can quote
    // what the client actually typed while a comparison stays case
    // insensitive.
    //
    // Accepted by exactly one production - the body of `CREATE PATTERN`
    // (docs/spec/create-pattern-user-defined-patterns-v1.md section 3.1).
    // Everywhere else the parser refuses it with a position; the token is
    // *reserved* for the extended protocol's named binds (D4), and nothing
    // wires that yet.
    //
    // A separate type from kParam rather than a spelling of it, because the
    // two differ in what the parser does with them (`?` is refused
    // everywhere, `$x` is accepted in a declared body) while agreeing in
    // what the fingerprint does with them (both are ShapeTag::kValue). One
    // type for both would collapse a grammar distinction to buy a hash
    // distinction that does not exist.
    //
    // **The sigil is what makes the feature possible at all.** Identifiers
    // are case-folded, so a parameter named `a` and an alias written
    // `AS a` would collide, and "value position" cannot disambiguate them
    // because a join predicate puts columns in value position too
    // (`ON t.id = a.id`). Removing the ambiguity at the token level is what
    // means no collision check against aliases or column names is needed
    // anywhere above.
    kNamedParam,

    // punctuation
    kLParen,
    kRParen,
    kComma,
    kSemicolon,
    kStar,

    // Qualifier separator in `a.x`. Until V04 this was a kError, so a
    // qualified name could not be tokenized at all and no statement
    // containing one had a fingerprint.
    kDot,

    // comparison operators
    kEq,
    kNeq,
    kLt,
    kLte,
    kGt,
    kGte,

    kError,  // unrecognized character
};

struct Token {
    TokenType type = TokenType::kEof;

    // The token as written, viewing the source. For a string literal this
    // is the value with the quotes stripped; for a named parameter, the
    // name without its sigil. Empty at end of input. See the lifetime rule
    // in this file's header before storing one.
    std::string_view text;
    std::int64_t int_val = 0;  // valid when type == kIntLit; see digits()
    Keyword kw = Keyword::kJoin;  // valid when type == kKeyword

    // Where the token sits in the statement text: `byte_offset` is its
    // first byte, `length` its extent as written. For a string literal
    // that includes both quotes, so a reported position covers the literal
    // a client wrote rather than the value the lexer decoded from it. At
    // end of input, `byte_offset` is the length of the input and `length`
    // is 0.
    //
    // This is what makes J2's promise - `Unsupported` with an *exact*
    // position - something the parser can keep. A message that says only
    // "derived tables are not supported" makes the client hunt for which
    // parenthesis was the problem.
    std::uint32_t byte_offset = 0;
    std::uint32_t length = 0;

    // kIntLit: whether the literal was written with a leading `-`.
    bool negative = false;

    // kIntLit: the digit run with the sign stripped, in the spelling the
    // client used.
    //
    // `int_val` is a signed decode that silently wraps past 64 bits, so it
    // cannot judge whether a literal is inside the 40-bit pk range - the
    // range check at V30 needs the digits themselves, and so does anything
    // else that must distinguish "large" from "wrapped to small". Keeping
    // both is cheaper than deciding here which one every future caller
    // wanted.
    //
    // A view into this token's own `text`, and so into the source behind
    // it - valid exactly as long as that is.
    std::string_view digits() const noexcept {
        return negative && !text.empty() ? text.substr(1) : text;
    }
};

// The reserved word `text` spells, or nullopt if it spells none. Case
// insensitive, ASCII only - the same fold the fingerprint uses, and for
// the same reason it is written out by hand there.
bool LookupKeyword(std::string_view text, Keyword& out) noexcept;

// The canonical spelling of a reserved word, for error messages.
std::string_view KeywordText(Keyword kw) noexcept;

}  // namespace kds::parser
