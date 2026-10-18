// **A counter bumped from two cores loses nothing** (BJ-S4, BJ-R2, BJ-R9).
//
// `SET v = v + 1 WHERE id = k` is the statement BJ exists for: a client
// applies a delta in one statement instead of a `SELECT`, arithmetic and an
// `UPDATE` that loses an update under READ COMMITTED. Two autocommit sessions,
// one on each core, each run it N times against one row. Every statement the
// engine accepts adds exactly one, so the final value is 2N; a statement it
// refuses retryable (`TXN_CONFLICT`: first-updater-wins meeting the other
// core's undecided write) is counted and retried by the driver, never lost.
// A read of the value between two writes could not give that - the cell fails
// if any increment is applied twice or dropped.

#include "two_core_rig.hpp"

#include <algorithm>
#include <atomic>
#include <string>

#include <gtest/gtest.h>

#include "kds/server/session.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

constexpr int kIncrementsPerCore = 600;
constexpr int kRetriesPerIncrement = 200;

struct Bumper {
    Session session;
    DispatchOutcome out;
    std::string first_refusal;  // one that was not retryable, or ran out of retries
    int retries = 0;            // retryable refusals, each one retried
    std::atomic<int> applied{0};  // statements the engine answered `UPDATED 1`
    // What the other core had applied when this one finished: above 0 only
    // if the two ran at the same time, so the cell is a no-loss check under
    // overlap and not under two runs in turn.
    int other_applied_at_done = -1;
    std::atomic<bool> done{false};
};

sched::Coro BumpMany(CommandDispatcher& d, Bumper& b, Bumper& other, std::string sql) {
    for (int i = 0; i < kIncrementsPerCore; ++i) {
        for (int attempt = 0; attempt <= kRetriesPerIncrement; ++attempt) {
            co_await d.DispatchAsync(sql, &b.session, &b.out);
            if (b.out.response.rfind("UPDATED 1", 0) == 0) {
                b.applied.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            const bool retryable = b.out.response.find("retryable=1") != std::string::npos;
            if (!retryable || attempt == kRetriesPerIncrement) {
                if (b.first_refusal.empty()) b.first_refusal = b.out.response;
                break;
            }
            ++b.retries;
        }
        if (!b.first_refusal.empty()) break;
    }
    b.other_applied_at_done = other.applied.load(std::memory_order_relaxed);
    b.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

TEST(SetExprTwoCoreRigTest, TwoCoresIncrementingOneRowLoseNoIncrement) {
    auto opened = TwoCoreRig::Open();
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto rig = std::move(opened.value());

    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    ASSERT_EQ(d0.Dispatch("CREATE TABLE c (id int64, v int64) BTREE").response.substr(0, 3), "CRE");
    ASSERT_EQ(d0.Dispatch("INSERT INTO c VALUES (1, 0)").response.substr(0, 8), "INSERTED");
    ASSERT_EQ(d0.Dispatch("INSERT INTO c VALUES (2, 0)").response.substr(0, 8), "INSERTED");
    ASSERT_TRUE(rig->store().Flush().ok());

    // Both cores bump row 1, so the two writers meet on one row; row 2 shares
    // its leaf and is never written, so a statement that touched a row it
    // did not name would show.
    Bumper a0;
    Bumper a1;
    rig->core(0).scheduler().Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        BumpMany(d0, a0, a1, "UPDATE c SET v = v + 1 WHERE id = 1")));
    rig->core(1).scheduler().Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        BumpMany(d1, a1, a0, "UPDATE c SET v = v + 1 WHERE id = 1")));
    rig->Start();
    ASSERT_TRUE(KickUntil(
        *rig, 0,
        [&] {
            rig->wakers().Kick(1);
            return a0.done.load(std::memory_order_acquire) &&
                   a1.done.load(std::memory_order_acquire);
        },
        60000ms));
    rig->Stop();

    EXPECT_TRUE(a0.first_refusal.empty()) << "core 0: " << a0.first_refusal;
    EXPECT_TRUE(a1.first_refusal.empty()) << "core 1: " << a1.first_refusal;
    EXPECT_EQ(a0.applied.load(), kIncrementsPerCore);
    EXPECT_EQ(a1.applied.load(), kIncrementsPerCore);
    EXPECT_GT(std::max(a0.other_applied_at_done, a1.other_applied_at_done), 0)
        << "the first core to finish found the other had applied nothing: no overlap";
    EXPECT_EQ(d0.Dispatch("SELECT v FROM c WHERE id = 1").response,
              "v\\n" + std::to_string(2 * kIncrementsPerCore))
        << "every accepted statement added exactly one (" << a0.retries + a1.retries
        << " retryable refusals were retried)";
    EXPECT_EQ(d0.Dispatch("SELECT v FROM c WHERE id = 2").response, "v\\n0");
    RecordProperty("retried_refusals", a0.retries + a1.retries);
}

TEST(SetExprTwoCoreRigTest, AFailureMidStatementUndoesTheRowsBeforeItInAutocommit) {
    // BJ-R10: the refusal names the row, and the statement is one
    // transaction, so the rows it had written are not left behind.
    auto opened = TwoCoreRig::Open();
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto rig = std::move(opened.value());
    CommandDispatcher& d = rig->core(0).dispatcher();
    ASSERT_EQ(d.Dispatch("CREATE TABLE c (id int64, v int8) BTREE").response.substr(0, 3), "CRE");
    ASSERT_EQ(d.Dispatch("INSERT INTO c VALUES (1, 10)").response.substr(0, 8), "INSERTED");
    ASSERT_EQ(d.Dispatch("INSERT INTO c VALUES (2, 100)").response.substr(0, 8), "INSERTED");

    const std::string out = d.Dispatch("UPDATE c SET v = v * 2 WHERE id >= 1").response;
    EXPECT_NE(out.find("outside int8"), std::string::npos) << out;
    EXPECT_NE(out.find("row id 2"), std::string::npos) << out;
    EXPECT_EQ(d.Dispatch("SELECT v FROM c WHERE id = 1").response, "v\\n10")
        << "row 1 was written before row 2 failed, and the abort undid it";
    EXPECT_EQ(d.Dispatch("SELECT v FROM c WHERE id = 2").response, "v\\n100");
}

TEST(SetExprTwoCoreRigTest, AFailureInsideBeginPoisonsTheTransaction) {
    auto opened = TwoCoreRig::Open();
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto rig = std::move(opened.value());
    CommandDispatcher& d = rig->core(0).dispatcher();
    ASSERT_EQ(d.Dispatch("CREATE TABLE c (id int64, v int8) BTREE").response.substr(0, 3), "CRE");
    ASSERT_EQ(d.Dispatch("INSERT INTO c VALUES (1, 10)").response.substr(0, 8), "INSERTED");
    ASSERT_EQ(d.Dispatch("INSERT INTO c VALUES (2, 100)").response.substr(0, 8), "INSERTED");

    Session session;
    ASSERT_EQ(d.Dispatch("BEGIN", &session).response.substr(0, 5), "BEGIN");
    const std::string out = d.Dispatch("UPDATE c SET v = v * 2 WHERE id >= 1", &session).response;
    EXPECT_NE(out.find("outside int8"), std::string::npos) << out;
    // Poisoned: a further statement is refused until the rollback, as after
    // every mid-statement write failure (the AS9 resolution).
    EXPECT_EQ(d.Dispatch("UPDATE c SET v = 1 WHERE id = 1", &session).response.substr(0, 3), "ERR");
    d.Dispatch("ROLLBACK", &session);
    EXPECT_EQ(d.Dispatch("SELECT v FROM c WHERE id = 1").response, "v\\n10");
}

}  // namespace
}  // namespace kds::server
