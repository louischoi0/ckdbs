// **A `group` commit's borrows are released before its sync lands** (CP-S0's
// M6 cell: `instructions/v3.2.0/workorder-cp-lock-model-exploration.md`, §1.1's
// last bullet and the CP-S0 row of §5), on the two-core rig.
//
// The claim, as the tree states it: under `group`, `WalManager::Commit`
// appends the commit record and returns without a sync
// (`src/wal/manager.cpp:424-445`, the `kGroup` arm at `:441-444`);
// `TransactionManager::Commit` then runs `FinishCommitBody`
// (`src/txn/manager.cpp:366-400`, the call at `:398`), which publishes the
// commit into the window (`:306`), lifts the marker (`:316`), retires the
// in-flight entry (`:325`), moves the schema word (`:329`) and **releases
// every borrow** (`:336`) - the relation `IX` and the row's tuple `X` the
// `UPDATE` took at `src/server/command_dispatcher.cpp:10147`. Only the
// acknowledgement waits: `CommitLocal` stages the LSN
// (`command_dispatcher.cpp:10858-10861`; the autocommit scope's twin is
// `:11771-11774`) and `DispatchAsync` parks on `IsDurable` (`:890-914`).
//
// So a second session, on the other core, writes the row the first just
// committed while the first's `COMMIT` is still parked on its durability
// point. Its view covers that commit because the window advances at the
// publish, not at the sync: `PublishCommit` raises `commit_ceiling_`
// (`src/txn/instance_visibility.cpp:313-314`), `SnapshotCeiling` reads it
// capped by the open markers (`:136-157`), a READ COMMITTED statement
// mints from it (`src/txn/manager.cpp:91`, `:120`), `ReadView::Visible`
// compares the window's LSN against that snapshot
// (`include/kds/txn/read_view.hpp:112-114`), and `CheckWriteConflict` passes
// a visible committed writer (`src/txn/manager.cpp:196`). Nothing in
// `src/txn` or `src/exec` reads the writer's watermark.
//
// What the cell corrects: `docs/inflight/known-gaps.md:1417-1423` says
// scenario 2's bookers keep the row's `X` "until its commit is durable".
// Under `group` they do not. Under `strict` above one core the deferred
// commit does hold its borrows across the sync by design (BA-S7,
// `DefersCommit`, `command_dispatcher.cpp:1141-1145`), and
// `strict_park_rig_test.cpp` is that cell; this one is `group`'s.
//
// **The sync is held at the gate** (`GatedLogDevice`, `two_core_rig.hpp`):
// the writer thread parks inside its `Sync`, every append still lands in
// the ring, and nothing in this cell rolls a segment (64 MiB,
// `kDefaultSegmentSize`) or runs a checkpoint (the rig arms no cadence),
// so no appender meets the held gate under the stream latch
// (`two_core_rig.hpp:93-95`, `src/wal/stream.cpp:140-152`).
//
// **Both arrangements run**: the committer on core 0 with the writer on
// core 1, and the reverse. On the rig both cores are attached to the one
// stream and hand their sync to the writer alike (`durable_kick_rig_test.cpp`'s
// note on core 0's arm), so the two cells differ in the direction of the
// release kick and the partition the row's entry lives in, not in the
// commit path.

#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/session.hpp"
#include "kds/txn/instance_visibility.hpp"
#include "kds/txn/lock_table.hpp"
#include "kds/wal/durability.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

bool StartsWith(const std::string& reply, std::string_view prefix) {
    return reply.rfind(prefix, 0) == 0;
}

// A session's statements, in order, once `go` is set.
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
    {
        // A test driver standing in for a client: the reactor runs strict (CJ WE04, I2).
        sched::WaitScope rig_idle(stats::WaitEvent::kIdleClient);
        co_await sched::WaitUntil{&s.go_pred};
    }
    s.outs.resize(s.lines.size());
    for (std::size_t i = 0; i < s.lines.size(); ++i) {
        co_await d.DispatchAsync(s.lines[i], s.session, &s.outs[i]);
        s.done.store(i + 1, std::memory_order_release);
    }
    co_return Status::OK();
}

// Read on the test thread while the reactors run: the response only once
// its `done` is seen - the release store follows the reactor's last write.
std::string Response(const Script& s, std::size_t i) {
    if (s.done.load(std::memory_order_acquire) <= i) return "(not done)";
    return s.outs[i].response;
}

// Session A on `a_core` commits an `UPDATE` of row 1 under `group`, its sync
// held at the gate. Session B on the other core then updates the same row
// inside an explicit transaction - a statement that commits nothing and so
// waits on no sync - and must be answered `UPDATED 1` while A's `COMMIT` is
// still parked.
void TheRowIsWritableBeforeTheCommittersSyncLands(std::uint32_t a_core) {
    const std::uint32_t b_core = 1 - a_core;
    TwoCoreRig::Options options;
    options.gated_log_sync = true;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    CommandDispatcher& da = rig->core(a_core).dispatcher();
    CommandDispatcher& db = rig->core(b_core).dispatcher();
    ASSERT_TRUE(rig->wal().KicksDurableWaiters()) << "the rig wires the writer's kick";

    // Durable before the gate is held: the table, the row, and one
    // autocommit write per core - each core carves its transaction-id window
    // at its first `Begin` (`trx_id.cpp:77`, a 4,096-id block), and the carve
    // persists page 0, whose writeback can wait on the log
    // (`strict_park_rig_test.cpp`'s fixture). Held, that wait would be B's.
    ASSERT_TRUE(StartsWith(da.Dispatch("CREATE TABLE t (id int64, v int64)").response, "CREATED"));
    ASSERT_FALSE(StartsWith(da.Dispatch("INSERT INTO t VALUES (1, 0)").response, "ERR"));
    ASSERT_FALSE(StartsWith(db.Dispatch("INSERT INTO t VALUES (2, 0)").response, "ERR"));

    Session a_session;
    a_session.set_durability(wal::DurabilityClass::kGroup);
    Script a;
    a.session = &a_session;
    a.lines = {"BEGIN", "UPDATE t SET v = 1 WHERE id = 1", "COMMIT"};
    Session b_session;
    b_session.set_durability(wal::DurabilityClass::kGroup);
    // `AND v = 1` is the visibility half of the claim: the row qualifies
    // only under a view that covers A's commit, so `UPDATED 1` says B read
    // the committed-but-not-yet-durable version, and `UPDATED 0` would say
    // the window waited for the sync.
    Script b_write;
    b_write.session = &b_session;
    b_write.lines = {"BEGIN", "UPDATE t SET v = 2 WHERE id = 1 AND v = 1"};
    Script b_commit;
    b_commit.session = &b_session;
    b_commit.lines = {"COMMIT"};
    rig->core(a_core).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(da, a)));
    rig->core(b_core).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(db, b_write)));
    rig->core(b_core).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(db, b_commit)));
    struct StopAtExit {
        TwoCoreRig& rig;
        ~StopAtExit() {
            rig.log_gate().Release();
            rig.Stop();
        }
    } stop_at_exit{*rig};

    // The published maximum before A commits: A's publish raises it.
    const std::uint64_t ceiling_before = rig->visibility().CommitCeiling();
    rig->log_gate().Hold();
    a.go.store(true, std::memory_order_release);
    rig->Start();

    // A's BEGIN and UPDATE are answered, its commit is published - the
    // ceiling moved - and its sync is held at the gate: the writer is
    // parked inside `Sync`, asked for by A's core's post-task drain once the
    // COMMIT parked (`core_runtime.cpp:417-422`, `wal/manager.cpp:509-525`).
    ASSERT_TRUE(Within(2000ms, [&] {
        return a.done.load(std::memory_order_acquire) == 2 && rig->log_gate().parked() >= 1 &&
               rig->visibility().CommitCeiling() > ceiling_before;
    })) << "A never reached its parked COMMIT: done=" << a.done.load() << " parked="
        << rig->log_gate().parked() << " " << Response(a, 0) << " / " << Response(a, 1);
    EXPECT_TRUE(StartsWith(Response(a, 0), "BEGIN")) << Response(a, 0);
    EXPECT_TRUE(StartsWith(Response(a, 1), "UPDATED 1")) << Response(a, 1);
    EXPECT_EQ(a.done.load(std::memory_order_acquire), 2u) << "A's COMMIT answered before its sync landed";

    // **The claim, read off the table.** A's `IX` on `t` and `X` on row 1
    // were the only tenancies - no statement runs anywhere - and an entry
    // exists only while somebody holds or waits (`lock_table.hpp:747-751`).
    // The release follows the publish on A's thread with no suspension
    // between (`txn/manager.cpp:306-336`), so the bound is a formality.
    EXPECT_TRUE(Within(2000ms, [&] { return rig->locks().EntryCount() == 0; }))
        << "A's borrows outlived its publish while its sync was held: "
        << rig->locks().EntryCount() << " live entries";
    EXPECT_GE(rig->log_gate().parked(), 1) << "the sync landed under the hold";
    EXPECT_EQ(a.done.load(std::memory_order_acquire), 2u);

    // B writes the row A just committed, on the other core, while A is
    // parked. Its reactor may be in its idle block, so the barrier kicks it.
    b_write.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, b_core, [&] { return b_write.done.load(std::memory_order_acquire) == 2; }))
        << "B's UPDATE did not answer while A's sync was held: " << Response(b_write, 0) << " / "
        << Response(b_write, 1);
    EXPECT_TRUE(StartsWith(Response(b_write, 0), "BEGIN")) << Response(b_write, 0);
    EXPECT_TRUE(StartsWith(Response(b_write, 1), "UPDATED 1"))
        << "B did not take the row A released, or read a view below A's commit: "
        << Response(b_write, 1);
    // And A is still where it was: parked, unanswered, its sync held.
    EXPECT_EQ(a.done.load(std::memory_order_acquire), 2u) << "A's COMMIT answered while its sync was held";
    EXPECT_GE(rig->log_gate().parked(), 1) << "the sync landed under the hold";

    // The sync lands, the writer kicks A's core, A's COMMIT is acknowledged.
    rig->log_gate().Release();
    ASSERT_TRUE(Within(2000ms, [&] { return a.done.load(std::memory_order_acquire) == 3; }))
        << "A's COMMIT was not answered after its sync";
    EXPECT_TRUE(StartsWith(Response(a, 2), "COMMIT")) << Response(a, 2);

    // B commits after it, under `group` too, and is kicked by the writer.
    b_commit.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, b_core, [&] { return b_commit.done.load(std::memory_order_acquire) == 1; }))
        << "B's COMMIT was not answered";
    EXPECT_TRUE(StartsWith(Response(b_commit, 0), "COMMIT")) << Response(b_commit, 0);

    // On this thread alone once both reactors are joined (the rig's rule):
    // B's version stands over A's, and nothing is left in the table.
    rig->Stop();
    EXPECT_EQ(da.Dispatch("SELECT v FROM t WHERE id = 1").response, "v\\n2");
    EXPECT_EQ(rig->locks().EntryCount(), 0u) << "a borrow outlived both transactions";
}

TEST(LockReleaseBeforeSyncRigTest, AGroupCommittersRowIsWritableFromThePeerBeforeItsSyncLands) {
    // The committer on core 0, the second writer on core 1.
    TheRowIsWritableBeforeTheCommittersSyncLands(/*a_core=*/0);
}

TEST(LockReleaseBeforeSyncRigTest, APeersGroupCommittedRowIsWritableFromCore0BeforeItsSyncLands) {
    // The committer on core 1, the second writer on core 0.
    TheRowIsWritableBeforeTheCommittersSyncLands(/*a_core=*/1);
}

}  // namespace
}  // namespace kds::server
