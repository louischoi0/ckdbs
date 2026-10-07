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

}  // namespace
}  // namespace kds::sim
