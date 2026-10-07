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
    int seam_calls = 0;  // the seam runs once per row, or once per sorted fill
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


// ---- BB-S4: the heap arm (BB-R7) -------------------------------------------
//
// The same races on a heap relation, which only a volume from before SUS-1
// can hold (the test binary lifts the suspension, `heap_suspension_env.cpp`).
// The tail held as the tail stands for the btree's rightmost leaf: an issued
// id, a named key's admission and the sorted fill's carve all happen under
// its exclusive hold, so placement order is issue order across pages and
// within each.

std::unique_ptr<TwoCoreRig> OpenRigWithHeap(const char* create) {
    auto opened = TwoCoreRig::Open();
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    if (!opened.ok()) return nullptr;
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    EXPECT_TRUE(StartsWith(rig->core(0).dispatcher().Dispatch(create).response, "CREATED"));
    Session warm;
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    EXPECT_TRUE(StartsWith(d1.Dispatch("BEGIN", &warm).response, "BEGIN"));
    EXPECT_TRUE(StartsWith(d1.Dispatch("ROLLBACK", &warm).response, "ROLLBACK"));
    return rig;
}

// The page an `INSERTED ... page=<p> slot=<s>` reply names.
std::string PageOf(const std::string& reply) {
    const std::size_t at = reply.find(" page=");
    if (at == std::string::npos) return {};
    const std::size_t end = reply.find(' ', at + 6);
    return reply.substr(at + 6, end == std::string::npos ? std::string::npos : end - at - 6);
}

TEST(IssueUnderTheLeafRig, OnAHeapAnIdIssuedFirstIsPlacedBelowALaterOne) {
    std::unique_ptr<TwoCoreRig> rig = OpenRigWithHeap("CREATE TABLE h (id int64, v int64) HEAP");
    ASSERT_NE(rig, nullptr);
    const Raced raced =
        RaceTwoInserts(*rig, "INSERT INTO h VALUES (10)", "INSERT INTO h VALUES (20)");
    ASSERT_TRUE(StartsWith(raced.second_reply, "INSERTED")) << raced.second_reply;
    ASSERT_EQ(raced.first_id, 1u);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    EXPECT_EQ(d0.Dispatch("SELECT id, v FROM h").response, "id,v\\n1,10\\n2,20")
        << "the tail holds the later id in the lower slot";
    EXPECT_EQ(d0.Dispatch("SELECT id, v FROM h ORDER BY id LIMIT 1").response, "id,v\\n1,10");
}

TEST(IssueUnderTheLeafRig, OnAHeapARaceAcrossAFullTailIsNotRefused) {
    // A `varchar(4000)` cell makes a row of about 4 KB, so a page holds two
    // and the tail is full before the race. Until BB-S4 core 1 grew a page
    // with `min_key` 4 and placed there first, and core 0's 3 was then below
    // the tail's `min_key` and refused `OutOfRange` - the tail-boundary
    // refusal `heap-and-tuple.md` §4.1a stated. Under the tail's hold core 0
    // grows the page itself, with `min_key` 3, and core 1's 4 follows it.
    std::unique_ptr<TwoCoreRig> rig =
        OpenRigWithHeap("CREATE TABLE h (id int64, s varchar(4000)) HEAP");
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    const std::string first = d0.Dispatch("INSERT INTO h VALUES ('a')").response;
    const std::string second = d0.Dispatch("INSERT INTO h VALUES ('b')").response;
    ASSERT_TRUE(StartsWith(second, "INSERTED")) << second;
    ASSERT_EQ(PageOf(first), PageOf(second)) << "the fixture needs both rows on one page";

    // Core 0's reply is checked inside the race: until BB-S4 it was the
    // refusal.
    const Raced raced =
        RaceTwoInserts(*rig, "INSERT INTO h VALUES ('c')", "INSERT INTO h VALUES ('d')");
    ASSERT_TRUE(StartsWith(raced.second_reply, "INSERTED")) << raced.second_reply;
    ASSERT_EQ(raced.first_id, 3u);
    EXPECT_NE(PageOf(raced.second_reply), PageOf(second)) << "the race never crossed a page";
    EXPECT_EQ(d0.Dispatch("SELECT id, s FROM h").response, "id,s\\n1,a\\n2,b\\n3,c\\n4,d");
}

TEST(IssueUnderTheLeafRig, OnAHeapANamedKeyAtTheMarkIsPlacedBelowALaterIssuedId) {
    std::unique_ptr<TwoCoreRig> rig = OpenRigWithHeap("CREATE TABLE h (id int64, v int64) HEAP");
    ASSERT_NE(rig, nullptr);
    const Raced raced =
        RaceTwoInserts(*rig, "INSERT INTO h VALUES (5, 50)", "INSERT INTO h VALUES (60)");
    ASSERT_TRUE(StartsWith(raced.second_reply, "INSERTED")) << raced.second_reply;
    ASSERT_EQ(raced.first_id, 5u);
    EXPECT_EQ(rig->core(0).dispatcher().Dispatch("SELECT id, v FROM h").response,
              "id,v\\n5,50\\n6,60");
}

TEST(IssueUnderTheLeafRig, OnAHeapASortedFillIsPlacedBelowALaterIssuedId) {
    // The sorted fill (bulkinsert.md T3-2: every row omits its pk, a heap
    // with no var-heap, index, Cabin or assertion) carves its block under the
    // hold of the tail it starts from (BB-R7); the seam runs once per fill,
    // with the block's first id, after the carve, the borrow and the encode.
    std::unique_ptr<TwoCoreRig> rig = OpenRigWithHeap("CREATE TABLE h (id int64, v int64) HEAP");
    ASSERT_NE(rig, nullptr);
    const Raced raced = RaceTwoInserts(*rig, "INSERT INTO h VALUES (10), (20), (30)",
                                       "INSERT INTO h VALUES (40)");
    ASSERT_TRUE(StartsWith(raced.second_reply, "INSERTED")) << raced.second_reply;
    ASSERT_EQ(raced.first_id, 1u);
    EXPECT_EQ(raced.seam_calls, 1) << "the per-row path ran, not the sorted fill";
    EXPECT_EQ(rig->core(0).dispatcher().Dispatch("SELECT id, v FROM h").response,
              "id,v\\n1,10\\n2,20\\n3,30\\n4,40");
}

}  // namespace
}  // namespace kds::server
