// **A writer parked on another core's row is kicked when the holder
// releases it** (AX-S2b, `instructions/v3.0.0/workorder-ax-inflight-publication.md`
// §6), on the two-core rig.
//
// Since AX-S1 a write that meets an undecided holder on another core waits
// for it rather than being refused. Until AX-S2b the wait was a poll of
// `IsInFlight` with nothing behind it: the holder's decide kicked no one, so
// a waiter whose reactor had gone to sleep saw the decide at the end of its
// idle block, and its re-run could meet the holder's tuple borrow still held
// - the in-flight table is retired before the borrows are released - and be
// refused. Now the row's refused borrow registers a wake on the unit, the
// wait is on that slot, and the release flips it and kicks the waiter's
// core: `lock_wake_rig_test.cpp`'s table-level shape, reached by a
// statement.
//
// Core 0's transaction is opened synchronously before the reactors start;
// its `COMMIT` and core 1's `UPDATE` run as tasks on their own reactors.

#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/session.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

bool StartsWith(const std::string& reply, std::string_view prefix) {
    return reply.rfind(prefix, 0) == 0;
}

// One statement on a reactor, once `go` is set. The predicate is a member
// because `WaitUntil` holds a pointer across the park.
struct Statement {
    Session* session = nullptr;
    std::string line;
    DispatchOutcome out;
    std::function<bool()> go_pred;
    std::atomic<bool> go{true};
    std::atomic<bool> done{false};
};

sched::Coro RunStatement(CommandDispatcher& d, Statement& s) {
    s.go_pred = [&s] { return s.go.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&s.go_pred};
    co_await d.DispatchAsync(s.line, s.session, &s.out);
    s.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

TEST(RowWaitWakeRigTest, AWriterParkedOnAnotherCoresRowProceedsAtTheReleaseKick) {
    // Red at `1040f60`: the decide wrote no kick to core 1, and core 1's
    // `UPDATE` ran only when its idle block (5 s here) expired.
    //
    // **Mutation**: `BorrowChain`'s unit ask registering no wake - the
    // write block carries no slot, the wait falls back to the poll, and no
    // kick to core 1 follows the commit.
    TwoCoreRig::Options options;
    options.wake.min_delay_ticks = 2;
    options.wake.max_delay_ticks = 2;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();

    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE TABLE t (id int64, v int64)").response, "CREATED"));
    ASSERT_FALSE(StartsWith(d0.Dispatch("INSERT INTO t VALUES (1, 0)").response, "ERR"));
    Session holder;
    ASSERT_TRUE(StartsWith(d0.Dispatch("BEGIN", &holder).response, "BEGIN"));
    const std::string held = d0.Dispatch("UPDATE t SET v = 1 WHERE id = 1", &holder).response;
    ASSERT_FALSE(StartsWith(held, "ERR")) << held;

    Session waiter;
    Statement update{&waiter, "UPDATE t SET v = 2 WHERE id = 1"};
    Statement commit{&holder, "COMMIT"};
    commit.go.store(false, std::memory_order_release);
    rig->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunStatement(d1, update)));
    rig->core(0).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunStatement(d0, commit)));
    rig->Start();

    // Core 1's statement has met the held row and parked, and its reactor
    // has gone to sleep over it.
    sched::Scheduler& peer = rig->core(1).scheduler();
    ASSERT_TRUE(Within(2000ms, [&] { return peer.idle_blocks() >= 1; }))
        << "core 1 never blocked with its UPDATE parked";
    ASSERT_FALSE(update.done.load(std::memory_order_acquire))
        << "core 1's UPDATE did not wait: " << update.out.response;
    const std::size_t kicks_to_peer_before = KicksTo(*rig, 1).size();

    // The holder decides, on its own reactor; the cell's kick to core 0 is
    // the barrier, through the real table.
    commit.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return commit.done.load(std::memory_order_acquire); }));
    ASSERT_TRUE(StartsWith(commit.out.response, "COMMIT")) << commit.out.response;

    // The release flipped the waiter's slot and kicked core 1 - through the
    // sim, so the kick is logged, addressed to core 1, and held.
    const std::vector<sched::SimWakerTable::Record> to_peer = KicksTo(*rig, 1);
    ASSERT_EQ(to_peer.size(), kicks_to_peer_before + 1) << "the decide did not kick core 1";
    EXPECT_FALSE(update.done.load(std::memory_order_acquire))
        << "core 1's UPDATE ran before any kick reached its reactor";

    rig->wake().Advance(2);
    ASSERT_TRUE(Within(1000ms, [&] { return update.done.load(std::memory_order_acquire); }))
        << "core 1's UPDATE did not proceed at the kick";
    // The re-run found the row released, not merely decided.
    EXPECT_TRUE(StartsWith(update.out.response, "UPDATED 1")) << update.out.response;
    rig->Stop();
    // No registration outlives the statement that made it.
    EXPECT_EQ(rig->locks().EntryCount(), 0u);
}

}  // namespace
}  // namespace kds::server
