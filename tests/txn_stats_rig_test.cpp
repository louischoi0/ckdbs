// **How transactions end, how long they live and who holds the read
// horizon** (DF-S2, `instructions/v3.2.0/workorder-df-statistics-coverage.md`
// §2 part B, DF-Q8 (a), DF-Q13 (a)): `sys.meta`'s `txn` fields,
// `sys.txn_histogram` and `sys.session_state`'s two ages, on the two-core rig
// so a count is seen to be its core's and the horizon another core's.
//
// The store is the process's (`stats/txn_stats.hpp`), so every cell reads a
// count as a difference. The rig's clock is the system's: an age is asserted
// against a sleep it must be at least, never against a value.

#include "two_core_rig.hpp"

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/server/session.hpp"
#include "kds/stats/txn_stats.hpp"
#include "view_text.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

bool StartsWith(const std::string& reply, std::string_view prefix) {
    return reply.rfind(prefix, 0) == 0;
}

using Ends = std::array<std::uint64_t, stats::kTxnEndCount>;

std::uint64_t Of(const Ends& ends, stats::TxnEnd end) {
    return ends[static_cast<std::size_t>(end)];
}

// `after - before`, as the six counts in `TxnEnd`'s order.
Ends Moved(const Ends& before, const Ends& after) {
    Ends out{};
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = after[i] - before[i];
    return out;
}

// One end and no other.
Ends Only(stats::TxnEnd end, std::uint64_t n = 1) {
    Ends out{};
    out[static_cast<std::size_t>(end)] = n;
    return out;
}

struct Script {
    Session session;
    std::vector<std::string> lines;
    std::vector<std::string> replies;
    std::atomic<bool> done{false};
};

sched::Coro RunScript(CommandDispatcher& d, Script& s) {
    for (const std::string& line : s.lines) {
        DispatchOutcome out;
        co_await d.DispatchAsync(line, &s.session, &out);
        s.replies.push_back(std::move(out.response));
    }
    s.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

sched::TaskPtr TaskOf(TwoCoreRig& rig, std::uint32_t core, Script& script) {
    auto task = sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                                    RunScript(rig.core(core).dispatcher(), script));
    task->wait_state_mut().session = script.session.WaitStats();
    return task;
}

class TxnStatsRig : public ::testing::Test {
protected:
    void SetUp() override {
        auto opened = TwoCoreRig::Open();
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        rig_ = std::move(opened.value());
        ASSERT_TRUE(StartsWith(D(0).Dispatch("CREATE TABLE t (id int64, v int64) BTREE").response,
                               "CREATED"));
        ASSERT_TRUE(StartsWith(D(0).Dispatch("INSERT INTO t VALUES (1, 1)").response, "INSERTED"));
    }
    void TearDown() override {
        if (rig_ != nullptr) rig_->Stop();
    }

    CommandDispatcher& D(std::uint32_t core) { return rig_->core(core).dispatcher(); }
    std::string Run(std::uint32_t core, Session& s, const std::string& sql) {
        return D(core).Dispatch(sql, &s).response;
    }
    static Ends EndsOf(std::uint32_t core) { return stats::TxnStats::CountsOf(core); }

    // One `txn` field as `core`'s `sys.meta` shows it.
    std::string Meta(std::uint32_t core, std::string_view field) {
        const auto value =
            testing_view::MetaField(D(core).Dispatch("SELECT * FROM sys.meta").response, field);
        EXPECT_TRUE(value.has_value()) << field << " is not a sys.meta field";
        return value.value_or("");
    }

    // `column` of `session`'s row in `sys.session_state`, read on `core`.
    std::string StateOf(std::uint32_t core, const Session& session, std::string_view column) {
        const auto shown = testing_view::ReadViewText(
            D(core)
                .Dispatch("SELECT session_id, txn_age_us, view_age_us FROM sys.session_state")
                .response);
        const auto value = shown.Find(
            "session_id", std::to_string(session.wait_stats()->session_id), column);
        EXPECT_TRUE(value.has_value()) << "the session has no row";
        return value.value_or("");
    }

    // The rows of `sys.txn_histogram` for one core and access mode, summed.
    std::uint64_t Lives(std::uint32_t core, bool read_only) {
        std::uint64_t sum = 0;
        for (const stats::TxnLifeBucket& b : stats::TxnStats::SnapshotLives()) {
            if (b.core == core && b.read_only == read_only) sum += b.count;
        }
        return sum;
    }

    // Declared before the rig and so destroyed after it: a script's session
    // must outlive the task that books to it.
    Script scripts_[3];
    std::unique_ptr<TwoCoreRig> rig_;
};

TEST_F(TxnStatsRig, ACommitAClientRollbackAndAnEngineAbortEachMoveOneCountOfTheirCore) {
    Session s;
    // An autocommit write is a transaction, and its commit is one.
    Ends before = EndsOf(1);
    const Ends other_core = EndsOf(0);
    ASSERT_TRUE(StartsWith(Run(1, s, "INSERT INTO t VALUES (2, 2)"), "INSERTED"));
    EXPECT_EQ(Moved(before, EndsOf(1)), Only(stats::TxnEnd::kCommit));

    // An explicit one: nothing at `BEGIN` or at a statement, one at `COMMIT`.
    before = EndsOf(1);
    ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN"), "BEGIN"));
    ASSERT_EQ(Run(1, s, "UPDATE t SET v = 3 WHERE id = 2"), "UPDATED 1");
    EXPECT_EQ(Moved(before, EndsOf(1)), Ends{});
    ASSERT_TRUE(StartsWith(Run(1, s, "COMMIT"), "COMMIT"));
    EXPECT_EQ(Moved(before, EndsOf(1)), Only(stats::TxnEnd::kCommit));

    // The client's ROLLBACK.
    before = EndsOf(1);
    ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN"), "BEGIN"));
    ASSERT_EQ(Run(1, s, "UPDATE t SET v = 4 WHERE id = 2"), "UPDATED 1");
    ASSERT_TRUE(StartsWith(Run(1, s, "ROLLBACK"), "ROLLBACK"));
    EXPECT_EQ(Moved(before, EndsOf(1)), Only(stats::TxnEnd::kRollback));

    // A failed autocommit statement: the engine rolls its transaction back.
    before = EndsOf(1);
    ASSERT_TRUE(StartsWith(Run(1, s, "INSERT INTO t VALUES (2, 9)"), "ERR"));
    EXPECT_EQ(Moved(before, EndsOf(1)), Only(stats::TxnEnd::kAbort));

    // A failed statement inside an explicit transaction decides nothing: the
    // client's ROLLBACK does, and is the one counted.
    before = EndsOf(1);
    ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN"), "BEGIN"));
    ASSERT_TRUE(StartsWith(Run(1, s, "INSERT INTO t VALUES (2, 9)"), "ERR"));
    EXPECT_EQ(Moved(before, EndsOf(1)), Ends{});
    ASSERT_TRUE(StartsWith(Run(1, s, "ROLLBACK"), "ROLLBACK"));
    EXPECT_EQ(Moved(before, EndsOf(1)), Only(stats::TxnEnd::kRollback));

    // A read is no transaction, and none of it was core 0's.
    before = EndsOf(1);
    ASSERT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 2"), "v\\n3");
    EXPECT_EQ(Moved(before, EndsOf(1)), Ends{});
    EXPECT_EQ(Moved(other_core, EndsOf(0)), Ends{});

    // `sys.meta` shows the core's own six under their names.
    const Ends now = EndsOf(1);
    for (std::size_t i = 0; i < now.size(); ++i) {
        const char* field = stats::TxnEndFieldName(static_cast<stats::TxnEnd>(i));
        EXPECT_EQ(Meta(1, field), std::to_string(now[i])) << field;
    }
    EXPECT_EQ(Meta(0, "commits"), std::to_string(Of(EndsOf(0), stats::TxnEnd::kCommit)));
}

TEST_F(TxnStatsRig, AReadOnlyTransactionDecidesNothingAndIsInNoCount) {
    Session s;
    const Ends before = EndsOf(1);
    ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN READ ONLY"), "BEGIN"));
    ASSERT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 1"), "v\\n1");
    ASSERT_EQ(Run(1, s, "COMMIT"), "COMMIT trx_id=none");
    ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN READ ONLY"), "BEGIN"));
    ASSERT_EQ(Run(1, s, "ROLLBACK"), "ROLLBACK trx_id=none");
    EXPECT_EQ(Moved(before, EndsOf(1)), Ends{});
}

TEST_F(TxnStatsRig, ARollbackTheListenerNamesACauseForIsCountedUnderItOnce) {
    // The listener's sweep and a closed connection dispatch the client's own
    // `ROLLBACK`, each naming its cause on the session first
    // (`tcp_server.cpp`); the `Expeditor` cells drive the listener itself.
    Session s;
    Ends before = EndsOf(0);
    ASSERT_TRUE(StartsWith(Run(0, s, "BEGIN"), "BEGIN"));
    s.set_rollback_cause(stats::TxnEnd::kCeilingRollback);
    ASSERT_TRUE(StartsWith(D(0).DispatchInternal("ROLLBACK", &s).response, "ROLLBACK"));
    EXPECT_EQ(Moved(before, EndsOf(0)), Only(stats::TxnEnd::kCeilingRollback));

    before = EndsOf(0);
    ASSERT_TRUE(StartsWith(Run(0, s, "BEGIN"), "BEGIN"));
    s.set_rollback_cause(stats::TxnEnd::kCloseRollback);
    ASSERT_TRUE(StartsWith(D(0).DispatchInternal("ROLLBACK", &s).response, "ROLLBACK"));
    EXPECT_EQ(Moved(before, EndsOf(0)), Only(stats::TxnEnd::kCloseRollback));

    // The cause was taken by the rollback that read it: the next is the
    // client's again.
    before = EndsOf(0);
    ASSERT_TRUE(StartsWith(Run(0, s, "BEGIN"), "BEGIN"));
    ASSERT_TRUE(StartsWith(Run(0, s, "ROLLBACK"), "ROLLBACK"));
    EXPECT_EQ(Moved(before, EndsOf(0)), Only(stats::TxnEnd::kRollback));

    // And one named for a `READ ONLY` transaction, which no count takes, is
    // dropped with that transaction rather than kept for the next.
    before = EndsOf(0);
    ASSERT_TRUE(StartsWith(Run(0, s, "BEGIN READ ONLY"), "BEGIN"));
    s.set_rollback_cause(stats::TxnEnd::kCeilingRollback);
    ASSERT_EQ(D(0).DispatchInternal("ROLLBACK", &s).response, "ROLLBACK trx_id=none");
    ASSERT_TRUE(StartsWith(Run(0, s, "BEGIN"), "BEGIN"));
    ASSERT_TRUE(StartsWith(Run(0, s, "ROLLBACK"), "ROLLBACK"));
    EXPECT_EQ(Moved(before, EndsOf(0)), Only(stats::TxnEnd::kRollback));
}

TEST_F(TxnStatsRig, AWriteThatGivesItsTransactionUpToWaitIsAWaitAbortNotAnAbort) {
    // Core 0 holds row 1 in an open transaction; core 1's autocommit UPDATE
    // of it has written nothing, so it gives its own transaction up, waits
    // for the decide and runs again (`txn.md` §5).
    Script& holder = scripts_[0];
    Script& waiter = scripts_[1];
    ASSERT_TRUE(StartsWith(Run(0, holder.session, "BEGIN"), "BEGIN"));
    ASSERT_EQ(Run(0, holder.session, "UPDATE t SET v = 5 WHERE id = 1"), "UPDATED 1");

    const Ends before0 = EndsOf(0);
    const Ends before1 = EndsOf(1);
    waiter.lines = {"UPDATE t SET v = v + 1 WHERE id = 1"};
    rig_->core(1).scheduler().Submit(TaskOf(*rig_, 1, waiter));
    rig_->Start();
    ASSERT_TRUE(KickUntil(*rig_, 1, [&] {
        return Of(EndsOf(1), stats::TxnEnd::kWaitAbort) > Of(before1, stats::TxnEnd::kWaitAbort);
    })) << "the waiter never gave its transaction up";
    EXPECT_FALSE(waiter.done.load(std::memory_order_acquire));

    holder.lines = {"COMMIT"};
    rig_->Post(0, TaskOf(*rig_, 0, holder));
    ASSERT_TRUE(KickUntil(*rig_, 1, [&] { return waiter.done.load(std::memory_order_acquire); },
                          8000ms));
    ASSERT_TRUE(KickUntil(*rig_, 0, [&] { return holder.done.load(std::memory_order_acquire); }));
    rig_->Stop();
    ASSERT_EQ(waiter.replies, (std::vector<std::string>{"UPDATED 1"}));

    const Ends moved1 = Moved(before1, EndsOf(1));
    EXPECT_GE(Of(moved1, stats::TxnEnd::kWaitAbort), 1u);
    EXPECT_EQ(Of(moved1, stats::TxnEnd::kAbort), 0u) << "a wait was counted as a failure";
    EXPECT_EQ(Of(moved1, stats::TxnEnd::kCommit), 1u) << "the statement that ran again commits once";
    EXPECT_EQ(Moved(before0, EndsOf(0)), Only(stats::TxnEnd::kCommit));
}

TEST_F(TxnStatsRig, AnExplicitTransactionsLifeIsRecordedOnceByAccessMode) {
    Session s;
    const std::uint64_t rw = Lives(1, /*read_only=*/false);
    const std::uint64_t ro = Lives(1, /*read_only=*/true);
    const std::uint64_t rw_core0 = Lives(0, /*read_only=*/false);

    // An autocommit statement's transaction is not an explicit one.
    ASSERT_TRUE(StartsWith(Run(1, s, "INSERT INTO t VALUES (3, 3)"), "INSERTED"));
    EXPECT_EQ(Lives(1, false), rw);

    ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN"), "BEGIN"));
    EXPECT_EQ(Lives(1, false), rw) << "a life is recorded at its end";
    ASSERT_TRUE(StartsWith(Run(1, s, "COMMIT"), "COMMIT"));
    EXPECT_EQ(Lives(1, false), rw + 1);
    ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN"), "BEGIN"));
    ASSERT_TRUE(StartsWith(Run(1, s, "ROLLBACK"), "ROLLBACK"));
    EXPECT_EQ(Lives(1, false), rw + 2);
    EXPECT_EQ(Lives(1, true), ro);

    // 20 ms held: at least bucket 14, [16.4 ms, 32.8 ms).
    ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN READ ONLY"), "BEGIN"));
    std::this_thread::sleep_for(20ms);
    ASSERT_EQ(Run(1, s, "COMMIT"), "COMMIT trx_id=none");
    EXPECT_EQ(Lives(1, true), ro + 1);
    EXPECT_EQ(Lives(1, false), rw + 2);
    EXPECT_EQ(Lives(0, false), rw_core0) << "a life is its core's";

    // The view shows the same rows: one per core, access mode and bucket.
    const auto shown = testing_view::ReadViewText(
        D(0).Dispatch("SELECT core, access, bucket, lo_us, count FROM sys.txn_histogram "
                      "WHERE core = 1")
            .response);
    std::uint64_t shown_ro = 0;
    std::uint64_t shown_rw = 0;
    bool held_20ms = false;
    for (const std::vector<std::string>& row : shown.rows) {
        ASSERT_EQ(row.size(), 5u);
        EXPECT_EQ(row[0], "1");
        const std::uint64_t bucket = std::stoull(row[2]);
        EXPECT_EQ(row[3], std::to_string(bucket == 0 ? 0 : std::uint64_t{1} << bucket));
        if (row[1] == "read only") {
            shown_ro += std::stoull(row[4]);
            if (bucket >= 14) held_20ms = true;
        } else {
            EXPECT_EQ(row[1], "read write");
            shown_rw += std::stoull(row[4]);
        }
    }
    EXPECT_EQ(shown_ro, Lives(1, true));
    EXPECT_EQ(shown_rw, Lives(1, false));
    EXPECT_TRUE(held_20ms) << "a transaction held 20 ms is in no bucket at or above 16 ms";
}

TEST_F(TxnStatsRig, TheHorizonsAgeGrowsWhileAReadOnlyTransactionIsHeldAndReturnsWhenItEnds) {
    Session held;
    (void)held.WaitStats();  // what a listener makes at a session's first statement
    ASSERT_TRUE(StartsWith(Run(1, held, "BEGIN READ ONLY ISOLATION LEVEL REPEATABLE READ"),
                           "BEGIN"));
    const std::uint64_t held_lsn = rig_->visibility().HorizonLsn();
    ASSERT_NE(held_lsn, txn::kUnboundedBound);
    // A commit on the other core: every view minted from here is above it.
    ASSERT_TRUE(StartsWith(D(0).Dispatch("INSERT INTO t VALUES (2, 2)").response, "INSERTED"));
    std::this_thread::sleep_for(30ms);

    // Read on core 0: the horizon is core 1's, as old as the transaction.
    EXPECT_EQ(Meta(0, "read_horizon_lsn"), std::to_string(held_lsn));
    EXPECT_EQ(Meta(0, "read_horizon_core"), "1");
    const std::uint64_t age_us = std::stoull(Meta(0, "read_horizon_age_us"));
    EXPECT_GE(age_us, 30'000u);
    // And the session that holds it is found by its two ages (DF-Q8 (a)).
    EXPECT_GE(std::stoull(StateOf(0, held, "txn_age_us")), 30'000u);
    EXPECT_GE(std::stoull(StateOf(0, held, "view_age_us")), 30'000u);

    std::this_thread::sleep_for(10ms);
    EXPECT_GE(std::stoull(Meta(0, "read_horizon_age_us")), age_us + 10'000u) << "it grows";

    ASSERT_EQ(Run(1, held, "COMMIT"), "COMMIT trx_id=none");
    // Nothing older is held: the statement reading the fields holds a view of
    // its own while it runs, on its own core, an instant old.
    EXPECT_EQ(Meta(0, "read_horizon_core"), "0");
    EXPECT_GT(std::stoull(Meta(0, "read_horizon_lsn")), held_lsn);
    EXPECT_LT(std::stoull(Meta(0, "read_horizon_age_us")), 30'000u);
    EXPECT_EQ(StateOf(0, held, "txn_age_us"), "NULL");
    EXPECT_EQ(StateOf(0, held, "view_age_us"), "NULL");
}

TEST_F(TxnStatsRig, AReadCommittedTransactionsViewIsAsOldAsItsLastStatement) {
    Session s;
    (void)s.WaitStats();
    EXPECT_EQ(StateOf(0, s, "txn_age_us"), "NULL") << "autocommit holds no transaction";
    EXPECT_EQ(StateOf(0, s, "view_age_us"), "NULL");

    ASSERT_TRUE(StartsWith(Run(1, s, "BEGIN"), "BEGIN"));
    std::this_thread::sleep_for(30ms);
    // Still `BEGIN`'s view: nothing has re-minted it.
    EXPECT_GE(std::stoull(StateOf(0, s, "view_age_us")), 30'000u);
    ASSERT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 1"), "v\\n1");
    const std::uint64_t txn_age = std::stoull(StateOf(0, s, "txn_age_us"));
    const std::uint64_t view_age = std::stoull(StateOf(0, s, "view_age_us"));
    EXPECT_GE(txn_age, 30'000u);
    EXPECT_LT(view_age, txn_age) << "the statement's boundary re-minted the view";
    EXPECT_LT(view_age, 30'000u);
    // The transaction's view, with the statement over, is what its core
    // still publishes: the horizon's age is the view's, not the transaction's.
    EXPECT_EQ(Meta(0, "read_horizon_core"), "1");
    EXPECT_LT(std::stoull(Meta(0, "read_horizon_age_us")), txn_age);

    ASSERT_TRUE(StartsWith(Run(1, s, "ROLLBACK"), "ROLLBACK"));
    EXPECT_EQ(StateOf(0, s, "txn_age_us"), "NULL");
    EXPECT_EQ(StateOf(0, s, "view_age_us"), "NULL");

    // An autocommit statement's view ends with the statement.
    ASSERT_EQ(Run(1, s, "SELECT v FROM t WHERE id = 1"), "v\\n1");
    EXPECT_EQ(StateOf(0, s, "view_age_us"), "NULL");
}

}  // namespace
}  // namespace kds::server
