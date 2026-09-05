// AM-S2 step 1: the frame table's structure latch, under concurrent pins.
//
// **This is the cell `page_latch_test.cpp`'s eight-thread case says could
// not be written.** Its comment reads: "AM-S1's contention cell at 8 cores,
// on the primitive: the store's frame table is not thread-safe and cannot
// host it, so the words stand in for frames." Step 1 gives the table a
// structure latch and moves the pin accounting under it, so the frames
// themselves can host the contention now, and the stand-in is no longer
// what is being tested.

#include <array>
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
        // **A budget, so the inline sweep actually runs** (AM-S2). The
        // fault path sweeps only when a miss takes the pool past
        // `frame_budget_` (EV5/MG06), and the review found this fixture left
        // it at 0 - so the cell written to show the sweep moved out from
        // under the structure latch never executed a sweep at all, and the
        // hit path's `loading_` test, which is what keeps a second thread
        // off a frame the loader is still inside `ResidentBytes` for, could
        // be deleted with every cell still passing. Small enough that the
        // rounds below cross it repeatedly.
        store_->SetFrameBudget(8);
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


TEST_F(PinProtocolTest, ConcurrentMissesOnOnePageIssueOneDeviceRead) {
    // **Step 2b's loading set, and the one property of it that can be
    // asserted deterministically here.** A miss records the page id, drops
    // the structure latch, reads outside it, then re-takes it to publish. A
    // second core missing the same page finds the id in flight and waits,
    // rather than issuing its own read whose `InsertFrame` would race the
    // first.
    //
    // The device trace is what makes that checkable: one `kRead` per round,
    // however many threads fault the page in that round.
    //
    // **Rounds and a reusable barrier, because neither is optional here.**
    // The first version of this cell spawned eight threads in a loop and
    // asserted one read. It passed 40/40 with the entire wait arm compiled
    // out, because `std::thread` construction costs far more than a
    // `MemoryPageDevice` read and the first faulter was finished before the
    // second was running - so it raced nothing and discriminated nothing. A
    // start barrier alone raises the catch rate to only one run in ten: the
    // window is a memcpy and a CRC, microseconds wide. Many rounds is what
    // converts a flaky detector into a reliable one - with the wait arm
    // removed this fails on essentially every run, and with it present the
    // count is exactly `kRounds`.
    const PageId id = MakeResidentPage(std::byte{0x5A});

    constexpr int kFaulters = 8;
    constexpr int kRounds = 200;
    const std::array<PageId, 1> victims{id};

    std::atomic<int> failures{0};
    // Counted, never asserted inside the loop: a bail-out mid-round leaves
    // eight threads parked on a barrier nobody will release, and the test
    // binary aborts instead of reporting. Every round is driven to the end
    // and the counts are read after the join.
    int evict_failures = 0;
    // The barrier: `arrived` counts faulters parked before a round, `round`
    // is the generation they wait to see, `finished` counts them out again
    // so the next eviction cannot run while a `PageRef` is still alive.
    std::atomic<int> arrived{0};
    std::atomic<int> round{0};
    std::atomic<int> finished{0};

    std::vector<std::thread> threads;
    threads.reserve(kFaulters);
    for (int t = 0; t < kFaulters; ++t) {
        threads.emplace_back([&] {
            for (int r = 0; r < kRounds; ++r) {
                ++arrived;
                while (round.load(std::memory_order_acquire) != r + 1) std::this_thread::yield();
                {
                    auto ref = store_->GetForRead(id);
                    if (!ref.ok() || ref.value().bytes()[kPageBodyOffset] != std::byte{0x5A}) {
                        ++failures;
                    }
                }  // the handle drops here: an eviction between rounds must
                   // find the frame unpinned.
                ++finished;
            }
        });
    }

    int reads = 0;
    for (int r = 0; r < kRounds; ++r) {
        while (arrived.load() != kFaulters * (r + 1)) std::this_thread::yield();
        // Evicted with every faulter parked, so the next round is a genuine
        // miss for all of them. `EvictClean` is the path with no dirty bytes
        // to write back, which is what a freshly synced page is.
        if (!store_->EvictClean(victims).ok()) ++evict_failures;
        device_->ClearTrace();
        round.store(r + 1, std::memory_order_release);
        while (finished.load() != kFaulters * (r + 1)) std::this_thread::yield();
        for (const MemoryPageDevice::TraceEntry& entry : device_->trace()) {
            if (entry.kind == MemoryPageDevice::OpKind::kRead && entry.first_page_id == id) {
                ++reads;
            }
        }
    }
    for (std::thread& thread : threads) thread.join();

    EXPECT_EQ(failures.load(), 0);
    // A refused eviction is the loading set's absence showing up as damage
    // rather than as a count: two concurrent loads of one page both reach
    // `InsertFrame`, whose `insert_or_assign` replaces the `Frame` - latch
    // word, pin count and all - under a thread that is holding it.
    EXPECT_EQ(evict_failures, 0)
        << "a round could not evict the page between faults; a frame was left pinned or latched";
    EXPECT_EQ(reads, kRounds) << "eight concurrent faults per round over " << kRounds
                              << " rounds issued " << reads
                              << " device reads; the loading set is what makes that one each";

    // **What this cell does not assert**, stated rather than left to be
    // discovered: that a hit on a *different* page proceeds while this read
    // is in flight - which is the actual reason step 2b exists. Showing it
    // needs a device that can be made slow on demand, and `MemoryPageDevice`
    // has fault injection but no delay hook. The count above would still be
    // one per round with the latch held across the read, because the other
    // seven would then block on the latch and find the page resident. So
    // this pins the duplicate-read half of the loading set and not the
    // no-blocking half.
}

}  // namespace
}  // namespace kds::storage
