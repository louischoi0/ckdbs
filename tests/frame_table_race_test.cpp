#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/wal/record.hpp"

// **The frame table with an inserter and a reader in it at once** (AM-S2).
//
// `device_page_store.hpp`'s structure-latch block names the table's
// non-eraser readers and says step 3 takes them one at a time. This is the
// cell that says whether a given one has been taken: a thread growing the
// table while another walks it is undefined, and the shape it takes in
// practice is a crash rather than a wrong answer, so nothing in a
// single-threaded suite can see it.
//
// **`StampPageLsn` is first because of where it sits**: on every logged page
// mutation on every core. Measured before it took the latch, two inserters
// against two stampers **segfaulted in 3 runs of 5**; with the latch, 8 of 8
// clean on the same workload.
//
// The assertion is survival plus a consistent count, and that is honest
// rather than weak: a torn read of an `unordered_map` mid-rehash does not
// return a wrong page, it dereferences a bucket array that has been freed.
// A cell that finishes is the evidence, exactly as
// `am_s2_pin_protocol_test.cpp` says of the latch ordering it carries
// structurally.

namespace kds::storage {
namespace {

TEST(FrameTableRaceTest, StampingSurvivesConcurrentGrowthOfTheTable) {
    auto device = MemoryPageDevice::Create(/*extent_pages=*/512, /*initial_pages=*/0);
    ASSERT_TRUE(device.ok()) << device.status().message();
    auto store = DevicePageStore::Open(*device.value(), /*first_new_page_id=*/16);
    ASSERT_TRUE(store.ok()) << store.status().message();
    store.value()->SetLatchArmed(true, /*concurrent_pinners=*/8);
    ASSERT_TRUE(store.value()->latch_armed());

    // A seed set of pages for the stampers to aim at, made before anything
    // races so every stamp names a page that is certainly resident.
    std::vector<PageId> seeded;
    for (int i = 0; i < 64; ++i) {
        auto made = store.value()->CreateNew();
        ASSERT_TRUE(made.ok()) << made.status().message();
        FormatPage(made.value().second.bytes(), PageType::kHeap);
        seeded.push_back(made.value().first);
        made.value().second.Release();
    }

    constexpr int kInserters = 3;
    constexpr int kStampers = 3;
    constexpr int kRounds = 4000;
    std::atomic<int> ready{0};
    std::atomic<int> stamped{0};
    std::atomic<int> created{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kInserters; ++t) {
        threads.emplace_back([&, t] {
            SetCurrentCore(static_cast<std::uint32_t>(t));
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) < kInserters + kStampers) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kRounds; ++i) {
                auto made = store.value()->CreateNew();
                if (!made.ok()) continue;
                made.value().second.Release();
                created.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int t = 0; t < kStampers; ++t) {
        threads.emplace_back([&, t] {
            SetCurrentCore(static_cast<std::uint32_t>(kInserters + t));
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) < kInserters + kStampers) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kRounds; ++i) {
                const PageId id = seeded[static_cast<std::size_t>(i) % seeded.size()];
                // A monotonically rising LSN, because `StampPageLsn` keeps
                // the *oldest* recLSN and a repeated one would exercise
                // nothing after the first round.
                if (store.value()->StampPageLsn(id, static_cast<std::uint64_t>(i) + 1).ok()) {
                    stamped.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& th : threads) th.join();

    EXPECT_EQ(stamped.load(), kStampers * kRounds) << "a stamp failed on a resident page";
    EXPECT_GT(created.load(), 0);
    // Every page that was created is still in the table, plus the seeds and
    // the map pages. A count below what was handed out would mean the table
    // lost an entry to a concurrent write.
    EXPECT_GE(store.value()->resident_pages(),
              static_cast<std::size_t>(created.load()) + seeded.size());
}

}  // namespace
}  // namespace kds::storage
