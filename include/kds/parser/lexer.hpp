#pragma once

#include <string_view>

#include "kds/parser/token.hpp"

// Hand-rolled lexer for the KDS SQL subset, with one-token lookahead
// (Peek()/Next()) - the parser needs it to decide things like "is there a
// WHERE clause" or "is there another column" without consuming the token
// that answers the question.

namespace kds::parser {

class Lexer {
public:
    explicit Lexer(std::string_view sql) noexcept : src_(sql), pos_(0) {}

    // Returns the next token without consuming it. Repeated calls without
    // an intervening Next() return the same token.
    const Token& Peek();

    // Consumes and returns the next token.
    Token Next();

private:
    // ReadToken() skips to the token, calls ScanToken() for it, and stamps
    // the byte range it covered. ScanToken() never sets a position: doing
    // it in one place is what guarantees every token has one.
    Token ReadToken();
    Token ScanToken();
    void SkipWhitespaceAndComments();

    std::string_view src_;
    std::size_t pos_;
    Token peeked_;
    bool has_peeked_ = false;
};

}  // namespace kds::parser
