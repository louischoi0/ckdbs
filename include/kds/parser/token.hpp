#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Lexer token types. Token text is a std::string rather than a
// fixed-capacity buffer, so there is no arbitrary length cap to size
// wrong.

namespace kds::parser {

// Words the grammar reserves (docs/parser-v2-workplan.md V04). Reserved
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
    std::string text;          // raw token text, as written
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
    // A view into this token's own `text`, valid as long as the token is.
    std::string_view digits() const noexcept {
        std::string_view all(text);
        return negative && !all.empty() ? all.substr(1) : all;
    }
};

// The reserved word `text` spells, or nullopt if it spells none. Case
// insensitive, ASCII only - the same fold the fingerprint uses, and for
// the same reason it is written out by hand there.
bool LookupKeyword(std::string_view text, Keyword& out) noexcept;

// The canonical spelling of a reserved word, for error messages.
std::string_view KeywordText(Keyword kw) noexcept;

}  // namespace kds::parser
