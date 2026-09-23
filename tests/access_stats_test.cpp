#include "kds/stats/access_stats.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/plan_printer.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// `sys.access_stats` (docs/spec/heap-and-tuple.md §7): how often each access
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
        // **HEAP, pinned since SUS-1**: `b` above is the btree half and this
        // is the heap one; the flipped default would make them the same pair.
        ASSERT_EQ(Run("CREATE TABLE h (id int64, flag int64, name varchar) HEAP").substr(0, 7),
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

// ---- Secondary indexes (docs/spec/index.md §8, workplan IX16) -----------

TEST_F(AccessStatsTest, AnIndexAccessIsRecordedByTheSameCallAsEveryOther) {
    // IX16's whole claim: the two new kinds needed no recording code, because
    // the recorder has no per-kind branch. Checked rather than assumed - the
    // kind reaches `sys.access_stats` through `StoredAccessKind`, and a kind
    // added without a number there records as `kAccessKindUnset` and merges
    // with every other unmapped one.
    ASSERT_EQ(Run("CREATE INDEX b_flag ON b (flag)").substr(0, 7), "CREATED");

    Run("SELECT * FROM b WHERE flag = 1");
    Run("SELECT * FROM b WHERE flag BETWEEN 1 AND 2");

    EXPECT_TRUE(Shape(exec::AccessKind::kIndexProbe, "b", kFlag).has_value());
    EXPECT_TRUE(Shape(exec::AccessKind::kIndexRange, "b", kFlag).has_value());
}

TEST_F(AccessStatsTest, TheSameStatementIsADifferentShapeOnceItHasAnIndex) {
    // The distinction §7 says the statistics exist to draw, now visible for
    // the case that fixes it. `h` has no index and `b` does, so one relation
    // reports "searched every row for a few" and the other "descended an
    // index for them" - which is the first pair a physical optimizer could
    // act on.
    ASSERT_EQ(Run("CREATE INDEX b_flag ON b (flag)").substr(0, 7), "CREATED");

    Run("SELECT * FROM b WHERE flag = 1");
    Run("SELECT * FROM h WHERE flag = 1");

    EXPECT_TRUE(Shape(exec::AccessKind::kIndexProbe, "b", kFlag).has_value());
    EXPECT_TRUE(Shape(exec::AccessKind::kFilterScan, "h", kFlag).has_value());
    // And the indexed relation records no filter scan for that column: the
    // two are different shapes, not one shape counted twice.
    EXPECT_FALSE(Shape(exec::AccessKind::kFilterScan, "b", kFlag).has_value());
}

TEST_F(AccessStatsTest, AnIndexShapeNamesThePinnedKeyColumnsAndNoOthers) {
    // The columns the access was assigned *for*, not every column the
    // residual happens to filter - the same rule kCabinProbe follows.
    // Reporting them all would merge this with the filter scan and lose the
    // distinction above.
    ASSERT_EQ(Run("CREATE TABLE c (id int64, a int64, x int64, y int64) BTREE").substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("INSERT INTO c VALUES (1, 2, 3)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("CREATE INDEX c_ax ON c (a, x)").substr(0, 7), "CREATED");

    const std::uint64_t kA = 1ull << 1;
    const std::uint64_t kX = 1ull << 2;
    const std::uint64_t kY = 1ull << 3;

    // Entered by `a` alone; `y` is residual and must not appear.
    Run("SELECT id FROM c WHERE a = 2 AND y = 4");
    EXPECT_TRUE(Shape(exec::AccessKind::kIndexProbe, "c", kA).has_value());
    EXPECT_FALSE(Shape(exec::AccessKind::kIndexProbe, "c", kA | kY).has_value());

    // Entered by both key columns: a different shape, which is the point.
    Run("SELECT id FROM c WHERE a = 2 AND x = 3");
    EXPECT_TRUE(Shape(exec::AccessKind::kIndexProbe, "c", kA | kX).has_value());
}

TEST_F(AccessStatsTest, ShowAccessNamesTheIndexKinds) {
    // The stored number and the rendered name are two mappings, and a kind
    // can be right in one and missing from the other - `SHOW ACCESS` would
    // then print `?` for a shape the catalog holds perfectly well.
    ASSERT_EQ(Run("CREATE INDEX b_flag ON b (flag)").substr(0, 7), "CREATED");
    Run("SELECT * FROM b WHERE flag = 1");
    Run("SELECT * FROM b WHERE flag BETWEEN 1 AND 2");

    const std::string shown = Run("SHOW ACCESS");
    EXPECT_NE(shown.find("kind=IndexProbe"), std::string::npos) << shown;
    EXPECT_NE(shown.find("kind=IndexRange"), std::string::npos) << shown;
    EXPECT_EQ(shown.find("kind=?"), std::string::npos) << shown;
}

TEST_F(AccessStatsTest, AnIndexShapeCountsUsesLikeAnyOther) {
    ASSERT_EQ(Run("CREATE INDEX b_flag ON b (flag)").substr(0, 7), "CREATED");
    for (int i = 0; i < 4; ++i) Run("SELECT * FROM b WHERE flag = " + std::to_string(i % 3));

    auto shape = Shape(exec::AccessKind::kIndexProbe, "b", kFlag);
    ASSERT_TRUE(shape.has_value());
    // Four executions, one shape - the values differ and the shape does not.
    EXPECT_EQ(shape->use_count, 4u);
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
    //
    // Enumerated by hand, which is the weakness of this test and the reason
    // it is written to be *extended*: kCabinProbe was missing here from the
    // day it landed, and the two index kinds were added with it in 2026-08.
    // A kind absent from this list is a kind whose round trip nobody checks.
    for (exec::AccessKind kind :
         {exec::AccessKind::kLookup, exec::AccessKind::kProbe, exec::AccessKind::kRange,
          exec::AccessKind::kCabinProbe, exec::AccessKind::kIndexProbe,
          exec::AccessKind::kIndexRange, exec::AccessKind::kFilterScan,
          exec::AccessKind::kScan}) {
        const std::uint8_t stored = exec::StoredAccessKind(kind);
        EXPECT_NE(stored, catalog::kAccessKindUnset);
        auto back = exec::AccessKindOfStored(stored);
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(*back, kind);
    }
    EXPECT_FALSE(exec::AccessKindOfStored(catalog::kAccessKindUnset).has_value());
}

// ---- One relation, whichever core recorded it (AT-S7) --------------------
//
// The three cells that stood in `access_batch_test.cpp` for CR7's wire -
// the fold by shape, the buffer's overflow count and a peer's fold reaching
// the row the local path writes - retire with the batch: a peer writes
// `sys.access_stats` itself, so there is no fold, no message and no drop.
// What has to stay true is the property the wire existed to preserve, and
// it is pinned here instead.

TEST_F(AccessStatsTest, TwoCoresRecordingOneShapeShareItsRow) {
    // Two catalogs over one store, which is what two cores are: each has
    // its own memo and both walk the same pages. A shape one of them admits
    // is the shape the other increments.
    catalog::Catalog peer(store_);
    ASSERT_TRUE(peer.RecordAccess(/*kind=*/1, /*rel_id=*/4242, /*mask=*/0b101, /*now=*/900).ok());
    ASSERT_TRUE(boot_->catalog.RecordAccess(1, 4242, 0b101, 950).ok());
    ASSERT_TRUE(peer.RecordAccess(1, 4242, 0b101, 990).ok());

    auto rows = boot_->catalog.ListAccessStats();
    ASSERT_TRUE(rows.ok()) << rows.status().message();
    std::size_t found = 0;
    for (const catalog::SysAccessStatRow& row : rows.value()) {
        if (row.rel_id != 4242) continue;
        ++found;
        // **The totals are what the done-condition names**: three
        // executions, one row, and `last_seen` the newest of them - which
        // is what a fold-then-apply produced and what a shared row has to
        // produce too.
        EXPECT_EQ(row.use_count, 3u);
        EXPECT_EQ(row.last_seen, 990u);
    }
    EXPECT_EQ(found, 1u) << "a second row for one shape splits its count and no later "
                            "increment reaches it - ForFirstRow stops at the first match";
}

// ---- CR6 / CB8: unlogged, and discarded when damaged ---------------------
//
// Carried over from `access_batch_test.cpp`, whose batch half AT-S7 retired.
// Nothing about these two changed: the relation is still the sole unlogged
// catalog relation, and a mount that finds it damaged still discards it.

class AccessStatsDurabilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_.emplace(store_);
        ASSERT_TRUE(catalog_->Bootstrap().ok());
        ASSERT_TRUE(catalog_->RecordAccess(/*kind=*/1, /*rel_id=*/5, /*mask=*/1, /*now=*/10).ok());
        ASSERT_TRUE(catalog_->RecordAccess(1, 6, 1, 11).ok());
    }

    storage::InMemoryPageStore store_{128};
    std::optional<catalog::Catalog> catalog_;
};

TEST_F(AccessStatsDurabilityTest, AnUndamagedRelationIsLeftAlone) {
    // The direction the discard must never err in: it runs at every mount,
    // so a false positive would empty a healthy statistic on every restart
    // and the optimizer would never accumulate anything.
    auto reset = catalog_->ResetAccessStatsIfDamaged();
    ASSERT_TRUE(reset.ok()) << reset.status().message();
    EXPECT_FALSE(reset.value()) << "a healthy relation was discarded";

    auto rows = catalog_->ListAccessStats();
    ASSERT_TRUE(rows.ok());
    EXPECT_EQ(rows.value().size(), 2u);
}

TEST_F(AccessStatsDurabilityTest, ABrokenChainIsDiscardedAndTheRelationWorksAgain) {
    // Damage the chain the way a half-applied grow would: a link to a page
    // that is not there. Nothing redoes this relation (CR6), so without the
    // discard the failure is permanent - every `RecordAccess` and every
    // `SHOW ACCESS` fails for the life of the file.
    {
        auto head = store_.Get(catalog::kCatalogPageAccessStats);
        ASSERT_TRUE(head.ok()) << head.status().message();
        heap::PageView page(head.value().bytes());
        page.set_next_page_id(9999);
    }
    ASSERT_FALSE(catalog_->ListAccessStats().ok())
        << "the fixture did not actually break the relation";

    auto reset = catalog_->ResetAccessStatsIfDamaged();
    ASSERT_TRUE(reset.ok()) << reset.status().message();
    EXPECT_TRUE(reset.value());

    // Empty, readable, and writable again - which is the whole claim:
    // invariant 8 prices a lost trail as performance, so an empty statistic
    // is a slower optimizer and never a wrong answer.
    auto rows = catalog_->ListAccessStats();
    ASSERT_TRUE(rows.ok()) << rows.status().message();
    EXPECT_TRUE(rows.value().empty());
    ASSERT_TRUE(catalog_->RecordAccess(1, 7, 1, 12).ok());
    EXPECT_EQ(catalog_->ListAccessStats().value().size(), 1u);
}

}  // namespace
}  // namespace kds::stats
