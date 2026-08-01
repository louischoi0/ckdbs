#include "kds/parser/fingerprint.hpp"

#include "kds/parser/lexer.hpp"
#include "kds/parser/token.hpp"

namespace kds::parser {

namespace {

// FNV-1a, 64-bit. Chosen for what it does *not* do: no seed, no
// randomization, no dependence on word size or endianness (it consumes one
// byte at a time), and no library whose implementation may change under
// us. std::hash is disqualified outright - its output is not required to
// be stable across runs, let alone across builds, and this value goes on
// disk.
inline constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ull;

class Fnv1a {
public:
    void Byte(std::uint8_t b) noexcept {
        state_ ^= b;
        state_ *= kFnvPrime;
    }

    void Bytes(std::string_view s) noexcept {
        for (char c : s) Byte(static_cast<std::uint8_t>(c));
    }

    // Length-prefixed, so adjacent fields cannot bleed into each other:
    // without it the shapes `a bc` and `ab c` would hash identically once
    // the space between identifiers is gone. The separator has to be part
    // of the hashed stream, not part of the caller's discipline.
    void Field(std::string_view s) noexcept {
        Byte(static_cast<std::uint8_t>(s.size() & 0xFF));
        Byte(static_cast<std::uint8_t>((s.size() >> 8) & 0xFF));
        Bytes(s);
    }

    std::uint64_t value() const noexcept { return state_; }

private:
    std::uint64_t state_ = kFnvOffsetBasis;
};

// Tags fed into the shape stream. The values are arbitrary, but changing
// one changes every pattern_id this build produces - so a change here is a
// format change and requires bumping `kFingerprintVersion`, which retires
// every stored pattern rather than letting a new hash resolve an old
// trail. One tag per token type the shape can contain, plus the two
// markers below.
enum class ShapeTag : std::uint8_t {
    kIdent = 1,
    kValue = 2,  // any bindable literal, and `?` - see kValue's note below
    kNull = 3,
    kLParen = 4,
    kRParen = 5,
    kComma = 6,
    kStar = 7,
    kEq = 8,
    kNeq = 9,
    kLt = 10,
    kLte = 11,
    kGt = 12,
    kGte = 13,
    // Appended by V04 with the `.` token. Appending is free: no statement
    // that hashes today contains a dot, because a dot was a kError and
    // kError returns nullopt. Statements with qualified names go from
    // "no fingerprint" to "a fingerprint", which the bump rule in
    // fingerprint.hpp names explicitly as the case that does *not* need a
    // version bump - nothing already stored changes meaning.
    kDot = 14,
};

// Tags for the argument stream, distinguishing the literal's type. The
// shape deliberately cannot tell an int hole from a string hole (a bind
// parameter's type is unknown at parse); the argument stream can and must,
// or `= 1` and `= '1'` would share an arg_hash. Versioned exactly like the
// shape tags above.
enum class ArgTag : std::uint8_t {
    kInt = 1,
    kStr = 2,
};

// ASCII-only lower-casing. Not std::tolower: that consults the locale, and
// a hash that depends on the server's locale is not a stable key.
char FoldAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Whether a statement whose first word is `word` has a pattern at all.
// Everything else - CREATE, SET, SHOW, DESCRIBE, SYNC, and any word this
// grammar does not know - reduces to nullopt.
//
// A list rather than a "not DDL" check: the safe default for an unknown
// leading keyword is *not patternable*, and an allow-list is the only
// shape that gets that right without being updated.
bool IsPatternableLeadingWord(std::string_view folded) noexcept {
    return folded == "select" || folded == "insert" || folded == "update";
}

bool ShapeTagOf(TokenType type, ShapeTag& out) noexcept {
    switch (type) {
        // A reserved word hashes as an identifier, tag and all. This is
        // not a convenience: before V04 reserved these seven words they
        // *were* identifiers to this lexer, so any other tag would move
        // the pattern_id of every statement containing `IN`, `EXISTS`,
        // `AS`, `NOT`, `BETWEEN`, `JOIN` or `ON` - a format break retiring
        // every waystone stored under one, for a lexer change that altered
        // no statement's meaning. The same holds for every keyword
        // reserved after this one, which is why they share a token type.
        case TokenType::kKeyword:
        case TokenType::kIdent: out = ShapeTag::kIdent; return true;
        // The convergence point: an int literal, a string literal and a
        // `?` all emit the same marker, which is what makes
        // `WHERE id = 42` and `WHERE id = ?` one pattern.
        case TokenType::kIntLit:
        case TokenType::kStrLit:
        case TokenType::kParam: out = ShapeTag::kValue; return true;
        case TokenType::kNullLit: out = ShapeTag::kNull; return true;
        case TokenType::kLParen: out = ShapeTag::kLParen; return true;
        case TokenType::kRParen: out = ShapeTag::kRParen; return true;
        case TokenType::kComma: out = ShapeTag::kComma; return true;
        case TokenType::kStar: out = ShapeTag::kStar; return true;
        case TokenType::kDot: out = ShapeTag::kDot; return true;
        case TokenType::kEq: out = ShapeTag::kEq; return true;
        case TokenType::kNeq: out = ShapeTag::kNeq; return true;
        case TokenType::kLt: out = ShapeTag::kLt; return true;
        case TokenType::kLte: out = ShapeTag::kLte; return true;
        case TokenType::kGt: out = ShapeTag::kGt; return true;
        case TokenType::kGte: out = ShapeTag::kGte; return true;
        // Not reachable: kSemicolon is skipped and kEof ends the walk
        // before either gets here, and kError has already returned.
        case TokenType::kSemicolon:
        case TokenType::kEof:
        case TokenType::kError: return false;
    }
    return false;
}

}  // namespace

std::optional<Fingerprint> FingerprintOf(std::string_view sql) {
    Lexer lexer(sql);
    Fnv1a shape;
    Fnv1a args;
    Fingerprint out;

    // The leading word decides patternability, and it is the first thing
    // hashed, so two statement kinds can never share a shape prefix.
    //
    // kIdent only: the three patternable words are unreserved, so a
    // statement opening with a reserved word (`NOT …`) is not patternable
    // - which is the same answer it got when that word lexed as an
    // identifier and failed the allow-list below.
    Token first = lexer.Next();
    if (first.type != TokenType::kIdent) return std::nullopt;

    std::string folded;
    folded.reserve(first.text.size());
    for (char c : first.text) folded.push_back(FoldAscii(c));
    if (!IsPatternableLeadingWord(folded)) return std::nullopt;

    shape.Byte(static_cast<std::uint8_t>(ShapeTag::kIdent));
    shape.Field(folded);

    for (;;) {
        Token tok = lexer.Next();
        if (tok.type == TokenType::kEof) break;
        // A statement that will not lex has no shape worth storing.
        if (tok.type == TokenType::kError) return std::nullopt;
        // Skipped, not tagged: `SELECT * FROM t;` and `SELECT * FROM t`
        // are one pattern, and the parser treats the semicolon as
        // optional too.
        if (tok.type == TokenType::kSemicolon) continue;

        ShapeTag tag;
        if (!ShapeTagOf(tok.type, tag)) return std::nullopt;
        shape.Byte(static_cast<std::uint8_t>(tag));

        switch (tok.type) {
            // Both hash their folded text after the shared kIdent tag -
            // see ShapeTagOf(). A keyword falling through to `default`
            // here would drop its text and collapse `WHERE id IN (…)` and
            // `WHERE id AS (…)` onto one shape, as well as moving both.
            case TokenType::kKeyword:
            case TokenType::kIdent: {
                folded.clear();
                for (char c : tok.text) folded.push_back(FoldAscii(c));
                shape.Field(folded);
                break;
            }
            case TokenType::kIntLit:
                args.Byte(static_cast<std::uint8_t>(ArgTag::kInt));
                args.Field(tok.text);
                ++out.literal_count;
                break;
            case TokenType::kStrLit:
                args.Byte(static_cast<std::uint8_t>(ArgTag::kStr));
                args.Field(tok.text);
                ++out.literal_count;
                break;
            case TokenType::kParam:
                // Contributes to the shape and nothing else. Its value
                // arrives at BIND, which is where arg_hash is completed.
                ++out.param_count;
                break;
            default:
                // Operators, punctuation and NULL are fully described by
                // their tag; there is nothing further to hash.
                break;
        }
    }

    out.pattern_id = shape.value();
    out.arg_hash = args.value();
    return out;
}

}  // namespace kds::parser
