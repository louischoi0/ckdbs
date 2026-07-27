#include "kds/parser/lexer.hpp"

#include <cctype>

namespace kds::parser {

namespace {

bool IsIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool IsIdentCont(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

void Lexer::SkipWhitespaceAndComments() {
    for (;;) {
        while (pos_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[pos_]))) {
            ++pos_;
        }

        // "--" line comment (SQL standard).
        if (pos_ + 1 < src_.size() && src_[pos_] == '-' && src_[pos_ + 1] == '-') {
            while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
            continue;
        }
        break;
    }
}

Token Lexer::ReadToken() {
    SkipWhitespaceAndComments();

    Token tok;

    if (pos_ >= src_.size()) {
        tok.type = TokenType::kEof;
        return tok;
    }

    char c = src_[pos_];

    // String literal: 'value'. No escaping - matches the legacy engine's
    // deliberate limitation (KDS-SQL string literals can't contain '\'').
    if (c == '\'') {
        ++pos_;
        std::size_t start = pos_;
        while (pos_ < src_.size() && src_[pos_] != '\'') ++pos_;
        tok.text = std::string(src_.substr(start, pos_ - start));
        tok.type = TokenType::kStrLit;
        if (pos_ < src_.size() && src_[pos_] == '\'') ++pos_;  // consume closing quote
        return tok;
    }

    // Integer literal (optional leading minus).
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '-' && pos_ + 1 < src_.size() &&
         std::isdigit(static_cast<unsigned char>(src_[pos_ + 1])))) {
        std::size_t start = pos_;
        if (src_[pos_] == '-') ++pos_;
        while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
            ++pos_;
        }
        tok.text = std::string(src_.substr(start, pos_ - start));
        tok.type = TokenType::kIntLit;

        bool neg = !tok.text.empty() && tok.text[0] == '-';
        std::int64_t v = 0;
        for (std::size_t i = neg ? 1 : 0; i < tok.text.size(); ++i) {
            v = v * 10 + (tok.text[i] - '0');
        }
        tok.int_val = neg ? -v : v;
        return tok;
    }

    // Identifier or keyword.
    if (IsIdentStart(c)) {
        std::size_t start = pos_;
        while (pos_ < src_.size() && IsIdentCont(src_[pos_])) ++pos_;
        tok.text = std::string(src_.substr(start, pos_ - start));
        tok.type = IEquals(tok.text, "NULL") ? TokenType::kNullLit : TokenType::kIdent;
        return tok;
    }

    // Two-character operators.
    if (c == '!' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '=') {
        tok.type = TokenType::kNeq;
        tok.text = "!=";
        pos_ += 2;
        return tok;
    }
    if (c == '<' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '=') {
        tok.type = TokenType::kLte;
        tok.text = "<=";
        pos_ += 2;
        return tok;
    }
    if (c == '>' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '=') {
        tok.type = TokenType::kGte;
        tok.text = ">=";
        pos_ += 2;
        return tok;
    }

    // Single-character tokens.
    tok.text = std::string(1, c);
    ++pos_;
    switch (c) {
        case '(': tok.type = TokenType::kLParen; break;
        case ')': tok.type = TokenType::kRParen; break;
        case ',': tok.type = TokenType::kComma; break;
        case ';': tok.type = TokenType::kSemicolon; break;
        case '*': tok.type = TokenType::kStar; break;
        case '=': tok.type = TokenType::kEq; break;
        case '<': tok.type = TokenType::kLt; break;
        case '>': tok.type = TokenType::kGt; break;
        default: tok.type = TokenType::kError; break;
    }
    return tok;
}

const Token& Lexer::Peek() {
    if (!has_peeked_) {
        peeked_ = ReadToken();
        has_peeked_ = true;
    }
    return peeked_;
}

Token Lexer::Next() {
    if (has_peeked_) {
        has_peeked_ = false;
        return std::move(peeked_);
    }
    return ReadToken();
}

}  // namespace kds::parser
