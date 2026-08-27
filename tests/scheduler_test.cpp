#include "kds/sched/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/epoll_io_backend.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/waker.hpp"

// Phase-2 timers, driven off a ManualClock so "an interval elapsed" is a
// statement about the injected clock and never about wall time (rules.md
// section 4). The reactor's whole periodic-work story - the checkpoint
// cadence of wal.md section 11 above all - rests on these.

namespace kds::sched {
namespace {

// Records the timeout the reactor asked to wait for - the observable side
// of the idle policy, which is otherwise invisible from outside.
class RecordingIoBackend final : public IoBackend {
public:
    Status Register(IoHandle, IoInterest) override { return Status::OK(); }
    Status Modify(IoHandle, IoInterest) override { return Status::OK(); }
    Status Unregister(IoHandle) override { return Status::OK(); }
    Status PollReady(int timeout_ms, std::vector<IoEvent>&) override {
        timeouts.push_back(timeout_ms);
        return Status::OK();
    }

    std::vector<int> timeouts;
};

class SchedulerTimerTest : public ::testing::Test {
protected:
    ManualClock clock_;
    NullIoBackend io_;
    Scheduler scheduler_{clock_, io_};
};

TEST_F(SchedulerTimerTest, AOneShotFiresOnceItsDeadlinePasses) {
    int fired = 0;
    scheduler_.SubmitAt(100, [&] { ++fired; });

    scheduler_.RunOnce();
    EXPECT_EQ(fired, 0) << "fired before its deadline";

    clock_.SetNow(99);
    scheduler_.RunOnce();
    EXPECT_EQ(fired, 0);

    clock_.SetNow(100);  // deadline is inclusive
    scheduler_.RunOnce();
    EXPECT_EQ(fired, 1);

    // And never again - a one-shot is disarmed by firing.
    clock_.SetNow(1000);
    scheduler_.RunOnce();
    EXPECT_EQ(fired, 1);
    EXPECT_EQ(scheduler_.armed_timers(), 0u);
}

TEST_F(SchedulerTimerTest, ADeadlineAlreadyPastFiresNextIterationNotRetroactively) {
    clock_.SetNow(500);
    int fired = 0;
    scheduler_.SubmitAt(100, [&] { ++fired; });

    scheduler_.RunOnce();
    EXPECT_EQ(fired, 1) << "a past deadline should fire once, not once per missed instant";
}

TEST_F(SchedulerTimerTest, APeriodicTimerFiresOncePerElapsedPeriod) {
    int fired = 0;
    scheduler_.SubmitEvery(100, [&] { ++fired; });

    scheduler_.RunOnce();
    EXPECT_EQ(fired, 0) << "first firing is one period out, not immediate";

    for (int tick = 1; tick <= 5; ++tick) {
        clock_.SetNow(static_cast<MonoTimeNs>(tick) * 100);
        scheduler_.RunOnce();
        EXPECT_EQ(fired, tick);
    }
    EXPECT_EQ(scheduler_.armed_timers(), 1u) << "a periodic timer stays armed";
}

TEST_F(SchedulerTimerTest, APeriodicTimerDoesNotDriftFromASlowCallback) {
    std::vector<MonoTimeNs> deadlines;
    // The callback "takes" 60ns of clock each time it runs.
    scheduler_.SubmitEvery(100, [&] {
        deadlines.push_back(clock_.Now());
        clock_.Advance(60);
    });

    clock_.SetNow(100);
    scheduler_.RunOnce();
    clock_.SetNow(200);
    scheduler_.RunOnce();
    clock_.SetNow(300);
    scheduler_.RunOnce();

    // Re-armed from the deadline, not from completion: firings land on
    // 100/200/300, not 100/260/420.
    ASSERT_EQ(deadlines.size(), 3u);
    EXPECT_EQ(deadlines[0], 100u);
    EXPECT_EQ(deadlines[1], 200u);
    EXPECT_EQ(deadlines[2], 300u);
}

TEST_F(SchedulerTimerTest, MissedPeriodsCoalesceIntoASingleFiring) {
    int fired = 0;
    scheduler_.SubmitEvery(100, [&] { ++fired; });

    // Jump four periods in one go, as a stalled reactor would. The timer
    // fires once, not four times - a burst of back-to-back checkpoints on
    // recovery is the opposite of what a cadence is for.
    clock_.SetNow(400);
    scheduler_.RunOnce();
    EXPECT_EQ(fired, 1);

    scheduler_.RunOnce();
    EXPECT_EQ(fired, 1) << "the skipped periods must not be replayed";

    // And the schedule resumes on the original phase, not offset by the
    // stall: the next deadline is 500, not 400 + 100.
    clock_.SetNow(500);
    scheduler_.RunOnce();
    EXPECT_EQ(fired, 2);
}

TEST_F(SchedulerTimerTest, CancelDisarmsBeforeFiring) {
    int fired = 0;
    TimerId id = scheduler_.SubmitAt(100, [&] { ++fired; });
    scheduler_.CancelTimer(id);

    clock_.SetNow(1000);
    scheduler_.RunOnce();
    EXPECT_EQ(fired, 0);
    EXPECT_EQ(scheduler_.armed_timers(), 0u);
}

TEST_F(SchedulerTimerTest, CancelStopsAPeriodicTimerFromInsideItsOwnCallback) {
    int fired = 0;
    TimerId id = kInvalidTimerId;
    id = scheduler_.SubmitEvery(100, [&] {
        ++fired;
        if (fired == 2) scheduler_.CancelTimer(id);
    });

    for (int tick = 1; tick <= 5; ++tick) {
        clock_.SetNow(static_cast<MonoTimeNs>(tick) * 100);
        scheduler_.RunOnce();
    }
    EXPECT_EQ(fired, 2);
    EXPECT_EQ(scheduler_.armed_timers(), 0u);
}

TEST_F(SchedulerTimerTest, CancellingAnAlreadyFiredOrUnknownIdIsHarmless) {
    int fired = 0;
    TimerId id = scheduler_.SubmitAt(100, [&] { ++fired; });
    clock_.SetNow(100);
    scheduler_.RunOnce();
    ASSERT_EQ(fired, 1);

    scheduler_.CancelTimer(id);            // already fired
    scheduler_.CancelTimer(kInvalidTimerId);
    scheduler_.CancelTimer(999999);        // never issued

    int other = 0;
    scheduler_.SubmitAt(200, [&] { ++other; });
    clock_.SetNow(200);
    scheduler_.RunOnce();
    EXPECT_EQ(other, 1) << "a stale cancel must not disarm somebody else's timer";
}

TEST_F(SchedulerTimerTest, TimersFireInDeadlineOrderNotSubmissionOrder) {
    std::vector<int> order;
    scheduler_.SubmitAt(300, [&] { order.push_back(3); });
    scheduler_.SubmitAt(100, [&] { order.push_back(1); });
    scheduler_.SubmitAt(200, [&] { order.push_back(2); });

    clock_.SetNow(1000);
    scheduler_.RunOnce();
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST_F(SchedulerTimerTest, ATimerCallbackCanArmAnotherTimer) {
    int inner = 0;
    scheduler_.SubmitAt(100, [&] { scheduler_.SubmitAt(200, [&] { ++inner; }); });

    clock_.SetNow(100);
    scheduler_.RunOnce();
    EXPECT_EQ(inner, 0);

    clock_.SetNow(200);
    scheduler_.RunOnce();
    EXPECT_EQ(inner, 1);
}

TEST_F(SchedulerTimerTest, ATimerCallbackCanSubmitATaskThatRunsTheSameIteration) {
    int ran = 0;
    scheduler_.SubmitAt(100, [&] {
        scheduler_.Submit(std::make_unique<FunctionTask>(SchedulingGroup::kSystem, [&] {
            ++ran;
            return PollResult::kDone;
        }));
    });

    clock_.SetNow(100);
    // Phase 2 runs before phase 4, so work a timer submits is picked up in
    // the same iteration rather than waiting for the next one.
    scheduler_.RunOnce();
    EXPECT_EQ(ran, 1);
}

TEST_F(SchedulerTimerTest, AParkedTaskIsPolledAtMostOncePerIteration) {
    // The lease-refill trace (docs/inflight/in-progress/workplan-peer-writer.md PW7): a parked
    // coroutine answers kSuspended in nanoseconds, and the loop budget used
    // to re-poll it until the budget ran out - 64 polls an iteration, every
    // one charged to its group's share.
    int polls = 0;
    scheduler_.Submit(std::make_unique<FunctionTask>(SchedulingGroup::kSystem, [&] {
        ++polls;
        return PollResult::kSuspended;
    }));
    scheduler_.RunOnce();
    EXPECT_EQ(polls, 1);
    scheduler_.RunOnce();
    EXPECT_EQ(polls, 2);
}

TEST_F(SchedulerTimerTest, AGroupInDebtStillGetsOnePollPerIteration) {
    // The same trace's other half. A system task that consumed 10 ms leaves
    // its group (share 50) owing the foreground (share 1000) two hundred
    // milliseconds of polls, and a cheap foreground task that never
    // finishes would, under the share law alone, keep the *next* system
    // task from its first poll until that debt was paid - hundreds of
    // iterations on a peer whose statements spend their time in the
    // drain's fdatasync, outside every group's account. One poll per ready
    // group per iteration is the floor.
    scheduler_.Submit(std::make_unique<FunctionTask>(SchedulingGroup::kSystem, [&] {
        clock_.Advance(10'000'000);  // 10 ms of system-group runtime
        return PollResult::kDone;
    }));
    scheduler_.RunOnce();

    int fg_polls = 0;
    scheduler_.Submit(std::make_unique<FunctionTask>(SchedulingGroup::kForeground, [&] {
        ++fg_polls;
        clock_.Advance(1'000);
        return PollResult::kSuspended;
    }));
    int sys_polls = 0;
    scheduler_.Submit(std::make_unique<FunctionTask>(SchedulingGroup::kSystem, [&] {
        ++sys_polls;
        return PollResult::kDone;
    }));
    scheduler_.RunOnce();
    EXPECT_EQ(sys_polls, 1) << "the new system task must be polled in the iteration it was "
                               "ready for, whatever its group's ratio says";
    EXPECT_EQ(fg_polls, 1) << "and the suspended foreground task is polled once, not 64 times";
}

// ---- Group accounting read from outside (sched.md §4, T4) --------------
//
// §4's last bullet - "reactor time spent outside task polls (the drain, the
// idle block) is charged to no group" - was unmeasurable from outside the
// process until these accessors existed: `bench/v2.1.0` §11-5 records that
// the counter was private and `SHOW META` never printed it.

TEST_F(SchedulerTimerTest, PolledTimeAndPollCountsAreChargedToTheTasksGroup) {
    scheduler_.Submit(std::make_unique<FunctionTask>(SchedulingGroup::kSystem, [&] {
        clock_.Advance(5'000);
        return PollResult::kDone;
    }));
    scheduler_.Submit(std::make_unique<FunctionTask>(SchedulingGroup::kForeground, [&] {
        clock_.Advance(2'000);
        return PollResult::kDone;
    }));
    scheduler_.RunOnce();

    EXPECT_EQ(scheduler_.polls_total(SchedulingGroup::kSystem), 1u);
    EXPECT_EQ(scheduler_.polls_total(SchedulingGroup::kForeground), 1u);
    EXPECT_EQ(scheduler_.polls_total(SchedulingGroup::kMaintenance), 0u);
    EXPECT_EQ(scheduler_.polled_ns_total(SchedulingGroup::kSystem), 5'000u);
    EXPECT_EQ(scheduler_.polled_ns_total(SchedulingGroup::kForeground), 2'000u);
    EXPECT_EQ(scheduler_.polled_ns_total(SchedulingGroup::kMaintenance), 0u);
}

TEST_F(SchedulerTimerTest, TheCumulativeCounterSurvivesTheDecayTheShareLawApplies) {
    // The whole reason there are two counters. `consumed_ns_` is halved once
    // it passes `decay_threshold_ns` so history does not dominate the pick;
    // an accounting total that halved itself would understate every group it
    // described, and by an amount that depends on when it was read.
    SchedulerConfig config;
    config.decay_threshold_ns = 1'000;
    Scheduler sched(clock_, io_, config);
    sched.Submit(std::make_unique<FunctionTask>(SchedulingGroup::kSystem, [&] {
        clock_.Advance(4'000);   // past the threshold, so the decay fires
        return PollResult::kDone;
    }));
    sched.RunOnce();

    EXPECT_EQ(sched.polled_ns_total(SchedulingGroup::kSystem), 4'000u)
        << "the accounting total is cumulative and never decays";
    EXPECT_EQ(sched.consumed_ns(SchedulingGroup::kSystem), 2'000u)
        << "the share law's own counter is halved past the threshold";
}

TEST_F(SchedulerTimerTest, WallTimeIsZeroBeforeTheFirstIterationAndSpansItAfter) {
    // A clock that legally reads 0 at its first tick is why the start is
    // flagged rather than sentinelled on a zero timestamp.
    EXPECT_EQ(scheduler_.run_wall_ns(), 0u);
    clock_.SetNow(0);
    scheduler_.RunOnce();
    EXPECT_EQ(scheduler_.run_wall_ns(), 0u) << "started at t=0, no time has passed yet";
    clock_.SetNow(7'000);
    EXPECT_EQ(scheduler_.run_wall_ns(), 7'000u);
    scheduler_.RunOnce();
    clock_.SetNow(9'000);
    EXPECT_EQ(scheduler_.run_wall_ns(), 9'000u) << "the origin is the FIRST iteration";
}

TEST_F(SchedulerTimerTest, TimeOutsideTaskPollsIsWhatTheAccountingGapMeasures) {
    // The measurement §4 owes, in miniature: a reactor iteration that
    // advances the clock outside any poll - which is what the WAL drain's
    // fdatasync does on a real core - leaves wall time above the sum of the
    // groups' polled time, and the difference is charged to nobody.
    clock_.SetNow(0);
    scheduler_.Submit(std::make_unique<FunctionTask>(SchedulingGroup::kForeground, [&] {
        clock_.Advance(1'000);
        return PollResult::kDone;
    }));
    scheduler_.RunOnce();
    clock_.Advance(9'000);   // stands in for the drain: outside every poll

    std::uint64_t polled = 0;
    for (SchedulingGroup g : {SchedulingGroup::kForeground, SchedulingGroup::kMaintenance,
                              SchedulingGroup::kSystem}) {
        polled += scheduler_.polled_ns_total(g);
    }
    EXPECT_EQ(polled, 1'000u);
    EXPECT_EQ(scheduler_.run_wall_ns(), 10'000u);
    EXPECT_EQ(scheduler_.run_wall_ns() - polled, 9'000u)
        << "the gap is exactly the time no group was charged for";
}

TEST_F(SchedulerTimerTest, RunStopsWhenATimerCallbackCallsStop) {
    int fired = 0;
    scheduler_.SubmitEvery(100, [&] {
        ++fired;
        if (fired == 3) scheduler_.Stop();
    });

    // Under a ManualClock nothing moves time on its own, so a resident
    // task drives it - standing in for the wall clock a real reactor runs
    // against. It also keeps a ready task in the queue, which is what
    // holds the idle timeout at 0 so Run() keeps iterating.
    scheduler_.Submit(std::make_unique<FunctionTask>(SchedulingGroup::kForeground, [&] {
        clock_.Advance(40);
        return PollResult::kSuspended;
    }));

    scheduler_.Run();  // must terminate
    EXPECT_EQ(fired, 3);
}

TEST_F(SchedulerTimerTest, AnIdleReactorWithNoTimersDoesNotSpinOnAZeroTimeout) {
    // Nothing armed and nothing ready: the poll should be allowed to block
    // up to the configured cap rather than returning immediately forever,
    // which is what made the old Phase-1 loop burn a core while idle.
    SchedulerConfig config;
    config.max_idle_block_ms = 7;
    RecordingIoBackend io;
    Scheduler scheduler(clock_, io, config);

    scheduler.RunOnce();
    ASSERT_EQ(io.timeouts.size(), 1u);
    EXPECT_EQ(io.timeouts[0], 7);
}

TEST_F(SchedulerTimerTest, APendingTimerShortensTheIdleBlockToItsDeadline) {
    SchedulerConfig config;
    config.max_idle_block_ms = 1000;
    RecordingIoBackend io;
    Scheduler scheduler(clock_, io, config);

    scheduler.SubmitAt(3'000'000, [] {});  // 3 ms out
    scheduler.RunOnce();
    ASSERT_EQ(io.timeouts.size(), 1u);
    EXPECT_EQ(io.timeouts[0], 3) << "the reactor must wake for its own timer, not sleep past it";
}

TEST_F(SchedulerTimerTest, AReadyTaskDropsTheIdleBlockToZero) {
    SchedulerConfig config;
    config.max_idle_block_ms = 1000;
    RecordingIoBackend io;
    Scheduler scheduler(clock_, io, config);

    scheduler.Submit(std::make_unique<FunctionTask>(SchedulingGroup::kForeground,
                                                     [] { return PollResult::kDone; }));
    scheduler.RunOnce();
    ASSERT_EQ(io.timeouts.size(), 1u);
    EXPECT_EQ(io.timeouts[0], 0) << "runnable work must never wait on the io backend";
}

// ---- Phase 3: the cross-core inbox drain (sched.md §5, workplan P1) ----
//
// What the drain owes its callers: a received message becomes a *task*, in
// the group the **sender** designated, run in phase 4 like any other - not
// work done inside the drain. And a message nobody handles is dropped
// rather than fatal, because a request can be torn down while its messages
// are still in flight (workplan guideline 5).

std::vector<std::byte> PayloadOf(std::string_view s) {
    std::vector<std::byte> out(s.size());
    if (!s.empty()) std::memcpy(out.data(), s.data(), s.size());
    return out;
}

MessageHeader MessageTo(std::uint32_t dst, RingMessageKind kind, SchedulingGroup group) {
    MessageHeader h{};
    h.request_id = 1;
    h.src_core = 0;
    h.dst_core = dst;
    h.session_core = 0;
    h.kind = static_cast<std::uint16_t>(kind);
    h.sched_group = static_cast<std::uint16_t>(group);
    return h;
}

class SchedulerInboxTest : public ::testing::Test {
protected:
    ManualClock clock_;
    NullIoBackend io_;
};

TEST_F(SchedulerInboxTest, WithNoTransportPhaseThreeIsANoOp) {
    // The single-core build (workplan guideline 2): the phase is present in
    // the fixed order and costs one null test.
    Scheduler scheduler(clock_, io_);
    scheduler.RunOnce();
    EXPECT_EQ(scheduler.messages_drained(), 0u);
}

TEST_F(SchedulerInboxTest, AReceivedMessageBecomesATaskInTheSendersGroup) {
    auto transport = RealRingTransport::Create(2, 8, 64);
    ASSERT_TRUE(transport.ok());

    Scheduler scheduler(clock_, io_);
    ASSERT_TRUE(scheduler.AttachTransport(&transport.value(), /*core_id=*/1).ok());

    SchedulingGroup ran_in = SchedulingGroup::kSystem;
    std::string got_payload;
    bool handled = false;
    ASSERT_TRUE(scheduler
                    .RegisterMessageHandler(RingMessageKind::kStepBatch,
                                            [&](const MessageHeader& h,
                                                std::span<const std::byte> payload) {
                                                handled = true;
                                                ran_in = GroupOf(h);
                                                got_payload.assign(
                                                    reinterpret_cast<const char*>(payload.data()),
                                                    payload.size());
                                            })
                    .ok());

    ASSERT_TRUE(transport.value()
                    .TrySend(MessageTo(1, RingMessageKind::kStepBatch,
                                        SchedulingGroup::kMaintenance),
                             PayloadOf("rows"))
                    .ok());

    scheduler.RunOnce();
    EXPECT_EQ(scheduler.messages_drained(), 1u);
    EXPECT_TRUE(handled);
    // The sender chose `maintenance`; the receiver must not substitute its
    // own idea of what this kind is worth (sched.md §5).
    EXPECT_EQ(ran_in, SchedulingGroup::kMaintenance);
    EXPECT_EQ(got_payload, "rows");
}

TEST_F(SchedulerInboxTest, TheHandlerRunsInPhaseFourAndNotInsideTheDrain) {
    // The drain has to stay cheap and bounded, exactly as the phase-1 io
    // handlers do. The observable form of that: the handler has not run
    // when the drain finishes, only when tasks do.
    SchedulerConfig config;
    config.max_tasks_per_iteration = 0;  // phase 4 runs nothing this iteration
    auto transport = RealRingTransport::Create(2, 8, 64);
    ASSERT_TRUE(transport.ok());

    Scheduler scheduler(clock_, io_, config);
    ASSERT_TRUE(scheduler.AttachTransport(&transport.value(), 1).ok());

    bool handled = false;
    ASSERT_TRUE(scheduler
                    .RegisterMessageHandler(
                        RingMessageKind::kStepBatch,
                        [&](const MessageHeader&, std::span<const std::byte>) { handled = true; })
                    .ok());
    ASSERT_TRUE(transport.value()
                    .TrySend(MessageTo(1, RingMessageKind::kStepBatch,
                                        SchedulingGroup::kForeground),
                             PayloadOf("x"))
                    .ok());

    scheduler.RunOnce();
    EXPECT_EQ(scheduler.messages_drained(), 1u) << "the message was not taken off the ring";
    EXPECT_FALSE(handled) << "the drain did the work itself instead of queuing a task";
}

TEST_F(SchedulerInboxTest, AMessageWithNoHandlerIsDroppedAndNotFatal) {
    auto transport = RealRingTransport::Create(2, 8, 64);
    ASSERT_TRUE(transport.ok());

    Scheduler scheduler(clock_, io_);
    ASSERT_TRUE(scheduler.AttachTransport(&transport.value(), 1).ok());

    ASSERT_TRUE(transport.value()
                    .TrySend(MessageTo(1, RingMessageKind::kStepCancel,
                                        SchedulingGroup::kForeground),
                             PayloadOf("late"))
                    .ok());

    // Normal operation, not an error: a cancel can outlive the request it
    // belonged to. The reactor keeps going and the message is consumed.
    EXPECT_TRUE(scheduler.RunOnce());
    EXPECT_EQ(scheduler.messages_drained(), 1u);

    MessageHeader header{};
    std::vector<std::byte> payload;
    EXPECT_FALSE(transport.value().TryReceive(1, header, payload)) << "the message was left behind";
}

TEST_F(SchedulerInboxTest, TheDrainIsBoundedByItsLoopBudget) {
    // sched.md §2: a phase may not run unboundedly, or a flooded core never
    // reaches its I/O completions. What is left on the ring is picked up
    // next iteration, so nothing is lost by stopping early.
    SchedulerConfig config;
    config.max_messages_per_iteration = 2;
    auto transport = RealRingTransport::Create(2, 16, 64);
    ASSERT_TRUE(transport.ok());

    Scheduler scheduler(clock_, io_, config);
    ASSERT_TRUE(scheduler.AttachTransport(&transport.value(), 1).ok());
    ASSERT_TRUE(scheduler
                    .RegisterMessageHandler(
                        RingMessageKind::kStepBatch,
                        [](const MessageHeader&, std::span<const std::byte>) {})
                    .ok());

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(transport.value()
                        .TrySend(MessageTo(1, RingMessageKind::kStepBatch,
                                            SchedulingGroup::kForeground),
                                 PayloadOf("x"))
                        .ok());
    }

    scheduler.RunOnce();
    EXPECT_EQ(scheduler.messages_drained(), 2u);
    scheduler.RunOnce();
    EXPECT_EQ(scheduler.messages_drained(), 4u);
    scheduler.RunOnce();
    EXPECT_EQ(scheduler.messages_drained(), 5u);
}

TEST_F(SchedulerInboxTest, AHandlerForAKindThisBuildDoesNotKnowIsRefused) {
    // Including kUnset. A handler bound to a number no sender can produce
    // is a silent no-op, and the central kind enum exists so that number
    // does not exist.
    Scheduler scheduler(clock_, io_);
    EXPECT_EQ(scheduler
                  .RegisterMessageHandler(
                      RingMessageKind::kUnset,
                      [](const MessageHeader&, std::span<const std::byte>) {})
                  .code(),
              StatusCode::kInvalidArgument);
}

TEST_F(SchedulerInboxTest, PhaseOrderIsUnchangedByTheDrain) {
    // Phases run in the fixed order of sched.md §2, and phase 3 sits
    // between timers and ready tasks. A timer armed for now must therefore
    // fire before a message received this same iteration is handled.
    auto transport = RealRingTransport::Create(2, 8, 64);
    ASSERT_TRUE(transport.ok());

    Scheduler scheduler(clock_, io_);
    ASSERT_TRUE(scheduler.AttachTransport(&transport.value(), 1).ok());

    std::vector<std::string> order;
    scheduler.SubmitAt(0, [&] { order.push_back("timer"); });
    ASSERT_TRUE(scheduler
                    .RegisterMessageHandler(RingMessageKind::kStepBatch,
                                            [&](const MessageHeader&,
                                                std::span<const std::byte>) {
                                                order.push_back("message");
                                            })
                    .ok());
    ASSERT_TRUE(transport.value()
                    .TrySend(MessageTo(1, RingMessageKind::kStepBatch,
                                        SchedulingGroup::kForeground),
                             PayloadOf("x"))
                    .ok());

    scheduler.RunOnce();
    EXPECT_EQ(order, (std::vector<std::string>{"timer", "message"}));
}

// ---- The wake path (sched/waker.hpp) ------------------------------------
//
// A reactor with nothing to run blocks in its I/O backend, and until this
// existed a ring message could not interrupt that block: sockets and timers
// are things the kernel knows about, a store to shared memory is not. The
// cost was measured rather than guessed - SS-B put a flat 1,064 µs on every
// shipped statement, tracking the idle block over a fivefold range
// (`bench/v2.2.0/results-shipping-ssb-v2.2.0-11-g982e133.md` §4a).
//
// These tests use the **real** epoll backend, because the thing under test
// is an fd and a block; a NullIoBackend never blocks and would pass them
// without the feature existing.

class SchedulerWakeTest : public ::testing::Test {
protected:
    SystemClock clock_;
};

TEST_F(SchedulerWakeTest, AWakerIsReadableOnceWrittenAndNotBefore) {
    auto waker = Waker::Create();
    ASSERT_TRUE(waker.ok()) << waker.status().message();
    auto backend = EpollIoBackend::Create();
    ASSERT_TRUE(backend.ok()) << backend.status().message();
    ASSERT_TRUE(backend.value().Register(waker.value().handle(), IoInterest::kReadable).ok());

    std::vector<IoEvent> events;
    ASSERT_TRUE(backend.value().PollReady(0, events).ok());
    EXPECT_TRUE(events.empty()) << "an unwritten waker must not report readable";

    waker.value().Wake();
    ASSERT_TRUE(backend.value().PollReady(0, events).ok());
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].handle, waker.value().handle());
    EXPECT_EQ(waker.value().wakes(), 1u);
    EXPECT_EQ(waker.value().wake_failures(), 0u);

    // Drained, it goes quiet again - and N wakes before a drain are one
    // wake, which is the correct reading: the ring is the queue and this
    // only says "look at it".
    waker.value().Wake();
    waker.value().Wake();
    waker.value().Drain();
    events.clear();
    ASSERT_TRUE(backend.value().PollReady(0, events).ok());
    EXPECT_TRUE(events.empty());
}

TEST_F(SchedulerWakeTest, ASingleCoreReactorArmsNoWakeAtAll) {
    // Guideline 2: no transport, no fd, no flag stores, no syscalls. The
    // fast path is what it was.
    NullIoBackend io;
    Scheduler scheduler(clock_, io);
    EXPECT_FALSE(scheduler.wake_armed());
    scheduler.RunOnce();
    EXPECT_EQ(scheduler.idle_blocks(), 0u);
}

TEST_F(SchedulerWakeTest, AMessageAlreadyQueuedIsNotSleptThrough) {
    // The pre-block re-check, driven deterministically: with a message
    // already in the ring and an idle block of a full second, `RunOnce`
    // must decline to block at all. Without the re-check this test takes a
    // second and the message waits it out.
    auto transport = RealRingTransport::Create(2, 8, 64);
    ASSERT_TRUE(transport.ok());
    auto backend = EpollIoBackend::Create();
    ASSERT_TRUE(backend.ok());

    SchedulerConfig config;
    config.max_idle_block_ms = 1000;
    Scheduler scheduler(clock_, backend.value(), config);
    ASSERT_TRUE(scheduler.AttachTransport(&transport.value(), /*core_id=*/1).ok());
    ASSERT_TRUE(scheduler.wake_armed());

    bool handled = false;
    ASSERT_TRUE(scheduler
                    .RegisterMessageHandler(RingMessageKind::kStepBatch,
                                            [&](const MessageHeader&,
                                                std::span<const std::byte>) { handled = true; })
                    .ok());

    MessageHeader header{};
    header.src_core = 0;
    header.dst_core = 1;
    header.kind = static_cast<std::uint16_t>(RingMessageKind::kStepBatch);
    header.sched_group = static_cast<std::uint16_t>(SchedulingGroup::kSystem);
    ASSERT_TRUE(transport.value().TrySend(header, {}).ok());

    // One iteration: phase 3 drains and phase 4 runs what it queued. A
    // second is deliberately not taken - it would find an empty inbox and
    // block the full second, which is correct behaviour and would only
    // measure the timeout.
    const auto started = std::chrono::steady_clock::now();
    scheduler.RunOnce();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_TRUE(handled);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500)
        << "the reactor slept on an inbox that was not empty";
    EXPECT_GE(scheduler.wake_race_skips(), 1u)
        << "the pre-block re-check is what must have found it";
}

TEST_F(SchedulerWakeTest, AMessageToABlockedReactorArrivesWithoutWaitingOutTheBlock) {
    // **The fix itself.** Core 1 blocks for a full second with nothing to
    // do; core 0 sends. The message must arrive in a small fraction of that
    // block, and before this path existed it could not - it waited for the
    // block to expire on its own, which is the millisecond SS-B measured on
    // every shipped statement (there, twice).
    auto transport = RealRingTransport::Create(2, 8, 64);
    ASSERT_TRUE(transport.ok());
    auto backend = EpollIoBackend::Create();
    ASSERT_TRUE(backend.ok());

    SchedulerConfig config;
    config.max_idle_block_ms = 1000;
    Scheduler scheduler(clock_, backend.value(), config);
    ASSERT_TRUE(scheduler.AttachTransport(&transport.value(), /*core_id=*/1).ok());

    std::atomic<bool> handled{false};
    ASSERT_TRUE(scheduler
                    .RegisterMessageHandler(RingMessageKind::kStepBatch,
                                            [&](const MessageHeader&,
                                                std::span<const std::byte>) {
                                                handled.store(true);
                                            })
                    .ok());

    std::thread reactor([&] { scheduler.Run(); });

    // Let core 1 reach its block. This is the one place a sleep is the
    // point rather than a smell: the test is about what happens to a
    // reactor that is *already* asleep.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_GE(scheduler.idle_blocks(), 1u) << "the reactor never blocked; the test proves nothing";

    MessageHeader header{};
    header.src_core = 0;
    header.dst_core = 1;
    header.kind = static_cast<std::uint16_t>(RingMessageKind::kStepBatch);
    header.sched_group = static_cast<std::uint16_t>(SchedulingGroup::kSystem);
    const auto sent = std::chrono::steady_clock::now();
    ASSERT_TRUE(transport.value().TrySend(header, {}).ok());

    while (!handled.load() &&
           std::chrono::steady_clock::now() - sent < std::chrono::milliseconds(900)) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    const auto latency = std::chrono::steady_clock::now() - sent;

    scheduler.Stop();
    // The stop flag is read by the reactor's own thread, so it needs a wake
    // of its own to be noticed promptly - which this path now provides for
    // free. A send is what carries it.
    (void)transport.value().TrySend(header, {});
    reactor.join();

    EXPECT_TRUE(handled.load()) << "the message never arrived";
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(latency).count(), 100)
        << "the message waited out the idle block instead of interrupting it";
    EXPECT_GE(transport.value().wakes_sent(), 1u)
        << "the sender never wrote a wake, so the arrival was luck";
    // D7's pair, from the two ends: what the transport wrote is what this
    // reactor's own eventfd received, and `SHOW META` prints both so the
    // sum over cores can be checked against the instance total.
    EXPECT_GE(scheduler.wakes_received(), 1u) << "the wake reached no eventfd";
    EXPECT_EQ(scheduler.wakes_sent(), transport.value().wakes_sent());
}

// The other half of the wake's contract, and the half a latency test cannot
// see: **what the sender does not do.** Both of these are properties the
// path's own comments claim, and neither shows up as a wrong answer when it
// breaks - only as syscalls nobody asked for.

TEST_F(SchedulerWakeTest, AnAwakeTargetIsNeverWoken) {
    // Why this matters and is not a micro-optimisation: a write to an
    // eventfd is a syscall on the *sender's* critical path, and the cells
    // shipping is already fast in (0.93-0.99x from four sessions up) are
    // exactly the ones where the owner is never asleep. Waking
    // unconditionally would pay for every one of them.
    auto transport = RealRingTransport::Create(2, 8, 64);
    ASSERT_TRUE(transport.ok());
    auto backend = EpollIoBackend::Create();
    ASSERT_TRUE(backend.ok());

    Scheduler scheduler(clock_, backend.value());
    ASSERT_TRUE(scheduler.AttachTransport(&transport.value(), /*core_id=*/1).ok());
    ASSERT_TRUE(scheduler.wake_armed());

    // The reactor is not running, so its `sleeping` flag is clear - the
    // same state a busy reactor is in between blocks.
    MessageHeader header{};
    header.src_core = 0;
    header.dst_core = 1;
    header.kind = static_cast<std::uint16_t>(RingMessageKind::kStepBatch);
    header.sched_group = static_cast<std::uint16_t>(SchedulingGroup::kSystem);
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(transport.value().TrySend(header, {}).ok());

    EXPECT_EQ(transport.value().wakes_sent(), 0u)
        << "the sender paid a syscall for a target that was not asleep";
}

TEST_F(SchedulerWakeTest, ARefusedSendWakesNobody) {
    // A wake for a message that is not in the ring wakes a core to find
    // nothing - the spin this path exists to remove, reintroduced at the
    // sender. One slot, filled, then a send that must fail.
    auto transport = RealRingTransport::Create(2, /*capacity_slots=*/1, 64);
    ASSERT_TRUE(transport.ok());
    auto waker = Waker::Create();
    ASSERT_TRUE(waker.ok());

    // No reactor here: the flag is driven by hand, which is the only way to
    // hold a target "asleep" across a send that is going to be refused.
    std::atomic<bool> sleeping{true};
    transport.value().SetWakeTarget(1, WakeTarget{&sleeping, &waker.value()});

    MessageHeader header{};
    header.src_core = 0;
    header.dst_core = 1;
    header.kind = static_cast<std::uint16_t>(RingMessageKind::kStepBatch);
    header.sched_group = static_cast<std::uint16_t>(SchedulingGroup::kSystem);

    ASSERT_TRUE(transport.value().TrySend(header, {}).ok());
    ASSERT_EQ(transport.value().wakes_sent(), 1u) << "a sleeping target must be woken";

    ASSERT_FALSE(transport.value().TrySend(header, {}).ok()) << "the ring should be full";
    EXPECT_EQ(transport.value().wakes_sent(), 1u)
        << "a refused send woke the target anyway, for a message it does not have";
}

TEST_F(SchedulerWakeTest, TheBlockAndTheWakesAroundItAreCounted) {
    // D7 of `instructions/v2.3.0-reactor-wake.md`: the idle block used to
    // be invisible from outside the process, so `sched_wall_us - sum(
    // polled_us)` was sleep and work in one lump. These four counters are
    // what separate them, and each is asserted against a fact the test
    // arranges rather than against itself.
    auto transport = RealRingTransport::Create(2, 8, 64);
    ASSERT_TRUE(transport.ok());
    auto backend = EpollIoBackend::Create();
    ASSERT_TRUE(backend.ok());

    SchedulerConfig config;
    config.max_idle_block_ms = 50;
    Scheduler scheduler(clock_, backend.value(), config);
    ASSERT_TRUE(scheduler.AttachTransport(&transport.value(), /*core_id=*/1).ok());

    EXPECT_EQ(scheduler.idle_block_ns(), 0u);
    EXPECT_EQ(scheduler.wakes_received(), 0u);
    EXPECT_EQ(scheduler.wakes_sent(), 0u);

    // One iteration with nothing to do: it blocks for the configured 50 ms
    // and the counter must show it. The bound is loose on purpose - this
    // asserts the block is *measured*, not how long a kernel sleeps.
    scheduler.RunOnce();
    EXPECT_GE(scheduler.idle_blocks(), 1u);
    EXPECT_GE(scheduler.idle_block_ns(), 10'000'000u)
        << "a 50 ms block was not accounted for";
    const std::uint64_t blocked_once = scheduler.idle_block_ns();

    // Now the wake side. The reactor is not running, so it is not asleep
    // and the send must not wake it - which also makes the next iteration
    // the one that both drains and reads the eventfd.
    MessageHeader header{};
    header.src_core = 0;
    header.dst_core = 1;
    header.kind = static_cast<std::uint16_t>(RingMessageKind::kStepBatch);
    header.sched_group = static_cast<std::uint16_t>(SchedulingGroup::kSystem);
    ASSERT_TRUE(transport.value().TrySend(header, {}).ok());
    EXPECT_EQ(scheduler.wakes_sent(), 0u) << "an awake target was woken";

    // The queued message is drained by the next iteration, which therefore
    // does not block at all: the block counter must not move, and with no
    // wake ever written nothing can have been spurious.
    scheduler.RunOnce();
    EXPECT_EQ(scheduler.idle_block_ns(), blocked_once)
        << "an iteration with a message waiting still slept";
    EXPECT_EQ(scheduler.spurious_wakes(), 0u)
        << "no wake has been written, so nothing can have been spurious";
    EXPECT_EQ(scheduler.wakes_received(), 0u);
}

// ---- Parked is not ready (sched.md §7) ---------------------------------
//
// A task that suspends goes back on its queue, so "the queue is non-empty"
// was never the same question as "there is work to do". Reading one for the
// other is what made a reactor holding one parked coroutine spin at ~90% of
// a core, asking a predicate three million times a second
// (`bench/v2.2.0/results-shipping-ssb-v2.2.0-11-g982e133.md` §7).
//
// The rule these pin: the reactor may block only after a full iteration in
// which **nothing advanced** - no event, no timer, no message, no task that
// ran a line or finished, no task newly queued, and no work from the
// post-task hook. The last clause has its own test, because getting it
// wrong would put the WAL drain interval on every commit, which is a worse
// engine than the spin.

class SchedulerParkTest : public ::testing::Test {
protected:
    ManualClock clock_;
};

TEST_F(SchedulerParkTest, AReactorWhoseOnlyTaskIsParkedBlocks) {
    SchedulerConfig config;
    config.max_idle_block_ms = 1000;
    RecordingIoBackend io;
    Scheduler scheduler(clock_, io, config);

    bool never = false;
    auto waiter = [&never]() -> Coro {
        co_await WaitFor{&never};
        co_return Status::OK();
    };
    scheduler.Submit(MakeCoroTask(SchedulingGroup::kForeground, waiter()));

    // The block arrives one iteration after the last advancing one, and
    // that lag is the rule working rather than a rough edge: the reactor
    // sleeps on evidence it has collected, never on a prediction.
    //
    // 1: the task has never been polled - not parked, merely unexamined.
    //    The poll starts the coroutine, which runs to its `co_await`.
    // 2: still awake, because iteration 1 ran code.
    // 3: iteration 2's poll executed nothing, so now it may sleep.
    for (int i = 0; i < 3; ++i) scheduler.RunOnce();

    ASSERT_EQ(io.timeouts.size(), 3u);
    EXPECT_EQ(io.timeouts[0], 0) << "a task nobody has polled yet is not a parked one";
    EXPECT_EQ(io.timeouts[1], 0) << "the iteration that started the coroutine was progress";
    EXPECT_EQ(io.timeouts[2], 1000) << "the reactor spun on a parked coroutine";
    EXPECT_GE(scheduler.parked_idle_blocks(), 1u);

    // And it stays asleep: nothing about the next iteration is different.
    scheduler.RunOnce();
    ASSERT_EQ(io.timeouts.size(), 4u);
    EXPECT_EQ(io.timeouts[3], 1000);
}

TEST_F(SchedulerParkTest, ATaskThatRunsKeepsTheReactorAwake) {
    // The other direction, and the one a wrong rule breaks silently: a
    // coroutine that resumes, does work and parks again has advanced, and
    // sleeping on it would stall a chain mid-flight.
    SchedulerConfig config;
    config.max_idle_block_ms = 1000;
    RecordingIoBackend io;
    Scheduler scheduler(clock_, io, config);

    int steps = 0;
    auto stepper = [&steps]() -> Coro {
        for (int i = 0; i < 3; ++i) {
            ++steps;
            co_await Yield{};
        }
        co_return Status::OK();
    };
    scheduler.Submit(MakeCoroTask(SchedulingGroup::kForeground, stepper()));

    for (int i = 0; i < 4; ++i) scheduler.RunOnce();
    EXPECT_EQ(steps, 3);
    for (std::size_t i = 0; i < io.timeouts.size() && i < 4; ++i) {
        EXPECT_EQ(io.timeouts[i], 0) << "iteration " << i << " slept while a task was advancing";
    }
}

TEST_F(SchedulerParkTest, APollThatFinishesATaskCountsAsProgress) {
    // A parked coroutine alongside a task that completes. The completion is
    // progress, so the iteration after it must not sleep even though the
    // only thing left in the queue is a park - something finished, and what
    // it did may be what the parked task is waiting for.
    SchedulerConfig config;
    config.max_idle_block_ms = 1000;
    RecordingIoBackend io;
    Scheduler scheduler(clock_, io, config);

    bool never = false;
    auto waiter = [&never]() -> Coro {
        co_await WaitFor{&never};
        co_return Status::OK();
    };
    scheduler.Submit(MakeCoroTask(SchedulingGroup::kForeground, waiter()));
    scheduler.RunOnce();  // 1: coroutine starts and parks
    scheduler.RunOnce();  // 2: parked, nothing else - the last awake one

    scheduler.Submit(std::make_unique<FunctionTask>(SchedulingGroup::kForeground,
                                                     [] { return PollResult::kDone; }));
    scheduler.RunOnce();  // 3: the new task completes; the park is still there
    scheduler.RunOnce();  // 4: must not have slept on the way in

    ASSERT_EQ(io.timeouts.size(), 4u);
    EXPECT_EQ(io.timeouts[3], 0)
        << "the iteration that finished a task was not counted as progress";
}

TEST_F(SchedulerParkTest, ANonCoroutineTaskIsNeverTreatedAsParked) {
    // `Task::advanced_in_last_poll` defaults to true, and the default is
    // the safety margin: a task type that does not track parking keeps the
    // reactor awake rather than being slept through.
    SchedulerConfig config;
    config.max_idle_block_ms = 1000;
    RecordingIoBackend io;
    Scheduler scheduler(clock_, io, config);

    scheduler.Submit(std::make_unique<FunctionTask>(SchedulingGroup::kForeground,
                                                     [] { return PollResult::kSuspended; }));
    for (int i = 0; i < 3; ++i) scheduler.RunOnce();
    for (int timeout : io.timeouts) {
        EXPECT_EQ(timeout, 0) << "a task that does not report parking was slept through";
    }
    EXPECT_EQ(scheduler.parked_idle_blocks(), 0u);
}

TEST_F(SchedulerParkTest, TheHooksWorkIsProgressSoACommitDoesNotWaitOutABlock) {
    // **D5's hazard, and the reason the hook returns a bool.** This is the
    // group commit's shape: a statement stages, parks on a durability
    // watermark, and the *only* thing that moves that watermark is the
    // post-task hook running after phase 4. If a hook that synced were
    // counted as "nothing happened", the next iteration would block before
    // re-polling the waiter, and every commit would gain the drain
    // interval - here, a full second.
    SchedulerConfig config;
    config.max_idle_block_ms = 1000;
    RecordingIoBackend io;
    Scheduler scheduler(clock_, io, config);

    bool durable = false;
    bool finished = false;
    const std::function<bool()> is_durable = [&durable] { return durable; };
    auto committer = [&is_durable]() -> Coro {
        co_await WaitUntil{&is_durable};
        co_return Status::OK();
    };
    scheduler.Submit(MakeCoroTask(SchedulingGroup::kForeground, committer(),
                                  [&finished](const Status&) { finished = true; }));

    // The hook syncs on the iteration after the statement has staged, and
    // says so. One tick, then nothing more to do.
    int hook_calls = 0;
    scheduler.SetPostTaskHook([&] {
        ++hook_calls;
        if (hook_calls == 2 && !durable) {
            durable = true;
            return true;  // this tick did the work the waiter is parked on
        }
        return false;
    });

    scheduler.RunOnce();  // 1: task polled, parks
    scheduler.RunOnce();  // 2: still parked; hook makes it durable and says so
    ASSERT_FALSE(finished) << "the waiter is only re-polled on the next iteration";
    scheduler.RunOnce();  // 3: must not have blocked on the way in
    EXPECT_TRUE(finished) << "the commit waited out an idle block the hook should have prevented";

    ASSERT_GE(io.timeouts.size(), 3u);
    EXPECT_EQ(io.timeouts[2], 0)
        << "the reactor blocked between the hook's work and the waiter's next poll";
}

// ---- The two halves together (the v2.3.0 order's RW4) -------------------

TEST_F(SchedulerWakeTest, AParkedCoroutineWithOnlyARingWakeIsResumedPromptly) {
    // **The shape the two halves exist for, and the one that hangs if
    // either is wrong.** A coroutine parked on a flag that only a peer's
    // message sets; a reactor that - since RW3 - is allowed to sleep on
    // exactly that; and a block long enough that waiting it out would be
    // unmistakable. Before "parked is not ready" this reactor would have
    // spun instead of sleeping and would have noticed the message by luck;
    // before the wake it would have slept through it.
    //
    // The deadline is the test's own, never CI's: a hang here must fail
    // this test with a message, not time out a suite.
    auto transport = RealRingTransport::Create(2, 8, 64);
    ASSERT_TRUE(transport.ok());
    auto backend = EpollIoBackend::Create();
    ASSERT_TRUE(backend.ok());

    SchedulerConfig config;
    config.max_idle_block_ms = 1000;
    Scheduler scheduler(clock_, backend.value(), config);
    ASSERT_TRUE(scheduler.AttachTransport(&transport.value(), /*core_id=*/1).ok());

    std::atomic<bool> resumed{false};
    bool released = false;
    auto waiter = [&released]() -> Coro {
        co_await WaitFor{&released};
        co_return Status::OK();
    };
    scheduler.Submit(MakeCoroTask(SchedulingGroup::kForeground, waiter(),
                                  [&resumed](const Status&) { resumed.store(true); }));
    ASSERT_TRUE(scheduler
                    .RegisterMessageHandler(RingMessageKind::kStepBatch,
                                            [&released](const MessageHeader&,
                                                        std::span<const std::byte>) {
                                                released = true;
                                            })
                    .ok());

    std::thread reactor([&] { scheduler.Run(); });

    // Wait for the reactor to be asleep **on the park** - which is what
    // `parked_idle_blocks` counts and what nothing before RW3 could have
    // produced. Bounded, so a reactor that never sleeps fails here rather
    // than hanging.
    const auto armed = std::chrono::steady_clock::now();
    while (scheduler.parked_idle_blocks() == 0 &&
           std::chrono::steady_clock::now() - armed < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    ASSERT_GE(scheduler.parked_idle_blocks(), 1u)
        << "the reactor never slept on the parked coroutine; the test proves nothing";

    MessageHeader header{};
    header.src_core = 0;
    header.dst_core = 1;
    header.kind = static_cast<std::uint16_t>(RingMessageKind::kStepBatch);
    header.sched_group = static_cast<std::uint16_t>(SchedulingGroup::kSystem);
    const auto sent = std::chrono::steady_clock::now();
    ASSERT_TRUE(transport.value().TrySend(header, {}).ok());

    while (!resumed.load() &&
           std::chrono::steady_clock::now() - sent < std::chrono::milliseconds(900)) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    const auto latency = std::chrono::steady_clock::now() - sent;

    scheduler.Stop();
    (void)transport.value().TrySend(header, {});
    reactor.join();

    ASSERT_TRUE(resumed.load()) << "the parked coroutine was never resumed";
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(latency).count(), 100)
        << "the park waited out the idle block instead of being woken";
    EXPECT_GE(transport.value().wakes_sent(), 1u)
        << "nothing wrote a wake, so the resume was the block expiring";
}

}  // namespace
}  // namespace kds::sched
