#include "kds/sched/send_retry.hpp"

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"

// M7: on a full ring the sending task **yields and retries**. Never an
// error to the client, never a reactor block, never a drop. These are the
// three ways the obvious implementations go wrong, and each is checkable.

namespace kds::sched {
namespace {

std::vector<std::byte> Bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    if (!s.empty()) std::memcpy(out.data(), s.data(), s.size());
    return out;
}

MessageHeader Msg(std::uint32_t src, std::uint32_t dst, std::uint64_t request_id,
                  SchedulingGroup group = SchedulingGroup::kForeground) {
    MessageHeader h{};
    h.request_id = request_id;
    h.src_core = src;
    h.dst_core = dst;
    h.session_core = src;
    h.kind = static_cast<std::uint16_t>(RingMessageKind::kStepBatch);
    h.sched_group = static_cast<std::uint16_t>(group);
    return h;
}

TEST(SendRetryTest, ASendIntoAnEmptyRingCompletesOnTheFirstPoll) {
    auto t = RealRingTransport::Create(2, 4, 64);
    ASSERT_TRUE(t.ok());

    Status reported = Status::InvalidArgument("never called");
    auto task = MakeSendRetryTask(t.value(), Msg(0, 1, 1), Bytes("x"),
                                  [&](Status s) { reported = std::move(s); });

    EXPECT_EQ(task->Poll(), PollResult::kDone);
    EXPECT_EQ(task->attempts(), 1u);
    EXPECT_TRUE(reported.ok());
}

TEST(SendRetryTest, AFullRingSuspendsTheTaskInsteadOfFailing) {
    auto t = RealRingTransport::Create(2, /*capacity_slots=*/1, 64);
    ASSERT_TRUE(t.ok());
    ASSERT_TRUE(t.value().TrySend(Msg(0, 1, 0), Bytes("filler")).ok());

    bool done_called = false;
    auto task = MakeSendRetryTask(t.value(), Msg(0, 1, 1), Bytes("x"),
                                  [&](Status) { done_called = true; });

    // Suspended, repeatedly, for as long as the ring stays full - and the
    // completion callback is *not* invoked, because a full ring is not a
    // result. This is the whole of M7: the send has neither succeeded nor
    // failed, it has not happened yet.
    EXPECT_EQ(task->Poll(), PollResult::kSuspended);
    EXPECT_EQ(task->Poll(), PollResult::kSuspended);
    EXPECT_FALSE(done_called);

    // Drain the peer, and the very next poll goes through.
    MessageHeader header{};
    std::vector<std::byte> payload;
    ASSERT_TRUE(t.value().TryReceive(1, header, payload));

    EXPECT_EQ(task->Poll(), PollResult::kDone);
    EXPECT_TRUE(done_called);

    ASSERT_TRUE(t.value().TryReceive(1, header, payload));
    EXPECT_EQ(header.request_id, 1u);
}

TEST(SendRetryTest, ANonRetryableFailureFinishesTheTaskAndIsReported) {
    // An out-of-range destination can never succeed, so retrying it would
    // be an infinite loop over a programming error. It must finish and say
    // so - which is the distinction that makes the ResourceExhausted branch
    // above meaningful.
    auto t = RealRingTransport::Create(2, 4, 64);
    ASSERT_TRUE(t.ok());

    Status reported;
    auto task = MakeSendRetryTask(t.value(), Msg(0, /*dst=*/9, 1), Bytes("x"),
                                  [&](Status s) { reported = std::move(s); });

    EXPECT_EQ(task->Poll(), PollResult::kDone);
    EXPECT_EQ(reported.code(), StatusCode::kInvalidArgument);
}

TEST(SendRetryTest, AStalledSendDrainsOnceTheReactorRunsThePeer) {
    // The same story end to end through a real reactor: the sending task is
    // re-queued by the scheduler, not spun on, so the core keeps running
    // everything else in the meantime.
    ManualClock clock;
    NullIoBackend io;
    Scheduler scheduler(clock, io);

    auto t = RealRingTransport::Create(2, /*capacity_slots=*/1, 64);
    ASSERT_TRUE(t.ok());
    ASSERT_TRUE(t.value().TrySend(Msg(0, 1, 0), Bytes("filler")).ok());

    bool sent = false;
    scheduler.Submit(MakeSendRetryTask(t.value(), Msg(0, 1, 1), Bytes("x"),
                                       [&](Status s) { sent = s.ok(); }));

    // Other work on the same core keeps running while the send is stalled -
    // this is the "never blocks the reactor" half of M7.
    int other_work = 0;
    for (int i = 0; i < 3; ++i) {
        scheduler.Submit(std::make_unique<FunctionTask>(
            SchedulingGroup::kForeground, [&] {
                ++other_work;
                return PollResult::kDone;
            }));
    }

    for (int i = 0; i < 5; ++i) scheduler.RunOnce();
    EXPECT_FALSE(sent) << "the send went through with the ring still full";
    EXPECT_EQ(other_work, 3) << "the stalled send blocked the core";

    MessageHeader header{};
    std::vector<std::byte> payload;
    ASSERT_TRUE(t.value().TryReceive(1, header, payload));

    for (int i = 0; i < 5; ++i) scheduler.RunOnce();
    EXPECT_TRUE(sent);
}

}  // namespace
}  // namespace kds::sched
