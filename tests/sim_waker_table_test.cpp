// AU-S1c: `SimWakerTable`, the deterministic wake registry the two-core rig
// runs on (`instructions/v3.0.0/workorder-au-ring-retirement.md` AU-R3 as
// AV-R1's mark rewrote it).
//
// Three claims, one cell each, plus the one AU-5's H1 makes for the rig: a
// seed's `(tick, dst)` log is byte-identical across runs; a kick to a busy
// destination is recorded and delivers nothing; the maximum delay still
// delivers; and a zero-delay kick is the production path with a log line in
// front of it. The reactor cells use a real `EpollIoBackend`, for
// `waker_table_test.cpp`'s reason - a `NullIoBackend` never blocks, so a
// wake test against one asserts nothing about waking.

#include "kds/sched/sim_waker_table.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/sched/epoll_io_backend.hpp"
#include "kds/sched/scheduler.hpp"

namespace kds::sched {
namespace {

// The script every determinism run replays: kicks and advances interleaved
// so that delays drawn at different ticks land in the log.
std::vector<SimWakerTable::Record> RunScript(std::uint64_t seed) {
    WakerTable real(/*core_count=*/2);
    SimWakerConfig config;
    config.min_delay_ticks = 0;
    config.max_delay_ticks = 7;
    config.seed = seed;
    SimWakerTable sim(real, config);
    for (int round = 0; round < 8; ++round) {
        sim.Kick(1);
        sim.Kick(0);
        sim.Advance(1);
        sim.Kick(1);
        sim.Advance(3);
    }
    sim.Advance(16);
    EXPECT_EQ(sim.in_flight(), 0u) << "sixteen ticks past the last kick, something is still held";
    return sim.log();
}

TEST(SimWakerTableTest, ASeedsKickLogIsByteIdenticalAcrossThreeRuns) {
    const std::vector<SimWakerTable::Record> first = RunScript(0x1234);
    const std::vector<SimWakerTable::Record> second = RunScript(0x1234);
    const std::vector<SimWakerTable::Record> third = RunScript(0x1234);
    ASSERT_EQ(first.size(), 24u);
    EXPECT_EQ(first, second);
    EXPECT_EQ(second, third);

    // The seed is read, not decorative: a different one draws different
    // delays for the same script. Not a strict inequality on every entry -
    // two seeds can agree on a draw - but a log that never differs would
    // mean the delay is not drawn at all.
    const std::vector<SimWakerTable::Record> other = RunScript(0x4321);
    ASSERT_EQ(other.size(), first.size());
    bool differs = false;
    for (std::size_t i = 0; i < first.size(); ++i) {
        // The tick and the destination are the script's; only `due` is the
        // seed's.
        EXPECT_EQ(first[i].tick, other[i].tick);
        EXPECT_EQ(first[i].dst, other[i].dst);
        if (first[i].due != other[i].due) differs = true;
    }
    EXPECT_TRUE(differs) << "two seeds drew identical delays for 24 kicks; the seed is not read";
}

TEST(SimWakerTableTest, AKickToABusyDestinationIsRecordedAndDeliversNothing) {
    // A reactor nobody runs has its `sleeping` flag clear, which is the
    // state a busy reactor is in between blocks. The kick must reach the
    // real table - it is the real table's skip that is under test, not a
    // second one - and the real table must write nothing.
    auto backend = EpollIoBackend::Create();
    ASSERT_TRUE(backend.ok()) << backend.status().message();
    SystemClock clock;
    Scheduler scheduler(clock, backend.value());
    WakerTable real(/*core_count=*/1);
    SimWakerTable sim(real);
    ASSERT_TRUE(scheduler.AttachWakerTable(&sim, /*core_id=*/0).ok());

    sim.Kick(0);

    ASSERT_EQ(sim.log().size(), 1u);
    EXPECT_EQ(sim.log()[0], (SimWakerTable::Record{0, 0, 0}));
    EXPECT_EQ(sim.forwarded(), 1u) << "a zero-delay kick is forwarded on the spot";
    EXPECT_EQ(real.kicks(), 0u) << "a kick was written to a reactor that was never asleep";
    EXPECT_EQ(real.kicks_skipped(), 1u) << "the real table never saw the kick";
    EXPECT_EQ(sim.kicks(), 0u) << "the sim reports kicks asked for, where it must report kicks written";
    EXPECT_EQ(scheduler.wakes_sent(), 0u);
}

// A reactor on its own thread, blocked for far longer than any cell runs,
// so that timing out instead of waking is a failure and not a slow pass.
struct ParkedReactor {
    ParkedReactor(WakeRegistry& registry, const WakerTable& real_table)
        : real(real_table), backend(EpollIoBackend::Create()) {
        EXPECT_TRUE(backend.ok()) << backend.status().message();
        SchedulerConfig config;
        config.max_idle_block_ms = 5000;
        scheduler.emplace(clock, backend.value(), config);
        EXPECT_TRUE(scheduler->AttachWakerTable(&registry, /*core_id=*/0).ok());
        thread = std::thread([this] {
            while (!stop.load(std::memory_order_acquire)) {
                scheduler->RunOnce();
                turns.fetch_add(1, std::memory_order_relaxed);
            }
        });
        // Let it reach the block. The wait below is the one place a sleep is
        // the point: the cell is about a reactor that is *already* asleep.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (scheduler->idle_blocks() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_GE(scheduler->idle_blocks(), 1u) << "the reactor never blocked; nothing is under test";
    }

    // Turns taken since `before`, waiting up to two seconds for one.
    bool AdvancedPast(int before) const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (turns.load(std::memory_order_relaxed) <= before &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return turns.load(std::memory_order_relaxed) > before;
    }

    ~ParkedReactor() {
        // Flag, then kick through the **real** table, `waker_table_test.cpp`'s
        // teardown: the reactor is in a five-second block again by now, and
        // a join alone would wait it out. The real table rather than the
        // sim's, so the teardown never depends on a tick nobody advances;
        // and the reactor leaves its block within `max_idle_block_ms`
        // regardless, so a broken wake fails an assertion above rather
        // than hanging here.
        stop.store(true, std::memory_order_release);
        real.Kick(0);
        thread.join();
    }

    const WakerTable& real;
    StatusOr<EpollIoBackend> backend;
    SystemClock clock;
    std::optional<Scheduler> scheduler;
    std::atomic<bool> stop{false};
    std::atomic<int> turns{0};
    std::thread thread;
};

TEST(SimWakerTableTest, TheMaximumDelayStillDelivers) {
    WakerTable real(/*core_count=*/1);
    SimWakerConfig config;
    config.min_delay_ticks = 5;
    config.max_delay_ticks = 5;
    SimWakerTable sim(real, config);
    ParkedReactor reactor(sim, real);
    const int before = reactor.turns.load(std::memory_order_relaxed);

    sim.Kick(0);
    EXPECT_EQ(sim.in_flight(), 1u);
    EXPECT_EQ(sim.forwarded(), 0u) << "a delayed kick reached the real table before its tick";
    EXPECT_EQ(real.kicks(), 0u);

    // Four ticks: one short. Nothing is written and the reactor stays in its
    // block - asserted on the counters, which are exact, rather than on the
    // turn count, which a five-second block would only make probable.
    sim.Advance(4);
    EXPECT_EQ(sim.in_flight(), 1u);
    EXPECT_EQ(real.kicks(), 0u) << "the kick was forwarded a tick early";

    // The fifth tick is the due one.
    sim.Advance(1);
    EXPECT_EQ(sim.in_flight(), 0u);
    EXPECT_EQ(sim.forwarded(), 1u);
    EXPECT_TRUE(reactor.AdvancedPast(before))
        << "the reactor never came out of its block, so the forwarded kick did not reach it";
    EXPECT_EQ(real.kicks(), 1u) << "the kick was skipped, so the reactor was not asleep";
    EXPECT_EQ(reactor.scheduler->wakes_received(), 1u);
    EXPECT_EQ(sim.kicks(), real.kicks());
}

TEST(SimWakerTableTest, AZeroDelayKickIsTheRealPath) {
    // AU-5's H1 for the rig: at delay 0 nothing is held, so a kick ends the
    // block on the kicking thread exactly as a production `Kick` does, and
    // the sim's only trace of itself is the log line.
    WakerTable real(/*core_count=*/1);
    SimWakerTable sim(real);
    ParkedReactor reactor(sim, real);
    const int before = reactor.turns.load(std::memory_order_relaxed);

    sim.Kick(0);
    EXPECT_EQ(sim.in_flight(), 0u);
    EXPECT_TRUE(reactor.AdvancedPast(before)) << "a zero-delay kick did not end the block";
    EXPECT_EQ(real.kicks(), 1u);
    ASSERT_EQ(sim.log().size(), 1u);
    EXPECT_EQ(sim.log()[0].due, sim.log()[0].tick);
}

}  // namespace
}  // namespace kds::sched
