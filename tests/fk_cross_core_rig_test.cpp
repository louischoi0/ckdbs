// The cross-core foreign-key wait, on the two-core rig. A child on core 1
// whose parent row is being written by an in-flight transaction on core 0
// was refused `TxnConflict` by the parent's busy answer until AO-S5(b),
// which parked a *probe* on the parent's core until the writer decided.
//
// **AT-S5f keeps the property and retires the mechanism.** The child
// descends the parent here - every core reads every page - meets its
// undecided writer, and asks the instance's lock table for the parent row:
// refused, it parks on the slot the holder's release flips from whichever
// core releases, and the statement runs again whole. So these cells pin
// the same two outcomes they always did, and the thing they watch for the
// park is the table's own waiter count on the parent row rather than a
// probe server's counter.
//
// **Five cells went with the probes** (AT-S5f), each named with the
// premise that went:
//   - `ADecideThatArrivesDuringAParkAbandonsItAndStrandsNoIntent` - AO-S5(b)
//     C2: a decide reaching a park on the parent's core before its own
//     re-answer could grant a reference intent nobody would release. No
//     park runs on the parent's core and no intent is granted.
//   - `AChildResumedFromItsProbeWaitsOutAFenceOnItsOwnCore`,
//     `...AlsoWaitsOutAFenceThatRollsBack` and
//     `AResumedChildInsideATransactionWaitsRatherThanBeingToldErrAndCommitting`
//     - AO-S6d item 16: a statement that parked on a probe and met a fence
//     on its *resume*. There is no resume; a child meeting a fence parks on
//     it from its first dispatch, which `Txn2pcBlockedWriterTest` and the
//     AO-S6d fence cells already pin.
//   - `AResumedWriteThatHasAlreadyWrittenRowsIsRefusedRatherThanParkedMidWalk`
//     - the item-16 review's B1, which withheld the mid-walk park from a
//     probe's resume. `resumed_from_fk_probe_` is gone: a re-run is a whole
//     statement and AO-S3b's own guard is what stands.

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

// A session that opens a transaction, runs one statement, holds what it
// took until told how to end - and, when a cell asks for one, runs one more
// statement straight after (C2 reads the parent row's fate through it).
//
// **Two things are held this way** and the only difference is the
// statement: core 0's parent `INSERT`, and - since AO-S6d - core 1's range
// fence over the child relation, declared by a `DELETE` whose second
// conjunct matches nothing so that the unit is taken and no row is written.
struct ParentWriter {
    Session session;
    DispatchOutcome begin_out;
    DispatchOutcome insert_out;
    DispatchOutcome end_out;
    DispatchOutcome after_out;
    std::function<bool()> end_pred;
    std::function<bool()> go_pred;
    // Ungated by default. A cell that has to seed rows on this core first
    // sets `gated` and releases it, which is the same `go` shape
    // `ChildWriter` has and for the same reason - a reactor runs its tasks
    // interleaved, so "submitted first" is not "ran first".
    std::atomic<bool> go{false};
    bool gated = false;
    std::atomic<bool> holding{false};
    std::atomic<bool> may_end{false};
    std::atomic<bool> ended{false};
    std::atomic<bool> after_done{false};
    std::string statement = "INSERT INTO p VALUES (7, 0)";
    std::string ending = "COMMIT";
    std::string after;
};

sched::Coro WriteParent(CommandDispatcher& d, ParentWriter& p) {
    if (p.gated) {
        p.go_pred = [&p] { return p.go.load(std::memory_order_acquire); };
        co_await sched::WaitUntil{&p.go_pred};
    }
    co_await d.DispatchAsync("BEGIN", &p.session, &p.begin_out);
    co_await d.DispatchAsync(p.statement, &p.session, &p.insert_out);
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

// Core 1's session: a child insert referencing that parent, started only
// once the parent is held. Autocommit by default; `in_txn` wraps it in
// `BEGIN`/`COMMIT`, which is where item 16's defect is `[quiet-wrong]`
// rather than a missing wait - a blocker recorded on a resume is what tells
// `EndWrite` to withhold the poison a failed statement owes, so a resume
// that records one and is never re-run leaves the client told `ERR` over a
// transaction that then commits. `inserted` is the insert's own end and
// `done` the session's, which is the whole reason that cell can tell the
// two apart.
struct ChildWriter {
    Session session;
    DispatchOutcome begin_out;
    DispatchOutcome out;
    DispatchOutcome commit_out;
    DispatchOutcome after_out;
    std::function<bool()> go_pred;
    std::atomic<bool> go{false};
    std::atomic<bool> inserted{false};
    std::atomic<bool> done{false};
    bool in_txn = false;
    std::string statement = "INSERT INTO c VALUES (7)";
    // One more statement after the `COMMIT`, when a cell asks for one. A
    // poisoned `COMMIT` refuses **without** rolling back, so a cell that
    // wants to show what ROLLBACK undoes has to issue it.
    std::string after;
};

sched::Coro WriteChild(CommandDispatcher& d, ChildWriter& c) {
    c.go_pred = [&c] { return c.go.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&c.go_pred};
    if (c.in_txn) co_await d.DispatchAsync("BEGIN", &c.session, &c.begin_out);
    co_await d.DispatchAsync(c.statement, &c.session, &c.out);
    c.inserted.store(true, std::memory_order_release);
    if (c.in_txn) co_await d.DispatchAsync("COMMIT", &c.session, &c.commit_out);
    if (!c.after.empty()) co_await d.DispatchAsync(c.after, &c.session, &c.after_out);
    c.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

// One statement on whichever core it is submitted to, for a cell that
// needs a write to happen *somewhere specific* rather than at a moment
// specific. `done` is the whole handshake.
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

    // The parent and the child, both created from core 0; no core owns
    // either since AT-S9, so "on core N" below names the session's core.
    // The peer is funded with a row-id block, since it writes the child.
    Status Seed() {
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        if (Status s = Expect("create p", d0.Dispatch("CREATE TABLE p (id int64, v int64) BTREE").response,
                              "CRE");
            !s.ok()) {
            return s;
        }
        if (Status s = Expect("create c",
                              d0.Dispatch("CREATE TABLE c (id int64, pid int64 REFERENCES p) BTREE")
                                  .response,
                              "CRE");
            !s.ok()) {
            return s;
        }
        auto parent_rel = rig->core(0).catalog().FindTableOidByName("p");
        if (!parent_rel.ok()) return parent_rel.status();
        parent_oid = parent_rel.value();
        auto child = rig->core(0).catalog().FindTableOidByName("c");
        if (!child.ok()) return child.status();
        if (Status s = rig->store().FlushPages(catalog::kEveryCatalogPage); !s.ok()) return s;
        return Status::OK();
    }

    // The parent's writer on core 0 and the child's on core 1.
    void Submit(const char* ending) {
        parent.ending = ending;
        rig->core(0).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, WriteParent(rig->core(0).dispatcher(), parent)));
        rig->core(1).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, WriteChild(rig->core(1).dispatcher(), child)));
        rig->Start();
    }

    // The unit the child parks on: the parent row itself, which its writer
    // holds in `X` (AO-S6a). Named rather than counted loosely, so a cell
    // that passes is a cell that watched the right entry.
    txn::LockKey parent_row() const { return txn::LockKey::Tuple(parent_oid, 7); }

    // The writers before the rig: coroutine frames the reactors hold borrow
    // them, and reverse destruction tears the frames down first.
    ParentWriter parent;
    ChildWriter child;
    catalog::Oid parent_oid = 0;
    OneShot one;
    std::unique_ptr<TwoCoreRig> rig;
};

// The parent held, the child started, and the child parked on the parent
// row: what both cells begin from. Returns whether it was reached.
bool ParkTheChild(FkRig& r) {
    if (!KickUntil(*r.rig, 0, [&] { return r.parent.holding.load(std::memory_order_acquire); })) {
        return false;
    }
    if (r.parent.insert_out.response.rfind("INSERTED", 0) != 0) return false;
    r.child.go.store(true, std::memory_order_release);
    // The child's own core descends the parent, meets its writer and parks
    // on the row's entry in the instance's table. A busy answer without a
    // wait would refuse the child at once, which the cells see as `done`.
    return KickUntil(*r.rig, 1,
                     [&] { return r.rig->locks().WaiterCount(r.parent_row()) >= 1; });
}

TEST(FkCrossCoreRigTest, AChildOnCore1WaitsOutAnInFlightParentOnCore0AndPassesWhenItCommits) {
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
    // The decide releases the parent row, which flips the slot the child is
    // parked on from core 0; the child runs again whole, descends a parent
    // that is now committed, and inserts - well inside the lock family's
    // fault net, which is the only other thing that could end this.
    EXPECT_TRUE(KickUntil(*r.rig, 1, [&] { return r.child.done.load(std::memory_order_acquire); },
                          4000ms))
        << "the child never proceeded after the parent committed";
    EXPECT_EQ(r.child.out.response.rfind("INSERTED", 0), 0u) << r.child.out.response;
    EXPECT_EQ(r.rig->locks().WaiterCount(r.parent_row()), 0u)
        << "the child's wake registration outlived the statement that made it";
}

TEST(FkCrossCoreRigTest, AChildWaitingOnAParentThatRollsBackAcrossCoresIsAViolation) {
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


// ---- D4's Cabin half: a per-core set may find a child and may not clear one
//
// `stats::CabinStore` is a dispatcher's own, and since AT-S5 a write runs
// where the **session** is rather than where the relation's owner is - so a
// child row written on core 1 files its Cabin entry into core 1's store and
// core 0's set never hears of it. The reverse check reads the deleting
// core's store, so a set observed on core 0 that has since drained used to
// answer an authoritative *"no children"* for a parent that has one:
// `foreign-keys.md` §1's one forbidden answer, from the structure that
// exists to give the opposite.
//
// AT-S5f makes a drained set fall through to the walk. The set may still
// **find** a child, which is the fast path F6 is for; it may not clear one
// until the store becomes the instance's at AT-S7 (AT-0 item 9).
//
// **The mutation**: restore the `kPass` return after the loop and this cell
// reads `DELETED 1` for a parent whose child is on the other core.
TEST(FkCrossCoreRigTest, ADrainedCabinSetDoesNotClearAParentAChildOnAnotherCoreReferences) {
    FkRig r({});
    ASSERT_NE(r.rig, nullptr);
    if (Status seeded = r.Seed(); !seeded.ok()) FAIL() << seeded.message();
    CommandDispatcher& d0 = r.rig->core(0).dispatcher();

    ASSERT_EQ(d0.Dispatch("INSERT INTO p VALUES (7, 0)").response.rfind("INSERTED", 0), 0u);
    ASSERT_EQ(d0.Dispatch("CREATE CABIN ON c(pid)").response.rfind("CRE", 0), 0u);
    // A child of 7 on core 0, and the read that banks core 0's set for the
    // value 7: a probe that finds nothing banks nothing, so the set has to
    // be made to drain rather than started empty.
    ASSERT_EQ(d0.Dispatch("INSERT INTO c VALUES (7)").response.rfind("INSERTED", 0), 0u);
    ASSERT_EQ(d0.Dispatch("SELECT id FROM c WHERE pid = 7").response, "id\\n1");
    // The row goes; the set keeps its pk, because maintenance is
    // append-only and the re-check is what subtracts it.
    ASSERT_EQ(d0.Dispatch("DELETE FROM c WHERE pid = 7").response, "DELETED 1");

    // **And a new child of 7, written on core 1.** Its entry is filed into
    // core 1's store - the one core 0's check does not read.
    r.one.statement = "INSERT INTO c VALUES (7)";
    r.rig->core(1).scheduler().Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground, RunOne(r.rig->core(1).dispatcher(), r.one)));
    r.rig->Start();
    ASSERT_TRUE(KickUntil(*r.rig, 1, [&] { return r.one.done.load(std::memory_order_acquire); },
                          4000ms))
        << r.one.out.response;
    ASSERT_EQ(r.one.out.response.rfind("INSERTED", 0), 0u) << r.one.out.response;

    // Core 0's set for 7 now drains - its one entry is the deleted row -
    // and the walk is what answers.
    const std::string del = d0.Dispatch("DELETE FROM p WHERE id = 7").response;
    EXPECT_NE(del.find("FK_VIOLATION"), std::string::npos) << del;
}

}  // namespace
}  // namespace kds::server
