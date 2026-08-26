#include "kds/sched/coro.hpp"

#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/sched/send_retry.hpp"

// C++20 stackless coroutines as the task representation (docs/spec/sched.md §3,
// settled 2026-08-05).
//
// The last test is the one that matters: a **cross-core request/response**,
// which is the thing that could not be written at all before this and which
// blocks the whole of workplan P4. Everything above it is the machinery that
// has to be right for that one to mean anything.

namespace kds::sched {
namespace {

TEST(CoroTest, ACoroutineDoesNotRunUntilItIsPolled) {
    // Lazy start: creating the frame and running the body are two acts, as
    // they are for every other task.
    int ran = 0;
    auto body = [&ran]() -> Coro {
        ++ran;
        co_return Status::OK();
    };

    auto task = MakeCoroTask(SchedulingGroup::kForeground, body());
    EXPECT_EQ(ran, 0) << "the body ran at construction";

    EXPECT_EQ(task->Poll(), PollResult::kDone);
    EXPECT_EQ(ran, 1);
}

TEST(CoroTest, AYieldSuspendsAndResumesWhereItLeftOff) {
    std::vector<int> marks;
    auto body = [&marks]() -> Coro {
        marks.push_back(1);
        co_await Yield{};
        marks.push_back(2);
        co_await Yield{};
        marks.push_back(3);
        co_return Status::OK();
    };

    auto task = MakeCoroTask(SchedulingGroup::kForeground, body());
    EXPECT_EQ(task->Poll(), PollResult::kSuspended);
    EXPECT_EQ(marks, (std::vector<int>{1}));
    EXPECT_EQ(task->Poll(), PollResult::kSuspended);
    EXPECT_EQ(marks, (std::vector<int>{1, 2}));
    EXPECT_EQ(task->Poll(), PollResult::kDone);
    EXPECT_EQ(marks, (std::vector<int>{1, 2, 3}));
}

TEST(CoroTest, TheReturnedStatusReachesTheCompletionCallback) {
    Status reported = Status::OK();
    auto body = []() -> Coro { co_return Status::Unsupported("nope"); };

    auto task = MakeCoroTask(SchedulingGroup::kForeground, body(),
                             [&reported](const Status& s) { reported = s; });
    EXPECT_EQ(task->Poll(), PollResult::kDone);
    EXPECT_EQ(reported.code(), StatusCode::kUnsupported);
    EXPECT_EQ(reported.message(), "nope");
}

TEST(CoroTest, AWaitDoesNotResumeUntilItsFlagIsSet) {
    // The bug this exists to prevent: `await_ready` runs once, at the
    // co_await, so a naive awaitable suspends once and then resumes on the
    // next poll whatever the condition says. The re-test lives in Poll().
    bool ready = false;
    bool past_the_wait = false;
    auto body = [&]() -> Coro {
        co_await WaitFor{&ready};
        past_the_wait = true;
        co_return Status::OK();
    };

    auto task = MakeCoroTask(SchedulingGroup::kForeground, body());
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(task->Poll(), PollResult::kSuspended);
        EXPECT_FALSE(past_the_wait) << "resumed on poll " << i << " with the flag still false";
    }

    ready = true;
    EXPECT_EQ(task->Poll(), PollResult::kDone);
    EXPECT_TRUE(past_the_wait);
}

TEST(CoroTest, AWaitOnAnAlreadySetFlagNeverSuspends) {
    // A reply that arrived before the wait began must cost nothing.
    bool ready = true;
    auto body = [&ready]() -> Coro {
        co_await WaitFor{&ready};
        co_return Status::OK();
    };

    auto task = MakeCoroTask(SchedulingGroup::kForeground, body());
    EXPECT_EQ(task->Poll(), PollResult::kDone) << "an already-satisfied wait suspended";
    EXPECT_EQ(task->resumes(), 1u);
}

TEST(CoroTest, WaitingCostsNoResumes) {
    // The claim the header makes: a long wait is a predicate call per
    // reactor turn, not a resumed frame.
    bool ready = false;
    auto body = [&ready]() -> Coro {
        co_await WaitFor{&ready};
        co_return Status::OK();
    };

    auto task = MakeCoroTask(SchedulingGroup::kForeground, body());
    task->Poll();  // runs to the co_await and parks
    const std::uint64_t after_park = task->resumes();
    for (int i = 0; i < 20; ++i) task->Poll();
    EXPECT_EQ(task->resumes(), after_park) << "a parked coroutine was resumed while waiting";
}

TEST(CoroTest, DroppingASuspendedTaskDestroysItsFrame) {
    // What makes a cancelled or abandoned statement leak nothing. The
    // destructor of a local inside the coroutine body runs when the frame
    // is destroyed, which is what this observes.
    struct Tracker {
        bool* destroyed;
        ~Tracker() { *destroyed = true; }
    };

    bool destroyed = false;
    auto body = [&destroyed]() -> Coro {
        Tracker t{&destroyed};
        co_await Yield{};
        co_return Status::OK();
    };

    {
        auto task = MakeCoroTask(SchedulingGroup::kForeground, body());
        EXPECT_EQ(task->Poll(), PollResult::kSuspended);
        EXPECT_FALSE(destroyed);
    }
    EXPECT_TRUE(destroyed) << "a suspended coroutine's frame leaked";
}

TEST(CoroTest, ACoroutineNeverPolledIsStillCleanedUp) {
    bool destroyed = false;
    struct Tracker {
        bool* destroyed;
        ~Tracker() { *destroyed = true; }
    };
    auto body = [&destroyed]() -> Coro {
        Tracker t{&destroyed};
        co_await Yield{};
        co_return Status::OK();
    };

    { auto task = MakeCoroTask(SchedulingGroup::kForeground, body()); }
    // Never started, so the Tracker was never constructed - what matters is
    // that destroying the frame does not crash or assert.
    EXPECT_FALSE(destroyed);
}

// ---- On a real reactor -------------------------------------------------

TEST(CoroTest, TheSchedulerRunsACoroutineToCompletionAcrossIterations) {
    // No change to Scheduler was needed for any of this: kSuspended/kDone
    // was already a coroutine's resume protocol.
    ManualClock clock;
    NullIoBackend io;
    Scheduler scheduler(clock, io);

    int steps = 0;
    bool finished = false;
    auto body = [&steps]() -> Coro {
        for (int i = 0; i < 3; ++i) {
            ++steps;
            co_await Yield{};
        }
        co_return Status::OK();
    };

    scheduler.Submit(MakeCoroTask(SchedulingGroup::kForeground, body(),
                                  [&finished](const Status&) { finished = true; }));
    for (int i = 0; i < 10 && !finished; ++i) scheduler.RunOnce();

    EXPECT_TRUE(finished);
    EXPECT_EQ(steps, 3);
}

TEST(CoroTest, AWaitingCoroutineDoesNotBlockTheCore) {
    // sched.md §3's whole point: waiting is suspension, not occupation.
    ManualClock clock;
    NullIoBackend io;
    Scheduler scheduler(clock, io);

    bool ready = false;
    bool finished = false;
    auto waiter = [&ready]() -> Coro {
        co_await WaitFor{&ready};
        co_return Status::OK();
    };
    scheduler.Submit(MakeCoroTask(SchedulingGroup::kForeground, waiter(),
                                  [&finished](const Status&) { finished = true; }));

    int other_work = 0;
    for (int i = 0; i < 5; ++i) {
        scheduler.Submit(std::make_unique<FunctionTask>(SchedulingGroup::kForeground, [&] {
            ++other_work;
            return PollResult::kDone;
        }));
    }

    for (int i = 0; i < 10; ++i) scheduler.RunOnce();
    EXPECT_FALSE(finished);
    EXPECT_EQ(other_work, 5) << "a waiting coroutine held the core";

    ready = true;
    for (int i = 0; i < 5 && !finished; ++i) scheduler.RunOnce();
    EXPECT_TRUE(finished);
}

// ---- The thing that was blocked ----------------------------------------

TEST(CoroTest, ACoroutineDoesACrossCoreRequestAndResponse) {
    // **This is what the decision was for.** Send a message to a peer, wait
    // for its reply, and continue - in straight-line code, on a reactor that
    // never blocks. Before coroutines this could not be written at all:
    // `Dispatch()` returns synchronously and `ChainRunner` has no suspension
    // point, so the only options were blocking the core or hand-rolling the
    // whole executor into a state machine.
    //
    // This is the exact shape workplan P4's step pipeline needs - send
    // STEP_OPEN, await batches - and the exact shape P5's lease services
    // need.
    ManualClock clock;
    NullIoBackend io_a;
    NullIoBackend io_b;
    Scheduler core0(clock, io_a);
    Scheduler core1(clock, io_b);

    auto transport = RealRingTransport::Create(/*core_count=*/2, 16, 64);
    ASSERT_TRUE(transport.ok());
    ASSERT_TRUE(core0.AttachTransport(&transport.value(), 0).ok());
    ASSERT_TRUE(core1.AttachTransport(&transport.value(), 1).ok());

    // Per-request state the reply is routed to. It outlives the wait, which
    // is WaitFor's one requirement.
    struct Request {
        bool replied = false;
        std::uint64_t answer = 0;
    };
    Request request;

    // Core 1: answers a lease request by sending one back.
    ASSERT_TRUE(core1
                    .RegisterMessageHandler(
                        RingMessageKind::kExtentLease,
                        [&transport](const MessageHeader& h, std::span<const std::byte>) {
                            MessageHeader reply{};
                            reply.src_core = 1;
                            reply.dst_core = h.src_core;
                            reply.request_id = h.request_id;
                            reply.kind = static_cast<std::uint16_t>(RingMessageKind::kExtentLease);
                            const std::uint64_t granted = 4096;
                            std::byte bytes[sizeof(granted)];
                            std::memcpy(bytes, &granted, sizeof(granted));
                            (void)transport.value().TrySend(
                                reply, std::span<const std::byte>(bytes, sizeof(bytes)));
                        })
                    .ok());

    // Core 0: routes the reply into the waiting request's state.
    ASSERT_TRUE(core0
                    .RegisterMessageHandler(
                        RingMessageKind::kExtentLease,
                        [&request](const MessageHeader&, std::span<const std::byte> payload) {
                            std::memcpy(&request.answer, payload.data(), sizeof(request.answer));
                            request.replied = true;
                        })
                    .ok());

    // The statement, as straight-line code.
    bool finished = false;
    std::uint64_t got = 0;
    auto ask = [&]() -> Coro {
        MessageHeader header{};
        header.src_core = 0;
        header.dst_core = 1;
        header.request_id = 1;
        header.kind = static_cast<std::uint16_t>(RingMessageKind::kExtentLease);
        if (Status s = transport.value().TrySend(header, {}); !s.ok()) co_return s;

        co_await WaitFor{&request.replied};

        got = request.answer;
        co_return Status::OK();
    };

    core0.Submit(MakeCoroTask(SchedulingGroup::kForeground, ask(),
                              [&finished](const Status& s) { finished = s.ok(); }));

    // Both reactors stepped round-robin on this thread - sched.md §8's
    // simulation shape, which is what makes this deterministic.
    for (int i = 0; i < 20 && !finished; ++i) {
        core0.RunOnce();
        core1.RunOnce();
    }

    EXPECT_TRUE(finished) << "the request/response never completed";
    EXPECT_EQ(got, 4096u);
}

// ---- Suspension safety -------------------------------------------------
//
// The rule the executor's coming suspension points have to obey, made
// mechanical *before* any of them exists. R1 already forbids a page fetch
// under a live span; suspending under one is worse, because the span stays
// live across every other statement that runs on this core in between.

TEST(CoroSuspendAuditTest, NoAuditInstalledMeansNothingIsReported) {
    ResetSuspendAudit();
    SetSuspendAudit(nullptr);

    auto body = []() -> Coro {
        co_await Yield{};
        co_return Status::OK();
    };
    auto task = MakeCoroTask(SchedulingGroup::kForeground, body());
    task->Poll();
    EXPECT_FALSE(SuspendAuditTripped());
}

TEST(CoroSuspendAuditTest, ASuspensionUnderAnUnsafeConditionIsReported) {
    ResetSuspendAudit();
    static bool unsafe = false;
    SetSuspendAudit([]() -> std::string_view {
        return unsafe ? std::string_view("holding something it must not hold")
                      : std::string_view();
    });

    auto body = []() -> Coro {
        co_await Yield{};
        co_return Status::OK();
    };

    // Safe: suspending reports nothing.
    auto ok_task = MakeCoroTask(SchedulingGroup::kForeground, body());
    ok_task->Poll();
    EXPECT_FALSE(SuspendAuditTripped());

    // Unsafe: the same suspension is caught.
    unsafe = true;
    auto bad_task = MakeCoroTask(SchedulingGroup::kForeground, body());
    bad_task->Poll();
#ifndef NDEBUG
    EXPECT_TRUE(SuspendAuditTripped()) << "a suspension under an unsafe condition went unnoticed";
    EXPECT_EQ(SuspendAuditReason(), "holding something it must not hold");
#endif

    unsafe = false;
    SetSuspendAudit(nullptr);
    ResetSuspendAudit();
}

TEST(CoroSuspendAuditTest, ACompletedCoroutineIsNeverAudited) {
    // The audit is about being *parked*, not about running. A coroutine
    // that finishes in one poll held nothing across anything.
    ResetSuspendAudit();
    SetSuspendAudit([]() -> std::string_view { return "should not be consulted"; });

    auto body = []() -> Coro { co_return Status::OK(); };
    auto task = MakeCoroTask(SchedulingGroup::kForeground, body());
    EXPECT_EQ(task->Poll(), PollResult::kDone);
    EXPECT_FALSE(SuspendAuditTripped());

    SetSuspendAudit(nullptr);
    ResetSuspendAudit();
}


// ---- Nested awaits (P4d-1): the executor conversion's primitive ----------

TEST(CoroNestedTest, AThreeDeepChainCompletesAndStatusesPropagate) {
    // The executor's spine is mutually recursive, so the chain shape is the
    // contract: parent awaits child awaits grandchild, each contributing to
    // one observable order, and the grandchild's error surfacing at the top
    // exactly as a return value would have.
    std::vector<int> order;

    auto grandchild = [&]() -> Coro {
        order.push_back(3);
        co_return Status::NotFound("bottom says no");
    };
    auto child = [&]() -> Coro {
        order.push_back(2);
        Status inner = co_await grandchild();
        EXPECT_EQ(inner.code(), StatusCode::kNotFound);
        co_return inner;  // propagated, as the executor's Status returns will be
    };
    auto parent = [&]() -> Coro {
        order.push_back(1);
        co_return co_await child();
    };

    Status final_status = Status::OK();
    auto task = MakeCoroTask(SchedulingGroup::kForeground, parent(),
                             [&](const Status& s) { final_status = s; });
    while (task->Poll() != PollResult::kDone) {
    }
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(final_status.code(), StatusCode::kNotFound);
    EXPECT_NE(final_status.message().find("bottom says no"), std::string::npos);
}

TEST(CoroNestedTest, AChildParkedOnAFlagParksTheWholeChain) {
    // The pipeline's shape: a step three frames down awaits a remote batch.
    // Until the flag flips, polls cost predicate checks and no resumes; when
    // it flips, the chain finishes without re-entering finished frames.
    bool batch_ready = false;
    std::vector<int> order;

    auto leaf = [&]() -> Coro {
        order.push_back(1);
        co_await WaitFor{&batch_ready};
        order.push_back(3);
        co_return Status::OK();
    };
    auto spine = [&]() -> Coro { co_return co_await leaf(); };

    auto task = MakeCoroTask(SchedulingGroup::kForeground, spine());
    EXPECT_EQ(task->Poll(), PollResult::kSuspended);  // ran to the wait
    EXPECT_EQ(task->Poll(), PollResult::kSuspended);  // parked: no resume
    EXPECT_EQ(task->Poll(), PollResult::kSuspended);
    order.push_back(2);
    batch_ready = true;
    EXPECT_EQ(task->Poll(), PollResult::kDone);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(CoroNestedTest, DroppingATaskMidAwaitDestroysTheWholeChain) {
    // CoroTask's owning destructor, extended transitively: the awaiter in
    // each parent frame owns its child frame, so destroying the top frame
    // unwinds every level - a cancelled statement leaks no step.
    bool never_set = false;
    bool leaf_destroyed = false;
    struct MarkOnDestroy {
        bool* flag;
        ~MarkOnDestroy() { *flag = true; }
    };

    auto leaf = [&]() -> Coro {
        MarkOnDestroy mark{&leaf_destroyed};
        co_await WaitFor{&never_set};
        co_return Status::OK();
    };
    auto spine = [&]() -> Coro { co_return co_await leaf(); };

    {
        auto task = MakeCoroTask(SchedulingGroup::kForeground, spine());
        EXPECT_EQ(task->Poll(), PollResult::kSuspended);
        EXPECT_FALSE(leaf_destroyed);
    }
    EXPECT_TRUE(leaf_destroyed) << "the chain's frames must die with the task";
}

TEST(CoroNestedTest, TryResumeDeepestRefusesAnUnsatisfiedWait) {
    // The synchronous boundary drivers' contract (P4d-3): a caller that
    // cannot wait must not resume a coroutine parked on a wait, because
    // entering the body acts as though the reply had arrived. `false` and
    // no resume - never a fabricated wake-up.
    bool batch_ready = false;
    int leaf_entries = 0;

    auto leaf = [&]() -> Coro {
        ++leaf_entries;
        co_await WaitFor{&batch_ready};
        ++leaf_entries;
        co_return Status::OK();
    };
    auto spine = [&]() -> Coro { co_return co_await leaf(); };

    Coro coro = spine();
    EXPECT_TRUE(coro.TryResumeDeepest());  // spine runs to co_await leaf()
    EXPECT_TRUE(coro.TryResumeDeepest());  // leaf runs to its wait
    EXPECT_EQ(leaf_entries, 1);

    // Parked and unsatisfied: refused, repeatedly, with no body re-entry.
    EXPECT_FALSE(coro.TryResumeDeepest());
    EXPECT_FALSE(coro.TryResumeDeepest());
    EXPECT_EQ(leaf_entries, 1);
    EXPECT_FALSE(coro.done());

    // Satisfied: consumed exactly as CoroTask::Poll consumes one, and the
    // chain then drives to completion.
    batch_ready = true;
    while (!coro.done()) {
        EXPECT_TRUE(coro.TryResumeDeepest());
    }
    EXPECT_EQ(leaf_entries, 2);
    EXPECT_TRUE(coro.result().ok());
}

TEST(CoroNestedTest, TryResumeDeepestRefusesAnUnsatisfiedPredicateToo) {
    // The WaitUntil half of the same contract - it shares
    // ConsumeWaitIfSatisfied with Poll, and this is the test that keeps
    // the predicate form from drifting out of that sharing.
    bool durable = false;
    const std::function<bool()> pred = [&] { return durable; };
    int entries = 0;

    auto leaf = [&]() -> Coro {
        ++entries;
        co_await WaitUntil{&pred};
        ++entries;
        co_return Status::OK();
    };
    Coro coro = leaf();
    EXPECT_TRUE(coro.TryResumeDeepest());  // runs to the wait
    EXPECT_FALSE(coro.TryResumeDeepest());
    EXPECT_FALSE(coro.TryResumeDeepest());
    EXPECT_EQ(entries, 1);

    durable = true;
    while (!coro.done()) {
        EXPECT_TRUE(coro.TryResumeDeepest());
    }
    EXPECT_EQ(entries, 2);
    EXPECT_TRUE(coro.result().ok());
}

}  // namespace
}  // namespace kds::sched
