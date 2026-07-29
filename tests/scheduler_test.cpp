#include "kds/sched/scheduler.hpp"

#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"

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

}  // namespace
}  // namespace kds::sched
