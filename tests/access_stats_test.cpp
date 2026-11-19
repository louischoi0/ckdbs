#include "kds/stats/access_stats.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <optional>
#include <string>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/plan_printer.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "view_text.hpp"

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
        // A second relation, for the cells that tell two apart.
        ASSERT_EQ(Run("CREATE TABLE h (id int64, flag int64, name varchar) BTREE").substr(0, 7),
                  "CREATED");
        for (int i = 1; i <= 6; ++i) {
            const std::string v = std::to_string(i % 3);
            ASSERT_EQ(Run("INSERT INTO b VALUES (" + v + ", 'n')").substr(0, 8), "INSERTED");
            ASSERT_EQ(Run("INSERT INTO h VALUES (" + v + ", 'n')").substr(0, 8), "INSERTED");
        }
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    catalog::Oid OidOf(const std::string& name) {
        auto oid = boot_->catalog.FindTableOidByName(::kds::catalog::kNamespacePublic, name);
        EXPECT_TRUE(oid.ok());
        return oid.ok() ? oid.value() : 0;
    }

    // The row for one shape, or nullopt.
    std::optional<AccessShape> Shape(exec::AccessKind kind, const std::string& rel,
                                     std::uint64_t mask) {
        for (const AccessShape& row : dispatcher_->access_stats().Snapshot()) {
            if (row.kind == kind && row.rel_id == OidOf(rel) && row.column_mask == mask) {
                return row;
            }
        }
        return std::nullopt;
    }

    static constexpr std::uint64_t kTupleKey = 1ull << 0;
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
    EXPECT_EQ(shape->uses, 3u);
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
    EXPECT_EQ(both->uses, 1u);
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

    EXPECT_TRUE(Shape(exec::AccessKind::kLookup, "b", kTupleKey).has_value());
    EXPECT_TRUE(Shape(exec::AccessKind::kRange, "b", kTupleKey).has_value());
    EXPECT_TRUE(Shape(exec::AccessKind::kFilterScan, "b", kFlag).has_value());
    // A bare scan is steered by nothing, so its shape has no columns.
    EXPECT_TRUE(Shape(exec::AccessKind::kScan, "b", 0).has_value());
}

// ---- Secondary indexes (docs/spec/index.md §8, workplan IX16) -----------

TEST_F(AccessStatsTest, AnIndexAccessIsRecordedByTheSameCallAsEveryOther) {
    // IX16's whole claim: the two new kinds needed no recording code, because
    // the recorder has no per-kind branch. Checked rather than assumed.
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
    // index for them" - the pair `SHOW ACCESS` reports apart.
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
    // The rendered name is a mapping of its own, and a kind missing from it
    // would print `?` for a shape the store holds perfectly well.
    ASSERT_EQ(Run("CREATE INDEX b_flag ON b (flag)").substr(0, 7), "CREATED");
    Run("SELECT * FROM b WHERE flag = 1");
    Run("SELECT * FROM b WHERE flag BETWEEN 1 AND 2");

    const std::string shown = Run("SELECT kind FROM sys.access_stats");
    const std::vector<std::string> kinds = testing_view::ReadViewText(shown).Column("kind");
    const auto has = [&kinds](const char* kind) {
        return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
    };
    EXPECT_TRUE(has("IndexProbe")) << shown;
    EXPECT_TRUE(has("IndexRange")) << shown;
    EXPECT_FALSE(has("?")) << shown;
}

TEST_F(AccessStatsTest, AnIndexShapeCountsUsesLikeAnyOther) {
    ASSERT_EQ(Run("CREATE INDEX b_flag ON b (flag)").substr(0, 7), "CREATED");
    for (int i = 0; i < 4; ++i) Run("SELECT * FROM b WHERE flag = " + std::to_string(i % 3));

    auto shape = Shape(exec::AccessKind::kIndexProbe, "b", kFlag);
    ASSERT_TRUE(shape.has_value());
    // Four executions, one shape - the values differ and the shape does not.
    EXPECT_EQ(shape->uses, 4u);
}

TEST_F(AccessStatsTest, AJoinRecordsBothOfItsSteps) {
    Run("SELECT a.id FROM b AS a JOIN h AS c ON a.id = c.id WHERE a.flag = 1");
    // The driving relation is filter-scanned; the inner one is probed.
    EXPECT_TRUE(Shape(exec::AccessKind::kFilterScan, "b", kFlag).has_value());
    EXPECT_TRUE(Shape(exec::AccessKind::kProbe, "h", kTupleKey).has_value());
}

// ---- Heat --------------------------------------------------------------

TEST_F(AccessStatsTest, CountAndLastSeenBothAdvance) {
    Run("SELECT * FROM b WHERE flag = 1");
    auto first = Shape(exec::AccessKind::kFilterScan, "b", kFlag);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->uses, 1u);

    Run("SELECT * FROM b WHERE flag = 1");
    auto second = Shape(exec::AccessKind::kFilterScan, "b", kFlag);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->uses, 2u);
    EXPECT_GE(second->last_seen, first->last_seen);
}

TEST_F(AccessStatsTest, AFailedStatementRecordsNothing) {
    const std::size_t before = dispatcher_->access_stats().Snapshot().size();
    EXPECT_EQ(Run("SELECT * FROM nosuchtable WHERE id = 1").substr(0, 3), "ERR");
    EXPECT_EQ(dispatcher_->access_stats().Snapshot().size(), before);
}

// ---- The store (DA, `stats/access_stats.hpp`) ----------------------------
//
// One part per core, each a flat table of fixed-size slots sized by the boot
// key's bytes (DA-Q8 (a)); a reader merges the parts by key.

constexpr exec::AccessKind kLookupKind = exec::AccessKind::kLookup;

std::vector<AccessShape> Of(const AccessStats& store, catalog::Oid rel) {
    std::vector<AccessShape> out;
    for (const AccessShape& s : store.Snapshot()) {
        if (s.rel_id == rel) out.push_back(s);
    }
    return out;
}

TEST(AccessStatsStore, TwoCoresRecordingOneShapeAnswerOneRowWithTheSum) {
    // The property page 11's exclusive hold gave and the parts must keep:
    // one row for one shape, its count the sum, `last_seen` the newest.
    AccessStats store;
    store.Record(/*core=*/1, kLookupKind, /*rel_id=*/4242, /*mask=*/0b101, /*now=*/900);
    store.Record(0, kLookupKind, 4242, 0b101, 990);
    store.Record(1, kLookupKind, 4242, 0b101, 950);
    const std::vector<AccessShape> rows = Of(store, 4242);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].uses, 3u);
    EXPECT_EQ(rows[0].last_seen, 990u);
}

TEST(AccessStatsStore, RowsAreInKeyOrder) {
    // DA-Q2 (a): `rel_id`, then the kind's name, then `column_mask` - not
    // the order shapes were admitted in, which no part can share.
    AccessStats store;
    store.Record(0, exec::AccessKind::kScan, 9, 0, 1);
    store.Record(1, exec::AccessKind::kFilterScan, 9, 4, 1);
    store.Record(0, exec::AccessKind::kFilterScan, 9, 2, 1);
    store.Record(1, exec::AccessKind::kLookup, 3, 1, 1);
    const std::vector<AccessShape> rows = store.Snapshot();
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0].rel_id, 3u);
    EXPECT_EQ(rows[1].kind, exec::AccessKind::kFilterScan);
    EXPECT_EQ(rows[1].column_mask, 2u);
    EXPECT_EQ(rows[2].column_mask, 4u);
    EXPECT_EQ(rows[3].kind, exec::AccessKind::kScan);
}

TEST(AccessStatsStore, AShapePastTheBoundIsDroppedCountedAndTheKnownOnesStillCount) {
    // The least key: two slots, one shape a part.
    AccessStats store(AccessStats::kMinBytesPerCore);
    EXPECT_EQ(store.ShapesPerCore(), 1u);
    store.Record(0, kLookupKind, 1, 1, 1);
    store.Record(0, kLookupKind, 2, 1, 1);  // past the bound
    store.Record(0, kLookupKind, 1, 1, 2);
    EXPECT_EQ(store.Dropped(), 1u);
    const std::vector<AccessShape> rows = store.Snapshot();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].uses, 2u);
    // Another core's part has its own bound.
    store.Record(1, kLookupKind, 2, 1, 3);
    EXPECT_EQ(store.Snapshot().size(), 2u);
    EXPECT_EQ(store.Dropped(), 1u);
}

TEST(AccessStatsStore, APartAdmitsNoMoreThanTheKeysBytesHoldAtHalfLoad) {
    // A value that is not a whole number of slots admits the slots that fit.
    AccessStats store(10 * AccessStats::kSlotBytes + 7);
    EXPECT_EQ(store.ShapesPerCore(), 5u);
    for (catalog::Oid rel = 1; rel <= 8; ++rel) store.Record(0, kLookupKind, rel, 1, rel);
    EXPECT_EQ(store.Snapshot().size(), 5u);
    EXPECT_EQ(store.Dropped(), 3u);
    // The default admits 4,096 a core (DA-Q3 (a)).
    EXPECT_EQ(AccessStats().ShapesPerCore(), 4096u);
}

TEST(AccessStatsStore, TheKeysRangeIsCheckedAndZeroIsRefused) {
    EXPECT_FALSE(AccessStats::CheckBytesPerCore(0).ok());
    EXPECT_FALSE(AccessStats::CheckBytesPerCore(AccessStats::kMinBytesPerCore - 1).ok());
    EXPECT_TRUE(AccessStats::CheckBytesPerCore(AccessStats::kMinBytesPerCore).ok());
    EXPECT_TRUE(AccessStats::CheckBytesPerCore(AccessStats::kDefaultBytesPerCore).ok());
    EXPECT_TRUE(AccessStats::CheckBytesPerCore(AccessStats::kMaxBytesPerCore).ok());
    EXPECT_FALSE(AccessStats::CheckBytesPerCore(AccessStats::kMaxBytesPerCore + 1).ok());
}

TEST(AccessStatsStore, AForgetOnOneCoreRemovesAnotherCoresShapes) {
    // CD-OI-13 (a), DA-R5: a reused position's shapes leave every part
    // before the `ADD COLUMN` answers; shapes not naming it stay.
    AccessStats store;
    store.Record(0, kLookupKind, 7, 0b100, 1);
    store.Record(3, exec::AccessKind::kFilterScan, 7, 0b110, 1);
    store.Record(3, exec::AccessKind::kFilterScan, 7, 0b010, 1);
    store.Record(2, exec::AccessKind::kFilterScan, 8, 0b100, 1);  // another relation
    store.Forget(7, 2);
    const std::vector<AccessShape> left = store.Snapshot();
    ASSERT_EQ(left.size(), 2u);
    EXPECT_EQ(left[0].rel_id, 7u);
    EXPECT_EQ(left[0].column_mask, 0b010u);
    EXPECT_EQ(left[1].rel_id, 8u);
    // A position past 63 names no bit, so it forgets nothing.
    store.Forget(8, 64);
    EXPECT_EQ(store.Snapshot().size(), 2u);
}

TEST(AccessStatsStore, AForgetKeepsEveryOtherShapeReachableInAFullTable) {
    // The backward shift: a part at its bound, every slot's run crossing the
    // others', and half its shapes forgotten - each remaining shape must
    // still be found (a record increments it rather than admitting a twin)
    // and the freed slots admit new shapes.
    AccessStats store(64 * AccessStats::kSlotBytes);
    const std::uint64_t n = store.ShapesPerCore();
    for (catalog::Oid rel = 1; rel <= n; ++rel) {
        store.Record(0, kLookupKind, 5, rel % 2 == 0 ? (1u << 3) | rel << 8 : rel << 8, rel);
    }
    ASSERT_EQ(store.Snapshot().size(), n);
    store.Forget(5, 3);
    ASSERT_EQ(store.Snapshot().size(), n / 2);
    for (catalog::Oid rel = 1; rel <= n; rel += 2) store.Record(0, kLookupKind, 5, rel << 8, 0);
    const std::vector<AccessShape> after = store.Snapshot();
    ASSERT_EQ(after.size(), n / 2);
    for (const AccessShape& s : after) EXPECT_EQ(s.uses, 2u) << s.column_mask;
    for (catalog::Oid rel = 1; rel <= n / 2; ++rel) store.Record(0, kLookupKind, 6, rel, 0);
    EXPECT_EQ(store.Snapshot().size(), n);
    EXPECT_EQ(store.Dropped(), 0u);
}

TEST(AccessStatsStore, RecordsAndForgetsAgreeWithAnOracleInSmallTables) {
    // Small tables make every run wrap the table's end; each round's
    // snapshot must equal a map's, so no forget skips a match nor loses a
    // shape it should keep.
    std::uint64_t seed = 0x9E3779B97F4A7C15ull;
    const auto next = [&seed] {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
    };
    for (std::uint64_t slots : {2u, 3u, 7u, 8u, 17u}) {
        AccessStats store(slots * AccessStats::kSlotBytes);
        std::map<std::pair<catalog::Oid, std::uint64_t>, std::uint64_t> oracle;
        for (int round = 0; round < 2000; ++round) {
            if (next() % 4 == 0) {
                const catalog::Oid rel = 1 + next() % 2;
                const std::uint16_t col = static_cast<std::uint16_t>(next() % 4);
                store.Forget(rel, col);
                std::erase_if(oracle, [&](const auto& e) {
                    return e.first.first == rel && (e.first.second & (1u << col)) != 0;
                });
            } else {
                const catalog::Oid rel = 1 + next() % 2;
                const std::uint64_t mask = next() % 16;
                const auto key = std::make_pair(rel, mask);
                if (oracle.contains(key) || oracle.size() < store.ShapesPerCore()) ++oracle[key];
                store.Record(0, kLookupKind, rel, mask, 0);
            }
            const std::vector<AccessShape> got = store.Snapshot();
            ASSERT_EQ(got.size(), oracle.size()) << "slots " << slots << " round " << round;
            for (const AccessShape& s : got) {
                const auto it = oracle.find({s.rel_id, s.column_mask});
                ASSERT_NE(it, oracle.end()) << "slots " << slots << " round " << round;
                EXPECT_EQ(s.uses, it->second);
            }
        }
    }
}

TEST(AccessStatsStore, AStoreThatRecordedNothingAllocatedNothingAndASecondStartsEmpty) {
    // DA-R6: a second instance in one process starts empty with no reset.
    AccessStats first;
    first.Record(0, kLookupKind, 1, 1, 1);
    AccessStats second;
    EXPECT_TRUE(second.Snapshot().empty());
    EXPECT_EQ(second.Dropped(), 0u);
    EXPECT_EQ(first.Snapshot().size(), 1u);
}

// ---- Through the dispatcher --------------------------------------------

TEST_F(AccessStatsTest, AHandedStoreIsTheOneEveryRecordReaches) {
    // DA-R6: the instance's store replaces the dispatcher's own.
    AccessStats instance;
    server::OptimizerSurface surface;
    surface.access_stats = &instance;
    dispatcher_->set_optimizer_surface(surface);
    Run("SELECT * FROM b WHERE flag = 1");
    EXPECT_EQ(Of(instance, OidOf("b")).size(), 1u);
    EXPECT_EQ(&dispatcher_->access_stats(), &instance);
}

TEST_F(AccessStatsTest, TheSwitchOffRecordsNothing) {
    server::CommandDispatcher off(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                                  /*clock=*/nullptr, /*wal=*/nullptr,
                                  wal::DurabilityClass::kGroup, exec::Budget(),
                                  /*recorder=*/nullptr, /*replay_enabled=*/false,
                                  /*access_statistics=*/false);
    off.Dispatch("SELECT * FROM b WHERE flag = 1");
    EXPECT_TRUE(off.access_stats().Snapshot().empty());
}

TEST_F(AccessStatsTest, SysMetaCountsTheDroppedAndNamesTheBound) {
    const std::string meta = Run("SHOW META");
    EXPECT_NE(meta.find("access_stats,access_stats_dropped,0"), std::string::npos) << meta;
    EXPECT_NE(meta.find("access_stats,access_stats_shapes_per_core,4096"), std::string::npos) << meta;
}

}  // namespace
}  // namespace kds::stats
