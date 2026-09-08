// AO-S5(b): the cross-core foreign-key wait, on the two-core rig
// (`instructions/v3.0.0/workorder-ao-m2-lock-family.md`, the AO-S5(b)
// section). A child on core 1 whose parent row is being written by an
// in-flight transaction on core 0 used to be refused `TxnConflict` by the
// parent's busy answer; now the parent's core parks the probe until the
// writer decides and answers from a fresh view. The same shape as AO-S3's
// same-core cells (`Txn2pcBlockedWriterTest`), with the parent on the
// other core. The last two cells are the stage's C1 and C2: a transaction
// asking about a row its own participant holds, and a decide that arrives
// while its probe is parked.

#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/session.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

// Core 0's session: opens a transaction, inserts the parent row, holds it
// until told how to end - and, when a cell asks for one, runs one more
// statement straight after (C2 reads the parent row's fate through it).
struct ParentWriter {
    Session session;
    DispatchOutcome begin_out;
    DispatchOutcome insert_out;
    DispatchOutcome end_out;
    DispatchOutcome after_out;
    std::function<bool()> end_pred;
    std::atomic<bool> holding{false};
    std::atomic<bool> may_end{false};
    std::atomic<bool> ended{false};
    std::atomic<bool> after_done{false};
    std::string ending = "COMMIT";
    std::string after;
};

sched::Coro WriteParent(CommandDispatcher& d, ParentWriter& p) {
    co_await d.DispatchAsync("BEGIN", &p.session, &p.begin_out);
    co_await d.DispatchAsync("INSERT INTO p VALUES (7, 0)", &p.session, &p.insert_out);
    p.holding.store(true, std::memory_order_release);
    p.end_pred = [&p] { return p.may_end.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&p.end_pred};
    co_await d.DispatchAsync(p.ending, &p.session, &p.end_out);
    p.ended.store(true, std::memory_order_release);
    if (!p.after.empty()) {
        co_await d.DispatchAsync(p.after, &p.session, &p.after_out);
        p.after_done.store(true, std::memory_order_release);
    }
    co_return Status::OK();
}

// Core 1's session: an autocommit child insert referencing that parent,
// started only once the parent is held.
struct ChildWriter {
    Session session;
    DispatchOutcome out;
    std::function<bool()> go_pred;
    std::atomic<bool> go{false};
    std::atomic<bool> done{false};
};

sched::Coro WriteChild(CommandDispatcher& d, ChildWriter& c) {
    c.go_pred = [&c] { return c.go.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&c.go_pred};
    co_await d.DispatchAsync("INSERT INTO c VALUES (7)", &c.session, &c.out);
    c.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

// Core 1's session writing its own parent (C1): one transaction that ships
// the parent `INSERT` to core 0 - enrolling a participant there - and then
// writes the child at home, so the row the probe asks core 0 about is held
// by the requester's own participant.
struct OwnParentWriter {
    Session session;
    DispatchOutcome begin_out;
    DispatchOutcome parent_out;
    DispatchOutcome child_out;
    DispatchOutcome end_out;
    std::function<bool()> end_pred;
    std::atomic<bool> child_done{false};
    std::atomic<bool> may_end{false};
    std::atomic<bool> ended{false};
};

sched::Coro WriteOwnParentThenChild(CommandDispatcher& d, OwnParentWriter& w) {
    co_await d.DispatchAsync("BEGIN", &w.session, &w.begin_out);
    co_await d.DispatchAsync("INSERT INTO p VALUES (7, 0)", &w.session, &w.parent_out);
    co_await d.DispatchAsync("INSERT INTO c VALUES (7)", &w.session, &w.child_out);
    w.child_done.store(true, std::memory_order_release);
    w.end_pred = [&w] { return w.may_end.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&w.end_pred};
    co_await d.DispatchAsync("COMMIT", &w.session, &w.end_out);
    w.ended.store(true, std::memory_order_release);
    co_return Status::OK();
}

// A task that holds core 0's reactor for `length` once told to (C2). The
// message drain runs between tasks, so a probe that lands during the hold
// is drained after it, and the park it opens is stamped that much later
// than the child's waiter - the transit-plus-drain lag the defect needs,
// made wide enough to land a decide in.
struct Stall {
    std::function<bool()> go_pred;
    std::atomic<bool> go{false};
    std::atomic<bool> begun{false};
    std::chrono::milliseconds length{0};
};

sched::Coro HoldTheReactor(Stall& s) {
    s.go_pred = [&s] { return s.go.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&s.go_pred};
    s.begun.store(true, std::memory_order_release);
    std::this_thread::sleep_for(s.length);
    co_return Status::OK();
}

struct FkRig {
    explicit FkRig(TwoCoreRig::Options options) {
        auto opened = TwoCoreRig::Open(options);
        EXPECT_TRUE(opened.ok()) << opened.status().message();
        if (!opened.ok()) return;
        rig = std::move(opened.value());
    }
    ~FkRig() {
        if (rig != nullptr) rig->Stop();
    }

    static Status Expect(const char* what, const std::string& response, const char* prefix) {
        if (response.rfind(prefix, 0) == 0) return Status::OK();
        return Status::InvalidArgument(std::string(what) + ": " + response);
    }

    // The parent on core 0 under `kCreatingCore`, the child on core 1 under
    // `kRotate` - `core_runtime_test.cpp`'s `OpenCrossOwnerFkPair` recipe -
    // and the child funded with a row-id block, since its key is issued.
    Status Seed() {
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        rig->core(0).catalog().SetPlacementPolicy(catalog::PlacementPolicy::kCreatingCore);
        if (Status s = Expect("create p", d0.Dispatch("CREATE TABLE p (id int64, v int64) BTREE").response,
                              "CRE");
            !s.ok()) {
            return s;
        }
        rig->core(0).catalog().SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
        if (Status s = Expect("create c",
                              d0.Dispatch("CREATE TABLE c (id int64, pid int64 REFERENCES p) BTREE")
                                  .response,
                              "CRE");
            !s.ok()) {
            return s;
        }
        auto child = rig->core(0).catalog().FindTableOidByName("c");
        if (!child.ok()) return child.status();
        auto row = rig->core(0).catalog().GetSysTableRow(child.value());
        if (!row.ok()) return row.status();
        if (row.value().owner_core != 1) {
            return Status::InvalidArgument("c was placed on core " +
                                           std::to_string(row.value().owner_core) + ", not 1");
        }
        if (Status s = rig->store().FlushPages(catalog::kEveryCatalogPage); !s.ok()) return s;
        rig->core(1).InvalidateCatalog();
        return rig->FundPeerRelation(child.value());
    }

    // The parent's writer on core 0 and the child's on core 1; with a
    // nonzero `stall_core0`, the hold as well, on core 0 ahead of both.
    void Submit(const char* ending, std::chrono::milliseconds stall_core0 = 0ms) {
        parent.ending = ending;
        if (stall_core0 > 0ms) {
            stall.length = stall_core0;
            rig->core(0).scheduler().Submit(
                sched::MakeCoroTask(sched::SchedulingGroup::kForeground, HoldTheReactor(stall)));
        }
        rig->core(0).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, WriteParent(rig->core(0).dispatcher(), parent)));
        rig->core(1).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, WriteChild(rig->core(1).dispatcher(), child)));
        rig->Start();
    }

    // C1's shape: one session on core 1 does everything.
    void SubmitOwn() {
        rig->core(1).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground,
            WriteOwnParentThenChild(rig->core(1).dispatcher(), own)));
        rig->Start();
    }

    FkProbeServer& core0_probes() { return *rig->core(0).fk_probe_server(); }

    // The writers before the rig: coroutine frames the reactors hold borrow
    // them, and reverse destruction tears the frames down first.
    ParentWriter parent;
    ChildWriter child;
    OwnParentWriter own;
    Stall stall;
    std::unique_ptr<TwoCoreRig> rig;
};

// Production's system tick, on for the cells whose waits a kick from the
// rig thread does not cover: a participant's prepare parks on its device
// sync and is polled, and the rig's default - the tick off, the idle block
// five seconds - turns that poll into a whole block. `KickUntil` kicks the
// one core it watches; the tick is what wakes the other.
TwoCoreRig::Options Ticking() {
    TwoCoreRig::Options options;
    options.wal_drain_interval_ns = 1'000'000;  // 1 ms
    return options;
}

// The parent held, the child started, and the child parked on the probe:
// what the first two cells begin from. Returns whether it was reached.
bool ParkTheChild(FkRig& r) {
    if (!KickUntil(*r.rig, 0, [&] { return r.parent.holding.load(std::memory_order_acquire); })) {
        return false;
    }
    if (r.parent.insert_out.response.rfind("INSERTED", 0) != 0) return false;
    r.child.go.store(true, std::memory_order_release);
    // The child ships a probe; the parent's core parks it on the holder.
    // A busy answer instead would refuse the child at once, which the
    // wait below would see as `done`.
    return KickUntil(*r.rig, 1, [&] {
        return r.rig->core(0).fk_probe_server() != nullptr &&
               r.rig->core(0).fk_probe_server()->probe_waits() >= 1;
    });
}

TEST(FkProbeRigTest, AChildOnCore1WaitsOutAnInFlightParentOnCore0AndPassesWhenItCommits) {
    FkRig r({});
    ASSERT_NE(r.rig, nullptr);
    if (Status seeded = r.Seed(); !seeded.ok()) FAIL() << seeded.message();
    r.Submit("COMMIT");
    ASSERT_TRUE(ParkTheChild(r)) << "the parent's core did not park the probe: "
                                 << r.child.out.response;
    EXPECT_FALSE(r.child.done.load(std::memory_order_acquire))
        << "the child was refused instead of waiting: " << r.child.out.response;

    r.parent.may_end.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*r.rig, 0, [&] { return r.parent.ended.load(std::memory_order_acquire); }))
        << r.parent.end_out.response;
    EXPECT_EQ(r.parent.end_out.response.rfind("COMMIT", 0), 0u) << r.parent.end_out.response;
    // The decide ends the parent core's wait; the probe answers from a fresh
    // view and the child's insert completes - well inside the probe's 5 s
    // deadline, which is the only other thing that could end this.
    EXPECT_TRUE(KickUntil(*r.rig, 1, [&] { return r.child.done.load(std::memory_order_acquire); },
                          4000ms))
        << "the child never proceeded after the parent committed";
    EXPECT_EQ(r.child.out.response.rfind("INSERTED", 0), 0u) << r.child.out.response;
    EXPECT_EQ(r.core0_probes().probe_wait_expiries(), 0u)
        << "the wait ended by the deadline rather than by the decide";
}

TEST(FkProbeRigTest, AChildWaitingOnAParentThatRollsBackAcrossCoresIsAViolation) {
    FkRig r({});
    ASSERT_NE(r.rig, nullptr);
    if (Status seeded = r.Seed(); !seeded.ok()) FAIL() << seeded.message();
    r.Submit("ROLLBACK");
    ASSERT_TRUE(ParkTheChild(r)) << r.child.out.response;
    EXPECT_FALSE(r.child.done.load(std::memory_order_acquire)) << r.child.out.response;

    r.parent.may_end.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*r.rig, 0, [&] { return r.parent.ended.load(std::memory_order_acquire); }));
    EXPECT_TRUE(KickUntil(*r.rig, 1, [&] { return r.child.done.load(std::memory_order_acquire); },
                          4000ms))
        << "the child never proceeded after the parent rolled back";
    const Status refused = StatusFromErrorReply(r.child.out.response);
    EXPECT_EQ(refused.code(), StatusCode::kFkViolation) << r.child.out.response;
}

// C1. The parent row core 0 is asked about was written by the requester's
// own participant there, so the probe's check view carries that
// participant as its writer and the row is the transaction's own pending
// image: pass at once, no park. **The mutation**: mint the view with no
// writer and the probe parks on the participant, which decides only at
// the `COMMIT` this statement stands ahead of - the child's insert then
// takes the whole 5 s deadline and is refused `TxnConflict`, and this cell
// fails at "the child waited on its own participant".
TEST(FkProbeRigTest, ATransactionWritingItsOwnParentAcrossCoresPassesItsChildWithoutAPark) {
    FkRig r(Ticking());
    ASSERT_NE(r.rig, nullptr);
    if (Status seeded = r.Seed(); !seeded.ok()) FAIL() << seeded.message();
    r.SubmitOwn();
    ASSERT_TRUE(KickUntil(*r.rig, 1, [&] { return r.own.child_done.load(std::memory_order_acquire); },
                          3000ms))
        << "the child waited on its own participant: " << r.own.child_out.response;
    EXPECT_EQ(r.own.parent_out.response.rfind("INSERTED", 0), 0u) << r.own.parent_out.response;
    EXPECT_EQ(r.own.child_out.response.rfind("INSERTED", 0), 0u) << r.own.child_out.response;
    EXPECT_EQ(r.core0_probes().probe_waits(), 0u)
        << "the probe parked on the requester's own participant";

    r.own.may_end.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*r.rig, 1, [&] { return r.own.ended.load(std::memory_order_acquire); },
                          4000ms))
        << "COMMIT did not return: [" << r.own.end_out.response << "]";
    EXPECT_EQ(r.own.end_out.response.rfind("COMMIT", 0), 0u) << r.own.end_out.response;
}

// C2. The park's deadline is stamped at the drain and the child's at the
// send, so a probe drained late parks past the child's waiter. The child
// gives up first, is refused retryably and - autocommit - decides at once;
// the decide reaches core 0 while the park is still up and abandons it.
// The parent then commits inside the park's own deadline, and the row it
// wrote is deletable afterwards. **The mutation**: let the park re-answer
// after the decide and it grants an intent to a holder already released -
// nothing ever releases it, and the `DELETE` is refused `TxnConflict` for
// the life of the process.
TEST(FkProbeRigTest, ADecideThatArrivesDuringAParkAbandonsItAndStrandsNoIntent) {
    FkRig r(Ticking());
    ASSERT_NE(r.rig, nullptr);
    if (Status seeded = r.Seed(); !seeded.ok()) FAIL() << seeded.message();
    r.parent.after = "DELETE FROM p WHERE id = 7";
    r.Submit("COMMIT", /*stall_core0=*/1000ms);
    ASSERT_TRUE(KickUntil(*r.rig, 0, [&] { return r.parent.holding.load(std::memory_order_acquire); }));
    ASSERT_EQ(r.parent.insert_out.response.rfind("INSERTED", 0), 0u) << r.parent.insert_out.response;

    // Hold core 0, then let the child send: its probe lands during the hold
    // and is drained after it, so the park is stamped about a second after
    // the child's waiter.
    r.stall.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*r.rig, 0, [&] { return r.stall.begun.load(std::memory_order_acquire); }));
    r.child.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*r.rig, 1, [&] { return r.core0_probes().probe_waits() >= 1; }, 4000ms))
        << "the parent's core did not park the probe: " << r.child.out.response;

    // The child's waiter expires first: refused retryably, and the abort
    // decide is sent before the statement returns.
    ASSERT_TRUE(KickUntil(*r.rig, 1, [&] { return r.child.done.load(std::memory_order_acquire); },
                          7000ms))
        << "the child never gave up";
    EXPECT_EQ(StatusFromErrorReply(r.child.out.response).code(), StatusCode::kTxnConflict)
        << r.child.out.response;
    ASSERT_TRUE(KickUntil(*r.rig, 0, [&] { return r.core0_probes().probe_wait_abandoned() >= 1; },
                          3000ms))
        << "the decide did not reach the park: child=[" << r.child.out.response
        << "] waits=" << r.core0_probes().probe_waits()
        << " expiries=" << r.core0_probes().probe_wait_expiries();

    // The holder decides inside the park's own deadline, and its session
    // deletes the row straight after. An abandoned park answers nothing; a
    // park that re-answered here would pass and grant, and the DELETE would
    // meet the intent - which is what a stranded one denies.
    r.parent.may_end.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*r.rig, 0, [&] { return r.parent.after_done.load(std::memory_order_acquire); },
                          4000ms))
        << "the COMMIT or the DELETE never completed: " << r.parent.end_out.response;
    EXPECT_EQ(r.parent.end_out.response.rfind("COMMIT", 0), 0u) << r.parent.end_out.response;
    EXPECT_EQ(r.parent.after_out.response.rfind("DELETED 1", 0), 0u) << r.parent.after_out.response;
    EXPECT_EQ(r.core0_probes().probe_wait_expiries(), 0u)
        << "the park ran to its deadline rather than ending at the decide";
}

}  // namespace
}  // namespace kds::server
