// **A statement refused a buffer-pool frame runs again** (DX-S2,
// `instructions/v3.2.0/workorder-dx-pool-under-pressure.md`).
//
// The whole engine on the two-core rig, a pool far smaller than the
// relation, and no writeback tick (the rig runs none, `Expeditor` does): a
// dirty frame a refused reservation queued is written back only by the
// re-run's drain, so what these cells see is that drain's and nothing
// else's.
//
// - Point updates fill the pool with dirty frames. Each one refused has
//   written nothing, so `DispatchAsync` drains holding nothing and runs it
//   again: no client is refused, no transaction is poisoned, and the pool
//   did refuse. In transactions of 30, since the rig's commit waits on its
//   writer (about 50 ms a commit here); the statement, not the
//   transaction, is what must have written nothing.
// - DX-R1: an explicit transaction's `UPDATE` that writes rows before it is
//   refused keeps the refusal - no drain is taken for it and nothing is run
//   twice - and so does a `DELETE` refused after its assertion reservation.
//
// The reactors run under the suspend audit and strict wait scopes
// (`CoreRuntime::Run`), so a drain that held a pin, or one outside a wait
// scope, would abort the cell (DX-R3).

#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "frame_budget_override.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/manager.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

constexpr int kRows = 1200;           // ~300 leaves of ~2 KB rows
constexpr std::size_t kFrames = 320;  // fewer frames than the leaves

struct Script {
    Session* session = nullptr;
    std::vector<std::string> lines;
    std::vector<DispatchOutcome> outs;
    // The pool's counters as each line answered, read on the reactor.
    std::vector<storage::PageStore::PoolCounters> counters;
    std::atomic<std::size_t> done{0};
};

sched::Coro RunScript(CommandDispatcher& d, storage::DevicePageStore& store, Script& s) {
    s.outs.resize(s.lines.size());
    s.counters.resize(s.lines.size());
    for (std::size_t i = 0; i < s.lines.size(); ++i) {
        co_await d.DispatchAsync(s.lines[i], s.session, &s.outs[i]);
        s.counters[i] = store.pool_counters();
        s.done.store(i + 1, std::memory_order_release);
    }
    co_return Status::OK();
}

class DxPoolRerunRigTest : public ::testing::Test {
protected:
    const WithoutFrameBudgetOverride exact_capacity_;  // the capacity is the subject

    void SetUp() override {
        TwoCoreRig::Options options;
        options.buffer_pool_frames = kFrames;
        auto opened = TwoCoreRig::Open(options);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        rig_ = std::move(opened.value());
        CommandDispatcher& d = rig_->core(0).dispatcher();
        ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, grp int64, name varchar(2000))")
                      .response.rfind("CREATED", 0),
                  0u);
        const std::string wide(1900, 'x');
        for (int at = 1; at <= kRows;) {
            std::string sql = "INSERT INTO t VALUES ";
            for (int n = 0; n < 40 && at <= kRows; ++n, ++at) {
                if (n != 0) sql += ", ";
                sql += "(" + std::to_string(at) + ", 0, '" + wide + "')";
            }
            const std::string reply = d.Dispatch(sql).response;
            ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
            // Written back as the load goes: a pool this small cannot hold
            // the relation dirty.
            ASSERT_TRUE(rig_->wal().DrainOnce().ok());
            ASSERT_TRUE(rig_->store().Sync().ok());
        }
    }

    void TearDown() override {
        if (rig_ != nullptr) rig_->Stop();
    }

    // Runs `script` on core 0 to its end.
    void RunToEnd(Script& script) {
        rig_->core(0).scheduler().Submit(
            sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                                RunScript(rig_->core(0).dispatcher(), rig_->store(), script)));
        rig_->Start();
        ASSERT_TRUE(Within(120000ms, [&] { return script.done.load() == script.lines.size(); }))
            << "the script stopped at " << script.done.load() << " of " << script.lines.size();
    }

    // `SUM(grp)`, read once the reactors have stopped; -1 if the reply has
    // no value line.
    long long Sum() {
        const std::string reply =
            rig_->core(0).dispatcher().Dispatch("SELECT SUM(grp) FROM t").response;
        const auto newline = reply.find("\\n");
        return newline == std::string::npos ? -1 : std::stoll(reply.substr(newline + 2));
    }

    std::unique_ptr<TwoCoreRig> rig_;
};

TEST_F(DxPoolRerunRigTest, PointUpdatesIntoAPoolOfDirtyFramesAreNeverRefused) {
    // Killed by `EndWrite` never marking a pool re-run due: the pool's
    // refusal reaches the client and poisons its transaction.
    constexpr int kPerTransaction = 30;
    Session session;
    Script script{&session};
    for (int k = 1; k <= kRows; ++k) {
        if (k % kPerTransaction == 1) script.lines.push_back("BEGIN");
        script.lines.push_back("UPDATE t SET grp = grp + 1 WHERE id = " + std::to_string(k));
        if (k % kPerTransaction == 0 || k == kRows) script.lines.push_back("COMMIT");
    }
    const auto before = rig_->store().pool_counters();
    ASSERT_NO_FATAL_FAILURE(RunToEnd(script));
    rig_->Stop();
    const auto after = rig_->store().pool_counters();

    std::size_t refused = 0;
    for (std::size_t i = 0; i < script.outs.size(); ++i) {
        const std::string& reply = script.outs[i].response;
        if (reply.rfind("ERR", 0) == 0 && ++refused <= 3) {
            ADD_FAILURE() << script.lines[i] << " -> " << reply;
        }
    }
    EXPECT_EQ(refused, 0u) << "statements refused";
    EXPECT_GT(after.refused, before.refused)
        << "the pool never refused: the cell does not reach the re-run";
    EXPECT_GT(after.dirty_drained, before.dirty_drained);
    EXPECT_EQ(Sum(), kRows) << "an update was lost or applied twice";
}

TEST_F(DxPoolRerunRigTest, AnUpdateRefusedAfterItWroteRowsKeepsTheRefusal) {
    // DX-R1. One `UPDATE` of every row inside `BEGIN` fills the pool with
    // its own dirty leaves and is refused part-way, having written rows: it
    // is answered with the refusal, poisons its transaction, and is not run
    // again - no drain is taken between its start and its answer - and the
    // `ROLLBACK` then undoes every row. Killed by dropping `EndWrite`'s
    // trail test: the statement is drained for and re-run, writing its rows
    // again, until its deadline answers the refusal.
    Session session;
    Script script{&session};
    script.lines = {"BEGIN", "UPDATE t SET grp = grp + 1", "SELECT COUNT(*) FROM t",
                    "ROLLBACK"};
    ASSERT_NO_FATAL_FAILURE(RunToEnd(script));
    rig_->Stop();
    const std::string& refused = script.outs[1].response;
    EXPECT_EQ(refused.rfind("ERR", 0), 0u) << "the update was not refused: " << refused;
    EXPECT_NE(refused.find("buffer pool full"), std::string::npos) << refused;
    EXPECT_EQ(script.counters[1].dirty_drained, script.counters[0].dirty_drained)
        << "a drain was taken for a statement that had written rows";
    EXPECT_EQ(script.outs[2].response.rfind("ERR", 0), 0u)
        << "the refused update did not poison its transaction: " << script.outs[2].response;
    EXPECT_EQ(script.outs[3].response.rfind("ERR", 0), std::string::npos)
        << script.outs[3].response;
    EXPECT_EQ(Sum(), 0) << "the rollback left a row updated";
}

TEST_F(DxPoolRerunRigTest, ADeleteRefusedAfterItsAssertionReservationKeepsTheRefusal) {
    // `[quiet-wrong]`: a `DELETE` reserves its row's assertion departure
    // outside any no-refuse window and before its trail entry. Refused at
    // its undo append, it leaves a reservation the trail does not show, so
    // an explicit transaction holding one keeps the refusal - a re-run
    // would reserve the departure again and the `COMMIT` keep both. The
    // seam refuses the second `DELETE`'s undo append once, as the pool
    // would, with dirty frames queued for a drain to write. Killed by
    // dropping `EndWrite`'s pending-reservation test: the re-run answers.
    CommandDispatcher& d = rig_->core(0).dispatcher();
    ASSERT_NE(d.Dispatch("CREATE ASSERTION grp_cap ON t GROUP BY (grp) CHECK COUNT(*) <= " +
                         std::to_string(kRows))
                  .response.find("CREATED"),
              std::string::npos);
    std::atomic<int> appends{0};
    rig_->core(0).transactions().SetBeforeUndoAppendForTest(
        [this, &appends](const txn::UndoRecordFields&) -> Status {
            if (appends.fetch_add(1) != 1) return Status::OK();
            // Every unpinned frame to usage zero, so the dirty ones queue.
            (void)rig_->store().EvictColdFrames(kFrames * 8);
            storage::NotePoolRefusal();
            return Status::ResourceExhausted("buffer pool full: refused by the cell's seam");
        });
    Session session;
    Script script{&session};
    script.lines = {"BEGIN", "DELETE FROM t WHERE id = 1", "DELETE FROM t WHERE id = 2",
                    "ROLLBACK"};
    ASSERT_NO_FATAL_FAILURE(RunToEnd(script));
    rig_->Stop();
    rig_->core(0).transactions().SetBeforeUndoAppendForTest(nullptr);
    EXPECT_EQ(script.outs[1].response.rfind("ERR", 0), std::string::npos)
        << script.outs[1].response;
    const std::string& refused = script.outs[2].response;
    EXPECT_EQ(refused.rfind("ERR", 0), 0u) << "the refused delete was run again: " << refused;
    EXPECT_GE(appends.load(), 2) << "the seam never refused";
    EXPECT_EQ(script.outs[3].response.rfind("ERR", 0), std::string::npos)
        << script.outs[3].response;
}

TEST_F(DxPoolRerunRigTest, AStatementFailingForAnotherReasonAfterARecoveredRefusalIsNotRunAgain) {
    // The note outlives a refusal its caller recovered from, so the re-run
    // asks the outcome's code too: a statement that then fails for another
    // reason is answered with that failure. The seam notes a refusal and
    // fails the `DELETE`'s append `InvalidArgument`, once, with the
    // `UPDATE`'s dirty leaf queued for a drain to write. Killed by dropping
    // `PoolRerunDue`'s code test: the re-run answers.
    std::atomic<int> appends{0};
    rig_->core(0).transactions().SetBeforeUndoAppendForTest(
        [this, &appends](const txn::UndoRecordFields&) -> Status {
            if (appends.fetch_add(1) != 1) return Status::OK();
            (void)rig_->store().EvictColdFrames(kFrames * 8);
            storage::NotePoolRefusal();
            return Status::InvalidArgument("refused by the cell's seam for another reason");
        });
    Session session;
    Script script{&session};
    script.lines = {"UPDATE t SET grp = 1 WHERE id = 600", "DELETE FROM t WHERE id = 1"};
    ASSERT_NO_FATAL_FAILURE(RunToEnd(script));
    rig_->Stop();
    rig_->core(0).transactions().SetBeforeUndoAppendForTest(nullptr);
    EXPECT_EQ(script.outs[0].response.rfind("ERR", 0), std::string::npos)
        << script.outs[0].response;
    const std::string& reply = script.outs[1].response;
    EXPECT_EQ(reply.rfind("ERR", 0), 0u) << "the failed delete was run again: " << reply;
    EXPECT_NE(reply.find("another reason"), std::string::npos) << reply;
}

}  // namespace
}  // namespace kds::server
