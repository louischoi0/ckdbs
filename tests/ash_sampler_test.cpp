#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/sched/wait_state.hpp"

// CJ's WE07 (`instructions/v3.2.0/workorder-cj-wait-events-ash.md` §2.7): the
// sampler on a logical clock, driven by the real scheduler's turn.

#ifndef KDS_WAIT_INSTRUMENTATION_OFF

namespace kds::sched {
namespace {

using stats::WaitEvent;

constexpr MonoTimeNs kInterval = 1'000'000;

Coro ParkedIn(WaitEvent event, bool* never) {
    WaitScope w(event, /*p1=*/42, /*p2=*/7, /*p3=*/3);
    co_await WaitFor{never};
    co_return Status::OK();
}

class AshSamplerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ResetWaitContext();
        SetWaitClock(&clock_);
        scheduler_.ash().set_interval_ns(kInterval);
    }
    void TearDown() override {
        SetWaitClock(nullptr);
        ResetWaitContext();
    }

    // A parked task, optionally told which session it runs for.
    void Park(WaitEvent event, SessionWaitStats* session = nullptr) {
        auto task = std::make_unique<CoroTask>(SchedulingGroup::kForeground, ParkedIn(event, &never_));
        task->wait_state_mut().session = session;
        scheduler_.Submit(std::move(task));
    }

    std::vector<stats::AshRecord> Records() { return scheduler_.ash().ring()->Snapshot(); }

    ManualClock clock_{0};
    NullIoBackend io_;
    Scheduler scheduler_{clock_, io_};
    bool never_ = false;
};

TEST_F(AshSamplerTest, TheSampleCountIsElapsedTimeOverTheIntervalForEveryActiveTask) {
    Park(WaitEvent::kCommitWalFlush);
    scheduler_.RunOnce();  // arms the grid at t = 0 and parks the task
    EXPECT_TRUE(Records().empty());
    for (int i = 1; i <= 10; ++i) {
        clock_.Advance(kInterval);
        scheduler_.RunOnce();
    }
    const auto got = Records();
    ASSERT_EQ(got.size(), 10u) << "10 intervals elapsed, one active task";
    for (std::size_t i = 0; i < got.size(); ++i) {
        EXPECT_EQ(got[i].sample_ts_us, (i + 1) * kInterval / 1000);
        EXPECT_EQ(got[i].flags & stats::kAshLate, 0) << "an on-time turn is not late";
        EXPECT_EQ(got[i].event_id, static_cast<std::uint16_t>(WaitEvent::kCommitWalFlush));
        EXPECT_EQ(got[i].p1, 42u);
        EXPECT_EQ(got[i].p2, 7u);
        EXPECT_EQ(got[i].state & stats::kAshStateMask, static_cast<int>(TaskState::kWaiting));
        EXPECT_NE(got[i].state & stats::kAshBackground, 0) << "no session: a background task";
    }
    EXPECT_EQ(got[9].wait_elapsed_us, 10'000u);
}

TEST_F(AshSamplerTest, ALongStepYieldsOneLateRecordAndTheGridDoesNotDrift) {
    Park(WaitEvent::kIoPageRead);
    scheduler_.RunOnce();
    clock_.Advance(5 * kInterval + kInterval / 2);  // a step that swallowed five samples
    scheduler_.RunOnce();
    auto got = Records();
    ASSERT_EQ(got.size(), 1u) << "one record at the real time, nothing interpolated";
    EXPECT_EQ(got[0].sample_ts_us, (5 * kInterval + kInterval / 2) / 1000);
    EXPECT_NE(got[0].flags & stats::kAshLate, 0);

    // The next due time is the grid point after the long turn (6 intervals),
    // not "late + interval".
    clock_.SetNow(6 * kInterval - 1);
    scheduler_.RunOnce();
    EXPECT_EQ(Records().size(), 1u);
    clock_.SetNow(6 * kInterval);
    scheduler_.RunOnce();
    got = Records();
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[1].flags & stats::kAshLate, 0);
}

TEST_F(AshSamplerTest, IdleTasksAreNotSampledAndASessionTaskIsToldByItsSession) {
    auto session = MakeSessionWaitStats();
    session->fingerprint.set(0xABCD);
    Park(WaitEvent::kIdleClient, session.get());
    Park(WaitEvent::kCommitWalFlush, session.get());
    scheduler_.RunOnce();
    clock_.Advance(kInterval);
    scheduler_.RunOnce();
    const auto got = Records();
    ASSERT_EQ(got.size(), 1u) << "the IDLE task is out (WD6)";
    EXPECT_EQ(got[0].session_id, session->session_id);
    EXPECT_EQ(got[0].fingerprint, 0xABCDu);
    EXPECT_EQ(got[0].state & stats::kAshBackground, 0);
}

TEST_F(AshSamplerTest, TheArgumentHashRidesInP1OnlyWhenOptedInAndNotOnAWaitingSample) {
    auto session = MakeSessionWaitStats();
    session->arg_hash.set(0x1234);
    // A RUNNABLE statement task: submitted, never polled before the sample.
    auto runnable = std::make_unique<CoroTask>(SchedulingGroup::kForeground, ParkedIn(WaitEvent::kIoPageRead, &never_));
    runnable->wait_state_mut().session = session.get();
    scheduler_.ash().Begin(0);  // a batch by hand: the state is the point, not the scheduler
    scheduler_.ash().Add(&runnable->wait_state_mut(), 0);
    EXPECT_EQ(Records().back().p1, 0u) << "off by default (WD11)";

    scheduler_.ash().set_capture_arg_hash(true);
    scheduler_.ash().Add(&runnable->wait_state_mut(), 0);
    const stats::AshRecord on = Records().back();
    EXPECT_EQ(on.p1, 0x1234u);
    EXPECT_NE(on.flags & stats::kAshArgHashValid, 0);

    runnable->wait_state_mut().state = TaskState::kWaiting;
    runnable->wait_state_mut().p1 = 99;
    scheduler_.ash().Add(&runnable->wait_state_mut(), 0);
    const stats::AshRecord waiting = Records().back();
    EXPECT_EQ(waiting.p1, 99u) << "a WAITING sample keeps the event's parameters";
    EXPECT_EQ(waiting.flags & stats::kAshArgHashValid, 0);
}

TEST_F(AshSamplerTest, TurningSamplingOffKeepsTheRingAndOnContinuesIt) {
    Park(WaitEvent::kCommitWalFlush);
    scheduler_.RunOnce();
    clock_.Advance(kInterval);
    scheduler_.RunOnce();
    ASSERT_EQ(Records().size(), 1u);
    scheduler_.ash().set_enabled(false);
    clock_.Advance(3 * kInterval);
    scheduler_.RunOnce();
    EXPECT_EQ(Records().size(), 1u) << "off: no sample, the ring kept";
    scheduler_.ash().set_enabled(true);
    scheduler_.RunOnce();  // re-arms
    clock_.Advance(kInterval);
    scheduler_.RunOnce();
    EXPECT_EQ(Records().size(), 2u);
}

TEST_F(AshSamplerTest, AShorterIntervalTakesEffectAtTheNextTurn) {
    scheduler_.ash().set_interval_ns(10 * kInterval);
    Park(WaitEvent::kCommitWalFlush);
    scheduler_.RunOnce();  // armed: next due at 10 intervals
    scheduler_.ash().set_interval_ns(kInterval);
    clock_.Advance(kInterval);
    scheduler_.RunOnce();  // the grid is pulled in to one new interval from here
    EXPECT_TRUE(Records().empty());
    clock_.Advance(kInterval);
    scheduler_.RunOnce();
    EXPECT_EQ(Records().size(), 1u) << "no wait for the old, longer grid point";
}

}  // namespace
}  // namespace kds::sched

#endif
