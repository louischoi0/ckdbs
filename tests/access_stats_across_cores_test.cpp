// **A peer's access statistics reach `sys.access_stats`** (AT-S7).
//
// A peer recorded nothing at all until CR7, because the relation is pinned
// at page 11 inside the reserved range and a peer could not write it; CR7
// gave it an `AccessBatch` to fold into and a ring kind to flush over, and
// core 0 applied the fold. Every core writes every page since AT-S5, so the
// batch, the kind and CR8's permitted drop are gone and a statistic is
// written where the statement ran.
//
// **The mutation**: restore the hard `false` that stood in
// `CoreRuntime::Open`'s dispatcher construction until this stage, for the
// peer alone - `is_peer ? false : config.access_statistics` - and the
// relation's total is unmoved by the peer's read, `1 vs 1`. Scoped to the
// peer because both cores are `CoreRuntime`s here: the unscoped `false`
// silences core 0 too and the cell fails at its own premise instead of at
// its claim.

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

TEST(AccessStatsAcrossCores, APeersStatementIsCountedInTheOneRelation) {
    TwoCoreRig::Options options;
    options.wal_drain_interval_ns = 1'000'000;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto rig = std::move(opened.value());

    CommandDispatcher& d0 = rig->core(0).dispatcher();
    ASSERT_EQ(d0.Dispatch("CREATE TABLE r0 (id int64, v int64) BTREE").response.substr(0, 3),
              "CRE");
    auto oid = rig->core(0).catalog().FindTableOidByName("r0");
    ASSERT_TRUE(oid.ok());
    ASSERT_TRUE(rig->store().FlushPages(catalog::kEveryCatalogPage).ok());
    ASSERT_EQ(d0.Dispatch("INSERT INTO r0 VALUES (7)").response.substr(0, 8), "INSERTED");
    // A read here first, because a shape is recorded from an executed step
    // chain and an INSERT's write path is not one.
    ASSERT_EQ(d0.Dispatch("SELECT id FROM r0 WHERE v = 7").response, "id\\n1");

    // What this core has recorded for the relation before the peer runs.
    const auto count_for = [&](catalog::Oid rel) {
        auto rows = rig->core(0).catalog().ListAccessStats();
        EXPECT_TRUE(rows.ok());
        std::uint64_t total = 0;
        if (rows.ok()) {
            for (const catalog::SysAccessStatRow& row : rows.value()) {
                if (row.rel_id == rel) total += row.use_count;
            }
        }
        return total;
    };
    const std::uint64_t before = count_for(oid.value());
    ASSERT_GT(before, 0u) << "core 0 recorded nothing, so the comparison below says nothing";

    // A read on core 1 - where since AT-S6 it runs rather than shipping.
    OneShot peer;
    peer.statement = "SELECT id FROM r0 WHERE v = 7";
    rig->core(1).scheduler().Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground, RunOne(rig->core(1).dispatcher(), peer)));
    rig->Start();
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return peer.done.load(std::memory_order_acquire); },
                          8000ms))
        << peer.out.response;
    ASSERT_EQ(peer.out.response, "id\\n1") << peer.out.response;

    // **Immediately, not on a tick.** There is no flush to wait for: the
    // peer wrote the row under the relation's own root-page latch before
    // its statement returned.
    EXPECT_GT(count_for(oid.value()), before)
        << "the peer's read left no shape in sys.access_stats";
    rig->Stop();
}

}  // namespace
}  // namespace kds::server
