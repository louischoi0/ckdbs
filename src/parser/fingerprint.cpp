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

// ASCII-only lower-casing. Not std::tolower: that consults the locale, and
// a hash that depends on the server's locale is not a stable key.
constexpr char FoldAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

class Fnv1a {
public:
    Fnv1a() = default;
    // Resumes from a state the caller is carrying. The accumulator holds
    // its two states as plain integers - it is a header type on the hot
    // path - and borrows this for each update.
    explicit Fnv1a(std::uint64_t state) noexcept : state_(state) {}

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

    // The same field, case-folded as it is hashed.
    //
    // **Identical bytes to `Field(fold(s))`** - the fold is one-to-one on
    // bytes so the length prefix does not move - but with no buffer to
    // build it in. That matters here and nowhere else: this runs once per
    // identifier and keyword of every statement, and the `std::string` it
    // replaces was an allocation per token past the small-string limit,
    // which would have quietly undone zero-copy tokens for exactly the
    // tokens that motivated them.
    void FoldedField(std::string_view s) noexcept {
        Byte(static_cast<std::uint8_t>(s.size() & 0xFF));
        Byte(static_cast<std::uint8_t>((s.size() >> 8) & 0xFF));
        for (char c : s) Byte(static_cast<std::uint8_t>(FoldAscii(c)));
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

// Whether a statement whose first word is `word` has a pattern at all.
// Everything else - CREATE, SET, SHOW, DESCRIBE, SYNC, and any word this
// grammar does not know - reduces to nullopt.
//
// A list rather than a "not DDL" check: the safe default for an unknown
// leading keyword is *not patternable*, and an allow-list is the only
// shape that gets that right without being updated.
bool IsPatternableLeadingWord(std::string_view word) noexcept {
    // Folded as it compares, so the caller needs no buffer either.
    auto is = [word](std::string_view lower) {
        if (word.size() != lower.size()) return false;
        for (std::size_t i = 0; i < word.size(); ++i) {
            if (FoldAscii(word[i]) != lower[i]) return false;
        }
        return true;
    };
    return is("select") || is("insert") || is("update");
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
        // The convergence point: an int literal, a string literal, a `?`
        // and a declared `$param` all emit the same marker, which is what
        // makes `WHERE id = 42`, `WHERE id = ?` and `WHERE id = $x` one
        // pattern.
        //
        // The last of those is the whole of CREATE PATTERN
        // (docs/spec/create-pattern-user-defined-patterns-v1.md section
        // 3.2). A declaration's body never runs; live traffic does, and it
        // carries no declaration. So if a `$param` hashed as anything else
        // - an identifier, or a marker of its own - a declared pattern
        // would match nothing that ever executes and the feature would be
        // silently dead. Neither the parameter's *name* nor its declared
        // *type* enters the stream here, for the same reason: live traffic
        // has neither to contribute.
        // kNumLit is in the group by the phase-2 rule (types.md TY3):
        // a bare `12.34` *is* the quoted `'12.34'`, so it must land where
        // every other bindable literal lands or the two spellings of one
        // statement would grow two pattern_ids.
        //
        // The fingerprint analysis that gated this token, recorded where it
        // is load-bearing: `12.34` lexed *before* this token existed - as
        // kIntLit, kDot, kIntLit, all valid - so a statement containing it
        // was fingerprintable, and fusing the three tokens moves that
        // statement's hash. `kFingerprintVersion` stays 1 anyway, because
        // the bump rule protects what is *stored*, and no such hash was
        // ever storable: int-dot-int parses in no production, a statement
        // that cannot parse cannot execute, recording happens only on the
        // execution path, and a CREATE PATTERN body must itself parse. A
        // hash that can never reach `sys.patterns` has no on-disk meaning
        // to preserve.
        case TokenType::kIntLit:
        case TokenType::kNumLit:
        case TokenType::kStrLit:
        case TokenType::kParam:
        case TokenType::kNamedParam: out = ShapeTag::kValue; return true;
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

// ---- The accumulator: the one implementation of the rules above ---------

void FingerprintAccumulator::Reset() noexcept {
    shape_ = kFnvOffsetBasis;
    args_ = kFnvOffsetBasis;
    literal_count_ = 0;
    param_count_ = 0;
    started_ = false;
    valid_ = false;
    complete_ = false;
    insert_head_ = false;
    first_group_closed_ = false;
    paren_depth_ = 0;
}

void FingerprintAccumulator::Feed(const Token& tok) noexcept {
    if (complete_) return;

    if (!started_) {
        started_ = true;
        shape_ = kFnvOffsetBasis;
        args_ = kFnvOffsetBasis;

        // The leading word decides patternability, and it is the first
        // thing hashed, so two statement kinds can never share a shape
        // prefix.
        //
        // kIdent only: the three patternable words are unreserved, so a
        // statement opening with a reserved word (`NOT …`) is not
        // patternable - which is the same answer it got when that word
        // lexed as an identifier and failed the allow-list.
        if (tok.type != TokenType::kIdent) {
            complete_ = tok.type == TokenType::kEof;
            return;
        }
        if (!IsPatternableLeadingWord(tok.text)) return;

        // Remembered for BI5's suppression below. Folded compare, like the
        // allow-list's own.
        insert_head_ = tok.text.size() == 6 && FoldAscii(tok.text[0]) == 'i' &&
                       FoldAscii(tok.text[1]) == 'n' && FoldAscii(tok.text[2]) == 's' &&
                       FoldAscii(tok.text[3]) == 'e' && FoldAscii(tok.text[4]) == 'r' &&
                       FoldAscii(tok.text[5]) == 't';

        valid_ = true;
        Fnv1a shape(shape_);
        shape.Byte(static_cast<std::uint8_t>(ShapeTag::kIdent));
        shape.FoldedField(tok.text);
        shape_ = shape.value();
        return;
    }

    if (tok.type == TokenType::kEof) {
        complete_ = true;
        return;
    }
    if (!valid_) return;  // already decided this stream has no pattern

    // A statement that will not lex has no shape worth storing.
    if (tok.type == TokenType::kError) {
        valid_ = false;
        return;
    }
    // Skipped, not tagged: `SELECT * FROM t;` and `SELECT * FROM t` are one
    // pattern, and the parser treats the semicolon as optional too.
    if (tok.type == TokenType::kSemicolon) return;

    // ---- BI5: an INSERT's shape is its first row's ----------------------
    //
    // Past the first top-level paren group of an INSERT-headed stream,
    // values keep folding into arg_hash - they are arguments wherever they
    // sit - and everything else folds nothing, so `VALUES (1)` and
    // `VALUES (1), (2)` share a pattern_id and row count never fragments
    // sys.patterns (docs/spec/bulkinsert.md §2.4). Every hash this moves
    // belongs to a statement that parsed in no production - fingerprintable
    // but never storable - which is the fingerprint bump rule's permitted
    // second transition (the corpus header carries the argument).
    if (first_group_closed_) {
        switch (tok.type) {
            case TokenType::kIntLit: {
                Fnv1a args(args_);
                args.Byte(static_cast<std::uint8_t>(ArgTag::kInt));
                args.Field(tok.text);
                args_ = args.value();
                ++literal_count_;
                return;
            }
            case TokenType::kNumLit:
            case TokenType::kStrLit: {
                Fnv1a args(args_);
                args.Byte(static_cast<std::uint8_t>(ArgTag::kStr));
                args.Field(tok.text);
                args_ = args.value();
                ++literal_count_;
                return;
            }
            case TokenType::kParam:
            case TokenType::kNamedParam:
                ++param_count_;
                return;
            default:
                return;
        }
    }
    if (insert_head_) {
        if (tok.type == TokenType::kLParen) {
            ++paren_depth_;
        } else if (tok.type == TokenType::kRParen && paren_depth_ > 0 && --paren_depth_ == 0) {
            first_group_closed_ = true;
        }
    }

    ShapeTag tag;
    if (!ShapeTagOf(tok.type, tag)) {
        valid_ = false;
        return;
    }

    Fnv1a shape(shape_);
    shape.Byte(static_cast<std::uint8_t>(tag));

    switch (tok.type) {
        // Both hash their folded text after the shared kIdent tag - see
        // ShapeTagOf(). A keyword falling through to `default` here would
        // drop its text and collapse `WHERE id IN (…)` and `WHERE id AS (…)`
        // onto one shape, as well as moving both.
        case TokenType::kKeyword:
        case TokenType::kIdent:
            shape.FoldedField(tok.text);
            break;
        case TokenType::kIntLit: {
            Fnv1a args(args_);
            args.Byte(static_cast<std::uint8_t>(ArgTag::kInt));
            args.Field(tok.text);
            args_ = args.value();
            ++literal_count_;
            break;
        }
        // One arm for both, and the sharing is the semantics: a bare
        // numeric is the quoted string of its spelling (token.hpp), and
        // `tok.text` carries the same characters for `12.34` and `'12.34'`
        // (the quotes are stripped at lexing). Identical tag, identical
        // field, identical arg_hash - the two spellings share a waystone
        // because they are one statement, not because they collide.
        case TokenType::kNumLit:
        case TokenType::kStrLit: {
            Fnv1a args(args_);
            args.Byte(static_cast<std::uint8_t>(ArgTag::kStr));
            args.Field(tok.text);
            args_ = args.value();
            ++literal_count_;
            break;
        }
        case TokenType::kParam:
        case TokenType::kNamedParam:
            // Contributes to the shape and nothing else. A `?`'s value
            // arrives at BIND, which is where arg_hash is completed; a
            // `$param` has no value at all, because a declaration is not an
            // execution (spec section 3.3).
            ++param_count_;
            break;
        default:
            // Operators, punctuation and NULL are fully described by their
            // tag; there is nothing further to hash.
            break;
    }
    shape_ = shape.value();
}

std::optional<Fingerprint> FingerprintAccumulator::Result() const noexcept {
    // A prefix of a hash is not the hash of a prefix. A parse that stopped
    // early fed part of the stream, and answering with what it managed to
    // hash would hand back a plausible number for a statement nobody can
    // reproduce.
    if (!valid_ || !complete_) return std::nullopt;

    Fingerprint out;
    out.pattern_id = shape_;
    out.arg_hash = args_;
    out.literal_count = literal_count_;
    out.param_count = param_count_;
    return out;
}

std::optional<Fingerprint> FingerprintOf(std::string_view sql) {
    // Driven through the accumulator rather than duplicating its walk: one
    // set of rules, two ways in. See fingerprint.hpp on why a second copy
    // would be a hazard rather than a convenience.
    Lexer lexer(sql);
    FingerprintAccumulator accumulator;
    for (;;) {
        Token tok = lexer.Next();
        const bool eof = tok.type == TokenType::kEof;
        accumulator.Feed(tok);
        if (eof) break;
    }
    return accumulator.Result();
}

}  // namespace kds::parser
