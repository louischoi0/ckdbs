#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "frame_budget_override.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "sim/instance.hpp"

// BE-S4: the pool's capacity is a ceiling (BE-R3, BE-R4), and a full pool
// refuses only where a mutation has written nothing (BE-Q11).

namespace kds::storage {
namespace {

TEST(BoundedPoolTest, AFaultIntoAPoolOfPinnedFramesIsRefusedAndThePoolNeverGrows) {
    // BE-R4: every frame pinned, nothing reclaimable, nothing written back on
    // the fault path - the next fault is refused, and the pool is exactly its
    // capacity afterwards. Killed by dropping the limit test: the fault would
    // take a chunk past the capacity instead.
    const WithoutFrameBudgetOverride exact_capacity;  // the capacity is the subject
    constexpr std::size_t kCapacity = 16;
    auto device = MemoryPageDevice::Create(/*extent_pages=*/64, /*initial_pages=*/0);
    ASSERT_TRUE(device.ok()) << device.status().message();
    std::vector<PageId> ids;
    {
        auto setup = DevicePageStore::Open(*device.value(), FrameCapacity{64}, 16);
        ASSERT_TRUE(setup.ok()) << setup.status().message();
        for (std::size_t i = 0; i <= kCapacity; ++i) {
            auto made = setup.value()->CreateNew();
            ASSERT_TRUE(made.ok()) << made.status().message();
            FormatPage(made.value().second.bytes(), PageType::kHeap);
            ids.push_back(made.value().first);
        }
        ASSERT_TRUE(setup.value()->Sync().ok());
    }
    auto store = DevicePageStore::Open(*device.value(), FrameCapacity{kCapacity}, 16);
    ASSERT_TRUE(store.ok()) << store.status().message();
    store.value()->SetLatchArmed(store.value()->latch_armed(), /*concurrent_pinners=*/2);

    std::vector<PageRef> held;
    for (std::size_t i = 0; i < kCapacity; ++i) {
        auto ref = store.value()->GetForRead(ids[i]);
        ASSERT_TRUE(ref.ok()) << ref.status().message();
        held.push_back(std::move(ref.value()));
    }
    auto refused = store.value()->GetForRead(ids[kCapacity]);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kResourceExhausted);
    EXPECT_NE(refused.status().message().find(std::to_string(kCapacity) + " pinned"),
              std::string::npos)
        << refused.status().message();
    EXPECT_EQ(store.value()->frame_slots().slots, kCapacity);
    EXPECT_EQ(store.value()->resident_pages(), kCapacity);
    EXPECT_EQ(store.value()->pool_counters().refused, 1u);

    // One pin let go is one frame back: the same fault now succeeds.
    held.pop_back();
    EXPECT_TRUE(store.value()->GetForRead(ids[kCapacity]).ok());
}

TEST(BoundedPoolTest, AWindowIsRefusedWhenThePoolCannotPromiseItsShare) {
    // BE-Q11: opening a window is the refusal point. A pool whose frames are
    // all dirty cannot promise a share, so the window is refused - with
    // nothing written - and a window it can promise always finds its slots.
    const WithoutFrameBudgetOverride exact_capacity;
    constexpr std::size_t kCapacity = 128;
    auto device = MemoryPageDevice::Create(/*extent_pages=*/64, /*initial_pages=*/0);
    ASSERT_TRUE(device.ok()) << device.status().message();
    auto store = DevicePageStore::Open(*device.value(), FrameCapacity{kCapacity}, 16);
    ASSERT_TRUE(store.ok()) << store.status().message();

    {
        // A share the pool can promise: every fill inside it succeeds, even
        // creations that leave the pool full of dirty frames.
        auto window = NoRefuseWindow::Open(*store.value(), kCapacity);
        ASSERT_TRUE(window.ok()) << window.status().message();
        for (std::size_t i = 0; i < kCapacity; ++i) {
            auto made = store.value()->CreateNew();
            ASSERT_TRUE(made.ok()) << "fill " << i << ": " << made.status().message();
            FormatPage(made.value().second.bytes(), PageType::kHeap);
        }
    }
    // Every frame is dirty now: no share can be promised.
    auto refused = NoRefuseWindow::Open(*store.value(), kWindowFrames);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kResourceExhausted);
    // And outside a window, a fill is refused the same way.
    EXPECT_EQ(store.value()->CreateNew().status().code(), StatusCode::kResourceExhausted);
    EXPECT_EQ(store.value()->frame_slots().slots, kCapacity);
}

TEST(BoundedPoolTest, ADrainHoldingPinsWritesBackTheQueueButNeverAPinnedFrame) {
    // DX-Q2 (a): a reservation in drain mode drains with pins held, and
    // needs no page of the caller's. A pool of dirty frames is queued for
    // writeback by a refused fill; then this thread write-fetches two of
    // the queued pages and changes them, as a rollback holding its leaf
    // would, and a creation in drain mode is served from the frames the
    // drain wrote clean - while the two held pages stay dirty and the device
    // never sees their new bytes. The first held page is the drain's first
    // claim, the second sits inside the run after it. Killed by:
    //   - the drain's old `t_pins_here == 0` gate: the creation is refused;
    //   - the drain in `kSkip`: with the latch unarmed the shared try
    //     admits the held pages and writes them mid-change;
    //   - the claim's pinned test: the first held page is written;
    //   - the run's pinned test: the second is written inside a run.
    const WithoutFrameBudgetOverride exact_capacity;
    constexpr std::size_t kCapacity = 64;
    auto device = MemoryPageDevice::Create(/*extent_pages=*/256, /*initial_pages=*/0);
    ASSERT_TRUE(device.ok()) << device.status().message();
    auto store = DevicePageStore::Open(*device.value(), FrameCapacity{kCapacity}, 16);
    ASSERT_TRUE(store.ok()) << store.status().message();

    std::vector<PageId> ids;
    {
        auto window = NoRefuseWindow::Open(*store.value(), kCapacity);
        ASSERT_TRUE(window.ok()) << window.status().message();
        for (std::size_t i = 0; i < kCapacity; ++i) {
            auto made = store.value()->CreateNew();
            ASSERT_TRUE(made.ok()) << "fill " << i << ": " << made.status().message();
            FormatPage(made.value().second.bytes(), PageType::kHeap);
            ids.push_back(made.value().first);
        }
    }
    // Outside drain mode the pool refuses, as BE-R4 says - and its walk
    // queues every dirty frame it passes.
    EXPECT_EQ(store.value()->CreateNew().status().code(), StatusCode::kResourceExhausted);
    ASSERT_GT(store.value()->pool_counters().dirty_queued, 0u);

    constexpr std::size_t kOffset = 4000;  // in the body, past every header field
    constexpr auto kMark = std::byte{0xAB};
    const std::vector<PageId> mine = {ids[0], ids[5]};
    std::vector<PageRef> held;
    for (const PageId id : mine) {
        auto ref = store.value()->Get(id);
        ASSERT_TRUE(ref.ok()) << ref.status().message();
        ref.value().bytes()[kOffset] = kMark;  // mid-change, as far as anyone can tell
        held.push_back(std::move(ref.value()));
    }
    ASSERT_EQ(PinsHeldOnThisThread(), 2u);
    const auto before = store.value()->pool_counters();
    {
        const DrainOnPressure drain(*store.value());
        auto made = store.value()->CreateNew();
        ASSERT_TRUE(made.ok()) << made.status().message();
    }
    const auto after = store.value()->pool_counters();
    EXPECT_GT(after.dirty_drained, before.dirty_drained);
    EXPECT_EQ(after.refused, before.refused);

    const std::vector<PageId> dirty = store.value()->DirtyPageIds();
    for (const PageId id : mine) {
        SCOPED_TRACE("held page " + std::to_string(id));
        EXPECT_NE(std::find(dirty.begin(), dirty.end(), id), dirty.end())
            << "a held page was written clean";
        std::array<std::byte, kPageSize> on_device{};
        if (device.value()->ReadPage(id, on_device).ok()) {
            EXPECT_NE(on_device[kOffset], kMark) << "the device holds a held page's change";
        }
    }
    held.clear();
    EXPECT_EQ(PinsHeldOnThisThread(), 0u);
}

TEST(BoundedPoolDeathTest, ARollbackEnteredHoldingAPinStopsTheInstance) {
    // DX-S1: every abort's entry asks `RequireNoPinsHeld`, in every build -
    // an abort entered mid-mutation would unwind under a page the caller is
    // still writing. A `ROLLBACK` unwinds the assertion reservations before
    // the trail (`RollbackLocal`), so one reached while this thread holds a
    // pin stops the process at `AssertionEnforcer::AbortTxn`. Killed by
    // dropping the call there (the next cell then names the one it reaches).
    EXPECT_DEATH(
        {
            auto created = sim::SimInstance::Create(sim::SimInstance::Options{});
            if (!created.ok()) std::abort();
            sim::SimInstance& db = *created.value();
            (void)db.Execute("CREATE TABLE t (id int64, v int64)");
            (void)db.Execute("BEGIN");
            (void)db.Execute("INSERT INTO t VALUES (1, 1)");
            auto pinned = db.store().GetForRead(0);  // page 0, the superblock
            if (!pinned.ok()) std::abort();
            (void)db.Execute("ROLLBACK");
        },
        "AssertionEnforcer::AbortTxn entered holding 1 page pin");
}

TEST(BoundedPoolDeathTest, ATransactionAbortEnteredHoldingAPinStopsTheInstance) {
    // The trail's own entry, reached without the enforcer in front of it (a
    // deferred commit's failed sync, a DDL build's lock scope). Killed by
    // dropping the call in `TransactionManager::Abort`.
    EXPECT_DEATH(
        {
            auto created = sim::SimInstance::Create(sim::SimInstance::Options{});
            if (!created.ok()) std::abort();
            sim::SimInstance& db = *created.value();
            auto begun = db.txn_manager().Begin(txn::IsolationLevel::kReadCommitted);
            if (!begun.ok()) std::abort();
            auto pinned = db.store().GetForRead(0);  // page 0, the superblock
            if (!pinned.ok()) std::abort();
            (void)db.txn_manager().Abort(*begun.value());
        },
        "TransactionManager::Abort entered holding 1 page pin");
}

// ---- BE-R5: the outermost read walk faults cold ---------------------------

class ColdScanTest : public ::testing::Test {
protected:
    const WithoutFrameBudgetOverride exact_capacity_;  // the capacity is the subject

    // `pages` formatted pages on the device, and a store of `capacity` over them.
    void Build(std::size_t pages, std::size_t capacity) {
        auto device = MemoryPageDevice::Create(/*extent_pages=*/256, /*initial_pages=*/0);
        ASSERT_TRUE(device.ok()) << device.status().message();
        device_ = std::move(device.value());
        {
            auto setup = DevicePageStore::Open(*device_, FrameCapacity{pages + 64}, 16);
            ASSERT_TRUE(setup.ok()) << setup.status().message();
            for (std::size_t i = 0; i < pages; ++i) {
                auto made = setup.value()->CreateNew();
                ASSERT_TRUE(made.ok()) << made.status().message();
                FormatPage(made.value().second.bytes(), PageType::kHeap);
                ids_.push_back(made.value().first);
            }
            ASSERT_TRUE(setup.value()->Sync().ok());
        }
        auto store = DevicePageStore::Open(*device_, FrameCapacity{capacity}, 16);
        ASSERT_TRUE(store.ok()) << store.status().message();
        store_ = std::move(store.value());
    }

    void Fetch(std::size_t i, PageAccess access) {
        auto ref = store_->Fetch(ids_[i], access);
        ASSERT_TRUE(ref.ok()) << ref.status().message();
    }

    bool Resident(std::size_t i) const { return store_->latch_word_for_test(ids_[i]).ok(); }

    std::unique_ptr<MemoryPageDevice> device_;
    std::unique_ptr<DevicePageStore> store_;
    std::vector<PageId> ids_;
};

TEST_F(ColdScanTest, AHotWorkingSetSurvivesAScanFourTimesThePool) {
    // A hot set of 32 pages read five times each, then a scan of four times
    // the 256-frame pool. Cold, the scan's pages are each the hand's next
    // victim, and the hot frames lose one usage point per lap - fewer laps
    // than their five. Warm, every scanned frame needs two of the hand's
    // passes to go, the laps double, and the hot set is reclaimed under the
    // scan: killed by `kScan` entering warm.
    constexpr std::size_t kCapacity = 256;
    constexpr std::size_t kHot = 32;
    Build(kHot + 4 * kCapacity, kCapacity);
    for (int touch = 0; touch < 5; ++touch) {
        for (std::size_t i = 0; i < kHot; ++i) Fetch(i, PageAccess::kRead);
    }
    for (std::size_t i = kHot; i < ids_.size(); ++i) Fetch(i, PageAccess::kScan);

    std::size_t hot_resident = 0;
    for (std::size_t i = 0; i < kHot; ++i) hot_resident += Resident(i);
    EXPECT_EQ(hot_resident, kHot) << "the scan displaced the working set";
    EXPECT_EQ(store_->frame_slots().slots, kCapacity);
}

TEST_F(ColdScanTest, AScannedPageIsTheFirstVictimAndAReadPageIsNot) {
    // The two heats side by side: a page `kScan` faulted enters at usage 0,
    // one `kRead` faulted at 1 - the shape a nested inner walk keeps, since
    // only the outermost walk passes `kScan`. One reclaim takes the first.
    // The warm page takes the first slot, so the hand meets it first: with
    // equal counters it would be the one to go.
    Build(2, 8);
    Fetch(1, PageAccess::kRead);
    Fetch(0, PageAccess::kScan);
    EXPECT_EQ(store_->EvictColdFrames(1), 1u);
    EXPECT_FALSE(Resident(0)) << "the cold page survived";
    EXPECT_TRUE(Resident(1)) << "the warm page went first";
}

TEST_F(ColdScanTest, AScannedPageItTouchesAgainWarmsLikeAnyHit) {
    // The other half of `kScan`: cold on the fault, a bump on the hit. Page 1
    // is scanned once, page 0 twice; one reclaim takes page 1, the only frame
    // left at zero. Killed by a `kScan` hit that does not bump - both would
    // sit at zero and the hand meets page 0 first.
    Build(2, 8);
    Fetch(0, PageAccess::kScan);
    Fetch(1, PageAccess::kScan);
    Fetch(0, PageAccess::kScan);
    EXPECT_EQ(store_->EvictColdFrames(1), 1u);
    EXPECT_TRUE(Resident(0)) << "a page the scan read twice was reclaimed first";
    EXPECT_FALSE(Resident(1));
}

TEST_F(ColdScanTest, ARepeatedRangeUnderThePoolHitsOnItsSecondPass) {
    // Cold on a fault, warm on a hit: a range scanned twice - xrock's per-day
    // probes - is served from the pool the second time, which the scan ring
    // this order declined (§1.6) would have re-faulted.
    Build(64, 256);
    for (std::size_t i = 0; i < ids_.size(); ++i) Fetch(i, PageAccess::kScan);
    device_->ResetStats();
    for (std::size_t i = 0; i < ids_.size(); ++i) Fetch(i, PageAccess::kScan);
    EXPECT_EQ(device_->stats().reads, 0u) << "the second pass re-read the device";
}

}  // namespace
}  // namespace kds::storage

namespace kds::sim {
namespace {

int CountRows(SimInstance& db) {
    const std::string reply = db.Execute("SELECT COUNT(*) FROM t");
    // A debug-text reply escapes its row separator as a backslash and an `n`.
    const auto newline = reply.find("\\n");
    if (newline == std::string::npos) return -1;
    return std::stoi(reply.substr(newline + 2));
}

TEST(BoundedPoolTest, AnInsertRefusedForAFullPoolLeavesNoRowBehind) {
    // BE-Q11 end to end, the census's worst site (`git show d83bd502:docs/inflight/bugs/a-fetch-
    // refused-after-a-page-write-leaves-the-mutation-half-done.md`): inside one
    // transaction, wide rows fill a small pool with dirty pages that nothing
    // checkpoints, until a row's window cannot be promised. That row is
    // refused before it writes anything, and the transaction rolls back to
    // the committed rows - live, and again after a reboot's recovery.
    const WithoutFrameBudgetOverride exact_capacity;
    SimInstance::Options options;
    options.buffer_pool_frames = 512;
    auto created = SimInstance::Create(options);
    ASSERT_TRUE(created.ok()) << created.status().message();
    SimInstance& db = *created.value();

    ASSERT_EQ(db.Execute("CREATE TABLE t (id int64, a char(2000), b char(2000), c char(2000))")
                  .rfind("ERR", 0),
              std::string::npos);
    for (int i = 0; i < 3; ++i) {
        const std::string reply = db.Execute("INSERT INTO t VALUES ('x', 'y', 'z')");
        ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
    }
    ASSERT_TRUE(db.RunCheckpoint().ok());

    ASSERT_EQ(db.Execute("BEGIN").rfind("ERR", 0), std::string::npos);
    std::string refusal;
    for (int i = 0; i < 4000 && refusal.empty(); ++i) {
        const std::string reply = db.Execute("INSERT INTO t VALUES ('x', 'y', 'z')");
        if (reply.rfind("ERR", 0) == 0) refusal = reply;
    }
    ASSERT_FALSE(refusal.empty()) << "the pool never filled";
    EXPECT_NE(refusal.find("buffer pool full"), std::string::npos) << refusal;

    ASSERT_EQ(db.Execute("ROLLBACK").rfind("ERR", 0), std::string::npos);
    EXPECT_EQ(CountRows(db), 3) << "a refused or rolled-back row survived";

    db.Crash();  // a power cut: recovery, not the live rollback, decides
    ASSERT_TRUE(db.Reboot().ok());
    EXPECT_EQ(CountRows(db), 3) << "recovery found a row the rollback did not undo";
}

TEST(BoundedPoolTest, ASelectLargerThanThePoolLeavesTheWorkingSetResident) {
    // BE-R5 at the executor: the outermost walk of a `SELECT` is the one that
    // faults cold (`RunWalkStep`). A small table read five times, then a
    // `COUNT(*)` over a table about twice the 512-frame pool, then the small
    // table again - served without one device read. Killed by the walk
    // passing `kRead`: the big scan's frames enter warm, the hand laps twice
    // as often, and the small table is reclaimed under it.
    const WithoutFrameBudgetOverride exact_capacity;
    SimInstance::Options options;
    options.buffer_pool_frames = 512;
    auto created = SimInstance::Create(options);
    ASSERT_TRUE(created.ok()) << created.status().message();
    SimInstance& db = *created.value();
    const auto ok = [&](const std::string& sql) {
        const std::string reply = db.Execute(sql);
        ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << sql << " -> " << reply;
    };

    ok("CREATE TABLE hot (id int64, a char(2000), b char(2000))");
    ok("CREATE TABLE big (id int64, a char(2000), b char(2000), c char(2000))");
    for (int i = 0; i < 16; ++i) ok("INSERT INTO hot VALUES ('h', 'h')");
    for (int i = 0; i < 2000; ++i) {
        ok("INSERT INTO big VALUES ('x', 'y', 'z')");
        if (i % 64 == 63) ASSERT_TRUE(db.RunCheckpoint().ok());  // the load's checkpoints
    }
    // A clean restart, so the pool holds no warm frames from the load: a
    // pool warmed by the load's own writes would have the hand lap them down
    // first, lowering the hot set with them - CLOCK's behaviour whatever the
    // scan's heat, and not the question here.
    ASSERT_TRUE(db.CleanShutdown().ok());
    ASSERT_TRUE(db.Reboot().ok());

    for (int touch = 0; touch < 5; ++touch) ok("SELECT * FROM hot");
    ok("SELECT COUNT(*) FROM big");
    db.page_device().ResetStats();
    ok("SELECT * FROM hot");
    EXPECT_EQ(db.page_device().stats().reads, 0u)
        << "the scan displaced the working set";
}

// ---- DX-R2: an abort's compensation is never refused a frame -------------
//
// One transaction updates rows over more pages than the pool holds: the
// first ones are written clean by a checkpoint mid-transaction and then
// evicted by the next ones, until an update is refused with the pool full
// of dirty frames. Undoing it then faults those first pages back in - into
// that full, dirty pool - and only drain mode's writeback makes room.
class PoolOfDirtyFramesTest : public ::testing::Test {
protected:
    static constexpr int kRows = 3000;  // about 1,000 leaves at three wide rows a leaf

    void SetUp() override {
        SimInstance::Options options;
        options.buffer_pool_frames = 512;
        auto created = SimInstance::Create(options);
        ASSERT_TRUE(created.ok()) << created.status().message();
        db_ = std::move(created.value());
        Ok("CREATE TABLE t (id int64, a char(2000), b char(2000))");
        for (int i = 1; i <= kRows; ++i) {
            Ok("INSERT INTO t VALUES (" + std::to_string(i) + ", 'x', 'y')");
            if (i % 64 == 0) ASSERT_TRUE(db_->RunCheckpoint().ok());
        }
        ASSERT_TRUE(db_->RunCheckpoint().ok());
        ASSERT_EQ(CountOld(), kRows);
    }

    // Opens the transaction and updates until the pool refuses.
    void FillOneTransactionPastThePool() {
        Ok("BEGIN");
        // The first part, then a checkpoint: its pages are clean and evictable.
        int id = 1;
        for (; id <= 200; ++id) Ok("UPDATE t SET a = 'n' WHERE id = " + std::to_string(id));
        ASSERT_TRUE(db_->RunCheckpoint().ok());
        // The rest, until the pool is full of this transaction's dirty pages.
        std::string refusal;
        for (; id <= kRows && refusal.empty(); ++id) {
            const std::string reply =
                db_->Execute("UPDATE t SET a = 'n' WHERE id = " + std::to_string(id));
            if (reply.rfind("ERR", 0) == 0) refusal = reply;
        }
        ASSERT_FALSE(refusal.empty()) << "the pool never filled with dirty frames";
        EXPECT_NE(refusal.find("buffer pool full"), std::string::npos) << refusal;
    }

    void Ok(const std::string& sql) {
        const std::string reply = db_->Execute(sql);
        ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << sql << " -> " << reply;
    }

    // Rows still carrying the value every update replaced.
    int CountOld() {
        const std::string reply = db_->Execute("SELECT COUNT(*) FROM t WHERE a = 'x'");
        const auto newline = reply.find("\\n");
        return newline == std::string::npos ? -1 : std::stoi(reply.substr(newline + 2));
    }

    const WithoutFrameBudgetOverride exact_capacity_;
    std::unique_ptr<SimInstance> db_;
};

TEST_F(PoolOfDirtyFramesTest, ARollbackFaultingIntoItUndoesEveryRow) {
    // The live rollback: every row reads as it was, live and after a
    // reboot. Killed by dropping `Abort`'s drain mode: a compensation is
    // refused and the rollback leaves a row updated.
    ASSERT_NO_FATAL_FAILURE(FillOneTransactionPastThePool());
    const auto before = db_->store().pool_counters();
    Ok("ROLLBACK");
    const auto after = db_->store().pool_counters();
    EXPECT_EQ(after.refused, before.refused) << "a compensation was refused a frame";
    EXPECT_GT(after.dirty_drained, before.dirty_drained)
        << "the rollback met no full pool: the cell does not reach drain mode";
    EXPECT_EQ(CountOld(), kRows) << "the rollback left a row updated";

    db_->Crash();  // recovery decides what the rollback's records left
    const Status rebooted = db_->Reboot();
    ASSERT_TRUE(rebooted.ok()) << rebooted.message();
    EXPECT_EQ(CountOld(), kRows) << "recovery found a row the rollback did not undo";
}

TEST_F(PoolOfDirtyFramesTest, AMountUndoingTheLoserInItUndoesEveryRow) {
    // BE's untested rule, drain mode under recovery at the cap (DX-Q5 (a)):
    // the same transaction left open by a power cut is the mount's loser,
    // and its undo meets a pool the redo filled. Killed by dropping the
    // mount's drain mode (`expeditor.cpp`'s `mount_drain`): the undo is
    // refused and the mount fails.
    ASSERT_NO_FATAL_FAILURE(FillOneTransactionPastThePool());
    ASSERT_TRUE(db_->RunCheckpoint().ok());  // the loser's records durable, its pages on the device
    db_->Crash();
    const Status rebooted = db_->Reboot();
    ASSERT_TRUE(rebooted.ok()) << rebooted.message();
    EXPECT_GT(db_->store().pool_counters().dirty_drained, 0u)
        << "the mount met no full pool: the cell does not reach drain mode";
    EXPECT_EQ(CountOld(), kRows) << "the mount's undo left a row updated";
}

}  // namespace
}  // namespace kds::sim
