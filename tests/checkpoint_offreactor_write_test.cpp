// DZ-S2 (`instructions/v3.2.0/workorder-dz-checkpoint-reactor-residue.md`):
// a cadence checkpoint run's page writes leave its reactor. The step's and
// the publish's batches are staged - gate, claims and copies on the reactor -
// written by the data-sync writer thread, and cleaned where DZ-S2 says (that
// thread armed, the store's one thread unarmed). These cells pin:
//
//   - posted jobs run in order on a writer thread beside the sync's, a sync
//     is served while a job is held, and `Stop` runs what is queued;
//     without the threads a job is inline;
//   - a staged write is not done while its device write is held, its pages
//     stay claimed and dirty, and once written they are clean and on the
//     device - except one written again after its copy (DZ-R2);
//   - **a blocking flush on the store's one thread meets a staged claim and
//     does not wait on itself** (unarmed): it cleans for the parked run;
//   - a failed staged write fails the batch and leaves its pages dirty, and
//     the next writeback writes them (DZ-R4: only a sync is sticky);
//   - a run task parks while its step's write is held, a foreground task
//     runs meanwhile, and the run publishes once it is released;
//   - the map halves park on a busy map write barrier (DZ-Q6 (a)).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/storage/data_syncer.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/page_store_checkpoint_target.hpp"
#include "kds/wal/checkpointer.hpp"
#include "kds/wal/memory_log_device.hpp"

namespace kds::storage {
namespace {

// A page device whose writes can be held or failed.
class HeldWriteDevice final : public PageDevice {
public:
    explicit HeldWriteDevice(PageDevice& inner) : inner_(inner) {}
    std::uint32_t page_capacity() const noexcept override { return inner_.page_capacity(); }
    Status ReadPage(PageId id, std::span<std::byte, kPageSize> out) override {
        return inner_.ReadPage(id, out);
    }
    Status WritePage(PageId id, std::span<const std::byte, kPageSize> in) override {
        if (Status s = Gate(); !s.ok()) return s;
        return inner_.WritePage(id, in);
    }
    Status WritePageRun(PageId first, std::uint32_t n, std::span<const std::byte> in) override {
        if (Status s = Gate(); !s.ok()) return s;
        return inner_.WritePageRun(first, n, in);
    }
    Status EnsureCapacity(std::uint32_t n) override { return inner_.EnsureCapacity(n); }
    Status Sync() override { return inner_.Sync(); }

    void Hold() {
        std::lock_guard lock(mu_);
        held_ = true;
    }
    void Release(bool fail = false) {
        std::lock_guard lock(mu_);
        held_ = false;
        fail_ = fail;
        cv_.notify_all();
    }
    std::uint64_t writes_entered() {
        std::lock_guard lock(mu_);
        return entered_;
    }

private:
    Status Gate() {
        std::unique_lock lock(mu_);
        ++entered_;
        cv_.wait(lock, [this] { return !held_; });
        if (fail_) return Status::IoError("injected: page write failed");
        return Status::OK();
    }
    PageDevice& inner_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool held_ = false;
    bool fail_ = false;
    std::uint64_t entered_ = 0;
};

bool WaitFor(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return true;
}

TEST(DataSyncerJobTest, JobsRunInOrderOffTheCallerAndStopRunsWhatIsQueued) {
    auto mem = MemoryPageDevice::Create(/*extent_pages=*/8, 0);
    ASSERT_TRUE(mem.ok());
    DataSyncer syncer(*mem.value());
    syncer.Start();
    std::mutex mu;
    std::vector<int> ran;
    std::atomic<bool> release{false};
    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> off_caller{true};
    syncer.Post([&] {
        while (!release.load()) std::this_thread::yield();
        if (std::this_thread::get_id() == caller) off_caller = false;
        std::lock_guard lock(mu);
        ran.push_back(1);
    });
    syncer.Post([&] {
        std::lock_guard lock(mu);
        ran.push_back(2);
    });
    release = true;
    syncer.Stop();  // the second job is still queued, or just ran: it runs either way
    EXPECT_EQ(ran, (std::vector<int>{1, 2}));
    EXPECT_TRUE(off_caller.load()) << "a posted job ran on the poster's thread with the thread up";
    int inline_ran = 0;
    syncer.Post([&] { ++inline_ran; });
    EXPECT_EQ(inline_ran, 1) << "with the thread stopped a job is the caller's";
}

TEST(DataSyncerJobTest, ASyncIsServedWhileAJobIsHeld) {
    // DZ-Q2 (b): the jobs' thread is not the sync's, so a write the kernel
    // throttles does not hold a sync behind it, nor a sync a write.
    auto mem = MemoryPageDevice::Create(/*extent_pages=*/8, 0);
    ASSERT_TRUE(mem.ok());
    DataSyncer syncer(*mem.value());
    syncer.Start();
    std::atomic<bool> in_job{false};
    std::atomic<bool> release{false};
    syncer.Post([&] {
        in_job = true;
        while (!release.load()) std::this_thread::yield();
    });
    ASSERT_TRUE(WaitFor([&] { return in_job.load(); }));
    const std::uint64_t ticket = syncer.Request();
    EXPECT_TRUE(WaitFor([&] { return syncer.Done(ticket); })) << "a sync waited behind a held job";
    EXPECT_TRUE(syncer.Result(ticket).ok());
    release = true;
    syncer.Stop();
}

class StagedWriteTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        auto mem = MemoryPageDevice::Create(/*extent_pages=*/64, 0);
        ASSERT_TRUE(mem.ok());
        memory_ = std::move(mem.value());
        held_ = std::make_unique<HeldWriteDevice>(*memory_);
        auto store = DevicePageStore::Open(*held_, FrameCapacity{4096}, /*first_new_page_id=*/16);
        ASSERT_TRUE(store.ok()) << store.status().message();
        store_ = std::move(store.value());
        store_->SetLatchArmed(GetParam());
        for (int i = 0; i < 20; ++i) {
            auto made = store_->CreateNew();
            ASSERT_TRUE(made.ok());
            FormatPage(made.value().second.bytes(), PageType::kHeap);
            made.value().second.bytes()[kPageBodyOffset] = std::byte{0x5A};
            ids_.push_back(made.value().first);
        }
        store_->data_sync().Start();
    }
    void TearDown() override {
        held_->Release();
        store_->data_sync().Stop();
    }
    std::byte OnDevice(PageId id) {
        std::array<std::byte, kPageSize> page{};
        EXPECT_TRUE(memory_->ReadPage(id, page).ok());
        return page[kPageBodyOffset];
    }

    std::unique_ptr<MemoryPageDevice> memory_;
    std::unique_ptr<HeldWriteDevice> held_;
    std::unique_ptr<DevicePageStore> store_;
    std::vector<PageId> ids_;
};

TEST_P(StagedWriteTest, AStagedWriteIsDoneOnlyOnceWrittenAndCleansOnlyWhatItsCopyCovered) {
    held_->Hold();
    auto staged = store_->StageWriteBack(ids_, nullptr);
    ASSERT_TRUE(staged.ok()) << staged.status().message();
    ASSERT_TRUE(WaitFor([&] { return held_->writes_entered() > 0; }));
    EXPECT_FALSE(staged.value()->Done()) << "done with its device write held";
    EXPECT_EQ(store_->DirtyPageIds().size(), ids_.size()) << "a page went clean before its write";
    // Written again after its copy: its bytes did not go out (DZ-R2).
    ASSERT_TRUE(store_->StampPageLsn(ids_[3], 4096).ok());
    held_->Release();
    ASSERT_TRUE(WaitFor([&] { return staged.value()->Done(); }));
    auto finished = store_->FinishStaged(*staged.value());
    ASSERT_TRUE(finished.ok()) << finished.status().message();
    EXPECT_EQ(store_->DirtyPageIds(), std::vector<PageId>{ids_[3]});
    EXPECT_EQ(OnDevice(ids_[0]), std::byte{0x5A}) << "the staged bytes are not on the device";
}

TEST_P(StagedWriteTest, ABlockingFlushOfAStagedPageDoesNotWaitOnTheRunThatStagedIt) {
    held_->Hold();
    auto staged = store_->StageWriteBack(ids_, nullptr);
    ASSERT_TRUE(staged.ok());
    ASSERT_TRUE(WaitFor([&] { return held_->writes_entered() > 0; }));
    std::thread releaser([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        held_->Release();
    });
    // On the store's own thread, as a SYNC or a carve on the checkpointing
    // core: the claim is the parked run's, which this thread would run.
    const Status flushed = store_->FlushPages(ids_);
    releaser.join();
    EXPECT_TRUE(flushed.ok()) << flushed.message();
    EXPECT_TRUE(store_->DirtyPageIds().empty());
    ASSERT_TRUE(staged.value()->Done());
    EXPECT_TRUE(store_->FinishStaged(*staged.value()).ok());
}

TEST_P(StagedWriteTest, AFailedStagedWriteLeavesItsPagesDirtyForTheNextWriteback) {
    held_->Hold();
    auto staged = store_->StageWriteBack(ids_, nullptr);
    ASSERT_TRUE(staged.ok());
    ASSERT_TRUE(WaitFor([&] { return held_->writes_entered() > 0; }));
    held_->Release(/*fail=*/true);
    ASSERT_TRUE(WaitFor([&] { return staged.value()->Done(); }));
    EXPECT_FALSE(store_->FinishStaged(*staged.value()).ok());
    EXPECT_EQ(store_->DirtyPageIds().size(), ids_.size());
    held_->Release(/*fail=*/false);
    auto again = store_->WriteBackPages(ids_, nullptr);
    ASSERT_TRUE(again.ok()) << again.status().message();
    EXPECT_EQ(again.value(), ids_.size()) << "the claims were not given back";
    EXPECT_TRUE(store_->DirtyPageIds().empty());
}

INSTANTIATE_TEST_SUITE_P(ArmedAndNot, StagedWriteTest, ::testing::Bool(),
                         [](const auto& info) { return info.param ? "Armed" : "Unarmed"; });

class OffReactorRunTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto mem = MemoryPageDevice::Create(/*extent_pages=*/64, 0);
        ASSERT_TRUE(mem.ok());
        memory_ = std::move(mem.value());
        held_ = std::make_unique<HeldWriteDevice>(*memory_);
        auto store = DevicePageStore::Open(*held_, FrameCapacity{4096}, /*first_new_page_id=*/16);
        ASSERT_TRUE(store.ok());
        store_ = std::move(store.value());
        store_->SetLatchArmed(true);
        for (int i = 0; i < 40; ++i) {
            auto made = store_->CreateNew();
            ASSERT_TRUE(made.ok());
            FormatPage(made.value().second.bytes(), PageType::kHeap);
        }
        store_->data_sync().Start();
        auto log = wal::MemoryLogDevice::Create(1024 * 1024);
        ASSERT_TRUE(log.ok());
        log_ = std::move(log.value());
        auto wal = wal::WalManager::Open(log_.get(), clock_, 0);
        ASSERT_TRUE(wal.ok());
        wal_ = std::move(wal.value());
        target_ = std::make_unique<PageStoreCheckpointTarget>(*store_);
        reactor_.Submit(sched::MakeCoroTask(sched::SchedulingGroup::kForeground, Count()));
    }
    void TearDown() override {
        stop_ = true;
        reactor_.RunOnce();
        held_->Release();
        store_->data_sync().Stop();
    }
    sched::Coro Count() {
        while (!stop_) {
            ++polls_;
            co_await sched::Yield{};
        }
        co_return Status::OK();
    }
    bool TurnUntil(const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            reactor_.RunOnce();
        }
        return true;
    }

    const CurrentCoreGuard as_core_0_{0};
    sched::SystemClock clock_;
    sched::NullIoBackend io_;
    sched::Scheduler reactor_{clock_, io_};
    std::unique_ptr<MemoryPageDevice> memory_;
    std::unique_ptr<HeldWriteDevice> held_;
    std::unique_ptr<DevicePageStore> store_;
    std::unique_ptr<wal::MemoryLogDevice> log_;
    std::unique_ptr<wal::WalManager> wal_;
    std::unique_ptr<PageStoreCheckpointTarget> target_;
    wal::NoActiveTransactions txns_;
    wal::InMemoryCheckpointAnchor anchor_;
    std::uint64_t polls_ = 0;
    bool stop_ = false;
};

TEST_F(OffReactorRunTest, ARunParksOnItsHeldStepWriteWhileTheReactorRunsOthersAndThenPublishes) {
    wal::Checkpointer c(*wal_, *target_, txns_, anchor_, wal::CheckpointerConfig{.pages_per_step = 8});
    held_->Hold();
    c.SubmitRun(reactor_, nullptr, 0);
    ASSERT_TRUE(TurnUntil([&] { return held_->writes_entered() > 0; }));
    const std::uint64_t at_hold = polls_;
    for (int i = 0; i < 50; ++i) reactor_.RunOnce();
    EXPECT_GE(polls_, at_hold + 50) << "the reactor waited on the run's write";
    EXPECT_TRUE(c.run_in_flight());
    EXPECT_EQ(anchor_.publishes(), 0u);
    held_->Release();
    ASSERT_TRUE(TurnUntil([&] { return !c.run_in_flight(); }));
    EXPECT_EQ(anchor_.publishes(), 1u);
    EXPECT_TRUE(store_->DirtyPageIds().empty());
    EXPECT_GT(c.task_report().write_park_ns, 0u);
}

TEST_F(OffReactorRunTest, TheMapHalvesParkOnABusyMapBarrierAndThenPublish) {
    wal::Checkpointer c(*wal_, *target_, txns_, anchor_, wal::CheckpointerConfig{.pages_per_step = 8});
    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
    std::thread flusher([&] {
        store_->HoldMapFlushBarrierForTest([&] {
            held = true;
            while (!release.load()) std::this_thread::yield();
        });
    });
    while (!held.load()) std::this_thread::yield();
    c.SubmitRun(reactor_, nullptr, 0);
    const bool met = TurnUntil([&] { return c.task_report().map_busy > 0; });
    release = true;
    flusher.join();
    ASSERT_TRUE(met) << "the run never met the held barrier";
    ASSERT_TRUE(TurnUntil([&] { return !c.run_in_flight(); }));
    EXPECT_EQ(anchor_.publishes(), 1u);
}

}  // namespace
}  // namespace kds::storage
