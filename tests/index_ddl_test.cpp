#include "kds/exec/index_ddl.hpp"

#include <optional>
#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/anchor_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// `CREATE INDEX` / `DROP INDEX` / `SHOW INDEXES` (docs/spec/index.md §10,
// workplan IX05): the grammar, the DDL layer under it, and the reply.
//
// Two things these are really about.
//
// **Nothing is reserved.** `index`, `covering` and `unique` reach the lexer
// as ordinary identifiers, so a column may still be named any of them and no
// `pattern_id` moves. The golden corpus is the evidence for the second half;
// this file covers the first.
//
// **A refusal carries a position.** Every declaration that could never do
// what it says is refused at the earliest layer that can name the byte -
// which for the grammar-level limits is the parser, and for everything about
// the relation is the catalog, passed through unrestated so there is one
// answer to "why not".

namespace kds::exec {
namespace {

using parser::IndexStmt;

const IndexStmt& ParseIndexStmt(const parser::Statement& stmt) {
    return std::get<IndexStmt>(stmt);
}

StatusOr<parser::Statement> Parse(std::string_view sql) {
    parser::Parser parser(sql);
    return parser.Parse();
}

// ---- Grammar ------------------------------------------------------------

TEST(IndexGrammarTest, ASingleColumnIndexParses) {
    auto stmt = Parse("CREATE INDEX by_owner ON accounts (owner)");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();
    const IndexStmt& ix = ParseIndexStmt(stmt.value());
    EXPECT_FALSE(ix.drop);
    EXPECT_EQ(ix.index_name, "by_owner");
    EXPECT_EQ(ix.table_name, "accounts");
    ASSERT_EQ(ix.key_columns.size(), 1u);
    EXPECT_EQ(ix.key_columns[0].name, "owner");
    EXPECT_TRUE(ix.covered_columns.empty());
}

TEST(IndexGrammarTest, AMultiColumnKeyKeepsItsDeclaredOrder) {
    // Declared order is what the key encoding concatenates in, so a parser
    // that sorted or deduplicated here would change what the index means.
    auto stmt = Parse("CREATE INDEX ix ON t (b, a, c)");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();
    const IndexStmt& ix = ParseIndexStmt(stmt.value());
    ASSERT_EQ(ix.key_columns.size(), 3u);
    EXPECT_EQ(ix.key_columns[0].name, "b");
    EXPECT_EQ(ix.key_columns[1].name, "a");
    EXPECT_EQ(ix.key_columns[2].name, "c");
}

TEST(IndexGrammarTest, CoveringIsOptionalAndTakesItsOwnList) {
    auto stmt = Parse("CREATE INDEX ix ON t (a) COVERING (b, c)");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();
    const IndexStmt& ix = ParseIndexStmt(stmt.value());
    ASSERT_EQ(ix.key_columns.size(), 1u);
    ASSERT_EQ(ix.covered_columns.size(), 2u);
    EXPECT_EQ(ix.covered_columns[0].name, "b");
    EXPECT_EQ(ix.covered_columns[1].name, "c");
}

TEST(IndexGrammarTest, DropNamesTheIndexAndNothingElse) {
    // An index's name is unique instance-wide, so naming its relation again
    // would be a second identity to keep in step with the first.
    auto stmt = Parse("DROP INDEX by_owner");
    ASSERT_TRUE(stmt.ok()) << stmt.status().message();
    const IndexStmt& ix = ParseIndexStmt(stmt.value());
    EXPECT_TRUE(ix.drop);
    EXPECT_EQ(ix.index_name, "by_owner");
    EXPECT_TRUE(ix.table_name.empty());
}

TEST(IndexGrammarTest, NothingIsReservedSoAColumnMayStillBeNamedIndex) {
    // `index`, `covering` and `unique` are ordinary identifiers to the
    // lexer, which is what keeps kFingerprintVersion where it is.
    auto create = Parse("CREATE TABLE t (id int64, index int64, covering int64)");
    EXPECT_TRUE(create.ok()) << create.status().message();

    auto select = Parse("SELECT index FROM t WHERE covering = 1");
    EXPECT_TRUE(select.ok()) << select.status().message();

    auto on_them = Parse("CREATE INDEX unique ON t (index) COVERING (covering)");
    ASSERT_TRUE(on_them.ok()) << on_them.status().message();
    EXPECT_EQ(ParseIndexStmt(on_them.value()).index_name, "unique");
}

TEST(IndexGrammarTest, UniqueIsRefusedAtItsOwnByte) {
    auto stmt = Parse("CREATE UNIQUE INDEX ix ON t (a)");
    ASSERT_FALSE(stmt.ok());
    EXPECT_EQ(stmt.status().code(), StatusCode::kUnsupported);
    // The word's own position, which is the point of refusing at the
    // dispatch rather than parsing and rejecting later.
    EXPECT_NE(stmt.status().message().find("byte 7"), std::string::npos)
        << stmt.status().message();
}

TEST(IndexGrammarTest, AnOverCapColumnListIsRefusedWithAPosition) {
    // The parser is the only layer that knows *where* the offending column
    // was written; Catalog::CreateIndex is the backstop for callers that do
    // not come through it.
    auto keys = Parse("CREATE INDEX ix ON t (a, b, c, d, e)");
    ASSERT_FALSE(keys.ok());
    EXPECT_EQ(keys.status().code(), StatusCode::kUnsupported);
    EXPECT_NE(keys.status().message().find("byte"), std::string::npos);

    auto covered = Parse("CREATE INDEX ix ON t (a) COVERING (b, c, d, e, f, g, h, i, j)");
    ASSERT_FALSE(covered.ok());
    EXPECT_EQ(covered.status().code(), StatusCode::kUnsupported);
}

TEST(IndexGrammarTest, AMissingOnOrListIsASyntaxErrorThatSaysWhatWasWanted) {
    EXPECT_FALSE(Parse("CREATE INDEX ix (a)").ok());
    EXPECT_FALSE(Parse("CREATE INDEX ix ON t").ok());
    EXPECT_FALSE(Parse("CREATE INDEX ix ON t ()").ok());
    EXPECT_FALSE(Parse("CREATE INDEX ON t (a)").ok());
    EXPECT_FALSE(Parse("DROP INDEX").ok());
}

// ---- End to end ---------------------------------------------------------

class IndexDdlTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
    }

    server::DispatchOutcome Run(const std::string& sql) {
        server::CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
        return d.Dispatch(sql);
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
};

TEST_F(IndexDdlTest, CreateThenShowThenDrop) {
    ASSERT_EQ(Run("CREATE TABLE t (id int64, owner int64, amount int64) BTREE")
                  .response.substr(0, 3),
              "CRE")
        << Run("SHOW TABLES").response;

    auto created = Run("CREATE INDEX by_owner ON t (owner) COVERING (amount)");
    EXPECT_NE(created.response.find("CREATED INDEX name=by_owner"), std::string::npos)
        << created.response;
    // Printed because it is the thing most likely to be misunderstood:
    // creating an index indexes nothing that is not written after it.
    EXPECT_NE(created.response.find("entries=0"), std::string::npos) << created.response;

    const std::string shown = Run("SHOW INDEXES").response;
    EXPECT_NE(shown.find("indexes=1"), std::string::npos) << shown;
    EXPECT_NE(shown.find("name=by_owner"), std::string::npos) << shown;
    EXPECT_NE(shown.find("keys=(owner)"), std::string::npos) << shown;
    EXPECT_NE(shown.find("covering=(amount)"), std::string::npos) << shown;
    EXPECT_NE(shown.find("height=1"), std::string::npos) << shown;

    EXPECT_NE(Run("DROP INDEX by_owner").response.find("DROPPED INDEX"), std::string::npos);
    EXPECT_NE(Run("SHOW INDEXES").response.find("indexes=0"), std::string::npos);
    EXPECT_NE(Run("DROP INDEX by_owner").response.find("ERR"), std::string::npos);
}

TEST_F(IndexDdlTest, AHeapRelationIsRefusedAndSaysWhy) {
    ASSERT_EQ(Run("CREATE TABLE h (id int64, owner int64)").response.substr(0, 3), "CRE");
    const std::string out = Run("CREATE INDEX ix ON h (owner)").response;
    EXPECT_EQ(out.substr(0, 3), "ERR") << out;
    EXPECT_NE(out.find("heap-clustered"), std::string::npos) << out;
}

TEST_F(IndexDdlTest, ThePrimaryKeyAndAnUnknownColumnAreBothRefused) {
    ASSERT_EQ(Run("CREATE TABLE t (id int64, owner int64) BTREE").response.substr(0, 3),
              "CRE");

    const std::string on_pk = Run("CREATE INDEX ix ON t (id)").response;
    EXPECT_EQ(on_pk.substr(0, 3), "ERR") << on_pk;
    EXPECT_NE(on_pk.find("clustered tree"), std::string::npos) << on_pk;

    const std::string absent = Run("CREATE INDEX ix ON t (nope)").response;
    EXPECT_EQ(absent.substr(0, 3), "ERR") << absent;
    EXPECT_NE(absent.find("no column 'nope'"), std::string::npos) << absent;

    const std::string no_rel = Run("CREATE INDEX ix ON nosuch (owner)").response;
    EXPECT_EQ(no_rel.substr(0, 3), "ERR") << no_rel;
}

TEST_F(IndexDdlTest, ARelationThatHasHeldRowsIsBackfilledRatherThanRefused) {
    // IX05 refused this outright, because an index created over existing
    // rows would have been empty and complete-looking. IX09 built the
    // backfill, so the refusal is lifted and the index arrives populated -
    // the coverage is index_maintain_test.cpp's, which has the transaction
    // manager the version walk needs.
    ASSERT_EQ(Run("CREATE TABLE t (id int64, owner int64) BTREE").response.substr(0, 3),
              "CRE");
    ASSERT_EQ(Run("INSERT INTO t VALUES (5)").response.substr(0, 3), "INS");
    ASSERT_EQ(Run("INSERT INTO t VALUES (6)").response.substr(0, 3), "INS");

    const std::string out = Run("CREATE INDEX ix ON t (owner)").response;
    EXPECT_NE(out.find("CREATED INDEX"), std::string::npos) << out;
    EXPECT_NE(Run("SHOW INDEXES").response.find("entries=2"), std::string::npos)
        << Run("SHOW INDEXES").response;
}

TEST_F(IndexDdlTest, AFloatlessTypeSetMeansEveryDeclarableColumnCanBeAKey) {
    // Every type the engine can store has an index encoding, which is the
    // claim types.md §1 makes about a type being four things. A column
    // type with no order would be refused here.
    const std::string made = Run("CREATE TABLE t (id int64, d date, ts timestamp, "
                                 "amt decimal(10,2), wide decimal(30,4), name varchar, "
                                 "code char, flag bool) BTREE").response;
    ASSERT_EQ(made.substr(0, 3), "CRE") << made;
    for (const char* col : {"d", "ts", "amt", "wide", "name", "code", "flag"}) {
        const std::string sql = std::string("CREATE INDEX ix_") + col + " ON t (" + col + ")";
        EXPECT_NE(Run(sql).response.find("CREATED INDEX"), std::string::npos)
            << col << ": " << Run(sql).response;
    }
}

TEST_F(IndexDdlTest, ADuplicateNameIsRefusedAndTheNameIsFreeAfterADrop) {
    ASSERT_EQ(Run("CREATE TABLE t (id int64, a int64, b int64) BTREE")
                  .response.substr(0, 3),
              "CRE");
    ASSERT_NE(Run("CREATE INDEX ix ON t (a)").response.find("CREATED"), std::string::npos);

    const std::string again = Run("CREATE INDEX ix ON t (b)").response;
    EXPECT_EQ(again.substr(0, 3), "ERR") << again;

    ASSERT_NE(Run("DROP INDEX ix").response.find("DROPPED"), std::string::npos);
    EXPECT_NE(Run("CREATE INDEX ix ON t (b)").response.find("CREATED"), std::string::npos);
}

TEST_F(IndexDdlTest, AnIndexOnACabinedColumnWarnsWithoutRefusing) {
    // An index is complete for every value where a Cabin is authoritative
    // only for observed ones - so the Cabin becomes dead weight, and saying
    // so is the whole of what the engine should do about it.
    ASSERT_EQ(Run("CREATE TABLE t (id int64, owner int64) BTREE").response.substr(0, 3),
              "CRE");
    ASSERT_NE(Run("CREATE CABIN ON t(owner)").response.find("CREATED CABIN"),
              std::string::npos);

    const std::string out = Run("CREATE INDEX ix ON t (owner)").response;
    EXPECT_NE(out.find("CREATED INDEX"), std::string::npos) << out;
    EXPECT_NE(out.find("WARN"), std::string::npos) << out;
    EXPECT_NE(out.find("supersedes"), std::string::npos) << out;
}

TEST_F(IndexDdlTest, ARefusalOnAFullAnchorCostsNoPages) {
    // G2 (the statement-shipping work order; `docs/inflight/known-gaps.md`): the
    // anchor slot was seeded *after* the tree was built, so once the entry
    // table filled, every attempt allocated a whole index tree and threw
    // it away - and nothing in this engine frees a page. Measured on a
    // 3,000-row relation at 32 pages an attempt: 3,259 refusals consumed
    // 104,257 pages in 30 seconds, past the 65,280 ids one free-map region
    // covers.
    //
    // The relation is left empty on purpose: a tree over no rows is one
    // root page, so a single leaked page fails this where a populated
    // relation would need 32 to notice.
    //
    // 679 round trips is the suite's heaviest single test and the count is
    // not negotiable - it is `kAnchorMaxIndexEntries`, spelled as the
    // constant, and the table cannot be filled with fewer. It is also not
    // linear: every `BumpVersion` drops `TableAccess`, so each build
    // re-scans a `sys.indexes` that is growing, which is why the relation
    // holds no rows to make the *pages* cheap.
    ASSERT_EQ(Run("CREATE TABLE t (id int64, owner int64) BTREE").response.substr(0, 3), "CRE");
    for (std::size_t i = 0; i < storage::kAnchorMaxIndexEntries; ++i) {
        const std::string name = "fill" + std::to_string(i);
        ASSERT_NE(Run("CREATE INDEX " + name + " ON t (owner)").response.find("CREATED INDEX"),
                  std::string::npos)
            << i;
        ASSERT_NE(Run("DROP INDEX " + name).response.find("DROPPED INDEX"), std::string::npos)
            << i;
    }

    // DROP INDEX frees no anchor entry - no removal exists (anchor_page.hpp
    // says why) - so the table is full and stays full.
    // Twice: once to refuse, once to prove the refusal did not change what
    // the second attempt meets. More would only multiply a leak the first
    // pair already detects.
    const std::size_t before = store_.page_count();
    for (int i = 0; i < 2; ++i) {
        const std::string out = Run("CREATE INDEX over" + std::to_string(i) + " ON t (owner)")
                                    .response;
        EXPECT_EQ(out.substr(0, 3), "ERR") << out;
        EXPECT_NE(out.find("the table is full"), std::string::npos) << out;
    }
    EXPECT_EQ(store_.page_count(), before)
        << "a refusal allocated pages; that is the leak this test exists for";
}

TEST_F(IndexDdlTest, UniqueIsRefusedThroughTheDispatcherToo) {
    const std::string out = Run("CREATE UNIQUE INDEX ix ON t (a)").response;
    EXPECT_EQ(out.substr(0, 3), "ERR") << out;
    EXPECT_NE(out.find("IX11"), std::string::npos) << out;
}

}  // namespace
}  // namespace kds::exec
