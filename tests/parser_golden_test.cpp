#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "kds/parser/fingerprint.hpp"
#include "kds/parser/parser.hpp"

// V01 - the golden corpus (docs/inflight/in-progress/parser-v2-workplan.md).
//
// This test does not decide whether the parser is *right*. It decides
// whether it still does what it did, which is a different and, for the
// next five phases, more useful question: every task from V04 onward edits
// the lexer or the grammar, and the corpus is what turns "I did not mean
// to change that" into a failing test.
//
// Two independent columns per statement, and the distinction matters:
//
//   verdict      accepted, or the StatusCode it was rejected with. This
//                column is *expected to change*, deliberately, as reserved
//                forms become supported (V05, V07) or become Unsupported
//                rather than InvalidArgument (V02). A diff here must be
//                accompanied by the task that intended it.
//
//   pattern_id   the fingerprint of the token stream, and arg_hash with
//   arg_hash     it. These columns must **not** move. The v2 language is
//                additive shape, so every statement that hashes to X today
//                must still hash to X after the grammar grows and after
//                the fingerprint pass is folded into the parser (V29).
//                That is the whole evidence for "no kFingerprintVersion
//                bump", which spec I1 claims and this file is asked to
//                back up. A diff here is a format break for every stored
//                waystone, so it fails loudly and on purpose.
//
// The verdicts were written by hand from src/parser/parser.cpp and then
// checked against the implementation; the hashes were recorded from it.
// That asymmetry is deliberate - a hand-written verdict that disagrees
// with the code is a finding, whereas a hand-computed 64-bit hash is only
// a typo waiting to happen. Independent cross-checking of the hash
// *algorithm* is tests/fingerprint_test.cpp's job and stays there.
//
// Regenerating: run with KDS_CORPUS_REGEN=1 and the test writes
// <corpus>.regen with the actual values, comments and layout preserved.
// Diff it, understand every line that moved, then move it over. It is
// never run as part of accepting a change - if it were, the file would
// record whatever the code does and stop being an oracle.

namespace kds::parser {
namespace {

std::string_view StatusCodeName(StatusCode code) {
    switch (code) {
        case StatusCode::kOk: return "ok";
        case StatusCode::kInvalidArgument: return "InvalidArgument";
        case StatusCode::kOutOfSpace: return "OutOfSpace";
        case StatusCode::kNotFound: return "NotFound";
        case StatusCode::kAlreadyExists: return "AlreadyExists";
        case StatusCode::kOutOfRange: return "OutOfRange";
        case StatusCode::kCorruption: return "Corruption";
        case StatusCode::kIoError: return "IoError";
        case StatusCode::kTxnConflict: return "TxnConflict";
        // V02's two codes. No corpus line carries them yet - the verdicts
        // that will flip to Unsupported are the ones marked [V05] and
        // [V07], and they flip with the task that makes them true.
        case StatusCode::kUnsupported: return "Unsupported";
        case StatusCode::kCardinalityViolation: return "CardinalityViolation";
        case StatusCode::kResourceExhausted: return "ResourceExhausted";
        // Runtime codes: a parse never produces either, so no corpus line
        // carries them. Named anyway, because the table is total.
        case StatusCode::kFkViolation: return "FkViolation";
        case StatusCode::kAssertionViolation: return "AssertionViolation";
        case StatusCode::kUnknownOutcome: return "UnknownOutcome";
    }
    return "<unknown>";
}

std::string Hex64(std::uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return std::string(buf, 16);
}

// A corpus line is `verdict pattern_id arg_hash sql`: three whitespace-
// delimited fields and then the rest of the line verbatim, so the SQL may
// contain spaces without quoting. `\n` in the SQL is an escape for a real
// newline, which the `--` comment cases need.
struct Entry {
    int line_no = 0;
    std::string verdict;
    std::string pattern;
    std::string args;
    std::string sql;      // escapes resolved; what the parser is handed
    std::string sql_raw;  // as written in the file, so a regen can echo it back
};

std::string Unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') {
            out.push_back('\n');
            ++i;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

bool SplitLine(const std::string& line, Entry& out) {
    std::size_t pos = 0;
    std::string* fields[3] = {&out.verdict, &out.pattern, &out.args};
    for (std::string* field : fields) {
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
        const std::size_t start = pos;
        while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
        if (start == pos) return false;
        *field = line.substr(start, pos - start);
    }
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
    if (pos >= line.size()) return false;
    out.sql_raw = line.substr(pos);
    out.sql = Unescape(out.sql_raw);
    return true;
}

const char* CorpusPath() { return KDS_PARSER_CORPUS; }

// Raw lines, so the regenerator can preserve comments and blank lines.
std::vector<std::string> ReadLines() {
    std::ifstream in(CorpusPath());
    EXPECT_TRUE(in.is_open()) << "cannot open corpus: " << CorpusPath();
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

bool IsSkippable(const std::string& line) {
    for (char c : line) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        return c == '#';
    }
    return true;  // blank
}

// What the current build says about one statement, in corpus field order.
struct Actual {
    std::string verdict;
    std::string pattern;
    std::string args;
};

Actual Observe(const std::string& sql) {
    Actual a;
    auto parsed = Parse(sql);
    a.verdict = parsed.ok() ? "ok" : std::string(StatusCodeName(parsed.status().code()));

    const std::optional<Fingerprint> fp = FingerprintOf(sql);
    a.pattern = fp ? Hex64(fp->pattern_id) : "-";
    a.args = fp ? Hex64(fp->arg_hash) : "-";
    return a;
}

// **The fingerprint is computed twice, two different ways, and must agree.**
//
// `FingerprintOf()` lexes the text on its own; `Parser::fingerprint()` rides
// along with the parse and never lexes anything twice. Folding the second
// pass away is only safe if the two produce *the same number* - a divergence
// would not fail, it would quietly give one statement two pattern_ids
// depending on which path computed it, retiring stored waystones at random.
//
// Checked over the whole corpus, which is the only collection of statements
// anyone maintains: every shape the grammar accepts, plus the ones it
// refuses. Compared only where the parse succeeded, because a failed parse
// stops mid-stream by design and hashes a prefix - see Parser::fingerprint().
TEST(ParserGoldenTest, TheParseTimeFingerprintMatchesTheStandaloneOne) {
    const std::vector<std::string> lines = ReadLines();
    ASSERT_FALSE(lines.empty());

    int compared = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (IsSkippable(lines[i])) continue;
        Entry e;
        ASSERT_TRUE(SplitLine(lines[i], e));
        const std::string& sql = e.sql;

        Parser parser(sql);
        auto parsed = parser.Parse();
        if (!parsed.ok()) continue;

        const std::optional<Fingerprint> during = parser.fingerprint();
        const std::optional<Fingerprint> standalone = FingerprintOf(sql);

        ASSERT_EQ(during.has_value(), standalone.has_value())
            << CorpusPath() << ":" << (i + 1) << ": one path found a pattern and the other did"
            << " not, for: " << e.sql_raw;
        if (!during.has_value()) continue;

        EXPECT_EQ(during->pattern_id, standalone->pattern_id)
            << CorpusPath() << ":" << (i + 1) << ": pattern_id differs between the parse-time"
            << " and standalone fingerprints, for: " << e.sql_raw;
        EXPECT_EQ(during->arg_hash, standalone->arg_hash)
            << CorpusPath() << ":" << (i + 1) << ": arg_hash differs, for: " << e.sql_raw;
        EXPECT_EQ(during->literal_count, standalone->literal_count) << e.sql_raw;
        EXPECT_EQ(during->param_count, standalone->param_count) << e.sql_raw;
        ++compared;
    }
    EXPECT_GT(compared, 20) << "too few statements compared; the corpus or the filter is wrong";
}

TEST(ParserGoldenTest, TheParseTimeFingerprintNeedsTheWholeTokenStream) {
    // The accumulator's rule is "did the lexer reach end of input", not "did
    // the parse succeed", and the difference is worth pinning because it
    // cuts both ways.
    //
    // A parse that stopped **in the middle** saw a prefix. A hash of a
    // prefix is not a prefix of a hash, so there is no honest number to
    // report and it reports none.
    {
        Parser parser("SELECT * FROM t extra");
        EXPECT_FALSE(parser.Parse().ok());
        EXPECT_FALSE(parser.fingerprint().has_value())
            << "a prefix of the stream must not produce a plausible-looking hash";
        // The standalone entry point lexes to the end regardless, so it
        // still has one. That asymmetry is why both exist.
        EXPECT_TRUE(FingerprintOf("SELECT * FROM t extra").has_value());
    }

    // A parse that failed **because the input ran out** still lexed the
    // whole statement, so its fingerprint is the whole statement's - and
    // equal to the standalone one. Reporting nullopt here would be
    // needlessly conservative.
    {
        const std::string sql = "SELECT * FROM t WHERE id =";
        Parser parser(sql);
        EXPECT_FALSE(parser.Parse().ok());
        const auto during = parser.fingerprint();
        const auto standalone = FingerprintOf(sql);
        ASSERT_TRUE(during.has_value());
        ASSERT_TRUE(standalone.has_value());
        EXPECT_EQ(during->pattern_id, standalone->pattern_id);
        EXPECT_EQ(during->arg_hash, standalone->arg_hash);
    }
}

TEST(ParserGoldenTest, CorpusIsWellFormed) {
    const std::vector<std::string> lines = ReadLines();
    ASSERT_FALSE(lines.empty()) << "corpus is empty: " << CorpusPath();

    int entries = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (IsSkippable(lines[i])) continue;
        Entry e;
        ASSERT_TRUE(SplitLine(lines[i], e))
            << CorpusPath() << ":" << (i + 1) << ": expected `verdict pattern arg sql`, got: "
            << lines[i];
        EXPECT_TRUE(e.pattern == "-" || e.pattern.size() == 16)
            << CorpusPath() << ":" << (i + 1) << ": pattern_id must be 16 hex digits or '-'";
        EXPECT_EQ(e.pattern == "-", e.args == "-")
            << CorpusPath() << ":" << (i + 1)
            << ": a statement is either fingerprintable in both columns or neither";
        ++entries;
    }

    // Not a coverage proof - nothing mechanical can prove every production
    // is exercised - but a floor that catches a truncated or half-written
    // corpus, which would silently stop being an oracle.
    EXPECT_GE(entries, 80) << "corpus lost entries; every production and error path in "
                              "src/parser/parser.cpp is supposed to appear here";
}

TEST(ParserGoldenTest, EveryStatementStillBehavesAsRecorded) {
    const std::vector<std::string> lines = ReadLines();
    const bool regen = std::getenv("KDS_CORPUS_REGEN") != nullptr;

    std::vector<std::string> regenerated;
    regenerated.reserve(lines.size());

    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (IsSkippable(lines[i])) {
            regenerated.push_back(lines[i]);
            continue;
        }
        Entry e;
        e.line_no = static_cast<int>(i + 1);
        if (!SplitLine(lines[i], e)) {
            regenerated.push_back(lines[i]);
            continue;  // CorpusIsWellFormed reports it
        }

        const Actual a = Observe(e.sql);
        if (regen) {
            std::ostringstream row;
            row << a.verdict << '\t' << a.pattern << '\t' << a.args << '\t' << e.sql_raw;
            regenerated.push_back(row.str());
            continue;
        }

        EXPECT_EQ(a.verdict, e.verdict)
            << CorpusPath() << ":" << e.line_no << ": verdict changed for: " << e.sql
            << "\n  a verdict may only move together with the task that intended it";
        EXPECT_EQ(a.pattern, e.pattern)
            << CorpusPath() << ":" << e.line_no << ": pattern_id moved for: " << e.sql
            << "\n  the v2 language is additive shape - this is a format break for every "
               "stored waystone, not a test to update";
        EXPECT_EQ(a.args, e.args)
            << CorpusPath() << ":" << e.line_no << ": arg_hash moved for: " << e.sql;
    }

    if (regen) {
        const std::string out_path = std::string(CorpusPath()) + ".regen";
        std::ofstream out(out_path);
        ASSERT_TRUE(out.is_open()) << "cannot write " << out_path;
        for (const std::string& line : regenerated) out << line << '\n';
        GTEST_SKIP() << "regenerated corpus written to " << out_path
                     << " - diff it, justify every moved line, then move it over";
    }
}

// The property the pattern_id column exists to protect, stated directly so
// it fails on its own rather than only as 80 corpus diffs: convergence is
// what makes a client that inlines literals and one that binds parameters
// share a waystone.
TEST(ParserGoldenTest, InlineAndBoundFormsShareOnePattern) {
    const auto inline_form = FingerprintOf("SELECT * FROM t WHERE id = 42");
    const auto bound_form = FingerprintOf("SELECT * FROM t WHERE id = ?");
    ASSERT_TRUE(inline_form.has_value());
    ASSERT_TRUE(bound_form.has_value());

    EXPECT_EQ(inline_form->pattern_id, bound_form->pattern_id);
    EXPECT_NE(inline_form->arg_hash, bound_form->arg_hash);
    EXPECT_EQ(inline_form->literal_count, 1u);
    EXPECT_EQ(bound_form->param_count, 1u);
}

}  // namespace
}  // namespace kds::parser
