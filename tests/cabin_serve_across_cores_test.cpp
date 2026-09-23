// **A query served from a Cabin set misses rows another core wrote.**
//
// A set is banked from a walk that sees every row, but it is kept in the
// **writing** core's store, and since AT-S5 a write runs where the session
// is - so two cores write one relation and each store holds half. An
// exhausted-set answer is then short.
//
// `docs/inflight/bugs/a-cabin-set-serves-a-query-short-across-cores.md`
// carries the finding; this is its reproduction, written during AT-S6's
// survey and **disabled** because the fix is AT-S7's (one store for the
// instance, AT-0 item 9). Enable it with that stage - it is the cell it
// owes.

#include "two_core_rig.hpp"

#include <string>

#include <gtest/gtest.h>

#include "kds/server/session.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

struct OneShot {
    Session session;
    DispatchOutcome out;
    std::atomic<bool> done{false};
    std::string statement;
};

sched::Coro RunOne(CommandDispatcher& d, OneShot& o) {
    co_await d.DispatchAsync(o.statement, &o.session, &o.out);
    o.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

TEST(CabinServeAcrossCores, DISABLED_AQueryServedFromThisCoresSetSeesARowAnotherCoreWrote) {
    TwoCoreRig::Options options;
    options.wal_drain_interval_ns = 1'000'000;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto rig = std::move(opened.value());

    CommandDispatcher& d0 = rig->core(0).dispatcher();
    // Core 0's own relation, so nothing about this probe is about routing.
    rig->core(0).catalog().SetPlacementPolicy(catalog::PlacementPolicy::kCreatingCore);
    ASSERT_EQ(d0.Dispatch("CREATE TABLE r0 (id int64, v int64) BTREE").response.substr(0, 3),
              "CRE");
    ASSERT_EQ(d0.Dispatch("CREATE CABIN ON r0(v)").response.substr(0, 3), "CRE");
    auto oid = rig->core(0).catalog().FindTableOidByName("r0");
    ASSERT_TRUE(oid.ok());
    ASSERT_TRUE(rig->store().FlushPages(catalog::kEveryCatalogPage).ok());
    ASSERT_TRUE(rig->FundPeerRelation(oid.value()).ok());

    // One row of v=7 written here, then the read that banks core 0's set
    // for the value 7 - from a walk, so the set is complete as of now.
    ASSERT_EQ(d0.Dispatch("INSERT INTO r0 VALUES (7)").response.substr(0, 8), "INSERTED");
    // The ids are issued, and the peer holds the first block: core 0's row
    // is 17 and core 1's is 1. Named here so the assertion below is about
    // two identified rows rather than about a count.
    const std::string first = d0.Dispatch("SELECT id FROM r0 WHERE v = 7").response;
    ASSERT_NE(first.find("17"), std::string::npos) << first;

    // A second row of v=7, written by a session on core 1 - which since
    // AT-S5 runs there rather than on the relation's owner, and files its
    // Cabin entry into core 1's store.
    OneShot peer;
    peer.statement = "INSERT INTO r0 VALUES (7)";
    rig->core(1).scheduler().Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground, RunOne(rig->core(1).dispatcher(), peer)));
    rig->Start();
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return peer.done.load(std::memory_order_acquire); },
                          8000ms))
        << peer.out.response;
    ASSERT_EQ(peer.out.response.rfind("INSERTED", 0), 0u) << peer.out.response;

    // Core 0 asks again. Two rows carry v=7; a set served from core 0's
    // store holds one.
    const std::string again = d0.Dispatch("SELECT id FROM r0 WHERE v = 7").response;
    EXPECT_NE(again.find("17"), std::string::npos) << "core 0's own row: " << again;
    EXPECT_NE(again.find("\n1\n"), std::string::npos)
        << "the row core 1 wrote is missing from core 0's answer: " << again;
    // And the walk agrees there are two, which is what makes the answer
    // above short rather than right.
    const std::string counted = d0.Dispatch("SELECT COUNT(*) FROM r0").response;
    EXPECT_NE(counted.find("2"), std::string::npos) << counted;
    rig->Stop();
}

}  // namespace
}  // namespace kds::server
