#pragma once

#include <cstdint>
#include <string>

// Lexer token types. Token text is a std::string rather than a
// fixed-capacity buffer, so there is no arbitrary length cap to size
// wrong.

namespace kds::parser {

enum class TokenType {
    kEof,

    // literals
    kIdent,    // table/column name, keyword
    kIntLit,   // 42, -7
    kStrLit,   // 'hello' (quotes stripped)
    kNullLit,  // NULL

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
    std::string text;              // raw token text
    std::int64_t int_val = 0;      // valid when type == kIntLit
};

}  // namespace kds::parser
