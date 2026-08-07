#include "kds/parser/lexer.hpp"

#include <cctype>
#include <iterator>
#include <string_view>

namespace kds::parser {

namespace {

bool IsIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool IsIdentCont(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// ASCII-only, locale-independent. std::tolower consults the locale, and
// which words this lexer reserves must not depend on the locale the server
// booted in - the same argument fingerprint.cpp makes for its own fold,
// and here it decides whether a statement parses at all.
char FoldAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (FoldAscii(a[i]) != FoldAscii(b[i])) return false;
    }
    return true;
}

// The reserved-word table. One row per Keyword enumerator, in enumerator
// order, so a word added to one and not the other is caught by the
// static_assert below rather than by a silent lookup miss.
struct KeywordRow {
    std::string_view text;
    Keyword kw;
};

inline constexpr KeywordRow kKeywords[] = {
    {"JOIN", Keyword::kJoin},       {"ON", Keyword::kOn},         {"AS", Keyword::kAs},
    {"IN", Keyword::kIn},           {"EXISTS", Keyword::kExists}, {"NOT", Keyword::kNot},
    {"BETWEEN", Keyword::kBetween}, {"LEFT", Keyword::kLeft},     {"RIGHT", Keyword::kRight},
    {"FULL", Keyword::kFull},       {"OUTER", Keyword::kOuter},
};

static_assert(std::size(kKeywords) == static_cast<std::size_t>(Keyword::kOuter) + 1,
              "every Keyword enumerator needs a row in kKeywords, or it can never be lexed");

}  // namespace

bool LookupKeyword(std::string_view text, Keyword& out) noexcept {
    for (const KeywordRow& row : kKeywords) {
        if (IEquals(text, row.text)) {
            out = row.kw;
            return true;
        }
    }
    return false;
}

std::string_view KeywordText(Keyword kw) noexcept {
    for (const KeywordRow& row : kKeywords) {
        if (row.kw == kw) return row.text;
    }
    return "<keyword>";
}

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

    // Position is stamped here, around ScanToken(), rather than inside its
    // dozen returns. Every token carries an exact extent and a token type
    // added later cannot forget to set one - which matters because J2's
    // "Unsupported with an exact position" is only as good as its worst
    // branch. Whitespace and comments are already consumed, so `start` is
    // the token's own first byte; at end of input the extent is zero.
    const std::size_t start = pos_;
    Token tok = ScanToken();
    tok.byte_offset = static_cast<std::uint32_t>(start);
    tok.length = static_cast<std::uint32_t>(pos_ - start);
    // Every token, exactly once, in order - which is the whole contract the
    // fingerprint is defined over (fingerprint.hpp). This is the only place
    // a token comes into existence, so it is the only place that contract
    // can be kept without the caller's cooperation.
    fingerprint_.Feed(tok);
    return tok;
}

Token Lexer::ScanToken() {
    Token tok;

    if (pos_ >= src_.size()) {
        tok.type = TokenType::kEof;
        return tok;
    }

    char c = src_[pos_];

    // String literal: 'value'. No escaping, deliberately - a string
    // literal cannot contain a quote.
    if (c == '\'') {
        ++pos_;
        std::size_t start = pos_;
        while (pos_ < src_.size() && src_[pos_] != '\'') ++pos_;
        tok.text = src_.substr(start, pos_ - start);
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

        // A '.' with a digit after it extends this into one numeric
        // literal: `12.34` is a kNumLit, not int-dot-int. The digit
        // requirement on *both* sides is what keeps everything else where
        // it was: `12.` stays an integer followed by a dot, `.5` never
        // reaches here, and `a.b` has no digits at all. No statement that
        // parsed before this token existed contains the fused sequence, so
        // no stored pattern_id moves (see the analysis in fingerprint.cpp).
        if (pos_ + 1 < src_.size() && src_[pos_] == '.' &&
            std::isdigit(static_cast<unsigned char>(src_[pos_ + 1]))) {
            ++pos_;  // the point
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                ++pos_;
            }
            tok.text = src_.substr(start, pos_ - start);
            tok.type = TokenType::kNumLit;
            // `int_val` and `negative` stay unset: the spelling is the
            // value here, and every consumer reads `text`.
            return tok;
        }

        tok.text = src_.substr(start, pos_ - start);
        tok.type = TokenType::kIntLit;

        tok.negative = !tok.text.empty() && tok.text[0] == '-';
        // Wraps past 64 bits, and always did. The digits survive on the
        // token (Token::digits()) precisely because this number cannot be
        // trusted to answer a range question - see the note there.
        std::int64_t v = 0;
        for (char d : tok.digits()) {
            v = v * 10 + (d - '0');
        }
        tok.int_val = tok.negative ? -v : v;
        return tok;
    }

    // Identifier, reserved word, or NULL.
    if (IsIdentStart(c)) {
        std::size_t start = pos_;
        while (pos_ < src_.size() && IsIdentCont(src_[pos_])) ++pos_;
        tok.text = src_.substr(start, pos_ - start);
        if (IEquals(tok.text, "NULL")) {
            tok.type = TokenType::kNullLit;
        } else if (LookupKeyword(tok.text, tok.kw)) {
            // Reserved: it can no longer be a column or table name. The
            // text is kept as written, so the fingerprint hashes it the
            // same way it hashes an identifier and error messages quote
            // what the client actually typed.
            tok.type = TokenType::kKeyword;
        } else {
            tok.type = TokenType::kIdent;
        }
        return tok;
    }

    // Named parameter: `$flag`. The sigil is consumed and the name is kept
    // as written, so `Token::text` reads exactly like an identifier's -
    // which is what lets the parser compare declared against used with the
    // same case-insensitive fold it uses everywhere else.
    //
    // A `$` with no identifier after it falls through to the switch below
    // and lexes as kError, the same answer it gave before this token
    // existed. There is no such thing as an anonymous named parameter, and
    // reporting the bad character is more use than inventing an empty name.
    if (c == '$' && pos_ + 1 < src_.size() && IsIdentStart(src_[pos_ + 1])) {
        ++pos_;  // the sigil
        std::size_t start = pos_;
        while (pos_ < src_.size() && IsIdentCont(src_[pos_])) ++pos_;
        tok.text = src_.substr(start, pos_ - start);
        tok.type = TokenType::kNamedParam;
        return tok;
    }

    // Two-character operators.
    if (c == '!' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '=') {
        tok.type = TokenType::kNeq;
        tok.text = src_.substr(pos_, 2);
        pos_ += 2;
        return tok;
    }
    if (c == '<' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '=') {
        tok.type = TokenType::kLte;
        tok.text = src_.substr(pos_, 2);
        pos_ += 2;
        return tok;
    }
    if (c == '>' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '=') {
        tok.type = TokenType::kGte;
        tok.text = src_.substr(pos_, 2);
        pos_ += 2;
        return tok;
    }

    // Single-character tokens. Sliced before `pos_` advances, so this is a
    // view into the source like every other token's text - no exception to
    // reason about later.
    tok.text = src_.substr(pos_, 1);
    ++pos_;
    switch (c) {
        case '(': tok.type = TokenType::kLParen; break;
        case ')': tok.type = TokenType::kRParen; break;
        case ',': tok.type = TokenType::kComma; break;
        case ';': tok.type = TokenType::kSemicolon; break;
        case '*': tok.type = TokenType::kStar; break;
        // Reached only when the '.' neither follows a digit run nor starts
        // a numeric literal: `1.5` is consumed whole as a kNumLit above,
        // and `12.` leaves its dot here. So a '.' here is a qualifier
        // separator, or a syntax error the parser will report.
        case '.': tok.type = TokenType::kDot; break;
        case '=': tok.type = TokenType::kEq; break;
        case '<': tok.type = TokenType::kLt; break;
        case '>': tok.type = TokenType::kGt; break;
        case '?': tok.type = TokenType::kParam; break;
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
