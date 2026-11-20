#include "kds/wal/checkpointer.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/page_store_checkpoint_target.hpp"
#include "kds/wal/memory_log_device.hpp"

// BI-S2 (BI-R1, BI-R3, BI-R4): the cadence's checkpoint run is a `system`
// task that takes one step per poll, the gate's hold lives in the
// checkpointer, and shutdown abandons a parked run and starts fresh.

namespace kds::wal {
namespace {

constexpr std::uint64_t kSegmentSize = 1024 * 1024;

class ScriptedTarget final : public CheckpointTarget {
public:
    std::vector<CheckpointDirtyPage> DirtyTable() const override { return dirty_; }
    Status FlushPages(std::span<const PageId> page_ids) override {
        ++batches_;
        for (PageId id : page_ids) {
            std::erase_if(dirty_, [id](const CheckpointDirtyPage& p) { return p.page_id == id; });
        }
        return Status::OK();
    }
    void SetDirty(std::vector<CheckpointDirtyPage> dirty) { dirty_ = std::move(dirty); }
    std::uint64_t batches() const noexcept { return batches_; }

private:
    std::vector<CheckpointDirtyPage> dirty_;
    std::uint64_t batches_ = 0;
};

// `n` dirty pages, each with a recLSN of `rec_lsn`.
std::vector<CheckpointDirtyPage> Pages(int n, Lsn rec_lsn) {
    std::vector<CheckpointDirtyPage> out;
    for (int i = 0; i < n; ++i) out.push_back({static_cast<PageId>(100 + i), rec_lsn});
    return out;
}

class CheckpointTaskTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto device = MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(device.ok());
        device_ = std::move(device.value());
        auto wal = WalManager::Open(device_.get(), clock_, 0);
        ASSERT_TRUE(wal.ok()) << wal.status().message();
        wal_ = std::move(wal.value());
    }

    std::unique_ptr<Checkpointer> Make(ScriptedTarget& target, InMemoryCheckpointAnchor& anchor,
                                       std::size_t pages_per_step = 2) {
        return std::make_unique<Checkpointer>(*wal_, target, txns_, anchor,
                                              CheckpointerConfig{.pages_per_step = pages_per_step});
    }

    // Turns the reactor until the run task is gone, returning the turns.
    int Drain(Checkpointer& c) {
        int turns = 0;
        while (c.run_in_flight() && turns < 1000) {
            scheduler_.RunOnce();
            ++turns;
        }
        return turns;
    }

    CurrentCoreGuard as_core_0_{0};
    sched::ManualClock clock_{1'000};
    sched::NullIoBackend io_;
    sched::Scheduler scheduler_{clock_, io_};
    std::unique_ptr<MemoryLogDevice> device_;
    std::unique_ptr<WalManager> wal_;
    NoActiveTransactions txns_;
    CheckpointGate gate_;
};

TEST_F(CheckpointTaskTest, TheCallbackReturnsWithoutRunningAStep) {
    ScriptedTarget target;
    target.SetDirty(Pages(6, 1));
    InMemoryCheckpointAnchor anchor;
    auto c = Make(target, anchor);

    c->SubmitRun(scheduler_, &gate_, 0);
    EXPECT_TRUE(c->run_in_flight());
    EXPECT_EQ(c->stats().started, 0u) << "the submit ran the checkpoint's Start inline";
    EXPECT_EQ(target.batches(), 0u) << "the submit ran a step inline";
}

TEST_F(CheckpointTaskTest, ARunOfNStepsSpansAtLeastNIterations) {
    ScriptedTarget target;
    target.SetDirty(Pages(10, 1));  // five steps of two
    InMemoryCheckpointAnchor anchor;
    auto c = Make(target, anchor);

    c->SubmitRun(scheduler_, &gate_, 0);
    const int turns = Drain(*c);
    EXPECT_EQ(target.batches(), 5u);
    EXPECT_GE(turns, 5) << "steps were taken more than one per reactor iteration";
    EXPECT_EQ(anchor.publishes(), 1u);
    EXPECT_EQ(c->stats().completed, 1u);
}

TEST_F(CheckpointTaskTest, AForegroundTaskIsPolledBetweenTwoSteps) {
    ScriptedTarget target;
    target.SetDirty(Pages(10, 1));
    InMemoryCheckpointAnchor anchor;
    auto c = Make(target, anchor);

    c->SubmitRun(scheduler_, &gate_, 0);
    scheduler_.RunOnce();  // the run's Start
    std::vector<std::uint64_t> seen;
    Checkpointer* run = c.get();
    auto foreground = [](ScriptedTarget* t, Checkpointer* r,
                         std::vector<std::uint64_t>* out) -> sched::Coro {
        while (r->run_in_flight()) {
            out->push_back(t->batches());
            co_await sched::Yield{};
        }
        co_return Status::OK();
    };
    scheduler_.Submit(sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                                          foreground(&target, run, &seen)));
    Drain(*c);
    const bool between = std::any_of(seen.begin(), seen.end(), [](std::uint64_t b) {
        return b > 0 && b < 5;
    });
    EXPECT_TRUE(between) << "the foreground task never ran while the run was between steps";
}

TEST_F(CheckpointTaskTest, ATickDuringARunSubmitsNothing) {
    ScriptedTarget target;
    target.SetDirty(Pages(10, 1));
    InMemoryCheckpointAnchor anchor;
    auto c = Make(target, anchor);

    const std::uint64_t edges_before = CheckpointCensus::Of(0).run_edges.load();
    c->SubmitRun(scheduler_, &gate_, 0);
    scheduler_.RunOnce();
    c->SubmitRun(scheduler_, &gate_, 0);  // the next tick, mid-run
    Drain(*c);
    for (int i = 0; i < 10; ++i) scheduler_.RunOnce();  // anything a second task would do
    EXPECT_EQ(c->stats().started, 1u);
    EXPECT_EQ(anchor.publishes(), 1u);
    EXPECT_EQ(CheckpointCensus::Of(0).run_edges.load() - edges_before, 2u)
        << "a second task ran: one run is two edges";
    EXPECT_TRUE(CheckpointCensus::Of(0).last_completed.load());
}

TEST_F(CheckpointTaskTest, ShutdownCheckpointsPublishWithARunParkedOnAnotherCore) {
    ScriptedTarget parked_target;
    parked_target.SetDirty(Pages(10, 1));
    ScriptedTarget other_target;
    InMemoryCheckpointAnchor parked_anchor;
    InMemoryCheckpointAnchor core1_anchor;
    InMemoryCheckpointAnchor core0_anchor;
    auto core2 = Make(parked_target, parked_anchor);
    auto core1 = Make(other_target, core1_anchor);
    auto core0 = Make(other_target, core0_anchor);

    // Core 2's run takes the gate and parks mid-steps; its reactor stops.
    core2->SubmitRun(scheduler_, &gate_, 2);
    scheduler_.RunOnce();
    scheduler_.RunOnce();
    ASSERT_TRUE(core2->run_in_flight());
    ASSERT_EQ(parked_target.batches(), 1u);

    // Shutdown: every core's run abandoned first, then each checkpoint.
    core2->Abandon();
    core1->Abandon();
    core0->Abandon();
    ASSERT_TRUE(core1->RunGated(&gate_, 1).ok());
    ASSERT_TRUE(core0->RunGated(&gate_, 0).ok());
    EXPECT_EQ(core1_anchor.publishes(), 1u) << "core 1's shutdown checkpoint skipped";
    EXPECT_EQ(core0_anchor.publishes(), 1u) << "core 0's shutdown checkpoint skipped";
    EXPECT_FALSE(core2->run_in_flight());
    EXPECT_FALSE(core2->in_progress());

    // The parked frame, polled after all, touches nothing.
    scheduler_.RunOnce();
    EXPECT_EQ(parked_target.batches(), 1u);
    EXPECT_EQ(parked_anchor.publishes(), 0u);
}

TEST_F(CheckpointTaskTest, AShutdownCheckpointStartsFromItsOwnBegin) {
    ScriptedTarget target;
    InMemoryCheckpointAnchor anchor;
    auto c = Make(target, anchor);

    // A real record for the parked snapshot's oldest page to point at.
    auto old = wal_->Append({RecordType::kCheckpointEnd, kNoTxnId, kInvalidPageId, 0},
                            std::vector<std::byte>(kCheckpointEndPayloadSize));
    ASSERT_TRUE(old.ok());
    target.SetDirty(Pages(10, old.value()));
    c->SubmitRun(scheduler_, &gate_, 0);
    scheduler_.RunOnce();
    scheduler_.RunOnce();
    ASSERT_TRUE(c->in_progress());

    // Shutdown: abandoned, the store synced (nothing dirty), then a fresh run.
    c->Abandon();
    target.SetDirty({});
    ASSERT_TRUE(c->RunGated(&gate_, 0).ok());
    ASSERT_EQ(anchor.publishes(), 1u);
    EXPECT_EQ(anchor.anchor().redo_start_lsn, anchor.anchor().checkpoint_lsn)
        << "the shutdown checkpoint published the parked snapshot's redo start";
    EXPECT_GT(anchor.anchor().redo_start_lsn, old.value());
}

TEST_F(CheckpointTaskTest, AParkedRunHoldsNoPin) {
    // BI-R3's pin half, which the suspend audit checks on core 0 only.
    auto device = storage::MemoryPageDevice::Create(/*extent_pages=*/512, /*initial_pages=*/0);
    ASSERT_TRUE(device.ok());
    auto store = storage::DevicePageStore::Open(*device.value(), storage::FrameCapacity{4096},
                                                /*first_new_page_id=*/16);
    ASSERT_TRUE(store.ok());
    for (int i = 0; i < 12; ++i) {
        auto made = store.value()->CreateNew();
        ASSERT_TRUE(made.ok());
        storage::FormatPage(made.value().second.bytes(), PageType::kHeap);
    }
    storage::PageStoreCheckpointTarget target(*store.value());
    InMemoryCheckpointAnchor anchor;
    Checkpointer c(*wal_, target, txns_, anchor, CheckpointerConfig{.pages_per_step = 3});
    c.SubmitRun(scheduler_, &gate_, 0);
    int turns = 0;
    while (c.run_in_flight() && turns < 100) {
        scheduler_.RunOnce();
        ++turns;
        EXPECT_EQ(store.value()->live_pins(), 0u) << "a pin outlived the poll, at turn " << turns;
    }
    EXPECT_EQ(anchor.publishes(), 1u);
    EXPECT_GE(turns, 4);
}

// ---- The gate's rotation (BI-Q7 (a)) ---------------------------------------

// Four cores staggered over a 5 s period, each run 2 s long: the shape that
// let cores 0 and 2 win every period and cores 1 and 3 skip for good.
std::vector<int> EntriesOverTime(std::uint64_t rotation) {
    CheckpointGate gate;
    gate.SetRotation(rotation);
    constexpr std::uint64_t kSec = 1'000'000'000;
    constexpr std::uint64_t kPeriod = 5 * kSec;
    constexpr std::uint64_t kRun = 2 * kSec;
    std::vector<int> entries(4, 0);
    std::optional<CheckpointGate::Hold> running;  // the run in progress, if any
    std::uint64_t running_until = 0;
    for (std::uint64_t t = 0; t < 60 * kSec; t += kSec / 4) {
        if (running.has_value() && t >= running_until) running.reset();
        for (std::uint32_t core = 0; core < 4; ++core) {
            if (t % kPeriod != core * kPeriod / 4) continue;  // this core's tick
            if (running.has_value()) {
                const CheckpointGate::Hold attempt(&gate, core, t);  // refused; the ask counts
                continue;
            }
            running.emplace(&gate, core, t);
            if (!running->entered()) {
                running.reset();
                continue;
            }
            ++entries[core];
            running_until = t + kRun;
        }
    }
    return entries;
}

TEST(CheckpointGateRotationTest, EveryCoreRunsWhenRunsOutlastTheStagger) {
    const std::vector<int> fixed = EntriesOverTime(5'000'000'000);
    for (int core = 0; core < 4; ++core) {
        EXPECT_GE(fixed[core], 2) << "core " << core << " ran " << fixed[core] << " times in 60 s";
    }
}

TEST(CheckpointGateRotationTest, ACoreThatStoppedAskingDoesNotHoldTheRestBack) {
    CheckpointGate gate;
    constexpr std::uint64_t kPeriod = 1000;
    gate.SetRotation(kPeriod);
    { const CheckpointGate::Hold first(&gate, 1, 0); ASSERT_TRUE(first.entered()); }
    // Core 0 asks while core 1 has the run, and never asks again: inside the
    // period it is owed the next turn ...
    {
        const CheckpointGate::Hold held(&gate, CheckpointGate::kAnyCore, 0);
        const CheckpointGate::Hold refused(&gate, 0, 10);
        ASSERT_FALSE(refused.entered());
    }
    { const CheckpointGate::Hold early(&gate, 1, 500); EXPECT_FALSE(early.entered()); }
    // ... through the second period too (DH: a tick that fires late is not a
    // core that stopped asking) ...
    { const CheckpointGate::Hold still(&gate, 1, 10 + kPeriod + 1); EXPECT_FALSE(still.entered()); }
    // ... and past that, core 1 goes again.
    {
        const CheckpointGate::Hold late(&gate, 1,
                                        10 + CheckpointGate::kRecentAskPeriods * kPeriod + 1);
        EXPECT_TRUE(late.entered());
    }
}

TEST(CheckpointGateRotationTest, AnAttemptNamingNoCoreIsNeverRotatedAway) {
    // Shutdown's and mount's attempt must not skip because a cadence turn is
    // owed to another core.
    CheckpointGate gate;
    gate.SetRotation(1000);
    { const CheckpointGate::Hold owed(&gate, 3, 0); }  // core 3 asked and ran: entry at 0
    { const CheckpointGate::Hold asked(&gate, 2, 5); } // core 2 asked: owed the turn, too soon
    const CheckpointGate::Hold shutdown(&gate);
    EXPECT_TRUE(shutdown.entered());
}

TEST(CheckpointGateRotationTest, AnAskStampedAfterThisCoresReadingStillCounts) {
    // Two cores read one clock at different instants: core 1's ask, stamped
    // later than core 0's reading, is recent, not ancient.
    CheckpointGate gate;
    gate.SetRotation(1000);
    { const CheckpointGate::Hold ran(&gate, 0, 100); ASSERT_TRUE(ran.entered()); }
    {
        const CheckpointGate::Hold held(&gate, CheckpointGate::kAnyCore, 0);
        const CheckpointGate::Hold asked(&gate, 1, 900);  // refused: the run is held
        ASSERT_FALSE(asked.entered());
    }
    // Core 0's reading predates core 1's ask, and is past the spacing (DH),
    // so it is the turn, not the spacing, that refuses it.
    const CheckpointGate::Hold behind(&gate, 0, 800);
    EXPECT_FALSE(behind.entered()) << "core 1, which never ran, is owed the turn";
}

// ---- The cadence is the instance's (DH-Q10 (a)) -----------------------------

// Four cores ticking in phase, `skew` apart in `order`, each run `run` long:
// how often each core entered over `periods` periods, and when.
struct InPhase {
    std::vector<int> entries = std::vector<int>(4, 0);
    std::vector<std::uint64_t> at;
};
InPhase InPhaseEntries(std::uint64_t period, std::uint64_t skew, std::uint64_t run, int periods,
                       const std::vector<std::uint32_t>& order = {0, 1, 2, 3}) {
    CheckpointGate gate;
    gate.SetRotation(period);
    InPhase out;
    std::uint64_t running_until = 0;
    std::optional<CheckpointGate::Hold> running;
    for (int n = 1; n <= periods; ++n) {
        for (std::size_t i = 0; i < order.size(); ++i) {
            const std::uint64_t t = static_cast<std::uint64_t>(n) * period + i * skew;
            if (running.has_value() && t >= running_until) running.reset();
            if (running.has_value()) {
                const CheckpointGate::Hold attempt(&gate, order[i], t);  // refused; the ask counts
                EXPECT_FALSE(attempt.entered());
                continue;
            }
            running.emplace(&gate, order[i], t);
            if (!running->entered()) {
                running.reset();
                continue;
            }
            ++out.entries[order[i]];
            out.at.push_back(t);
            running_until = t + run;
        }
    }
    return out;
}

TEST(CheckpointGateCadenceTest, OneRunAPeriodEachCoreInTurn) {
    constexpr std::uint64_t kMs = 1'000'000;
    // A run (2 ms) shorter than the skew between two cores' ticks (3 ms):
    // the next core in turn asks after it has ended, and still waits.
    const InPhase ran = InPhaseEntries(5000 * kMs, 3 * kMs, 2 * kMs, /*periods=*/12);
    ASSERT_EQ(ran.at.size(), 12u) << "one run a period";
    for (int core = 0; core < 4; ++core) EXPECT_EQ(ran.entries[core], 3) << "core " << core;
    for (std::size_t i = 1; i < ran.at.size(); ++i) {
        EXPECT_GE(ran.at[i] - ran.at[i - 1], 5000 * kMs - 9 * kMs) << "run " << i;
        EXPECT_LE(ran.at[i] - ran.at[i - 1], 5000 * kMs + 9 * kMs) << "run " << i;
    }
}

TEST(CheckpointGateCadenceTest, TheOrderOfArrivalDoesNotChooseTheCore) {
    constexpr std::uint64_t kMs = 1'000'000;
    // Core 3 always asks first, and the turn still goes round: the first
    // asker is refused when the turn is another core's.
    const InPhase ran = InPhaseEntries(5000 * kMs, 3 * kMs, 2 * kMs, /*periods=*/12, {3, 2, 1, 0});
    ASSERT_EQ(ran.at.size(), 12u);
    for (int core = 0; core < 4; ++core) EXPECT_EQ(ran.entries[core], 3) << "core " << core;
}

TEST(CheckpointGateCadenceTest, ATickThatFiresLateDoesNotTakeAnotherCoresTurn) {
    // Core 0's second tick fires 10 late, by which time core 1's ask of the
    // first period is more than one period old. Were "recent" one period,
    // core 0 would read core 1 as a core that stopped asking and run again.
    CheckpointGate gate;
    gate.SetRotation(1000);
    { const CheckpointGate::Hold first(&gate, 0, 1000); ASSERT_TRUE(first.entered()); }
    { const CheckpointGate::Hold soon(&gate, 1, 1003); ASSERT_FALSE(soon.entered()); }
    { const CheckpointGate::Hold late(&gate, 0, 2010); EXPECT_FALSE(late.entered()); }
    { const CheckpointGate::Hold owed(&gate, 1, 2020); EXPECT_TRUE(owed.entered()); }
}

TEST(CheckpointGateCadenceTest, ARunThatOutlastsThePeriodCostsATurnNotACore) {
    constexpr std::uint64_t kMs = 1'000'000;
    // Every run is 1.2 periods long, so every second period meets it: the
    // cores still take turns, at half the rate.
    const InPhase ran = InPhaseEntries(5000 * kMs, 3 * kMs, 6000 * kMs, /*periods=*/16);
    EXPECT_EQ(ran.at.size(), 8u);
    for (int core = 0; core < 4; ++core) EXPECT_EQ(ran.entries[core], 2) << "core " << core;
}

TEST(CheckpointGateCadenceTest, AnAttemptNamingNoCoreIsNotSpaced) {
    // Shutdown's, mount's and SYNC's runs are not the cadence's: one is
    // admitted right behind a cadence run and does not push the next back.
    CheckpointGate gate;
    gate.SetRotation(1000);
    { const CheckpointGate::Hold cadence(&gate, 0, 1000); ASSERT_TRUE(cadence.entered()); }
    { const CheckpointGate::Hold sync(&gate); EXPECT_TRUE(sync.entered()); }
    { const CheckpointGate::Hold soon(&gate, 1, 1400); EXPECT_FALSE(soon.entered()); }
    { const CheckpointGate::Hold next(&gate, 1, 2000); EXPECT_TRUE(next.entered()); }
}

TEST(CheckpointGateRotationTest, FirstComeStarvesHalfTheCores) {
    // The defect the rotation exists for, shown on the unrotated gate.
    const std::vector<int> first_come = EntriesOverTime(0);
    EXPECT_EQ(first_come[1], 0);
    EXPECT_EQ(first_come[3], 0);
}

}  // namespace
}  // namespace kds::wal
