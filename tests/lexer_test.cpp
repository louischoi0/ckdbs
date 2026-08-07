#include "kds/parser/lexer.hpp"

#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace kds::parser {
namespace {

// Collects every token of `sql`.
//
// **`sql` must outlive the returned tokens**: a Token's `text` is a view
// into it (token.hpp). Every caller here passes a string literal, which has
// static storage duration, so the views stay valid for the whole program.
std::vector<Token> LexAll(std::string_view sql) {
    Lexer lex(sql);
    std::vector<Token> out;
    for (;;) {
        Token t = lex.Next();
        out.push_back(t);
        if (t.type == TokenType::kEof) break;
    }
    return out;
}

TEST(LexerTest, EmptyInputIsJustEof) {
    auto toks = LexAll("");
    ASSERT_EQ(toks.size(), 1u);
    EXPECT_EQ(toks[0].type, TokenType::kEof);
}

TEST(LexerTest, SkipsWhitespaceAndLineComments) {
    auto toks = LexAll("  \n -- a comment\n  PING  ");
    ASSERT_EQ(toks.size(), 2u);
    EXPECT_EQ(toks[0].type, TokenType::kIdent);
    EXPECT_EQ(toks[0].text, "PING");
}

TEST(LexerTest, IdentifierAndKeywordCasePreserved) {
    auto toks = LexAll("SeLeCt");
    ASSERT_EQ(toks[0].type, TokenType::kIdent);
    EXPECT_EQ(toks[0].text, "SeLeCt");
}

TEST(LexerTest, NullLiteralRecognizedCaseInsensitively) {
    for (auto text : {"NULL", "null", "Null"}) {
        Lexer lex(text);
        Token t = lex.Next();
        EXPECT_EQ(t.type, TokenType::kNullLit) << text;
    }
}

TEST(LexerTest, IntegerLiteralPositiveAndNegative) {
    auto toks = LexAll("42 -7 0");
    ASSERT_EQ(toks.size(), 4u);  // 3 ints + EOF
    EXPECT_EQ(toks[0].type, TokenType::kIntLit);
    EXPECT_EQ(toks[0].int_val, 42);
    EXPECT_EQ(toks[1].int_val, -7);
    EXPECT_EQ(toks[2].int_val, 0);
}

TEST(LexerTest, StringLiteralStripsQuotes) {
    auto toks = LexAll("'hello world'");
    ASSERT_EQ(toks[0].type, TokenType::kStrLit);
    EXPECT_EQ(toks[0].text, "hello world");
}

TEST(LexerTest, UnterminatedStringReadsToEnd) {
    auto toks = LexAll("'no closing quote");
    ASSERT_EQ(toks[0].type, TokenType::kStrLit);
    EXPECT_EQ(toks[0].text, "no closing quote");
}

TEST(LexerTest, PunctuationAndOperators) {
    auto toks = LexAll("( ) , ; * = != < <= > >=");
    std::vector<TokenType> expected = {
        TokenType::kLParen, TokenType::kRParen, TokenType::kComma, TokenType::kSemicolon,
        TokenType::kStar,   TokenType::kEq,     TokenType::kNeq,   TokenType::kLt,
        TokenType::kLte,    TokenType::kGt,     TokenType::kGte,   TokenType::kEof,
    };
    ASSERT_EQ(toks.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(toks[i].type, expected[i]) << "token " << i;
    }
}

TEST(LexerTest, UnrecognizedCharacterIsError) {
    auto toks = LexAll("@");
    ASSERT_EQ(toks[0].type, TokenType::kError);
}

TEST(LexerTest, BindParameterIsItsOwnTokenNotAnError) {
    // `?` used to lex as kError. It has a type now so fingerprinting can
    // tell a placeholder from a lexing failure (fingerprint.hpp) - but it
    // is still accepted by no production, which the parser test below
    // pins.
    auto toks = LexAll("?");
    ASSERT_EQ(toks[0].type, TokenType::kParam);
    EXPECT_EQ(toks[0].text, "?");
}

// ---- V04: reserved words, the dot, positions, raw digits ------------------

TEST(LexerTest, ReservedWordsLexAsKeywordsCaseInsensitively) {
    struct Case {
        const char* text;
        Keyword kw;
    };
    const Case cases[] = {
        {"JOIN", Keyword::kJoin},       {"on", Keyword::kOn},   {"As", Keyword::kAs},
        {"IN", Keyword::kIn},           {"exists", Keyword::kExists},
        {"NoT", Keyword::kNot},         {"BETWEEN", Keyword::kBetween},
    };
    for (const Case& c : cases) {
        Lexer lex(c.text);
        Token t = lex.Next();
        EXPECT_EQ(t.type, TokenType::kKeyword) << c.text;
        EXPECT_EQ(t.kw, c.kw) << c.text;
        // Text is preserved as written: the fingerprint folds it itself,
        // and an error message should quote what the client typed.
        EXPECT_EQ(t.text, c.text);
    }
}

TEST(LexerTest, WordsTheGrammarMatchesByTextAreStillIdentifiers) {
    // Reserving a word makes it unusable as a column name, so the list is
    // deliberately short. These are matched by text where the grammar
    // wants them (Parser::ExpectKeyword) and stay identifiers everywhere
    // else - a column may still be called `values` or `set`.
    for (auto text : {"SELECT", "FROM", "WHERE", "AND", "VALUES", "SET", "TABLE", "HEAP"}) {
        Lexer lex(text);
        EXPECT_EQ(lex.Next().type, TokenType::kIdent) << text;
    }
}

TEST(LexerTest, QualifiedNameLexesAsThreeTokens) {
    // `a.x` could not be tokenized at all before V04: the '.' was a
    // kError, which is why every corpus statement containing one had no
    // fingerprint.
    auto toks = LexAll("a.x");
    ASSERT_EQ(toks.size(), 4u);  // ident, dot, ident, EOF
    EXPECT_EQ(toks[0].type, TokenType::kIdent);
    EXPECT_EQ(toks[0].text, "a");
    EXPECT_EQ(toks[1].type, TokenType::kDot);
    EXPECT_EQ(toks[2].type, TokenType::kIdent);
    EXPECT_EQ(toks[2].text, "x");
}

TEST(LexerTest, DigitsAroundADotAreOneNumericLiteral) {
    // TY3 phase 2. This test's predecessor pinned the opposite - `1.5` as
    // int, dot, int - and lifting that is this change's whole point, so
    // the pin flips deliberately. The spelling survives on `text`, sign
    // and point included, because the spelling *is* the value: the parser
    // hands it on as the string `'1.5'` would have been.
    auto toks = LexAll("1.5");
    ASSERT_EQ(toks.size(), 2u);
    EXPECT_EQ(toks[0].type, TokenType::kNumLit);
    EXPECT_EQ(toks[0].text, "1.5");

    auto neg = LexAll("-12.34");
    ASSERT_EQ(neg.size(), 2u);
    EXPECT_EQ(neg[0].type, TokenType::kNumLit);
    EXPECT_EQ(neg[0].text, "-12.34");
}

TEST(LexerTest, ANumericLiteralNeedsDigitsOnBothSidesOfTheDot) {
    // `12.` - the dot is not consumed; an integer and a dot, exactly as
    // before the numeric token existed. This is what keeps `t.x`-style
    // grammar (and every statement that parsed before) token-identical.
    auto trailing = LexAll("12.");
    ASSERT_EQ(trailing.size(), 3u);
    EXPECT_EQ(trailing[0].type, TokenType::kIntLit);
    EXPECT_EQ(trailing[0].int_val, 12);
    EXPECT_EQ(trailing[1].type, TokenType::kDot);

    // `.5` - a leading dot starts nothing.
    auto leading = LexAll(".5");
    ASSERT_EQ(leading.size(), 3u);
    EXPECT_EQ(leading[0].type, TokenType::kDot);
    EXPECT_EQ(leading[1].type, TokenType::kIntLit);
    EXPECT_EQ(leading[1].int_val, 5);

    // `1.b` - a digit run, a qualifier dot, an identifier. Unchanged.
    auto qual = LexAll("1.b");
    ASSERT_EQ(qual.size(), 4u);
    EXPECT_EQ(qual[0].type, TokenType::kIntLit);
    EXPECT_EQ(qual[1].type, TokenType::kDot);
    EXPECT_EQ(qual[2].type, TokenType::kIdent);
}

TEST(LexerTest, ASecondDotEndsTheNumericLiteral) {
    // `1.2.3` is a numeric literal, a dot and an integer - which fails to
    // parse, and should, rather than guessing which two dots to keep.
    auto toks = LexAll("1.2.3");
    ASSERT_EQ(toks.size(), 4u);
    EXPECT_EQ(toks[0].type, TokenType::kNumLit);
    EXPECT_EQ(toks[0].text, "1.2");
    EXPECT_EQ(toks[1].type, TokenType::kDot);
    EXPECT_EQ(toks[2].type, TokenType::kIntLit);
    EXPECT_EQ(toks[2].int_val, 3);
}

TEST(LexerTest, ANumericLiteralCarriesItsExactByteRange) {
    //                              0123456789012345678
    auto toks = LexAll("WHERE amt = 12.34");
    ASSERT_EQ(toks.size(), 5u);
    EXPECT_EQ(toks[3].type, TokenType::kNumLit);
    EXPECT_EQ(toks[3].byte_offset, 12u);
    EXPECT_EQ(toks[3].length, 5u);
}

TEST(LexerTest, EveryTokenCarriesItsExactByteRange) {
    //                0123456789...
    const std::string_view sql = "SELECT * FROM t WHERE name = 'ab'";
    auto toks = LexAll(sql);
    ASSERT_EQ(toks.size(), 9u);  // SELECT * FROM t WHERE name = 'ab' EOF

    for (const Token& t : toks) {
        if (t.type == TokenType::kEof) continue;
        // The recorded range must reproduce the token as written - which
        // is the property an "exact position" in an error message needs,
        // and the one a length computed from the decoded text would lose.
        EXPECT_LE(t.byte_offset + t.length, sql.size());
        const std::string_view raw = sql.substr(t.byte_offset, t.length);
        if (t.type == TokenType::kStrLit) {
            EXPECT_EQ(raw, "'ab'") << "a string's extent covers its quotes";
        } else {
            EXPECT_EQ(raw, t.text);
        }
    }

    EXPECT_EQ(toks[0].byte_offset, 0u);
    EXPECT_EQ(toks[0].length, 6u);
    EXPECT_EQ(toks[7].type, TokenType::kStrLit);
    EXPECT_EQ(toks[7].byte_offset, 29u);
    EXPECT_EQ(toks[7].length, 4u);
}

TEST(LexerTest, PositionsSkipWhitespaceAndComments) {
    const std::string_view sql = "  -- lead\n  PING";
    auto toks = LexAll(sql);
    ASSERT_EQ(toks.size(), 2u);
    // Points at the token, not at the whitespace that preceded it.
    EXPECT_EQ(toks[0].byte_offset, 12u);
    EXPECT_EQ(sql.substr(toks[0].byte_offset, toks[0].length), "PING");

    // End of input has a position and no extent, so a "something is
    // missing here" error can still point somewhere real.
    EXPECT_EQ(toks[1].type, TokenType::kEof);
    EXPECT_EQ(toks[1].byte_offset, sql.size());
    EXPECT_EQ(toks[1].length, 0u);
}

TEST(LexerTest, IntegerLiteralsKeepTheirDigitsAlongsideTheDecodedValue) {
    auto toks = LexAll("42 -7");
    EXPECT_EQ(toks[0].digits(), "42");
    EXPECT_FALSE(toks[0].negative);
    EXPECT_EQ(toks[1].digits(), "7") << "the sign is not part of the digit run";
    EXPECT_TRUE(toks[1].negative);
    EXPECT_EQ(toks[1].int_val, -7);
}

TEST(LexerTest, DigitsSurviveAnIntegerThatOverflowsTheSignedDecode) {
    // The reason the digits are kept at all: int_val wraps, so it cannot
    // answer "is this literal inside the 40-bit pk range?" - the check at
    // V30 reads digits() instead. 2^64 - 1 decodes to -1 here.
    auto toks = LexAll("18446744073709551615");
    ASSERT_EQ(toks[0].type, TokenType::kIntLit);
    EXPECT_EQ(toks[0].digits(), "18446744073709551615");
    EXPECT_EQ(toks[0].int_val, -1) << "documenting the wrap, not endorsing it";
}

TEST(LexerTest, ANamedParameterKeepsItsNameAndDropsItsSigil) {
    auto toks = LexAll("$flag $Name_2");
    ASSERT_EQ(toks[0].type, TokenType::kNamedParam);
    // The name is kept as written, like an identifier's: folding is the
    // consumer's job, so an error message can quote what was typed.
    EXPECT_EQ(toks[0].text, "flag");
    EXPECT_EQ(toks[1].type, TokenType::kNamedParam);
    EXPECT_EQ(toks[1].text, "Name_2");

    // The extent covers the sigil, so a reported position points at the
    // `$` a client wrote rather than at the letter after it.
    EXPECT_EQ(toks[0].byte_offset, 0u);
    EXPECT_EQ(toks[0].length, 5u);
    EXPECT_EQ(toks[1].byte_offset, 6u);
    EXPECT_EQ(toks[1].length, 7u);
}

TEST(LexerTest, ANamedParameterIsNotTheBindPlaceholder) {
    // Two token types, deliberately: they agree about the fingerprint
    // (both are ShapeTag::kValue) and disagree about the grammar - `?` is
    // refused everywhere, `$x` is accepted in a declared pattern body.
    auto toks = LexAll("? $x");
    EXPECT_EQ(toks[0].type, TokenType::kParam);
    EXPECT_EQ(toks[1].type, TokenType::kNamedParam);
}

TEST(LexerTest, ABareSigilIsStillALexingError) {
    // There is no anonymous named parameter. Reporting the bad character
    // is more use than inventing a parameter with an empty name.
    auto toks = LexAll("$ $1");
    EXPECT_EQ(toks[0].type, TokenType::kError);
    EXPECT_EQ(toks[1].type, TokenType::kError) << "a digit cannot start an identifier";
}

TEST(LexerTest, TokenTextViewsTheSourceRatherThanCopyingIt) {
    // The zero-copy property, pinned by address rather than by value:
    // comparing text to a string would pass just as well if the lexer went
    // back to allocating a copy of it, which is the regression this guards.
    //
    // Lexing allocates nothing now, and that matters most for exactly the
    // tokens that used to allocate - identifiers past the small-string
    // limit, which is what a schema with real relation names is made of.
    static constexpr std::string_view kSql =
        "SELECT long_column_name_past_sso FROM a_relation_with_a_long_name WHERE x = 'y'";
    auto toks = LexAll(kSql);

    const char* begin = kSql.data();
    const char* end = begin + kSql.size();
    int checked = 0;
    for (const Token& t : toks) {
        if (t.type == TokenType::kEof) continue;
        ASSERT_FALSE(t.text.empty()) << "every non-EOF token has text";
        EXPECT_GE(t.text.data(), begin) << "token text must point into the source";
        EXPECT_LE(t.text.data() + t.text.size(), end);
        ++checked;
    }
    EXPECT_GT(checked, 5);

    // And the view really is the source's own bytes, not a copy that
    // happens to live nearby.
    EXPECT_EQ(toks[1].text.data(), begin + toks[1].byte_offset);
}

TEST(LexerTest, PeekDoesNotConsume) {
    Lexer lex("PING PONG");
    const Token& p1 = lex.Peek();
    EXPECT_EQ(p1.text, "PING");
    const Token& p2 = lex.Peek();
    EXPECT_EQ(p2.text, "PING");
    Token n = lex.Next();
    EXPECT_EQ(n.text, "PING");
    EXPECT_EQ(lex.Next().text, "PONG");
}

}  // namespace
}  // namespace kds::parser
