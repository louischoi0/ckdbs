// **Statistics queue no statement** (`docs/rules/rules.md` §3a;
// `instructions/v3.2.0/workorder-dl-statistics-queue-no-statement.md` DL-Q8):
// the contract suite. A store at a time, every latch the store owns is held
// from the test's thread - the way a reader's copy or a collection task
// holds it, and for as long as the test likes - while each record entry
// point runs on a second thread under a deadline. A record that waits does
// not return under the hold and fails its cell; a record that is lost to
// the hold is counted where a reader can see it (DL-R2).
//
// A store with no latch has nothing to hold: its cells run records beside
// readers without pause, and under the thread sanitizer show that a read
// beside a write is no data race (DL-R8).
//
// **Where the rest of the suite is.** The Cabin controller's collector, a
// part a core: `optimizer_signals_test.cpp` (`ANoteNeverWaitsForAHeldPart...`,
// `EveryNoteBesideAFoldLandsOrIsCounted`). The latch-free table under the
// oid-keyed stores: `counter_table_test.cpp`. The
// planner's steward under each latch it tries: `plan_stats_test.cpp`
// (`AStewardStepReturnsUnderHeldLatches...`). The controller's views and the
// tick's publish: `core_runtime_test.cpp` (`APeerHandedTheOptimizerSurface...`).
// The debug guard that aborts a record that would wait:
// `statistics_scope_test.cpp`, armed in every debug cell of the suite.
//
// **The second rule's cells** - a switched-off consumer collects nothing -
// one a switch: `cabin_optimizer`, `optimizer_signals_test.cpp`
// (`ASwitchedOffControllerCollectsNothing`,
// `TheDispatcherBuildsNothingForASwitchedOffController`), and where no tick
// can run - `cabins = off`, `cabin_optimizer_snapshot_interval_ms = 0` -
// `expeditor_test.cpp` (`WhereNoTickCanRunNoCoreIsHandedACollector`); `access_statistics`,
// `access_stats_test.cpp` (`TheSwitchOffRecordsNothing`); `ash_enabled`,
// `ash_sampler_test.cpp` (`TurningSamplingOffKeepsTheRingAndOnContinuesIt`);
// `join_order = written`, `plan_stats_test.cpp`
// (`UnderWrittenAJoinedStatementGathersNothingForThePlanner`).

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/latch.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/wait_state.hpp"
#include "kds/stats/access_stats.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/ash_ring.hpp"
#include "kds/stats/checkpoint_log.hpp"
#include "kds/stats/index_stats.hpp"
#include "kds/stats/leaf_stats.hpp"
#include "kds/stats/lock_wait_log.hpp"
#include "kds/stats/plan_stats.hpp"
#include "kds/stats/relation_stats.hpp"
#include "kds/stats/refusal_stats.hpp"
#include "kds/stats/statement_stats.hpp"
#include "kds/stats/txn_stats.hpp"

namespace kds::stats {
namespace {

// Runs `record` on a second thread while `latch` is held here, and answers
// whether it returned under the hold. The hold is let go before the join, so
// a record that did wait finishes and the cell fails instead of hanging.
bool ReturnsUnderTheHold(Latch* latch, const std::function<void()>& record) {
    std::atomic<bool> returned{false};
    std::optional<LatchGuard> held(std::in_place, latch);
    std::thread peer([&] {
        record();
        returned.store(true);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!returned.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool under_the_hold = returned.load();
    held.reset();
    peer.join();
    return under_the_hold;
}

// Runs `writer` on a second thread while `reader` loops here until it is
// done: the shape of a view read beside a core's records.
void BesideAReader(const std::function<void()>& writer, const std::function<void()>& reader) {
    std::atomic<bool> done{false};
    std::thread peer([&] {
        writer();
        done.store(true, std::memory_order_release);
    });
    while (!done.load(std::memory_order_acquire)) reader();
    peer.join();
    reader();
}

// ---- The stores a record tries (W2) -----------------------------------------

TEST(StatisticsNoWaitContract, AStatementsRecordReturnsUnderAHeldPartAndIsCounted) {
    StatementStats::ResetForTest();
    StatementStats::Record(3, 42, "SELECT v FROM t WHERE id = 1", false, 10, 1'000);
    ASSERT_TRUE(ReturnsUnderTheHold(StatementStats::PartLatchForTest(3), [] {
        StatementStats::Record(3, 42, "SELECT v FROM t WHERE id = 2", false, 10, 2'000);
        StatementStats::Record(3, 43, "SELECT w FROM t WHERE id = 2", false, 10, 2'000);
        // Another core's part is not this hold's.
        StatementStats::Record(4, 42, "SELECT v FROM t WHERE id = 3", false, 10, 3'000);
    })) << "a statement's record waited for its part's latch";
    EXPECT_EQ(StatementStats::Busy(), 2u);
    EXPECT_EQ(StatementStats::Dropped(), 0u) << "a busy part is not a full one";
    const std::vector<StatementShape> shapes = StatementStats::Snapshot();
    ASSERT_EQ(shapes.size(), 1u);
    EXPECT_EQ(shapes[0].calls, 2u) << "core 3's first record and core 4's";
    StatementStats::ResetForTest();
}

TEST(StatisticsNoWaitContract, ALockWaitsRowReturnsUnderAHeldPartAndItsRelationStillCounts) {
    LockWaitLog::ResetForTest();
    RelationStats::ResetForTest();
    LockWaitRecord row;
    row.core = 2;
    row.rel_oid = 77;
    row.waited_us = 5;
    ASSERT_TRUE(ReturnsUnderTheHold(LockWaitLog::PartLatchForTest(2),
                                    [&] { LockWaitLog::Record(row); }))
        << "a lock wait's row waited for its part's latch";
    const LockWaitLog::Totals totals = LockWaitLog::Read();
    EXPECT_EQ(totals.busy, 1u);
    EXPECT_EQ(totals.recorded, 0u);
    EXPECT_TRUE(LockWaitLog::Snapshot().empty());
    // The relation's cumulative columns take no latch: the wait is counted
    // there though the ring lost its row.
    EXPECT_EQ(RelationStats::Snapshot()[77].lock_waits.get(), 1u);
    LockWaitLog::ResetForTest();
    RelationStats::ResetForTest();
}

TEST(StatisticsNoWaitContract, ACheckpointsRowReturnsUnderAHeldPart) {
    CheckpointLog::ResetForTest();
    CheckpointRunReport report;
    ASSERT_TRUE(ReturnsUnderTheHold(CheckpointLog::PartLatchForTest(1),
                                    [&] { CheckpointLog::Record(1, report, 1'000); }))
        << "a checkpoint run's row waited for its part's latch";
    EXPECT_EQ(CheckpointLog::Read().busy, 1u);
    EXPECT_TRUE(CheckpointLog::Snapshot().empty());
    CheckpointLog::ResetForTest();
}

TEST(StatisticsNoWaitContract, ASampleReturnsUnderAHeldRing) {
    AshRing ring(64 * kAshRecordBytes);
    AshRecord record;
    ring.Record(record);
    ASSERT_TRUE(ReturnsUnderTheHold(ring.LatchForTest(), [&] { ring.Record(record); }))
        << "the sampler waited for its ring's latch";
    EXPECT_EQ(ring.busy(), 1u);
    EXPECT_EQ(ring.recorded(), 1u);
    EXPECT_EQ(ring.Snapshot().size(), 1u);
}

TEST(StatisticsNoWaitContract, APlannedStatementsFoldReturnsUnderAHeldPart) {
    PlanStats stats(/*cores=*/2);
    PlanObservation observation;
    observation.key.fetch_id = 9;
    observation.key.n = 2;
    observation.rows[1] = 5;
    observation.present = 1u << 1;
    const catalog::Oid wanted[] = {11, 12};
    ASSERT_TRUE(ReturnsUnderTheHold(stats.PartLatchForTest(1), [&] {
        stats.Fold(1, observation);
        stats.WantPriors(1, wanted);
    })) << "a planned statement's end waited for its part's latch";
    EXPECT_EQ(stats.counters().part_busy, 2u);
    EXPECT_EQ(stats.counters().part_drops, 0u);
}

// Records beside a reader of a latched store: nothing waits, every record
// is in the store or counted busy, and the counts a reader loads without
// the latch are read beside their writer (the thread sanitizer's cell).
TEST(StatisticsNoWaitContract, StatementRecordsBesideAReaderAreTakenOrCounted) {
    StatementStats::ResetForTest();
    constexpr std::uint64_t kRounds = 20000;
    BesideAReader(
        [&] {
            for (std::uint64_t i = 0; i < kRounds; ++i) {
                // 64 shapes, first seen as the reader runs: its reserve is
                // outgrown and retried.
                StatementStats::Record(5, 1000 + i % 64, "SELECT v FROM t WHERE id = 1", false, 1,
                                       static_cast<std::int64_t>(i));
            }
        },
        [&] {
            std::uint64_t calls = 0;
            for (const StatementShape& shape : StatementStats::Snapshot()) calls += shape.calls;
            EXPECT_LE(calls, kRounds);
            (void)StatementStats::Dropped();
            (void)StatementStats::Busy();
        });
    std::uint64_t calls = 0;
    for (const StatementShape& shape : StatementStats::Snapshot()) calls += shape.calls;
    EXPECT_EQ(calls + StatementStats::Busy(), kRounds) << "a record neither taken nor counted";
    EXPECT_EQ(StatementStats::Dropped(), 0u);
    StatementStats::ResetForTest();
}

TEST(StatisticsNoWaitContract, LockWaitRowsBesideAReaderAreTakenOrCounted) {
    LockWaitLog::ResetForTest();
    RelationStats::ResetForTest();
    constexpr std::uint64_t kRounds = 20000;
    LockWaitRecord row;
    row.core = 6;
    row.rel_oid = 9;
    BesideAReader(
        [&] {
            for (std::uint64_t i = 0; i < kRounds; ++i) LockWaitLog::Record(row);
        },
        [] {
            std::uint64_t last = 0;
            for (const LockWaitRecord& r : LockWaitLog::Snapshot()) {
                EXPECT_GT(r.seq, last) << "a part's rows out of order";
                last = r.seq;
            }
            (void)LockWaitLog::Read();
        });
    const LockWaitLog::Totals totals = LockWaitLog::Read();
    EXPECT_EQ(totals.recorded + totals.busy, kRounds) << "a row neither taken nor counted";
    EXPECT_EQ(totals.dropped, 0u);
    // The relation's count takes no latch, so it lost nothing.
    EXPECT_EQ(RelationStats::Snapshot()[9].lock_waits.get(), kRounds);
    LockWaitLog::ResetForTest();
    RelationStats::ResetForTest();
}

// ---- A statement's read of a statistic (clause (d)) ---------------------------

// A compile's pin of the planner's snapshot waits for no install: under a
// held publish latch - the way an install's swap holds it - the pin returns
// with the snapshot it has, and takes the new one at its next pin.
TEST(StatisticsNoWaitContract, ACompilesPinReturnsUnderAHeldPublishLatchWithTheSnapshotItHas) {
    PlanStats stats(/*cores=*/1);
    stats.PublishForTest({{7, 4}}, {});
    PlanStatsSource source(&stats);
    const std::shared_ptr<const PlanStatsSnapshot> first = source.Pin();
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(first->generation(), stats.generation());

    stats.PublishForTest({{7, 6}}, {});
    ASSERT_NE(stats.generation(), first->generation());
    std::shared_ptr<const PlanStatsSnapshot> under;
    ASSERT_TRUE(ReturnsUnderTheHold(stats.PublishLatchForTest(), [&] { under = source.Pin(); }))
        << "a compile's pin waited for the publish latch";
    EXPECT_EQ(under, first) << "a pin that met the swap keeps the snapshot it has";
    const std::shared_ptr<const PlanStatsSnapshot> next = source.Pin();
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->generation(), stats.generation()) << "the next pin asks again";

    // A source with nothing cached yet prices from no statistics, as a
    // compile before the first publish does - and does not wait either.
    PlanStatsSource fresh(&stats);
    std::shared_ptr<const PlanStatsSnapshot> none = first;
    ASSERT_TRUE(ReturnsUnderTheHold(stats.PublishLatchForTest(), [&] { none = fresh.Pin(); }));
    EXPECT_EQ(none, nullptr);
    EXPECT_NE(fresh.Pin(), nullptr);
}

// ---- A DDL's forget of a statistic (DL-Q19) -----------------------------------

// A relation's drop forgets its managed statistics and waits for neither the
// managed store's latch - an operator's statistics statement holds it - nor
// an install: what it meets held is done by the steward's next step. With
// nothing held it is done where the drop ran.
TEST(StatisticsNoWaitContract, ARelationsDropForgetsItsStatisticsAndWaitsForNoStatement) {
    PlanStats stats(/*cores=*/1);
    const auto publish = [&](catalog::Oid oid) {
        RelationStatistic stat;
        stat.rows = 50000;
        ASSERT_TRUE(stats.managed().Store(oid, stat, /*pending=*/false, /*force=*/false).ok());
        ASSERT_TRUE(stats.RepublishManaged());
        ASSERT_TRUE(stats.Current()->Managed(oid));
    };
    stats.PublishForTest({{7, 4}, {8, 4}, {9, 4}}, {});

    // Nothing held: forgotten and republished at once.
    publish(7);
    stats.ForgetRelation(7);
    EXPECT_FALSE(stats.Current()->Managed(7));

    // The store's latch held: the drop returns; the object goes at the
    // steward's next step.
    publish(8);
    ASSERT_TRUE(ReturnsUnderTheHold(stats.ManagedLatchForTest(), [&] { stats.ForgetRelation(8); }))
        << "a relation's drop waited for the managed store's latch";
    EXPECT_TRUE(stats.Current()->Managed(8)) << "forgotten under a latch another thread held";

    // An install in flight: the object goes where the drop ran, and its
    // republish is owed to the steward.
    publish(9);
    ASSERT_TRUE(ReturnsUnderTheHold(stats.InstallLatchForTest(), [&] { stats.ForgetRelation(9); }))
        << "a relation's drop waited for an install";
    EXPECT_TRUE(stats.managed().PublishedBands().size() == 1u)
        << "relation 9's object was not forgotten, or relation 8's already was";
    EXPECT_TRUE(stats.Current()->Managed(9)) << "republished under an installer's hold";

    // The steward's step: both are gone from what a compile prices from.
    // (No catalog or page is read: nothing is wanted and no prior is due.)
    storage::InMemoryPageStore store{server::kFirstUserPageId};
    auto boot = bootstrap::BootstrapDatabase(store, 1000);
    ASSERT_TRUE(boot.ok()) << boot.status().message();
    ASSERT_TRUE(stats.StewardStep(boot.value().catalog, store, /*now_ns=*/1).ok());
    EXPECT_TRUE(stats.managed().PublishedBands().empty());
    EXPECT_FALSE(stats.Current()->Managed(8));
    EXPECT_FALSE(stats.Current()->Managed(9));
}

// Installs, republishes and pins at once, three threads: the thread
// sanitizer's cell for the snapshot's two latches and the generation.
TEST(StatisticsNoWaitContract, InstallsRepublishesAndPinsRunAtOnce) {
    PlanStats stats(/*cores=*/1);
    stats.PublishForTest({{7, 4}}, {});
    std::atomic<bool> done{false};
    std::thread republisher([&] {
        RelationStatistic stat;
        std::uint64_t rows = 16;
        while (!done.load(std::memory_order_acquire)) {
            stat.rows = rows;
            rows = rows > (std::uint64_t{1} << 40) ? 16 : rows * 4;  // a new band each time
            (void)stats.managed().Store(7, stat, /*pending=*/false, /*force=*/true);
            (void)stats.RepublishManaged();
        }
    });
    BesideAReader(
        [&] {
            for (std::int16_t band = 0; band < 2000; ++band) {
                stats.PublishForTest({{7, static_cast<std::int16_t>(band % 40)}}, {});
            }
        },
        [&] {
            PlanStatsSource source(&stats);
            std::uint64_t last = 0;
            for (int i = 0; i < 64; ++i) {
                const std::shared_ptr<const PlanStatsSnapshot> pinned = source.Pin();
                if (pinned == nullptr) continue;  // every try so far met a swap
                EXPECT_GE(pinned->generation(), last) << "a pin went back a generation";
                last = pinned->generation();
            }
        });
    done.store(true, std::memory_order_release);
    republisher.join();
}

// ---- The stores with no latch (W1): records beside a reader ------------------

TEST(StatisticsNoWaitContract, RelationAndLeafCountsRunBesideAReaderAndLoseNothing) {
    RelationStats::ResetForTest();
    LeafStats::ResetForTest();
    constexpr std::uint64_t kRounds = 20000;
    BesideAReader(
        [&] {
            for (std::uint64_t i = 0; i < kRounds; ++i) {
                RelationStats::Record(0, 100 + i % 8, RelationWrite::kInsert, 1);
                RelationStats::RecordLockWait(0, 100 + i % 8, 3, false, false);
                LeafStats::CountDivision(200 + i % 8);
            }
        },
        [] {
            (void)RelationStats::Snapshot();
            (void)LeafStats::Snapshot();
            (void)RelationStats::Dropped();
        });
    std::uint64_t inserts = 0, waits = 0, divisions = 0;
    for (const auto& [oid, a] : RelationStats::Snapshot()) {
        inserts += a.inserts.get();
        waits += a.lock_waits.get();
    }
    for (const auto& [oid, a] : LeafStats::Snapshot()) divisions += a.divisions.get();
    EXPECT_EQ(inserts, kRounds);
    EXPECT_EQ(waits, kRounds);
    EXPECT_EQ(divisions, kRounds);
    RelationStats::ResetForTest();
    LeafStats::ResetForTest();
}

TEST(StatisticsNoWaitContract, AnIndexesFirstSightAndItsCountsRunBesideAReader) {
    IndexStats stats;
    constexpr std::uint64_t kIndexes = 256;
    BesideAReader(
        [&] {
            for (int round = 0; round < 50; ++round) {
                for (std::uint64_t oid = 1; oid <= kIndexes; ++oid) {
                    IndexUse* use = stats.CountsOf(0, oid);
                    ASSERT_NE(use, nullptr);
                    use->Add(/*range=*/false, 1, 1, 1, 0);
                }
            }
        },
        [&] {
            for (const auto& [oid, use] : stats.Snapshot()) EXPECT_LE(use.probes.get(), 50u);
        });
    const auto rows = stats.Snapshot();
    EXPECT_EQ(rows.size(), kIndexes);
    for (const auto& [oid, use] : rows) EXPECT_EQ(use.probes.get(), 50u) << oid;
}

// Records, a column change's forgets and a view's reads at once, three
// threads: nothing waits, a reader never shows a row that is two shapes',
// and a shape no forget names keeps every count. Each churned shape's
// relation is derived from its mask, so a row whose relation came from one
// shape and whose mask from another is told apart.
TEST(StatisticsNoWaitContract, AccessShapesAreRecordedForgottenAndReadAtOnce) {
    AccessStats store(256 * AccessStats::kSlotBytes);
    constexpr std::uint64_t kRounds = 20000;
    constexpr std::uint64_t kChurned = 16;
    const exec::AccessKind kind = exec::AccessKind::kLookup;
    const auto mask_of = [](std::uint64_t k) { return (std::uint64_t{1} << 3) | (k << 8); };
    std::atomic<bool> done{false};
    std::thread forgetter([&] {
        // Column 3 of every churned relation: every churned shape.
        while (!done.load(std::memory_order_acquire)) {
            for (std::uint64_t k = 0; k < kChurned; ++k) store.Forget(100 + k, 3);
        }
    });
    BesideAReader(
        [&] {
            for (std::uint64_t i = 0; i < kRounds; ++i) {
                store.Record(0, kind, 5, 1u << 1, i);  // never forgotten
                const std::uint64_t k = i % kChurned;
                store.Record(0, kind, 100 + k, mask_of(k), i);  // forgotten over and over
            }
        },
        [&] {
            for (const AccessShape& shape : store.Snapshot()) {
                if (shape.rel_id == 5) {
                    EXPECT_EQ(shape.column_mask, 1u << 1);
                } else {
                    ASSERT_GE(shape.rel_id, 100u);
                    ASSERT_LT(shape.rel_id, 100 + kChurned);
                    EXPECT_EQ(shape.column_mask, mask_of(shape.rel_id - 100))
                        << "a row that is two shapes'";
                }
                EXPECT_LE(shape.uses, kRounds);
            }
        });
    done.store(true, std::memory_order_release);
    forgetter.join();
    std::uint64_t kept_uses = 0;
    for (const AccessShape& shape : store.Snapshot()) {
        if (shape.rel_id == 5) kept_uses = shape.uses;
    }
    EXPECT_EQ(kept_uses, kRounds) << "a forget of another relation's shapes cost this one a count";
    EXPECT_EQ(store.Dropped(), 0u) << "a forgotten slot was not free again";

    // Everything forgotten, then every shape again: each is admitted as new
    // and counts from one - the forgets left nothing that crowds the part.
    for (std::uint64_t k = 0; k < kChurned; ++k) store.Forget(100 + k, 3);
    store.Forget(5, 1);
    EXPECT_TRUE(store.Snapshot().empty());
    for (std::uint64_t k = 0; k < kChurned; ++k) store.Record(0, kind, 100 + k, mask_of(k), 1);
    const std::vector<AccessShape> again = store.Snapshot();
    ASSERT_EQ(again.size(), kChurned);
    for (const AccessShape& shape : again) EXPECT_EQ(shape.uses, 1u);
    EXPECT_EQ(store.Dropped(), 0u);
}

// The stores that are words and nothing else - the refusal counts, the
// transaction counts, the Cabin store's totals and a Cabin's probe counts -
// counted beside a reader that sums them: nothing waits, nothing is lost
// (one writer a part), and a sum read beside its writer is no data race.
TEST(StatisticsNoWaitContract, TheWordStoresCountBesideAReaderAndLoseNothing) {
    RefusalStats::ResetForTest();
    TxnStats::ResetForTest();
    CabinStore cabins;
    constexpr std::uint64_t kRounds = 20000;
    BesideAReader(
        [&] {
            for (std::uint64_t i = 0; i < kRounds; ++i) {
                RefusalStats::Record(0, 3, static_cast<std::int64_t>(i));
                TxnStats::Count(0, TxnEnd::kCommit);
                TxnStats::RecordLife(0, /*read_only=*/false, 1000);
                cabins.NoteHit(1 + i % 4);
                cabins.NoteMiss(1 + i % 4);
            }
        },
        [&] {
            (void)RefusalStats::Snapshot();
            (void)TxnStats::CountsOf(0);
            (void)TxnStats::SnapshotLives();
            const CabinStore::Stats totals = cabins.stats();
            EXPECT_LE(totals.hits, kRounds);
            for (std::uint64_t cabin = 1; cabin <= 4; ++cabin) (void)cabins.InfoFor(cabin);
        });
    std::uint64_t refused = 0;
    for (const RefusalCount& r : RefusalStats::Snapshot()) refused += r.count;
    EXPECT_EQ(refused, kRounds);
    const CabinStore::Stats totals = cabins.stats();
    EXPECT_EQ(totals.hits, kRounds);
    EXPECT_EQ(totals.misses, kRounds);
    std::uint64_t probed = 0;
    for (std::uint64_t cabin = 1; cabin <= 4; ++cabin) {
        const CabinStore::CabinInfo info = cabins.InfoFor(cabin);
        probed += info.hits + info.misses;
    }
    EXPECT_EQ(probed, 2 * kRounds);
    EXPECT_EQ(cabins.use_dropped(), 0u);
    RefusalStats::ResetForTest();
    TxnStats::ResetForTest();
}

// Sessions come and go on two threads while a third lists them: nothing
// waits, an id is listed at most once, and a session that is alive across a
// whole listing is in it.
TEST(StatisticsNoWaitContract, SessionsRegisterAndLeaveBesideAListing) {
    const std::shared_ptr<sched::SessionWaitStats> resident = sched::MakeSessionWaitStats();
    const std::uint64_t resident_id = resident->session_id.get();
    std::atomic<int> running{2};
    std::vector<std::thread> churn;
    for (int t = 0; t < 2; ++t) {
        churn.emplace_back([&] {
            for (int i = 0; i < 5000; ++i) {
                std::shared_ptr<sched::SessionWaitStats> session = sched::MakeSessionWaitStats();
                session->on_cpu_ns.add(1);
            }
            running.fetch_sub(1);
        });
    }
    std::uint64_t listings = 0;
    while (running.load() != 0) {
        std::set<std::uint64_t> ids;
        bool saw_resident = false;
        for (const sched::SessionWaitStats& s : sched::SnapshotSessionWaits()) {
            const std::uint64_t id = s.session_id.get();
            EXPECT_NE(id, 0u);
            EXPECT_TRUE(ids.insert(id).second) << "session " << id << " listed twice";
            saw_resident |= id == resident_id;
        }
        EXPECT_TRUE(saw_resident) << "a session alive across the listing was left out";
        ++listings;
    }
    for (std::thread& t : churn) t.join();
    EXPECT_GT(listings, 0u);
}

}  // namespace
}  // namespace kds::stats
