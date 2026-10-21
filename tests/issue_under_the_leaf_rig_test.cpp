#include "two_core_rig.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/session.hpp"
#include "kds/wal/durability.hpp"

// Defect A on the two-core rig (`instructions/v3.0.0/workorder-bb-issue-under-
// the-leaf.md` §0, BB-S2). Core 0 stops at the seam with its row's id fixed
// (`SetAfterRowIdFixedForTest`); core 1 inserts into the same relation; core 0
// is released. The walk must come back in key order.
//
// **BB fixed it by issue order; BD keeps it by placement** (BD-R6,
// `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`). Under BB-R1
// the seam sat inside the hold of the leaf the row lands on, so core 1
// blocked until core 0 placed. BD deleted BB-R1 on a btree: the seam runs
// before the descent, core 1 places its higher id first, and core 0's lower
// one lands below it where it sorts - so the cells keep their subjects and
// pass by placement (BD-R2), not by the order the ids were placed in.
//
// **The cells must not hang**: each gives core 1 a bounded look, releases
// core 0 whatever it saw, and only then waits for both.

namespace kds::server {
namespace {

using namespace std::chrono_literals;

bool StartsWith(const std::string& reply, std::string_view prefix) {
    return reply.rfind(prefix, 0) == 0;
}

// One statement on one reactor, once `go` is set.
struct OneStatement {
    Session session;
    std::string sql;
    DispatchOutcome out;
    std::function<bool()> go_pred;
    std::atomic<bool> go{false};
    std::atomic<bool> done{false};
};

sched::Coro Run(CommandDispatcher& d, OneStatement& s) {
    s.go_pred = [&s] { return s.go.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&s.go_pred};
    co_await d.DispatchAsync(s.sql, &s.session, &s.out);
    s.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

// Core 0's stop at the seam: records the id and waits for `release`. The
// hook is installed only for the race, and once released it returns at once.
struct Seam {
    std::atomic<std::uint64_t> stopped_at{0};
    std::atomic<bool> release{false};
    std::atomic<int> calls{0};

    void operator()(std::uint64_t id) {
        calls.fetch_add(1, std::memory_order_relaxed);
        stopped_at.store(id, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
    }
};

// **The race, run once.** Core 0 runs `first` and stops at the seam with its
// id fixed; core 1 then runs `second`; core 0 is released. Returns the id core
// 0 stopped at and core 1's reply, with core 0's checked to be an insert.
struct Raced {
    std::uint64_t first_id = 0;
    std::string second_reply;
    int seam_calls = 0;  // the seam runs once per row
    // Whether core 1 finished inside its bounded look, while core 0 was
    // stopped: a btree places without a hold over the issue (BD-R6), so it
    // does; a heap's tail is held across its issue (BB-R7), so it does not.
    bool second_finished_alone = false;
};

// `look` is core 1's bounded look; a cell whose core 1 waits on core 0
// keeps it below the lock family's 1 s fault net, or the wait is refused
// before core 0 is released.
Raced RaceTwoInserts(TwoCoreRig& rig, const std::string& first, const std::string& second,
                     std::chrono::milliseconds look = 1000ms) {
    Seam seam;
    OneStatement s0;
    OneStatement s1;
    s0.sql = first;
    s1.sql = second;
    // `relaxed`: the cell is about placement, and an acknowledgement that
    // waited for a sync would put the log's writer between the two cores.
    s0.session.set_durability(wal::DurabilityClass::kRelaxed);
    s1.session.set_durability(wal::DurabilityClass::kRelaxed);
    CommandDispatcher& d0 = rig.core(0).dispatcher();
    CommandDispatcher& d1 = rig.core(1).dispatcher();
    d0.SetAfterRowIdFixedForTest(std::ref(seam));
    rig.core(0).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, Run(d0, s0)));
    rig.core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, Run(d1, s1)));
    rig.Start();

    Raced raced;
    s0.go.store(true, std::memory_order_release);
    EXPECT_TRUE(KickUntil(rig, 0, [&] {
        return seam.stopped_at.load(std::memory_order_acquire) != 0;
    })) << "core 0 never reached the seam";
    raced.first_id = seam.stopped_at.load(std::memory_order_acquire);

    // Core 1's look: long enough for an unblocked insert to finish, and
    // bounded, because a blocked one never does until core 0 moves.
    s1.go.store(true, std::memory_order_release);
    raced.second_finished_alone =
        KickUntil(rig, 1, [&] { return s1.done.load(std::memory_order_acquire); }, look);

    seam.release.store(true, std::memory_order_release);
    EXPECT_TRUE(KickUntil(
        rig, 0,
        [&] {
            rig.wakers().Kick(1);
            return s0.done.load(std::memory_order_acquire) &&
                   s1.done.load(std::memory_order_acquire);
        },
        10000ms))
        << "a statement did not finish once core 0 was released";
    rig.Stop();
    d0.SetAfterRowIdFixedForTest({});

    EXPECT_TRUE(StartsWith(s0.out.response, "INSERTED")) << s0.out.response;
    raced.second_reply = s1.out.response;
    raced.seam_calls = seam.calls.load(std::memory_order_relaxed);
    return raced;
}

std::unique_ptr<TwoCoreRig> OpenRigWithRelation() {
    auto opened = TwoCoreRig::Open();
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    if (!opened.ok()) return nullptr;
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    EXPECT_TRUE(StartsWith(
        rig->core(0).dispatcher().Dispatch("CREATE TABLE t (id int64, v int64) BTREE").response,
        "CREATED"));
    // Core 1 carves its transaction-id window here, not in the race: a carve
    // persists page 0 through a full flush, which would put a durable sync in
    // core 1's bounded look and, once the seam sits under the leaf, block
    // core 1 on that leaf inside the flush instead of in its descent.
    Session warm;
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    EXPECT_TRUE(StartsWith(d1.Dispatch("BEGIN", &warm).response, "BEGIN"));
    EXPECT_TRUE(StartsWith(d1.Dispatch("ROLLBACK", &warm).response, "ROLLBACK"));
    return rig;
}

TEST(IssueUnderTheLeafRig, AnIdIssuedFirstIsPlacedBelowALaterOne) {
    // Omitted tuple key against omitted tuple key (BB §1.2): core 0 is issued the first
    // id and stops; core 1 is issued the next. Placement order must be issue
    // order, observed three ways: the bare walk (slot order), `ORDER BY id`
    // (which the compiler discards on the premise that a walk already emits
    // it), and `ORDER BY id LIMIT 1` - where a misordered leaf answers a
    // different row, not only a different order.
    std::unique_ptr<TwoCoreRig> rig = OpenRigWithRelation();
    ASSERT_NE(rig, nullptr);
    const Raced raced =
        RaceTwoInserts(*rig, "INSERT INTO t VALUES (10)", "INSERT INTO t VALUES (20)");
    ASSERT_TRUE(StartsWith(raced.second_reply, "INSERTED")) << raced.second_reply;
    ASSERT_EQ(raced.first_id, 1u);
    EXPECT_TRUE(raced.second_finished_alone)
        << "core 1 waited on core 0's stop: the issue is under a hold again";

    CommandDispatcher& d0 = rig->core(0).dispatcher();
    EXPECT_EQ(d0.Dispatch("SELECT id, v FROM t").response, "id,v\\n1,10\\n2,20")
        << "the leaf holds the later id in the lower slot";
    EXPECT_EQ(d0.Dispatch("SELECT id FROM t ORDER BY id").response, "id\\n1\\n2");
    EXPECT_EQ(d0.Dispatch("SELECT id, v FROM t ORDER BY id LIMIT 1").response, "id,v\\n1,10");
}

TEST(IssueUnderTheLeafRig, ANamedKeyAndALaterIssuedIdLandInKeyOrder) {
    // BB §1.3's path, by placement (BD-R6): core 0 names 5 and stops,
    // borrowed and encoded but not yet admitted; core 1 omits its tuple key, is
    // issued 1 - the mark has not moved - and places it at once. Core 0 then
    // admits 5 and places it above. The walk is key order whatever order the
    // two placed in.
    std::unique_ptr<TwoCoreRig> rig = OpenRigWithRelation();
    ASSERT_NE(rig, nullptr);
    const Raced raced =
        RaceTwoInserts(*rig, "INSERT INTO t VALUES (5, 50)", "INSERT INTO t VALUES (60)");
    ASSERT_TRUE(StartsWith(raced.second_reply, "INSERTED")) << raced.second_reply;
    ASSERT_EQ(raced.first_id, 5u);
    EXPECT_TRUE(raced.second_finished_alone) << "core 1 waited on core 0's stop";
    EXPECT_NE(raced.second_reply.find(" id=1 "), std::string::npos) << raced.second_reply;

    CommandDispatcher& d0 = rig->core(0).dispatcher();
    EXPECT_EQ(d0.Dispatch("SELECT id, v FROM t").response, "id,v\\n1,60\\n5,50");
    EXPECT_EQ(d0.Dispatch("SELECT id, v FROM t ORDER BY id").response, "id,v\\n1,60\\n5,50");
    EXPECT_NE(d0.Dispatch("INSERT INTO t VALUES (70)").response.find(" id=6 "), std::string::npos);
}

TEST(IssueUnderTheLeafRig, AnIssuedIdANamedKeyPlacedFirstIsDrawnAgain) {
    // The one collision BD-R6 leaves (BD-S3's review), driven step by step:
    // core 0 names 1, the mark, and stops borrowed and not admitted; core 1
    // omits its tuple key, is issued the same 1 - the mark has not moved - and stops
    // before its borrow; core 0 admits, places 1 and commits; core 1's borrow
    // of 1 is then granted and its placement finds 1 present - which is no
    // reason an omitted tuple key is refused for. It burns 1 and is issued 2.
    std::unique_ptr<TwoCoreRig> rig = OpenRigWithRelation();
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    Seam named;
    Seam issued;
    d0.SetAfterRowIdFixedForTest(std::ref(named));
    d1.SetAfterRowIdIssuedForTest(std::ref(issued));
    OneStatement s0;
    OneStatement s1;
    s0.sql = "INSERT INTO t VALUES (1, 10)";
    s1.sql = "INSERT INTO t VALUES (20)";
    s0.session.set_durability(wal::DurabilityClass::kRelaxed);
    s1.session.set_durability(wal::DurabilityClass::kRelaxed);
    rig->core(0).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, server::Run(d0, s0)));
    rig->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, server::Run(d1, s1)));
    rig->Start();

    s0.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return named.stopped_at.load() != 0; }));
    s1.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return issued.stopped_at.load() != 0; }));
    EXPECT_EQ(issued.stopped_at.load(), 1u) << "core 1 was not issued the id core 0 named";

    named.release.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return s0.done.load(); }, 10000ms));
    issued.release.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(
        *rig, 1,
        [&] {
            rig->wakers().Kick(0);
            return s1.done.load();
        },
        10000ms));
    rig->Stop();
    d0.SetAfterRowIdFixedForTest({});
    d1.SetAfterRowIdIssuedForTest({});

    EXPECT_TRUE(StartsWith(s0.out.response, "INSERTED")) << s0.out.response;
    ASSERT_TRUE(StartsWith(s1.out.response, "INSERTED"))
        << "an omitted tuple key was refused for an id a named key placed: " << s1.out.response;
    EXPECT_NE(s1.out.response.find(" id=2 "), std::string::npos) << s1.out.response;
    EXPECT_EQ(issued.calls.load(), 2) << "the collision never happened";
    EXPECT_EQ(d0.Dispatch("SELECT id, v FROM t").response, "id,v\\n1,10\\n2,20");
}

}  // namespace
}  // namespace kds::server
