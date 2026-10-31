#include <gtest/gtest.h>

#include <memory>

#include "kds/sched/clock.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/wait_state.hpp"

// CJ's WE02 (`instructions/v3.2.0/workorder-cj-wait-events-ash.md` §2.1-§2.5):
// a task's state transitions and the elapsed times they book, on a logical
// clock. `CoroTask::Poll` is driven by hand, as `coro_test.cpp` does.

#ifndef KDS_WAIT_INSTRUMENTATION_OFF

namespace kds::sched {
namespace {

using stats::WaitEvent;

constexpr std::size_t Ix(WaitEvent e) { return static_cast<std::size_t>(e); }

class WaitStateTest : public ::testing::Test {
protected:
    void SetUp() override {
        ResetWaitContext();
        SetWaitClock(&clock_);
    }
    void TearDown() override {
        SetWaitClock(nullptr);
        ResetWaitContext();
    }

    const EventTotals& Totals(WaitEvent e) { return WaitContext().totals[Ix(e)]; }

    ManualClock clock_{1'000};
};

Coro YieldOnce() {
    co_await Yield{};
    co_return Status::OK();
}

Coro ScopedWait(bool* ready) {
    {
        WaitScope w(WaitEvent::kCommitWalFlush, /*p1=*/77, /*p2=*/2, /*p3=*/5);
        co_await WaitFor{ready};
    }
    co_return Status::OK();
}

Coro WorkWithASynchronousRead(ManualClock* clock) {
    clock->Advance(100);
    {
        // A blocking `pread` on the reactor: a scope that never suspends.
        WaitScope w(WaitEvent::kIoPageRead, /*p1=*/12);
        clock->Advance(50);
    }
    clock->Advance(10);
    co_return Status::OK();
}

Coro YieldInsideAScope(ManualClock* clock) {
    WaitScope w(WaitEvent::kIoVarHeapRead);
    co_await Yield{};
    clock->Advance(5);
    co_return Status::OK();
}

Coro DropsAnotherTask(ManualClock* clock, std::unique_ptr<CoroTask>* other) {
    clock->Advance(20);
    other->reset();  // destroys a task parked inside its scope
    clock->Advance(30);
    co_return Status::OK();
}

Coro UnscopedWait(bool* ready) {
    co_await WaitFor{ready};
    co_return Status::OK();
}

Coro IdleWait(bool* ready) {
    WaitScope w(WaitEvent::kIdleClient);
    co_await WaitFor{ready};
    co_return Status::OK();
}

TEST_F(WaitStateTest, AnUnpolledTaskIsRunnableAndItsQueueTimeIsBookedAtTheFirstPoll) {
    CoroTask task(SchedulingGroup::kForeground, YieldOnce());
    EXPECT_EQ(task.wait_state().state, TaskState::kRunnable);
    clock_.Advance(300);
    EXPECT_EQ(task.Poll(), PollResult::kSuspended);  // runs to its Yield
    EXPECT_EQ(Totals(WaitEvent::kSchedRunQueue).count, 1u);
    EXPECT_EQ(Totals(WaitEvent::kSchedRunQueue).total_ns, 300u);
    // A yield re-queues the task: RUNNABLE again, and the second poll's queue
    // time is the span since.
    EXPECT_EQ(task.wait_state().state, TaskState::kRunnable);
    clock_.Advance(40);
    EXPECT_EQ(task.Poll(), PollResult::kDone);
    EXPECT_EQ(Totals(WaitEvent::kSchedRunQueue).count, 2u);
    EXPECT_EQ(Totals(WaitEvent::kSchedRunQueue).total_ns, 340u);
    EXPECT_EQ(WaitContext().unscoped_parks, 0u);
}

TEST_F(WaitStateTest, OnCpuTimeIsTheSpanInsideAResumingPoll) {
    // A poll's body takes no logical time, so ON_CPU stays 0 until the clock
    // moves inside it; the point here is that parked polls add nothing.
    bool ready = false;
    CoroTask task(SchedulingGroup::kForeground, ScopedWait(&ready));
    EXPECT_EQ(task.Poll(), PollResult::kSuspended);
    const auto after_first = WaitContext().on_cpu_ns;
    clock_.Advance(1'000);
    EXPECT_EQ(task.Poll(), PollResult::kSuspended);  // asked, still false
    EXPECT_EQ(WaitContext().on_cpu_ns, after_first);
}

TEST_F(WaitStateTest, ASynchronousScopeIsTheEventsTimeAndNotOnCpuTime) {
    CoroTask task(SchedulingGroup::kForeground, WorkWithASynchronousRead(&clock_));
    EXPECT_EQ(task.Poll(), PollResult::kDone);
    EXPECT_EQ(Totals(WaitEvent::kIoPageRead).total_ns, 50u);
    EXPECT_EQ(WaitContext().on_cpu_ns, 110u) << "the 160 ns poll less the 50 ns read (I4)";
    EXPECT_EQ(Totals(WaitEvent::kSchedRunQueue).total_ns, 0u);
}

TEST_F(WaitStateTest, AYieldInsideAScopeIsStillTheScopesTimeAndNotRunQueueTime) {
    CoroTask task(SchedulingGroup::kForeground, YieldInsideAScope(&clock_));
    EXPECT_EQ(task.Poll(), PollResult::kSuspended);
    EXPECT_EQ(task.wait_state().state, TaskState::kWaiting);
    clock_.Advance(70);
    EXPECT_EQ(task.Poll(), PollResult::kDone);
    EXPECT_EQ(Totals(WaitEvent::kIoVarHeapRead).total_ns, 75u);
    EXPECT_EQ(Totals(WaitEvent::kSchedRunQueue).total_ns, 0u) << "booked twice (I4)";
    EXPECT_EQ(WaitContext().on_cpu_ns, 0u);
}

TEST_F(WaitStateTest, ATaskDroppedWhileParkedInItsScopeBooksNothingAndTakesNoCpuFromTheRunner) {
    bool ready = false;
    auto parked = std::make_unique<CoroTask>(SchedulingGroup::kForeground, ScopedWait(&ready));
    EXPECT_EQ(parked->Poll(), PollResult::kSuspended);
    CoroTask runner(SchedulingGroup::kForeground, DropsAnotherTask(&clock_, &parked));
    EXPECT_EQ(runner.Poll(), PollResult::kDone);
    EXPECT_EQ(parked, nullptr);
    EXPECT_EQ(Totals(WaitEvent::kCommitWalFlush).count, 0u);
    EXPECT_EQ(WaitContext().on_cpu_ns, 50u) << "the runner's whole 50 ns poll";
}

TEST_F(WaitStateTest, AWaitScopeBooksItsEventAndShowsParametersWhileOpen) {
    bool ready = false;
    CoroTask task(SchedulingGroup::kForeground, ScopedWait(&ready));
    EXPECT_EQ(task.Poll(), PollResult::kSuspended);
    EXPECT_EQ(task.wait_state().state, TaskState::kWaiting);
    EXPECT_EQ(task.wait_state().event, WaitEvent::kCommitWalFlush);
    EXPECT_EQ(task.wait_state().p1, 77u);
    EXPECT_EQ(task.wait_state().p2, 2u);
    EXPECT_EQ(task.wait_state().p3, 5u);
    EXPECT_EQ(WaitContext().unscoped_parks, 0u);

    clock_.Advance(2'500);
    ready = true;
    EXPECT_EQ(task.Poll(), PollResult::kDone);
    EXPECT_EQ(Totals(WaitEvent::kCommitWalFlush).count, 1u);
    EXPECT_EQ(Totals(WaitEvent::kCommitWalFlush).total_ns, 2'500u);
    EXPECT_EQ(task.wait_state().state, TaskState::kOnCpu);
    EXPECT_EQ(task.wait_state().event, WaitEvent::kOnCpu);
}

TEST_F(WaitStateTest, AParkOutsideEveryScopeIsCountedAndBookedAsUnclassified) {
    bool ready = false;
    CoroTask task(SchedulingGroup::kForeground, UnscopedWait(&ready));
    EXPECT_EQ(task.Poll(), PollResult::kSuspended);
    EXPECT_EQ(WaitContext().unscoped_parks, 1u);
    EXPECT_EQ(task.wait_state().state, TaskState::kWaiting);
    EXPECT_EQ(task.wait_state().event, WaitEvent::kSchedUnclassified);

    // Re-asking an unsatisfied wait changes nothing: no second violation.
    clock_.Advance(700);
    EXPECT_EQ(task.Poll(), PollResult::kSuspended);
    EXPECT_EQ(WaitContext().unscoped_parks, 1u);

    clock_.Advance(300);
    ready = true;
    EXPECT_EQ(task.Poll(), PollResult::kDone);
    EXPECT_EQ(Totals(WaitEvent::kSchedUnclassified).count, 1u);
    EXPECT_EQ(Totals(WaitEvent::kSchedUnclassified).total_ns, 1'000u);
}

TEST_F(WaitStateTest, AnIdleScopeIsIdleStateAndAnExpectedParkNotAViolation) {
    bool ready = false;
    CoroTask task(SchedulingGroup::kForeground, IdleWait(&ready));
    EXPECT_EQ(task.Poll(), PollResult::kSuspended);
    EXPECT_EQ(task.wait_state().state, TaskState::kIdle);
    EXPECT_EQ(WaitContext().unscoped_parks, 0u);
    clock_.Advance(9'000);
    ready = true;
    EXPECT_EQ(task.Poll(), PollResult::kDone);
    EXPECT_EQ(Totals(WaitEvent::kIdleClient).total_ns, 9'000u);
}

TEST_F(WaitStateTest, ACounterEventRaisesItsCountAndNoTime) {
    RecordCounterEvent(WaitEvent::kAppLockConflict);
    RecordCounterEvent(WaitEvent::kAppLockConflict);
    EXPECT_EQ(Totals(WaitEvent::kAppLockConflict).count, 2u);
    EXPECT_EQ(Totals(WaitEvent::kAppLockConflict).total_ns, 0u);
}

TEST_F(WaitStateTest, AScopeWithNoTaskBeingPolledStillBooksToTheCore) {
    {
        WaitScope w(WaitEvent::kIoPageRead, 9);
        clock_.Advance(120);
    }
    EXPECT_EQ(Totals(WaitEvent::kIoPageRead).count, 1u);
    EXPECT_EQ(Totals(WaitEvent::kIoPageRead).total_ns, 120u);
}

}  // namespace
}  // namespace kds::sched

#endif
