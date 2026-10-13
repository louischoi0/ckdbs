#pragma once

#include <cstdint>
#include <string_view>

// Lexer token types.
//
// ---- A token owns nothing: `text` is a view into the SQL ----------------
//
// `Token::text` is a `std::string_view` **into the statement the lexer was
// given**, not a copy of it. A `Token` is therefore trivially copyable and
// costs nothing to move around, and lexing a statement allocates nothing.
//
// **The rule that makes this safe: a token must not outlive the SQL it was
// lexed from.** Tokens never escape a parse - the AST copies every name and
// value it keeps into its own `std::string` (docs/spec/parser-v2.md I4), so
// a `Statement` is self-contained once `Parse()` returns. Anything that
// keeps `Token`s past the source is a use-after-free nothing here catches.

namespace kds::parser {

// Words the grammar reserves (docs/inflight/in-progress/parser-v2-workplan.md V04). Reserved
// means a statement cannot use one as a column or table name - `SELECT *
// FROM t WHERE in = 1` stops parsing here rather than reading `in` as an
// identifier.
//
// The list is exactly what the v2 grammar needs to *see*; a reserved word
// the grammar cannot execute is refused with its position rather than
// misread (the outer-join words below).
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

    // Reserved and refused (V05): an outer join answers `NotImplemented`
    // with the keyword's own position (spec I9), and the grammar will not
    // shift if they land. Free to the fingerprint: a keyword hashes exactly
    // as an identifier does (one token type, below).
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
    // One token type for all of them: the fingerprint must hash a keyword
    // exactly as an identifier, or every stored pattern_id containing one
    // moves and its waystones are retired (fingerprint.hpp's bump rule).
    // One type makes that one line in ShapeTagOf() for every keyword.
    kKeyword,

    // Bind-parameter placeholder: `?`. Lexed so the fingerprint can see it
    // (`WHERE id = 42` and `WHERE id = ?` reduce to one pattern_id).
    kParam,

    // A named parameter: `$flag`. `Token::text` carries the name with the
    // sigil stripped, **as written** - like an identifier, and for the same
    // reason: folding is the consumer's job, so an error message can quote
    // what the client actually typed while a comparison stays case
    // insensitive.
    //
    // Accepted by **no** production: the parser refuses `$x` with a
    // position wherever it is written. Background: the body of the
    // withdrawn `CREATE PATTERN` accepted it until 2026-08-31
    // (docs/spec/create-pattern-user-defined-patterns-v1.md). The token
    // stays reserved for the extended protocol's named binds (D4), which
    // nothing wires yet.
    //
    // A separate type from kParam: both fingerprint as ShapeTag::kValue,
    // but the refusal message differs (`?` is a placeholder, `$x` a name).
    // The sigil keeps a parameter name from colliding with a case-folded
    // alias or column, which "value position" cannot disambiguate
    // (`ON t.id = a.id`).
    kNamedParam,

    // punctuation
    kLParen,
    kRParen,
    kComma,
    kSemicolon,
    kStar,

    // Qualifier separator in `a.x` (V04; a kError before, so no stored
    // pattern contains one).
    kDot,

    // comparison operators
    kEq,
    kNeq,
    kLt,
    kLte,
    kGt,
    kGte,

    // Arithmetic and string operators (BJ-S2), the tokens of a SET value
    // expression. Before BJ each lexed as kError, so no statement
    // containing one had a fingerprint and none is stored. A `-` before a
    // digit is **not** kMinus: it is part of the signed literal, as it
    // always was (BJ-Q9 (a)), and the parser reads it as a minus in binary
    // position. `*` is kStar in every position; the parser decides which
    // it is.
    kPlus,
    kMinus,
    kSlash,
    kPercent,
    kConcat,  // `||`
    // The reference dialect's cast (BJ-S5): `x::int`. Lexed as kError until
    // then, so no statement containing one had a fingerprint.
    kDoubleColon,

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
    // This is what lets every refusal carry an exact position (J2).
    std::uint32_t byte_offset = 0;
    std::uint32_t length = 0;

    // kIntLit: whether the literal was written with a leading `-`.
    bool negative = false;

    // kIntLit: the digit run with the sign stripped, in the spelling the
    // client used.
    //
    // `int_val` is a signed decode that silently wraps past 64 bits, so a
    // range check (the pk's id space, V30) needs the digits themselves.
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
