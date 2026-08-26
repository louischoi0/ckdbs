#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/sched/core_waker.hpp"
#include "kds/sched/epoll_io_backend.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"

// The wake path (docs/spec/sched.md §7, core_waker.hpp): a core that is
// asleep is woken by the message that has work for it.
//
// What these pin, in the order the mechanism is built: the eventfd's two
// contract properties (a wake is sticky, wakes coalesce), the sender's
// wake-only-a-sleeper rule, and the sleeper's peek that closes the race the
// flag alone cannot. The end-to-end case is last and is the one that fails
// by *timing out* rather than by asserting - which is why it carries a
// deadline of its own rather than trusting the harness's.
//
// Wall-clock reads live here and nowhere near engine code: rules.md §4 bans
// them from the engine, and a test whose subject is "did this block end
// early" has no other instrument.

namespace kds::sched {
namespace {

using std::chrono::milliseconds;
using std::chrono::steady_clock;

std::int64_t ElapsedMs(steady_clock::time_point from) {
    return std::chrono::duration_cast<milliseconds>(steady_clock::now() - from).count();
}

MessageHeader MessageTo(std::uint32_t dst, std::uint32_t src = 0) {
    MessageHeader h{};
    h.request_id = 1;
    h.src_core = src;
    h.dst_core = dst;
    h.session_core = src;
    h.kind = static_cast<std::uint16_t>(RingMessageKind::kStepBatch);
    h.sched_group = static_cast<std::uint16_t>(SchedulingGroup::kForeground);
    return h;
}

// ---- The seam: EpollIoBackend::Wake() ----------------------------------

TEST(ReactorWakeTest, AWakeEndsABlockingPoll) {
    auto backend = EpollIoBackend::Create();
    ASSERT_TRUE(backend.ok());

    std::thread waker([&] {
        std::this_thread::sleep_for(milliseconds(30));
        backend.value().Wake();
    });

    std::vector<IoEvent> events;
    const auto started = steady_clock::now();
    // 30 seconds is not a timeout anybody should wait for: it is a ceiling
    // chosen so that a *failure* is unambiguous. Without the wake this poll
    // returns after 30 s with nothing; with it, in tens of milliseconds.
    ASSERT_TRUE(backend.value().PollReady(30'000, events).ok());
    const auto elapsed = ElapsedMs(started);
    waker.join();

    EXPECT_LT(elapsed, 5'000) << "the wake did not end the block";
    // The wake fd is the reactor's own plumbing and has no handler.
    EXPECT_TRUE(events.empty());
}

TEST(ReactorWakeTest, AWakeBeforeTheBlockIsNotLost) {
    auto backend = EpollIoBackend::Create();
    ASSERT_TRUE(backend.ok());

    // The race the sleeping flag cannot close on its own: the wake lands
    // before the sleeper is inside the syscall. An eventfd counter is
    // sticky, so the block must not start at all.
    backend.value().Wake();

    std::vector<IoEvent> events;
    const auto started = steady_clock::now();
    ASSERT_TRUE(backend.value().PollReady(30'000, events).ok());
    EXPECT_LT(ElapsedMs(started), 5'000);
    EXPECT_TRUE(events.empty());
}

TEST(ReactorWakeTest, WakesCoalesceAndAreDrainedSoTheNextBlockStillBlocks) {
    auto backend = EpollIoBackend::Create();
    ASSERT_TRUE(backend.ok());

    for (int i = 0; i < 16; ++i) backend.value().Wake();

    std::vector<IoEvent> events;
    ASSERT_TRUE(backend.value().PollReady(30'000, events).ok());
    EXPECT_TRUE(events.empty());

    // Sixteen wakes, one block ended. If PollReady left the counter set,
    // this second call would return instantly forever and an idle reactor
    // would spin at 100% - the failure the drain exists to prevent.
    const auto started = steady_clock::now();
    ASSERT_TRUE(backend.value().PollReady(60, events).ok());
    EXPECT_GE(ElapsedMs(started), 40) << "the wake counter was not drained";
}

// ---- The sender's half: wake only a sleeper -----------------------------

class WakeCountingBackend final : public IoBackend {
public:
    Status Register(IoHandle, IoInterest) override { return Status::OK(); }
    Status Modify(IoHandle, IoInterest) override { return Status::OK(); }
    Status Unregister(IoHandle) override { return Status::OK(); }
    Status PollReady(int, std::vector<IoEvent>&) override { return Status::OK(); }
    void Wake() noexcept override { ++wakes; }

    std::atomic<int> wakes{0};
};

TEST(ReactorWakeTest, ASendWakesASleepingTargetAndOnlyASleepingOne) {
    auto transport = RealRingTransport::Create(2, 8, 64);
    ASSERT_TRUE(transport.ok());

    WakeCountingBackend backend;
    std::atomic<bool> sleeping{false};
    transport.value().RegisterWaker(1, CoreWakeHandle{&sleeping, &backend});

    // Awake: the syscall stays off the sender's path. This is the property
    // the loaded cells depend on - a busy owner is woken zero times.
    ASSERT_TRUE(transport.value().TrySend(MessageTo(1), {}).ok());
    EXPECT_EQ(backend.wakes.load(), 0);

    sleeping.store(true, std::memory_order_seq_cst);
    ASSERT_TRUE(transport.value().TrySend(MessageTo(1), {}).ok());
    EXPECT_EQ(backend.wakes.load(), 1);
}

TEST(ReactorWakeTest, AFailedSendDoesNotWake) {
    // One slot, filled, then a send that must fail: a wake for a message
    // that is not in the ring would wake a core to find nothing, which is
    // the spin this work removes rather than adds.
    auto transport = RealRingTransport::Create(2, 1, 64);
    ASSERT_TRUE(transport.ok());

    WakeCountingBackend backend;
    std::atomic<bool> sleeping{true};
    transport.value().RegisterWaker(1, CoreWakeHandle{&sleeping, &backend});

    ASSERT_TRUE(transport.value().TrySend(MessageTo(1), {}).ok());
    EXPECT_EQ(backend.wakes.load(), 1);

    ASSERT_FALSE(transport.value().TrySend(MessageTo(1), {}).ok());
    EXPECT_EQ(backend.wakes.load(), 1) << "a refused send woke the target anyway";
}

TEST(ReactorWakeTest, AnUnregisteredCoreIsSimplyNeverWoken) {
    // The pre-wake-path behaviour, kept reachable on purpose: a core that
    // never attached a transport pays its idle block and nothing crashes.
    auto transport = RealRingTransport::Create(2, 8, 64);
    ASSERT_TRUE(transport.ok());
    EXPECT_TRUE(transport.value().TrySend(MessageTo(1), {}).ok());
}

// ---- The sleeper's half: the peek before the block ----------------------

// Records the timeout the reactor asked for, which is the only place the
// idle decision is observable from outside.
class TimeoutRecordingBackend final : public IoBackend {
public:
    Status Register(IoHandle, IoInterest) override { return Status::OK(); }
    Status Modify(IoHandle, IoInterest) override { return Status::OK(); }
    Status Unregister(IoHandle) override { return Status::OK(); }
    Status PollReady(int timeout_ms, std::vector<IoEvent>&) override {
        timeouts.push_back(timeout_ms);
        return Status::OK();
    }
    void Wake() noexcept override {}

    std::vector<int> timeouts;
};

TEST(ReactorWakeTest, APendingMessageCancelsTheIdleBlock) {
    ManualClock clock;
    TimeoutRecordingBackend io;
    auto transport = RealRingTransport::Create(2, 8, 64);
    ASSERT_TRUE(transport.ok());

    Scheduler scheduler(clock, io);
    scheduler.AttachTransport(&transport.value(), /*core_id=*/1);

    // Nothing ready, no timers: the reactor would block for its cap. The
    // message is already in the ring - the case a peer produces when it
    // read `sleeping_ == false` a moment too early.
    ASSERT_TRUE(transport.value().TrySend(MessageTo(1), {}).ok());
    scheduler.RunOnce();

    ASSERT_EQ(io.timeouts.size(), 1u);
    EXPECT_EQ(io.timeouts[0], 0) << "the reactor slept on top of a message already delivered";
    EXPECT_EQ(scheduler.messages_drained(), 1u);
}

TEST(ReactorWakeTest, AnIdleReactorWithNothingPendingStillBlocks) {
    ManualClock clock;
    TimeoutRecordingBackend io;
    auto transport = RealRingTransport::Create(2, 8, 64);
    ASSERT_TRUE(transport.ok());

    SchedulerConfig config;
    config.max_idle_block_ms = 7;
    Scheduler scheduler(clock, io, config);
    scheduler.AttachTransport(&transport.value(), 1);

    scheduler.RunOnce();

    ASSERT_EQ(io.timeouts.size(), 1u);
    EXPECT_EQ(io.timeouts[0], 7) << "the peek turned every idle block into a spin";
}

// ---- End to end ---------------------------------------------------------

TEST(ReactorWakeTest, AMessageToASleepingReactorDoesNotWaitOutItsBlock) {
    ManualClock clock;
    auto backend = EpollIoBackend::Create();
    ASSERT_TRUE(backend.ok());
    auto transport = RealRingTransport::Create(2, 8, 64);
    ASSERT_TRUE(transport.ok());

    SchedulerConfig config;
    // The whole test: a block long enough that waiting it out is
    // indistinguishable from a hang. Before the wake path this reactor
    // would notice the message in 30 seconds.
    config.max_idle_block_ms = 30'000;
    Scheduler scheduler(clock, backend.value(), config);
    scheduler.AttachTransport(&transport.value(), /*core_id=*/1);

    std::atomic<bool> handled{false};
    ASSERT_TRUE(scheduler
                    .RegisterMessageHandler(RingMessageKind::kStepBatch,
                                            [&](const MessageHeader&, std::span<const std::byte>) {
                                                handled.store(true);
                                                scheduler.Stop();
                                            })
                    .ok());

    std::thread reactor([&] { scheduler.Run(); });

    // Let it reach the block. Not a synchronization point - the eventfd
    // makes an early wake harmless - just a way to test the interesting
    // case rather than the trivial one.
    std::this_thread::sleep_for(milliseconds(50));

    const auto started = steady_clock::now();
    ASSERT_TRUE(transport.value().TrySend(MessageTo(1), {}).ok());

    while (!handled.load() && ElapsedMs(started) < 10'000) {
        std::this_thread::sleep_for(milliseconds(1));
    }
    const auto latency = ElapsedMs(started);

    if (!handled.load()) {
        // Never leave a hung reactor behind: the join below would inherit
        // the hang and the suite would report a timeout instead of a
        // failure.
        scheduler.Stop();
        backend.value().Wake();
    }
    reactor.join();

    EXPECT_TRUE(handled.load()) << "the message waited out the whole idle block";
    EXPECT_LT(latency, 1'000) << "woken, but not promptly: " << latency << " ms";
}

}  // namespace
}  // namespace kds::sched
