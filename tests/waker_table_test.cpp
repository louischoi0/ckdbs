// AU-S1: the kick, AR0-6's one cross-core primitive.
//
// What these pin is that a reactor is wakeable **without a transport**. Until
// AU-S1 `may_sleep` read `transport_ != nullptr`, so a reactor with no
// transport never raised its `sleeping` flag and could not be woken at all -
// it only ever timed out. Retiring the ring would have taken every cross-core
// wake with it, silently, and the symptom would have been latency rather than
// a failure.
//
// A real `EpollIoBackend` and a real `SystemClock`, deliberately: a
// `NullIoBackend` never blocks, so a wake test against one would assert
// nothing about waking.

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/sched/epoll_io_backend.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/sched/waker_table.hpp"

namespace kds::sched {
namespace {

TEST(WakerTableTest, AKickEndsAPeersIdleBlockWithNoTransportAttached) {
    auto backend = EpollIoBackend::Create();
    ASSERT_TRUE(backend.ok()) << backend.status().message();
    SystemClock clock;
    SchedulerConfig config;
    // Long enough that timing out rather than waking is a failure, not a
    // slow pass.
    config.max_idle_block_ms = 5000;
    Scheduler scheduler(clock, backend.value(), config);

    WakerTable table(/*core_count=*/2);
    ASSERT_TRUE(scheduler.AttachWakerTable(&table, /*core_id=*/0).ok());

    std::atomic<bool> stop{false};
    std::atomic<int> turns{0};
    std::thread reactor([&] {
        while (!stop.load(std::memory_order_acquire)) {
            scheduler.RunOnce();
            turns.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Let it reach the block. Without a kick the loop advances about once
    // per five seconds.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int before = turns.load(std::memory_order_relaxed);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (turns.load(std::memory_order_relaxed) <= before &&
           std::chrono::steady_clock::now() < deadline) {
        table.Kick(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_GT(turns.load(std::memory_order_relaxed), before)
        << "the reactor never came out of its idle block, so the kick did not reach it";
    EXPECT_GT(table.kicks(), 0u) << "every kick was skipped, so the reactor never slept";

    // **Bounded teardown, so a broken wake fails rather than hangs.** The
    // first version of this spun until the reactor advanced, which with the
    // wake broken is forever - and a hanging cell says less than a failing
    // one. Mutating `may_sleep` back to requiring a transport was what
    // showed it: the assertions above fire correctly, and then the teardown
    // spun. The reactor leaves its block within `max_idle_block_ms`
    // regardless, so join is safe once the kicks stop.
    stop.store(true, std::memory_order_release);
    const auto teardown_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < teardown_deadline) {
        table.Kick(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    reactor.join();
}

TEST(WakerTableTest, AKickToABusyReactorWritesNothing) {
    // The `sleeping` flag's whole job: a reactor that is not blocked costs
    // no syscall. Nothing runs this reactor, so the flag is never raised and
    // every kick must be skipped rather than written.
    auto backend = EpollIoBackend::Create();
    ASSERT_TRUE(backend.ok()) << backend.status().message();
    SystemClock clock;
    Scheduler scheduler(clock, backend.value());
    WakerTable table(/*core_count=*/1);
    ASSERT_TRUE(scheduler.AttachWakerTable(&table, /*core_id=*/0).ok());

    for (int i = 0; i < 100; ++i) table.Kick(0);

    EXPECT_EQ(table.kicks(), 0u) << "a kick was written to a reactor that was never asleep";
    EXPECT_EQ(table.kicks_skipped(), 100u);
}

TEST(WakerTableTest, AnUnregisteredCoreIsNeverKickedAndIsNotAnError) {
    // A core that registers nothing falls back to its idle block, which is
    // what every single-core build does. Out of range is the same: the table
    // is not the place to discover a bad core id.
    WakerTable table(/*core_count=*/2);
    table.Kick(0);
    table.Kick(1);
    table.Kick(7);
    EXPECT_EQ(table.kicks(), 0u);
    EXPECT_EQ(table.kicks_skipped(), 0u) << "an unregistered core is not a skipped kick";
}

}  // namespace
}  // namespace kds::sched
