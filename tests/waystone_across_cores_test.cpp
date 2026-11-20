// **A peer records a Waystone trail** (AT-S7), **for a pattern opted in on
// another core, from its first statement after the opt-in** (CF-S1b, and
// through the opted count and the opted map since CF-S1c).
//
// `waystone_recording` was honoured as "off" on every core a `CoreRuntime`
// opened until AT-S7; every core writes catalog pages since AT-S5, so the
// instance's switch means the same thing on every core. Since CF-S1b no core
// registers a pattern: `WAYSTONE ON` writes the row and its directory, moves
// the instance's opted count and the schema word, and a peer's next statement
// drops its catalog memo at its head (`Catalog::Revalidate`), reads the count
// non-zero and refills its opted map from the page.
//
// **The mutations**: restore the `/*recorder=*/nullptr` that stood in
// `CoreRuntime::Open`'s dispatcher construction until AT-S7, for the peer
// alone - `is_peer ? nullptr : ...` - and nothing replays on the peer; drop
// `SetPatternWaystone`'s `BumpVersion`, and the peer's memo keeps the
// opted-out row's root and replays after the OFF.

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

TEST(WaystoneAcrossCores, APeerRecordsAndStopsWithinOneStatementOfAnotherCoresOptIn) {
    TwoCoreRig::Options options;
    options.wal_drain_interval_ns = 1'000'000;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto rig = std::move(opened.value());

    CommandDispatcher& d0 = rig->core(0).dispatcher();
    ASSERT_EQ(d0.Dispatch("CREATE TABLE r0 (id int64, v int64) BTREE").response.substr(0, 3),
              "CRE");
    ASSERT_TRUE(rig->store().Flush().ok());
    ASSERT_EQ(d0.Dispatch("INSERT INTO r0 VALUES (7)").response.substr(0, 8), "INSERTED");

    const std::string sql = "SELECT v FROM r0 WHERE id = 1";
    const auto on = [&](std::uint32_t core, const std::string& statement) {
        OneShot shot;
        shot.statement = statement;
        rig->Post(core, sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                                            RunOne(rig->core(core).dispatcher(), shot)));
        EXPECT_TRUE(KickUntil(*rig, core,
                              [&] { return shot.done.load(std::memory_order_acquire); }, 8000ms))
            << statement << " -> " << shot.out.response;
        return shot.out.response;
    };

    rig->Start();
    // Not opted: the peer runs the shape past the threshold and records
    // nothing - nor is it sighted (CF-S1c), so the count starts at the
    // opt-in below.
    for (int round = 0; round < 3; ++round) ASSERT_EQ(on(1, sql), "v\\n7");
    EXPECT_EQ(on(1, "ANALYZE " + sql).find("replays="), std::string::npos)
        << "a shape nobody opted in replayed";

    // Opted in on core 0. The peer's very next execution is its first
    // sighting, the one after records, and the third replays - so a replay
    // on the third proves the first statement after the opt-in already saw
    // it; a peer a statement late would sight on the second, record on the
    // third and replay nothing.
    const std::string opted = on(0, "WAYSTONE ON " + sql);
    ASSERT_EQ(opted.rfind("OK WAYSTONE ON", 0), 0u) << opted;
    ASSERT_EQ(on(1, sql), "v\\n7");
    ASSERT_EQ(on(1, sql), "v\\n7");
    const std::string replayed = on(1, "ANALYZE " + sql);
    EXPECT_NE(replayed.find("replays=1"), std::string::npos)
        << "the peer did not see the opt-in within one statement: " << replayed;

    // Opted out on core 0: the peer's memo held the row with its root, and
    // its next statement must not replay through it.
    ASSERT_EQ(on(0, "WAYSTONE OFF " + sql).rfind("OK WAYSTONE OFF", 0), 0u);
    const std::string after = on(1, "ANALYZE " + sql);
    EXPECT_EQ(after.find("replays="), std::string::npos)
        << "the peer replayed after another core's opt-out: " << after;
    rig->Stop();
}

}  // namespace
}  // namespace kds::server
