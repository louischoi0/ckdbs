#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/catalog/rows.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/stats/plan_stats.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/page_header.hpp"
#include "reclaim_census.hpp"

// `sys.statistics` (docs/spec/plan-stats.md §8, CY-S1): the join planner's
// relation priors across a clean shutdown. The relation is unlogged, so the
// decision this file pins is when a mount may believe it: only where the
// shutdown's closing mark and its count read whole, and never twice - a
// mount leaves the chain empty whatever it found. And damage to it costs
// the statistic, never the mount.

namespace kds::catalog {
namespace {

using Priors = std::vector<std::pair<Oid, std::int16_t>>;

class StatisticsPersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_.emplace(store_);
        ASSERT_TRUE(catalog_->Bootstrap().ok());
    }

    // Priors for `n` relations, oids from 5000.
    static Priors Relations(std::size_t n) {
        Priors priors;
        for (std::size_t i = 0; i < n; ++i) {
            priors.emplace_back(5000 + i, static_cast<std::int16_t>(i % 40));
        }
        return priors;
    }

    // A shutdown's write of `priors` alone, as relation rows.
    Status Write(const Priors& priors) {
        std::vector<SysStatisticsRow> rows;
        for (const auto& [oid, band] : priors) {
            SysStatisticsRow row{};
            row.kind = kStatisticsRelation;
            row.rel_oid = oid;
            row.band = band;
            rows.push_back(row);
        }
        return catalog_->WriteStatisticsAtShutdown(rows);
    }

    // What a mount takes, as priors in oid order.
    Priors Take() {
        auto taken = catalog_->TakeStatisticsAtMount();
        EXPECT_TRUE(taken.ok()) << taken.status().message();
        Priors priors;
        if (taken.ok()) {
            for (const SysStatisticsRow& row : taken.value()) {
                EXPECT_EQ(row.kind, kStatisticsRelation);
                priors.emplace_back(row.rel_oid, row.band);
            }
        }
        std::sort(priors.begin(), priors.end());
        return priors;
    }

    PageId NextOf(PageId page_id) {
        auto bytes = store_.GetForRead(page_id);
        EXPECT_TRUE(bytes.ok()) << bytes.status().message();
        return bytes.ok() ? heap::PageView(bytes.value().bytes()).next_page_id() : kInvalidPageId;
    }

    storage::InMemoryPageStore store_{128};
    std::optional<Catalog> catalog_;
};

TEST_F(StatisticsPersistenceTest, AFreshVolumeHoldsNoneAndItsMountIsNotRefused) {
    EXPECT_TRUE(Take().empty());
    EXPECT_TRUE(Take().empty()) << "a second mount of an empty chain";
}

TEST_F(StatisticsPersistenceTest, WhatAShutdownWroteTheNextMountTakesOnce) {
    const Priors written = Relations(3);
    ASSERT_TRUE(Write(written).ok());
    EXPECT_EQ(Take(), written) << "each prior comes back at the band it left at";
    // Emptied by the mount that took it: a crash after that mount leaves
    // the next one nothing to believe.
    EXPECT_TRUE(Take().empty());
}

TEST_F(StatisticsPersistenceTest, AShutdownWithNoPriorWritesAMarkAndTheMountTakesNone) {
    ASSERT_TRUE(Write({}).ok());
    EXPECT_TRUE(Take().empty());
}

TEST_F(StatisticsPersistenceTest, RowsWithNoClosingMarkAreNotTaken) {
    // A shutdown that failed before its mark: the rows are there and
    // nothing says they are all of them.
    ASSERT_TRUE(Write(Relations(3)).ok());
    {
        auto head = store_.Get(kCatalogPageStatistics);
        ASSERT_TRUE(head.ok());
        heap::PageView page(head.value().bytes());
        bool retired = false;
        for (std::uint16_t slot = 0; slot < page.slot_count(); ++slot) {
            auto tuple = page.ReadTuple(slot);
            ASSERT_TRUE(tuple.ok());
            auto row = SysStatisticsRow::Decode(tuple.value().payload);
            ASSERT_TRUE(row.ok());
            if (row.value().kind == kStatisticsWhole) {
                ASSERT_TRUE(page.RetireSlot(slot).ok());
                retired = true;
            }
        }
        ASSERT_TRUE(retired) << "the fixture found no closing mark to remove";
    }
    EXPECT_TRUE(Take().empty());
    EXPECT_TRUE(Take().empty()) << "and the chain was emptied all the same";
}

TEST_F(StatisticsPersistenceTest, AMarkThatCountsMoreRowsThanTheChainHoldsIsNotBelieved) {
    // A shutdown torn between its pages: the page with the mark reached the
    // device and a page of rows did not. Here, a page's rows are gone after
    // the mark was written, and the link past it is kept so the mark is
    // still read.
    ASSERT_TRUE(Write(Relations(900)).ok());
    const PageId second = NextOf(kCatalogPageStatistics);
    ASSERT_NE(second, kInvalidPageId) << "900 rows did not grow the chain";
    const PageId third = NextOf(second);
    ASSERT_NE(third, kInvalidPageId) << "the fixture needs a page to lose between head and tail";
    {
        auto bytes = store_.Get(second);
        ASSERT_TRUE(bytes.ok());
        const std::uint64_t owner = storage::GetOwnerOid(bytes.value().bytes());
        auto fresh = heap::PageView::CreateEmpty(bytes.value().bytes(), 0, owner);
        ASSERT_TRUE(fresh.ok());
        fresh.value().set_next_page_id(third);
    }
    EXPECT_TRUE(Take().empty());
}

TEST_F(StatisticsPersistenceTest, AChainOfSeveralPagesComesBackWholeAndKeepsItsPages) {
    const Priors written = Relations(900);
    ASSERT_TRUE(Write(written).ok());
    const PageId grown = NextOf(kCatalogPageStatistics);
    ASSERT_NE(grown, kInvalidPageId);
    {
        auto bytes = store_.GetForRead(grown);
        ASSERT_TRUE(bytes.ok());
        EXPECT_EQ(storage::GetOwnerOid(bytes.value().bytes()), kSysStatisticsTable)
            << "a growth page that does not say whose it is would be cut at the next mount";
    }
    EXPECT_EQ(Take(), written);
    EXPECT_EQ(NextOf(kCatalogPageStatistics), grown)
        << "the emptied chain keeps its growth pages for the next shutdown";
    // And the next shutdown writes onto them.
    ASSERT_TRUE(Write(Relations(2)).ok());
    EXPECT_EQ(Take().size(), 2u);
}

TEST_F(StatisticsPersistenceTest, ALinkToAPageThatIsNotThereTakesNoneAndCutsTheChain) {
    ASSERT_TRUE(Write(Relations(3)).ok());
    {
        auto head = store_.Get(kCatalogPageStatistics);
        ASSERT_TRUE(head.ok());
        heap::PageView(head.value().bytes()).set_next_page_id(9999);
    }
    EXPECT_TRUE(Take().empty()) << "rows before a cut are not whole";
    EXPECT_EQ(NextOf(kCatalogPageStatistics), kInvalidPageId);
    ASSERT_TRUE(Write(Relations(1)).ok());
    EXPECT_EQ(Take().size(), 1u);
}

TEST_F(StatisticsPersistenceTest, TheMountsDeleteMarkSweepPassesADamagedChainBy) {
    // The mount finalizes delete marks over every catalog root before its
    // discard window, with a walk that trusts every link. A kill during a
    // shutdown can leave the head written and the map that carries its
    // growth page's bit not: followed there, the link refuses the mount -
    // and every mount after it, since nothing logs or repairs the page.
    // The sweep therefore passes an unlogged relation by
    // (`IsUnloggedCatalogRelation`), and the guarded take cuts the link.
    ASSERT_TRUE(Write(Relations(3)).ok());
    {
        auto head = store_.Get(kCatalogPageStatistics);
        ASSERT_TRUE(head.ok());
        heap::PageView(head.value().bytes()).set_next_page_id(9999);
    }
    auto finalized = catalog_->FinalizeDeleteMarksAtMount();
    ASSERT_TRUE(finalized.ok()) << "the mount is refused: " << finalized.status().message();
    EXPECT_TRUE(Take().empty());
    // The same holds for page 11, which a pre-DA build left unlogged and
    // which nothing walks since DA: a damaged link there refuses no mount.
    {
        auto head = store_.Get(kCatalogPageAccessStats);
        ASSERT_TRUE(head.ok());
        heap::PageView(head.value().bytes()).set_next_page_id(9999);
    }
    finalized = catalog_->FinalizeDeleteMarksAtMount();
    EXPECT_TRUE(finalized.ok()) << finalized.status().message();
}

TEST_F(StatisticsPersistenceTest, AHeadThatIsNoHeapPageIsMadeAgainEmptyAndTheMountGoesOn) {
    // The head is rewritten at every clean shutdown and nothing logs it, so
    // a write torn there has no image to heal from. It is advisory: made
    // again empty, never a refused mount.
    ASSERT_TRUE(Write(Relations(3)).ok());
    {
        auto head = store_.Get(kCatalogPageStatistics);
        ASSERT_TRUE(head.ok());
        std::fill(head.value().bytes().begin(), head.value().bytes().end(), std::byte{0});
    }
    EXPECT_TRUE(Take().empty());
    const Priors again = Relations(2);
    ASSERT_TRUE(Write(again).ok());
    EXPECT_EQ(Take(), again) << "the healed head does not hold the next shutdown's rows";
}

TEST_F(StatisticsPersistenceTest, ASecondShutdownWriteReplacesTheFirst) {
    ASSERT_TRUE(Write(Relations(5)).ok());
    ASSERT_TRUE(Write(Relations(2)).ok());
    EXPECT_EQ(Take().size(), 2u) << "two marks, or seven rows, would not read whole";
}

// ---- The planner's half: what a mount seeds is what a compile reads -------

TEST(StatisticsSeedTest, SeededPriorsArePublishedAtTheirBandsAndExportedBack) {
    stats::PlanStats plan_stats(1);
    EXPECT_EQ(plan_stats.generation(), 0u);
    const Priors bands = {{5000, 1}, {5001, 15}, {5002, 27}};
    plan_stats.SeedPriors(bands);
    EXPECT_EQ(plan_stats.generation(), 1u) << "published before any statement compiles";
    const auto snapshot = plan_stats.Current();
    ASSERT_NE(snapshot, nullptr);
    for (const auto& [oid, band] : bands) {
        const std::optional<double> rows = snapshot->RelationRows(oid);
        ASSERT_TRUE(rows.has_value()) << oid;
        EXPECT_DOUBLE_EQ(*rows, stats::BandCentre(band));
    }
    EXPECT_EQ(plan_stats.ExportPriors(), bands);
}

TEST(StatisticsSeedTest, SeedingNothingPublishesNothing) {
    stats::PlanStats plan_stats(1);
    plan_stats.SeedPriors({});
    EXPECT_EQ(plan_stats.generation(), 0u) << "an empty mount must not look informed";
    EXPECT_TRUE(plan_stats.ExportPriors().empty());
}

}  // namespace
}  // namespace kds::catalog

// ---- A whole instance: a clean stop keeps the priors, a crash keeps none --

namespace kds::server {
namespace {

namespace fs = std::filesystem;
using Priors = std::vector<std::pair<catalog::Oid, std::int16_t>>;

struct TempDir {
    fs::path path;
    TempDir() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() / ("kds_sp_" + std::to_string(::getpid()) + "_" +
                                            std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

catalog::Oid OidOf(Expeditor& db, const std::string& name) {
    auto oid = db.catalog().FindTableOidByName(catalog::kNamespacePublic, name);
    EXPECT_TRUE(oid.ok()) << name;
    return oid.ok() ? oid.value() : 0;
}

// A run that made two relations and came to know their sizes - seeded, as
// the steward's own observation would leave them - and answers the priors
// it holds.
Priors RunThatObserved(Expeditor& db) {
    for (const char* line : {"CREATE TABLE cargos (id int64, org_id int64) BTREE",
                             "CREATE TABLE freights (id int64, cargo_id int64) BTREE"}) {
        const std::string reply = db.dispatcher().Dispatch(line).response;
        EXPECT_EQ(reply.rfind("ERR", 0), std::string::npos) << line << " -> " << reply;
    }
    const Priors bands = {{OidOf(db, "cargos"), stats::BandOf(10000.0)},
                          {OidOf(db, "freights"), stats::BandOf(9000.0)}};
    db.plan_stats().SeedPriors(bands);
    return db.plan_stats().ExportPriors();
}

// A run that observed and stopped cleanly, leaving its priors on the volume.
Priors CleanStopThatWrote(const fs::path& dir) {
    auto opened = reclaim_census::Mount(dir);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    if (!opened.ok()) return {};
    CurrentCoreGuard as(0);
    const Priors observed = RunThatObserved(*opened.value());
    opened.value()->WriteRelationPriors();
    reclaim_census::CleanShutdownCheckpoint(*opened.value());
    return observed;
}

TEST(StatisticsMountTest, ACleanStopKeepsTheRelationPriorsForOneMount) {
    TempDir live;
    {
        auto fresh = reclaim_census::Mount(live.path);
        ASSERT_TRUE(fresh.ok()) << fresh.status().message();
        EXPECT_EQ(fresh.value()->plan_stats().generation(), 0u) << "a fresh volume is blind";
        reclaim_census::CleanShutdownCheckpoint(*fresh.value());
    }
    const Priors observed = CleanStopThatWrote(live.path);
    ASSERT_EQ(observed.size(), 2u);
    {
        // The next mount is informed before its first statement.
        auto mounted = reclaim_census::Mount(live.path);
        ASSERT_TRUE(mounted.ok()) << mounted.status().message();
        CurrentCoreGuard as(0);
        EXPECT_GE(mounted.value()->plan_stats().generation(), 1u);
        EXPECT_EQ(mounted.value()->plan_stats().ExportPriors(), observed)
            << "each prior comes back at the band it left at";
        // This run stops without writing any: the mount emptied the chain,
        // so what it took is not there for the mount after.
        reclaim_census::CleanShutdownCheckpoint(*mounted.value());
    }
    auto again = reclaim_census::Mount(live.path);
    ASSERT_TRUE(again.ok()) << again.status().message();
    EXPECT_EQ(again.value()->plan_stats().generation(), 0u)
        << "a statistic was believed twice: the mount that took it did not empty the chain";
}

TEST(StatisticsMountTest, ACrashAfterTheMountThatTookThemKeepsNone) {
    // The reason the mount empties the chain inside its discard window: the
    // emptying must be on the device before the listener binds, so a crash
    // at any later instant finds none. A mount that kept the rows for the
    // run would let a crashed run's next mount believe a chain whose pages
    // recovery never vouched for.
    TempDir live, image;
    ASSERT_EQ(CleanStopThatWrote(live.path).size(), 2u);
    {
        auto mounted = reclaim_census::Mount(live.path);
        ASSERT_TRUE(mounted.ok()) << mounted.status().message();
        ASSERT_GE(mounted.value()->plan_stats().generation(), 1u)
            << "the fixture's mount took nothing; the cell proves nothing";
        // The crash: the files as the mount left them, nothing synced since.
        reclaim_census::CrashImage(live.path, image.path);
    }
    auto crashed = reclaim_census::Mount(image.path);
    ASSERT_TRUE(crashed.ok()) << crashed.status().message();
    EXPECT_EQ(crashed.value()->plan_stats().generation(), 0u)
        << "a mount after a crash believed the previous clean stop's statistics";
}

TEST(StatisticsMountTest, ACrashWithNoStopKeepsNone) {
    // Nothing writes the relation while statements run, so a run that is
    // cut short leaves the next mount what it found: nothing.
    TempDir live, image;
    {
        auto opened = reclaim_census::Mount(live.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        CurrentCoreGuard as(0);
        ASSERT_EQ(RunThatObserved(*opened.value()).size(), 2u);
        ASSERT_TRUE(opened.value()->wal().Flush().ok());
        reclaim_census::CrashImage(live.path, image.path);
    }
    auto crashed = reclaim_census::Mount(image.path);
    ASSERT_TRUE(crashed.ok()) << crashed.status().message();
    EXPECT_EQ(crashed.value()->plan_stats().generation(), 0u);
    // The relations themselves came back from the log.
    CurrentCoreGuard as(0);
    EXPECT_NE(OidOf(*crashed.value(), "cargos"), 0u);
}

TEST(StatisticsMountTest, AHeadPageTornOnTheDeviceCostsTheStatisticAndNotTheMount) {
    // The shutdown's own write of page 17, torn: bytes of its body changed
    // on the device, so its checksum fails at the next fetch. Nothing logs
    // the page, so there is no image to heal it from - and a mount refused
    // on it would be refused for the life of the file.
    TempDir live;
    ASSERT_EQ(CleanStopThatWrote(live.path).size(), 2u);
    {
        std::fstream file(live.path / "kds.db", std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file.good());
        const std::streamoff at =
            static_cast<std::streamoff>(catalog::kCatalogPageStatistics) * kPageSize + 512;
        const std::string garbage(64, '\x5a');
        file.seekp(at);
        file.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
        ASSERT_TRUE(file.good());
    }
    {
        auto mounted = reclaim_census::Mount(live.path);
        ASSERT_TRUE(mounted.ok()) << "the mount is refused: " << mounted.status().message();
        EXPECT_EQ(mounted.value()->plan_stats().generation(), 0u)
            << "a torn head was believed";
        reclaim_census::CleanShutdownCheckpoint(*mounted.value());
    }
    // The head was made again: the next clean stop's priors come back.
    const Priors observed = [&] {
        auto opened = reclaim_census::Mount(live.path);
        EXPECT_TRUE(opened.ok()) << opened.status().message();
        if (!opened.ok()) return Priors{};
        CurrentCoreGuard as(0);
        const Priors bands = {{OidOf(*opened.value(), "cargos"), stats::BandOf(64.0)}};
        opened.value()->plan_stats().SeedPriors(bands);
        const Priors held = opened.value()->plan_stats().ExportPriors();
        opened.value()->WriteRelationPriors();
        reclaim_census::CleanShutdownCheckpoint(*opened.value());
        return held;
    }();
    ASSERT_EQ(observed.size(), 1u);
    auto again = reclaim_census::Mount(live.path);
    ASSERT_TRUE(again.ok()) << again.status().message();
    EXPECT_EQ(again.value()->plan_stats().ExportPriors(), observed);
}

}  // namespace
}  // namespace kds::server
