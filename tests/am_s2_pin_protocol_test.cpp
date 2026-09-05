// AM-S2 step 1: the frame table's structure latch, under concurrent pins.
//
// **This is the cell `page_latch_test.cpp`'s eight-thread case says could
// not be written.** Its comment reads: "AM-S1's contention cell at 8 cores,
// on the primitive: the store's frame table is not thread-safe and cannot
// host it, so the words stand in for frames." Step 1 gives the table a
// structure latch and moves the pin accounting under it, so the frames
// themselves can host the contention now, and the stand-in is no longer
// what is being tested.

#include <atomic>
#include <cstddef>
#include <random>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_header.hpp"

namespace kds::storage {
namespace {

class PinProtocolTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto device = MemoryPageDevice::Create(/*extent_pages=*/64, /*initial_pages=*/0);
        ASSERT_TRUE(device.ok()) << device.status().message();
        device_ = std::move(device.value());
        auto store = DevicePageStore::Open(*device_, /*first_new_page_id=*/16);
        ASSERT_TRUE(store.ok()) << store.status().message();
        store_ = std::move(store.value());
        // Armed, or none of this is under test: unarmed the structure latch
        // is a null pointer and the guard is two branches (base/latch.hpp).
        //
        // **And the pin ceiling is scaled for the pinners.** `kPinCeiling`
        // bounds *one operation's* pin stack, and `live_pins_` was a proxy
        // for it only while one core ran one operation. Left unscaled this
        // cell sits exactly *at* the bound - eight threads, one handle each
        // - so a ninth thread, or any one of them holding two handles as a
        // btree descent does, aborts the process on entirely correct
        // traffic. Measured, not reasoned: at nine it aborts.
        store_->SetLatchArmed(true, /*concurrent_pinners=*/12);
    }

    PageId MakeResidentPage(std::byte fill) {
        auto created = store_->CreateNew();
        EXPECT_TRUE(created.ok()) << created.status().message();
        const PageId id = created.value().first;
        FormatPage(created.value().second.bytes(), PageType::kHeap);
        created.value().second.bytes()[kPageBodyOffset] = fill;
        EXPECT_TRUE(store_->Sync().ok());
        return id;
    }

    std::unique_ptr<MemoryPageDevice> device_;
    std::unique_ptr<DevicePageStore> store_;
};

TEST_F(PinProtocolTest, ManyThreadsPinOneStoreAndTheLivePinGaugeReturnsToZero) {
    // **What this discriminates, established by mutation rather than
    // claimed.** An earlier version of this comment said it carried the
    // pin-before-page-latch *ordering* - "hold either across the other and
    // this deadlocks". Both halves are false, and the mutations say so:
    // taking the pin *after* the page latch passes, and holding the
    // structure latch across the page-latch acquire passes. It cannot
    // deadlock, structurally, because this cell takes **shared holds only**
    // and a shared acquire never blocks against other shared holders - so
    // there is nothing being waited for that could be held across.
    //
    // What it does carry: the pin counters are under *some* mutual
    // exclusion. Remove the structure latch and it fails on its own
    // assertions - `live_pins` lands in the dozens and frames stay pinned.
    // That is real and worth having, and it is all this is.
    //
    // **An ordering cell needs what this lacks**: at least one exclusive
    // acquirer per page, so shared waiters actually block, and a concurrent
    // eviction path, so the pin's protection is load-bearing rather than
    // incidental. That belongs with step 2, which gives it a fault path to
    // race against.
    constexpr int kThreads = 12;
    constexpr int kPages = 16;
    constexpr int kTurnsPerThread = 3000;

    std::vector<PageId> pages;
    pages.reserve(kPages);
    for (int i = 0; i < kPages; ++i) {
        pages.push_back(MakeResidentPage(static_cast<std::byte>(i)));
    }

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(static_cast<std::uint32_t>(7000 + t));
            std::uniform_int_distribution<int> pick(0, kPages - 1);
            for (int turn = 0; turn < kTurnsPerThread; ++turn) {
                const PageId id = pages[static_cast<std::size_t>(pick(rng))];
                // **Shared holds only.** Several threads holding one page
                // exclusive is the page latch's own subject, covered by the
                // primitive's cell; what is under test here is that the
                // *table* survives concurrent pin accounting.
                auto ref = store_->GetForRead(id);
                if (!ref.ok()) {
                    ++failures;
                    continue;
                }
                // Touch the bytes, so a frame is genuinely in use while
                // other threads pin and unpin around it.
                if (ref.value().bytes()[kPageBodyOffset] == std::byte{0xFF}) ++failures;
            }
        });
    }
    for (std::thread& thread : threads) thread.join();

    EXPECT_EQ(failures.load(), 0);
    // Every handle was released, so the gauge is back to zero.
    //
    // **What this does and does not discriminate**, checked rather than
    // claimed: splitting `UnpinFrame`'s pin and gauge decrements apart
    // leaves this cell **passing**, because the early return there already
    // filters `pins == 0` and the racing window needs two unpins at
    // `pins == 1` - which cannot happen, since one pin means one handle.
    // The coupling is kept because it is the right semantics, not because
    // this catches it.
    //
    // **Twelve threads, deliberately past the old bound.** At eight this
    // cell sat exactly on the unscaled `kPinCeiling`, so it passed only by
    // arithmetic coincidence and a ninth reader aborted the process. Running
    // it above the per-operation number is what keeps the scaling honest.
    EXPECT_EQ(store_->live_pins(), 0u)
        << "the live-pin gauge did not return to zero after every handle dropped";
    EXPECT_EQ(store_->pinned_frames(), 0u) << "a frame was left pinned after every handle dropped";
}

}  // namespace
}  // namespace kds::storage
