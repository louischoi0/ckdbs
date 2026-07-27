#pragma once

#include <cstdint>
#include <string>

// Lexer token types, ported from the legacy kernel engine's
// kds_token_type_t/kds_token_t (kds_parser.h). No fixed-capacity text
// buffer here (legacy used a KDS_PARSER_VAL_MAX char array) - a
// std::string has no arbitrary length cap to get wrong.

namespace kds::parser {

enum class TokenType {
    kEof,

    // literals
    kIdent,    // table/column name, keyword
    kIntLit,   // 42, -7
    kStrLit,   // 'hello' (quotes stripped)
    kNullLit,  // NULL

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
