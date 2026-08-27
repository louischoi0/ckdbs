#include <string>
#include <string_view>
#include <variant>

#include <gtest/gtest.h>

#include "kds/parser/parser.hpp"

// ALT01 - the ALTER grammar (docs/spec/alter.md AL1, AL7).
//
// Two things this file holds down. **The refusal surface is the feature**:
// AL1 makes v1 catalog-only, so ADD/DROP/MODIFY/SET answer a positioned
// Unsupported carrying the reason (invariant 13 - a column-set change is a
// relation rewrite), never a syntax error pointing past the word. And
// **nothing is reserved**: `alter`, `table`, `rename`, `column` and `to`
// are ordinary identifiers matched by text, so a table may be named
// `rename` and `kFingerprintVersion` does not move - ALTER is not a
// patternable head, so its corpus lines carry `-` hashes.

namespace kds::parser {
namespace {

AlterStmt MustAlter(const StatusOr<Statement>& parsed) {
    EXPECT_TRUE(parsed.ok()) << parsed.status().message();
    if (!parsed.ok()) return AlterStmt{};
    return std::get<AlterStmt>(parsed.value());
}

std::uint32_t ByteOf(std::string_view sql, std::string_view word) {
    const auto at = sql.find(word);
    EXPECT_NE(at, std::string_view::npos) << word << " not in: " << sql;
    return static_cast<std::uint32_t>(at);
}

::testing::AssertionResult MentionsByte(const Status& status, std::uint32_t offset) {
    const std::string want = "byte " + std::to_string(offset);
    if (status.message().find(want) != std::string::npos) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << "expected '" << want << "' in: " << status.message();
}

// ---- Accepted forms -------------------------------------------------------

TEST(ParserAlterTest, RenameTableParses) {
    const AlterStmt stmt = MustAlter(Parse("ALTER TABLE t RENAME TO u"));
    EXPECT_EQ(stmt.table_name, "t");
    EXPECT_FALSE(stmt.rename_column);
    EXPECT_EQ(stmt.new_name, "u");
}

TEST(ParserAlterTest, RenameColumnParses) {
    const AlterStmt stmt = MustAlter(Parse("ALTER TABLE t RENAME COLUMN a TO b"));
    EXPECT_EQ(stmt.table_name, "t");
    EXPECT_TRUE(stmt.rename_column);
    EXPECT_EQ(stmt.old_column, "a");
    EXPECT_EQ(stmt.new_name, "b");
}

TEST(ParserAlterTest, LowercaseAndSemicolonParse) {
    const AlterStmt stmt = MustAlter(Parse("alter table t rename to u;"));
    EXPECT_EQ(stmt.new_name, "u");
}

// Nothing is reserved: every word of the grammar is usable as a name.
TEST(ParserAlterTest, TheGrammarsOwnWordsAreOrdinaryNames) {
    const AlterStmt stmt = MustAlter(Parse("ALTER TABLE rename RENAME TO to"));
    EXPECT_EQ(stmt.table_name, "rename");
    EXPECT_EQ(stmt.new_name, "to");
}

// ---- The AL1 refusal surface ----------------------------------------------

TEST(ParserAlterTest, AddColumnIsUnsupportedWithItsPosition) {
    const std::string_view sql = "ALTER TABLE t ADD COLUMN x int";
    const auto parsed = Parse(sql);
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_TRUE(MentionsByte(parsed.status(), ByteOf(sql, "ADD")));
}

TEST(ParserAlterTest, DropColumnIsUnsupportedWithItsPosition) {
    const std::string_view sql = "ALTER TABLE t DROP COLUMN a";
    const auto parsed = Parse(sql);
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_TRUE(MentionsByte(parsed.status(), ByteOf(sql, "DROP")));
}

TEST(ParserAlterTest, ModifyAndSetAreUnsupported) {
    EXPECT_EQ(Parse("ALTER TABLE t MODIFY a bigint").status().code(),
              StatusCode::kUnsupported);
    EXPECT_EQ(Parse("ALTER TABLE t SET x = 1").status().code(), StatusCode::kUnsupported);
}

// ---- Malformed forms ------------------------------------------------------

TEST(ParserAlterTest, AMissingVerbIsInvalid) {
    const auto parsed = Parse("ALTER TABLE t");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument);
}

TEST(ParserAlterTest, AlterWithoutTableIsInvalid) {
    EXPECT_EQ(Parse("ALTER t RENAME TO u").status().code(), StatusCode::kInvalidArgument);
}

TEST(ParserAlterTest, RenameWithoutToIsInvalid) {
    EXPECT_EQ(Parse("ALTER TABLE t RENAME u").status().code(),
              StatusCode::kInvalidArgument);
}

TEST(ParserAlterTest, TrailingGarbageIsInvalid) {
    EXPECT_EQ(Parse("ALTER TABLE t RENAME TO u extra").status().code(),
              StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace kds::parser
