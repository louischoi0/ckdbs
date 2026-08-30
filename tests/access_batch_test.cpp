#include "kds/stats/access_batch.hpp"

#include <gtest/gtest.h>

#include <optional>

#include "kds/catalog/catalog.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/access_stats_service.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// CR7/CR8 — a peer's access statistics, folded locally and flushed to core 0.
//
// What is worth pinning here is not that a counter increments, but the three
// properties the design rests on: the fold is by *shape*, the send is the one
// in this engine that may **drop**, and a peer's count reaches the same row
// the local path writes rather than a second one.

namespace kds::server {
namespace {

TEST(AccessBatchTest, AccessesFoldByShapeAndNotByStatement) {
    stats::AccessBatch batch;
    // Three executions of one shape, one of another. The fold is what makes
    // a batch a batch: without it this is four wire entries and CR7 asked
    // for one message rather than one per statement.
    batch.Note(/*kind=*/1, /*rel_id=*/7, /*column_mask=*/0b1, /*now=*/100);
    batch.Note(1, 7, 0b1, 200);
    batch.Note(1, 7, 0b1, 300);
    batch.Note(1, 7, /*column_mask=*/0b10, 400);

    ASSERT_EQ(batch.entries().size(), 2u);
    EXPECT_EQ(batch.entries()[0].count, 3u);
    EXPECT_EQ(batch.entries()[0].last_seen, 300u)
        << "last_seen must follow the newest execution of the shape";
    EXPECT_EQ(batch.entries()[1].count, 1u);
    // The mask is part of the shape, exactly as it is in the row: two
    // accesses to one relation on different columns are two shapes.
    EXPECT_NE(batch.entries()[0].column_mask, batch.entries()[1].column_mask);
}

TEST(AccessBatchTest, AShapeWithNoSlotLeftIsCountedRatherThanForcingASend) {
    stats::AccessBatch batch;
    for (std::size_t i = 0; i < stats::kAccessBatchCapacity; ++i) {
        batch.Note(1, static_cast<catalog::Oid>(100 + i), 0, 1);
    }
    ASSERT_EQ(batch.entries().size(), stats::kAccessBatchCapacity);
    EXPECT_EQ(batch.overflow_drops(), 0u);

    // The buffer is sized to one message and the tick empties it, so a
    // shape arriving with no slot is dropped and counted - never a send
    // from the statement path, which is what CR7 rules out.
    batch.Note(1, 9999, 0, 1);
    EXPECT_EQ(batch.entries().size(), stats::kAccessBatchCapacity);
    EXPECT_EQ(batch.overflow_drops(), 1u)
        << "a lost shape must be visible; SHOW META is what reports it";

    // A shape already present still folds when the buffer is full - the cap
    // is on distinct shapes, not on executions.
    batch.Note(1, 100, 0, 2);
    EXPECT_EQ(batch.overflow_drops(), 1u);
}

class AccessStatsWireTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto transport = sched::RealRingTransport::Create(/*core_count=*/2, /*slots=*/4,
                                                          sched::kCoreRingPayloadBytes);
        ASSERT_TRUE(transport.ok()) << transport.status().message();
        transport_.emplace(std::move(transport.value()));
        core0_.emplace(clock_, io0_);
        peer_.emplace(clock_, io1_);
        ASSERT_TRUE(core0_->AttachTransport(&*transport_, 0).ok());
        ASSERT_TRUE(peer_->AttachTransport(&*transport_, 1).ok());

        catalog_.emplace(store_);
        ASSERT_TRUE(catalog_->Bootstrap().ok());
        ASSERT_TRUE(RegisterAccessStatsBatchHandler(*core0_, *catalog_, &applied_).ok());
    }

    void Pump(int rounds = 8) {
        for (int i = 0; i < rounds; ++i) {
            peer_->RunOnce();
            core0_->RunOnce();
        }
    }

    sched::ManualClock clock_;
    sched::NullIoBackend io0_;
    sched::NullIoBackend io1_;
    std::optional<sched::RealRingTransport> transport_;
    std::optional<sched::Scheduler> core0_;
    std::optional<sched::Scheduler> peer_;
    storage::InMemoryPageStore store_{128};
    std::optional<catalog::Catalog> catalog_;
    stats::AccessBatchCounters applied_;
};

TEST_F(AccessStatsWireTest, APeersFoldReachesTheRowTheLocalPathWrites) {
    stats::AccessBatch batch;
    batch.Note(/*kind=*/1, /*rel_id=*/42, /*column_mask=*/0b101, /*now=*/900);
    batch.Note(1, 42, 0b101, 950);
    batch.Note(1, 42, 0b101, 990);

    ASSERT_TRUE(FlushAccessBatch(*transport_, /*from_core=*/1, /*system_core=*/0, batch).ok());
    EXPECT_TRUE(batch.empty()) << "a sent batch must be cleared, or the next flush double-counts";
    EXPECT_EQ(batch.counters().batches_sent, 1u);
    EXPECT_EQ(batch.counters().entries_sent, 1u);
    Pump();

    EXPECT_EQ(applied_.batches_applied, 1u);
    EXPECT_EQ(applied_.entries_applied, 1u);

    auto rows = catalog_->ListAccessStats();
    ASSERT_TRUE(rows.ok()) << rows.status().message();
    ASSERT_EQ(rows.value().size(), 1u);
    // **The count crossed as a count, not as an execution.** A batch applied
    // with the statement path's `++` would report three executions as one,
    // which is the whole reason `RecordAccess` took a `count`.
    EXPECT_EQ(rows.value()[0].use_count, 3u);
    EXPECT_EQ(rows.value()[0].rel_id, 42u);
    EXPECT_EQ(rows.value()[0].column_mask, 0b101u);
    EXPECT_EQ(rows.value()[0].last_seen, 990u);

    // And a peer's fold lands on the *same* row a later local access takes,
    // rather than beside it - one shape, one row, whichever core saw it.
    ASSERT_TRUE(catalog_->RecordAccess(1, 42, 0b101, 1000).ok());
    auto again = catalog_->ListAccessStats();
    ASSERT_TRUE(again.ok());
    ASSERT_EQ(again.value().size(), 1u);
    EXPECT_EQ(again.value()[0].use_count, 4u);
}

TEST_F(AccessStatsWireTest, AFullRingDropsTheBatchAndSaysSo) {
    // CR8, and it is the engine's one exception to the never-drop rule
    // (`sched/send_retry.hpp`): fill the ring with unconsumed messages, then
    // flush. Nothing retries and nothing blocks - the statistic is invariant
    // 8's advisory class, and a retry loop behind it would trade latency for
    // a count.
    stats::AccessBatch filler;
    for (int i = 0; i < 64; ++i) {
        filler.Note(1, static_cast<catalog::Oid>(i), 0, 1);
        // Sent but never pumped, so core 0 consumes none of them.
        ASSERT_TRUE(FlushAccessBatch(*transport_, 1, 0, filler).ok());
    }
    ASSERT_GT(filler.counters().batches_dropped, 0u)
        << "the ring never filled, so this test proves nothing about a drop";

    // The batch is cleared either way: holding it would grow one shape's
    // count without bound and then flush a number naming no interval.
    EXPECT_TRUE(filler.empty());

    // And nothing reached the catalog from the dropped ones. What did get
    // through is bounded by the ring, so the assertion is on the relation
    // being *behind*, never on it being empty.
    Pump();
    auto rows = catalog_->ListAccessStats();
    ASSERT_TRUE(rows.ok());
    EXPECT_LT(rows.value().size(), 64u)
        << "every batch was applied, so the ring did not fill and CR8 was not exercised";
}

// ---- CR6 / CB8: unlogged, and discarded when damaged ---------------------

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
}  // namespace kds::server
