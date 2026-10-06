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
// is released. Placement order must be issue order.
//
// **The cells must not hang** (BB-S2's row). Once BB-R1 holds, the seam sits
// inside the hold of the leaf the row lands on, so core 1's insert blocks on
// that leaf's latch and cannot finish while core 0 is stopped. Each cell
// therefore gives core 1 a bounded look, releases core 0 whatever it saw, and
// only then waits for both.

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

    void operator()(std::uint64_t id) {
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
};

Raced RaceTwoInserts(TwoCoreRig& rig, const std::string& first, const std::string& second) {
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
    KickUntil(rig, 1, [&] { return s1.done.load(std::memory_order_acquire); }, 1000ms);

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
    // Omitted pk against omitted pk (BB §1.2): core 0 is issued the first
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

    CommandDispatcher& d0 = rig->core(0).dispatcher();
    EXPECT_EQ(d0.Dispatch("SELECT id, v FROM t").response, "id,v\\n1,10\\n2,20")
        << "the leaf holds the later id in the lower slot";
    EXPECT_EQ(d0.Dispatch("SELECT id FROM t ORDER BY id").response, "id\\n1\\n2");
    EXPECT_EQ(d0.Dispatch("SELECT id, v FROM t ORDER BY id LIMIT 1").response, "id,v\\n1,10");
}

TEST(IssueUnderTheLeafRig, ANamedKeyAtTheMarkIsPlacedBelowALaterIssuedId) {
    // BB §1.3, the path neither the bug entry nor BA-R1b named: core 0 names
    // a key at the mark, which moves the mark past it, and stops; core 1
    // omits its pk and is issued the next id above. The named key must land
    // first.
    std::unique_ptr<TwoCoreRig> rig = OpenRigWithRelation();
    ASSERT_NE(rig, nullptr);
    const Raced raced =
        RaceTwoInserts(*rig, "INSERT INTO t VALUES (5, 50)", "INSERT INTO t VALUES (60)");
    ASSERT_TRUE(StartsWith(raced.second_reply, "INSERTED")) << raced.second_reply;
    ASSERT_EQ(raced.first_id, 5u);
    EXPECT_NE(raced.second_reply.find(" id=6 "), std::string::npos) << raced.second_reply;

    CommandDispatcher& d0 = rig->core(0).dispatcher();
    EXPECT_EQ(d0.Dispatch("SELECT id, v FROM t").response, "id,v\\n5,50\\n6,60")
        << "the leaf holds the later id in the lower slot";
    EXPECT_EQ(d0.Dispatch("SELECT id, v FROM t ORDER BY id").response, "id,v\\n5,50\\n6,60");
}

}  // namespace
}  // namespace kds::server
