// **A peer records a Waystone trail** (AT-S7).
//
// `waystone_recording` was honoured as "off" on every core a `CoreRuntime`
// opened, and `core_runtime.hpp`'s asymmetry 3 said why: `sys.patterns` is
// a catalog page, the catalog was read-only on a peer, and the registration
// cannot be shipped because `RegisterPattern` returns a `PatternAccess*`
// the recorder uses immediately. Every core writes catalog pages since
// AT-S5, so the instance's switch means the same thing on every core.
//
// **The mutation**: restore the `/*recorder=*/nullptr` that stood in
// `CoreRuntime::Open`'s dispatcher construction until this stage, for the
// peer alone - `is_peer ? nullptr : ...` - and the count is unmoved,
// `1 vs 1`. Scoped to the peer because both cores are `CoreRuntime`s here
// and the unscoped form fails the cell at its premise instead of its
// claim.

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

TEST(WaystoneAcrossCores, APeersRepeatedStatementRegistersItsPattern) {
    TwoCoreRig::Options options;
    options.wal_drain_interval_ns = 1'000'000;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto rig = std::move(opened.value());

    CommandDispatcher& d0 = rig->core(0).dispatcher();
    rig->core(0).catalog().SetPlacementPolicy(catalog::PlacementPolicy::kCreatingCore);
    ASSERT_EQ(d0.Dispatch("CREATE TABLE r0 (id int64, v int64) BTREE").response.substr(0, 3),
              "CRE");
    auto oid = rig->core(0).catalog().FindTableOidByName("r0");
    ASSERT_TRUE(oid.ok());
    ASSERT_TRUE(rig->store().FlushPages(catalog::kEveryCatalogPage).ok());
    ASSERT_TRUE(rig->FundPeerRelation(oid.value()).ok());
    ASSERT_EQ(d0.Dispatch("INSERT INTO r0 VALUES (7)").response.substr(0, 8), "INSERTED");

    const auto pattern_count = [&] {
        auto rows = rig->core(0).catalog().ListPatterns();
        EXPECT_TRUE(rows.ok());
        return rows.ok() ? rows.value().size() : std::size_t{0};
    };
    // Core 0's own shape first, twice, so `before` proves this fixture
    // records at all - without it a peer that recorded nothing and a
    // fixture that records nothing look the same.
    d0.Dispatch("SELECT id FROM r0 WHERE id = 17");
    d0.Dispatch("SELECT id FROM r0 WHERE id = 17");
    const std::size_t before = pattern_count();
    ASSERT_EQ(before, 1u) << "core 0 registered nothing, so the comparison below says nothing";

    // **Twice would be the threshold and is not enough here.**
    // `kAutoRecordThreshold` is 2, so an instance seen once is a one-shot
    // query and pays no catalog page - but the registration that follows
    // allocates a `sys.patterns` row id, and on a peer that is the row-id
    // lease's demand-then-grant (PW1b): the first attempts are refused
    // with a spent block, the demand rides the tick to core 0, and the
    // grant lands a few rounds later. The same wait the **first** INSERT
    // into a relation pays on a peer, arriving here because AT-S7 gave a
    // peer a recorder. Eight rounds is that wait with room, not a
    // threshold.
    rig->Start();
    for (int round = 0; round < 8; ++round) {
        OneShot peer;
        // A **different** shape from core 0's: one pattern row per shape,
        // so the peer re-running core 0's would move a `use_count` and no
        // row count.
        peer.statement = "SELECT v FROM r0 WHERE id = 17";
        rig->core(1).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, RunOne(rig->core(1).dispatcher(), peer)));
        ASSERT_TRUE(KickUntil(*rig, 1, [&] { return peer.done.load(std::memory_order_acquire); },
                              8000ms))
            << peer.out.response;
        // The row's `v`, and the separator in a debug-text response is the
        // two characters `\n`.
        ASSERT_EQ(peer.out.response, "v\\n7") << peer.out.response;
    }

    EXPECT_GT(pattern_count(), before) << "the peer's repeated shape left no sys.patterns row";
    rig->Stop();
}

}  // namespace
}  // namespace kds::server
