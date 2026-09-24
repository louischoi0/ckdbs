// **A transaction cannot read its own uncommitted write to a peer-owned
// relation**, because the write runs locally and the read still ships.
//
// Written during AT-S6's survey as the reproduction of a defect, landed
// disabled, and **enabled by AT-S6 itself**: the operator took AT-0
// item 4 / D18's first shape on 2026-09-23 - a read stops shipping - so
// both halves of the transaction run on the core the session is on, under
// one transaction id, and the read sees its own row.
//
// **The mutation**: put back AT-S6's ship arm for a read of a relation
// another core owned and this cell reads a header and no rows.

#include "two_core_rig.hpp"

#include <string>

#include <gtest/gtest.h>

#include "kds/server/session.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

struct Txn {
    Session session;
    DispatchOutcome begin_out;
    DispatchOutcome write_out;
    DispatchOutcome read_out;
    DispatchOutcome end_out;
    DispatchOutcome after_out;
    std::atomic<bool> done{false};
};

sched::Coro WriteThenRead(CommandDispatcher& d, Txn& t) {
    co_await d.DispatchAsync("BEGIN", &t.session, &t.begin_out);
    co_await d.DispatchAsync("INSERT INTO r1 VALUES (41)", &t.session, &t.write_out);
    co_await d.DispatchAsync("SELECT v FROM r1", &t.session, &t.read_out);
    co_await d.DispatchAsync("COMMIT", &t.session, &t.end_out);
    co_await d.DispatchAsync("SELECT v FROM r1", &t.session, &t.after_out);
    t.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

TEST(ShippedReadOwnWrite, ATransactionReadsItsOwnUncommittedWriteToAPeerOwnedRelation) {
    TwoCoreRig::Options options;
    options.wal_drain_interval_ns = 1'000'000;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto rig = std::move(opened.value());

    CommandDispatcher& d0 = rig->core(0).dispatcher();
    // No core owns r1 since AT-S9, so there is no ship left to route
    // around; the cell now pins read-your-own-write on the session's core.
    ASSERT_EQ(d0.Dispatch("CREATE TABLE r1 (id int64, v int64) BTREE").response.substr(0, 3),
              "CRE");
    auto oid = rig->core(0).catalog().FindTableOidByName("r1");
    ASSERT_TRUE(oid.ok());
    ASSERT_TRUE(rig->store().FlushPages(catalog::kEveryCatalogPage).ok());
    ASSERT_TRUE(rig->FundPeerRelation(oid.value()).ok());

    Txn t;
    rig->core(0).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, WriteThenRead(d0, t)));
    rig->Start();
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return t.done.load(std::memory_order_acquire); },
                          8000ms))
        << "begin=[" << t.begin_out.response << "] write=[" << t.write_out.response
        << "] read=[" << t.read_out.response << "] commit=[" << t.end_out.response << "]";

    EXPECT_EQ(t.write_out.response.rfind("INSERTED", 0), 0u)
        << "the local write to a peer-owned relation: " << t.write_out.response;
    // The question this probe exists to answer, printed either way.
    EXPECT_NE(t.read_out.response.find("41"), std::string::npos)
        << "read-your-own-writes across the ship: " << t.read_out.response;
    // And the row is really there once the transaction commits, which is
    // what tells a blind read from a lost write.
    EXPECT_EQ(t.end_out.response.rfind("COMMIT", 0), 0u) << t.end_out.response;
    EXPECT_NE(t.after_out.response.find("41"), std::string::npos)
        << "the committed row, read back: " << t.after_out.response;
    rig->Stop();
}


// **AT-S9: a join inside a transaction sees the transaction's own writes.**
// The two-step pipeline sent a join to its relations' owners whenever
// either was another core's, and its stages read under their own
// autocommit snapshot - so inside `BEGIN` it could not see the rows the
// transaction had just written, the single-step defect above in the join's
// shape. AT-S6 had removed the enrolment test that kept the route off
// transactions; AT-S9 removed the route with ownership. The session here is
// the peer's, and both relations were created on core 0, which under the
// shipped placement made both core 0's - the shape that shipped.
struct JoinTxn {
    Session session;
    DispatchOutcome begin_out;
    DispatchOutcome outer_out;
    DispatchOutcome inner_out;
    DispatchOutcome join_out;
    DispatchOutcome end_out;
    std::atomic<bool> done{false};
};

sched::Coro WriteBothThenJoin(CommandDispatcher& d, JoinTxn& t) {
    co_await d.DispatchAsync("BEGIN", &t.session, &t.begin_out);
    co_await d.DispatchAsync("INSERT INTO acct VALUES (7, 500)", &t.session, &t.outer_out);
    co_await d.DispatchAsync("INSERT INTO fill VALUES (7, 99)", &t.session, &t.inner_out);
    co_await d.DispatchAsync("SELECT acct.bal, fill.qty FROM acct JOIN fill ON acct.id = fill.aid",
                             &t.session, &t.join_out);
    co_await d.DispatchAsync("ROLLBACK", &t.session, &t.end_out);
    t.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

TEST(ShippedReadOwnWrite, AJoinInsideATransactionOnAPeerSeesItsOwnWrites) {
    TwoCoreRig::Options options;
    options.wal_drain_interval_ns = 1'000'000;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto rig = std::move(opened.value());

    CommandDispatcher& d0 = rig->core(0).dispatcher();
    ASSERT_EQ(d0.Dispatch("CREATE TABLE acct (id int64, bal int64) BTREE").response.substr(0, 3),
              "CRE");
    ASSERT_EQ(
        d0.Dispatch("CREATE TABLE fill (id int64, aid int64, qty int64) BTREE").response.substr(0, 3),
        "CRE");
    ASSERT_TRUE(rig->store().FlushPages(catalog::kEveryCatalogPage).ok());
    for (const char* name : {"acct", "fill"}) {
        auto oid = rig->core(0).catalog().FindTableOidByName(name);
        ASSERT_TRUE(oid.ok()) << name;
        ASSERT_TRUE(rig->FundPeerRelation(oid.value()).ok()) << name;
    }

    JoinTxn t;
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    rig->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, WriteBothThenJoin(d1, t)));
    rig->Start();
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return t.done.load(std::memory_order_acquire); },
                          8000ms))
        << "begin=[" << t.begin_out.response << "] outer=[" << t.outer_out.response
        << "] inner=[" << t.inner_out.response << "] join=[" << t.join_out.response << "]";

    ASSERT_EQ(t.outer_out.response.rfind("INSERTED", 0), 0u) << t.outer_out.response;
    ASSERT_EQ(t.inner_out.response.rfind("INSERTED", 0), 0u) << t.inner_out.response;
    EXPECT_NE(t.join_out.response.find("500,99"), std::string::npos)
        << "the join could not see the transaction's own rows: " << t.join_out.response;
    rig->Stop();
}
}  // namespace
}  // namespace kds::server
