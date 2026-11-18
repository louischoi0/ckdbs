#include "kds/wal/checkpointer.hpp"

#include <algorithm>
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
#include "kds/wal/log_scanner.hpp"
#include "kds/wal/memory_log_device.hpp"

// CZ-S3 (`instructions/v3.2.0/workorder-cz-checkpoint-roll-wait.md` §8.8): the
// cadence checkpoint task tries the stream latch at its begin record, its end
// record and its log park's flush, and parks on a busy answer instead of
// sleeping its reactor thread; its recycle is tried and skipped. The rig is
// `checkpoint_log_park_test.cpp`'s - a shared stream, core 1 attached, the
// reactor driven by hand on a manual clock - with a log device whose sync and
// whose writes can each be held. A held sync under a roll is a held roll.

namespace kds::wal {
namespace {

constexpr std::uint64_t kSegmentSize = 1024 * 1024;
using Site = CheckpointLatchSite;

// A log device whose `Sync` and `WriteAt` can each be held, and whose sync
// can be made to fail, by the test.
class GatedLogDevice final : public LogDevice {
public:
    explicit GatedLogDevice(std::unique_ptr<MemoryLogDevice> inner) : inner_(std::move(inner)) {}
    std::uint64_t segment_size() const noexcept override { return inner_->segment_size(); }
    std::uint64_t first_segment() const noexcept override { return inner_->first_segment(); }
    std::uint64_t end_segment() const noexcept override { return inner_->end_segment(); }
    Status CreateSegment(std::uint64_t n, std::span<const std::byte> header) override {
        return inner_->CreateSegment(n, header);
    }
    void PrepareSegment(std::uint64_t n) override { inner_->PrepareSegment(n); }
    Status WriteAt(std::uint64_t n, std::uint64_t off, std::span<const std::byte> in) override {
        {
            std::unique_lock lock(mu_);
            ++writes_entered_;
            cv_.notify_all();
            cv_.wait(lock, [this] { return !writes_held_; });
        }
        return inner_->WriteAt(n, off, in);
    }
    Status ReadAt(std::uint64_t n, std::uint64_t off, std::span<std::byte> out) override {
        return inner_->ReadAt(n, off, out);
    }
    Status Sync() override {
        std::unique_lock lock(mu_);
        ++syncs_entered_;
        cv_.notify_all();
        cv_.wait(lock, [this] { return !syncs_held_; });
        if (fail_) return Status::IoError("injected: log sync failed");
        lock.unlock();
        return inner_->Sync();
    }
    Status DetachBelow(std::uint64_t n) override { return inner_->DetachBelow(n); }
    Status ReclaimDetached() override { return inner_->ReclaimDetached(); }
    std::uint64_t segments_removed() const noexcept override { return inner_->segments_removed(); }

    void HoldSyncs() { Set([this] { syncs_held_ = true; }); }
    void HoldWrites() { Set([this] { writes_held_ = true; }); }
    void ReleaseWrites() { Set([this] { writes_held_ = false; }); }
    void Release(bool fail = false) {
        Set([this, fail] {
            syncs_held_ = false;
            writes_held_ = false;
            fail_ = fail;
        });
    }
    std::uint64_t writes_entered() {
        std::lock_guard lock(mu_);
        return writes_entered_;
    }
    MemoryLogDevice& inner() { return *inner_; }

private:
    void Set(const std::function<void()>& change) {
        std::lock_guard lock(mu_);
        change();
        cv_.notify_all();
    }
    std::unique_ptr<MemoryLogDevice> inner_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool syncs_held_ = false;
    bool writes_held_ = false;
    bool fail_ = false;
    std::uint64_t syncs_entered_ = 0;
    std::uint64_t writes_entered_ = 0;
};

// The live set, read by the test: how often it was snapshotted, and what.
class CountedTransactions final : public ActiveTransactions {
public:
    std::vector<CheckpointActiveTxn> Snapshot() const override {
        std::lock_guard lock(mu_);
        ++snapshots_;
        return live_;
    }
    void Begin(std::uint64_t id) {
        std::lock_guard lock(mu_);
        live_.push_back({id, 0});
    }
    std::uint64_t snapshots() const {
        std::lock_guard lock(mu_);
        return snapshots_;
    }

private:
    mutable std::mutex mu_;
    mutable std::uint64_t snapshots_ = 0;
    std::vector<CheckpointActiveTxn> live_;
};

class CheckpointRollParkTest : public ::testing::Test {
protected:
    void SetUp() override { Build(/*relaxed_flush_interval_ns=*/10'000'000); }

    void Build(sched::MonoTimeNs relaxed_flush_interval_ns,
               std::uint64_t segment_size = kSegmentSize) {
        // A rebuild (tests 9 and 10) drops the old rig users first, so no old
        // writer thread or store outlives the device it reads.
        for (auto& reactor : reactors_) reactor.reset();
        target_.reset();
        store_.reset();
        page_device_.reset();
        peer_.reset();
        owner_.reset();
        auto mem = MemoryLogDevice::Create(segment_size);
        ASSERT_TRUE(mem.ok());
        log_ = std::make_unique<GatedLogDevice>(std::move(mem.value()));
        WalManagerConfig config;
        config.shared_stream = true;
        config.relaxed_flush_interval_ns = relaxed_flush_interval_ns;
        auto owner = WalManager::Open(log_.get(), clock_, 0, config);
        ASSERT_TRUE(owner.ok()) << owner.status().message();
        owner_ = std::move(owner.value());
        owner_->StartWriter();
        auto peer = WalManager::Attach(owner_->stream(), owner_->writer(), clock_, 1, config);
        ASSERT_TRUE(peer.ok()) << peer.status().message();
        peer_ = std::move(peer.value());

        auto pages = storage::MemoryPageDevice::Create(/*extent_pages=*/512, /*initial_pages=*/0);
        ASSERT_TRUE(pages.ok());
        page_device_ = std::move(pages.value());
        auto store = storage::DevicePageStore::Open(*page_device_, storage::FrameCapacity{4096},
                                                    /*first_new_page_id=*/16);
        ASSERT_TRUE(store.ok()) << store.status().message();
        store_ = std::move(store.value());
        ASSERT_TRUE(store_->SetCoreWalGate(0, owner_.get()).ok());
        ASSERT_TRUE(store_->SetCoreWalGate(1, peer_.get()).ok());
        target_ = std::make_unique<storage::PageStoreCheckpointTarget>(*store_);
        for (std::uint32_t core = 0; core < 2; ++core) {
            reactors_[core] = std::make_unique<sched::Scheduler>(clock_, io_);
            ASSERT_TRUE(reactors_[core]->AttachWakerTable(&wakers_, core).ok());
        }
    }

    void TearDown() override {
        // A reactor's `AttachWakerTable` names this thread its core; the next
        // test in the binary must not inherit core 1.
        SetCurrentCore(0);
        if (log_ == nullptr) return;
        log_->Release();
        JoinHelpers();
        store_->ClearCoreWalGate(0, owner_.get());
        store_->ClearCoreWalGate(1, peer_.get());
    }

    void JoinHelpers() {
        for (std::thread& t : helpers_) {
            if (t.joinable()) t.join();
        }
        helpers_.clear();
    }

    WalStream& stream() { return *owner_->stream(); }

    // One page changed by a record `wal` appended and has not synced.
    PageId LoggedPage(WalManager& wal) {
        auto made = store_->CreateNew();
        EXPECT_TRUE(made.ok());
        storage::FormatPage(made.value().second.bytes(), PageType::kHeap);
        const PageId id = made.value().first;
        auto lsn = wal.Append({RecordType::kCheckpointEnd, kNoTxnId, id, 0},
                              std::vector<std::byte>(kCheckpointEndPayloadSize));
        EXPECT_TRUE(lsn.ok()) << lsn.status().message();
        EXPECT_TRUE(store_->StampPageLsn(id, lsn.value()).ok());
        return id;
    }

    // **A roll held on a helper thread**: the syncs are held, the segment is
    // sealed and the next append rolls, so the roll sits in its sync of the
    // segment it leaves with the stream latch held. Returns once the roll
    // flag is up.
    void HoldARoll() {
        log_->HoldSyncs();
        helpers_.emplace_back([this] {
            const CurrentCoreGuard as(0);
            (void)stream().Seal();
            (void)stream().Append({RecordType::kTxnAbort, 7, kInvalidPageId, 0});
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!stream().rolling()) {
            ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "the roll never started";
            std::this_thread::yield();
        }
    }

    // **A flush in flight on a helper thread**: bytes staged, the writes
    // held, and a flush that has swapped the ring and sits in its unlatched
    // write. The latch is free; a tried flush or detach answers busy.
    void HoldAFlushInFlight() {
        ASSERT_TRUE(peer_->Append({RecordType::kTxnAbort, 8, kInvalidPageId, 0}).ok());
        const std::uint64_t entered = log_->writes_entered();
        log_->HoldWrites();
        helpers_.emplace_back([this] { (void)stream().Flush(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (log_->writes_entered() == entered) {
            ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "the flush never wrote";
            std::this_thread::yield();
        }
    }

    // Turns core `core`'s reactor until `done`, for at most 10 s of wall
    // time: the writer and the helpers are real threads.
    bool TurnUntil(std::uint32_t core, const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            reactors_[core]->RunOnce();
        }
        return true;
    }
    void Turn(std::uint32_t core, int times) {
        for (int i = 0; i < times; ++i) reactors_[core]->RunOnce();
    }

    static const CheckpointRunReport::LatchSite& At(const Checkpointer& c, Site site) {
        return c.task_report().latch_sites[static_cast<std::size_t>(site) - 1];
    }

    // The records of type `type` in the log, in stream order.
    std::vector<Lsn> RecordsOf(RecordType type) {
        std::vector<Lsn> out;
        auto scanned = ScanLog(log_->inner(), 0, 0, [&](const DecodedRecord& r) {
            if (r.type() == type) out.push_back(r.header.lsn);
            return Status::OK();
        });
        EXPECT_TRUE(scanned.ok()) << scanned.status().message();
        return out;
    }

    sched::ManualClock clock_{1'000};
    sched::NullIoBackend io_;
    sched::WakerTable wakers_{2};
    std::unique_ptr<GatedLogDevice> log_;
    std::unique_ptr<WalManager> owner_;
    std::unique_ptr<WalManager> peer_;
    std::unique_ptr<storage::MemoryPageDevice> page_device_;
    std::unique_ptr<storage::DevicePageStore> store_;
    std::unique_ptr<storage::PageStoreCheckpointTarget> target_;
    std::unique_ptr<sched::Scheduler> reactors_[2];
    CountedTransactions txns_;
    InMemoryCheckpointAnchor anchor_;
    std::vector<std::thread> helpers_;
};

// 1. The begin record under a roll: the task parks before it snapshots
// anything and appends no begin record; a transaction begun and a page
// dirtied during the hold are in the record appended after the release
// (CZ-R3).
TEST_F(CheckpointRollParkTest, ABeginRecordWaitsOutARollWithNoSnapshotTaken) {
    Checkpointer c(*peer_, *target_, txns_, anchor_, CheckpointerConfig{.pages_per_step = 4});
    HoldARoll();
    c.SubmitRun(*reactors_[1], nullptr, 1);
    Turn(1, 20);
    EXPECT_TRUE(c.run_in_flight());
    EXPECT_FALSE(c.in_progress()) << "a begin record went in under the roll";
    EXPECT_EQ(txns_.snapshots(), 0u) << "a snapshot was taken to be thrown away";
    EXPECT_GE(At(c, Site::kBegin).busy, 1u);
    EXPECT_GE(At(c, Site::kBegin).roll_parks, 1u);

    // During the hold: a transaction begins and a page is dirtied.
    txns_.Begin(42);
    auto made = store_->CreateNew();
    ASSERT_TRUE(made.ok());
    storage::FormatPage(made.value().second.bytes(), PageType::kHeap);
    const PageId dirtied = made.value().first;

    log_->Release();
    JoinHelpers();
    ASSERT_TRUE(TurnUntil(1, [&] { return !c.run_in_flight(); }));
    EXPECT_EQ(anchor_.publishes(), 1u);
    EXPECT_EQ(At(c, Site::kBegin).bound, 0u);

    bool found = false;
    auto scanned = ScanLog(log_->inner(), 0, 0, [&](const DecodedRecord& r) {
        if (r.type() != RecordType::kCheckpointBegin) return Status::OK();
        auto begin = DecodeCheckpointBegin(r.payload);
        EXPECT_TRUE(begin.ok());
        found = true;
        EXPECT_EQ(begin.value().active_txns.size(), 1u);
        EXPECT_TRUE(std::ranges::any_of(begin.value().dirty_pages,
                                        [&](const CheckpointDirtyPage& p) {
                                            return p.page_id == dirtied;
                                        }))
            << "the page dirtied during the hold is not in the begin record";
        return Status::OK();
    });
    ASSERT_TRUE(scanned.ok());
    EXPECT_TRUE(found);
}

// 1b. The begin record under another holder: the tried append answers busy
// after both snapshots, appends nothing, and the retake carries what changed.
TEST_F(CheckpointRollParkTest, ABusyBeginRecordRetakesItsSnapshot) {
    Checkpointer c(*peer_, *target_, txns_, anchor_, CheckpointerConfig{.pages_per_step = 4});
    // A seal under the latch, its write held: the latch is busy, no roll.
    ASSERT_TRUE(peer_->Append({RecordType::kTxnAbort, 9, kInvalidPageId, 0}).ok());
    log_->HoldWrites();
    helpers_.emplace_back([this] { (void)stream().Seal(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (log_->writes_entered() == 0) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::yield();
    }
    ASSERT_FALSE(stream().rolling());
    c.SubmitRun(*reactors_[1], nullptr, 1);
    Turn(1, 3);
    EXPECT_FALSE(c.in_progress());
    EXPECT_GE(At(c, Site::kBegin).busy, 1u);
    EXPECT_EQ(At(c, Site::kBegin).roll_parks, 0u);
    const std::uint64_t before = txns_.snapshots();
    EXPECT_GE(before, 1u);
    txns_.Begin(43);
    log_->ReleaseWrites();
    JoinHelpers();
    clock_.Advance(Checkpointer::kBusyParkNs);
    ASSERT_TRUE(TurnUntil(1, [&] { return !c.run_in_flight(); }));
    EXPECT_GT(txns_.snapshots(), before) << "the snapshot was not taken again";
    EXPECT_EQ(anchor_.publishes(), 1u);
    auto scanned = ScanLog(log_->inner(), 0, 0, [&](const DecodedRecord& r) {
        if (r.type() == RecordType::kCheckpointBegin) {
            EXPECT_EQ(DecodeCheckpointBegin(r.payload).value().active_txns.size(), 1u);
        }
        return Status::OK();
    });
    ASSERT_TRUE(scanned.ok());
    EXPECT_EQ(RecordsOf(RecordType::kCheckpointBegin).size(), 1u);
}

// 2. The park's flush under a roll: skipped, the durable point never above
// what was flushed at any step (CZ-R2), and the park ends at the roll's
// publication with no second try.
TEST_F(CheckpointRollParkTest, AFlushUnderARollIsSkippedAndTheParkEndsAtItsPublication) {
    Checkpointer c(*peer_, *target_, txns_, anchor_, CheckpointerConfig{.pages_per_step = 4});
    LoggedPage(*peer_);
    c.SubmitRun(*reactors_[1], nullptr, 1);
    Turn(1, 1);  // `Start`: the begin record is staged
    ASSERT_TRUE(c.in_progress());
    HoldARoll();
    for (int i = 0; i < 20; ++i) {
        reactors_[1]->RunOnce();
        EXPECT_LE(owner_->durable_lsn(), owner_->flushed_lsn()) << "step " << i;
    }
    EXPECT_GE(At(c, Site::kFlush).busy, 1u);
    EXPECT_GE(At(c, Site::kFlush).roll_parks, 1u);
    EXPECT_EQ(anchor_.publishes(), 0u);

    log_->Release();
    JoinHelpers();
    ASSERT_TRUE(TurnUntil(1, [&] {
        EXPECT_LE(owner_->durable_lsn(), owner_->flushed_lsn());
        return !c.run_in_flight();
    }));
    EXPECT_EQ(anchor_.publishes(), 1u);
    EXPECT_EQ(At(c, Site::kFlush).busy, 1u) << "the park under a roll tried the flush again";
    EXPECT_EQ(At(c, Site::kFlush).bound, 0u);
}

// 3. Another thread's flush in flight: the park's flush is skipped, tried
// again behind the timed park, and done once the write is released.
TEST_F(CheckpointRollParkTest, AFlushInFlightIsTriedAgainBehindATimedPark) {
    Checkpointer c(*peer_, *target_, txns_, anchor_, CheckpointerConfig{.pages_per_step = 4});
    HoldAFlushInFlight();
    c.SubmitRun(*reactors_[1], nullptr, 1);
    Turn(1, 3);
    ASSERT_TRUE(c.in_progress());
    EXPECT_EQ(At(c, Site::kFlush).busy, 1u);
    Turn(1, 5);
    EXPECT_EQ(At(c, Site::kFlush).busy, 1u) << "tried again before the park's 1 ms";
    clock_.Advance(Checkpointer::kBusyParkNs);
    Turn(1, 3);
    EXPECT_EQ(At(c, Site::kFlush).busy, 2u) << "not tried again behind the park";
    EXPECT_EQ(At(c, Site::kFlush).roll_parks, 0u);

    log_->ReleaseWrites();
    JoinHelpers();
    clock_.Advance(Checkpointer::kBusyParkNs);
    ASSERT_TRUE(TurnUntil(1, [&] { return !c.run_in_flight(); }));
    EXPECT_EQ(anchor_.publishes(), 1u);
    EXPECT_EQ(At(c, Site::kFlush).busy, 2u);
    EXPECT_EQ(At(c, Site::kFlush).bound, 0u);
}

// 4a. The write released while the clock stands: the task's own retry
// cannot come, and core 0's drain ends the park.
class CheckpointRollParkDrainTest : public CheckpointRollParkTest {
protected:
    void SetUp() override { Build(/*relaxed_flush_interval_ns=*/0); }
};

TEST_F(CheckpointRollParkDrainTest, AParkWhoseRetryCannotComeIsEndedByCoreZerosDrain) {
    Checkpointer c(*peer_, *target_, txns_, anchor_, CheckpointerConfig{.pages_per_step = 4});
    HoldAFlushInFlight();
    c.SubmitRun(*reactors_[1], nullptr, 1);
    Turn(1, 3);
    ASSERT_EQ(At(c, Site::kFlush).busy, 1u);
    log_->ReleaseWrites();
    JoinHelpers();
    Turn(1, 10);
    EXPECT_TRUE(c.run_in_flight()) << "the park ended with nothing to end it";
    {
        const CurrentCoreGuard as(0);
        ASSERT_TRUE(owner_->DrainOnce().ok());
    }
    ASSERT_TRUE(TurnUntil(1, [&] { return !c.run_in_flight(); }));
    EXPECT_EQ(anchor_.publishes(), 1u);
    EXPECT_EQ(At(c, Site::kFlush).busy, 1u) << "the drain's flush, not a second try, ended it";
}

// 4b. Core 0's own skipped flush leaves its D3 clock where it was: the
// drain's loss-window flush still fires on time.
TEST_F(CheckpointRollParkTest, CoreZerosSkippedFlushLeavesTheLossWindowClockAlone) {
    const CurrentCoreGuard as(0);
    ASSERT_TRUE(owner_->RequestSyncNow().ok());  // the D3 clock stamped now
    clock_.Advance(10'000'000);                  // one interval later
    HoldAFlushInFlight();
    bool busy = false;
    ASSERT_TRUE(owner_->TryRequestSyncNow(&busy).ok());
    EXPECT_TRUE(busy);
    log_->ReleaseWrites();
    JoinHelpers();
    ASSERT_TRUE(owner_->Append({RecordType::kTxnAbort, 10, kInvalidPageId, 0}).ok());
    const std::uint64_t before = owner_->stats().interval_syncs;
    ASSERT_TRUE(owner_->DrainOnce().ok());
    EXPECT_EQ(owner_->stats().interval_syncs, before + 1)
        << "the skipped flush pushed the loss window's clock forward";
}

// 5. The end record under a roll: none until the release, and the anchor
// is published after it.
TEST_F(CheckpointRollParkTest, AnEndRecordWaitsOutARoll) {
    Checkpointer c(*peer_, *target_, txns_, anchor_, CheckpointerConfig{.pages_per_step = 4});
    c.SubmitRun(*reactors_[1], nullptr, 1);
    Turn(1, 1);
    ASSERT_TRUE(c.in_progress());
    {
        const CurrentCoreGuard as(0);
        ASSERT_TRUE(owner_->SyncAll().ok());  // the begin record durable
    }
    HoldARoll();
    Turn(1, 20);
    EXPECT_GE(At(c, Site::kEnd).busy, 1u);
    EXPECT_GE(At(c, Site::kEnd).roll_parks, 1u);
    EXPECT_TRUE(RecordsOf(RecordType::kCheckpointEnd).empty());
    EXPECT_EQ(anchor_.publishes(), 0u);
    log_->Release();
    JoinHelpers();
    ASSERT_TRUE(TurnUntil(1, [&] { return !c.run_in_flight(); }));
    EXPECT_EQ(RecordsOf(RecordType::kCheckpointEnd).size(), 1u);
    EXPECT_EQ(anchor_.publishes(), 1u);
}

// 6. The bound: a roll that never ends, the clock moved a ceiling at a time.
// The sixteenth busy answer is the last; the blocking entry is called and a
// helper thread's release ends it.
TEST_F(CheckpointRollParkTest, PastSixteenBusyAnswersTheBlockingEntryIsCalled) {
    Checkpointer c(*peer_, *target_, txns_, anchor_, CheckpointerConfig{.pages_per_step = 4});
    HoldARoll();
    c.SubmitRun(*reactors_[1], nullptr, 1);
    Turn(1, 2);
    for (int i = 1; i < Checkpointer::kLatchTries - 1; ++i) {
        ASSERT_EQ(At(c, Site::kBegin).busy, static_cast<std::uint64_t>(i));
        clock_.Advance(Checkpointer::kRollParkCeilingNs);
        Turn(1, 2);
    }
    ASSERT_EQ(At(c, Site::kBegin).busy, static_cast<std::uint64_t>(Checkpointer::kLatchTries - 1));
    EXPECT_EQ(At(c, Site::kBegin).bound, 0u);
    // The next poll takes the sixteenth busy answer and, with no park, blocks
    // this thread in `Start`; a helper ends the roll.
    std::thread releaser([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        log_->Release();
    });
    clock_.Advance(Checkpointer::kRollParkCeilingNs);
    ASSERT_TRUE(TurnUntil(1, [&] { return c.in_progress() || !c.run_in_flight(); }));
    releaser.join();
    JoinHelpers();
    ASSERT_TRUE(TurnUntil(1, [&] { return !c.run_in_flight(); }));
    EXPECT_EQ(At(c, Site::kBegin).busy, static_cast<std::uint64_t>(Checkpointer::kLatchTries));
    EXPECT_EQ(At(c, Site::kBegin).bound, 1u);
    EXPECT_EQ(anchor_.publishes(), 1u);
}

// 6b. The same bound at the flush site, which is the run's, not a call's: the
// sixteenth busy answer sends the park to the blocking flush.
TEST_F(CheckpointRollParkTest, PastSixteenBusyAnswersTheFlushIsTakenBlocking) {
    Checkpointer c(*peer_, *target_, txns_, anchor_, CheckpointerConfig{.pages_per_step = 4});
    LoggedPage(*peer_);
    c.SubmitRun(*reactors_[1], nullptr, 1);
    Turn(1, 1);
    ASSERT_TRUE(c.in_progress());
    HoldARoll();
    Turn(1, 2);
    for (int i = 1; i < Checkpointer::kLatchTries - 1; ++i) {
        ASSERT_EQ(At(c, Site::kFlush).busy, static_cast<std::uint64_t>(i));
        clock_.Advance(Checkpointer::kRollParkCeilingNs);
        Turn(1, 2);
    }
    ASSERT_EQ(At(c, Site::kFlush).bound, 0u);
    std::thread releaser([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        log_->Release();
    });
    clock_.Advance(Checkpointer::kRollParkCeilingNs);
    ASSERT_TRUE(TurnUntil(1, [&] { return At(c, Site::kFlush).bound > 0 || !c.run_in_flight(); }));
    releaser.join();
    JoinHelpers();
    ASSERT_TRUE(TurnUntil(1, [&] { return !c.run_in_flight(); }));
    EXPECT_EQ(At(c, Site::kFlush).busy, static_cast<std::uint64_t>(Checkpointer::kLatchTries));
    // Every later flush of the run is blocking too - the bound is the run's -
    // and each is counted.
    EXPECT_GE(At(c, Site::kFlush).bound, 1u);
    EXPECT_EQ(anchor_.publishes(), 1u);
}

// 7. The roll flag left set with no roll behind it: the park ends at its
// ceiling, so nothing's liveness rests on the flag.
TEST_F(CheckpointRollParkTest, ARollFlagLeftSetEndsTheParkAtItsCeiling) {
    Checkpointer c(*peer_, *target_, txns_, anchor_, CheckpointerConfig{.pages_per_step = 4});
    stream().SetRollingForTest(true);
    c.SubmitRun(*reactors_[1], nullptr, 1);
    Turn(1, 2);
    ASSERT_EQ(At(c, Site::kBegin).busy, 1u);
    clock_.Advance(Checkpointer::kRollParkCeilingNs - 1);
    Turn(1, 3);
    EXPECT_EQ(At(c, Site::kBegin).busy, 1u) << "the park ended before its ceiling";
    clock_.Advance(1);
    Turn(1, 3);
    EXPECT_EQ(At(c, Site::kBegin).busy, 2u) << "the park did not end at its ceiling";
    stream().SetRollingForTest(false);
    clock_.Advance(Checkpointer::kRollParkCeilingNs);
    ASSERT_TRUE(TurnUntil(1, [&] { return !c.run_in_flight(); }));
    EXPECT_EQ(anchor_.publishes(), 1u);
}

// 8. The log stopped while the task is parked, at each site: the task ends
// with the stop and publishes no anchor (CZ-R8).
TEST_F(CheckpointRollParkTest, AStoppedLogEndsAParkAtTheBeginRecord) {
    Checkpointer c(*peer_, *target_, txns_, anchor_, CheckpointerConfig{.pages_per_step = 4});
    HoldARoll();
    c.SubmitRun(*reactors_[1], nullptr, 1);
    Turn(1, 5);
    ASSERT_GE(At(c, Site::kBegin).busy, 1u);
    log_->Release(/*fail=*/true);
    JoinHelpers();
    ASSERT_TRUE(TurnUntil(1, [&] { return !c.run_in_flight(); }));
    EXPECT_TRUE(peer_->stopped());
    EXPECT_FALSE(c.in_progress());
    EXPECT_EQ(anchor_.publishes(), 0u);
}

TEST_F(CheckpointRollParkTest, AStoppedLogEndsAParkAtTheEndRecord) {
    Checkpointer c(*peer_, *target_, txns_, anchor_, CheckpointerConfig{.pages_per_step = 4});
    c.SubmitRun(*reactors_[1], nullptr, 1);
    Turn(1, 1);
    {
        const CurrentCoreGuard as(0);
        ASSERT_TRUE(owner_->SyncAll().ok());
    }
    HoldARoll();
    Turn(1, 20);
    ASSERT_GE(At(c, Site::kEnd).busy, 1u);
    log_->Release(/*fail=*/true);
    JoinHelpers();
    ASSERT_TRUE(TurnUntil(1, [&] { return !c.run_in_flight(); }));
    EXPECT_TRUE(peer_->stopped());
    EXPECT_EQ(anchor_.publishes(), 0u);
    EXPECT_TRUE(RecordsOf(RecordType::kCheckpointEnd).empty());
}

TEST_F(CheckpointRollParkTest, AStoppedLogEndsAParkAtTheFlush) {
    Checkpointer c(*peer_, *target_, txns_, anchor_, CheckpointerConfig{.pages_per_step = 4});
    LoggedPage(*peer_);
    c.SubmitRun(*reactors_[1], nullptr, 1);
    Turn(1, 1);
    HoldARoll();
    Turn(1, 5);
    ASSERT_GE(At(c, Site::kFlush).busy, 1u);
    log_->Release(/*fail=*/true);
    JoinHelpers();
    ASSERT_TRUE(TurnUntil(1, [&] { return !c.run_in_flight(); }));
    EXPECT_TRUE(peer_->stopped());
    EXPECT_EQ(anchor_.publishes(), 0u);
}

// 9. The task abandoned behind each new suspension does nothing more: a
// frame polled after `Abandon` appends nothing and publishes nothing.
TEST_F(CheckpointRollParkTest, AnAbandonedTaskDoesNothingMoreAfterAnyNewPark) {
    for (const Site site : {Site::kBegin, Site::kEnd, Site::kFlush}) {
        SCOPED_TRACE(static_cast<int>(site));
        TearDown();
        Build(10'000'000);
        InMemoryCheckpointAnchor anchor;
        Checkpointer c(*peer_, *target_, txns_, anchor, CheckpointerConfig{.pages_per_step = 4});
        if (site == Site::kFlush) LoggedPage(*peer_);
        c.SubmitRun(*reactors_[1], nullptr, 1);
        if (site != Site::kBegin) {
            Turn(1, 1);
            ASSERT_TRUE(c.in_progress());
        }
        if (site == Site::kEnd) {
            const CurrentCoreGuard as(0);
            ASSERT_TRUE(owner_->SyncAll().ok());
        }
        HoldARoll();
        Turn(1, 20);
        ASSERT_GE(At(c, site).busy, 1u);
        // `LoggedPage` stamps with an end record's type, so both are counted.
        const std::size_t begins = RecordsOf(RecordType::kCheckpointBegin).size();
        const std::size_t ends = RecordsOf(RecordType::kCheckpointEnd).size();
        c.Abandon();
        log_->Release();
        JoinHelpers();
        Turn(1, 50);
        EXPECT_EQ(RecordsOf(RecordType::kCheckpointBegin).size(), begins);
        EXPECT_EQ(RecordsOf(RecordType::kCheckpointEnd).size(), ends);
        EXPECT_EQ(anchor.publishes(), 0u);
        EXPECT_FALSE(c.in_progress());
    }
}

// 10. A tried append on a full ring: the tried flush busy, nothing staged;
// free, the record goes in.
TEST_F(CheckpointRollParkTest, ATriedAppendOnAFullRingDrainsWithTheTriedFlush) {
    // A segment four rings long, so the ring fills before the segment does
    // and no roll waits out the held flush.
    TearDown();
    Build(10'000'000, 4 * kDefaultRingCapacity);
    const CurrentCoreGuard as(1);
    HoldAFlushInFlight();  // the ring swapped out and being written
    const std::vector<std::byte> payload(100 * 1024, std::byte{0x11});
    while (stream().ring_free() >= 2 * EncodedRecordSize(payload.size())) {
        ASSERT_TRUE(peer_->Append({RecordType::kHeapInsert, 1, 7, 0}, payload).ok());
    }
    const Lsn append_before = stream().append_lsn();
    bool busy = false;
    auto lsn = peer_->TryAppend({RecordType::kHeapInsert, 1, 7, 0},
                                std::vector<std::byte>(stream().ring_free()), &busy);
    ASSERT_TRUE(lsn.ok()) << lsn.status().message();
    EXPECT_TRUE(busy);
    EXPECT_EQ(lsn.value(), kNoLsn);
    EXPECT_EQ(stream().append_lsn(), append_before) << "a busy tried append staged its record";
    EXPECT_FALSE(stream().stopped());

    log_->ReleaseWrites();
    JoinHelpers();
    lsn = peer_->TryAppend({RecordType::kHeapInsert, 1, 7, 0},
                           std::vector<std::byte>(stream().ring_free()), &busy);
    ASSERT_TRUE(lsn.ok()) << lsn.status().message();
    EXPECT_FALSE(busy);
    EXPECT_EQ(lsn.value(), append_before);
}

// 11. The recycle: the tried detach skips on a held latch and on a flush in
// flight, and a later detach, cumulative, takes what it skipped.
TEST_F(CheckpointRollParkTest, ATriedDetachSkipsOnABusyStreamAndTheNextOneCoversIt) {
    const std::vector<std::byte> payload(64 * 1024, std::byte{0x22});
    while (log_->end_segment() < 4) {
        ASSERT_TRUE(peer_->Append({RecordType::kHeapInsert, 1, 7, 0}, payload).ok());
    }
    {
        const CurrentCoreGuard as(0);
        ASSERT_TRUE(owner_->SyncAll().ok());
    }
    const Lsn below_two = 2 * kSegmentSize + kSegmentHeaderSize;
    HoldAFlushInFlight();
    EXPECT_FALSE(peer_->RecycleBelow(below_two, /*tried=*/true)) << "not skipped on a flush";
    EXPECT_EQ(log_->first_segment(), 0u);
    log_->ReleaseWrites();
    JoinHelpers();
    HoldARoll();
    EXPECT_FALSE(peer_->RecycleBelow(below_two, /*tried=*/true)) << "not skipped on a roll";
    log_->Release();
    JoinHelpers();
    const Lsn below_three = 3 * kSegmentSize + kSegmentHeaderSize;
    EXPECT_TRUE(peer_->RecycleBelow(below_three, /*tried=*/true));
    EXPECT_EQ(log_->first_segment(), 3u) << "the later detach did not cover the skipped one";
    EXPECT_EQ(owner_->recycle_failures(), 0u);
}

// 12. Core 0's run with an assertion source, a roll started behind its begin
// record: the bases stay blocking (CZ-Q16 (a)), so the run is whole and each
// pending list follows its base.
class RollingAssertionSource final : public AssertionSnapshotSource {
public:
    explicit RollingAssertionSource(std::function<void()> before_visit)
        : before_visit_(std::move(before_visit)) {}
    Status VisitSnapshots(std::size_t, const SnapshotVisitor& visit) override {
        before_visit_();
        AssertionCabinSnapshot cabin;
        cabin.assertion_id = 5;
        cabin.groups.push_back({1, 3, 30, "k"});
        cabin.pending.push_back({77, 20, 0, 1, false, "k"});
        return visit({cabin});
    }

private:
    std::function<void()> before_visit_;
};

TEST_F(CheckpointRollParkTest, CoreZerosBasesBehindARollStayWholeAndInOrder) {
    const CurrentCoreGuard as(0);
    std::thread releaser;
    RollingAssertionSource source([&] {
        HoldARoll();
        releaser = std::thread([this] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            log_->Release();
        });
    });
    Checkpointer c(*owner_, *target_, txns_, anchor_, CheckpointerConfig{.pages_per_step = 4});
    c.SetAssertionSource(&source);
    c.SubmitRun(*reactors_[0], nullptr, 0);
    reactors_[0]->RunOnce();  // `Start`: the begin record, then the bases, blocking
    releaser.join();
    JoinHelpers();
    ASSERT_TRUE(c.in_progress());
    ASSERT_TRUE(TurnUntil(0, [&] { return !c.run_in_flight(); }));
    EXPECT_EQ(anchor_.publishes(), 1u);

    std::vector<RecordType> after_begin;
    bool begun = false;
    auto scanned = ScanLog(log_->inner(), 0, 0, [&](const DecodedRecord& r) {
        if (r.type() == RecordType::kCheckpointBegin) begun = true;
        if (begun) after_begin.push_back(r.type());
        return Status::OK();
    });
    ASSERT_TRUE(scanned.ok());
    const auto base = std::ranges::find(after_begin, RecordType::kAssertSnapshot);
    ASSERT_NE(base, after_begin.end()) << "no base behind the begin record";
    ASSERT_NE(base + 1, after_begin.end());
    EXPECT_EQ(*(base + 1), RecordType::kAssertPending) << "the pending list does not follow its base";
}

// 13. One core: the tried entries never answer busy, and a cadence run's
// records are the blocking entries' - a begin record and an end record.
TEST(CheckpointRollParkOneCoreTest, OnOneCoreNothingIsBusy) {
    const CurrentCoreGuard as(0);  // an earlier test's reactor may have left another
    auto device = MemoryLogDevice::Create(kSegmentSize);
    ASSERT_TRUE(device.ok());
    sched::ManualClock clock{1'000};
    auto wal = WalManager::Open(device.value().get(), clock, 0);
    ASSERT_TRUE(wal.ok());
    ASSERT_FALSE(wal.value()->stream()->shared());
    bool busy = true;
    ASSERT_TRUE(wal.value()->TryAppend({RecordType::kTxnAbort, 1, kInvalidPageId, 0}, {}, &busy).ok());
    EXPECT_FALSE(busy);
    busy = true;
    ASSERT_TRUE(wal.value()->TryRequestSyncNow(&busy).ok());
    EXPECT_FALSE(busy);
    EXPECT_TRUE(wal.value()->RecycleBelow(0, /*tried=*/true));

    auto pages = storage::MemoryPageDevice::Create(512, 0);
    ASSERT_TRUE(pages.ok());
    auto store = storage::DevicePageStore::Open(*pages.value(), storage::FrameCapacity{256}, 16);
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE(store.value()->SetCoreWalGate(0, wal.value().get()).ok());
    storage::PageStoreCheckpointTarget target(*store.value());
    NoActiveTransactions txns;
    InMemoryCheckpointAnchor anchor;
    Checkpointer c(*wal.value(), target, txns, anchor);
    sched::NullIoBackend io;
    sched::Scheduler reactor(clock, io);
    c.SubmitRun(reactor, nullptr, 0);
    for (int i = 0; i < 100 && c.run_in_flight(); ++i) reactor.RunOnce();
    ASSERT_FALSE(c.run_in_flight());
    EXPECT_EQ(anchor.publishes(), 1u);
    for (const auto& site : c.task_report().latch_sites) {
        EXPECT_EQ(site.busy, 0u);
        EXPECT_EQ(site.bound, 0u);
    }
    std::vector<RecordType> kinds;
    auto scanned = ScanLog(*device.value(), 0, 0, [&](const DecodedRecord& r) {
        if (r.type() == RecordType::kCheckpointBegin || r.type() == RecordType::kCheckpointEnd) {
            kinds.push_back(r.type());
        }
        return Status::OK();
    });
    ASSERT_TRUE(scanned.ok());
    EXPECT_EQ(kinds, (std::vector<RecordType>{RecordType::kCheckpointBegin,
                                              RecordType::kCheckpointEnd}));
    store.value()->ClearCoreWalGate(0, wal.value().get());
}

}  // namespace
}  // namespace kds::wal
