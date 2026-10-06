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
// thread reads only responses the reactor has finished writing.
struct Script {
    Session* session = nullptr;
    std::vector<std::string> lines;
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
        co_await d.DispatchAsync(s.lines[i], s.session, &s.outs[i]);
        s.done.store(i + 1, std::memory_order_release);
    }
    co_return Status::OK();
}

std::string Response(const Script& s, std::size_t i) {
    if (s.done.load(std::memory_order_acquire) <= i) return "(not done)";
    return s.outs[i].response;
}

TEST(StrictMarkerSnapshotRigTest, ASessionSeesItsOwnAcknowledgedCommitWhileAnotherCoreSyncs) {
    // Red at `dfabae1`: core 1's SELECT ran at once, under a snapshot capped
    // by core 0's marker, and answered no row.
    //
    // **Mutation**: the statement boundary ignoring the session's bound -
    // the SELECT runs at once and misses its row, as on `dfabae1`. And the
    // marker's lift kicking nobody - the SELECT waits out core 1's idle
    // block (5 s on this rig) and the 1 s bound below fails.
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
    // sync would stall core 1's INSERT on that instead of the marker.
    ASSERT_FALSE(StartsWith(d1.Dispatch("INSERT INTO t VALUES (100, 0)").response, "ERR"));
    // A failed assertion below must not leave core 0 inside its sync, or the
    // rig's teardown joins a reactor that never returns.
    struct ReleaseAtExit {
        GatedLogDevice& gate;
        ~ReleaseAtExit() { gate.Release(); }
    } release_at_exit{rig->log_gate()};

    Session strict_session;
    strict_session.set_durability(wal::DurabilityClass::kStrict);
    Script strict{&strict_session, {"INSERT INTO t VALUES (1, 1)"}};
    Session relaxed_session;
    relaxed_session.set_durability(wal::DurabilityClass::kRelaxed);
    Script relaxed{&relaxed_session,
                   {"INSERT INTO t VALUES (2, 2)", "SELECT id FROM t WHERE id = 2"}};
    rig->core(0).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(d0, strict)));
    rig->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(d1, relaxed)));
    rig->log_gate().Hold();
    rig->Start();

    // Core 0's `strict` commit is inside its sync: its marker is set, its
    // reactor blocked in the device.
    strict.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return rig->log_gate().parked() >= 1; }))
        << "core 0's strict commit never reached its sync";

    // Core 1 commits `relaxed` and is acknowledged.
    relaxed.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return relaxed.done.load() >= 1; }))
        << "core 1's INSERT never finished";
    ASSERT_TRUE(StartsWith(Response(relaxed, 0), "INSERTED")) << Response(relaxed, 0);

    // Its next statement waits for core 0's marker rather than read under it:
    // core 1's reactor goes to sleep with the SELECT parked.
    sched::Scheduler& peer = rig->core(1).scheduler();
    const std::uint64_t blocks_before = peer.idle_blocks();
    ASSERT_TRUE(Within(2000ms, [&] {
        return relaxed.done.load() >= 2 || peer.idle_blocks() > blocks_before;
    }));
    EXPECT_EQ(relaxed.done.load(), 1u)
        << "core 1's SELECT did not wait for the marker; it answered " << Response(relaxed, 1);

    // The sync lands, the marker lifts, and the lift kicks core 1.
    rig->log_gate().Release();
    ASSERT_TRUE(Within(1000ms, [&] { return relaxed.done.load() >= 2; }))
        << "core 1's SELECT did not proceed at the marker's lift";
    EXPECT_EQ(Response(relaxed, 1), "id\\n2")
        << "the session missed its own acknowledged commit";
    ASSERT_TRUE(Within(2000ms, [&] { return strict.done.load() >= 1; }));
    EXPECT_TRUE(StartsWith(Response(strict, 0), "INSERTED")) << Response(strict, 0);
    rig->Stop();
}

}  // namespace
}  // namespace kds::server
