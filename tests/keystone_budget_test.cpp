#include <cstdint>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/catalog/keystone_budget.hpp"
#include "kds/catalog/rows.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/keystone.hpp"
#include "view_text.hpp"

// K-M4: an operator can see budget consumption without arithmetic, and
// crossing the threshold is visible (`docs/rules/keystoneid-invariant.md` §5).
//
// Two halves, tested separately on purpose. The arithmetic is a pure
// function of one integer, so its edges - the kFirstRowId offset, the
// inclusive ceiling, exhaustion - are testable without a database at all.
// The surfaces are then only responsible for rendering it, which is the
// split that keeps K-M2 cheap: when the source becomes a persisted HWM,
// the first half does not move.

namespace kds {
namespace {

// ---- The arithmetic ------------------------------------------------------

TEST(KeystoneBudgetTest, AFreshRelationHasSpentNothingAndCanIssueEverything) {
    const catalog::KeystoneBudget budget = catalog::BudgetOf(catalog::kFirstRowId);
    EXPECT_EQ(budget.issued, 0u);
    EXPECT_EQ(budget.remaining, catalog::kKeystoneBudgetCapacity);
    EXPECT_EQ(budget.capacity, catalog::kKeystoneBudgetCapacity);
    EXPECT_DOUBLE_EQ(budget.used_fraction, 0.0);
    EXPECT_FALSE(budget.warn);
    EXPECT_FALSE(budget.exhausted);
}

TEST(KeystoneBudgetTest, TheCapacityIsOneShortOfTheIdSpaceBecauseZeroIsReserved) {
    // Id 0 means "unset" (rows.hpp), so the budget is [1, 2^48-1] and an
    // off-by-one here is an off-by-one in the warning threshold.
    EXPECT_EQ(catalog::kKeystoneBudgetCapacity, kMaxKeystoneId);
    EXPECT_EQ(catalog::kFirstRowId, 1u);
}

TEST(KeystoneBudgetTest, IssuedCountsIdsSpentNotRowsLiving) {
    // The distinction K4 rests on: gaps are spent budget. Nothing here can
    // observe rows, which is the point - the function takes a sequence
    // position and says what it cost.
    EXPECT_EQ(catalog::BudgetOf(1).issued, 0u);
    EXPECT_EQ(catalog::BudgetOf(2).issued, 1u);
    EXPECT_EQ(catalog::BudgetOf(1001).issued, 1000u);
}

TEST(KeystoneBudgetTest, TheLastIssuableIdStillCountsAsRemaining) {
    // `AllocateRowId` refuses on `id > kMaxKeystoneId`, so at
    // next_id == kMaxKeystoneId one id is still issuable. Reporting 0 here
    // would tell an operator the relation was dead a row early.
    const catalog::KeystoneBudget budget = catalog::BudgetOf(kMaxKeystoneId);
    EXPECT_EQ(budget.remaining, 1u);
    EXPECT_FALSE(budget.exhausted);
    EXPECT_TRUE(budget.warn);
}

TEST(KeystoneBudgetTest, PastTheCeilingIsExhaustedRatherThanWrapped) {
    const catalog::KeystoneBudget budget = catalog::BudgetOf(kMaxKeystoneId + 1);
    EXPECT_EQ(budget.remaining, 0u);
    EXPECT_TRUE(budget.exhausted);
    EXPECT_TRUE(budget.warn);
    EXPECT_DOUBLE_EQ(budget.used_fraction, 1.0);
    // Capped, not run past: `issued <= capacity` for every input.
    EXPECT_EQ(budget.issued, budget.capacity);
}

TEST(KeystoneBudgetTest, ACorruptSequenceBelowTheFirstIdRendersRatherThanWrapping) {
    // next_id 0 is not reachable through AllocateRowId, but an inspection
    // surface exists for the case where something is wrong. The unsigned
    // subtraction that would have produced 2^64-1 is what this guards.
    const catalog::KeystoneBudget budget = catalog::BudgetOf(0);
    EXPECT_EQ(budget.issued, 0u);
    EXPECT_EQ(budget.remaining, catalog::kKeystoneBudgetCapacity);
    EXPECT_FALSE(budget.exhausted);
}

TEST(KeystoneBudgetTest, TheWarningTripsAtTheThresholdAndNotBefore) {
    const auto at = static_cast<std::uint64_t>(
        static_cast<double>(catalog::kKeystoneBudgetCapacity) *
        catalog::kKeystoneBudgetWarnFraction);

    // A little under, a little over. The exact boundary id is subject to
    // double rounding, so the test brackets it rather than pinning it -
    // what matters is that the flag is monotonic in consumption.
    EXPECT_FALSE(catalog::BudgetOf(at - 1000000).warn);
    EXPECT_TRUE(catalog::BudgetOf(at + 1000000).warn);
}

// ---- The surfaces --------------------------------------------------------

class KeystoneBudgetSurfaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
        ASSERT_EQ(Run("CREATE TABLE acct (id int64, tier int64)").substr(0, 7), "CREATED");
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    // Drives `acct`'s sequence to `next_id` by writing the catalog row -
    // 2^48 inserts is not a test, and the surface only reads the number.
    void SetNextId(std::uint64_t next_id) {
        auto oid = boot_->catalog.FindTableOidByName(::kds::catalog::kNamespacePublic, "acct");
        ASSERT_TRUE(oid.ok()) << oid.status().message();
        auto bytes = store_.Get(catalog::kCatalogPageTables);
        ASSERT_TRUE(bytes.ok());
        heap::PageView page(bytes.value().bytes());
        for (std::uint16_t i = 0; i < page.slot_count(); ++i) {
            auto tuple = page.ReadTuple(i);
            if (!tuple.ok()) continue;
            auto row = catalog::SysTableRow::Decode(tuple.value().payload);
            ASSERT_TRUE(row.ok());
            if (row.value().oid != oid.value()) continue;
            row.value().next_id = next_id;
            const auto encoded = row.value().Encode();
            ASSERT_TRUE(
                page.OverwriteTuple(i, encoded, tuple.value().trx_id, tuple.value().undo_ptr)
                    .ok());
            return;
        }
        FAIL() << "no sys.tables row for acct";
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<server::CommandDispatcher> dispatcher_;
};

// `acct`'s budget column in `sys.relation_stats`.
std::optional<std::string> Budget(server::CommandDispatcher& d, std::string_view column) {
    return testing_view::RelationField(d, "relation_stats", "acct", column);
}

TEST_F(KeystoneBudgetSurfaceTest, TheRelationsRowReportsConsumptionBesideTheSequence) {
    ASSERT_EQ(Run("INSERT INTO acct VALUES (1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO acct VALUES (2)").substr(0, 8), "INSERTED");

    EXPECT_EQ(Budget(*dispatcher_, "next_id"), "3");
    // Two ids spent, and the remaining count is stated rather than left to
    // be worked out from the ceiling - which is K-M4's whole acceptance.
    EXPECT_EQ(Budget(*dispatcher_, "ids_issued"), "2");
    EXPECT_EQ(Budget(*dispatcher_, "ids_remaining"), "281474976710653");
    EXPECT_EQ(Budget(*dispatcher_, "budget_used_ppm"), "0");
    EXPECT_EQ(Budget(*dispatcher_, "budget_warning"), "0");
}

TEST_F(KeystoneBudgetSurfaceTest, ShowBudgetListsEveryRelationIncludingTheCatalogsOwn) {
    const std::string out = Run("SHOW BUDGET");
    const auto shown = testing_view::ReadViewText(out);
    EXPECT_EQ(shown.Find("name", "acct", "ids_issued"), "0") << out;
    // Catalog relations that genuinely issue ids. Hiding them would hide
    // the only consumption an operator does not control. `pattern_defs` was
    // one of these until declared patterns were withdrawn on 2026-08-31.
    EXPECT_TRUE(shown.Find("name", "patterns", "oid").has_value()) << out;
    EXPECT_TRUE(shown.Find("name", "assertions", "oid").has_value()) << out;
    // K4's per-relation budget, 2^48 - 1: what an unused relation has left.
    EXPECT_EQ(shown.Find("name", "acct", "ids_remaining"), "281474976710655") << out;
}

TEST_F(KeystoneBudgetSurfaceTest, CrossingTheThresholdIsOneFilterAway) {
    // The acceptance criterion: an operator must not have to read every row
    // to learn a relation is in trouble. The summary line is gone (BO-Q3:
    // no aggregation over a view); a filter on the flag is the answer.
    const std::string warned = "SELECT name FROM sys.relation_stats WHERE budget_warning = 1";
    EXPECT_TRUE(testing_view::ReadViewText(Run(warned)).rows.empty());

    SetNextId(static_cast<std::uint64_t>(
        static_cast<double>(catalog::kKeystoneBudgetCapacity) * 0.95));

    EXPECT_EQ(testing_view::ReadViewText(Run(warned)).Column("name"),
              std::vector<std::string>{"acct"});
    // Floored: `next_id` at 95 % of the capacity has issued one id fewer.
    EXPECT_EQ(Budget(*dispatcher_, "budget_used_ppm"), "949999");
    EXPECT_EQ(Budget(*dispatcher_, "budget_exhausted"), "0");
}

TEST_F(KeystoneBudgetSurfaceTest, AnExhaustedRelationSaysSo) {
    SetNextId(kMaxKeystoneId + 1);

    EXPECT_EQ(Budget(*dispatcher_, "ids_remaining"), "0");
    EXPECT_EQ(Budget(*dispatcher_, "budget_used_ppm"), "1000000");
    EXPECT_EQ(Budget(*dispatcher_, "budget_warning"), "1");
    EXPECT_EQ(Budget(*dispatcher_, "budget_exhausted"), "1");

    // And the report is true: the next insert is refused, so the surface
    // and the allocator agree about what "exhausted" means.
    EXPECT_EQ(Run("INSERT INTO acct VALUES (1)").substr(0, 4), "ERR ");
}

TEST_F(KeystoneBudgetSurfaceTest, TheListingIsOneLineWithEscapedSeparators) {
    // Same wire contract as DESCRIBE, SHOW PAGE and SELECT: one response
    // line, "\n" escaped, never a raw newline byte.
    const std::string out = Run("SHOW BUDGET");
    EXPECT_EQ(out.find('\n'), std::string::npos) << out;
    EXPECT_NE(out.find("\\n"), std::string::npos) << out;
}

TEST_F(KeystoneBudgetSurfaceTest, AnUnknownShowTargetIsStillRejected) {
    EXPECT_EQ(Run("SHOW BUDGETS").substr(0, 4), "ERR ");
}

}  // namespace
}  // namespace kds
