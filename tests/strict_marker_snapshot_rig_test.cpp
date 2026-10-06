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
// `relaxed` and is acknowledged, and then `SELECT`s the row it wrote.
void SeesItsOwnCommit(const std::vector<std::string>& writes, bool select_sync) {
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
    Script relaxed{&relaxed_session, writes, select_sync};
    relaxed.lines.push_back("SELECT id FROM t WHERE id = 102");
    const std::size_t select = relaxed.lines.size() - 1;
    rig->core(0).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(d0, strict)));
    rig->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(d1, relaxed)));
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

    // Core 1 commits `relaxed` and is acknowledged.
    relaxed.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return relaxed.done.load() >= select; }))
        << "core 1's writes never finished";
    for (std::size_t i = 0; i < select; ++i) {
        ASSERT_FALSE(StartsWith(Response(relaxed, i), "ERR")) << Response(relaxed, i);
    }
    // The race this shape is about: the acknowledged commit sits above every
    // snapshot a mint could take now. Ordered by `done`'s acquire.
    ASSERT_LT(rig->visibility().SnapshotCeiling(), relaxed_session.acknowledged_commit_lsn());

    // Its next statement waits for core 0's marker rather than read under it,
    // counted into core 1's ceiling waiters - parked, or holding its reactor
    // on the synchronous path.
    const txn::CoreVisibilitySlot& peer_slot = rig->visibility().slot(1);
    ASSERT_TRUE(Within(2000ms, [&] {
        return relaxed.done.load() > select || peer_slot.ceiling_waiters.load() >= 1;
    }));
    EXPECT_EQ(relaxed.done.load(), select)
        << "core 1's SELECT did not wait for the marker; it answered " << Response(relaxed, select);

    // The sync lands, the marker lifts, and the lift kicks core 1.
    rig->log_gate().Release();
    ASSERT_TRUE(Within(1000ms, [&] { return relaxed.done.load() > select; }))
        << "core 1's SELECT did not proceed at the marker's lift";
    EXPECT_EQ(Response(relaxed, select), "id\\n102")
        << "the session missed its own acknowledged commit";
    ASSERT_TRUE(Within(2000ms, [&] { return strict.done.load() >= 1; }));
    EXPECT_TRUE(StartsWith(Response(strict, 0), "INSERTED")) << Response(strict, 0);
    // No waiter outlives its statement.
    EXPECT_EQ(peer_slot.ceiling_waiters.load(), 0u);
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

}  // namespace
}  // namespace kds::server
