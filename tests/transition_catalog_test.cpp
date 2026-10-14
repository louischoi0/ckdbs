#include "kds/exec/transition_catalog.hpp"
#include "kds/exec/declaration_relation.hpp"
#include "kds/exec/varheap_sweep.hpp"

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// `sys.transitions` - the catalog half of BV (BV-S1,
// `instructions/v3.2.0/workorder-bv-transition-constraints.md`, BV-Q1 (a)).
//
// The relation is bootstrapped on fixed page 16, which was the first
// overflow-range id, so it cost superblock 23 -> 24 and every older file
// stops mounting. It shares sys.assertions' storage
// (`exec/declaration_relation.hpp`); these cells pin the transition face
// and that the two relations never see each other's rows.

namespace kds::exec {
namespace {

class TransitionCatalogTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
    }

    catalog::Catalog& catalog() { return boot_->catalog; }

    TransitionDef Def(std::string name, catalog::Oid target, std::uint16_t column,
                      std::string text) {
        TransitionDef def;
        auto id = catalog().AllocateRowId(catalog::kSysTransitionsTable);
        EXPECT_TRUE(id.ok()) << id.status().message();
        def.id = id.ok() ? id.value() : 0;
        def.target_oid = target;
        def.column_no = column;
        def.name = std::move(name);
        def.source_text = std::move(text);
        return def;
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
};

TEST_F(TransitionCatalogTest, BootstrapCreatesTheRelationOnItsFixedPage) {
    auto access = catalog().InitTableAccess(catalog::kSysTransitionsTable);
    ASSERT_TRUE(access.ok()) << access.status().message();
    ASSERT_EQ(access.value()->schema.columns.size(), 6u);
    EXPECT_EQ(catalog::NameView(access.value()->schema.columns[2].name), "column_no");
    EXPECT_EQ(catalog::NameView(access.value()->schema.columns[5].name), "source_text");
    EXPECT_EQ(access.value()->desc_page_id, catalog::kCatalogPageTransitions);
    EXPECT_NE(access.value()->varheap_page_id, kInvalidPageId);
    // The bump and the root it bought, asserted so neither is quietly
    // undone; `>=` for the reason assertion_catalog_test.cpp states.
    EXPECT_GE(server::kSuperBlockVersion, 24u);
    EXPECT_EQ(catalog::kCatalogPageTransitions, 16u);
}

TEST_F(TransitionCatalogTest, ADeclarationRoundTripsAndSpillsWhole) {
    // Longer than an inline cell, so the text spills and must come back
    // byte for byte through the var-heap.
    std::string text = "CREATE TRANSITION t ON orders(status) ON INSERT ('NEW') ON UPDATE (";
    for (int i = 0; i < 40; ++i) text += "'S" + std::to_string(i) + "' => 'S" +
                                         std::to_string(i + 1) + "', ";
    text += "'NEW' => 'S0')";
    ASSERT_TRUE(InsertTransition(catalog(), store_, nullptr, Def("Flow", 5000, 3, text)).ok());

    auto found = FindTransitionByName(catalog(), store_, "flow");
    ASSERT_TRUE(found.ok()) << found.status().message();
    ASSERT_TRUE(found.value().has_value());
    EXPECT_EQ(found.value()->name, "Flow");
    EXPECT_EQ(found.value()->target_oid, 5000u);
    EXPECT_EQ(found.value()->column_no, 3u);
    EXPECT_EQ(found.value()->source_text, text);
}

TEST_F(TransitionCatalogTest, NamesAreUniqueAndTheTwoDeclarationRelationsAreApart) {
    ASSERT_TRUE(InsertTransition(catalog(), store_, nullptr, Def("a", 5000, 1, "x")).ok());
    const Status dup = InsertTransition(catalog(), store_, nullptr, Def("A", 5001, 2, "y"));
    EXPECT_EQ(dup.code(), StatusCode::kAlreadyExists);

    auto on = TransitionsOnRelation(catalog(), store_, 5000);
    ASSERT_TRUE(on.ok());
    EXPECT_EQ(on.value().size(), 1u);

    // A transition is not an assertion: the shared storage is per relation.
    auto assertions = ScanDeclarations(catalog(), store_, catalog::kSysAssertionsTable);
    ASSERT_TRUE(assertions.ok());
    EXPECT_TRUE(assertions.value().empty());
}

TEST_F(TransitionCatalogTest, DeleteRetiresTheRowAndFreesTheName) {
    ASSERT_TRUE(InsertTransition(catalog(), store_, nullptr, Def("a", 5000, 1, "x")).ok());
    auto gone = DeleteTransition(catalog(), store_, nullptr, "A");
    ASSERT_TRUE(gone.ok()) << gone.status().message();
    EXPECT_EQ(gone.value().target_oid, 5000u);
    EXPECT_EQ(DeleteTransition(catalog(), store_, nullptr, "a").status().code(),
              StatusCode::kNotFound);
    EXPECT_TRUE(InsertTransition(catalog(), store_, nullptr, Def("a", 5000, 1, "x")).ok());
}

TEST_F(TransitionCatalogTest, TheMountSweepCollectsADroppedTransitionsSpilledText) {
    // The delete retires the row and leaves its spilled text on the
    // var-heap; the sweep is what collects it (`catalog_spills.hpp`'s list).
    // A Transition missing from that list leaks for good - the S1 review's
    // finding, pinned.
    const std::string text(400, 'x');
    ASSERT_TRUE(InsertTransition(catalog(), store_, nullptr, Def("keep", 5000, 1, text)).ok());
    ASSERT_TRUE(InsertTransition(catalog(), store_, nullptr, Def("gone", 5000, 2, text)).ok());
    ASSERT_TRUE(DeleteTransition(catalog(), store_, nullptr, "gone").ok());

    auto swept = SweepUnownedSpills(catalog(), store_, nullptr);
    ASSERT_TRUE(swept.ok()) << swept.status().message();
    EXPECT_GE(swept.value().released, 1u);
    EXPECT_GE(swept.value().retained, 1u);

    auto kept = FindTransitionByName(catalog(), store_, "keep");
    ASSERT_TRUE(kept.ok());
    ASSERT_TRUE(kept.value().has_value());
    EXPECT_EQ(kept.value()->source_text, text);
}

TEST_F(TransitionCatalogTest, ADeclarationSurvivesAReopenOfTheSameStore) {
    ASSERT_TRUE(InsertTransition(catalog(), store_, nullptr, Def("a", 5000, 1, "x")).ok());
    catalog::Catalog reopened(store_);
    auto defs = ListTransitions(reopened, store_);
    ASSERT_TRUE(defs.ok()) << defs.status().message();
    ASSERT_EQ(defs.value().size(), 1u);
    EXPECT_EQ(defs.value()[0].name, "a");
}

}  // namespace
}  // namespace kds::exec
