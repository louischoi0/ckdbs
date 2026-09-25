// **A query served from a Cabin set misses rows another core wrote.**
//
// A set is banked from a walk that sees every row, but it is kept in the
// **writing** core's store, and since AT-S5 a write runs where the session
// is - so two cores write one relation and each store holds half. An
// exhausted-set answer is then short.
//
// Written during AT-S6's survey as the reproduction of a defect, landed
// disabled, and **enabled by AT-S7**: one store for the instance, so the
// set a query is served from holds what every core wrote.
//
// **The mutation**: give each core its own store again - drop
// `config.cabins_store = &cabins_` from `two_core_rig.hpp`, which is the
// line `Expeditor` has as `core_config.cabins_store` - and the second
// answer is `id\n1`, one row where two carry the value.

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

TEST(CabinServeAcrossCores, AQueryServedFromThisCoresSetSeesARowAnotherCoreWrote) {
    TwoCoreRig::Options options;
    options.wal_drain_interval_ns = 1'000'000;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto rig = std::move(opened.value());

    CommandDispatcher& d0 = rig->core(0).dispatcher();
    ASSERT_EQ(d0.Dispatch("CREATE TABLE r0 (id int64, v int64) BTREE").response.substr(0, 3),
              "CRE");
    ASSERT_EQ(d0.Dispatch("CREATE CABIN ON r0(v)").response.substr(0, 3), "CRE");
    auto oid = rig->core(0).catalog().FindTableOidByName("r0");
    ASSERT_TRUE(oid.ok());
    ASSERT_TRUE(rig->store().FlushPages(catalog::kEveryCatalogPage).ok());

    // One row of v=7 written here, then the read that banks core 0's set
    // for the value 7 - from a walk, so the set is complete as of now.
    ASSERT_EQ(d0.Dispatch("INSERT INTO r0 VALUES (7)").response.substr(0, 8), "INSERTED");
    // The ids are one sequence both cores bump (AT-S10b): core 0's row is
    // 1 and core 1's will be 2. Named here so the assertion below is about
    // two identified rows rather than about a count.
    const std::string first = d0.Dispatch("SELECT id FROM r0 WHERE v = 7").response;
    ASSERT_EQ(first, "id\\n1") << first;

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
    // **Both rows, and the set is what served them.** The separator in a
    // debug-text response is the two characters `\n`, so the row core 1
    // wrote is `\n2` spelled that way and a real newline would match
    // nothing here - which is how this cell read as short when it was not.
    const std::string again = d0.Dispatch("SELECT id FROM r0 WHERE v = 7").response;
    EXPECT_EQ(again, "id\\n1\\n2")
        << "core 0's answer holds its own row and the one core 1 wrote: " << again;
    // And it was the set: `hits` moves on this statement where the read
    // above moved `misses` and banked. Without it the assertion above
    // passes on a walk and says nothing about the store.
    EXPECT_EQ(rig->core(0).cabins()->stats().hits, 1u)
        << "the answer above came from a walk, not from the banked set";
    // And the walk agrees there are two, which is what makes the answer
    // above short rather than right.
    const std::string counted = d0.Dispatch("SELECT COUNT(*) FROM r0").response;
    EXPECT_NE(counted.find("2"), std::string::npos) << counted;
    rig->Stop();
}

// **The other half of the same rule, and the stage's second
// done-condition**: a peer's observation is *banked* and then *served*.
// The cell above has core 0 bank and core 1 write; this one has core 1
// bank and core 0 serve, which is the direction a per-core store could
// not do at all - a set banked on a peer lived and died there.
//
// **The mutation**: drop `config.cabins_store = &cabins_` from
// `two_core_rig.hpp` and core 0's read is a miss that banks its own set,
// `hits = 0`.
TEST(CabinServeAcrossCores, ASetBankedByAPeerServesThisCoresQuery) {
    TwoCoreRig::Options options;
    options.wal_drain_interval_ns = 1'000'000;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto rig = std::move(opened.value());

    CommandDispatcher& d0 = rig->core(0).dispatcher();
    ASSERT_EQ(d0.Dispatch("CREATE TABLE r0 (id int64, v int64) BTREE").response.substr(0, 3),
              "CRE");
    ASSERT_EQ(d0.Dispatch("CREATE CABIN ON r0(v)").response.substr(0, 3), "CRE");
    auto oid = rig->core(0).catalog().FindTableOidByName("r0");
    ASSERT_TRUE(oid.ok());
    ASSERT_TRUE(rig->store().FlushPages(catalog::kEveryCatalogPage).ok());
    ASSERT_EQ(d0.Dispatch("INSERT INTO r0 VALUES (7)").response.substr(0, 8), "INSERTED");

    // Core 1 reads first, and a **declared** Cabin records on the first
    // sighting (`cabin.md` §5's n=1), so this walk is what banks the set.
    OneShot peer;
    peer.statement = "SELECT id FROM r0 WHERE v = 7";
    rig->core(1).scheduler().Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground, RunOne(rig->core(1).dispatcher(), peer)));
    rig->Start();
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return peer.done.load(std::memory_order_acquire); },
                          8000ms))
        << peer.out.response;
    ASSERT_EQ(peer.out.response, "id\\n1") << peer.out.response;
    ASSERT_EQ(rig->core(1).cabins()->stats().recordings, 1u)
        << "the peer banked nothing, so what core 0 serves below is its own set";

    // And core 0 is served from it - a set it never walked for.
    const std::string served = d0.Dispatch("SELECT id FROM r0 WHERE v = 7").response;
    EXPECT_EQ(served, "id\\n1") << served;
    EXPECT_EQ(rig->core(0).cabins()->stats().hits, 1u)
        << "core 0 walked instead of serving from the peer's set";
    EXPECT_EQ(rig->core(0).cabins()->stats().recordings, 1u)
        << "core 0 banked a second set, so the two stores are not one";
    rig->Stop();
}

}  // namespace
}  // namespace kds::server
