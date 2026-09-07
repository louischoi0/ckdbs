// AV-S1: the two-core rig's own three cells
// (`instructions/v3.0.0/workorder-av-two-core-rig.md` §6, cells 1-3). What
// they price is AR0-6-R1's cost: a kick ends a peer's idle block, a lost
// kick costs one idle block and nothing else, and two `PageLatch` holders
// are genuinely two cores.
//
// Every wait here is bounded and every claim is made on a counter the
// protocol itself moves - `wakes_received()`, `idle_blocks()` - never on
// how long a kernel sleeps. Both are among the three scheduler accessors
// its header admits from another thread. Where a cell must know a reactor
// is asleep it reads `idle_blocks()`, after which nothing but a kick or the
// block's own ceiling can end the block.

#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <thread>

#include <gtest/gtest.h>

#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/storage/page_header.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

// A task that parks on `go` and records that it proceeded. The predicate is
// a member because `WaitUntil` holds a pointer to it across the park.
struct ParkedTask {
    std::atomic<bool> go{false};
    std::atomic<bool> done{false};
    std::function<bool()> pred;
};

sched::Coro ParkUntilGo(ParkedTask& t) {
    t.pred = [&t] { return t.go.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&t.pred};
    t.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

std::unique_ptr<TwoCoreRig> OpenRig(TwoCoreRig::Options options) {
    auto rig = TwoCoreRig::Open(options);
    EXPECT_TRUE(rig.ok()) << rig.status().message();
    return rig.ok() ? std::move(rig.value()) : nullptr;
}

TEST(TwoCoreRigTest, AKickWakesAParkedPeer) {
    // §6 cell 1. Core 1's reactor is in its idle block with a task parked on
    // a flag; the cell writes the flag, kicks, and core 1 proceeds at the
    // kick - which the sim holds for three ticks, so "at the kick" is a
    // moment the cell chooses rather than one it races.
    TwoCoreRig::Options options;
    options.wake.min_delay_ticks = 3;
    options.wake.max_delay_ticks = 3;
    auto rig = OpenRig(options);
    ASSERT_NE(rig, nullptr);

    ParkedTask parked;
    rig->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, ParkUntilGo(parked)));
    rig->Start();

    sched::Scheduler& peer = rig->core(1).scheduler();
    ASSERT_TRUE(Within(2000ms, [&] { return peer.idle_blocks() >= 1; }))
        << "core 1 never blocked; nothing is under test";

    // Write, then kick (AR0-6-R1). The kick is recorded now and held.
    parked.go.store(true, std::memory_order_release);
    rig->wake().Kick(1);
    const std::vector<sched::SimWakerTable::Record> to_peer = KicksTo(*rig, 1);
    ASSERT_EQ(to_peer.size(), 1u);
    EXPECT_EQ(to_peer[0], (sched::SimWakerTable::Record{0, 1, 3}));
    EXPECT_EQ(peer.wakes_received(), 0u) << "a held kick reached core 1's eventfd";
    // Nothing has ended core 1's block: the flag is set, but a parked task
    // is polled only when its reactor turns, and its reactor is asleep for
    // five seconds unless something writes its eventfd.
    EXPECT_FALSE(parked.done.load(std::memory_order_acquire))
        << "the parked task proceeded before any kick reached its reactor";

    rig->wake().Advance(3);
    EXPECT_GE(rig->wake().tick(), to_peer[0].due);
    EXPECT_TRUE(Within(1000ms, [&] { return parked.done.load(std::memory_order_acquire); }))
        << "core 1 did not proceed at the kick";
    // `sched_wakes_received` on core 1: the one kick was written, which is
    // the proof it found core 1 asleep rather than being skipped.
    EXPECT_EQ(peer.wakes_received(), 1u) << "the kick was skipped, so core 1 was not asleep";
}

TEST(TwoCoreRigTest, ALostKickCostsOneIdleBlockAndNothingElse) {
    // §6 cell 2, from the receiver's side. The publisher's kick never
    // arrives - here because the sim holds it and the cell never advances,
    // which is the same thing the accepted race produces when a publisher
    // reads the flag clear and skips (`waker_table.hpp`, and the skip
    // itself is `WakerTableTest`'s). The peer proceeds at block expiry,
    // correct and slow: AR0-6-R1's stated cost, not a defect.
    TwoCoreRig::Options options;
    options.max_idle_block_ms = 200;
    options.wake.min_delay_ticks = 5;
    options.wake.max_delay_ticks = 5;
    auto rig = OpenRig(options);
    ASSERT_NE(rig, nullptr);

    ParkedTask parked;
    rig->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, ParkUntilGo(parked)));
    rig->Start();

    sched::Scheduler& peer = rig->core(1).scheduler();
    ASSERT_TRUE(Within(2000ms, [&] { return peer.idle_blocks() >= 1; }));
    const std::uint64_t blocks_before = peer.idle_blocks();

    parked.go.store(true, std::memory_order_release);
    rig->wake().Kick(1);
    const std::vector<sched::SimWakerTable::Record> to_peer = KicksTo(*rig, 1);
    ASSERT_EQ(to_peer.size(), 1u);
    EXPECT_GT(to_peer[0].due, rig->wake().tick()) << "the kick was not held";

    // Correct: it proceeds. Slow: only once the block it was in expired -
    // nothing wrote its eventfd (the counter says so) and it has no fd and
    // no timer, so expiry is the only exit the block had. And having
    // proceeded with nothing left to run, it blocks again, which is the
    // strict move on `idle_blocks()` a bounded wait sees.
    EXPECT_TRUE(Within(3000ms, [&] { return parked.done.load(std::memory_order_acquire); }))
        << "the peer never proceeded, so a lost kick cost liveness rather than latency";
    EXPECT_EQ(peer.wakes_received(), 0u)
        << "something wrote core 1's eventfd; the kick was not lost";
    EXPECT_TRUE(Within(2000ms, [&] { return peer.idle_blocks() > blocks_before; }))
        << "the peer never blocked again after proceeding";
}

// The two halves of cell 3, as coroutines on their own cores.
struct PageHold {
    PageId page = kInvalidPageId;
    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
    std::atomic<bool> released{false};
    std::atomic<bool> fetching{false};
    std::atomic<bool> fetched{false};
    std::atomic<int> failures{0};
    std::function<bool()> held_pred;
};

// Core 0: take the page exclusive and hold it until told to let go. The
// spin is deliberate - a holder in a long section, and no park with a pin.
sched::Coro HoldExclusive(storage::DevicePageStore& store, PageHold& h) {
    {
        auto ref = store.Get(h.page);
        if (!ref.ok()) {
            ++h.failures;
            co_return ref.status();
        }
        h.held.store(true, std::memory_order_release);
        while (!h.release.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    h.released.store(true, std::memory_order_release);
    co_return Status::OK();
}

// Core 1: wait for the hold, then fetch the page through a scan ring. The
// fetch takes the page's latch shared, so it waits for core 0's exclusive
// to drop - inside `Fetch`, on this reactor's thread, as AM-R8c says it
// must.
sched::Coro FetchThroughRing(storage::DevicePageStore& store, PageHold& h) {
    h.held_pred = [&h] { return h.held.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&h.held_pred};
    auto ring = store.OpenScanRing(/*frames=*/1);
    h.fetching.store(true, std::memory_order_release);
    auto bytes = ring->Fetch(h.page);
    if (!bytes.ok() || bytes.value()[storage::kPageBodyOffset] != std::byte{0x7E}) ++h.failures;
    h.fetched.store(true, std::memory_order_release);
    co_return Status::OK();
}

TEST(TwoCoreRigTest, APageHeldExclusiveOnCore0BlocksAFetchOnCore1) {
    // §6 cell 3, AV-R5's promotion of `ARingFetchWaitsForAPageAnotherCore
    // HoldsExclusive` (AM-S2-P S-P2). That cell took its foreign hold
    // through `LatchFrameForTest(..., core=7)` because two threads of one
    // store are one core to `PageLatch`, which admits a shared acquire
    // under its own core's exclusive. Here the holder runs on core 0's
    // reactor and the fetcher on core 1's, each thread declaring its core
    // through `CoreRuntime::Run`, and the word sees two cores.
    auto rig = OpenRig({});
    ASSERT_NE(rig, nullptr);
    storage::DevicePageStore& store = rig->store();

    PageHold hold;
    // **The holder is released on every exit from this cell.** Its task
    // spins on `release` while holding the page, and the fetcher's reactor
    // spins in `PageLatch::Acquire` behind it; an `ASSERT` that left the
    // cell without setting the flag left both reactors spinning and the
    // rig's `Stop()` joining forever - which is how the budget-8 suite hung
    // on this cell once (2026-09-07).
    struct ReleaseOnExit {
        PageHold& h;
        ~ReleaseOnExit() { h.release.store(true, std::memory_order_release); }
    } release_on_exit{hold};
    {
        auto created = store.CreateNew();
        ASSERT_TRUE(created.ok()) << created.status().message();
        hold.page = created.value().first;
        storage::FormatPage(created.value().second.bytes(), PageType::kHeap);
        created.value().second.bytes()[storage::kPageBodyOffset] = std::byte{0x7E};
    }
    ASSERT_TRUE(store.Sync().ok());

    rig->core(0).scheduler().Submit(sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                                                         HoldExclusive(store, hold)));
    rig->core(1).scheduler().Submit(sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                                                         FetchThroughRing(store, hold)));
    rig->Start();

    // Both are the cell's own barriers across cores, so both kick: core 1
    // parks on `held`, a flag core 0 sets with nothing kicking for it, and
    // under a frame budget core 0's `Get` faults slowly enough that core 1
    // has blocked by the time the flag flips - a five-second sleep against
    // a two-second wait, which is the shape that hung this cell.
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return hold.held.load(std::memory_order_acquire); }))
        << "core 0 never took the page";
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return hold.fetching.load(std::memory_order_acquire); }))
        << "core 1 never reached its fetch";
    // Bounded rather than timed, as the promoted cell was: what is asserted
    // is that a wait happens, not how long it is. Two thousand yields after
    // core 1 published `fetching` is far past the latch's spin-then-yield.
    for (int turn = 0; turn < 2000 && !hold.fetched.load(std::memory_order_acquire); ++turn) {
        std::this_thread::yield();
    }
    EXPECT_FALSE(hold.fetched.load(std::memory_order_acquire))
        << "the ring read a page another core held exclusive";

    hold.release.store(true, std::memory_order_release);
    EXPECT_TRUE(Within(2000ms, [&] { return hold.released.load(std::memory_order_acquire); }));
    EXPECT_TRUE(Within(2000ms, [&] { return hold.fetched.load(std::memory_order_acquire); }))
        << "the ring never got the page after the hold dropped";
    EXPECT_EQ(hold.failures.load(), 0);
    // Both tasks are done and both rings are dropped, so the pool holds no
    // pin of theirs - read after `Stop()`, which is the join that orders
    // the reactors' last stores before this thread's load.
    rig->Stop();
    EXPECT_EQ(store.live_pins(), 0u);
}

}  // namespace
}  // namespace kds::server
