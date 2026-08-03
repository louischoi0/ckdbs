#include "kds/stats/access_stats.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/plan_printer.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// `sys.access_stats` (docs/heap-and-tuple.md §7): how often each access
// *shape* ran, and when it last ran.
//
// The decision this file exists to pin is the keying. A shape is
// `(kind, relation, columns)` and **never the values** - `flag = 1` and
// `flag = 2` are one row. That is what bounds the relation by the schema
// instead of by the data, and it is the difference between a statistic that
// needs no eviction policy and one that needs the whole directory machinery
// Waystone has for its instances.

namespace kds::stats {
namespace {

class AccessStatsTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);

        ASSERT_EQ(Run("CREATE TABLE b (id int64, flag int64, name varchar) BTREE").substr(0, 7),
                  "CREATED");
        ASSERT_EQ(Run("CREATE TABLE h (id int64, flag int64, name varchar)").substr(0, 7),
                  "CREATED");
        for (int i = 1; i <= 6; ++i) {
            const std::string v = std::to_string(i % 3);
            ASSERT_EQ(Run("INSERT INTO b VALUES (" + v + ", 'n')").substr(0, 8), "INSERTED");
            ASSERT_EQ(Run("INSERT INTO h VALUES (" + v + ", 'n')").substr(0, 8), "INSERTED");
        }
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    catalog::Oid OidOf(const std::string& name) {
        auto oid = boot_->catalog.FindTableOidByName(name);
        EXPECT_TRUE(oid.ok());
        return oid.ok() ? oid.value() : 0;
    }

    // The row for one shape, or nullopt.
    std::optional<catalog::SysAccessStatRow> Shape(exec::AccessKind kind, const std::string& rel,
                                                   std::uint64_t mask) {
        auto rows = boot_->catalog.ListAccessStats();
        EXPECT_TRUE(rows.ok());
        if (!rows.ok()) return std::nullopt;
        for (const catalog::SysAccessStatRow& row : rows.value()) {
            if (row.kind == exec::StoredAccessKind(kind) && row.rel_id == OidOf(rel) &&
                row.column_mask == mask) {
                return row;
            }
        }
        return std::nullopt;
    }

    static constexpr std::uint64_t kPk = 1ull << 0;
    static constexpr std::uint64_t kFlag = 1ull << 1;
    static constexpr std::uint64_t kName = 1ull << 2;

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<server::CommandDispatcher> dispatcher_;
};

// ---- The keying decision -------------------------------------------------

TEST_F(AccessStatsTest, TheSameShapeWithDifferentValuesIsOneRow) {
    // **The decision this whole relation rests on.** Three executions, three
    // different literals, one shape - because the question it answers is
    // "which columns does this workload search on", and the answer does not
    // change with the literal.
    //
    // Keying on values instead would make the row count grow with the data,
    // which is the unbounded population Waystone needs a directory and an
    // eviction policy to survive. Here there is nothing to evict.
    Run("SELECT * FROM b WHERE flag = 0");
    Run("SELECT * FROM b WHERE flag = 1");
    Run("SELECT * FROM b WHERE flag = 2");

    auto shape = Shape(exec::AccessKind::kFilterScan, "b", kFlag);
    ASSERT_TRUE(shape.has_value());
    EXPECT_EQ(shape->use_count, 3u);
}

TEST_F(AccessStatsTest, DifferentColumnsAreDifferentShapes) {
    Run("SELECT * FROM b WHERE flag = 1");
    Run("SELECT * FROM b WHERE name = 'n'");

    EXPECT_TRUE(Shape(exec::AccessKind::kFilterScan, "b", kFlag).has_value());
    EXPECT_TRUE(Shape(exec::AccessKind::kFilterScan, "b", kName).has_value());
    // ...and a two-column filter is a third shape, not either of the above.
    Run("SELECT * FROM b WHERE flag = 1 AND name = 'n'");
    auto both = Shape(exec::AccessKind::kFilterScan, "b", kFlag | kName);
    ASSERT_TRUE(both.has_value());
    EXPECT_EQ(both->use_count, 1u);
}

TEST_F(AccessStatsTest, DifferentRelationsAreDifferentShapes) {
    Run("SELECT * FROM b WHERE flag = 1");
    Run("SELECT * FROM h WHERE flag = 1");
    EXPECT_TRUE(Shape(exec::AccessKind::kFilterScan, "b", kFlag).has_value());
    EXPECT_TRUE(Shape(exec::AccessKind::kFilterScan, "h", kFlag).has_value());
}

// ---- Every kind, through one interface -----------------------------------

TEST_F(AccessStatsTest, EveryAccessKindIsRecordedTheSameWay) {
    // The point of "the same interface as lookup or fullscan": a Lookup is
    // counted by exactly the code that counts a FilterScan, so the numbers
    // are comparable. A `switch` on kind in the recorder would let them
    // drift into meaning different things.
    Run("SELECT * FROM b WHERE id = 2");                 // Lookup
    Run("SELECT * FROM b WHERE id BETWEEN 1 AND 3");     // Range
    Run("SELECT * FROM b WHERE flag = 1");               // FilterScan
    Run("SELECT * FROM b");                              // Scan

    EXPECT_TRUE(Shape(exec::AccessKind::kLookup, "b", kPk).has_value());
    EXPECT_TRUE(Shape(exec::AccessKind::kRange, "b", kPk).has_value());
    EXPECT_TRUE(Shape(exec::AccessKind::kFilterScan, "b", kFlag).has_value());
    // A bare scan is steered by nothing, so its shape has no columns.
    EXPECT_TRUE(Shape(exec::AccessKind::kScan, "b", 0).has_value());
}

TEST_F(AccessStatsTest, AJoinRecordsBothOfItsSteps) {
    Run("SELECT a.id FROM b AS a JOIN h AS c ON a.id = c.id WHERE a.flag = 1");
    // The driving relation is filter-scanned; the inner one is probed.
    EXPECT_TRUE(Shape(exec::AccessKind::kFilterScan, "b", kFlag).has_value());
    EXPECT_TRUE(Shape(exec::AccessKind::kProbe, "h", kPk).has_value());
}

// ---- Heat --------------------------------------------------------------

TEST_F(AccessStatsTest, CountAndLastSeenBothAdvance) {
    Run("SELECT * FROM b WHERE flag = 1");
    auto first = Shape(exec::AccessKind::kFilterScan, "b", kFlag);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->use_count, 1u);

    Run("SELECT * FROM b WHERE flag = 1");
    auto second = Shape(exec::AccessKind::kFilterScan, "b", kFlag);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->use_count, 2u);
    EXPECT_GE(second->last_seen, first->last_seen);
}

TEST_F(AccessStatsTest, AFailedStatementRecordsNothing) {
    const std::size_t before = boot_->catalog.ListAccessStats().value().size();
    EXPECT_EQ(Run("SELECT * FROM nosuchtable WHERE id = 1").substr(0, 3), "ERR");
    EXPECT_EQ(boot_->catalog.ListAccessStats().value().size(), before);
}

// ---- The stored kind ------------------------------------------------------

TEST_F(AccessStatsTest, TheStoredKindIsNeverZeroAndRoundTrips) {
    // A zeroed catalog row decodes `kind` to 0, so no real kind may map to
    // it - the same collision `stmt_class` had to be taught to avoid, and
    // the reason neither mapping is a cast.
    for (exec::AccessKind kind :
         {exec::AccessKind::kLookup, exec::AccessKind::kProbe, exec::AccessKind::kRange,
          exec::AccessKind::kFilterScan, exec::AccessKind::kScan}) {
        const std::uint8_t stored = exec::StoredAccessKind(kind);
        EXPECT_NE(stored, catalog::kAccessKindUnset);
        auto back = exec::AccessKindOfStored(stored);
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(*back, kind);
    }
    EXPECT_FALSE(exec::AccessKindOfStored(catalog::kAccessKindUnset).has_value());
}

}  // namespace
}  // namespace kds::stats
