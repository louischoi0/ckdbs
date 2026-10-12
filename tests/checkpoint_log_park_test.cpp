#include "kds/wal/checkpointer.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/sched/checkpoint_census.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/sched/waker_table.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/page_store_checkpoint_target.hpp"
#include "kds/wal/memory_log_device.hpp"

// BI-S3 (BI-R2's first half): a checkpoint step whose batch is past the
// durable point asks the writer for the sync and parks, and the reactor runs
// everything else meanwhile - on a peer through the instance's writer, and on
// core 0 through the same writer rather than an inline `fdatasync`. A page
// re-dirtied during the park is re-gated by another park, a page never
// reaches the device ahead of its record, and a fail-stopped log answers the
// park.

namespace kds::wal {
namespace {

constexpr std::uint64_t kSegmentSize = 1024 * 1024;

// A log device whose `Sync` can be held, and made to fail, by the test.
class HeldLogDevice final : public LogDevice {
public:
    explicit HeldLogDevice(std::unique_ptr<MemoryLogDevice> inner) : inner_(std::move(inner)) {}
    std::uint64_t segment_size() const noexcept override { return inner_->segment_size(); }
    std::uint64_t first_segment() const noexcept override { return inner_->first_segment(); }
    std::uint64_t end_segment() const noexcept override { return inner_->end_segment(); }
    Status CreateSegment(std::uint64_t n, std::span<const std::byte> header) override {
        return inner_->CreateSegment(n, header);
    }
    void PrepareSegment(std::uint64_t n) override { inner_->PrepareSegment(n); }
    Status WriteAt(std::uint64_t n, std::uint64_t off, std::span<const std::byte> in) override {
        return inner_->WriteAt(n, off, in);
    }
    Status ReadAt(std::uint64_t n, std::uint64_t off, std::span<std::byte> out) override {
        return inner_->ReadAt(n, off, out);
    }
    Status Sync() override {
        std::unique_lock lock(mu_);
        ++syncs_entered_;
        cv_.notify_all();
        cv_.wait(lock, [this] { return !held_; });
        if (fail_) return Status::IoError("injected: log sync failed");
        return inner_->Sync();
    }
    Status DetachBelow(std::uint64_t n) override { return inner_->DetachBelow(n); }
    Status ReclaimDetached() override { return inner_->ReclaimDetached(); }
    std::uint64_t segments_removed() const noexcept override { return inner_->segments_removed(); }

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

private:
    std::unique_ptr<MemoryLogDevice> inner_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool held_ = false;
    bool fail_ = false;
    std::uint64_t syncs_entered_ = 0;
};

// A page device that records any page written while its record was not
// durable - WAL-before-data (wal.md §8-1), checked at the device.
class WalCheckingDevice final : public storage::PageDevice {
public:
    WalCheckingDevice(storage::PageDevice& inner, const WalDurability& log)
        : inner_(inner), log_(log) {}
    std::uint32_t page_capacity() const noexcept override { return inner_.page_capacity(); }
    Status ReadPage(PageId id, std::span<std::byte, kPageSize> out) override {
        return inner_.ReadPage(id, out);
    }
    Status WritePage(PageId id, std::span<const std::byte, kPageSize> in) override {
        Check(in);
        ++writes;
        return inner_.WritePage(id, in);
    }
    Status WritePageRun(PageId first, std::uint32_t n, std::span<const std::byte> in) override {
        for (std::uint32_t k = 0; k < n; ++k) {
            Check(std::span<const std::byte, kPageSize>(in.data() + k * kPageSize, kPageSize));
        }
        ++writes;
        return inner_.WritePageRun(first, n, in);
    }
    Status EnsureCapacity(std::uint32_t n) override { return inner_.EnsureCapacity(n); }
    Status Sync() override { return inner_.Sync(); }

    std::uint64_t writes = 0;
    std::uint64_t violations = 0;

private:
    void Check(std::span<const std::byte, kPageSize> page) {
        const std::uint64_t lsn = storage::GetPageLsn(page);
        if (lsn != 0 && !log_.IsDurable(lsn)) ++violations;
    }
    storage::PageDevice& inner_;
    const WalDurability& log_;
};

std::uint64_t Count(std::uint32_t core, CheckpointPhase p) {
    return CheckpointCensus::Of(core).last_count[static_cast<std::size_t>(p)].load();
}

class CheckpointLogParkTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto mem = MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(mem.ok());
        log_device_ = std::make_unique<HeldLogDevice>(std::move(mem.value()));
        WalManagerConfig config;
        config.shared_stream = true;
        auto owner = WalManager::Open(log_device_.get(), clock_, 0, config);
        ASSERT_TRUE(owner.ok()) << owner.status().message();
        owner_ = std::move(owner.value());
        owner_->StartWriter();
        auto peer = WalManager::Attach(owner_->stream(), owner_->writer(), clock_, 1);
        ASSERT_TRUE(peer.ok()) << peer.status().message();
        peer_ = std::move(peer.value());

        auto pages = storage::MemoryPageDevice::Create(/*extent_pages=*/512, /*initial_pages=*/0);
        ASSERT_TRUE(pages.ok());
        page_device_ = std::move(pages.value());
        checking_ = std::make_unique<WalCheckingDevice>(*page_device_, *owner_);
        auto store = storage::DevicePageStore::Open(*checking_, storage::FrameCapacity{4096},
                                                    /*first_new_page_id=*/16);
        ASSERT_TRUE(store.ok()) << store.status().message();
        store_ = std::move(store.value());
        ASSERT_TRUE(store_->SetCoreWalGate(0, owner_.get()).ok());
        ASSERT_TRUE(store_->SetCoreWalGate(1, peer_.get()).ok());
        target_ = std::make_unique<storage::PageStoreCheckpointTarget>(*store_);
    }

    void TearDown() override {
        log_device_->Release();
        store_->ClearCoreWalGate(0, owner_.get());
        store_->ClearCoreWalGate(1, peer_.get());
    }

    // `n` pages, each changed by a record `wal` appended and has not synced.
    std::vector<PageId> LoggedPages(WalManager& wal, int n) {
        std::vector<PageId> ids;
        for (int i = 0; i < n; ++i) {
            auto made = store_->CreateNew();
            EXPECT_TRUE(made.ok());
            storage::FormatPage(made.value().second.bytes(), PageType::kHeap);
            ids.push_back(made.value().first);
        }
        for (PageId id : ids) Stamp(wal, id);
        return ids;
    }

    void Stamp(WalManager& wal, PageId id) {
        auto lsn = wal.Append({RecordType::kCheckpointEnd, kNoTxnId, id, 0},
                              std::vector<std::byte>(kCheckpointEndPayloadSize));
        ASSERT_TRUE(lsn.ok()) << lsn.status().message();
        ASSERT_TRUE(store_->StampPageLsn(id, lsn.value()).ok());
    }

    // Turns `scheduler` until `done`, for at most 10 s of wall time: the
    // writer is a real thread.
    static bool TurnUntil(sched::Scheduler& scheduler, const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            scheduler.RunOnce();
        }
        return true;
    }

    std::unique_ptr<sched::Scheduler> ReactorAs(std::uint32_t core) {
        auto s = std::make_unique<sched::Scheduler>(clock_, io_);
        EXPECT_TRUE(s->AttachWakerTable(&wakers_, core).ok());
        return s;
    }

    // One cell for both cores: the step parks with the sync held, the reactor
    // keeps polling a foreground task, and nothing is written until the log is.
    void ParksWhileTheReactorRuns(sched::Scheduler& reactor, WalManager& wal, std::uint32_t core) {
        HeldLogDevice& log = *log_device_;
        WalCheckingDevice& device = *checking_;
        InMemoryCheckpointAnchor& anchor = anchor_;
        Checkpointer c(wal, *target_, txns_, anchor, CheckpointerConfig{.pages_per_step = 4});
        log.Hold();
        const std::uint64_t writes_before = device.writes;
        const std::uint64_t syncs_before = log.syncs_entered();
        c.SubmitRun(reactor, nullptr, core);

        std::uint64_t foreground_polls = 0;
        bool stop = false;
        auto counter = [](std::uint64_t* n, bool* halt) -> sched::Coro {
            while (!*halt) {
                ++*n;
                co_await sched::Yield{};
            }
            co_return Status::OK();
        };
        reactor.Submit(sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                                           counter(&foreground_polls, &stop)));
        // The sync reached the writer, which sits in it; the run is parked.
        ASSERT_TRUE(TurnUntil(reactor, [&] { return log.syncs_entered() > syncs_before; }));
        for (int i = 0; i < 50; ++i) reactor.RunOnce();
        EXPECT_TRUE(c.run_in_flight());
        EXPECT_GE(foreground_polls, 50u) << "the reactor stopped turning while the step waited";
        EXPECT_EQ(device.writes, writes_before) << "a page went out before its record was durable";

        log.Release();
        ASSERT_TRUE(TurnUntil(reactor, [&] { return !c.run_in_flight(); }));
        stop = true;
        reactor.RunOnce();
        EXPECT_EQ(anchor.publishes(), 1u);
        EXPECT_EQ(device.violations, 0u);
        EXPECT_GE(CheckpointCensus::Of(core).last_gate_parks.load(), 1u);
        EXPECT_EQ(Count(core, CheckpointPhase::kLogGateBatch), 0u)
            << "the step waited for the log on the reactor after all";
    }


    sched::ManualClock clock_{1'000};
    sched::NullIoBackend io_;
    sched::WakerTable wakers_{2};
    std::unique_ptr<HeldLogDevice> log_device_;
    std::unique_ptr<WalManager> owner_;
    std::unique_ptr<WalManager> peer_;
    std::unique_ptr<storage::MemoryPageDevice> page_device_;
    std::unique_ptr<WalCheckingDevice> checking_;
    std::unique_ptr<storage::DevicePageStore> store_;
    std::unique_ptr<storage::PageStoreCheckpointTarget> target_;
    NoActiveTransactions txns_;
    InMemoryCheckpointAnchor anchor_;
};

TEST_F(CheckpointLogParkTest, APeersRunParksWhileItsReactorRuns) {
    LoggedPages(*peer_, 6);
    auto reactor = ReactorAs(1);
    ParksWhileTheReactorRuns(*reactor, *peer_, 1);
}

TEST_F(CheckpointLogParkTest, CoreZerosRunParksThroughTheWriter) {
    LoggedPages(*owner_, 6);
    auto reactor = ReactorAs(0);
    const std::uint64_t inline_before = owner_->stats().syncs;
    ParksWhileTheReactorRuns(*reactor, *owner_, 0);
    EXPECT_EQ(owner_->stats().syncs, inline_before) << "core 0 synced the log on its reactor";
}

TEST_F(CheckpointLogParkTest, APageRedirtiedDuringTheParkIsRegatedByAnotherPark) {
    const std::vector<PageId> ids = LoggedPages(*peer_, 4);
    auto reactor = ReactorAs(1);
    Checkpointer c(*peer_, *target_, txns_, anchor_, CheckpointerConfig{.pages_per_step = 4});
    log_device_->Hold();
    const std::uint64_t syncs_before = log_device_->syncs_entered();
    c.SubmitRun(*reactor, nullptr, 1);
    ASSERT_TRUE(TurnUntil(*reactor, [&] { return log_device_->syncs_entered() > syncs_before; }));
    reactor->RunOnce();

    // A session on this core changes a page of the batch while the step
    // waits: its record is newer than the sync the step asked for.
    Stamp(*peer_, ids[2]);
    log_device_->Release();
    ASSERT_TRUE(TurnUntil(*reactor, [&] { return !c.run_in_flight(); }));
    EXPECT_EQ(anchor_.publishes(), 1u);
    EXPECT_EQ(checking_->violations, 0u);
    EXPECT_GE(CheckpointCensus::Of(1).last_gate_parks.load(), 2u)
        << "the page re-dirtied during the park was not re-gated by a park";
    EXPECT_EQ(Count(1, CheckpointPhase::kLogGateBatch), 0u)
        << "the re-dirtied page was gated by an inline sync";
}

TEST_F(CheckpointLogParkTest, ARunWhoseRecordIsNotDurableIsHandedBackUnwritten) {
    // BI-Q4 (a): the batch's gate passes, then a page of the second run is
    // changed by a record not yet durable before that run is copied. The
    // writeback writes the first run, hands back the second's LSN and leaves
    // its frames dirty - and never writes a page ahead of its record.
    const CurrentCoreGuard as_peer(1);
    const std::vector<PageId> ids = LoggedPages(*peer_, 2 * storage::DevicePageStore::kWritebackRunPages);
    ASSERT_TRUE(peer_->SyncAll().ok());
    bool stamped = false;
    wal::Lsn late = kNoLsn;
    store_->SetAfterWritebackCopyForTest([&](PageId copied) {
        if (stamped || copied != ids.front()) return;
        stamped = true;
        auto lsn = peer_->Append({RecordType::kCheckpointEnd, kNoTxnId, ids.back(), 0},
                                 std::vector<std::byte>(kCheckpointEndPayloadSize));
        ASSERT_TRUE(lsn.ok());
        late = lsn.value();
        ASSERT_TRUE(store_->StampPageLsn(ids.back(), late).ok());
    });
    log_device_->Hold();  // nothing the writeback could ask for lands meanwhile
    const std::uint64_t writes_before = checking_->writes;
    wal::Lsn deferred = kNoLsn;
    ASSERT_TRUE(store_->WriteBackPages(ids, &deferred).ok());
    store_->SetAfterWritebackCopyForTest({});

    EXPECT_EQ(deferred, late) << "the second run's LSN was not handed back";
    EXPECT_EQ(checking_->writes, writes_before + 1) << "only the first run is written";
    const std::vector<PageId> dirty = store_->DirtyPageIds();
    EXPECT_TRUE(std::find(dirty.begin(), dirty.end(), ids.back()) != dirty.end());
    EXPECT_EQ(checking_->violations, 0u);

    log_device_->Release();
    ASSERT_TRUE(peer_->SyncAll().ok());
    deferred = kNoLsn;
    ASSERT_TRUE(store_->WriteBackPages(ids, &deferred).ok());
    EXPECT_EQ(deferred, kNoLsn);
    EXPECT_TRUE(store_->DirtyPageIds().empty());
    EXPECT_EQ(checking_->violations, 0u);
}

TEST_F(CheckpointLogParkTest, AFailStoppedLogAnswersAParkedStep) {
    LoggedPages(*peer_, 6);
    auto reactor = ReactorAs(1);
    Checkpointer c(*peer_, *target_, txns_, anchor_, CheckpointerConfig{.pages_per_step = 4});
    log_device_->Hold();
    const std::uint64_t writes_before = checking_->writes;
    const std::uint64_t syncs_before = log_device_->syncs_entered();
    c.SubmitRun(*reactor, nullptr, 1);
    ASSERT_TRUE(TurnUntil(*reactor, [&] { return log_device_->syncs_entered() > syncs_before; }));
    log_device_->Release(/*fail=*/true);

    ASSERT_TRUE(TurnUntil(*reactor, [&] { return !c.run_in_flight(); }))
        << "the parked step never noticed the log had stopped";
    EXPECT_TRUE(peer_->stopped());
    EXPECT_FALSE(CheckpointCensus::Of(1).last_completed.load());
    EXPECT_EQ(checking_->writes, writes_before) << "a page was written after the log stopped";
    EXPECT_TRUE(c.in_progress()) << "the failed run is the next run's to resume";
    EXPECT_EQ(anchor_.publishes(), 0u);
}

}  // namespace
}  // namespace kds::wal
