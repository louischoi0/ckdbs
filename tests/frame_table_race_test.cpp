#include <atomic>
#include <chrono>
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
    auto store = DevicePageStore::Open(*device.value(), ::kds::storage::FrameCapacity{4096}, /*first_new_page_id=*/16);
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

// **The collecting readers, whose walk is the whole table** (AM-S2 step 3f).
// `Flush` reserves `frames_.size()` and then iterates every entry, so a
// concurrent insert can rehash under the iterator - and unlike `StampPageLsn`
// there is no id to come back missing, only an iterator into a freed bucket
// array. Survival is the assertion, which is what this tree already says of
// an ordering carried structurally (`am_s2_pin_protocol_test.cpp`).
TEST(FrameTableRaceTest, FlushingSurvivesConcurrentGrowthOfTheTable) {
    auto device = MemoryPageDevice::Create(/*extent_pages=*/512, /*initial_pages=*/0);
    ASSERT_TRUE(device.ok()) << device.status().message();
    auto store = DevicePageStore::Open(*device.value(), ::kds::storage::FrameCapacity{4096}, /*first_new_page_id=*/16);
    ASSERT_TRUE(store.ok()) << store.status().message();
    store.value()->SetLatchArmed(true, /*concurrent_pinners=*/8);

    // **Sized against the flusher, not the inserters.** Each pass writes
    // every dirty frame, so a table of N pages costs O(N) per flush and the
    // pair is quadratic; what the cell needs is only that a walk overlaps a
    // growth, which a few dozen passes over a growing table gives.
    constexpr int kInserters = 3;
    constexpr int kRounds = 600;
    constexpr int kFlushPasses = 40;
    std::atomic<int> ready{0};
    std::atomic<int> flushes{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kInserters; ++t) {
        threads.emplace_back([&, t] {
            SetCurrentCore(static_cast<std::uint32_t>(t));
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) < kInserters + 1) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kRounds; ++i) {
                auto made = store.value()->CreateNew();
                if (!made.ok()) continue;
                FormatPage(made.value().second.bytes(), PageType::kHeap);
                made.value().second.Release();
            }
        });
    }
    threads.emplace_back([&] {
        SetCurrentCore(kInserters);
        ready.fetch_add(1, std::memory_order_acq_rel);
        while (ready.load(std::memory_order_acquire) < kInserters + 1) {
            std::this_thread::yield();
        }
        for (int pass = 0; pass < kFlushPasses; ++pass) {
            if (store.value()->Flush().ok()) flushes.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (std::thread& th : threads) th.join();

    EXPECT_GT(flushes.load(), 0) << "the flusher never completed a pass";
    // Nothing is left dirty that the last pass should have taken: the
    // inserters are joined, so a final flush has the table to itself.
    ASSERT_TRUE(store.value()->Flush().ok());
    EXPECT_TRUE(store.value()->DirtyPageIds().empty())
        << "a dirty page survived a quiescent flush, so a walk lost an entry";
}

// **The armed miss path, whose device read runs with the latch released.**
// It used to reach the table through `Resolve`, whose `find` then ran
// unlatched over a table another core's `InsertFrame` was growing. Faulters
// re-read clean pages an evictor keeps dropping while inserters grow the
// table; survival and every read answering its own page are the assertion.
//
// **A guard, not a reproduction**: unlike the stamping cell, the unlatched
// code passed this 30 of 30 runs (and at three times the load), since the
// window is one `find` against a rehash. What closes the race is the code,
// which no longer reaches the table there except under holds of its own.
TEST(FrameTableRaceTest, FaultingSurvivesConcurrentGrowthOfTheTable) {
    auto device = MemoryPageDevice::Create(/*extent_pages=*/512, /*initial_pages=*/0);
    ASSERT_TRUE(device.ok()) << device.status().message();
    auto store = DevicePageStore::Open(*device.value(), ::kds::storage::FrameCapacity{8192},
                                       /*first_new_page_id=*/16);
    ASSERT_TRUE(store.ok()) << store.status().message();
    store.value()->SetLatchArmed(true, /*concurrent_pinners=*/8);

    // Written to the device and clean, so the evictor can drop them and a
    // read faults them back.
    std::vector<PageId> seeded;
    for (int i = 0; i < 256; ++i) {
        auto made = store.value()->CreateNew();
        ASSERT_TRUE(made.ok()) << made.status().message();
        // The page's own id as its owner, so a read can tell it got its page.
        FormatPage(made.value().second.bytes(), PageType::kHeap, /*flags=*/0,
                   /*owner_oid=*/made.value().first);
        seeded.push_back(made.value().first);
        made.value().second.Release();
    }
    ASSERT_TRUE(store.value()->Flush().ok());

    constexpr int kInserters = 2;
    constexpr int kFaulters = 3;
    constexpr int kRounds = 1000;
    constexpr int kThreads = kInserters + kFaulters + 1;
    std::atomic<int> ready{0};
    std::atomic<int> faulters_done{0};
    std::atomic<int> wrong{0};
    std::atomic<std::size_t> evicted{0};
    std::vector<std::thread> threads;
    const auto start = [&](int core) {
        SetCurrentCore(static_cast<std::uint32_t>(core));
        ready.fetch_add(1, std::memory_order_acq_rel);
        while (ready.load(std::memory_order_acquire) < kThreads) std::this_thread::yield();
    };

    for (int t = 0; t < kInserters; ++t) {
        threads.emplace_back([&, t] {
            start(t);
            for (int i = 0; i < kRounds; ++i) {
                auto made = store.value()->CreateNew();
                if (!made.ok()) continue;
                made.value().second.Release();
            }
        });
    }
    for (int t = 0; t < kFaulters; ++t) {
        threads.emplace_back([&, t] {
            start(kInserters + t);
            for (int i = 0; i < kRounds; ++i) {
                const PageId id = seeded[static_cast<std::size_t>(i * kFaulters + t) % seeded.size()];
                auto read = store.value()->GetForRead(id);
                if (!read.ok() || GetOwnerOid(read.value().bytes()) != id) {
                    wrong.fetch_add(1, std::memory_order_relaxed);
                }
            }
            faulters_done.fetch_add(1, std::memory_order_acq_rel);
        });
    }
    threads.emplace_back([&] {
        start(kInserters + kFaulters);
        // At least one sweep, however fast the faulters finish. Paced: a
        // sweep that finds too few clean frames walks its whole step bound
        // under the latch, and one re-taken at once starves every other
        // thread of it.
        do {
            evicted.fetch_add(store.value()->EvictColdFrames(/*budget=*/16),
                              std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        } while (faulters_done.load(std::memory_order_acquire) < kFaulters);
    });
    for (std::thread& th : threads) th.join();

    EXPECT_EQ(wrong.load(), 0) << "a fault answered with an error or another page's bytes";
    EXPECT_GT(evicted.load(), 0u) << "nothing was evicted, so no read faulted";
}

}  // namespace
}  // namespace kds::storage
