// **A statement sees its own session's acknowledged commit while another
// core's `strict` commit syncs** (BA-S1c, BA-R1c,
// `instructions/v3.0.0/workorder-ba-parallelism.md`), on the two-core rig.
//
// A commit holds its snapshot marker from before its append until after its
// publish, and every new snapshot on every core is capped below the lowest
// marker held (`InstanceVisibility::SnapshotCeiling`). Under `strict` that
// span includes the `fdatasync`. So while core 0's `strict` commit syncs,
// core 1 can commit `relaxed`, be acknowledged, and then mint a snapshot
// capped below its own commit: the next statement of the same session
// misses the row it was just told is committed.
//
// BA-R1c closes the session's own case: the session records the LSN of its
// last acknowledged commit, and a statement whose bound sits above the
// ceiling parks at the statement boundary until the markers below it lift -
// kicked by the commit that lifts the last of them.

#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/contention.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/session.hpp"
#include "kds/wal/durability.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

bool StartsWith(const std::string& reply, std::string_view prefix) {
    return reply.rfind(prefix, 0) == 0;
}

// A session's statements on one reactor, in order, once `go` is set. Each
// statement's response is published by its own `done` count, so the test
// thread reads only responses the reactor has finished writing. The last
// line runs through the synchronous `Dispatch` when `last_sync` is set -
// the path `KwpLoadServer` takes on a reactor.
struct Script {
    Session* session = nullptr;
    std::vector<std::string> lines;
    bool last_sync = false;
    std::vector<DispatchOutcome> outs;
    std::function<bool()> go_pred;
    std::atomic<bool> go{false};
    std::atomic<std::size_t> done{0};
};

sched::Coro RunScript(CommandDispatcher& d, Script& s) {
    s.go_pred = [&s] { return s.go.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&s.go_pred};
    s.outs.resize(s.lines.size());
    for (std::size_t i = 0; i < s.lines.size(); ++i) {
        if (s.last_sync && i + 1 == s.lines.size()) {
            s.outs[i] = d.Dispatch(s.lines[i], s.session);
        } else {
            co_await d.DispatchAsync(s.lines[i], s.session, &s.outs[i]);
        }
        s.done.store(i + 1, std::memory_order_release);
    }
    co_return Status::OK();
}

std::string Response(const Script& s, std::size_t i) {
    if (s.done.load(std::memory_order_acquire) <= i) return "(not done)";
    return s.outs[i].response;
}

// **The shape every cell here runs.** Core 0 commits `strict` and is held
// inside its sync. Core 1's session runs `writes`, the last of which commits
// `relaxed` and is acknowledged, and then `SELECT`s the row it wrote - or,
// with `other_session`, a second session on core 1 that has committed nothing
// does, once the writer is acknowledged: the statement that began after the
// commit was acknowledged must see it, whoever told its client.
void SeesItsOwnCommit(const std::vector<std::string>& writes, bool select_sync,
                      bool other_session = false) {
    TwoCoreRig::Options options;
    options.gated_log_sync = true;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();

    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE TABLE t (id int64, v int64)").response, "CREATED"));
    // Core 1 carves its transaction-id window now, while syncs land: the
    // carve persists page 0, whose writeback waits on the log, and a held
    // sync would stall core 1's write on that instead of the marker. Every
    // key named after this row ascends above it: a named key below the
    // relation's mark is refused (BB-R3).
    ASSERT_FALSE(StartsWith(d1.Dispatch("INSERT INTO t VALUES (100, 0)").response, "ERR"));

    Session strict_session;
    strict_session.set_durability(wal::DurabilityClass::kStrict);
    Script strict{&strict_session, {"INSERT INTO t VALUES (101, 1)"}};
    Session relaxed_session;
    relaxed_session.set_durability(wal::DurabilityClass::kRelaxed);
    Script relaxed{&relaxed_session, writes, select_sync && !other_session};
    Session reader_session;
    Script reader{&reader_session, {"SELECT id FROM t WHERE id = 102"}, select_sync};
    if (!other_session) relaxed.lines.push_back("SELECT id FROM t WHERE id = 102");
    // The statement under test is the writer's own last line, or the reader's
    // only one; `writes_done` is how many of the writer's lines are writes.
    Script& asker = other_session ? reader : relaxed;
    const std::size_t select = other_session ? 0 : relaxed.lines.size() - 1;
    const std::size_t writes_done = other_session ? relaxed.lines.size() : select;
    rig->core(0).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(d0, strict)));
    rig->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(d1, relaxed)));
    if (other_session) {
        rig->core(1).scheduler().Submit(
            sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(d1, reader)));
    }
    // A failed assertion below must neither leave core 0 inside its sync,
    // whose reactor the teardown would then never join, nor leave both
    // reactors running over the scripts this scope destroys before the rig.
    struct StopAtExit {
        TwoCoreRig& rig;
        ~StopAtExit() {
            rig.log_gate().Release();
            rig.Stop();
        }
    } stop_at_exit{*rig};
    rig->log_gate().Hold();
    rig->Start();

    // Core 0's `strict` commit is inside its sync: its marker is set, its
    // reactor blocked in the device. Both, because another sync parked at
    // the gate - a drain tick, the writer - would satisfy the first alone.
    const txn::CoreVisibilitySlot& core0_slot = rig->visibility().slot(0);
    strict.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 0, [&] {
        return rig->log_gate().parked() >= 1 &&
               core0_slot.pending_commit_bound.load() != txn::kUnboundedBound;
    })) << "core 0's strict commit never reached its sync";

    // BA-S2's ceiling count, read before the statement that waits.
    const Contention::Snapshot before = Contention::Read();

    // Core 1 commits `relaxed` and is acknowledged.
    relaxed.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return relaxed.done.load() >= writes_done; }))
        << "core 1's writes never finished";
    for (std::size_t i = 0; i < writes_done; ++i) {
        ASSERT_FALSE(StartsWith(Response(relaxed, i), "ERR")) << Response(relaxed, i);
    }
    // The race this shape is about: the acknowledged commit sits above every
    // snapshot a mint could take now. Ordered by `done`'s acquire.
    ASSERT_LT(rig->visibility().SnapshotCeiling(), relaxed_session.acknowledged_commit_lsn());
    // The other session asks only now, after the commit was acknowledged.
    if (other_session) reader.go.store(true, std::memory_order_release);

    // Its next statement waits for core 0's marker rather than read under it,
    // counted into core 1's ceiling waiters - parked, or holding its reactor
    // on the synchronous path.
    const txn::CoreVisibilitySlot& peer_slot = rig->visibility().slot(1);
    const auto answered_or_waiting = [&] {
        return asker.done.load() > select || peer_slot.ceiling_waiters.load() >= 1;
    };
    // The reader was released just now and its reactor may be idle: kick it.
    ASSERT_TRUE(other_session ? KickUntil(*rig, 1, answered_or_waiting)
                              : Within(2000ms, answered_or_waiting));
    EXPECT_EQ(asker.done.load(), select)
        << "core 1's SELECT did not wait for the marker; it answered " << Response(asker, select);

    // The sync lands, the marker lifts, and the lift kicks core 1.
    rig->log_gate().Release();
    ASSERT_TRUE(Within(1000ms, [&] { return asker.done.load() > select; }))
        << "core 1's SELECT did not proceed at the marker's lift";
    EXPECT_EQ(Response(asker, select), "id\\n102")
        << "the statement missed a commit acknowledged before it began";
    ASSERT_TRUE(Within(2000ms, [&] { return strict.done.load() >= 1; }));
    EXPECT_TRUE(StartsWith(Response(strict, 0), "INSERTED")) << Response(strict, 0);
    // No waiter outlives its statement.
    EXPECT_EQ(peer_slot.ceiling_waiters.load(), 0u);
    // **And the wait is counted** (BA-S2): one statement waited, for at
    // least the time this thread held the gate after it registered.
    const Contention::Snapshot after = Contention::Read();
    EXPECT_GE(after.of(Tally::kCeilingWaits) - before.of(Tally::kCeilingWaits), 1u);
    EXPECT_GT(after.of(Tally::kCeilingWaitNs), before.of(Tally::kCeilingWaitNs));
}

// **Mutations**, each run repeatedly (BA-S1c): the bound never recorded -
// every cell misses its row, as on `dfabae1`; `DispatchAsync`'s wait
// skipped - the two parked cells miss it; `Dispatch`'s wait skipped - the
// synchronous cell misses it; the marker's lift kicking nobody - the parked
// cells wait out core 1's idle block (5 s on this rig) and fail the 1 s
// bound.

TEST(StrictMarkerSnapshotRigTest, AnAutocommitWriteIsSeenByTheSessionsNextStatement) {
    // Red at `dfabae1`: core 1's SELECT ran at once, under a snapshot capped
    // by core 0's marker, and answered no row.
    SeesItsOwnCommit({"INSERT INTO t VALUES (102, 2)"}, /*select_sync=*/false);
}

TEST(StrictMarkerSnapshotRigTest, AnExplicitCommitIsSeenByTheSessionsNextStatement) {
    // `COMMIT`'s arm records the bound, not the autocommit write's.
    SeesItsOwnCommit({"BEGIN", "INSERT INTO t VALUES (102, 2)", "COMMIT"}, /*select_sync=*/false);
}

TEST(StrictMarkerSnapshotRigTest, ASynchronousDispatchWaitsForTheBoundToo) {
    // The synchronous path cannot park: it holds its reactor until the lift,
    // as its own group commit blocks.
    SeesItsOwnCommit({"INSERT INTO t VALUES (102, 2)"}, /*select_sync=*/true);
}

// **Another session's commit** (the half BA-S1c left): a statement that
// began after a commit was acknowledged to someone else must see it too,
// whatever the session it ran in, because `READ COMMITTED` promises a
// statement everything committed before it began (`txn.md`'s level table).
TEST(StrictMarkerSnapshotRigTest, AnotherSessionsAcknowledgedCommitIsSeenByAStatementThatBeganAfter) {
    SeesItsOwnCommit({"INSERT INTO t VALUES (102, 2)"}, /*select_sync=*/false,
                     /*other_session=*/true);
}

TEST(StrictMarkerSnapshotRigTest, AnotherSessionsExplicitCommitIsSeenByTheSynchronousPathToo) {
    SeesItsOwnCommit({"BEGIN", "INSERT INTO t VALUES (102, 2)", "COMMIT"}, /*select_sync=*/true,
                     /*other_session=*/true);
}

TEST(StrictMarkerSnapshotRigTest, WithNoMarkerHeldNoStatementWaitsForAnyCommit) {
    // The cost is only where a marker is held: with none, another session's
    // acknowledged commit is already under the ceiling, the statement reads it
    // at once, and nothing is counted as a ceiling wait.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE TABLE t (id int64, v int64)").response, "CREATED"));

    Session writer;
    writer.set_durability(wal::DurabilityClass::kRelaxed);
    const Contention::Snapshot before = Contention::Read();
    ASSERT_TRUE(StartsWith(d1.Dispatch("INSERT INTO t VALUES (7, 1)", &writer).response, "INSERTED"));
    Session reader;
    EXPECT_EQ(d0.Dispatch("SELECT id FROM t WHERE id = 7", &reader).response, "id\\n7");
    const Contention::Snapshot after = Contention::Read();
    EXPECT_EQ(after.of(Tally::kCeilingWaits), before.of(Tally::kCeilingWaits));
}

}  // namespace
}  // namespace kds::server
