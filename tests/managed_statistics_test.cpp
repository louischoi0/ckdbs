#include <algorithm>
#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/sched/coro.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/expeditor.hpp"
#include "kds/stats/managed_statistics.hpp"
#include "kds/stats/plan_stats.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "reclaim_census.hpp"

// **Managed statistics** (CY-S1b, CY-S4; docs/spec/plan-stats.md §9; work
// order instructions/v3.2.0/workorder-cy-managed-statistics.md). What this
// file pins: a relation object changes only by an operator's act (CY-R3); it
// outranks the steward's sampled prior while it is published (CY-Q16 (a));
// versions only grow (CY-R6) and the last 8 replaced are kept (CY-Q12 (a));
// a lock stops every act that would change the published object without
// FORCE (CY-Q28 (a)); a gather counts what the leaves hold, sliced (CY-Q26
// (a), CY-Q21 (a)); an export imports back to the same object (CY-Q29 (a));
// a clean stop keeps it and a crash does not (W15); a drop forgets it.

namespace kds::server {
namespace {

using stats::ManagedStatistics;
using stats::RelationStatistic;
using stats::StatisticsOrigin;

RelationStatistic Gathered(std::uint64_t rows) {
    RelationStatistic stat;
    stat.origin = StatisticsOrigin::kGathered;
    stat.rows = rows;
    stat.leaves = rows / 100 + 1;
    stat.made_us = 1;
    return stat;
}

// ---- The store ------------------------------------------------------------------

TEST(ManagedStatisticsTest, AStorePublishesAndVersionsOnlyGrowAndEightReplacedAreKept) {
    ManagedStatistics m;
    for (std::uint64_t i = 1; i <= 12; ++i) {
        auto stored = m.Store(7, Gathered(i * 1000), /*pending=*/false, /*force=*/false);
        ASSERT_TRUE(stored.ok());
        EXPECT_EQ(stored.value().version, i);
    }
    const auto slots = m.Slots();
    ASSERT_EQ(slots.size(), 1u);
    ASSERT_TRUE(slots[0].published.has_value());
    EXPECT_EQ(slots[0].published->version, 12u);
    ASSERT_EQ(slots[0].history.size(), ManagedStatistics::kHistoryVersions);
    EXPECT_EQ(slots[0].history.front().version, 4u) << "oldest first, the last eight replaced";
    EXPECT_EQ(slots[0].history.back().version, 11u);
}

TEST(ManagedStatisticsTest, APendingVersionPricesNothingUntilPublishedAndCanBeDiscarded) {
    ManagedStatistics m;
    ASSERT_TRUE(m.Store(7, Gathered(1000), false, false).ok());
    const auto before = m.PublishedBands();
    ASSERT_TRUE(m.Store(7, Gathered(64000), /*pending=*/true, false).ok());
    EXPECT_EQ(m.PublishedBands(), before);
    auto published = m.PublishPending(7, false);
    ASSERT_TRUE(published.ok());
    EXPECT_EQ(published.value().version, 2u);
    EXPECT_NE(m.PublishedBands(), before);
    EXPECT_EQ(m.PublishPending(7, false).status().code(), StatusCode::kNotFound);
    ASSERT_TRUE(m.Store(7, Gathered(5), true, false).ok());
    ASSERT_TRUE(m.DiscardPending(7).ok());
    EXPECT_EQ(m.DiscardPending(7).code(), StatusCode::kNotFound);
}

TEST(ManagedStatisticsTest, ALockStopsEveryActThatChangesThePublishedObjectWithoutForce) {
    ManagedStatistics m;
    ASSERT_TRUE(m.Lock(7).ok()) << "a lock needs no object";
    EXPECT_EQ(m.Store(7, Gathered(10), false, false).status().code(),
              StatusCode::kInvalidArgument);
    ASSERT_TRUE(m.Store(7, Gathered(10), /*pending=*/true, false).ok()) << "pending is admitted";
    EXPECT_EQ(m.PublishPending(7, false).status().code(), StatusCode::kInvalidArgument);
    ASSERT_TRUE(m.PublishPending(7, /*force=*/true).ok());
    EXPECT_EQ(m.Delete(7, false).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(m.Restore(7, 1, false).status().code(), StatusCode::kInvalidArgument);
    ASSERT_TRUE(m.Unlock(7).ok());
    EXPECT_EQ(m.Unlock(7).code(), StatusCode::kInvalidArgument);
    ASSERT_TRUE(m.Delete(7, false).ok());
}

TEST(ManagedStatisticsTest, ARestorePublishesAKeptVersionAsANewOne) {
    ManagedStatistics m;
    ASSERT_TRUE(m.Store(7, Gathered(100), false, false).ok());
    ASSERT_TRUE(m.Store(7, Gathered(90000), false, false).ok());
    auto restored = m.Restore(7, 1, false);
    ASSERT_TRUE(restored.ok());
    EXPECT_EQ(restored.value().version, 3u);
    EXPECT_EQ(restored.value().rows, 100u);
    EXPECT_EQ(m.PublishedBands().front().second, stats::BandOf(100));
    EXPECT_EQ(m.Restore(7, 40, false).status().code(), StatusCode::kNotFound);
}

TEST(ManagedStatisticsTest, ADeleteKeepsTheCounterAndAForgottenRelationTakesNoStore) {
    ManagedStatistics m;
    ASSERT_TRUE(m.Store(7, Gathered(100), false, false).ok());
    ASSERT_TRUE(m.Delete(7, false).ok());
    EXPECT_TRUE(m.PublishedBands().empty());
    auto again = m.Store(7, Gathered(100), false, false);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.value().version, 2u) << "a later version is still newer (CY-R6)";
    m.Forget(7);
    EXPECT_TRUE(m.Slots().empty());
    EXPECT_EQ(m.Store(7, Gathered(1), false, false).status().code(), StatusCode::kNotFound);
    EXPECT_EQ(m.Lock(7).code(), StatusCode::kNotFound);
}

TEST(ManagedStatisticsTest, AReGatherInsideTheBandMovesNothingAndAnImportPublishesFresh) {
    ManagedStatistics m;
    ASSERT_TRUE(m.Store(7, Gathered(1000), false, false).ok());
    const std::int16_t band = m.PublishedBands().front().second;
    // 1260 rounds to the next band fresh, but sits inside 1000's hysteresis.
    ASSERT_NE(stats::BandOf(1260), band);
    ASSERT_TRUE(m.Store(7, Gathered(1260), false, false).ok());
    EXPECT_EQ(m.PublishedBands().front().second, band) << "CC's hysteresis (PS5)";
    RelationStatistic imported = Gathered(1260);
    imported.origin = StatisticsOrigin::kImported;
    ASSERT_TRUE(m.Store(7, imported, false, false).ok());
    EXPECT_EQ(m.PublishedBands().front().second, stats::BandOf(1260))
        << "an import takes its own band (CY-Q19 (a))";
}

// ---- The planner's snapshot ------------------------------------------------------

TEST(ManagedStatisticsPlanTest, APublishedObjectOutranksTheObservedPriorWhileItExists) {
    stats::PlanStats ps(1);
    ps.PublishForTest({{7, stats::BandOf(10)}}, {});
    ASSERT_TRUE(ps.managed().Store(7, Gathered(50000), false, false).ok());
    const std::uint64_t before = ps.generation();
    ps.RepublishManaged();
    EXPECT_GT(ps.generation(), before);
    const auto snapshot = ps.Current();
    ASSERT_TRUE(snapshot->RelationRows(7).has_value());
    EXPECT_EQ(*snapshot->RelationRows(7), stats::BandCentre(stats::BandOf(50000)));
    EXPECT_EQ(*snapshot->ObservedRows(7), stats::BandCentre(stats::BandOf(10)))
        << "the steward's prior is kept beside it (CY-Q16 (a))";
    ASSERT_TRUE(ps.managed().Delete(7, false).ok());
    ps.RepublishManaged();
    EXPECT_EQ(*ps.Current()->RelationRows(7), stats::BandCentre(stats::BandOf(10)));
}

TEST(ManagedStatisticsPlanTest, AShutdownsRowsLoadBackEveryVersionLockAndCounter) {
    stats::PlanStats ps(1);
    ps.SeedPriors(std::vector<std::pair<catalog::Oid, std::int16_t>>{{9, 12}});
    for (std::uint64_t rows : {100u, 2000u, 3000u}) {
        ASSERT_TRUE(ps.managed().Store(7, Gathered(rows), false, false).ok());
    }
    ASSERT_TRUE(ps.managed().Store(7, Gathered(77), /*pending=*/true, false).ok());
    ASSERT_TRUE(ps.managed().Store(7, Gathered(4000), false, false).ok());
    ASSERT_TRUE(ps.managed().Store(7, Gathered(55), true, false).ok());
    ASSERT_TRUE(ps.managed().Lock(7).ok());
    ASSERT_TRUE(ps.managed().Lock(8).ok());
    const std::vector<catalog::SysStatisticsRow> rows = ps.ExportStatistics();

    stats::PlanStats next(1);
    next.LoadStatistics(rows);
    const auto a = ps.managed().Slots();
    const auto b = next.managed().Slots();
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].oid, b[i].oid);
        EXPECT_EQ(a[i].locked, b[i].locked);
        EXPECT_EQ(a[i].last_version, b[i].last_version);
        EXPECT_EQ(a[i].published.has_value(), b[i].published.has_value());
        if (a[i].published.has_value()) {
            EXPECT_EQ(a[i].published->version, b[i].published->version);
            EXPECT_EQ(a[i].published->rows, b[i].published->rows);
            EXPECT_EQ(a[i].published->leaves, b[i].published->leaves);
        }
        EXPECT_EQ(a[i].pending.has_value(), b[i].pending.has_value());
        ASSERT_EQ(a[i].history.size(), b[i].history.size());
        for (std::size_t k = 0; k < a[i].history.size(); ++k) {
            EXPECT_EQ(a[i].history[k].version, b[i].history[k].version);
        }
    }
    EXPECT_EQ(*next.Current()->ObservedRows(9), stats::BandCentre(12));
    EXPECT_EQ(*next.Current()->RelationRows(7), stats::BandCentre(stats::BandOf(4000)));
}

TEST(ManagedStatisticsTest, AGatherOverAnImportPublishesFresh) {
    ManagedStatistics m;
    RelationStatistic imported = Gathered(1000);
    imported.origin = StatisticsOrigin::kImported;
    ASSERT_TRUE(m.Store(7, imported, false, false).ok());
    ASSERT_NE(stats::BandOf(1260), stats::BandOf(1000));
    ASSERT_TRUE(m.Store(7, Gathered(1260), false, false).ok());
    EXPECT_EQ(m.PublishedBands().front().second, stats::BandOf(1260))
        << "PS19: only a gather over a gather carries the hysteresis";
}

TEST(ManagedStatisticsPlanTest, TheHistoryComesBackInReplacementOrderNotVersionOrder) {
    stats::PlanStats ps(1);
    ASSERT_TRUE(ps.managed().Store(7, Gathered(10), /*pending=*/true, false).ok());   // v1
    ASSERT_TRUE(ps.managed().Store(7, Gathered(20), false, false).ok());              // v2
    ASSERT_TRUE(ps.managed().PublishPending(7, false).ok());  // v1 published, v2 replaced
    ASSERT_TRUE(ps.managed().Store(7, Gathered(30), false, false).ok());  // v1 replaced
    const auto before = ps.managed().Slots();
    ASSERT_EQ(before.size(), 1u);
    ASSERT_EQ(before[0].history.size(), 2u);
    ASSERT_EQ(before[0].history[0].version, 2u);
    ASSERT_EQ(before[0].history[1].version, 1u);
    std::vector<catalog::SysStatisticsRow> rows = ps.ExportStatistics();
    std::reverse(rows.begin(), rows.end());  // a chain keeps no row order
    stats::PlanStats next(1);
    next.LoadStatistics(rows);
    const auto after = next.managed().Slots();
    ASSERT_EQ(after.size(), 1u);
    ASSERT_EQ(after[0].history.size(), 2u);
    EXPECT_EQ(after[0].history[0].version, 2u) << "oldest replaced first";
    EXPECT_EQ(after[0].history[1].version, 1u);
}

TEST(ManagedStatisticsPlanTest, ARepublishWhoseBandsDidNotMoveKeepsTheGeneration) {
    stats::PlanStats ps(1);
    ASSERT_TRUE(ps.managed().Store(7, Gathered(1000), false, false).ok());
    ps.RepublishManaged();
    const std::uint64_t published = ps.generation();
    // A re-gather inside the band (PS5) and a pending store move no band, so
    // no core's order memo is re-keyed (CY-Q31 (a)).
    ASSERT_TRUE(ps.managed().Store(7, Gathered(1010), false, false).ok());
    ASSERT_TRUE(ps.managed().Store(7, Gathered(90000), /*pending=*/true, false).ok());
    ps.RepublishManaged();
    EXPECT_EQ(ps.generation(), published);
    ASSERT_TRUE(ps.managed().PublishPending(7, false).ok());
    ps.RepublishManaged();
    EXPECT_GT(ps.generation(), published);
}

TEST(ManagedStatisticsPlanTest, LoadingNothingPublishesNothing) {
    stats::PlanStats ps(1);
    ps.LoadStatistics({});
    EXPECT_EQ(ps.generation(), 0u);
}

// ---- Through a dispatcher --------------------------------------------------------

class StatisticsStatementTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 4000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay=*/false,
                            /*access_statistics=*/true, /*cabins=*/nullptr, &*mgr_);
        dispatcher_->set_plan_stats(&stats_);
        Ok("CREATE TABLE t (id int64, v int64) BTREE");
        Ok("CREATE TABLE u (id int64, v int64) BTREE");
        Fill("t", 1, kRows);
        Fill("u", 1, 3);
        t_ = OidOf("t");
    }
    void TearDown() override { dispatcher_->set_plan_stats(nullptr); }

    static constexpr int kRows = 6000;

    void Fill(const std::string& table, int from, int to) {
        std::string sql;
        for (int i = from; i <= to; ++i) {
            if (sql.empty()) sql = "INSERT INTO " + table + " VALUES ";
            else sql += ", ";
            sql += "(" + std::to_string(i) + ", " + std::to_string(i % 7) + ")";
            if ((i - from) % 500 == 499 || i == to) {
                Ok(sql);
                sql.clear();
            }
        }
    }
    DispatchOutcome Do(const std::string& sql) { return dispatcher_->Dispatch(sql); }
    std::string Run(const std::string& sql) { return Do(sql).response; }
    void Ok(const std::string& sql) {
        const std::string out = Run(sql);
        ASSERT_NE(out.rfind("ERR", 0), 0u) << sql.substr(0, 120) << " -> " << out;
    }
    Status Refusal(const std::string& sql) {
        const DispatchOutcome out = Do(sql);
        EXPECT_EQ(out.response.rfind("ERR", 0), 0u) << sql << " -> " << out.response;
        return out.status;
    }
    catalog::Oid OidOf(const std::string& name) {
        auto oid = boot_->catalog.ResolveRelation("", name, catalog::kNamespacePublic, 0, nullptr);
        EXPECT_TRUE(oid.ok()) << name;
        return oid.ok() ? oid.value() : 0;
    }
    // A reply field's value: ` key=value`.
    static std::string Field(const std::string& reply, const std::string& key) {
        const std::size_t at = reply.find(" " + key + "=");
        if (at == std::string::npos) return "";
        const std::size_t from = at + key.size() + 2;
        return reply.substr(from, reply.find(' ', from) - from);
    }
    // `sys.relation_stats`' own count of t's leaves, a whole-tree walk.
    std::uint64_t Leaves() {
        const std::string out = Run("SELECT leaves FROM sys.relation_stats WHERE name = 't'");
        const std::size_t at = out.rfind("\\n");
        EXPECT_NE(at, std::string::npos) << out;
        return at == std::string::npos ? 0 : std::stoull(out.substr(at + 2));
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    stats::PlanStats stats_{1};
    std::optional<CommandDispatcher> dispatcher_;
    catalog::Oid t_ = 0;
};

TEST_F(StatisticsStatementTest, AGatherCountsTheLiveRowsAndEveryLeafAndThePlannerPricesThem) {
    Ok("DELETE FROM t WHERE id <= 100");
    const std::string reply = Run("GATHER STATISTICS ON t");
    ASSERT_EQ(reply.rfind("GATHERED STATISTICS relation=t version=1 ", 0), 0u) << reply;
    EXPECT_EQ(Field(reply, "rows"), std::to_string(kRows - 100)) << "delete-marked are not live";
    EXPECT_EQ(Field(reply, "leaves"), std::to_string(Leaves()));
    EXPECT_EQ(Field(reply, "state"), "published");
    const auto rows = stats_.Current()->RelationRows(t_);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(*rows, stats::BandCentre(stats::BandOf(kRows - 100)));
    const std::string view =
        Run("SELECT name, state, origin, version, rows, used FROM sys.statistics");
    EXPECT_NE(view.find("t,published,gathered,1," + std::to_string(kRows - 100) + ",1"),
              std::string::npos)
        << view;
}

// DN: a heap's walk is its `kHeapData` chain, which the gather refused page
// by page as "no longer a leaf" - every GATHER over a heap was TxnConflict.
TEST_F(StatisticsStatementTest, AGatherOverAHeapCountsItsDataPages) {
    Ok("CREATE TABLE h (id int64 PRIMARY KEY, v int64) HEAP");
    Fill("h", 1, 3000);
    Ok("DELETE FROM h WHERE id <= 10");
    const std::string reply = Run("GATHER STATISTICS ON h");
    ASSERT_EQ(reply.rfind("GATHERED STATISTICS relation=h version=1 ", 0), 0u) << reply;
    EXPECT_EQ(Field(reply, "rows"), "2990");
    EXPECT_GT(std::stoull(Field(reply, "leaves")), 1u);
    // And `sys.relation_stats` counts the same pages, where it answered NULL.
    const std::string shown = Run("SELECT leaves FROM sys.relation_stats WHERE name = 'h'");
    EXPECT_EQ(shown, "leaves\\n" + Field(reply, "leaves")) << shown;
}

TEST_F(StatisticsStatementTest, ASlicedGatherYieldsBetweenSlicesAndCountsTheSame) {
    ASSERT_GT(Leaves(), 16u) << "the cell needs more than one slice";
    DispatchOutcome out;
    auto task = sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                                    dispatcher_->DispatchAsync("GATHER STATISTICS ON t", nullptr,
                                                               &out));
    int polls = 0;
    bool done = false;
    for (; polls < 100000 && !done; ++polls) done = task->Poll() == sched::PollResult::kDone;
    ASSERT_TRUE(done);
    EXPECT_EQ(static_cast<std::uint64_t>(polls), (Leaves() + 15) / 16)
        << "one poll a slice of 16 leaves: the walk yielded its core between them";
    ASSERT_EQ(out.response.rfind("GATHERED STATISTICS", 0), 0u) << out.response;
    EXPECT_EQ(Field(out.response, "rows"), std::to_string(kRows));
    EXPECT_EQ(Field(out.response, "leaves"), std::to_string(Leaves()));
}

TEST_F(StatisticsStatementTest, EachStatementsRefusalsNameTheirByte) {
    EXPECT_EQ(Refusal("GATHER STATISTICS ON t (v)").code(), StatusCode::kNotImplemented);
    EXPECT_EQ(Refusal("GATHER STATISTICS ON t ALL COLUMNS").code(), StatusCode::kNotImplemented);
    EXPECT_EQ(Refusal("GATHER STATISTICS ON nosuch").code(), StatusCode::kNotFound);
    EXPECT_EQ(Refusal("GATHER STATISTICS ON sys.tables").code(), StatusCode::kUnsupported);
    const Status bad = Refusal("GATHER STATISTICS ON t SOON");
    EXPECT_EQ(bad.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(bad.message().find("(byte 23)"), std::string::npos) << bad.message();
    EXPECT_EQ(Refusal("GATHER STATISTICS t").code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(Refusal("LOCK STATISTICS ON t FORCE").code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(Refusal("RESTORE STATISTICS ON t").code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(Refusal("IMPORT STATISTICS ON t FROM 'KDS-STATISTICS 1 RELATION'").code(),
              StatusCode::kInvalidArgument)
        << "rows= is required";
    EXPECT_EQ(Refusal("IMPORT STATISTICS ON t FROM 'KDS-STATISTICS 2 RELATION rows=1'").code(),
              StatusCode::kNotImplemented);
    const Status field = Refusal("IMPORT STATISTICS ON t FROM 'KDS-STATISTICS 1 RELATION nope=1'");
    EXPECT_EQ(field.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(field.message().find("(byte 55)"), std::string::npos) << field.message();
    // An escaped quote is two bytes of the statement and one of the document.
    const Status escaped = Refusal(
        "IMPORT STATISTICS ON t FROM 'KDS-STATISTICS 1 RELATION relation=a''b nope=1'");
    EXPECT_NE(escaped.message().find("(byte 69)"), std::string::npos) << escaped.message();
    EXPECT_EQ(Refusal("IMPORT STATISTICS ON t FROM 'KDS-STATISTICS 1 RELATION rows=1 rows=2'")
                  .code(),
              StatusCode::kInvalidArgument)
        << "a field named twice";
    EXPECT_EQ(Refusal("UNLOCK STATISTICS ON t").code(), StatusCode::kInvalidArgument);
    ASSERT_EQ(Run("BEGIN").rfind("ERR", 0), std::string::npos);
    EXPECT_EQ(Refusal("GATHER STATISTICS ON t").code(), StatusCode::kNotImplemented);
    Ok("ROLLBACK");
}

TEST_F(StatisticsStatementTest, ALockedRelationIsGatheredOnlyPendingOrByForce) {
    Ok("LOCK STATISTICS ON t");
    EXPECT_EQ(Refusal("GATHER STATISTICS ON t").code(), StatusCode::kInvalidArgument);
    const std::string pending = Run("GATHER STATISTICS ON t PENDING");
    ASSERT_EQ(Field(pending, "state"), "pending") << pending;
    EXPECT_FALSE(stats_.Current() != nullptr && stats_.Current()->Managed(t_));
    EXPECT_EQ(Refusal("PUBLISH STATISTICS ON t").code(), StatusCode::kInvalidArgument);
    ASSERT_EQ(Run("PUBLISH STATISTICS ON t FORCE").rfind("PUBLISHED STATISTICS relation=t "
                                                         "version=1",
                                                         0),
              0u);
    EXPECT_TRUE(stats_.Current()->Managed(t_));
    const std::string view = Run("SELECT name, state, locked FROM sys.statistics");
    EXPECT_NE(view.find("t,published,1"), std::string::npos) << view;
    Ok("UNLOCK STATISTICS ON t");
    Ok("GATHER STATISTICS ON t");
}

TEST_F(StatisticsStatementTest, AnExportImportsBackToTheSameObjectOnAnotherRelation) {
    Ok("GATHER STATISTICS ON t");
    const std::string exported = Run("EXPORT STATISTICS ON t");
    const std::size_t at = exported.find("KDS-STATISTICS 1 RELATION");
    ASSERT_NE(at, std::string::npos) << exported;
    const std::string doc = exported.substr(at, exported.find("\\n", at) - at);
    EXPECT_NE(doc.find(" rows=" + std::to_string(kRows) + " "), std::string::npos) << doc;
    EXPECT_NE(doc.find(" relation=public.t"), std::string::npos) << doc;
    const std::string imported = Run("IMPORT STATISTICS ON u FROM '" + doc + "'");
    ASSERT_EQ(imported.rfind("IMPORTED STATISTICS relation=u version=1 rows=" +
                                 std::to_string(kRows),
                             0),
              0u)
        << imported;
    EXPECT_EQ(stats_.Current()->RelationRows(OidOf("u")), stats_.Current()->RelationRows(t_));
    const std::string view = Run("SELECT name, origin FROM sys.statistics");
    EXPECT_NE(view.find("u,imported"), std::string::npos) << view;
}

TEST_F(StatisticsStatementTest, ARestoreAndADeleteAreSeenInTheViews) {
    Ok("GATHER STATISTICS ON t");
    Fill("t", kRows + 1, kRows * 3);
    Ok("GATHER STATISTICS ON t");
    const std::string history = Run("SELECT name, version, rows FROM sys.statistics_history");
    EXPECT_NE(history.find("t,1," + std::to_string(kRows)), std::string::npos) << history;
    const std::string restored = Run("RESTORE STATISTICS ON t TO VERSION 1");
    ASSERT_EQ(restored.rfind("RESTORED STATISTICS relation=t version=3 rows=" +
                                 std::to_string(kRows),
                             0),
              0u)
        << restored;
    EXPECT_EQ(*stats_.Current()->RelationRows(t_), stats::BandCentre(stats::BandOf(kRows)));
    Ok("DELETE STATISTICS ON t");
    EXPECT_FALSE(stats_.Current()->Managed(t_));
    EXPECT_EQ(Run("SELECT version FROM sys.statistics_history").find("\\n"), std::string::npos);
    EXPECT_EQ(Refusal("DELETE STATISTICS ON t").code(), StatusCode::kNotFound);
    // A row DELETE is not this statement.
    Ok("DELETE FROM u WHERE id = 1");
}

TEST_F(StatisticsStatementTest, AReadWriteConnectionMayNotGather) {
    Session session;
    session.set_role(Role::kReadWrite);
    const DispatchOutcome out = dispatcher_->Dispatch("DELETE STATISTICS ON t", &session);
    EXPECT_EQ(out.response.rfind("ERR permission", 0), 0u) << out.response;
    const DispatchOutcome gather = dispatcher_->Dispatch("GATHER STATISTICS ON t", &session);
    EXPECT_EQ(gather.response.rfind("ERR permission", 0), 0u) << gather.response;
}

// ---- A whole instance: a clean stop, a crash, a drop ------------------------------

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() / ("kds_managed_stats_" + std::to_string(::getpid()) +
                                            "_" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

std::string Reply(Expeditor& db, const std::string& sql) {
    return db.dispatcher().Dispatch(sql).response;
}

TEST(ManagedStatisticsMountTest, ACleanStopKeepsTheObjectsAndACrashKeepsNone) {
    TempDir live;
    TempDir crashed;
    {
        auto opened = reclaim_census::Mount(live.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        ASSERT_EQ(Reply(db, "CREATE TABLE t (id int64, v int64) BTREE").rfind("ERR", 0),
                  std::string::npos);
        ASSERT_EQ(Reply(db, "INSERT INTO t VALUES (1, 1), (2, 2), (3, 3)").rfind("ERR", 0),
                  std::string::npos);
        ASSERT_EQ(Reply(db, "GATHER STATISTICS ON t").rfind("GATHERED", 0), 0u);
        ASSERT_EQ(Reply(db, "GATHER STATISTICS ON t PENDING").rfind("GATHERED", 0), 0u);
        ASSERT_EQ(Reply(db, "LOCK STATISTICS ON t").rfind("LOCKED", 0), 0u);
        reclaim_census::CrashImage(live.path, crashed.path);
        db.WriteRelationPriors();
        reclaim_census::CleanShutdownCheckpoint(db);
    }
    {
        auto mounted = reclaim_census::Mount(live.path);
        ASSERT_TRUE(mounted.ok()) << mounted.status().message();
        Expeditor& db = *mounted.value();
        CurrentCoreGuard as(0);
        const std::string view =
            Reply(db, "SELECT name, state, origin, version, rows, locked FROM sys.statistics");
        EXPECT_NE(view.find("t,published,gathered,1,3,1"), std::string::npos) << view;
        EXPECT_NE(view.find("t,pending,gathered,2,3,1"), std::string::npos) << view;
        // The counter came back: the next version is newer than both.
        const std::string next = Reply(db, "GATHER STATISTICS ON t FORCE");
        EXPECT_EQ(next.rfind("GATHERED STATISTICS relation=t version=3 ", 0), 0u) << next;
        // A committed drop forgets the relation's objects.
        ASSERT_EQ(Reply(db, "DROP TABLE t").rfind("DROPPED", 0), 0u);
        EXPECT_TRUE(db.plan_stats().managed().Slots().empty());
        reclaim_census::CleanShutdownCheckpoint(db);
    }
    {
        auto mounted = reclaim_census::Mount(crashed.path);
        ASSERT_TRUE(mounted.ok()) << mounted.status().message();
        Expeditor& db = *mounted.value();
        CurrentCoreGuard as(0);
        EXPECT_TRUE(db.plan_stats().managed().Slots().empty()) << "a crash keeps none (W15)";
        reclaim_census::CleanShutdownCheckpoint(db);
    }
}

}  // namespace
}  // namespace kds::server
