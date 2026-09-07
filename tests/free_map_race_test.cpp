#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/status.hpp"
#include "kds/base/current_core.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/free_map.hpp"
#include "kds/storage/memory_page_device.hpp"

// **The free map with more than one thread in it** (AM-S3).
//
// `alloc_race_test.cpp` is this file's sibling and the precedent: it asked
// whether two callers can be handed one page id, which is a question about
// the *bits*. This one asks about the **structure that holds the bits** -
// `map_regions_`, a `std::map` that region creation inserts into while
// every other core is inside `find` from `IsAllocated`, which sits on the
// fault path, the writeback path and the WAL gate.
//
// **What it looked like before the fix.** Region creation ran wholly
// unlatched (`EnsureRegionResident`'s `emplace`), and the public readers
// took nothing, so this is a `std::map::find` racing a `std::map::insert` -
// undefined behaviour rather than a stale read, and the same shape AM-S2
// step 3c-ii measured on the frame table as *"a segfault, not a slow
// path"*. Measured here before the fix: **4 of 5 runs died**, three on a
// segfault inside `std::_Rb_tree_increment` and one on an assertion in the
// bitmap read, and the fifth reported 214 of 4000 reads answering
// *unallocated* for an id the creating thread had already been handed.
// That last one is the reason a crash is not the only thing asserted: the
// benign-looking outcome is a wrong answer on the path that decides whether
// a page may be written back.
//
// **Why the ids are so far apart.** A region covers `kFreeMapBitsPerPage`
// (65280) ids, so reaching a second one means naming an id past it. That
// is free here: `MemoryPageDevice` stores pages in an `unordered_map`, so
// capacity is a number and only the pages actually written cost anything.

namespace kds::storage {
namespace {

// 64 regions rather than a handful, and they are *published* one at a
// time: the discriminating power of this cell is how much of the run has
// an insertion in flight while readers are inside `find`, and four
// insertions at the start of a 3 ms run is none. Measured both ways - see
// the mutation note on the cell.
constexpr int kRegions = 64;
constexpr int kWriters = 4;
constexpr int kPerRegion = 6;

// The nth id this test places in `region` - deliberately past the two
// bitmap ids at the region base, which allocation skips by arithmetic.
PageId IdIn(int region, int n) {
    return FreeMapRegionBase(static_cast<std::uint32_t>(region)) + 8 + static_cast<PageId>(n);
}

std::unique_ptr<DevicePageStore> ArmedStore(std::unique_ptr<MemoryPageDevice>& device) {
    auto made = MemoryPageDevice::Create(/*extent_pages=*/512, /*initial_pages=*/0);
    EXPECT_TRUE(made.ok()) << made.status().message();
    device = std::move(made.value());
    auto store = DevicePageStore::Open(*device, /*first_new_page_id=*/16);
    EXPECT_TRUE(store.ok()) << store.status().message();
    // Armed, for `alloc_race_test.cpp`'s reason: the question exists only
    // where the store is shared, and a store is only shared where it is
    // armed.
    store.value()->SetLatchArmed(true, /*concurrent_pinners=*/16);
    return std::move(store.value());
}

// **Creation racing reads, which is the defect itself.** Four writers each
// own sixteen regions and place pages in them, so a `map_regions_`
// insertion is in flight for most of the run, while four readers ask
// `IsAllocated` and `IsHeaderless` about regions that have already been
// *published*.
//
// **The assertion is a wrong answer, not a crash.** A reader only ever asks
// about a region whose writer has finished it and set its flag, so
// `IsAllocated` must answer true for every id in it - there is no benign
// reading. Under the race a reader's `find` runs inside another thread's
// `insert`, and the outcome that is not a crash is the tree walk missing
// the node: `free_map_bytes_for` then hands back `AbsentRegionPage`, whose
// bits are all clear, and the store reports a page it created moments ago
// as unallocated. That is the answer the writeback path and the WAL gate
// both read.
TEST(FreeMapRaceTest, RegionCreationAndReadsAgreeUnderConcurrency) {
    std::unique_ptr<MemoryPageDevice> device;
    auto store = ArmedStore(device);
    ASSERT_NE(store, nullptr);

    std::vector<std::vector<PageId>> placed(kRegions);
    std::vector<std::atomic<bool>> published(kRegions);
    for (std::atomic<bool>& flag : published) flag.store(false, std::memory_order_relaxed);

    std::atomic<int> ready{0};
    std::atomic<int> create_failures{0};
    std::atomic<int> wrong_answers{0};
    std::atomic<int> writers_left{kWriters};

    const int kThreads = kWriters * 2;
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(kThreads));

    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&, w] {
            SetCurrentCore(static_cast<std::uint32_t>(w));
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) < kThreads) {
                std::this_thread::yield();
            }
            // Strided, so the four writers are inserting into *different*
            // parts of the tree at once rather than walking it together.
            for (int r = w; r < kRegions; r += kWriters) {
                for (int n = 0; n < kPerRegion; ++n) {
                    const PageId id = IdIn(r, n);
                    auto made = store->CreateAt(id);
                    if (!made.ok()) {
                        create_failures.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    placed[r].push_back(id);
                    // Returned at once: this cell is about the map, and
                    // holding every handle would trip the debug pin ceiling
                    // instead of saying anything about it.
                    made.value().Release();
                }
                // Release, so a reader that sees the flag sees the writes.
                published[r].store(true, std::memory_order_release);
            }
            writers_left.fetch_sub(1, std::memory_order_acq_rel);
        });
    }

    for (int t = 0; t < kWriters; ++t) {
        threads.emplace_back([&, t] {
            SetCurrentCore(static_cast<std::uint32_t>(kWriters + t));
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) < kThreads) {
                std::this_thread::yield();
            }
            // Until every writer is done, not until the first: a reader that
            // stops at the first writer's exit spends the rest of the run
            // not reading, which is how this cell first failed to
            // discriminate anything at all.
            while (writers_left.load(std::memory_order_acquire) > 0) {
                for (int r = 0; r < kRegions; ++r) {
                    if (!published[r].load(std::memory_order_acquire)) continue;
                    for (int n = 0; n < kPerRegion; ++n) {
                        const PageId id = IdIn(r, n);
                        if (!store->IsAllocated(id)) {
                            wrong_answers.fetch_add(1, std::memory_order_relaxed);
                        }
                        (void)store->IsHeaderless(id);
                    }
                }
            }
        });
    }
    for (std::thread& th : threads) th.join();

    EXPECT_EQ(create_failures.load(), 0) << "a placement was refused outright";
    EXPECT_EQ(wrong_answers.load(), 0)
        << "a reader was told an id in a published region is unallocated: its `find` ran inside "
           "another core's region insertion";

    // Every id still reads back allocated once everything has landed, and
    // every region exists exactly once. `try_emplace` is what makes a second
    // creator return the first one's rather than replace it, so a count
    // above `kRegions` would mean a bitmap somebody had set bits in was
    // overwritten.
    std::size_t total = 0;
    for (int r = 0; r < kRegions; ++r) {
        total += placed[r].size();
        for (PageId id : placed[r]) EXPECT_TRUE(store->IsAllocated(id));
    }
    EXPECT_EQ(total, static_cast<std::size_t>(kRegions * kPerRegion));
    EXPECT_EQ(store->map_residency().regions, static_cast<std::size_t>(kRegions));
}

// **Every core creating the *same* region at once**, which is the writer
// side of the defect and the one the cell above does not reach. Eight
// threads place ids in one region that does not exist yet, so all eight
// reach `EnsureRegionResident` for the same key, all eight find it absent,
// all eight grow the file - `EnsureCapacity` is idempotent - and all eight
// arrive at the insertion together. That is `std::map::insert` racing
// `std::map::insert` on one tree, which the strided cell above almost never
// produces: with the publication moved back outside the hold, that cell
// stayed green in 10 of 10 runs and this one fails in 4 of 8 - once with a
// SIGSEGV, and otherwise with the region count reading 14, 23 or 377 where
// 13 regions exist.
//
// **`try_emplace`, not `emplace`, and the difference is what is asserted.**
// Seven of the eight lose, and a loser must return the winner's region
// rather than replace it - the bitmap it built is a fresh one with no bits
// set, and installing it over a map another core has already allocated out
// of would silently free every page in the region. The residency count and
// the read-back of every placed id are the two halves of that.
//
// **Mutation:** `insert_or_assign` for `try_emplace` - 8 of 8 runs fail,
// reporting 13 to 46 placed ids reading unallocated.
TEST(FreeMapRaceTest, EveryCoreCreatingOneRegionAtOnceLeavesOneRegion) {
    std::unique_ptr<MemoryPageDevice> device;
    auto store = ArmedStore(device);
    ASSERT_NE(store, nullptr);

    constexpr int kThreads = 8;
    constexpr int kRounds = 12;

    std::vector<std::vector<PageId>> placed(kThreads);
    std::atomic<int> create_failures{0};

    // **Twelve fresh regions, one per round**, because one is not enough to
    // see it: a thread that loses the race early returns at the
    // find-under-the-hold and never reaches the insertion at all, so any
    // single round is mostly a no-contest. Measured - one round killed the
    // `insert_or_assign` mutation in 1 run of 6, twelve kills it in every
    // run.
    for (int round = 0; round < kRounds; ++round) {
        const int fresh_region = 9 + round;
        std::atomic<int> ready{0};
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                SetCurrentCore(static_cast<std::uint32_t>(t));
                ready.fetch_add(1, std::memory_order_acq_rel);
                while (ready.load(std::memory_order_acquire) < kThreads) {
                    std::this_thread::yield();
                }
                // Disjoint ids inside one region, so nothing here is
                // contending for a *bit* - `alloc_race_test.cpp` owns that
                // question. What they contend for is the region's existence.
                for (int n = 0; n < 4; ++n) {
                    const PageId id = IdIn(fresh_region, t * 4 + n);
                    auto made = store->CreateAt(id);
                    if (!made.ok()) {
                        create_failures.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    placed[t].push_back(id);
                    made.value().Release();
                }
            });
        }
        for (std::thread& th : threads) th.join();
    }

    EXPECT_EQ(create_failures.load(), 0) << "a placement was refused outright";

    // Region 0 exists from `Open`; each round should have added exactly one.
    EXPECT_EQ(store->map_residency().regions, static_cast<std::size_t>(kRounds + 1))
        << "a region was created more than once, or an insertion was lost";

    std::size_t total = 0;
    std::size_t missing = 0;
    for (int t = 0; t < kThreads; ++t) {
        total += placed[t].size();
        for (PageId id : placed[t]) {
            if (!store->IsAllocated(id)) ++missing;
        }
    }
    EXPECT_EQ(total, static_cast<std::size_t>(kThreads * 4 * kRounds));
    EXPECT_EQ(missing, 0u)
        << missing << " of " << total
        << " placed ids read unallocated: a loser's empty bitmap replaced the winner's";
}

// **Who runs the map writeback when one store serves every core** - the
// second half of AM-S3, and it needs no rule of its own. N checkpointers
// each call `FlushMaps`; the first to take the hold clears the dirty flags
// while it copies, so the rest find nothing and write nothing.
//
// Asserted through the **device**, not a test hook: the map's page is
// written once per round of dirtying, however many cores ask. Counting
// writes to that one id is what an operator would count.
//
// **Rounds, for the reason the cell above has them.** One round killed the
// mutation below in 1 run of 8 - four threads rarely land inside one
// another's copy on a first try. Sixteen rounds, each dirtying the map
// afresh, is what makes the overlap reliable.
//
// **Mutation:** clear the dirty flag after the write instead of at the
// copy, and a second flusher finds the region still dirty and writes it
// again.
TEST(FreeMapRaceTest, ConcurrentFlushersWriteTheMapOnceBetweenThem) {
    std::unique_ptr<MemoryPageDevice> device;
    auto store = ArmedStore(device);
    ASSERT_NE(store, nullptr);

    const PageId map_page = FreeMapPageIdFor(FreeMapRegionBase(0));
    constexpr int kFlushers = 4;
    constexpr int kRounds = 16;

    device->ClearTrace();
    for (int round = 0; round < kRounds; ++round) {
        // One allocation, so exactly one region is dirty going into the
        // round.
        auto made = store->CreateNew();
        ASSERT_TRUE(made.ok()) << made.status().message();
        made.value().second.Release();

        std::atomic<int> ready{0};
        std::vector<std::thread> threads;
        threads.reserve(kFlushers);
        for (int t = 0; t < kFlushers; ++t) {
            threads.emplace_back([&, t] {
                SetCurrentCore(static_cast<std::uint32_t>(t));
                ready.fetch_add(1, std::memory_order_acq_rel);
                while (ready.load(std::memory_order_acquire) < kFlushers) {
                    std::this_thread::yield();
                }
                EXPECT_TRUE(store->PersistMaps().ok());
            });
        }
        for (std::thread& th : threads) th.join();
    }

    std::size_t writes = 0;
    for (const MemoryPageDevice::TraceEntry& entry : device->trace()) {
        if (entry.kind == MemoryPageDevice::OpKind::kWrite && entry.first_page_id == map_page) {
            ++writes;
        }
    }
    EXPECT_EQ(writes, static_cast<std::size_t>(kRounds))
        << kFlushers << " cores flushed one dirty region " << kRounds << " times and wrote its map "
        << writes << " times; the hold is what makes the losers find it clean";
}

// **Exactly one caller may place a page at a chosen id** - the guarantee
// `ClaimNamedIdLocked` gives, and the one AM-S3 nearly moved off its latch.
// That function decides one question out of *two* structures: the free
// map's bit says whether the id is allocated, and the frame table's
// `frames_.count` says whether it is resident. Guarding it with the map's
// latch alone left the second read racing `InsertFrame`'s `emplace` on an
// `unordered_map`, so a missed node reads as "not resident" and a live page
// passes the in-use test.
//
// Eight threads race for the same ids, so every id has one winner and seven
// losers, and every loser must be refused `AlreadyExists`. The faulting
// threads beside them are what put `InsertFrame` in flight during the test.
//
// **Mutation:** drop the `LatchGuard structure(structure_latch())` from
// `CreateAtUnpinned`'s claim loop, leaving the map's latch alone - the
// shape this commit shipped before the review.
TEST(FreeMapRaceTest, OnlyOneCallerEverPlacesAPageAtAChosenId) {
    std::unique_ptr<MemoryPageDevice> device;
    auto store = ArmedStore(device);
    ASSERT_NE(store, nullptr);

    constexpr int kThreads = 8;
    constexpr int kIds = 40;
    constexpr int kFaulters = 4;

    // Pages that already exist, for the faulting half to reach.
    std::vector<PageId> resident;
    for (int n = 0; n < 16; ++n) {
        auto made = store->CreateNew();
        ASSERT_TRUE(made.ok()) << made.status().message();
        resident.push_back(made.value().first);
        made.value().second.Release();
    }

    std::vector<std::atomic<int>> winners(kIds);
    for (std::atomic<int>& w : winners) w.store(0, std::memory_order_relaxed);
    std::atomic<int> unexpected{0};
    std::atomic<bool> done{false};
    std::atomic<int> ready{0};

    const int kAll = kThreads + kFaulters;
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(kAll));

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            SetCurrentCore(static_cast<std::uint32_t>(t));
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) < kAll) std::this_thread::yield();
            for (int n = 0; n < kIds; ++n) {
                auto made = store->CreateAt(IdIn(2, n));
                if (made.ok()) {
                    winners[n].fetch_add(1, std::memory_order_relaxed);
                    made.value().Release();
                } else if (made.status().code() != StatusCode::kAlreadyExists) {
                    // A loser is owed `AlreadyExists` and nothing else.
                    unexpected.fetch_add(1, std::memory_order_relaxed);
                }
            }
            done.store(true, std::memory_order_release);
        });
    }
    for (int f = 0; f < kFaulters; ++f) {
        threads.emplace_back([&, f] {
            SetCurrentCore(static_cast<std::uint32_t>(kThreads + f));
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) < kAll) std::this_thread::yield();
            // Reads of resident pages, to keep the frame table busy while
            // the claimers are inside its `count`.
            while (!done.load(std::memory_order_acquire)) {
                for (PageId id : resident) {
                    auto got = store->Get(id);
                    if (got.ok()) got.value().Release();
                }
            }
        });
    }
    for (std::thread& th : threads) th.join();

    EXPECT_EQ(unexpected.load(), 0) << "a losing claim was refused something other than "
                                       "AlreadyExists";
    int multiply_placed = 0;
    for (int n = 0; n < kIds; ++n) {
        if (winners[n].load(std::memory_order_relaxed) != 1) ++multiply_placed;
    }
    EXPECT_EQ(multiply_placed, 0)
        << multiply_placed << " of " << kIds
        << " ids were placed by a number of callers other than one: the in-use test read a frame "
           "table another core was inserting into";
}

// **A failed map write leaves the whole batch owing one** (AM-S3's review).
// The dirty flag is cleared at the copy, for the concurrency reason
// `FlushMaps` gives, which puts the batch in debt to the tail: a write that
// fails partway leaves every region after it copied, cleared and unwritten.
// Leaving those clean means their allocation bits never reach the platter
// and a restart reads the pages free and re-issues them - the exact loss
// the free-map write's own comment warns about, one region over.
//
// Single-threaded on purpose. This is not a race; it is what the race's fix
// costs, and it reproduces deterministically.
//
// **Mutation:** re-mark only the failing region rather than the tail - the
// shape this commit shipped before the review - and the second flush writes
// one region instead of two.
TEST(FreeMapRaceTest, AFailedMapWriteLeavesEveryLaterRegionStillOwed) {
    std::unique_ptr<MemoryPageDevice> device;
    auto store = ArmedStore(device);
    ASSERT_NE(store, nullptr);

    // Three dirty regions: 0 from the store's own open, plus two more.
    // `CreateAt` past a region's base is what brings it into existence.
    constexpr int kA = 3;
    constexpr int kB = 5;
    ASSERT_TRUE(store->CreateAt(IdIn(kA, 0)).ok());
    ASSERT_TRUE(store->CreateAt(IdIn(kB, 0)).ok());
    auto seed = store->CreateNew();  // dirties region 0
    ASSERT_TRUE(seed.ok());
    seed.value().second.Release();

    // Fail the *first* map write of the batch. Regions are written in
    // ascending order, so region 0's is first and A's and B's are behind
    // it - copied and cleared, and never reached.
    device->FailNextWrite(Status::IoError("injected"));
    EXPECT_FALSE(store->PersistMaps().ok()) << "the injected failure was not reported";

    // Now let it through. Every region that was dirty going into the failed
    // flush must be written here; if the tail was left clean, the ones
    // behind the failure are missing.
    device->ClearTrace();
    EXPECT_TRUE(store->PersistMaps().ok());

    bool wrote_zero = false;
    bool wrote_a = false;
    bool wrote_b = false;
    for (const MemoryPageDevice::TraceEntry& entry : device->trace()) {
        if (entry.kind != MemoryPageDevice::OpKind::kWrite) continue;
        if (entry.first_page_id == FreeMapPageIdFor(FreeMapRegionBase(0))) wrote_zero = true;
        if (entry.first_page_id == FreeMapPageIdFor(FreeMapRegionBase(kA))) wrote_a = true;
        if (entry.first_page_id == FreeMapPageIdFor(FreeMapRegionBase(kB))) wrote_b = true;
    }
    EXPECT_TRUE(wrote_zero) << "the region whose write failed was not retried";
    EXPECT_TRUE(wrote_a) << "a region behind the failure was left clean and never written";
    EXPECT_TRUE(wrote_b) << "a region behind the failure was left clean and never written";
}

}  // namespace
}  // namespace kds::storage
