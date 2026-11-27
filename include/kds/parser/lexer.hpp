#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "kds/parser/fingerprint.hpp"
#include "kds/parser/token.hpp"

// Hand-rolled lexer for the KDS SQL subset, with one-token lookahead
// (Peek()/Next()) - the parser needs it to decide things like "is there a
// WHERE clause" or "is there another column" without consuming the token
// that answers the question.
//
// ---- It fingerprints as it lexes ----------------------------------------
//
// Every token produced is fed to a `FingerprintAccumulator`, so a parse
// yields the statement's `pattern_id`/`arg_hash` for free and nothing has
// to lex twice (a separate `FingerprintOf()` lex once cost ~13% of a point
// join's latency, bench/results-waystone-v2.md at `1769487`).
//
// **The hook is in ReadToken(), not in Next().** `Peek()` caches, so a
// peeked token is produced once and consumed later; hooking the consumer
// would hash a lookahead token twice and hash nothing for a token that was
// peeked and never consumed. Producing is the event that happens exactly
// once per token, in order, which is precisely the stream the fingerprint
// is defined over.

namespace kds::parser {

// The identifier rule every name the lexer reads follows: a letter or `_`,
// then letters, digits and `_`. Exposed for the session statements routed
// before the parser (`USE`), so a name they take is one a statement can write.
bool IsIdentStart(char c) noexcept;
bool IsIdentCont(char c) noexcept;
bool IsIdentifier(std::string_view text) noexcept;

// A statement's **shape as text**: `sql` with every value literal written
// `?`, a run of white space or a comment between two tokens written as one
// space, and a trailing semicolon dropped - the text form of what
// `Fingerprint::pattern_id` hashes, so two statements of one shape give one
// text up to the case and spelling of their identifiers, which are kept as
// written. For a reader, and for a catalog row that must not hold a client's
// values (`sys.plans`); nothing compares it. Text the lexer cannot read is
// returned as far as it could be read.
std::string ShapeText(std::string_view sql);

// A statement's **arguments**: its value positions in token order - every
// integer, numeric and string literal and every `?`, the tokens `ShapeText`
// writes `?` and the fingerprint counts (`Fingerprint::literal_count` and
// `param_count`); NULL is shape and is not one. What a managed plan's
// `ARG(k)` reads, 1-based (`exec/plan_rule.hpp`). `text` views `sql`: a
// string's value with its quotes stripped, a number as written. A numeric
// literal with a point is a string, as it is everywhere (`token.hpp`).
// Text the lexer cannot read gives the arguments before the unreadable
// byte.
struct StatementArgument {
    enum class Kind : std::uint8_t { kInt, kStr, kUnbound };
    Kind kind = Kind::kUnbound;
    std::string_view text;
};
std::vector<StatementArgument> ArgumentsOf(std::string_view sql);

// The `Fingerprint::arg_hash` of an argument stream given whole: for the
// arguments `ArgumentsOf(sql)` answers, the `arg_hash` the lexer folds while
// it reads `sql` (`fingerprint_test.cpp` pins the two equal over the golden
// corpus). A `kUnbound` argument folds nothing, as a `?` does. What a kept
// parse's bind computes its hash with, its holes' values in place
// (`kept_parse.hpp`), so a bound statement's key is its substituted text's.
std::uint64_t ArgumentHashOf(std::span<const StatementArgument> args) noexcept;

class Lexer {
public:
    explicit Lexer(std::string_view sql) noexcept : src_(sql), pos_(0) {}

    // Returns the next token without consuming it. Repeated calls without
    // an intervening Next() return the same token.
    const Token& Peek();

    // Consumes and returns the next token.
    Token Next();

    // The fingerprint of every token produced so far, or nullopt when the
    // stream is not a pattern or has not reached end of input.
    //
    // Meaningful only once the whole statement has been read - which for a
    // successful parse is guaranteed, since the parser checks that only EOF
    // follows a complete statement. A parse that stopped early leaves this
    // nullopt rather than returning a hash of the prefix it managed.
    std::optional<Fingerprint> fingerprint() const noexcept {
        return fingerprint_.Result();
    }

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
    FingerprintAccumulator fingerprint_;
};

}  // namespace kds::parser
