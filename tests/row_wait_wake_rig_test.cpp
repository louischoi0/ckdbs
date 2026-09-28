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

TEST(RowWaitWakeRigTest, AWaitRefusedAsFutileLeavesNoRegistrationOnTheRow) {
    // The registration is made at the ask, before anyone decides whether
    // the statement will wait for it. A repeatable-read writer meeting the
    // row's own undecided writer is refused rather than parked - its view
    // could never see that commit (`NoteBlockingWriter`) - so its wake is
    // handed to no wait, and `DispatchAndStage`'s end is what drops it.
    //
    // **Mutation**: that drop removed - the leaked registration keeps the
    // row's entry alive after both transactions have decided.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();

    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE TABLE t (id int64, v int64)").response, "CREATED"));
    ASSERT_FALSE(StartsWith(d0.Dispatch("INSERT INTO t VALUES (1, 0)").response, "ERR"));
    Session holder;
    ASSERT_TRUE(StartsWith(d0.Dispatch("BEGIN", &holder).response, "BEGIN"));
    ASSERT_FALSE(
        StartsWith(d0.Dispatch("UPDATE t SET v = 1 WHERE id = 1", &holder).response, "ERR"));
    Session reader;
    ASSERT_TRUE(StartsWith(
        d1.Dispatch("BEGIN ISOLATION LEVEL REPEATABLE READ", &reader).response, "BEGIN"));

    Statement update{&reader, "UPDATE t SET v = 2 WHERE id = 1"};
    Statement rollback{&reader, "ROLLBACK"};
    Statement commit{&holder, "COMMIT"};
    rollback.go.store(false, std::memory_order_release);
    commit.go.store(false, std::memory_order_release);
    rig->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunStatement(d1, update)));
    rig->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunStatement(d1, rollback)));
    rig->core(0).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunStatement(d0, commit)));
    rig->Start();

    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return update.done.load(std::memory_order_acquire); }));
    EXPECT_TRUE(StartsWith(update.out.response, "ERR")) << update.out.response;
    commit.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return commit.done.load(std::memory_order_acquire); }));
    rollback.go.store(true, std::memory_order_release);
    ASSERT_TRUE(
        KickUntil(*rig, 1, [&] { return rollback.done.load(std::memory_order_acquire); }));
    rig->Stop();
    EXPECT_EQ(rig->locks().EntryCount(), 0u) << "a wake registration outlived its statement";
}

TEST(RowWaitWakeRigTest, ACrossCoreRowCycleRefusesTheWaiterThatClosesItAndTheOtherProceeds) {
    // A on core 0 holds row 1 and waits for row 2; B on core 1 holds row 2
    // and asks for row 1. B's registration closes the cycle, so B is the
    // victim (AO-R7) - refused before it parks, which is the one exit where
    // the wake its ask registered is still on the outcome and
    // `RefuseParkedWrite` drops it. B's rollback releases row 2, which
    // flips A's slot and kicks core 0, and A's re-run takes the row.
    //
    // **Mutation**: the drop in `RefuseParkedWrite` removed - B's
    // registration on row 1 outlives it and the entry with it. Also the
    // statement-level cross-core cycle the lost `LockDeadlockTest` cells
    // (`known-gaps.md`, Testing: R8.3) no longer pin.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();

    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE TABLE t (id int64, v int64)").response, "CREATED"));
    ASSERT_FALSE(StartsWith(d0.Dispatch("INSERT INTO t VALUES (1, 0), (2, 0)").response, "ERR"));
    Session a;
    Session b;
    ASSERT_TRUE(StartsWith(d0.Dispatch("BEGIN", &a).response, "BEGIN"));
    ASSERT_FALSE(StartsWith(d0.Dispatch("UPDATE t SET v = 1 WHERE id = 1", &a).response, "ERR"));
    ASSERT_TRUE(StartsWith(d1.Dispatch("BEGIN", &b).response, "BEGIN"));
    ASSERT_FALSE(StartsWith(d1.Dispatch("UPDATE t SET v = 2 WHERE id = 2", &b).response, "ERR"));

    Statement a_waits{&a, "UPDATE t SET v = 10 WHERE id = 2"};
    Statement a_commit{&a, "COMMIT"};
    Statement b_closes{&b, "UPDATE t SET v = 20 WHERE id = 1"};
    Statement b_rollback{&b, "ROLLBACK"};
    a_commit.go.store(false, std::memory_order_release);
    b_closes.go.store(false, std::memory_order_release);
    b_rollback.go.store(false, std::memory_order_release);
    rig->core(0).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunStatement(d0, a_waits)));
    rig->core(0).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunStatement(d0, a_commit)));
    rig->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunStatement(d1, b_closes)));
    rig->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunStatement(d1, b_rollback)));
    rig->Start();

    // A has met row 2 and parked, and core 0 has gone to sleep over it.
    ASSERT_TRUE(Within(2000ms, [&] { return rig->core(0).scheduler().idle_blocks() >= 1; }));
    ASSERT_FALSE(a_waits.done.load(std::memory_order_acquire)) << a_waits.out.response;

    b_closes.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return b_closes.done.load(std::memory_order_acquire); }));
    EXPECT_TRUE(StartsWith(b_closes.out.response, "ERR")) << b_closes.out.response;
    EXPECT_NE(b_closes.out.response.find("deadlock"), std::string::npos) << b_closes.out.response;
    EXPECT_FALSE(a_waits.done.load(std::memory_order_acquire)) << "the survivor was refused too";

    b_rollback.go.store(true, std::memory_order_release);
    ASSERT_TRUE(
        KickUntil(*rig, 1, [&] { return b_rollback.done.load(std::memory_order_acquire); }));
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return a_waits.done.load(std::memory_order_acquire); }));
    EXPECT_TRUE(StartsWith(a_waits.out.response, "UPDATED 1")) << a_waits.out.response;
    a_commit.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return a_commit.done.load(std::memory_order_acquire); }));
    rig->Stop();
    EXPECT_EQ(rig->locks().EntryCount(), 0u) << "a wake registration outlived its statement";
}

}  // namespace
}  // namespace kds::server
