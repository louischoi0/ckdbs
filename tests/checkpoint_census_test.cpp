#include "kds/sched/checkpoint_census.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/stats/checkpoint_log.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/page_store_checkpoint_target.hpp"
#include "kds/wal/checkpointer.hpp"
#include "kds/wal/memory_log_device.hpp"

// BI-S1's instrument (`sched/checkpoint_census.hpp`): a run's steps and pages,
// each phase's time landing in its own phase, and a reactor iteration's hold
// split by whether a run overlapped it.

namespace kds {
namespace {

constexpr std::uint32_t kCore = 5;  // the slot these cells write; each reads a delta or the last run
constexpr std::uint64_t kSegmentSize = 1024 * 1024;

std::uint64_t PhaseSum(std::uint32_t core, CheckpointPhase p) {
    return CheckpointCensus::Of(core).last_sum_ns[static_cast<std::size_t>(p)].load();
}

// A device whose writes and syncs take scripted time on the injected clock.
class SlowDevice final : public storage::PageDevice {
public:
    SlowDevice(storage::PageDevice& inner, sched::ManualClock& clock) : inner_(inner), clock_(clock) {}
    std::uint32_t page_capacity() const noexcept override { return inner_.page_capacity(); }
    Status ReadPage(PageId id, std::span<std::byte, kPageSize> out) override {
        return inner_.ReadPage(id, out);
    }
    Status WritePage(PageId id, std::span<const std::byte, kPageSize> in) override {
        clock_.Advance(write_ns);
        ++writes;
        return inner_.WritePage(id, in);
    }
    Status WritePageRun(PageId first, std::uint32_t n, std::span<const std::byte> in) override {
        clock_.Advance(write_ns);
        ++writes;
        return inner_.WritePageRun(first, n, in);
    }
    Status EnsureCapacity(std::uint32_t n) override { return inner_.EnsureCapacity(n); }
    Status Sync() override {
        clock_.Advance(sync_ns);
        ++syncs;
        return inner_.Sync();
    }

    std::uint64_t write_ns = 0;
    std::uint64_t sync_ns = 0;
    std::uint64_t writes = 0;
    std::uint64_t syncs = 0;

private:
    storage::PageDevice& inner_;
    sched::ManualClock& clock_;
};

class CheckpointCensusTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto device = storage::MemoryPageDevice::Create(/*extent_pages=*/512, /*initial_pages=*/0);
        ASSERT_TRUE(device.ok());
        memory_ = std::move(device.value());
        slow_ = std::make_unique<SlowDevice>(*memory_, clock_);
        auto store = storage::DevicePageStore::Open(*slow_, storage::FrameCapacity{4096},
                                                    /*first_new_page_id=*/16);
        ASSERT_TRUE(store.ok()) << store.status().message();
        store_ = std::move(store.value());
        auto log = wal::MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(log.ok());
        log_device_ = std::move(log.value());
        auto wal = wal::WalManager::Open(log_device_.get(), clock_, kCore);
        ASSERT_TRUE(wal.ok()) << wal.status().message();
        wal_ = std::move(wal.value());
        target_ = std::make_unique<storage::PageStoreCheckpointTarget>(*store_);
    }

    // `n` fresh pages, dirty and with ids far enough apart that every one is
    // its own write.
    void DirtyPages(int n) {
        for (int i = 0; i < n; ++i) {
            auto made = store_->CreateNew();
            ASSERT_TRUE(made.ok()) << made.status().message();
            storage::FormatPage(made.value().second.bytes(), PageType::kHeap);
            auto gap = store_->CreateNew();  // left clean by the flush below
            ASSERT_TRUE(gap.ok());
        }
    }

    CurrentCoreGuard as_core_{kCore};
    sched::ManualClock clock_{1'000'000};
    std::unique_ptr<storage::MemoryPageDevice> memory_;
    std::unique_ptr<SlowDevice> slow_;
    std::unique_ptr<storage::DevicePageStore> store_;
    std::unique_ptr<wal::MemoryLogDevice> log_device_;
    std::unique_ptr<wal::WalManager> wal_;
    std::unique_ptr<storage::PageStoreCheckpointTarget> target_;
    wal::NoActiveTransactions txns_;
    wal::InMemoryCheckpointAnchor anchor_;
};

TEST_F(CheckpointCensusTest, ARunOfNStepsReportsNStepsAndItsPages) {
    DirtyPages(10);
    wal::Checkpointer checkpointer(*wal_, *target_, txns_, anchor_,
                                   wal::CheckpointerConfig{.pages_per_step = 8});
    const std::uint64_t edges_before = CheckpointCensus::Of(kCore).run_edges.load();
    ASSERT_TRUE(checkpointer.RunGated(nullptr, kCore).ok());

    const CheckpointCensus::Slot& slot = CheckpointCensus::Of(kCore);
    EXPECT_EQ(slot.run_edges.load(), edges_before + 2) << "one run: an entry edge and an end edge";
    EXPECT_TRUE(slot.last_completed.load());
    // 20 dirty pages, 8 a step: three steps, the last one finishing.
    EXPECT_EQ(slot.last_dirty_at_start.load(), 20u);
    EXPECT_EQ(slot.last_steps.load(), 3u);
    EXPECT_EQ(slot.last_pages.load(), 20u);
}

TEST_F(CheckpointCensusTest, AScriptedDeviceDelayLandsInItsOwnPhase) {
    DirtyPages(3);
    slow_->write_ns = 7'000;
    slow_->sync_ns = 1'000'000;
    slow_->writes = 0;
    wal::Checkpointer checkpointer(*wal_, *target_, txns_, anchor_);
    ASSERT_TRUE(checkpointer.RunGated(nullptr, kCore).ok());

    // Every device write the run made was a step's page or map write - the
    // anchor here publishes no store - and each phase holds its own.
    const std::uint64_t writes = slow_->writes;
    EXPECT_GT(PhaseSum(kCore, CheckpointPhase::kPageWrite), 0u);
    EXPECT_EQ(PhaseSum(kCore, CheckpointPhase::kPageWrite) +
                  PhaseSum(kCore, CheckpointPhase::kMapWrite),
              7'000 * writes);
    // One step, one data-file sync; the scripted sync's time is that phase's
    // alone, and the write phase did not absorb it.
    EXPECT_EQ(slow_->syncs, 1u);
    EXPECT_EQ(PhaseSum(kCore, CheckpointPhase::kDataSync), 1'000'000u);
    EXPECT_EQ(PhaseSum(kCore, CheckpointPhase::kPublishSync), 0u);
    // One step: its page writes are the run's, and its hold is all of it
    // but the end record's wait, which this clock does not move.
    EXPECT_EQ(CheckpointCensus::Of(kCore).last_step_page_write_max_ns.load(),
              PhaseSum(kCore, CheckpointPhase::kPageWrite));
    EXPECT_EQ(CheckpointCensus::Of(kCore).last_step_max_ns.load(), 7'000 * writes + 1'000'000);
    EXPECT_EQ(CheckpointCensus::Of(kCore).last_wall_ns.load(), 7'000 * writes + 1'000'000) << "the clock moved only in the device";
}

// ---- `sys.checkpoints`' row (BO-S7) ----------------------------------------

TEST_F(CheckpointCensusTest, EveryRunIsOneRowOfTheCheckpointLog) {
    stats::CheckpointLog::ResetForTest();
    DirtyPages(3);
    slow_->write_ns = 7'000;
    slow_->sync_ns = 1'000'000;
    wal::Checkpointer checkpointer(*wal_, *target_, txns_, anchor_);
    ASSERT_TRUE(checkpointer.RunGated(nullptr, kCore).ok());

    const std::vector<stats::CheckpointRecord> rows = stats::CheckpointLog::Snapshot();
    ASSERT_EQ(rows.size(), 1u);
    const stats::CheckpointRecord& r = rows[0];
    const CheckpointCensus::Slot& slot = CheckpointCensus::Of(kCore);
    EXPECT_EQ(r.seq, 1u);
    EXPECT_EQ(r.core, kCore);
    EXPECT_TRUE(r.completed);
    EXPECT_FALSE(r.resumed);
    EXPECT_EQ(r.steps, slot.last_steps.load());
    EXPECT_EQ(r.pages_flushed, slot.last_pages.load());
    EXPECT_EQ(r.dirty_at_start, 6u);
    EXPECT_EQ(r.duration_us, slot.last_wall_ns.load() / 1000);
    // The step's data-file sync, inline in a synchronous run, is its wait on
    // the data file; this clock does not move for the log.
    EXPECT_EQ(r.data_wait_us, 1'000u);
    EXPECT_EQ(r.log_wait_us, 0u);
    EXPECT_GT(r.started_us, 0) << "a wall instant, not the injected clock's";
    EXPECT_EQ(stats::CheckpointLog::Read().recorded, 1u);
}

TEST_F(CheckpointCensusTest, NoSinkOutlivesTheRun) {
    // A writeback after the run - SYNC, eviction - must find no sink on the
    // thread, or it would be charged to a run that has ended.
    DirtyPages(2);
    wal::Checkpointer checkpointer(*wal_, *target_, txns_, anchor_);
    ASSERT_TRUE(checkpointer.RunGated(nullptr, kCore).ok());
    EXPECT_EQ(detail::g_checkpoint_sink, nullptr);
}

// An anchor whose publish syncs the store, as the superblock's does.
class SyncingAnchor final : public wal::CheckpointAnchor {
public:
    explicit SyncingAnchor(storage::DevicePageStore& store) : store_(store) {}
    Status Publish(const wal::CheckpointAnchorRecord&) override { return store_.Sync(); }

private:
    storage::DevicePageStore& store_;
};

TEST_F(CheckpointCensusTest, ThePublishsWritebackIsOnePhaseAndItsInnerTimersRecordNothing) {
    stats::CheckpointLog::ResetForTest();
    DirtyPages(2);
    slow_->write_ns = 3'000;
    slow_->sync_ns = 50'000;
    SyncingAnchor anchor(*store_);
    wal::Checkpointer checkpointer(*wal_, *target_, txns_, anchor,
                                   wal::CheckpointerConfig{.pages_per_step = 2});
    // A page the snapshot does not name, dirtied before the run and written
    // by the publish's whole-store flush alone: Start snapshots every dirty
    // page, so it is made dirty from inside the first step instead.
    ASSERT_TRUE(checkpointer.Start().ok());  // outside a run: records nothing
    auto late = store_->CreateNew();
    ASSERT_TRUE(late.ok());
    late.value().second = {};
    const std::uint64_t step_writes_before = slow_->writes;
    ASSERT_TRUE(checkpointer.RunGated(nullptr, kCore).ok());  // resumes the started run

    EXPECT_TRUE(CheckpointCensus::Of(kCore).last_completed.load());
    // A resumed run is its own row, its snapshot the started run's (BO-S7);
    // the `Start` outside a run is none.
    const std::vector<stats::CheckpointRecord> rows = stats::CheckpointLog::Snapshot();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].resumed);
    EXPECT_EQ(rows[0].dirty_at_start, 0u);
    EXPECT_GT(PhaseSum(kCore, CheckpointPhase::kPublishWriteback), 0u)
        << "the late page went out in the publish";
    EXPECT_EQ(PhaseSum(kCore, CheckpointPhase::kPublishSync), 50'000u);
    // Every device write is in exactly one of the three write phases.
    EXPECT_EQ(PhaseSum(kCore, CheckpointPhase::kPageWrite) +
                  PhaseSum(kCore, CheckpointPhase::kMapWrite) +
                  PhaseSum(kCore, CheckpointPhase::kPublishWriteback),
              3'000 * (slow_->writes - step_writes_before));
}

// ---- The reactor's side ---------------------------------------------------


TEST(CheckpointCensusReactorTest, ASlowTimerCallbackIsCountedOutsideARun) {
    sched::ManualClock clock(1'000);
    sched::NullIoBackend io;
    sched::Scheduler scheduler(clock, io);
    scheduler.SubmitAt(clock.Now(), [&clock] { clock.Advance(150'000'000); });  // 150 ms
    scheduler.RunOnce();
    const auto& out = scheduler.holds_outside_run();
    EXPECT_EQ(out.longest_ns, 150'000'000u);
    EXPECT_EQ(out.over_10ms, 1u);
    EXPECT_EQ(out.over_100ms, 1u);
    EXPECT_EQ(out.over_1s, 0u);
    EXPECT_EQ(scheduler.holds_inside_run().over_10ms, 0u);
}

TEST(CheckpointCensusReactorTest, AnIterationARunOverlapsIsCountedInside) {
    sched::ManualClock clock(1'000);
    sched::NullIoBackend io;
    sched::Scheduler scheduler(clock, io);  // core 0's slot: never attached to a table
    const std::uint32_t core = scheduler.core_id();
    scheduler.SubmitAt(clock.Now(), [&clock, core] {
        CheckpointCensus::RunEdge(core);  // a run begins and ends inside the callback
        clock.Advance(20'000'000);
        CheckpointCensus::RunEdge(core);
    });
    scheduler.RunOnce();
    EXPECT_EQ(scheduler.holds_inside_run().over_10ms, 1u);
    EXPECT_EQ(scheduler.holds_outside_run().over_10ms, 0u);
}

}  // namespace
}  // namespace kds
