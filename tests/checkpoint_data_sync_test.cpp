#include "kds/storage/data_syncer.hpp"

#include <algorithm>
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

#include "kds/sched/checkpoint_census.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/page_store_checkpoint_target.hpp"
#include "kds/wal/checkpointer.hpp"
#include "kds/wal/memory_log_device.hpp"

// BI-S4 (BI-R2's second half, BI-Q2 (a), BI-Q3 (a), BI-Q5 (a)): a checkpoint
// task parks on the data file's sync, which the instance's one thread runs;
// a failed sync is sticky and publishes nothing; and the paced publish puts
// the anchor's page on the device only after a sync of every other page.

namespace kds::storage {
namespace {

constexpr std::uint64_t kSegmentSize = 1024 * 1024;

// A page device whose `Sync` can be held or failed, and which records the
// order of its writes and syncs.
class HeldPageDevice final : public PageDevice {
public:
    explicit HeldPageDevice(PageDevice& inner) : inner_(inner) {}
    std::uint32_t page_capacity() const noexcept override { return inner_.page_capacity(); }
    Status ReadPage(PageId id, std::span<std::byte, kPageSize> out) override {
        return inner_.ReadPage(id, out);
    }
    Status WritePage(PageId id, std::span<const std::byte, kPageSize> in) override {
        Note('w', id);
        return inner_.WritePage(id, in);
    }
    Status WritePageRun(PageId first, std::uint32_t n, std::span<const std::byte> in) override {
        for (std::uint32_t k = 0; k < n; ++k) Note('w', first + k);
        return inner_.WritePageRun(first, n, in);
    }
    Status EnsureCapacity(std::uint32_t n) override { return inner_.EnsureCapacity(n); }
    Status Sync() override {
        std::unique_lock lock(mu_);
        ++syncs_entered_;
        cv_.notify_all();
        cv_.wait(lock, [this] { return !held_; });
        trace_.push_back({'s', 0});
        if (fail_) return Status::IoError("injected: data sync failed");
        lock.unlock();
        return inner_.Sync();
    }

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
    std::uint64_t syncs_entered() {
        std::lock_guard lock(mu_);
        return syncs_entered_;
    }
    std::vector<std::pair<char, PageId>> trace() {
        std::lock_guard lock(mu_);
        return trace_;
    }

private:
    void Note(char kind, PageId id) {
        std::lock_guard lock(mu_);
        trace_.push_back({kind, id});
    }
    PageDevice& inner_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool held_ = false;
    bool fail_ = false;
    std::uint64_t syncs_entered_ = 0;
    std::vector<std::pair<char, PageId>> trace_;
};

// An anchor whose page is one of the store's own, as page 0 is the
// superblock anchor's: `Encode` dirties it, `Commit` counts.
class PageAnchor final : public wal::CheckpointAnchor {
public:
    PageAnchor(DevicePageStore& store, PageId page) : store_(store), page_(page) {}
    Status Publish(const wal::CheckpointAnchorRecord& a) override {
        if (auto e = Encode(a); !e.ok()) return e.status();
        if (Status s = store_.Sync(); !s.ok()) return s;
        Commit();
        return Status::OK();
    }
    StatusOr<PageId> Encode(const wal::CheckpointAnchorRecord&) override {
        auto page = store_.Get(page_);
        if (!page.ok()) return page.status();
        page.value().bytes()[kPageBodyOffset] = std::byte{0x5A};
        return page_;
    }
    void Commit() override { ++commits; }
    std::uint64_t commits = 0;

private:
    DevicePageStore& store_;
    PageId page_;
};

class CheckpointDataSyncTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto mem = MemoryPageDevice::Create(/*extent_pages=*/512, /*initial_pages=*/0);
        ASSERT_TRUE(mem.ok());
        memory_ = std::move(mem.value());
        held_ = std::make_unique<HeldPageDevice>(*memory_);
        auto store = DevicePageStore::Open(*held_, FrameCapacity{4096}, /*first_new_page_id=*/16);
        ASSERT_TRUE(store.ok()) << store.status().message();
        store_ = std::move(store.value());
        auto log = wal::MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(log.ok());
        log_ = std::move(log.value());
        auto wal = wal::WalManager::Open(log_.get(), clock_, 0);
        ASSERT_TRUE(wal.ok());
        wal_ = std::move(wal.value());
        target_ = std::make_unique<PageStoreCheckpointTarget>(*store_);
        for (int i = 0; i < 10; ++i) {
            auto made = store_->CreateNew();
            ASSERT_TRUE(made.ok());
            FormatPage(made.value().second.bytes(), PageType::kHeap);
            pages_.push_back(made.value().first);
        }
    }

    void TearDown() override {
        held_->Release();
        store_->data_sync().Stop();
    }

    static bool TurnUntil(sched::Scheduler& reactor, const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            reactor.RunOnce();
        }
        return true;
    }

    sched::ManualClock clock_{1'000};
    sched::NullIoBackend io_;
    sched::Scheduler reactor_{clock_, io_};
    std::unique_ptr<MemoryPageDevice> memory_;
    std::unique_ptr<HeldPageDevice> held_;
    std::unique_ptr<DevicePageStore> store_;
    std::unique_ptr<wal::MemoryLogDevice> log_;
    std::unique_ptr<wal::WalManager> wal_;
    std::unique_ptr<PageStoreCheckpointTarget> target_;
    wal::NoActiveTransactions txns_;
    std::vector<PageId> pages_;
};

TEST_F(CheckpointDataSyncTest, TheRunParksWhileItsDataSyncIsHeld) {
    store_->data_sync().Start();
    wal::InMemoryCheckpointAnchor anchor;
    wal::Checkpointer c(*wal_, *target_, txns_, anchor, wal::CheckpointerConfig{.pages_per_step = 4});
    held_->Hold();
    const std::uint64_t entered = held_->syncs_entered();
    c.SubmitRun(reactor_, nullptr, 0);

    std::uint64_t polls = 0;
    bool stop = false;
    auto counter = [](std::uint64_t* n, bool* halt) -> sched::Coro {
        while (!*halt) {
            ++*n;
            co_await sched::Yield{};
        }
        co_return Status::OK();
    };
    reactor_.Submit(sched::MakeCoroTask(sched::SchedulingGroup::kForeground, counter(&polls, &stop)));
    ASSERT_TRUE(TurnUntil(reactor_, [&] { return held_->syncs_entered() > entered; }));
    const std::uint64_t polls_at_hold = polls;
    const auto writes = [this] {
        std::size_t n = 0;
        for (const auto& [kind, id] : held_->trace()) n += kind == 'w' ? 1 : 0;
        return n;
    };
    for (int i = 0; i < 50; ++i) reactor_.RunOnce();
    // BI-Q13: no step waits for the data file - every snapshot page is
    // written while the first sync is still held - and the run then parks on
    // the publish's sync and writes nothing past it.
    const auto trace = held_->trace();
    for (PageId id : pages_) {
        EXPECT_TRUE(std::any_of(trace.begin(), trace.end(),
                                [id](const auto& e) { return e.first == 'w' && e.second == id; }))
            << "page " << id << " waited for a held sync before it was written";
    }
    const std::size_t writes_after_steps = writes();
    for (int i = 0; i < 50; ++i) reactor_.RunOnce();
    EXPECT_EQ(writes(), writes_after_steps) << "the run wrote on past a sync that had not returned";
    EXPECT_GE(polls - polls_at_hold, 50u) << "the reactor waited for the data file's sync";
    EXPECT_TRUE(c.run_in_flight());
    EXPECT_EQ(anchor.publishes(), 0u) << "the run went on past a sync that had not returned";

    held_->Release();
    ASSERT_TRUE(TurnUntil(reactor_, [&] { return !c.run_in_flight(); }));
    stop = true;
    reactor_.RunOnce();
    EXPECT_EQ(anchor.publishes(), 1u);
    EXPECT_EQ(CheckpointCensus::Of(0).last_count[static_cast<std::size_t>(CheckpointPhase::kDataSync)].load(),
              0u)
        << "a step synced the data file on the reactor";
}

TEST_F(CheckpointDataSyncTest, AFailedDataSyncFailsTheRunPublishesNothingAndSticks) {
    store_->data_sync().Start();
    Status handed;
    int handler_calls = 0;
    store_->data_sync().SetFailureHandler([&](const Status& s) {
        handed = s;
        ++handler_calls;
    });
    wal::InMemoryCheckpointAnchor anchor;
    wal::Checkpointer c(*wal_, *target_, txns_, anchor, wal::CheckpointerConfig{.pages_per_step = 4});
    held_->Hold();
    const std::uint64_t entered = held_->syncs_entered();
    c.SubmitRun(reactor_, nullptr, 0);
    ASSERT_TRUE(TurnUntil(reactor_, [&] { return held_->syncs_entered() > entered; }));
    held_->Release(/*fail=*/true);
    ASSERT_TRUE(TurnUntil(reactor_, [&] { return !c.run_in_flight(); }));

    EXPECT_EQ(anchor.publishes(), 0u);
    EXPECT_FALSE(CheckpointCensus::Of(0).last_completed.load());
    EXPECT_EQ(handler_calls, 1);
    EXPECT_FALSE(handed.ok());
    // Sticky (BI-Q3 (a)): the device is healthy again and still nothing
    // reports OK, and nothing more is written back.
    held_->Release(/*fail=*/false);
    EXPECT_FALSE(store_->data_sync().SyncNow().ok()) << "a healthy device reported the lost sync OK";
    EXPECT_FALSE(store_->Sync().ok());
    auto dirty = store_->CreateNew();
    ASSERT_TRUE(dirty.ok());
    const PageId again[] = {dirty.value().first};
    dirty.value().second = {};
    EXPECT_FALSE(store_->WriteBackPages(again, nullptr).ok());
    EXPECT_EQ(handler_calls, 1) << "the handler is the first failure's alone";
}

TEST_F(CheckpointDataSyncTest, TheAnchorsPageReachesTheDeviceAfterASyncOfEveryOtherPage) {
    store_->data_sync().Start();
    // The anchor's page is one of the store's, clean before the run.
    auto made = store_->CreateNew();
    ASSERT_TRUE(made.ok());
    FormatPage(made.value().second.bytes(), PageType::kHeap);
    const PageId anchor_page = made.value().first;
    made.value().second = {};
    const PageId only[] = {anchor_page};
    ASSERT_TRUE(store_->FlushPages(only).ok());

    PageAnchor anchor(*store_, anchor_page);
    wal::Checkpointer c(*wal_, *target_, txns_, anchor, wal::CheckpointerConfig{.pages_per_step = 4});
    const std::size_t trace_from = held_->trace().size();
    c.SubmitRun(reactor_, nullptr, 0);
    ASSERT_TRUE(TurnUntil(reactor_, [&] { return !c.run_in_flight(); }));
    ASSERT_EQ(anchor.commits, 1u);

    const auto trace = held_->trace();
    std::size_t anchor_write = trace.size();
    std::size_t last_other_write = 0;
    for (std::size_t i = trace_from; i < trace.size(); ++i) {
        if (trace[i].first != 'w') continue;
        if (trace[i].second == anchor_page) {
            anchor_write = i;
        } else {
            last_other_write = i;
        }
    }
    ASSERT_LT(anchor_write, trace.size()) << "the anchor's page was never written";
    EXPECT_LT(last_other_write, anchor_write);
    bool synced_between = false;
    for (std::size_t i = last_other_write; i < anchor_write; ++i) {
        if (trace[i].first == 's') synced_between = true;
    }
    EXPECT_TRUE(synced_between) << "the anchor's page went out before every other page was synced";
    bool synced_after = false;
    for (std::size_t i = anchor_write; i < trace.size(); ++i) {
        if (trace[i].first == 's') synced_after = true;
    }
    EXPECT_TRUE(synced_after) << "the anchor was committed before its page was synced";
}

TEST_F(CheckpointDataSyncTest, ASnapshotPageAnotherWritebackCarriedIsSyncedBeforeTheAnchor) {
    // Under BI-Q5 (a) only the publish's collection sync covers a snapshot
    // page another writeback wrote before this run's step reached it: the
    // step finds it clean, writes nothing and asks for no sync.
    store_->data_sync().Start();
    auto made = store_->CreateNew();
    ASSERT_TRUE(made.ok());
    FormatPage(made.value().second.bytes(), PageType::kHeap);
    const PageId anchor_page = made.value().first;
    made.value().second = {};
    {
        const PageId only[] = {anchor_page};
        ASSERT_TRUE(store_->FlushPages(only).ok());
    }
    PageAnchor anchor(*store_, anchor_page);
    // One step of 16 covers every snapshot page; the "other core" carries
    // them all after Start, before the step.
    wal::Checkpointer c(*wal_, *target_, txns_, anchor, wal::CheckpointerConfig{.pages_per_step = 16});
    ASSERT_TRUE(store_->WriteMaps().ok());  // maps clean, so they cannot stand in for the sync
    c.SubmitRun(reactor_, nullptr, 0);
    reactor_.RunOnce();  // Start
    const std::size_t from = held_->trace().size();
    ASSERT_TRUE(store_->WriteBackPages(pages_, nullptr).ok());  // another core's writeback
    ASSERT_TRUE(TurnUntil(reactor_, [&] { return !c.run_in_flight(); }));
    ASSERT_EQ(anchor.commits, 1u);

    const auto trace = held_->trace();
    std::size_t carried = 0;
    std::size_t anchor_write = trace.size();
    for (std::size_t i = from; i < trace.size(); ++i) {
        if (trace[i].first != 'w') continue;
        if (trace[i].second == anchor_page) anchor_write = i;
        else carried = i;
    }
    ASSERT_LT(anchor_write, trace.size());
    bool synced_between = false;
    for (std::size_t i = carried; i < anchor_write; ++i) synced_between |= trace[i].first == 's';
    EXPECT_TRUE(synced_between) << "the anchor went out with a carried page unsynced";
}

TEST(DataSyncerTest, StopAnswersAWaiterInline) {
    auto mem = MemoryPageDevice::Create(/*extent_pages=*/64, /*initial_pages=*/0);
    ASSERT_TRUE(mem.ok());
    HeldPageDevice held(*mem.value());
    DataSyncer syncer(held);
    syncer.Start();
    held.Hold();
    std::thread first([&] { (void)syncer.SyncNow(); });  // the thread's sync, held
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (held.syncs_entered() == 0 && std::chrono::steady_clock::now() < deadline) {
    }
    std::thread waiter([&] { EXPECT_TRUE(syncer.SyncNow().ok()); });  // queued behind it
    std::thread stopper([&] { syncer.Stop(); });  // joins once the held sync returns
    held.Release();
    stopper.join();
    waiter.join();  // hangs here if Stop strands it
    first.join();
}

TEST(DataSyncerTest, ATicketIsNotDoneBeforeItsSyncReturns) {
    auto mem = MemoryPageDevice::Create(/*extent_pages=*/64, /*initial_pages=*/0);
    ASSERT_TRUE(mem.ok());
    HeldPageDevice held(*mem.value());
    DataSyncer syncer(held);
    syncer.Start();
    held.Hold();
    const std::uint64_t ticket = syncer.Request();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (held.syncs_entered() == 0 && std::chrono::steady_clock::now() < deadline) {
    }
    EXPECT_FALSE(syncer.Done(ticket));
    held.Release();
    while (!syncer.Done(ticket) && std::chrono::steady_clock::now() < deadline) {
    }
    EXPECT_TRUE(syncer.Done(ticket));
    EXPECT_TRUE(syncer.Result(ticket).ok());
    syncer.Stop();
}

}  // namespace
}  // namespace kds::storage
