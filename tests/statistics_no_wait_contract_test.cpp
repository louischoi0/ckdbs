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

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/latch.hpp"
#include "kds/sched/wait_state.hpp"
#include "kds/stats/access_stats.hpp"
#include "kds/stats/ash_ring.hpp"
#include "kds/stats/checkpoint_log.hpp"
#include "kds/stats/index_stats.hpp"
#include "kds/stats/leaf_stats.hpp"
#include "kds/stats/lock_wait_log.hpp"
#include "kds/stats/plan_stats.hpp"
#include "kds/stats/relation_stats.hpp"
#include "kds/stats/statement_stats.hpp"

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
