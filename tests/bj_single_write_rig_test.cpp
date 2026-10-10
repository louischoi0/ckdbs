// **A qualifying row is written at most once per statement, on every path**
// (BJ-R9, `instructions/v3.0.0/workorder-bj-expression-update.md` §1.5).
//
// A literal `SET` is idempotent, so a path that applied it twice to one row
// has always been invisible. `SET v = v + 1`, which BJ-S4 admits, is not.
// Three facts keep a row single-written today - the walk never revisits a
// row it wrote, a park resumes past what it wrote, and a whole re-run
// happens only for a statement that wrote nothing - and none of them had a
// cell that could see a second application. This file is those cells: the
// dispatcher's `SetAfterUpdateRowAppliedForTest` seam counts the writes per
// pk, so the count is read directly and does not depend on a `SET` value.

#include "two_core_rig.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/session.hpp"
#include "kds/wal/durability.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

bool StartsWith(const std::string& reply, std::string_view prefix) {
    return reply.rfind(prefix, 0) == 0;
}

// The applications per pk, written from a reactor and read from the test
// thread once the statement is done.
struct Applied {
    std::mutex mu;
    std::map<std::uint64_t, int> count;

    void Attach(CommandDispatcher& d) {
        d.SetAfterUpdateRowAppliedForTest([this](std::uint64_t pk) {
            std::lock_guard<std::mutex> lock(mu);
            ++count[pk];
        });
    }
    int Of(std::uint64_t pk) {
        std::lock_guard<std::mutex> lock(mu);
        const auto it = count.find(pk);
        return it == count.end() ? 0 : it->second;
    }
    int Max() {
        std::lock_guard<std::mutex> lock(mu);
        int m = 0;
        for (const auto& [pk, n] : count) m = std::max(m, n);
        return m;
    }
};

struct Statement {
    Session* session = nullptr;
    std::string line;
    DispatchOutcome out;
    std::function<bool()> go_pred;
    std::atomic<bool> go{true};
    std::atomic<bool> done{false};
};

sched::Coro RunStatement(CommandDispatcher& d, Statement& s) {
    s.go_pred = [&s] { return s.go.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&s.go_pred};
    co_await d.DispatchAsync(s.line, s.session, &s.out);
    s.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

std::unique_ptr<TwoCoreRig> OpenRig() {
    TwoCoreRig::Options options;
    options.wake.min_delay_ticks = 2;
    options.wake.max_delay_ticks = 2;
    auto opened = TwoCoreRig::Open(options);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    if (!opened.ok()) return nullptr;
    return std::move(opened.value());
}

// `t(id, v)` with rows 1..n, all `v = 0`.
void FillT(CommandDispatcher& d, int n) {
    ASSERT_TRUE(StartsWith(d.Dispatch("CREATE TABLE t (id int64, v int64)").response, "CREATED"));
    for (int i = 1; i <= n; ++i) {
        const std::string out =
            d.Dispatch("INSERT INTO t VALUES (" + std::to_string(i) + ", 0)").response;
        ASSERT_FALSE(StartsWith(out, "ERR")) << out;
    }
}

TEST(BjSingleWriteRigTest, AMultiRowWalkAppliesEveryRowExactlyOnce) {
    // The first of BJ-R9's three facts: an `UPDATE` overwrites in place and
    // never migrates a tuple, so the walk, advancing in key order, never
    // meets a row it already wrote. Enough rows to span several leaves, so
    // a walk that re-entered a leaf after a divide would count twice.
    auto rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    constexpr int kRows = 1200;
    FillT(d0, kRows);

    Applied applied;
    applied.Attach(d0);
    const std::string out = d0.Dispatch("UPDATE t SET v = 7 WHERE v >= 0").response;
    ASSERT_TRUE(StartsWith(out, "UPDATED " + std::to_string(kRows))) << out;
    for (int pk = 1; pk <= kRows; ++pk) {
        ASSERT_EQ(applied.Of(static_cast<std::uint64_t>(pk)), 1) << "row " << pk;
    }
}

// A holder on core 0 with `id = held` updated and undecided, and core 1's
// `UPDATE t SET v = 2 WHERE v >= 0` walking behind it. The predicate names
// no key, so no range is declared at the bind and the walk takes each row's
// borrow as it reaches it: it writes the rows before `held`, parks on
// `held`, and resumes at the holder's decide.
struct ParkedWalk {
    explicit ParkedWalk(int rows, int held_id, std::string_view decide)
        : rows(rows), held(held_id), decide_line(decide) {}

    // Whether the waiter finished; its response is in `update.out`.
    bool Run() {
        rig = OpenRig();
        if (rig == nullptr) return false;
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CommandDispatcher& d1 = rig->core(1).dispatcher();
        FillT(d0, rows);
        if (::testing::Test::HasFatalFailure()) return false;

        if (!StartsWith(d0.Dispatch("BEGIN", &holder).response, "BEGIN")) return false;
        const std::string held_out =
            d0.Dispatch("UPDATE t SET v = 1 WHERE id = " + std::to_string(held), &holder).response;
        if (StartsWith(held_out, "ERR")) return false;

        applied.Attach(d1);
        // `relaxed`, so the resumed statement commits without parking on its
        // sync: this rig holds every kick in the sim, and since BA-S7 a
        // parked committer is woken by the writer's kick. The wake under
        // test is the holder's release, as in `row_wait_wake_rig_test.cpp`.
        waiter.set_durability(wal::DurabilityClass::kRelaxed);
        update.session = &waiter;
        update.line = "UPDATE t SET v = 2 WHERE v >= 0";
        decide.session = &holder;
        decide.line = decide_line;
        decide.go.store(false, std::memory_order_release);
        rig->core(1).scheduler().Submit(
            sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunStatement(d1, update)));
        rig->core(0).scheduler().Submit(
            sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunStatement(d0, decide)));
        rig->Start();

        sched::Scheduler& peer = rig->core(1).scheduler();
        if (!Within(2000ms, [&] { return peer.idle_blocks() >= 1; })) return false;
        parked_before_decide = !update.done.load(std::memory_order_acquire);
        for (int pk = 1; pk <= rows; ++pk) {
            parked_counts.push_back(applied.Of(static_cast<std::uint64_t>(pk)));
        }

        decide.go.store(true, std::memory_order_release);
        if (!KickUntil(*rig, 0, [&] { return decide.done.load(std::memory_order_acquire); })) {
            return false;
        }
        rig->wake().Advance(2);
        return Within(2000ms, [&] { return update.done.load(std::memory_order_acquire); });
    }

    ~ParkedWalk() {
        if (rig != nullptr) rig->Stop();
    }

    // The waiter's response, read only once its `done` is seen: a failed
    // `Run` leaves the statement live on core 1, writing `update.out`.
    std::string Seen() const {
        return update.done.load(std::memory_order_acquire) ? update.out.response
                                                            : "<still running>";
    }

    int rows;
    int held;
    std::string decide_line;
    std::unique_ptr<TwoCoreRig> rig;
    Session holder;
    Session waiter;
    Statement update;
    Statement decide;
    Applied applied;
    bool parked_before_decide = false;
    std::vector<int> parked_counts;  // per pk 1..rows, read while the walk was parked
};

TEST(BjSingleWriteRigTest, AWalkParkedMidwayResumesPastWhatItWroteAndWritesTheRestOnce) {
    // The second fact: `WalkCursor` carries the key the walk stopped at,
    // which it has not written, so the resume re-enters past every row it
    // did write. Rows 1 and 2 are written before the park on row 3; a
    // resume that began again at the leftmost row would count them twice.
    ParkedWalk w(/*rows=*/5, /*held_id=*/3, "ROLLBACK");
    ASSERT_TRUE(w.Run()) << "the parked statement did not finish; saw '" << w.Seen() << "'";
    EXPECT_TRUE(w.parked_before_decide) << "the walk did not park behind the holder";
    // Rows before the held one written once, the held one and the rest not yet.
    EXPECT_EQ(w.parked_counts, (std::vector<int>{1, 1, 0, 0, 0}));
    EXPECT_TRUE(StartsWith(w.update.out.response, "UPDATED 5")) << w.update.out.response;
    for (std::uint64_t pk = 1; pk <= 5; ++pk) {
        EXPECT_EQ(w.applied.Of(pk), 1) << "row " << pk << " was written "
                                       << w.applied.Of(pk) << " time(s)";
    }
}

TEST(BjSingleWriteRigTest, ARefusedResumeNeverWritesARowTwiceEither) {
    // The holder commits row 3, so the resumed walk, still under its old
    // view, meets a version it cannot see: the contract is a refusal, and
    // whatever it answers, no row may have been applied more than once.
    ParkedWalk w(/*rows=*/5, /*held_id=*/3, "COMMIT");
    ASSERT_TRUE(w.Run()) << "the parked statement did not finish; saw '" << w.Seen() << "'";
    EXPECT_TRUE(w.parked_before_decide) << "the walk did not park behind the holder";
    // The resumed walk, still under its old view, meets the committed
    // version it cannot see: a retryable refusal, and no row twice.
    EXPECT_TRUE(StartsWith(w.update.out.response, "ERR TXN_CONFLICT")) << w.update.out.response;
    EXPECT_EQ(w.applied.Of(1), 1);
    EXPECT_EQ(w.applied.Of(2), 1);
    EXPECT_EQ(w.applied.Of(3), 0);
    EXPECT_LE(w.applied.Max(), 1);
}

TEST(BjSingleWriteRigTest, AStatementThatWroteNothingBeforeItsParkRunsOnceAfterIt) {
    // The third fact: a whole re-run is for a statement that wrote nothing.
    // The holder has row 1, the first the walk reaches, so nothing is
    // written before the park; the statement then runs from the start, and
    // each row must still be applied exactly once.
    ParkedWalk w(/*rows=*/5, /*held_id=*/1, "ROLLBACK");
    ASSERT_TRUE(w.Run()) << "the parked statement did not finish; saw '" << w.Seen() << "'";
    EXPECT_TRUE(w.parked_before_decide) << "the walk did not park behind the holder";
    EXPECT_EQ(w.parked_counts, (std::vector<int>{0, 0, 0, 0, 0}))
        << "a row was written before the first one's park";
    EXPECT_TRUE(StartsWith(w.update.out.response, "UPDATED 5")) << w.update.out.response;
    for (std::uint64_t pk = 1; pk <= 5; ++pk) {
        EXPECT_EQ(w.applied.Of(pk), 1) << "row " << pk << " was written "
                                       << w.applied.Of(pk) << " time(s)";
    }
}

}  // namespace
}  // namespace kds::server
