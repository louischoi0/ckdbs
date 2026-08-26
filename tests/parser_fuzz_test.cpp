#include <cstdint>
#include <fstream>
#include <optional>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "kds/parser/fingerprint.hpp"
#include "kds/parser/lexer.hpp"
#include "kds/parser/parser.hpp"

// V13 - grammar hygiene and fuzz (docs/inflight/in-progress/parser-v2-workplan.md).
//
// The parser is the one subsystem reachable from an unauthenticated
// client with arbitrary bytes, so its contract is narrower than "mostly
// works": for **every** input, valid or not, it must
//
//   1. return a Status rather than crash, hang, or read out of bounds,
//   2. never recurse without bound (V07's depth cap is the guard, and
//      this is what proves the guard is actually reachable),
//   3. leave the fingerprint pass agreeing with it about what lexes.
//
// The mutations below are deliberately dumb. A grammar-aware generator
// produces inputs that look like SQL and therefore exercise the paths a
// hand-written test already covers; byte-level noise finds the ones
// nobody thought to write down - a truncated token, a lone high byte, a
// paren that closes nothing.
//
// Coverage note, stated because it is a real gap rather than an
// oversight: the seed corpus is the **V04-V07** grammar. V08-V12 (`IN`
// value lists, `BETWEEN`, `ORDER BY`/`LIMIT`, `DELETE`, table options,
// session statements) are not built, so no input here exercises them.
// When they land, their forms belong in kSeeds.

namespace kds::parser {
namespace {

// A tiny deterministic PRNG. Not std::mt19937 and not std::random_device:
// this test must produce the same inputs on every machine and every run,
// or a failure cannot be reproduced from the failure message alone.
class Rng {
public:
    explicit Rng(std::uint64_t seed) noexcept : state_(seed ? seed : 0x9E3779B97F4A7C15ull) {}

    std::uint64_t Next() noexcept {
        // xorshift64*, chosen for being four lines and dependency-free.
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1Dull;
    }

    std::size_t Below(std::size_t n) noexcept { return n == 0 ? 0 : Next() % n; }

private:
    std::uint64_t state_;
};

// Seed statements: one per production the V04-V07 grammar accepts or
// reserves. Mutations start from these rather than from random bytes,
// because a mutation of something almost-valid reaches far deeper into
// the parser than random noise ever does.
const std::vector<std::string>& Seeds() {
    static const std::vector<std::string> seeds = {
        "CREATE TABLE t (id int, name varchar)",
        "CREATE TABLE t (id int) BTREE",
        "INSERT INTO t VALUES (1, 'abc', NULL)",
        "SELECT * FROM t",
        "SELECT * FROM t WHERE id = 1 AND name = 'x'",
        "SELECT * FROM t WHERE id = -1",
        "SELECT * FROM t AS a WHERE a.id = 1",
        "SELECT a.x, y, b.z FROM t AS a JOIN u AS b ON a.id = b.id",
        "SELECT t.id FROM t JOIN u ON t.id = u.t_id JOIN v ON u.id = v.u_id",
        "SELECT * FROM t LEFT JOIN u ON t.id = u.id",
        "SELECT * FROM t WHERE EXISTS (SELECT * FROM u)",
        "SELECT * FROM t WHERE NOT EXISTS (SELECT * FROM u WHERE u.t_id = 1)",
        "SELECT * FROM t WHERE id IN (SELECT t_id FROM u)",
        "SELECT * FROM t WHERE id NOT IN (SELECT t_id FROM u)",
        "SELECT * FROM t WHERE id = (SELECT id FROM u)",
        "SELECT * FROM (SELECT * FROM u)",
        "WITH x AS (SELECT * FROM u) SELECT * FROM x",
        "UPDATE t SET a = 1, b = 'x' WHERE t.id = 2",
        "UPDATE t SET a = 1 WHERE id IN (SELECT t_id FROM u)",
        "SELECT * FROM t WHERE s = 'unterminated",
        "-- comment only",
        "",
    };
    return seeds;
}

// The alphabet mutations splice in: every character with syntactic
// meaning in this grammar, plus bytes chosen to be hostile - NUL, a high
// byte that is not valid UTF-8 on its own, a newline.
constexpr char kInteresting[] = {
    '(', ')', ',', ';', '*', '.', '=', '!', '<', '>', '?', '\'', '-', ' ', '\t', '\n',
    'a', 'Z', '0', '9', '_', '\0', '\x7f', '\xff', '\xc3',
};

std::string Mutate(std::string s, Rng& rng) {
    const int op = static_cast<int>(rng.Below(6));
    switch (op) {
        case 0:  // truncate - the classic "token cut in half" case
            if (!s.empty()) s.resize(rng.Below(s.size()));
            break;
        case 1:  // insert one interesting byte
            s.insert(rng.Below(s.size() + 1), 1, kInteresting[rng.Below(sizeof(kInteresting))]);
            break;
        case 2:  // overwrite one byte
            if (!s.empty()) s[rng.Below(s.size())] = kInteresting[rng.Below(sizeof(kInteresting))];
            break;
        case 3:  // delete one byte
            if (!s.empty()) s.erase(rng.Below(s.size()), 1);
            break;
        case 4:  // duplicate a run, which is how unbalanced parens appear
            if (!s.empty()) {
                const std::size_t at = rng.Below(s.size());
                const std::size_t len = 1 + rng.Below(s.size() - at);
                s.insert(at, s.substr(at, len));
            }
            break;
        case 5:  // splice two seeds
            s += Seeds()[rng.Below(Seeds().size())];
            break;
        default: break;
    }
    return s;
}

// ---- The contract ---------------------------------------------------------

TEST(ParserFuzzTest, EveryMutatedInputReturnsAStatusAndNeverCrashes) {
    // The whole test is the absence of a crash: if Parse() reads out of
    // bounds, recurses without bound, or hangs, this never reaches its
    // assertions. What *is* asserted is the narrower property that a
    // failure carries a message - an empty one is useless to a client and
    // usually means a Status was default-constructed by mistake.
    Rng rng(0xC0FFEE);
    int accepted = 0;

    for (int i = 0; i < 20000; ++i) {
        std::string input = Seeds()[rng.Below(Seeds().size())];
        const int rounds = 1 + static_cast<int>(rng.Below(4));
        for (int r = 0; r < rounds; ++r) input = Mutate(std::move(input), rng);

        auto parsed = Parse(input);
        if (parsed.ok()) {
            ++accepted;
        } else {
            EXPECT_FALSE(parsed.status().message().empty())
                << "input " << i << " rejected with an empty message: " << input;
        }
    }

    // Not a correctness property, a coverage one: if mutation destroyed
    // everything, this test would pass while proving nothing about the
    // accepting paths.
    EXPECT_GT(accepted, 0) << "no mutated input parsed - the fuzzer is only hitting error paths";
}

TEST(ParserFuzzTest, TheLexerTerminatesOnEveryInput) {
    // Parse() stops at its first error, so a lexer bug past that point is
    // invisible to it. Drive the lexer directly to the end of every
    // mutated input, which is also the only way a token-position bug on a
    // late token gets exercised.
    Rng rng(0xBADF00D);

    for (int i = 0; i < 5000; ++i) {
        std::string input = Seeds()[rng.Below(Seeds().size())];
        input = Mutate(Mutate(std::move(input), rng), rng);

        Lexer lex(input);
        int guard = 0;
        for (;;) {
            Token tok = lex.Next();
            if (tok.type == TokenType::kEof) break;
            // Every token must lie inside the input it came from - the
            // property an "exact position" error message depends on.
            EXPECT_LE(static_cast<std::size_t>(tok.byte_offset) + tok.length, input.size())
                << "token escapes its input at " << i;
            ASSERT_LT(++guard, 100000) << "lexer did not reach EOF on: " << input;
        }
    }
}

TEST(ParserFuzzTest, DeeplyNestedInputIsRefusedRatherThanOverflowingTheStack) {
    // The adversarial input the depth cap exists for. Every one of these
    // must come back as a Status; a stack overflow here is remotely
    // triggerable from a single unauthenticated string.
    for (int depth : {5, 50, 500, 5000}) {
        std::string sql = "SELECT * FROM t";
        for (int i = 0; i < depth; ++i) sql += " WHERE EXISTS (SELECT * FROM t";
        for (int i = 0; i < depth; ++i) sql += ")";

        auto parsed = Parse(sql);
        ASSERT_FALSE(parsed.ok()) << "depth " << depth << " should exceed the cap";
        EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported) << "depth " << depth;
    }

    // Unbalanced open parens, which recurse without ever unwinding.
    std::string unbalanced = "SELECT * FROM t";
    for (int i = 0; i < 5000; ++i) unbalanced += " WHERE EXISTS (SELECT * FROM t";
    EXPECT_FALSE(Parse(unbalanced).ok());
}

TEST(ParserFuzzTest, LongInputIsBoundedByItsOwnLengthNotByAMultiplier) {
    // A statement that is merely long, rather than nested, must still
    // parse in proportion to its size. The failure this guards against is
    // a production that rescans - which turns a 100 KB statement into a
    // wedged core, since nothing preempts a cooperative task.
    std::string wide = "SELECT * FROM t WHERE id = 1";
    for (int i = 0; i < 20000; ++i) wide += " AND id = 1";
    auto parsed = Parse(wide);
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    EXPECT_EQ(std::get<SelectStmt>(parsed.value()).where.size(), 20001u);

    std::string long_name(100000, 'a');
    EXPECT_FALSE(Parse("SELECT * FROM " + long_name + " WHERE").ok());
}

TEST(ParserFuzzTest, FingerprintAgreesWithTheParserAboutWhatLexes) {
    // A statement the parser accepts but `FingerprintOf` refuses to hash
    // would execute with no pattern, silently opting out of Waystone
    // forever - so the two must agree about what lexes.
    Rng rng(0x5EED);

    for (int i = 0; i < 10000; ++i) {
        std::string input = Seeds()[rng.Below(Seeds().size())];
        input = Mutate(std::move(input), rng);

        auto parsed = Parse(input);
        if (!parsed.ok()) continue;

        const bool patternable = std::holds_alternative<SelectStmt>(parsed.value()) ||
                                 std::holds_alternative<InsertStmt>(parsed.value()) ||
                                 std::holds_alternative<UpdateStmt>(parsed.value());
        if (!patternable) continue;

        auto fp = FingerprintOf(input);
        EXPECT_TRUE(fp.has_value())
            << "the parser accepted a patternable statement the fingerprint would not hash: "
            << input;
    }
}

TEST(ParserFuzzTest, TheParseTimeAndStandaloneFingerprintsNeverDisagree) {
    // The fingerprint is computed two ways: accumulated by the lexer during
    // a parse, and by `FingerprintOf` lexing the text on its own. They must
    // produce the same number, over anything.
    //
    // A divergence would not fail anything - it would give one statement two
    // `pattern_id`s depending on which path computed it, so a trail recorded
    // under one would never be found by the other, and the only symptom is
    // Waystone quietly never hitting. Fuzzed rather than left to the corpus
    // because the corpus is the shapes someone thought of.
    Rng rng(0xC0FFEE11);

    int compared = 0;
    for (int i = 0; i < 10000; ++i) {
        const std::string input = Mutate(Seeds()[rng.Below(Seeds().size())], rng);

        Parser parser(input);
        auto parsed = parser.Parse();
        if (!parsed.ok()) continue;

        const auto during = parser.fingerprint();
        const auto standalone = FingerprintOf(input);
        ASSERT_EQ(during.has_value(), standalone.has_value())
            << "one path found a pattern and the other did not: " << input;
        if (!during.has_value()) continue;

        ASSERT_EQ(during->pattern_id, standalone->pattern_id) << input;
        ASSERT_EQ(during->arg_hash, standalone->arg_hash) << input;
        ASSERT_EQ(during->literal_count, standalone->literal_count) << input;
        ASSERT_EQ(during->param_count, standalone->param_count) << input;
        ++compared;
    }
    EXPECT_GT(compared, 100) << "too few inputs parsed; the mutator or the filter is wrong";
}

TEST(ParserFuzzTest, AStatementOutlivesTheTextItWasParsedFrom) {
    // Token text is a view into the SQL (token.hpp), so the AST's
    // copy-at-the-boundary rule is what stops a `Statement` from dangling.
    // Parsed from a buffer that is then destroyed and overwritten: if any
    // AST field were still viewing it, the names below would be garbage.
    std::optional<Statement> stmt;
    {
        std::string sql = "SELECT a_long_column_name_past_sso FROM a_long_relation_name_past_sso "
                          "WHERE another_long_column_name = 'a string value past sso'";
        auto parsed = Parse(sql);
        ASSERT_TRUE(parsed.ok()) << parsed.status().message();
        stmt = std::move(parsed.value());
        // Scribble over the source before it dies, so a surviving view sees
        // changed bytes rather than merely freed ones.
        std::fill(sql.begin(), sql.end(), 'Z');
    }

    const auto& select = std::get<SelectStmt>(*stmt);
    EXPECT_EQ(select.from.table_name, "a_long_relation_name_past_sso");
    ASSERT_EQ(select.projection.size(), 1u);
    EXPECT_EQ(select.projection[0].name, "a_long_column_name_past_sso");
    ASSERT_EQ(select.where.size(), 1u);
    EXPECT_EQ(select.where[0].col.name, "another_long_column_name");
    EXPECT_EQ(select.where[0].val.str_val, "a string value past sso");
}

TEST(ParserFuzzTest, FingerprintingIsAPureFunctionOfTheText) {
    // pattern_id goes on disk in sys.patterns, so it must depend on
    // nothing but the bytes - not on call order, not on what was parsed
    // before, not on any residual state.
    Rng rng(0xF19E4B71);

    for (int i = 0; i < 5000; ++i) {
        std::string input = Mutate(Seeds()[rng.Below(Seeds().size())], rng);

        auto first = FingerprintOf(input);
        auto second = FingerprintOf(input);
        ASSERT_EQ(first.has_value(), second.has_value()) << input;
        if (!first.has_value()) continue;
        EXPECT_EQ(first->pattern_id, second->pattern_id) << input;
        EXPECT_EQ(first->arg_hash, second->arg_hash) << input;
    }
}

// ---- Hygiene --------------------------------------------------------------

TEST(ParserFuzzTest, NoProductionExistsOnlyForCompatibility) {
    // Spec I13: dialect compatibility is a non-goal, and the way that
    // erodes is one "harmless" production at a time, each justified in a
    // comment. Grep-enforced rather than left to review, because the
    // whole point is that each individual addition looks reasonable.
    //
    // Reads the source rather than testing behaviour: the property is
    // about what the grammar *contains*, which no input can demonstrate.
    const char* kSources[] = {
        KDS_SOURCE_DIR "/src/parser/parser.cpp",
        KDS_SOURCE_DIR "/src/parser/lexer.cpp",
        KDS_SOURCE_DIR "/include/kds/parser/ast.hpp",
    };
    // Lower-cased before matching, so a capitalised comment does not slip
    // through. "compatibility" alone is too broad - the fingerprint's
    // "compatibility surface" is a legitimate use - so the phrases are
    // the ones that would introduce a production, not describe one.
    const char* kBanned[] = {
        "for compatibility",
        "postgres compat",
        "mysql compat",
        "dialect compat",
        "accepted for compat",
    };

    for (const char* path : kSources) {
        std::ifstream in(path);
        ASSERT_TRUE(in.is_open()) << "cannot read " << path;
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        for (const char* phrase : kBanned) {
            EXPECT_EQ(text.find(phrase), std::string::npos)
                << path << " contains \"" << phrase << "\" - spec I13 makes dialect "
                << "compatibility a non-goal; a production that exists only to accept "
                << "another database's spelling is the thing this test forbids";
        }
    }
}

}  // namespace
}  // namespace kds::parser
