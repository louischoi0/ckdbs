#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"

// **Page allocation with more than one thread in it** (AM-S2).
//
// This is the cell the shared buffer pool cannot land without, and it is
// deliberately about the *store* rather than about an instance: the pool
// becomes shared at step 3c, and from that moment two reactor threads reach
// `CreateNew()` on one object. Until then nothing in the tree does, which is
// why the allocator has never had a latch and why no existing cell says so.
//
// **What it measures, and why an id is the right thing to count.** The
// failure is not two frames for one id - `InsertFrame` hands a loser the
// winner's frame, so the table stays consistent while two *logical* pages, a
// btree leaf and a heap chain page say, believe they own one 8 KiB buffer and
// one device page. So the assertion is on the ids handed out: every caller
// must get one nobody else got.
//
// Measured before the fix, four threads x 400 creates on one unleased armed
// store: 218 duplicates out of 1600, then 221, 230, 235, 229, 237 across six
// runs. The cause is structural rather than a narrow window -
// `FreeMapFindFirstFree` chose the id and `FreeMapAllocate` marked it two
// calls away in another function, so any number of threads could scan the
// same clear bit before one of them set it.
//
// **Unleased on purpose.** A leased store takes ids from a run core 0
// reserved for it and touches no shared state, which is exactly the
// arrangement the shared pool removes (`storage/extent_lease.hpp`: leases
// exist because "per-core page stores do not work without it"). The unleased
// path is core 0's today and every core's after step 3c.

namespace kds::storage {
namespace {

TEST(AllocRaceTest, ConcurrentCreatesNeverHandTwoCallersTheSameId) {
    auto device = MemoryPageDevice::Create(/*extent_pages=*/512, /*initial_pages=*/0);
    ASSERT_TRUE(device.ok()) << device.status().message();
    auto store = DevicePageStore::Open(*device.value(), /*first_new_page_id=*/16);
    ASSERT_TRUE(store.ok()) << store.status().message();
    // Armed, because an unarmed store's `LatchGuard` is a null test: the
    // question here only exists where the store is shared, and a store is
    // only shared where it is armed.
    store.value()->SetLatchArmed(true, /*concurrent_pinners=*/8);
    ASSERT_TRUE(store.value()->latch_armed());

    constexpr int kThreads = 4;
    constexpr int kPerThread = 400;
    std::vector<std::vector<PageId>> handed(kThreads);
    std::atomic<int> ready{0};
    std::atomic<int> failures{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            // Each thread is a core, as it will be once the pool is shared.
            SetCurrentCore(static_cast<std::uint32_t>(t));
            handed[t].reserve(kPerThread);
            // Every thread starts scanning at the same moment, which is what
            // makes them contend for the same clear bit rather than trail
            // one another up the map.
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) < kThreads) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kPerThread; ++i) {
                auto made = store.value()->CreateNew();
                if (!made.ok()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                handed[t].push_back(made.value().first);
                // The handle goes back immediately: this cell is about the
                // id, and holding 1600 pins would trip the debug ceiling
                // rather than say anything about allocation.
                made.value().second.Release();
            }
        });
    }
    for (std::thread& th : threads) th.join();

    std::unordered_set<PageId> unique;
    std::size_t total = 0;
    for (const std::vector<PageId>& ids : handed) {
        total += ids.size();
        unique.insert(ids.begin(), ids.end());
    }
    EXPECT_EQ(failures.load(), 0) << "allocation refused a caller outright";
    EXPECT_EQ(unique.size(), total)
        << "two callers were handed the same page id: " << total << " allocated, "
        << unique.size() << " distinct, " << (total - unique.size()) << " duplicated";
}

}  // namespace
}  // namespace kds::storage
