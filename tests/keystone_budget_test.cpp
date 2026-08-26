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

TEST(KeystoneBudgetTest, TheCapacityIsOneShortOfTwoToTheFortyBecauseZeroIsReserved) {
    // Id 0 means "unset" (rows.hpp), so the budget is [1, 2^40-1] and an
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
    // 2^40 inserts is not a test, and the surface only reads the number.
    void SetNextId(std::uint64_t next_id) {
        auto oid = boot_->catalog.FindTableOidByName("acct");
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

TEST_F(KeystoneBudgetSurfaceTest, DescribeReportsConsumptionBesideTheSequence) {
    ASSERT_EQ(Run("INSERT INTO acct VALUES (1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO acct VALUES (2)").substr(0, 8), "INSERTED");

    const std::string out = Run("DESCRIBE acct");
    EXPECT_NE(out.find("next_id=3"), std::string::npos) << out;
    // Two ids spent, and the remaining count is stated rather than left to
    // be worked out from the ceiling - which is K-M4's whole acceptance.
    EXPECT_NE(out.find("ids_issued=2"), std::string::npos) << out;
    EXPECT_NE(out.find("ids_remaining=1099511627773"), std::string::npos) << out;
    EXPECT_NE(out.find("budget_used=0.000%"), std::string::npos) << out;
    EXPECT_EQ(out.find("budget_warning"), std::string::npos) << out;
}

TEST_F(KeystoneBudgetSurfaceTest, ShowBudgetListsEveryRelationIncludingTheCatalogsOwn) {
    const std::string out = Run("SHOW BUDGET");
    EXPECT_EQ(out.substr(0, 10), "relations=") << out;
    EXPECT_NE(out.find("rel=acct issued=0"), std::string::npos) << out;
    // The two catalog relations that genuinely issue ids. Hiding them would
    // hide the only consumption an operator does not control.
    EXPECT_NE(out.find("rel=patterns "), std::string::npos) << out;
    EXPECT_NE(out.find("rel=pattern_defs "), std::string::npos) << out;
    // The threshold is stated, so the listing explains its own warn column.
    EXPECT_NE(out.find("warn_at=90.000%"), std::string::npos) << out;
    EXPECT_NE(out.find("warning=0"), std::string::npos) << out;
}

TEST_F(KeystoneBudgetSurfaceTest, CrossingTheThresholdIsVisibleInTheSummaryLine) {
    // The acceptance criterion, literally: an operator must not have to
    // read every row to learn a relation is in trouble.
    const std::string before = Run("SHOW BUDGET");
    EXPECT_NE(before.find("warning=0 exhausted=0"), std::string::npos) << before;

    SetNextId(static_cast<std::uint64_t>(
        static_cast<double>(catalog::kKeystoneBudgetCapacity) * 0.95));

    const std::string after = Run("SHOW BUDGET");
    EXPECT_NE(after.find("warning=1 exhausted=0"), std::string::npos) << after;
    EXPECT_NE(after.find("rel=acct"), std::string::npos) << after;
    EXPECT_NE(after.find("warn=yes"), std::string::npos) << after;
    EXPECT_NE(after.find("used=95.000%"), std::string::npos) << after;

    // And on the per-relation surface too.
    EXPECT_NE(Run("DESCRIBE acct").find("budget_warning=yes"), std::string::npos);
}

TEST_F(KeystoneBudgetSurfaceTest, AnExhaustedRelationSaysSoOnBothSurfaces) {
    SetNextId(kMaxKeystoneId + 1);

    const std::string out = Run("SHOW BUDGET");
    EXPECT_NE(out.find("warning=1 exhausted=1"), std::string::npos) << out;
    EXPECT_NE(out.find("remaining=0 used=100.000% warn=yes exhausted=yes"), std::string::npos)
        << out;

    EXPECT_NE(Run("DESCRIBE acct").find("budget_exhausted=yes"), std::string::npos);

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
