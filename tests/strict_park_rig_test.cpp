// **A `strict` commit parks before its publish** (BA-S7 part 2, BA-Q3 (b),
// `instructions/v3.0.0/workorder-ba-parallelism.md` BA-R4), on the
// two-core rig.
//
// Before BA-S7, a `strict` commit synced inside its statement: core 0 on
// its reactor, a peer blocked on the writer's condition variable. Either
// way every other session on that core waited for the sync, and BA-S4's
// census found `strict` flat at about 700 statements/s whatever the session
// count. Now the commit stages its record for the writer, holds its marker
// and parks; the publish runs once the record is durable. So the core
// serves its other sessions meanwhile, and the commit stays invisible until
// it is durable - D1's meaning.

#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
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

// A session's statements, in order, once `go` is set; the last through the
// synchronous `Dispatch` when `last_sync` is.
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

// This core's checkpoint snapshot of its active transactions, taken on its
// own thread (`live_` is core-local).
sched::Coro TakeActiveSnapshot(txn::TransactionManager& txns,
                               std::vector<wal::CheckpointActiveTxn>* out,
                               std::atomic<bool>* done) {
    *out = txns.Snapshot();
    done->store(true, std::memory_order_release);
    co_return Status::OK();
}

std::string Response(const Script& s, std::size_t i) {
    if (s.done.load(std::memory_order_acquire) <= i) return "(not done)";
    return s.outs[i].response;
}

struct Fixture {
    std::unique_ptr<TwoCoreRig> rig;
    void Open() {
        TwoCoreRig::Options options;
        options.gated_log_sync = true;
        auto opened = TwoCoreRig::Open(options);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        rig = std::move(opened.value());
        CommandDispatcher& d1 = rig->core(1).dispatcher();
        ASSERT_TRUE(StartsWith(d1.Dispatch("CREATE TABLE t (id int64, v int64)").response, "CREATED"));
        // Each core carves its transaction-id window now, while syncs land:
        // the carve persists page 0, whose writeback waits on the log.
        ASSERT_FALSE(StartsWith(d1.Dispatch("INSERT INTO t VALUES (1, 1)").response, "ERR"));
        ASSERT_FALSE(
            StartsWith(rig->core(0).dispatcher().Dispatch("INSERT INTO t VALUES (2, 2)").response, "ERR"));
    }
    void Submit(std::uint32_t core, Script& s) {
        rig->core(core).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, RunScript(rig->core(core).dispatcher(), s)));
    }
};

TEST(StrictParkRigTest, ACoreServesItsOtherSessionsWhileAStrictCommitAwaitsItsSync) {
    Fixture f;
    f.Open();
    if (HasFatalFailure()) return;
    Session strict_session;
    strict_session.set_durability(wal::DurabilityClass::kStrict);
    Script strict{&strict_session, {"INSERT INTO t VALUES (10, 10)"}};
    Session other_session;
    other_session.set_durability(wal::DurabilityClass::kRelaxed);
    Script other{&other_session, {"SELECT id FROM t WHERE id = 1"}};
    Session observer_session;
    Script observer{&observer_session, {"SELECT id FROM t WHERE id = 10"}};
    f.Submit(1, strict);
    f.Submit(1, other);
    f.Submit(0, observer);
    struct StopAtExit {
        TwoCoreRig& rig;
        ~StopAtExit() {
            rig.log_gate().Release();
            rig.Stop();
        }
    } stop_at_exit{*f.rig};
    f.rig->log_gate().Hold();
    strict.go.store(true, std::memory_order_release);
    f.rig->Start();

    // The strict commit's sync is held at the gate, its marker set.
    ASSERT_TRUE(KickUntil(*f.rig, 1, [&] {
        return f.rig->log_gate().parked() >= 1 &&
               f.rig->visibility().slot(1).pending_commit_bound.load() != txn::kUnboundedBound;
    })) << "core 1's strict commit never reached its sync";
    EXPECT_EQ(strict.done.load(), 0u) << "the strict commit answered before its sync landed";

    // A checkpoint taken now must not list the parked commit as active: its
    // TXN_COMMIT is below the BEGIN, so a scan starting above that record
    // would undo a committed transaction.
    std::vector<wal::CheckpointActiveTxn> active{{1, 1}};
    std::atomic<bool> snapped{false};
    f.rig->core(1).scheduler().Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        TakeActiveSnapshot(f.rig->core(1).transactions(), &active, &snapped)));
    ASSERT_TRUE(KickUntil(*f.rig, 1, [&] { return snapped.load(std::memory_order_acquire); }));
    EXPECT_TRUE(active.empty()) << "a checkpoint listed a commit whose record is appended";

    // Core 1 is not blocked by it: another session there runs to its end.
    // Red before BA-S7 part 2: the peer's reactor sat in the writer's wait.
    other.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*f.rig, 1, [&] { return other.done.load() == 1; }))
        << "core 1 served nothing while its strict commit awaited its sync";
    EXPECT_EQ(Response(other, 0), "id\\n1");

    // And the commit is not visible yet, on any core: D1 publishes once
    // durable.
    observer.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*f.rig, 0, [&] { return observer.done.load() == 1; }));
    EXPECT_EQ(Response(observer, 0), "id") << "a strict commit was visible before it was durable";
    EXPECT_EQ(strict.done.load(), 0u);

    // The sync lands, the writer kicks core 1, the commit publishes.
    f.rig->log_gate().Release();
    ASSERT_TRUE(Within(2000ms, [&] { return strict.done.load() == 1; }))
        << "the strict commit was not answered after its sync";
    EXPECT_TRUE(StartsWith(Response(strict, 0), "INSERTED")) << Response(strict, 0);
    EXPECT_EQ(f.rig->visibility().slot(1).pending_commit_bound.load(), txn::kUnboundedBound)
        << "the marker outlived its commit";
}

TEST(StrictParkRigTest, AnExplicitStrictCommitParksAndPublishesAtItsSync) {
    Fixture f;
    f.Open();
    if (HasFatalFailure()) return;
    Session strict_session;
    strict_session.set_durability(wal::DurabilityClass::kStrict);
    Script strict{&strict_session, {"BEGIN", "INSERT INTO t VALUES (11, 11)", "COMMIT"}};
    f.Submit(1, strict);
    struct StopAtExit {
        TwoCoreRig& rig;
        ~StopAtExit() {
            rig.log_gate().Release();
            rig.Stop();
        }
    } stop_at_exit{*f.rig};
    f.rig->log_gate().Hold();
    strict.go.store(true, std::memory_order_release);
    f.rig->Start();
    ASSERT_TRUE(KickUntil(*f.rig, 1, [&] {
        return strict.done.load() == 2 && f.rig->log_gate().parked() >= 1;
    })) << "COMMIT never reached its sync";
    EXPECT_EQ(strict.done.load(), 2u) << "COMMIT answered before its sync landed";
    f.rig->log_gate().Release();
    ASSERT_TRUE(Within(2000ms, [&] { return strict.done.load() == 3; }));
    EXPECT_TRUE(StartsWith(Response(strict, 2), "COMMIT")) << Response(strict, 2);
    EXPECT_EQ(f.rig->core(0).dispatcher().Dispatch("SELECT id FROM t WHERE id = 11").response,
              "id\\n11");
}

TEST(StrictParkRigTest, ASynchronousWaitOnItsOwnCoresParkedMarkerFinishesTheCommit) {
    // `Dispatch` cannot park. Its BA-R1c wait for the session's last commit
    // can be capped by a `strict` commit parked on the *same* core, whose
    // statement cannot run until `Dispatch` returns: the wait makes that
    // record durable and publishes it itself (BA-S1c's review, C2).
    Fixture f;
    f.Open();
    if (HasFatalFailure()) return;
    Session strict_session;
    strict_session.set_durability(wal::DurabilityClass::kStrict);
    Script strict{&strict_session, {"INSERT INTO t VALUES (12, 12)"}};
    Session relaxed_session;
    relaxed_session.set_durability(wal::DurabilityClass::kRelaxed);
    // A relaxed commit after the strict one, acknowledged, then a
    // synchronous statement whose snapshot must cover it.
    Script relaxed{&relaxed_session, {"INSERT INTO t VALUES (13, 13)", "SELECT id FROM t WHERE id = 13"},
                   /*last_sync=*/true};
    f.Submit(1, strict);
    f.Submit(1, relaxed);
    struct StopAtExit {
        TwoCoreRig& rig;
        ~StopAtExit() {
            rig.log_gate().Release();
            rig.Stop();
        }
    } stop_at_exit{*f.rig};
    f.rig->log_gate().Hold();
    strict.go.store(true, std::memory_order_release);
    f.rig->Start();
    ASSERT_TRUE(KickUntil(*f.rig, 1, [&] {
        return f.rig->log_gate().parked() >= 1 &&
               f.rig->visibility().slot(1).pending_commit_bound.load() != txn::kUnboundedBound;
    }));
    relaxed.go.store(true, std::memory_order_release);
    f.rig->wakers().Kick(1);
    // The synchronous SELECT is now waiting on core 1's own marker, inside
    // `EnsureDurable`, behind the held sync.
    std::this_thread::sleep_for(200ms);
    EXPECT_LT(relaxed.done.load(), 2u) << "the SELECT read under a marker its snapshot must cover";
    f.rig->log_gate().Release();
    ASSERT_TRUE(Within(2000ms, [&] { return relaxed.done.load() == 2; }))
        << "the synchronous wait never finished its own core's parked commit";
    EXPECT_EQ(Response(relaxed, 1), "id\\n13");
    ASSERT_TRUE(Within(2000ms, [&] { return strict.done.load() == 1; }));
    EXPECT_TRUE(StartsWith(Response(strict, 0), "INSERTED")) << Response(strict, 0);
}

TEST(StrictParkRigTest, AParkedStrictCommitWhoseSyncFailsAbortsAndIsNeverVisible) {
    // D1 publishes only once durable, and a failed sync aborts on both
    // paths (txn.md §6): a log that stops under a parked `strict` commit
    // leaves it refused and invisible - not published beside its refusal -
    // and its session's next statement does not wait for a bound no
    // ceiling will reach.
    Fixture f;
    f.Open();
    if (HasFatalFailure()) return;
    Session strict_session;
    strict_session.set_durability(wal::DurabilityClass::kStrict);
    Script strict{&strict_session,
                  {"INSERT INTO t VALUES (14, 14)", "SELECT id FROM t WHERE id = 1"}};
    Session observer_session;
    Script observer{&observer_session, {"SELECT id FROM t WHERE id = 14"}};
    f.Submit(1, strict);
    f.Submit(0, observer);
    struct StopAtExit {
        TwoCoreRig& rig;
        ~StopAtExit() {
            rig.log_gate().Release();
            rig.Stop();
        }
    } stop_at_exit{*f.rig};
    f.rig->log_gate().Hold();
    strict.go.store(true, std::memory_order_release);
    f.rig->Start();
    ASSERT_TRUE(KickUntil(*f.rig, 1, [&] {
        return f.rig->log_gate().parked() >= 1 &&
               f.rig->visibility().slot(1).pending_commit_bound.load() != txn::kUnboundedBound;
    }));
    f.rig->log_gate().FailSyncs();
    f.rig->log_gate().Release();
    ASSERT_TRUE(KickUntil(*f.rig, 1, [&] { return strict.done.load() == 2; }))
        << "the refused commit's session never ran its next statement";
    EXPECT_TRUE(StartsWith(Response(strict, 0), "ERR")) << Response(strict, 0);
    EXPECT_EQ(Response(strict, 1), "id\\n1");
    EXPECT_EQ(f.rig->visibility().slot(1).pending_commit_bound.load(), txn::kUnboundedBound)
        << "the aborted commit's marker outlived it";
    observer.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*f.rig, 0, [&] { return observer.done.load() == 1; }));
    EXPECT_EQ(Response(observer, 0), "id") << "a strict commit the log never made durable was published";
}

TEST(StrictParkRigTest, StrictCommitsStagedDuringOneSyncShareTheNext) {
    // **`strict` batches now** (BA-R4: "under (b), D1 shares whatever sync
    // concurrent requests ride"). Four sessions on one core commit `strict`
    // while the writer's first sync is held: the first sync covers the
    // first record, and every record staged behind it rides one more. Red
    // before BA-S7 part 2: each commit blocked its reactor for a sync of its
    // own, one after another - four syncs.
    Fixture f;
    f.Open();
    if (HasFatalFailure()) return;
    constexpr int kSessions = 4;
    std::vector<std::unique_ptr<Session>> sessions;
    std::vector<std::unique_ptr<Script>> scripts;
    for (int i = 0; i < kSessions; ++i) {
        sessions.push_back(std::make_unique<Session>());
        sessions.back()->set_durability(wal::DurabilityClass::kStrict);
        scripts.push_back(std::make_unique<Script>());
        scripts.back()->session = sessions.back().get();
        scripts.back()->lines = {"INSERT INTO t VALUES (" + std::to_string(20 + i) + ", 0)"};
        scripts.back()->go.store(true, std::memory_order_release);
        f.Submit(1, *scripts.back());
    }
    struct StopAtExit {
        TwoCoreRig& rig;
        ~StopAtExit() {
            rig.log_gate().Release();
            rig.Stop();
        }
    } stop_at_exit{*f.rig};
    f.rig->log_gate().Hold();
    const Contention::Snapshot before = Contention::Read();
    f.rig->Start();
    ASSERT_TRUE(KickUntil(*f.rig, 1, [&] { return f.rig->log_gate().parked() >= 1; }));
    // Every session reaches its commit while the first sync is held: none
    // is blocked behind it.
    ASSERT_TRUE(KickUntil(*f.rig, 1, [&] {
        return f.rig->core(1).wal().stats().strict_commits >= kSessions;
    })) << "the strict commits did not all stage while one sync was held";
    f.rig->log_gate().Release();
    ASSERT_TRUE(Within(2000ms, [&] {
        for (const auto& s : scripts) {
            if (s->done.load() != 1) return false;
        }
        return true;
    }));
    for (const auto& s : scripts) {
        EXPECT_TRUE(StartsWith(Response(*s, 0), "INSERTED")) << Response(*s, 0);
    }
    const Contention::Snapshot after = Contention::Read();
    EXPECT_LE(after.of(Tally::kSyncsWriter) - before.of(Tally::kSyncsWriter), 2u)
        << "the strict commits did not share a sync";
}

}  // namespace
}  // namespace kds::server
