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
        // **No frame budget, because no budget can make the sweep run
        // here** - measured with a counter on the sweep block, not reasoned.
        // A previous version of this set `SetFrameBudget(8)` and claimed the
        // rounds below crossed it repeatedly; the count was **0 in both
        // cells**. The sweep sits on the *miss* path, after `InsertFrame`
        // (EV5/MG06), and neither cell reaches it:
        //
        //   - `ManyThreads...` makes its 16 pages with `CreateNew`, which
        //     does not pass through that block at all, and then only ever
        //     *hits*. No miss, no sweep, whatever the budget.
        //   - `ConcurrentMisses...` evicts down to nothing and faults one
        //     page, so `frames_.size()` is 1 at the miss - under any budget
        //     that is not disabled.
        //
        // A cell that reaches the sweep has to fault **more distinct pages
        // than the budget**, which is a different rig, not a line here. Left
        // out rather than left in looking load-bearing.
        //
        // That rig is `BudgetedPinProtocolTest` at the bottom of this file
        // (AM-R10), and it asserts `inline_sweeps()` moved rather than
        // reasoning about arithmetic - which is what turned the claim above
        // from an assumption into a measurement in the first place.
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

TEST_F(PinProtocolTest, ARingSpanSurvivesAConcurrentSweepBecauseTheRingPinsIt) {
    // **AM-R8, and the reason it exists.** A scan reader holds a span into
    // a frame while another thread sweeps the pool. Before the ruling the
    // ring pinned nothing, `EvictColdFrames` saw a clean, unpinned, usage-0
    // frame and reclaimed it - correctly by its own rule - and freed the
    // bytes under the reader with every pin gauge balancing perfectly.
    //
    // **Deterministic, not a race**: the reader is parked on `sweep_done`
    // while the sweep runs, so there is exactly one thread inside the
    // frame table and the only thing under test is whether the pin stops
    // the reclaim.
    //
    // **Mutation**: with `FetchPinned`'s pin removed from `ScanRing::Fetch`
    // the frame is erased here, and the cell fails on both assertions
    // below; the reader's own check then reads MG05's 0xEF poison (or, in
    // an ASan build, a freed page).
    const PageId id = MakeResidentPage(std::byte{0x3C});
    // Absent to start, so the ring faults it rather than pinning a frame
    // somebody else made resident.
    ASSERT_GT(store_->EvictColdFrames(8), 0u);

    std::atomic<bool> fetched{false};
    std::atomic<bool> sweep_done{false};
    std::atomic<int> failures{0};
    std::thread scan([&] {
        auto ring = store_->OpenScanRing(/*frames=*/2);
        auto bytes = ring->Fetch(id);
        if (!bytes.ok()) {
            ++failures;
            fetched.store(true, std::memory_order_release);
            return;
        }
        fetched.store(true, std::memory_order_release);
        while (!sweep_done.load(std::memory_order_acquire)) std::this_thread::yield();
        // Read *after* the sweep walked over this frame.
        if (bytes.value()[kPageBodyOffset] != std::byte{0x3C}) ++failures;
    });

    while (!fetched.load(std::memory_order_acquire)) std::this_thread::yield();
    // Laps enough to walk any counter to zero and reclaim; a ring fetch
    // left this frame's usage at zero, so the pin is the only thing
    // standing between the sweep and the reader's bytes.
    (void)store_->EvictColdFrames(8);
    EXPECT_EQ(store_->pinned_frames(), 1u) << "the ring's page was not pinned during the scan";
    EXPECT_TRUE(store_->latch_word_for_test(id).ok())
        << "the sweep reclaimed the frame a scan was reading";
    sweep_done.store(true, std::memory_order_release);
    scan.join();

    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(store_->live_pins(), 0u) << "the ring did not release its pin";
}

TEST_F(PinProtocolTest, ARingFetchWaitsForAPageAnotherCoreHoldsExclusive) {
    // **AM-R8c**: the ring's pin is a *latched* pin, so a page another core
    // holds exclusive is waited for exactly as `GetForRead` waits. A pin
    // alone would let the scan read a page mid-write on another core.
    //
    // The hold is taken through the test hook because this store is core 0
    // and the word admits a shared acquire under its *own* core's exclusive
    // (page_latch.hpp) - two threads of one store cannot stand in for two
    // cores here, which is the same reason `page_latch_test.cpp` uses the
    // hook for the sweep's refusal.
    const PageId id = MakeResidentPage(std::byte{0x7E});
    ASSERT_TRUE(store_->LatchFrameForTest(id, PinMode::kExclusive, /*core=*/7).ok());

    std::atomic<bool> started{false};
    std::atomic<bool> fetched{false};
    std::atomic<int> failures{0};
    std::thread scan([&] {
        auto ring = store_->OpenScanRing(/*frames=*/1);
        started.store(true, std::memory_order_release);
        auto bytes = ring->Fetch(id);
        if (!bytes.ok() || bytes.value()[kPageBodyOffset] != std::byte{0x7E}) ++failures;
        fetched.store(true, std::memory_order_release);
    });

    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
    // Bounded rather than timed, and the bound is far past the latch's
    // spin-then-yield: what is asserted is that a wait happens, not how
    // long it is. The one thing this cannot exclude is that the scan
    // thread had not yet reached `Fetch` - which two thousand yields after
    // it published `started` makes remote, and which the join below turns
    // into a hang rather than a false pass if the wait were unbounded.
    for (int turn = 0; turn < 2000 && !fetched.load(std::memory_order_acquire); ++turn) {
        std::this_thread::yield();
    }
    EXPECT_FALSE(fetched.load(std::memory_order_acquire))
        << "the ring read a page another core held exclusive";

    // **`EXPECT`, not `ASSERT`**: `scan` is joinable and parked in
    // `PageLatch::Acquire` right now, so a fatal assert here would return
    // from the body and `~thread` would call `std::terminate` - the binary
    // dying instead of one cell failing. Release, join, then judge.
    EXPECT_TRUE(store_->UnlatchFrameForTest(id, /*core=*/7).ok());
    scan.join();
    EXPECT_TRUE(fetched.load()) << "the ring never got the page after the hold dropped";
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(store_->live_pins(), 0u);
}

TEST_F(PinProtocolTest, AFaultChargesTheClockCounterOnceArmedAsUnarmed) {
    // **AM-R9, and the disagreement it closes.** The armed loop runs the raw
    // fetch twice on a fault - once on the miss branch that loads the page,
    // once on the hit branch it rounds into - and both used to bump, so an
    // armed store warmed a faulted page to 2 where an unarmed one warms it
    // to 1. That is `SetLatchArmed` changing what the sweep reclaims, which
    // is the one thing arming must not do.
    //
    // **Mutation**: `bump_usage && !charged` → `bump_usage` in
    // `FetchAndPin`'s hit branch, and the armed reading below is 2.
    const PageId id = MakeResidentPage(std::byte{0x11});
    ASSERT_GT(store_->EvictColdFrames(8), 0u);  // absent, so the read faults

    ASSERT_TRUE(store_->latch_armed());
    { ASSERT_TRUE(store_->GetForRead(id).ok()); }
    auto armed = store_->frame_usage_for_test(id);
    ASSERT_TRUE(armed.ok()) << armed.status().message();
    EXPECT_EQ(armed.value(), 1) << "one accessor's fault charged the CLOCK counter more than once";

    // The other half of the claim - *equal to* the unarmed value, not merely
    // 1 - needs an unarmed store, which the census run does not allow: it
    // arms every store at Open() and keeps it armed. Asserted where it can
    // be, skipped where it cannot, rather than faked either way.
    if (std::getenv("KDS_TEST_PAGE_LATCH") != nullptr) return;
    auto plain_device = MemoryPageDevice::Create(/*extent_pages=*/64, /*initial_pages=*/0);
    ASSERT_TRUE(plain_device.ok());
    auto plain_store = DevicePageStore::Open(*plain_device.value(), /*first_new_page_id=*/16);
    ASSERT_TRUE(plain_store.ok());
    ASSERT_FALSE(plain_store.value()->latch_armed());
    auto made = plain_store.value()->CreateNew();
    ASSERT_TRUE(made.ok());
    const PageId plain_id = made.value().first;
    FormatPage(made.value().second.bytes(), PageType::kHeap);
    made.value().second = PageRef();  // drop the handle before the sweep
    ASSERT_TRUE(plain_store.value()->Sync().ok());
    ASSERT_GT(plain_store.value()->EvictColdFrames(8), 0u);
    { ASSERT_TRUE(plain_store.value()->GetForRead(plain_id).ok()); }
    auto unarmed = plain_store.value()->frame_usage_for_test(plain_id);
    ASSERT_TRUE(unarmed.ok()) << unarmed.status().message();
    EXPECT_EQ(armed.value(), unarmed.value())
        << "arming changed what a fault charges the CLOCK counter";
}

// **The budgeted rig R6 said did not exist** (AM-R10). Everything
// `PinProtocolTest` has, plus a frame budget below the working set, so the
// fault path's inline sweep (EV5, MG06) genuinely runs - which is the
// precondition for the window `FetchAndPin`'s `loading_` test guards.
class BudgetedPinProtocolTest : public PinProtocolTest {
protected:
    void SetUp() override {
        PinProtocolTest::SetUp();
        store_->SetFrameBudget(4);
    }
};

TEST_F(BudgetedPinProtocolTest, ConcurrentFaultsPastTheBudgetRunTheInlineSweep) {
    // Twelve pages against a budget of four, faulted by eight threads at
    // once: every miss past the budget sweeps inline, under the temporary
    // hand-pin `ResidentBytes` takes outside the structure latch. That is
    // the only window in which a page is *both* resident and in flight, and
    // it is what the hit path's `loading_` test exists for.
    //
    // **What this asserts is that the window is entered**, by the counter,
    // not that the race inside it is benign - that is a data race, and
    // showing it needs TSan (`FetchAndPin`'s comment says so at the site).
    // Before this rig the counter was 0 in every cell of this file, so the
    // "five green runs with the test deleted" measurement said nothing.
    constexpr int kThreads = 8;
    constexpr int kPages = 12;

    std::vector<PageId> pages;
    pages.reserve(kPages);
    for (int i = 0; i < kPages; ++i) {
        pages.push_back(MakeResidentPage(static_cast<std::byte>(i)));
    }
    while (store_->EvictColdFrames(32) > 0) {
    }
    const std::size_t sweeps_before = store_->inline_sweeps();

    std::atomic<int> failures{0};
    std::atomic<int> arrived{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            ++arrived;
            while (arrived.load(std::memory_order_acquire) != kThreads) std::this_thread::yield();
            for (int i = 0; i < kPages; ++i) {
                const int which = (t + i) % kPages;
                auto ref = store_->GetForRead(pages[static_cast<std::size_t>(which)]);
                if (!ref.ok() ||
                    ref.value().bytes()[kPageBodyOffset] != static_cast<std::byte>(which)) {
                    ++failures;
                }
            }
        });
    }
    for (std::thread& thread : threads) thread.join();

    EXPECT_EQ(failures.load(), 0);
    EXPECT_GT(store_->inline_sweeps(), sweeps_before)
        << "twelve pages against a budget of four never ran the inline sweep; the fixture, not "
           "the code, is what the loading_ test's measurement was about";
    EXPECT_EQ(store_->live_pins(), 0u);
}

}  // namespace
}  // namespace kds::storage
