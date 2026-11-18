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
        SetStrictWaitScopes(false);
        ResetWaitContext();
    }

    const EventTotals& Totals(WaitEvent e) { return WaitTotals().totals[Ix(e)]; }

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
    EXPECT_EQ(WaitTotals().unscoped_parks, 0u);
}

TEST_F(WaitStateTest, OnCpuTimeIsTheSpanInsideAResumingPoll) {
    // A poll's body takes no logical time, so ON_CPU stays 0 until the clock
    // moves inside it; the point here is that parked polls add nothing.
    bool ready = false;
    CoroTask task(SchedulingGroup::kForeground, ScopedWait(&ready));
    EXPECT_EQ(task.Poll(), PollResult::kSuspended);
    const auto after_first = WaitTotals().on_cpu_ns;
    clock_.Advance(1'000);
    EXPECT_EQ(task.Poll(), PollResult::kSuspended);  // asked, still false
    EXPECT_EQ(WaitTotals().on_cpu_ns, after_first);
}

TEST_F(WaitStateTest, ASynchronousScopeIsTheEventsTimeAndNotOnCpuTime) {
    CoroTask task(SchedulingGroup::kForeground, WorkWithASynchronousRead(&clock_));
    EXPECT_EQ(task.Poll(), PollResult::kDone);
    EXPECT_EQ(Totals(WaitEvent::kIoPageRead).total_ns, 50u);
    EXPECT_EQ(WaitTotals().on_cpu_ns, 110u) << "the 160 ns poll less the 50 ns read (I4)";
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
    EXPECT_EQ(WaitTotals().on_cpu_ns, 0u);
}

TEST_F(WaitStateTest, ATaskDroppedWhileParkedInItsScopeBooksNothingAndTakesNoCpuFromTheRunner) {
    bool ready = false;
    auto parked = std::make_unique<CoroTask>(SchedulingGroup::kForeground, ScopedWait(&ready));
    EXPECT_EQ(parked->Poll(), PollResult::kSuspended);
    CoroTask runner(SchedulingGroup::kForeground, DropsAnotherTask(&clock_, &parked));
    EXPECT_EQ(runner.Poll(), PollResult::kDone);
    EXPECT_EQ(parked, nullptr);
    EXPECT_EQ(Totals(WaitEvent::kCommitWalFlush).count, 0u);
    EXPECT_EQ(WaitTotals().on_cpu_ns, 50u) << "the runner's whole 50 ns poll";
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
    EXPECT_EQ(WaitTotals().unscoped_parks, 0u);

    clock_.Advance(2'500);
    ready = true;
    EXPECT_EQ(task.Poll(), PollResult::kDone);
    EXPECT_EQ(Totals(WaitEvent::kCommitWalFlush).count, 1u);
    EXPECT_EQ(Totals(WaitEvent::kCommitWalFlush).total_ns, 2'500u);
    EXPECT_EQ(task.wait_state().state, TaskState::kOnCpu);
    EXPECT_EQ(task.wait_state().event, WaitEvent::kOnCpu);
}

TEST_F(WaitStateTest, AParkOutsideEveryScopeIsCountedAndBookedToNoEvent) {
    bool ready = false;
    CoroTask task(SchedulingGroup::kForeground, UnscopedWait(&ready));
    EXPECT_EQ(task.Poll(), PollResult::kSuspended);
    EXPECT_EQ(WaitTotals().unscoped_parks, 1u);
    EXPECT_EQ(task.wait_state().state, TaskState::kWaiting);

    // Re-asking an unsatisfied wait changes nothing: no second violation.
    clock_.Advance(700);
    EXPECT_EQ(task.Poll(), PollResult::kSuspended);
    EXPECT_EQ(WaitTotals().unscoped_parks, 1u);

    clock_.Advance(300);
    ready = true;
    EXPECT_EQ(task.Poll(), PollResult::kDone);
    for (const EventTotals& t : WaitTotals().totals) {
        EXPECT_EQ(t.total_ns, 0u) << "the unscoped wait's time is booked to no event";
    }
}

TEST_F(WaitStateTest, HistogramBucketKCoversTwoToTheKToTwiceThatInMicroseconds) {
    EXPECT_EQ(WaitBucketOf(0), 0u);
    EXPECT_EQ(WaitBucketOf(1'999), 0u);   // 1 us
    EXPECT_EQ(WaitBucketOf(2'000), 1u);   // 2 us
    EXPECT_EQ(WaitBucketOf(3'999), 1u);
    EXPECT_EQ(WaitBucketOf(4'000), 2u);
    EXPECT_EQ(WaitBucketOf(1'000'000), 9u);  // 1000 us: [512, 1024)
    EXPECT_EQ(WaitBucketOf(~std::uint64_t{0}), kWaitHistogramBuckets - 1);
}

TEST_F(WaitStateTest, ACoreKeepsTheMaxAndTheHistogramOfAnEvent) {
    for (const MonoTimeNs span : {500u, 3'000u, 70'000u, 3'000u}) {
        WaitScope w(WaitEvent::kIoPageRead, 1);
        clock_.Advance(span);
    }
    const EventTotals& t = Totals(WaitEvent::kIoPageRead);
    EXPECT_EQ(t.count, 4u);
    EXPECT_EQ(t.total_ns, 76'500u);
    EXPECT_EQ(t.max_ns, 70'000u);
    EXPECT_EQ(t.histogram[0], 1u);
    EXPECT_EQ(t.histogram[1], 2u);
    EXPECT_EQ(t.histogram[WaitBucketOf(70'000)], 1u);
    std::uint64_t in_buckets = 0;
    for (const RelaxedU64& b : t.histogram) in_buckets += b;
    EXPECT_EQ(in_buckets, t.count) << "every wait lands in exactly one bucket";
}

TEST_F(WaitStateTest, ASessionBooksItsTasksTimeAndItsTimeIsConserved) {
    // I3: over a statement, ON_CPU + the waits equal the elapsed time.
    auto session = MakeSessionWaitStats();
    bool ready = false;
    auto body = [&]() -> Coro {
        clock_.Advance(100);  // CPU
        {
            WaitScope w(WaitEvent::kCommitWalFlush, 9);
            co_await WaitFor{&ready};
        }
        clock_.Advance(40);  // CPU
        co_return Status::OK();
    };
    CoroTask task(SchedulingGroup::kForeground, body());
    task.wait_state_mut().session = session.get();
    clock_.Advance(10);  // run-queue time before the first poll
    EXPECT_EQ(task.Poll(), PollResult::kSuspended);
    EXPECT_EQ(task.wait_state().state, TaskState::kWaiting);
    EXPECT_EQ(session->current_state.get(), static_cast<std::uint64_t>(TaskState::kWaiting));
    EXPECT_EQ(session->current_event.get(), static_cast<std::uint64_t>(WaitEvent::kCommitWalFlush));
    EXPECT_EQ(session->current_p1.get(), 9u);
    clock_.Advance(900);
    ready = true;
    EXPECT_EQ(task.Poll(), PollResult::kDone);
    const auto idx = [](WaitEvent e) { return static_cast<std::size_t>(e); };
    EXPECT_EQ(session->on_cpu_ns.get(), 140u);
    EXPECT_EQ(session->events[idx(WaitEvent::kCommitWalFlush)].total_ns.get(), 900u);
    EXPECT_EQ(session->events[idx(WaitEvent::kSchedRunQueue)].total_ns.get(), 10u);
    EXPECT_EQ(session->on_cpu_ns.get() + 900u + 10u, 1'050u) << "elapsed 1050 ns is all accounted";

    // A session is listed while it lives and gone with its last owner.
    bool listed = false;
    for (const SessionWaitStats& s : SnapshotSessionWaits()) listed |= s.session_id == session->session_id;
    EXPECT_TRUE(listed);
    const std::uint64_t id = session->session_id;
    session.reset();
    for (const SessionWaitStats& s : SnapshotSessionWaits()) EXPECT_NE(s.session_id, id);
}

TEST_F(WaitStateTest, TheDbTimeLineIsTheSessionsDeltaInRegistryOrder) {
    // CJ's WE10: the line's total is the sum of the session's deltas.
    auto session = MakeSessionWaitStats();
    const SessionWaitStats before = *session;
    const auto idx = [](WaitEvent e) { return static_cast<std::size_t>(e); };
    session->on_cpu_ns.add(1'020'000);
    session->events[idx(WaitEvent::kIoPageRead)].total_ns.add(130'000);
    session->events[idx(WaitEvent::kCommitWalFlush)].total_ns.add(2'950'000);
    session->events[idx(WaitEvent::kSchedRunQueue)].total_ns.add(110'000);
    session->events[idx(WaitEvent::kIdleClient)].total_ns.add(9'000'000);  // not DB time
    EXPECT_EQ(FormatDbTimeLine(before, *session),
              "DB time 4.21 ms = CPU 1.02 + commit.wal_flush 2.95 + io.page_read 0.13 + run_queue 0.11")
        << "registry order, idle left out";
    EXPECT_EQ(FormatDbTimeLine(*session, *session), "DB time 0.00 ms = CPU 0.00");
}

TEST_F(WaitStateTest, FlushOnCpuBooksTheOpenSegmentWithoutChangingTheTotal) {
    auto session = MakeSessionWaitStats();
    auto body = [&]() -> Coro {
        clock_.Advance(100);
        FlushOnCpu();
        EXPECT_EQ(session->on_cpu_ns.get(), 100u) << "booked before the statement ends";
        clock_.Advance(50);
        co_return Status::OK();
    };
    CoroTask task(SchedulingGroup::kForeground, body());
    task.wait_state_mut().session = session.get();
    EXPECT_EQ(task.Poll(), PollResult::kDone);
    EXPECT_EQ(session->on_cpu_ns.get(), 150u);
}

// ---- DF-S1: a statement's cost is a session's sample less another -----------

TEST_F(WaitStateTest, ASessionsCostSampleFoldsItsEventsByClassAndCountsItsReads) {
    auto session = MakeSessionWaitStats();
    const auto idx = [](WaitEvent e) { return static_cast<std::size_t>(e); };
    const auto cls = [](stats::WaitClass c) { return static_cast<std::size_t>(c); };
    session->on_cpu_ns.add(700);
    session->events[idx(WaitEvent::kCommitWalFlush)].total_ns.add(100);
    session->events[idx(WaitEvent::kCommitCeilingWait)].total_ns.add(20);
    session->events[idx(WaitEvent::kIoPageRead)].total_ns.add(30);
    session->events[idx(WaitEvent::kIoPageRead)].count.add(2);
    session->events[idx(WaitEvent::kIoVarHeapRead)].count.add(1);
    session->events[idx(WaitEvent::kIoScanRingRead)].count.add(4);
    session->events[idx(WaitEvent::kSchedRunQueue)].total_ns.add(5);
    // A counter event has a count and no time; it is no page read.
    session->events[idx(WaitEvent::kAppLockConflict)].count.add(9);
    session->log_bytes.add(64);
    session->log_records.add(1);

    const SessionCostSample sample = SampleSessionCost(*session);
    EXPECT_EQ(sample.on_cpu_ns, 700u);
    EXPECT_EQ(sample.class_ns[cls(stats::WaitClass::kCommit)], 120u);
    EXPECT_EQ(sample.class_ns[cls(stats::WaitClass::kUserIo)], 30u);
    EXPECT_EQ(sample.class_ns[cls(stats::WaitClass::kScheduler)], 5u);
    EXPECT_EQ(sample.class_ns[cls(stats::WaitClass::kApplication)], 0u);
    EXPECT_EQ(sample.class_ns[cls(stats::WaitClass::kCpu)], 0u) << "ON_CPU is beside the table";
    EXPECT_EQ(sample.page_reads, 7u);
    EXPECT_EQ(sample.log_bytes, 64u);
    EXPECT_EQ(sample.log_records, 1u);
}

#ifndef KDS_WAIT_INSTRUMENTATION_OFF
TEST_F(WaitStateTest, ALogAppendIsBookedToThePolledTasksSessionAndToNobodyElse) {
    // DF-Q12 (a): booked as a wait is, to the session of the task being polled.
    auto session = MakeSessionWaitStats();
    BookLogAppend(100);  // no task is being polled: nobody's
    EXPECT_EQ(session->log_bytes.get(), 0u);

    auto appends = [&]() -> Coro {
        BookLogAppend(40);
        BookLogAppend(24);
        co_return Status::OK();
    };
    CoroTask own(SchedulingGroup::kForeground, appends());
    own.wait_state_mut().session = session.get();
    EXPECT_EQ(own.Poll(), PollResult::kDone);
    EXPECT_EQ(session->log_bytes.get(), 64u);
    EXPECT_EQ(session->log_records.get(), 2u);

    // A task with no session - a background task's append - is no session's.
    CoroTask background(SchedulingGroup::kForeground, appends());
    EXPECT_EQ(background.Poll(), PollResult::kDone);
    EXPECT_EQ(session->log_bytes.get(), 64u);
}
#endif

#ifndef NDEBUG
TEST_F(WaitStateTest, AStrictCoreRefusesAParkOutsideEveryScope) {
    SetStrictWaitScopes(true);
    bool ready = false;
    CoroTask task(SchedulingGroup::kForeground, UnscopedWait(&ready));
    EXPECT_DEATH(task.Poll(), "WaitScope");
    SetStrictWaitScopes(false);
}
#endif

TEST_F(WaitStateTest, AnIdleScopeIsIdleStateAndAnExpectedParkNotAViolation) {
    bool ready = false;
    CoroTask task(SchedulingGroup::kForeground, IdleWait(&ready));
    EXPECT_EQ(task.Poll(), PollResult::kSuspended);
    EXPECT_EQ(task.wait_state().state, TaskState::kIdle);
    EXPECT_EQ(WaitTotals().unscoped_parks, 0u);
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
