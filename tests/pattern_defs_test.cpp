#include "kds/stats/pattern_defs.hpp"

#include <gtest/gtest.h>

#include <string>

#include "kds/catalog/well_known.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/varheap.hpp"

// sys.pattern_defs, the one catalog relation stored in ordinary user tuple
// format. What is worth pinning here is not that a row round-trips - the row
// codec's own tests cover that - but the three things that are true of this
// relation and of no other catalog one: it exists at bootstrap with a
// var-heap, a body longer than the inline cell survives the spill, and a
// deleted definition is *gone* rather than delete-marked.

namespace kds::stats {
namespace {

class PatternDefsTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(catalog_.Bootstrap().ok()); }

    // 128 = kds::server::kFirstUserPageId, so pages allocated by CreateNew()
    // (this relation's var-heap chain among them) never collide with the
    // fixed catalog pages.
    storage::InMemoryPageStore store_{128};
    catalog::Catalog catalog_{store_};
};

TEST_F(PatternDefsTest, BootstrapCreatesTheRelationWithAVarHeap) {
    auto access = catalog_.InitTableAccess(catalog::kSysPatternDefsTable);
    ASSERT_TRUE(access.ok()) << access.status().message();

    // Five columns: the Keystone pk the spec's list omits (invariant 11),
    // then pattern_id, the materialized arity, and the two text columns.
    ASSERT_EQ(access.value()->schema.columns.size(), 5u);
    EXPECT_EQ(catalog::NameView(access.value()->schema.columns[0].name), "id");
    EXPECT_EQ(catalog::NameView(access.value()->schema.columns[1].name), "pattern_id");
    EXPECT_EQ(catalog::NameView(access.value()->schema.columns[2].name), "param_count");
    EXPECT_EQ(catalog::NameView(access.value()->schema.columns[3].name), "name");
    EXPECT_EQ(catalog::NameView(access.value()->schema.columns[4].name), "source_text");

    // It can spill, so it has a chain. A relation of plain integers would
    // get kInvalidPageId and no page at all.
    EXPECT_NE(access.value()->varheap_page_id, kInvalidPageId);
    EXPECT_EQ(access.value()->desc_page_id, catalog::kCatalogPagePatternDefs);
}

TEST_F(PatternDefsTest, ADefinitionRoundTripsByNameAndByPatternId) {
    // The **whole declaration**, not just the body: it is the canon a
    // fingerprint version bump re-registers from, and the declared types and
    // WITH options are recoverable from nothing else.
    const std::string decl =
        "CREATE PATTERN acct($flag bool) WITH (pinned = on) "
        "OF SELECT id FROM account AS a WHERE a.flag = $flag";
    ASSERT_TRUE(InsertPatternDef(catalog_, store_, nullptr, 0xDEADBEEFCAFEF00Dull, "acct", decl, 1).ok());

    auto by_name = FindPatternDefByName(catalog_, store_, "acct");
    ASSERT_TRUE(by_name.ok());
    ASSERT_TRUE(by_name.value().has_value());
    EXPECT_EQ(by_name.value()->pattern_id, 0xDEADBEEFCAFEF00Dull)
        << "a uint64 pattern_id must survive the upper half of the range";
    // Verbatim, sigils included: a normalized copy would re-register the
    // pattern under an id that no longer matches the traffic it was written
    // for.
    EXPECT_EQ(by_name.value()->source_text, decl);
    // The materialized arity, stored rather than rederived so it cannot come
    // to disagree with what an older build hashed.
    EXPECT_EQ(by_name.value()->param_count, 1u);

    auto by_id = FindPatternDefByPatternId(catalog_, store_, 0xDEADBEEFCAFEF00Dull);
    ASSERT_TRUE(by_id.ok());
    ASSERT_TRUE(by_id.value().has_value());
    EXPECT_EQ(by_id.value()->name, "acct");
}

TEST_F(PatternDefsTest, NameLookupIsCaseInsensitive) {
    ASSERT_TRUE(InsertPatternDef(catalog_, store_, nullptr, 1, "AcctTrades", "SELECT * FROM t", 0).ok());

    auto found = FindPatternDefByName(catalog_, store_, "accttrades");
    ASSERT_TRUE(found.ok());
    EXPECT_TRUE(found.value().has_value())
        << "every other identifier in this engine folds; a DROP that missed "
           "because of case would strand the declaration";

    auto missing = FindPatternDefByName(catalog_, store_, "other");
    ASSERT_TRUE(missing.ok());
    EXPECT_FALSE(missing.value().has_value()) << "an absence is not an error";
}

TEST_F(PatternDefsTest, ABodyLongerThanAnInlineCellSpillsAndComesBackWhole) {
    // Well past kds.inline_cell_width, so both text columns take the
    // var-heap path and the decode has to resolve two pending spills - the
    // ordering I15's R1 forces.
    const std::string body(4000, 'x');
    ASSERT_TRUE(InsertPatternDef(catalog_, store_, nullptr, 7, "big", body, 0).ok());

    auto found = FindPatternDefByPatternId(catalog_, store_, 7);
    ASSERT_TRUE(found.ok());
    ASSERT_TRUE(found.value().has_value());
    EXPECT_EQ(found.value()->source_text.size(), body.size());
    EXPECT_EQ(found.value()->source_text, body);
}

TEST_F(PatternDefsTest, ABodyLargerThanOneVarHeapPageIsRefusedRatherThanChained) {
    // The spilled-value size cap is an open decision and this does not
    // settle it: one var-heap page is what fits without a multi-page
    // representation, and anything larger is Unsupported.
    const std::string too_long(varheap::kMaxValueSize + 1, 'x');
    Status s = InsertPatternDef(catalog_, store_, nullptr, 9, "huge", too_long, 0);
    EXPECT_EQ(s.code(), StatusCode::kUnsupported);
    EXPECT_NE(s.message().find(std::to_string(varheap::kMaxValueSize)), std::string::npos)
        << "the message has to name the limit; the client wrote the body";
}

TEST_F(PatternDefsTest, DeleteRemovesTheRowRatherThanMarkingIt) {
    ASSERT_TRUE(InsertPatternDef(catalog_, store_, nullptr, 11, "gone", "SELECT * FROM t", 0).ok());
    ASSERT_TRUE(DeletePatternDef(catalog_, store_, nullptr, 11).ok());

    auto found = FindPatternDefByName(catalog_, store_, "gone");
    ASSERT_TRUE(found.ok());
    EXPECT_FALSE(found.value().has_value());

    // The point of retiring rather than delete-marking: catalog reads have
    // no snapshot to filter a mark against, so re-declaring the same name
    // has to succeed.
    EXPECT_TRUE(InsertPatternDef(catalog_, store_, nullptr, 12, "gone", "SELECT * FROM u", 0).ok());
    auto again = FindPatternDefByName(catalog_, store_, "gone");
    ASSERT_TRUE(again.ok());
    ASSERT_TRUE(again.value().has_value());
    EXPECT_EQ(again.value()->pattern_id, 12u);

    EXPECT_EQ(DeletePatternDef(catalog_, store_, nullptr, 999).code(), StatusCode::kNotFound);
}

TEST_F(PatternDefsTest, ListReturnsEveryDefinitionInChainOrder) {
    ASSERT_TRUE(InsertPatternDef(catalog_, store_, nullptr, 1, "one", "SELECT * FROM a", 0).ok());
    ASSERT_TRUE(InsertPatternDef(catalog_, store_, nullptr, 2, "two", "SELECT * FROM b", 0).ok());

    auto all = ListPatternDefs(catalog_, store_);
    ASSERT_TRUE(all.ok());
    ASSERT_EQ(all.value().size(), 2u);
    EXPECT_EQ(all.value()[0].name, "one");
    EXPECT_EQ(all.value()[1].name, "two");
    // Keystone ids come from the relation's own persistent sequence, so
    // they are distinct and increasing.
    EXPECT_LT(all.value()[0].id, all.value()[1].id);
}

}  // namespace
}  // namespace kds::stats
