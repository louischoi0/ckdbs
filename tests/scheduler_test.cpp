#include "kds/sched/scheduler.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/ring_transport.hpp"

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
    scheduler.AttachTransport(&transport.value(), /*core_id=*/1);

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
    scheduler.AttachTransport(&transport.value(), 1);

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
    scheduler.AttachTransport(&transport.value(), 1);

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
    scheduler.AttachTransport(&transport.value(), 1);
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
    scheduler.AttachTransport(&transport.value(), 1);

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

}  // namespace
}  // namespace kds::sched
