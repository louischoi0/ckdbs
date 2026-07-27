#include "kds/parser/lexer.hpp"

#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace kds::parser {
namespace {

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
