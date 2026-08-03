#pragma once

#include <optional>
#include <string_view>

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
// to lex twice. `FingerprintOf()` used to be that second lex, and on the
// statement path it cost more than the work it enabled - measured at ~13%
// of a point join's latency, three times the cost of the recording it was
// for (bench/results-waystone-v2.md).
//
// **The hook is in ReadToken(), not in Next().** `Peek()` caches, so a
// peeked token is produced once and consumed later; hooking the consumer
// would hash a lookahead token twice and hash nothing for a token that was
// peeked and never consumed. Producing is the event that happens exactly
// once per token, in order, which is precisely the stream the fingerprint
// is defined over.

namespace kds::parser {

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
