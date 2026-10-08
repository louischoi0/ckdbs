#include <cstddef>
#include <memory>
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
    // BE-Q11 end to end, the census's worst site (`docs/inflight/bugs/a-fetch-
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

}  // namespace
}  // namespace kds::sim
