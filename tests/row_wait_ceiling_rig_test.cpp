// **A row wait whose writer has committed above the snapshot ceiling waits
// for the ceiling, kicked at the marker's lift** (`foreign-keys.md` §3a,
// `index.md` §10.2), on the two-core rig.
//
// A commit holds its snapshot marker from before its append until after its
// publish, and under `strict` that span includes the sync. A `relaxed`
// commit made inside it is published and released, yet every check view
// minted until the marker lifts is capped below it, so it still reads that
// writer's row as busy. `WaitForRowWriter`'s `U` probe on the row is then
// granted - the writer has released - and before this cell the grant arm
// handed back a slot already flipped: the statement re-ran at once, met the
// same busy row, and spun without yielding until the ceiling lifted or the
// 1 s net refused it - and with the marker on the waiter's own core, the
// marker's publish could not run while it spun. Now the grant arm asks
// whether a fresh view would see the holder's commit, and where it would
// not, the statement waits for the ceiling to cover it
// (`TransactionManager::CeilingWait`), counted into its core's ceiling
// waiters so that the lift kicks it.
//
// **Mutation**: the grant arm pre-flipping its slot whatever the ceiling -
// every cell sees the statement answer without counting itself a ceiling
// waiter.

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
#include "kds/txn/lock_table.hpp"
#include "kds/wal/durability.hpp"

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
    std::atomic<bool> go{false};
    std::atomic<bool> done{false};
};

sched::Coro RunStatement(CommandDispatcher& d, Statement& s) {
    s.go_pred = [&s] { return s.go.load(std::memory_order_acquire); };
    {
        // A test driver standing in for a client: the reactor runs strict (CJ WE04, I2).
        sched::WaitScope rig_idle(stats::WaitEvent::kIdleClient);
        co_await sched::WaitUntil{&s.go_pred};
    }
    co_await d.DispatchAsync(s.line, s.session, &s.out);
    s.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

std::string Response(const Statement& s) {
    return s.done.load(std::memory_order_acquire) ? s.out.response : "(not done)";
}

struct Shape {
    std::vector<std::string> schema;       // on core 0, before anything
    std::string holder_write;              // core 1, inside BEGIN, before the reactors start
    std::string waiter_write;              // core 1, autocommit, after the holder's COMMIT
    std::uint32_t marker_core = 0;         // where the `strict` commit holds its marker
    const char* expected = "";             // words the waiter's answer carries after the lift
};

// **The shape every cell runs.** A `strict` commit on `marker_core` is held
// inside its sync. The holder on core 1 commits `relaxed` under it and is
// acknowledged above the ceiling. The waiter on core 1 then meets the
// holder's row busy, must wait counted into core 1's ceiling waiters, and
// answers `expected` once the sync lands and the marker lifts.
void WaitsForTheCeiling(const Shape& shape) {
    // Before the rig, so a failed assertion's early return stops the
    // reactors before the frames they borrow go.
    Session strict_session;
    Session holder;
    Session waiter;
    Statement strict;
    Statement commit;
    Statement wait;
    TwoCoreRig::Options options;
    options.gated_log_sync = true;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();

    ASSERT_TRUE(StartsWith(d0.Dispatch("CREATE TABLE t (id int64, v int64)").response, "CREATED"));
    for (const std::string& sql : shape.schema) {
        const std::string out = d0.Dispatch(sql).response;
        ASSERT_FALSE(StartsWith(out, "ERR")) << sql << " -> " << out;
    }
    // Both cores carve their transaction-id windows now, while syncs land:
    // a carve persists page 0, whose writeback waits on the log, and a held
    // sync would stall that core's write on it instead of on the marker.
    ASSERT_FALSE(StartsWith(d0.Dispatch("INSERT INTO t VALUES (100, 0)").response, "ERR"));
    ASSERT_FALSE(StartsWith(d1.Dispatch("INSERT INTO t VALUES (200, 0)").response, "ERR"));
    holder.set_durability(wal::DurabilityClass::kRelaxed);
    ASSERT_TRUE(StartsWith(d1.Dispatch("BEGIN", &holder).response, "BEGIN"));
    const std::string held = d1.Dispatch(shape.holder_write, &holder).response;
    ASSERT_FALSE(StartsWith(held, "ERR")) << held;

    strict_session.set_durability(wal::DurabilityClass::kStrict);
    strict.session = &strict_session;
    strict.line = "INSERT INTO t VALUES (" + std::to_string(101 + shape.marker_core * 100) + ", 1)";
    commit.session = &holder;
    commit.line = "COMMIT";
    waiter.set_durability(wal::DurabilityClass::kRelaxed);
    wait.session = &waiter;
    wait.line = shape.waiter_write;
    CommandDispatcher& dm = rig->core(shape.marker_core).dispatcher();
    rig->core(shape.marker_core)
        .scheduler()
        .Submit(sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunStatement(dm, strict)));
    rig->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunStatement(d1, commit)));
    rig->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunStatement(d1, wait)));
    // A failed assertion below must neither leave a core inside its sync,
    // whose reactor the teardown would then never join, nor leave both
    // reactors running over the statements this scope destroys before the
    // rig.
    struct StopAtExit {
        TwoCoreRig& rig;
        ~StopAtExit() {
            rig.log_gate().Release();
            rig.Stop();
        }
    } stop_at_exit{*rig};
    rig->log_gate().Hold();
    rig->Start();

    // The `strict` commit is inside its sync, its marker set.
    const txn::CoreVisibilitySlot& marker_slot = rig->visibility().slot(shape.marker_core);
    strict.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, shape.marker_core, [&] {
        return rig->log_gate().parked() >= 1 &&
               marker_slot.pending_commit_bound.load() != txn::kUnboundedBound;
    })) << "the strict commit never reached its sync";

    // The holder commits `relaxed` under the marker: published and
    // released, and above every view a mint could take now.
    commit.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return commit.done.load(); }))
        << "the holder's COMMIT never finished";
    ASSERT_TRUE(StartsWith(Response(commit), "COMMIT")) << Response(commit);
    ASSERT_LT(rig->visibility().SnapshotCeiling(), holder.acknowledged_commit_lsn())
        << "the holder's commit is not above the ceiling; the cell's premise is gone";

    // The waiter meets the holder's row busy under a capped view, and waits
    // for the ceiling rather than answering.
    const txn::CoreVisibilitySlot& waiter_slot = rig->visibility().slot(1);
    wait.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 1, [&] {
        return wait.done.load() || waiter_slot.ceiling_waiters.load() >= 1;
    })) << "core 1's statement neither answered nor waited";
    ASSERT_FALSE(wait.done.load())
        << "core 1's statement did not wait for the ceiling; it answered " << Response(wait);

    // The sync lands, the marker lifts, and the lift kicks core 1.
    rig->log_gate().Release();
    ASSERT_TRUE(Within(1000ms, [&] { return wait.done.load(); }))
        << "core 1's statement did not proceed at the marker's lift";
    EXPECT_NE(Response(wait).find(shape.expected), std::string::npos) << Response(wait);
    ASSERT_TRUE(Within(2000ms, [&] { return strict.done.load(); }));
    EXPECT_TRUE(StartsWith(Response(strict), "INSERTED")) << Response(strict);
    // No waiter outlives its statement, and no wake registration either.
    EXPECT_EQ(waiter_slot.ceiling_waiters.load(), 0u);
    rig->Stop();
    EXPECT_EQ(rig->locks().EntryCount(), 0u) << "a wake registration outlived its statement";
}

const std::vector<std::string> kParentChild = {
    "CREATE TABLE p (id int64, v int64) BTREE",
    "CREATE TABLE c (id int64, pid int64 REFERENCES p, note int64) BTREE",
    "INSERT INTO p VALUES (7, 0)",
    "INSERT INTO p VALUES (8, 0)",
    "INSERT INTO c VALUES (1, 7, 0)",
};

TEST(RowWaitCeilingRigTest, AParentDeleteMeetingAChildCommittedAboveTheCeilingWaitsForTheLift) {
    // The update of another column leaves the child referencing 7.
    WaitsForTheCeiling({kParentChild, "UPDATE c SET note = 1 WHERE id = 1",
                        "DELETE FROM p WHERE id = 7", 0, "FK_VIOLATION"});
}

TEST(RowWaitCeilingRigTest, AParentDeleteWaitsForTheLiftOfAMarkerOnItsOwnCore) {
    // The marker's publish runs on the waiter's reactor: a statement that
    // spun there instead of parking would hold the lift off.
    WaitsForTheCeiling({kParentChild, "UPDATE c SET note = 1 WHERE id = 1",
                        "DELETE FROM p WHERE id = 7", 1, "FK_VIOLATION"});
}

TEST(RowWaitCeilingRigTest, AParentDeleteMeetingAChildMovedOffAboveTheCeilingPassesAtTheLift) {
    // The capped view reads the version before the move through undo; the
    // lift shows the move, and the parent goes.
    WaitsForTheCeiling({kParentChild, "UPDATE c SET pid = 8 WHERE id = 1",
                        "DELETE FROM p WHERE id = 7", 0, "DELETED 1"});
}

TEST(RowWaitCeilingRigTest, ABusyUniqueKeyWhoseWriterCommittedAboveTheCeilingWaitsForTheLift) {
    // Before this cell the unique check refused the capped writer retryably
    // at once, as it had before its probe existed.
    WaitsForTheCeiling({{"CREATE TABLE u (id int64, owner int64) BTREE",
                         "CREATE UNIQUE INDEX by_owner ON u (owner)"},
                        "INSERT INTO u VALUES (1, 7)",
                        "INSERT INTO u VALUES (9, 7)",
                        0,
                        "is already held by tuple key"});
}

}  // namespace
}  // namespace kds::server
