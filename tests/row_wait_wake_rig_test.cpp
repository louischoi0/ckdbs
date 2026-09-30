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
#include "kds/txn/lock_table.hpp"

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

// **What a timed-out wait saw** (AZ-S4), for the one-off failures
// `known-gaps.md` (Testing) records. Read on the test thread while both
// reactors run, so only what is safe to read there: the statement's response
// once its `done` is seen - the release store follows the reactor's last
// write to it - and the scheduler's and tables' atomic counters.
std::string Seen(const Statement& s) {
    if (!s.done.load(std::memory_order_acquire)) return "the statement not done";
    return "the statement done, response '" + s.out.response + "'";
}

// The wake path to one core, from a baseline: a kick still held in the sim,
// one the real table skipped on a clear sleep flag (best-effort, costing one
// idle block), one written to the core's eventfd, and the blocks the core
// took. Tells "the kick never landed" from "it landed and the statement was
// slow".
struct WakePath {
    std::uint64_t idle_blocks = 0;
    std::uint64_t wakes_received = 0;
    std::uint64_t kicks_skipped = 0;
};

WakePath WakesTo(TwoCoreRig& rig, std::uint32_t core) {
    const sched::Scheduler& s = rig.core(core).scheduler();
    return {s.idle_blocks(), s.wakes_received(), rig.wakers().kicks_skipped()};
}

std::string Since(const WakePath& before, TwoCoreRig& rig, std::uint32_t core) {
    const WakePath now = WakesTo(rig, core);
    return "since the kick: " + std::to_string(rig.wake().in_flight()) +
           " kick(s) held in the sim, " +
           std::to_string(now.kicks_skipped - before.kicks_skipped) + " skipped, " +
           std::to_string(now.wakes_received - before.wakes_received) +
           " wake(s) received and " + std::to_string(now.idle_blocks - before.idle_blocks) +
           " idle block(s) on core " + std::to_string(core);
}

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
        << "core 1 never blocked with its UPDATE parked; saw " << Seen(update);
    ASSERT_FALSE(update.done.load(std::memory_order_acquire))
        << "core 1's UPDATE did not wait: " << update.out.response;
    const std::size_t kicks_to_peer_before = KicksTo(*rig, 1).size();

    // The holder decides, on its own reactor; the cell's kick to core 0 is
    // the barrier, through the real table.
    commit.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return commit.done.load(std::memory_order_acquire); }))
        << "core 0's COMMIT never finished; saw " << Seen(commit);
    ASSERT_TRUE(StartsWith(commit.out.response, "COMMIT")) << commit.out.response;

    // The release flipped the waiter's slot and kicked core 1 - through the
    // sim, so the kick is logged, addressed to core 1, and held.
    const std::vector<sched::SimWakerTable::Record> to_peer = KicksTo(*rig, 1);
    ASSERT_EQ(to_peer.size(), kicks_to_peer_before + 1) << "the decide did not kick core 1";
    EXPECT_FALSE(update.done.load(std::memory_order_acquire))
        << "core 1's UPDATE ran before any kick reached its reactor";

    const WakePath before_kick = WakesTo(*rig, 1);
    rig->wake().Advance(2);
    ASSERT_TRUE(Within(1000ms, [&] { return update.done.load(std::memory_order_acquire); }))
        << "core 1's UPDATE did not proceed at the kick; " << Since(before_kick, *rig, 1);
    // The re-run found the row released, not merely decided.
    EXPECT_TRUE(StartsWith(update.out.response, "UPDATED 1")) << update.out.response;
    rig->Stop();
    // No registration outlives the statement that made it.
    EXPECT_EQ(rig->locks().EntryCount(), 0u);
}

TEST(RowWaitWakeRigTest, ADeclaredRangeOnCore1ProceedsAtTheReleaseKickOfARowCore0Holds) {
    // **The containment wake** (AY-S2). The same shape as the cell above,
    // with core 1's statement declaring a range (`WHERE id >= 1`) rather
    // than naming the row: its `X` fence meets core 0's tuple `X` in the
    // lock table's verify arm, in another unit's entry - where until AY-S2
    // nothing was registered, the wait fell back to a poll of `IsInFlight`,
    // and no release kicked core 1.
    //
    // **Mutation**: the verify's scans registering nothing - the refusal
    // comes back without a slot, and no kick to core 1 follows the commit.
    TwoCoreRig::Options options;
    options.wake.min_delay_ticks = 2;
    options.wake.max_delay_ticks = 2;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();

    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE TABLE t (id int64, v int64)").response, "CREATED"));
    ASSERT_FALSE(StartsWith(d0.Dispatch("INSERT INTO t VALUES (1, 0), (2, 0)").response, "ERR"));
    Session holder;
    ASSERT_TRUE(StartsWith(d0.Dispatch("BEGIN", &holder).response, "BEGIN"));
    const std::string held = d0.Dispatch("UPDATE t SET v = 1 WHERE id = 1", &holder).response;
    ASSERT_FALSE(StartsWith(held, "ERR")) << held;

    Session waiter;
    Statement update{&waiter, "UPDATE t SET v = 2 WHERE id >= 1"};
    Statement commit{&holder, "COMMIT"};
    commit.go.store(false, std::memory_order_release);
    rig->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunStatement(d1, update)));
    rig->core(0).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunStatement(d0, commit)));
    rig->Start();

    sched::Scheduler& peer = rig->core(1).scheduler();
    ASSERT_TRUE(Within(2000ms, [&] { return peer.idle_blocks() >= 1; }))
        << "core 1 never blocked with its UPDATE parked; saw " << Seen(update);
    ASSERT_FALSE(update.done.load(std::memory_order_acquire))
        << "core 1's UPDATE did not wait: " << update.out.response;
    // Registered on the row's own entry, the unit that refused the range.
    auto oid = rig->core(0).catalog().FindTableOidByName("t");
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    EXPECT_EQ(rig->locks().WaiterCount(txn::LockKey::Tuple(oid.value(), 1)), 1u)
        << "the range's refusal left no registration on the row that refused it";
    const std::size_t kicks_to_peer_before = KicksTo(*rig, 1).size();

    commit.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return commit.done.load(std::memory_order_acquire); }))
        << "core 0's COMMIT never finished; saw " << Seen(commit);
    ASSERT_TRUE(StartsWith(commit.out.response, "COMMIT")) << commit.out.response;

    const std::vector<sched::SimWakerTable::Record> to_peer = KicksTo(*rig, 1);
    ASSERT_EQ(to_peer.size(), kicks_to_peer_before + 1) << "the decide did not kick core 1";
    EXPECT_FALSE(update.done.load(std::memory_order_acquire))
        << "core 1's UPDATE ran before any kick reached its reactor";

    const WakePath before_kick = WakesTo(*rig, 1);
    rig->wake().Advance(2);
    ASSERT_TRUE(Within(1000ms, [&] { return update.done.load(std::memory_order_acquire); }))
        << "core 1's UPDATE did not proceed at the kick; " << Since(before_kick, *rig, 1);
    EXPECT_TRUE(StartsWith(update.out.response, "UPDATED 2")) << update.out.response;
    rig->Stop();
    EXPECT_EQ(rig->locks().EntryCount(), 0u);
}

// The holder's late release, on core 0's reactor once `go` is set.
struct LateRelease {
    txn::LockTable* table = nullptr;
    std::uint64_t txn = 0;
    txn::LockHoldings holdings;
    std::function<bool()> go_pred;
    std::atomic<bool> go{false};
    std::atomic<bool> done{false};
};

sched::Coro RunLateRelease(LateRelease& r) {
    r.go_pred = [&r] { return r.go.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&r.go_pred};
    r.table->Release(r.txn, r.holdings);
    r.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

// `t` with row 1, and `holder`'s transaction open on core 0 holding the
// relation `IX` and row 1's `X` in `late`'s own ledger - which the
// transaction's `COMMIT` does not release, so the release comes when `late`
// says. The stretched retire-to-release window both late-release cells use.
void HoldRowLate(TwoCoreRig& rig, Session& holder, LateRelease& late) {
    CommandDispatcher& d0 = rig.core(0).dispatcher();
    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE TABLE t (id int64, v int64)").response, "CREATED"));
    ASSERT_FALSE(StartsWith(d0.Dispatch("INSERT INTO t VALUES (1, 0)").response, "ERR"));
    auto oid = rig.core(0).catalog().FindTableOidByName("t");
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    ASSERT_TRUE(StartsWith(d0.Dispatch("BEGIN", &holder).response, "BEGIN"));
    ASSERT_NE(holder.transaction(), nullptr);
    late.table = &rig.locks();
    late.txn = holder.transaction()->id();
    ASSERT_TRUE(rig.locks()
                    .TryAcquire(late.txn, txn::LockKey::Relation(oid.value()),
                                txn::LockMode::kIntentionExclusive, late.holdings)
                    .value());
    ASSERT_TRUE(rig.locks()
                    .TryAcquire(late.txn, txn::LockKey::Tuple(oid.value(), 1),
                                txn::LockMode::kExclusive, late.holdings)
                    .value());
}

TEST(RowWaitWakeRigTest, AWaiterOnTheSlotSleepsThroughTheHoldersDecideUntilItsRelease) {
    // **The window the slot closes**: the holder is retired from the
    // in-flight table before its borrows are released (`manager.cpp`'s
    // commit), and a re-run inside that window meets the row's `X` still
    // held by a transaction no longer in flight. The window is microseconds
    // in a real commit; here it is stretched by holding the holder's tuple
    // `X` in a ledger its `COMMIT` does not release, and releasing it late.
    //
    // **Mutation**: the wait's predicate polling `IsInFlight` although a
    // slot is present (AX-S2b's mutant (a)). Until AY-S3 it settled at the
    // commit and the re-run was refused inside the window. Since B6 the
    // re-run waits again, on a predicate already true, so the mutant spins
    // core 1 through the window and ends `UPDATED 1` all the same - which is
    // why the cell asserts that core 1 **sleeps** there (re-measured, AY-S3).
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();

    Session holder;
    LateRelease late;
    HoldRowLate(*rig, holder, late);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    Session waiter;
    Statement update{&waiter, "UPDATE t SET v = 2 WHERE id = 1"};
    Statement commit{&holder, "COMMIT"};
    commit.go.store(false, std::memory_order_release);
    rig->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunStatement(d1, update)));
    rig->core(0).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunStatement(d0, commit)));
    rig->core(0).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunLateRelease(late)));
    rig->Start();

    ASSERT_TRUE(Within(2000ms, [&] { return rig->core(1).scheduler().idle_blocks() >= 1; }))
        << "core 1 never blocked with its UPDATE parked; saw " << Seen(update);
    ASSERT_FALSE(update.done.load(std::memory_order_acquire))
        << "core 1's UPDATE did not wait: " << update.out.response;

    // The holder decides and is out of the in-flight table; its `X` stays.
    commit.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return commit.done.load(std::memory_order_acquire); }))
        << "core 0's COMMIT never finished; saw " << Seen(commit);
    ASSERT_TRUE(StartsWith(commit.out.response, "COMMIT")) << commit.out.response;
    ASSERT_FALSE(rig->core(1).transactions().IsInFlight(late.txn));
    // Core 1 is run for 50 ms inside the window, well inside the 1 s net
    // the statement's deadline is: a poll would settle now. Kicked every
    // millisecond, a reactor whose only task is parked blocks again after
    // each kick; one whose task keeps re-running never blocks.
    const std::uint64_t idle_before = rig->core(1).scheduler().idle_blocks();
    EXPECT_FALSE(KickUntil(*rig, 1, [&] { return update.done.load(std::memory_order_acquire); },
                           50ms))
        << "core 1's UPDATE ran between the decide and the release: " << update.out.response;
    EXPECT_GE(rig->core(1).scheduler().idle_blocks(), idle_before + 2)
        << "core 1 never slept inside the window, so its UPDATE was re-running rather than "
           "parked on the slot";

    late.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return late.done.load(std::memory_order_acquire); }));
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return update.done.load(std::memory_order_acquire); }));
    EXPECT_TRUE(StartsWith(update.out.response, "UPDATED 1")) << update.out.response;
    rig->Stop();
    EXPECT_EQ(rig->locks().EntryCount(), 0u) << "a wake registration outlived its statement";
}

TEST(RowWaitWakeRigTest, AFirstEncounterInsideTheRetireToReleaseWindowWaitsForTheRelease) {
    // **B6** (AY-S3; `raft-marks-2026-09-29.md` §13). The cell above,
    // reordered: the holder has decided - retired from the in-flight table -
    // *before* core 1's statement first meets its row, whose `X` is still
    // held. The refused ask registers its wake on the row as always, but
    // `NoteBlockingWriter` returned on `!IsInFlight` before it recorded
    // anything, so the statement was refused `TxnConflict` for a wait that
    // would have ended at the release. It now records the block whenever the
    // refusing unit's wake names the holder.
    //
    // **Mutation**: the early return restored ahead of the wake test.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();

    Session holder;
    LateRelease late;
    HoldRowLate(*rig, holder, late);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    // The holder decides first: out of the in-flight table, its `X` held.
    ASSERT_TRUE(StartsWith(d0.Dispatch("COMMIT", &holder).response, "COMMIT"));
    ASSERT_FALSE(rig->core(1).transactions().IsInFlight(late.txn));

    Session waiter;
    Statement update{&waiter, "UPDATE t SET v = 2 WHERE id = 1"};
    rig->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunStatement(d1, update)));
    rig->core(0).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunLateRelease(late)));
    rig->Start();

    // Checked without an early return: on a failure the rig must still stop
    // before the statements on this frame go.
    EXPECT_TRUE(Within(2000ms, [&] { return rig->core(1).scheduler().idle_blocks() >= 1; }))
        << "core 1 never blocked; saw " << Seen(update);
    const bool answered_early = update.done.load(std::memory_order_acquire);
    EXPECT_FALSE(answered_early)
        << "the first encounter inside the window was answered rather than waited: "
        << update.out.response;

    late.go.store(true, std::memory_order_release);
    EXPECT_TRUE(KickUntil(*rig, 0, [&] { return late.done.load(std::memory_order_acquire); }));
    EXPECT_TRUE(KickUntil(*rig, 1, [&] { return update.done.load(std::memory_order_acquire); }));
    rig->Stop();
    if (!answered_early) {
        EXPECT_TRUE(StartsWith(update.out.response, "UPDATED 1")) << update.out.response;
    }
    EXPECT_EQ(rig->locks().EntryCount(), 0u) << "a wake registration outlived its statement";
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
    // victim (AO-R7) - refused before it parks, one of the two exits where
    // the wake its ask registered is still on the outcome and
    // `RefuseParkedWrite` drops it (the other, the net read at the loop's
    // top, no cell reaches). B's rollback releases row 2, which
    // flips A's slot and kicks core 0, and A's re-run takes the row.
    //
    // **Mutation**: the drop in `RefuseParkedWrite` removed - B's
    // registration on row 1 outlives it and the entry with it; and
    // `NoteWaitFor` never finding a cycle - B parks, and both waits end at
    // the fault net (killed 3/3 at AX-S3, once the victim's message rather
    // than the word "deadlock" was asserted). The statement-level cross-core
    // cycle; the one-core cycles are `lock_family_test.cpp`'s
    // `LockDeadlockTest` cells, restored at AY-S1.
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
    ASSERT_TRUE(Within(2000ms, [&] { return rig->core(0).scheduler().idle_blocks() >= 1; }))
        << "core 0 never blocked with A parked; saw " << Seen(a_waits);
    ASSERT_FALSE(a_waits.done.load(std::memory_order_acquire)) << a_waits.out.response;

    b_closes.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return b_closes.done.load(std::memory_order_acquire); }));
    EXPECT_TRUE(StartsWith(b_closes.out.response, "ERR")) << b_closes.out.response;
    // The victim's own refusal: the fault net's names deadlock too ("a
    // deadlock went undetected"), so a bare "deadlock" passed with the
    // detector removed.
    EXPECT_NE(b_closes.out.response.find("deadlock: this transaction waited for"),
              std::string::npos)
        << b_closes.out.response;
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
