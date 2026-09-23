// **A transaction cannot read its own uncommitted write to a peer-owned
// relation**, because the write runs locally and the read still ships.
//
// Written during AT-S6's survey as the reproduction of a defect, landed
// disabled, and **enabled by AT-S6 itself**: the operator took AT-0
// item 4 / D18's first shape on 2026-09-23 - a read stops shipping - so
// both halves of the transaction run on the core the session is on, under
// one transaction id, and the read sees its own row.
//
// **The mutation**: put the ship arm back under `CheckReadAffinity`'s
// refusal and this cell reads a header and no rows.

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
    rig->core(0).catalog().SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    ASSERT_EQ(d0.Dispatch("CREATE TABLE r1 (id int64, v int64) BTREE").response.substr(0, 3),
              "CRE");
    auto oid = rig->core(0).catalog().FindTableOidByName("r1");
    ASSERT_TRUE(oid.ok());
    auto row = rig->core(0).catalog().GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    ASSERT_EQ(row.value().owner_core, 1u) << "r1 must be core 1's for the read to ship";
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

}  // namespace
}  // namespace kds::server
