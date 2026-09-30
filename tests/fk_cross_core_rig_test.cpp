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
//     it from its first dispatch, as any insert does -
//     `lock_family_test.cpp`'s `AnInsertIntoAFencedWindowWaitsForItsHolder`
//     pins the insert's, on a relation with no foreign key, and no cell
//     pins it with one. `Txn2pcBlockedWriterTest`, cited here until AY-S1,
//     went with AT-S6 and ran with no lock table (AY-Q9).
//   - `AResumedWriteThatHasAlreadyWrittenRowsIsRefusedRatherThanParkedMidWalk`
//     - the item-16 review's B1, which withheld the mid-walk park from a
//     probe's resume. `resumed_from_fk_probe_` is gone: a re-run is a whole
//     statement and AO-S3b's own guard is what stands.

#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/catalog/catalog.hpp"
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

// ---- D9(a)'s cells, red first (AY-S4) --------------------------------------
//
// **Written red at AY-S4 and disabled until AY-S5 built D9(a)**; each
// cell's red at the commit that wrote it is in
// `workorder-ay-following-letter.md` §6, AY-S4. D9(a) as marked: the
// child's forward check takes `IS` on the parent relation, then `S` on the
// parent row, **then** descends (AY-Q2), and holds both to its decide; the
// self-referencing arm takes the same pair per row (AY-Q3); and a parent
// `DELETE` meeting a child row whose writer holds no `S` on it - a child
// `DELETE`, or an `UPDATE` moving the fk column - waits for that writer
// mid-walk rather than being refused (AY-Q8). AR2 E3 retires when the
// parent-`DELETE`-against-child-write cells pass (`raft-marks-2026-09-29.md`
// §4).

// One session's statements on one core, each run only once the cell allows
// it: `allowed` is how many may have started, `done` how many have ended.
// The statements are fixed before `Start()`, so `outs` never reallocates
// under a running statement.
struct Script {
    explicit Script(std::vector<std::string> s)
        : statements(std::move(s)), outs(statements.size()) {}
    Session session;
    std::vector<std::string> statements;
    std::vector<DispatchOutcome> outs;
    std::function<bool()> pred;
    std::atomic<std::size_t> allowed{0};
    std::atomic<std::size_t> done{0};

    void Allow(std::size_t n) { allowed.store(n, std::memory_order_release); }
    bool Done(std::size_t n) const { return done.load(std::memory_order_acquire) >= n; }
    const std::string& Reply(std::size_t i) const { return outs[i].response; }
};

sched::Coro RunScript(CommandDispatcher& d, Script& s) {
    for (std::size_t i = 0; i < s.statements.size(); ++i) {
        s.pred = [&s, i] { return s.allowed.load(std::memory_order_acquire) > i; };
        co_await sched::WaitUntil{&s.pred};
        co_await d.DispatchAsync(s.statements[i], &s.session, &s.outs[i]);
        s.done.store(i + 1, std::memory_order_release);
    }
    co_return Status::OK();
}

// A script per core over `p` (parents 7 and 8) and `c`, plus whatever
// `seed` writes from core 0 and `prepare` does before the reactors start;
// both scripts are submitted then and wait for their first `Allow`.
struct ScriptRig {
    ScriptRig(std::vector<std::string> on0, std::vector<std::string> on1,
             const std::vector<std::string>& seed = {},
             const std::function<bool(TwoCoreRig&)>& prepare = nullptr)
        : core0(std::move(on0)), core1(std::move(on1)), r({}) {
        if (r.rig == nullptr) return;
        if (Status s = r.Seed(); !s.ok()) {
            ADD_FAILURE() << s.message();
            return;
        }
        CommandDispatcher& d0 = r.rig->core(0).dispatcher();
        std::vector<std::string> rows = {"INSERT INTO p VALUES (7, 0)",
                                         "INSERT INTO p VALUES (8, 0)"};
        rows.insert(rows.end(), seed.begin(), seed.end());
        for (const std::string& sql : rows) {
            const std::string reply = d0.Dispatch(sql).response;
            if (reply.rfind("ERR", 0) == 0) {
                ADD_FAILURE() << sql << " -> " << reply;
                return;
            }
        }
        if (prepare && !prepare(*r.rig)) return;
        r.rig->core(0).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, RunScript(r.rig->core(0).dispatcher(), core0)));
        r.rig->core(1).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, RunScript(r.rig->core(1).dispatcher(), core1)));
        r.rig->Start();
        ok = true;
    }

    // Runs `script` through its statement `n - 1` on `core` and waits.
    bool RunTo(std::uint32_t core, Script& script, std::size_t n) {
        script.Allow(n);
        return KickUntil(*r.rig, core, [&] { return script.Done(n); }, 4000ms);
    }
    // Whether `script`'s statement `n - 1` is parked on the row `(rel, pk)`
    // rather than finished: what "parks on the parent row" means below.
    bool ParkedOn(std::uint32_t core, Script& script, std::size_t n, catalog::Oid rel,
                  std::uint64_t pk) {
        script.Allow(n);
        const txn::LockKey row = txn::LockKey::Tuple(rel, pk);
        auto waiting = [&] { return r.rig->locks().WaiterCount(row) >= 1; };
        KickUntil(*r.rig, core, [&] { return script.Done(n) || waiting(); }, 4000ms);
        return !script.Done(n) && waiting();
    }
    // Whether `script`'s statement `n - 1` is still unanswered after
    // `given`: a wait, whichever unit it is on. **`given` stays under the
    // lock family's 1 s fault net** (`kLockWaitFaultNetNs`): a wait still
    // open at the net is refused `TxnConflict`, so a longer look would read
    // a correct park as an answer, and the cell has to end the wait itself
    // before the net does.
    bool StillWaiting(std::uint32_t core, Script& script, std::size_t n,
                      std::chrono::milliseconds given = 500ms) {
        script.Allow(n);
        KickUntil(*r.rig, core, [&] { return script.Done(n); }, given);
        return !script.Done(n);
    }

    // The scripts before the rig: coroutine frames the reactors hold borrow
    // them, and reverse destruction tears the frames down first.
    Script core0;
    Script core1;
    FkRig r;
    bool ok = false;
};

// E3 (i). A child's open reference parks a parent `DELETE` **on the parent
// row**, which the child's `S` holds until it decides. Without the `S` the
// delete reaches its reverse check and waits on the child's row instead -
// the interval after the child's write, which was always covered. The cell
// pins the unit because the interval before the write (the window cell
// below) is covered by nothing else.
TEST(FkCrossCoreRigTest, AChildsOpenReferenceParksAParentDeleteOnTheParentRow) {
    ScriptRig f({"DELETE FROM p WHERE id = 7"}, {"BEGIN", "INSERT INTO c VALUES (7)", "COMMIT"});
    ASSERT_TRUE(f.ok);
    ASSERT_TRUE(f.RunTo(1, f.core1, 2)) << f.core1.Reply(1);
    ASSERT_EQ(f.core1.Reply(1).rfind("INSERTED", 0), 0u) << f.core1.Reply(1);

    EXPECT_TRUE(f.ParkedOn(0, f.core0, 1, f.r.parent_oid, 7))
        << "the parent DELETE did not park on the parent row: " << f.core0.Reply(0);

    ASSERT_TRUE(f.RunTo(1, f.core1, 3)) << f.core1.Reply(2);
    ASSERT_TRUE(f.RunTo(0, f.core0, 1));
    EXPECT_NE(f.core0.Reply(0).find("FK_VIOLATION"), std::string::npos) << f.core0.Reply(0);
}

// E3 (iii). A multi-key parent `DELETE` declares a range `X`, which meets
// the child's tuple `S` only in the lock table's verify arm: AY-S2's
// containment wake is what parks it, on the parent row's entry.
TEST(FkCrossCoreRigTest, ARangeDeleteOfParentsParksOnAChildsOpenReference) {
    ScriptRig f({"DELETE FROM p WHERE id >= 7"}, {"BEGIN", "INSERT INTO c VALUES (7)", "COMMIT"});
    ASSERT_TRUE(f.ok);
    ASSERT_TRUE(f.RunTo(1, f.core1, 2)) << f.core1.Reply(1);

    EXPECT_TRUE(f.ParkedOn(0, f.core0, 1, f.r.parent_oid, 7))
        << "the range DELETE did not park on the referenced parent: " << f.core0.Reply(0);

    ASSERT_TRUE(f.RunTo(1, f.core1, 3)) << f.core1.Reply(2);
    ASSERT_TRUE(f.RunTo(0, f.core0, 1));
    // Statement-atomic: 8 is unreferenced, and the refusal over 7 keeps it.
    EXPECT_NE(f.core0.Reply(0).find("FK_VIOLATION"), std::string::npos) << f.core0.Reply(0);
    EXPECT_EQ(f.r.rig->core(0).dispatcher().Dispatch("SELECT id FROM p").response, "id\\n7\\n8");
}

// E3 (iv), AY-Q3. The self-referencing arm is not hoisted and checks per
// row; it takes the same `IS` + `S` there. No `CREATE TABLE` can declare a
// self-reference (`ASelfReferencingForeignKeyCannotBeDeclared`), so the key
// is written through `Catalog::CreateForeignKey` directly, the way a
// split relation's directory rows are written in the cells that need one.
TEST(FkCrossCoreRigTest, ASelfReferencingChildsOpenReferenceParksTheParentsDelete) {
    catalog::Oid s_oid = 0;
    ScriptRig f({"DELETE FROM s WHERE id = 1"}, {"BEGIN", "INSERT INTO s VALUES (2, 1)", "COMMIT"},
               {"CREATE TABLE s (id int64, pid int64 NULL) BTREE", "INSERT INTO s VALUES (1, NULL)"},
               [&](TwoCoreRig& rig) {
                   auto oid = rig.core(0).catalog().FindTableOidByName("s");
                   if (!oid.ok()) return false;
                   s_oid = oid.value();
                   auto fk = rig.core(0).catalog().CreateForeignKey(s_oid, 1, s_oid);
                   EXPECT_TRUE(fk.ok()) << fk.status().message();
                   return fk.ok();
               });
    ASSERT_TRUE(f.ok);

    ASSERT_TRUE(f.RunTo(1, f.core1, 2)) << f.core1.Reply(1);
    ASSERT_EQ(f.core1.Reply(1).rfind("INSERTED", 0), 0u) << f.core1.Reply(1);

    EXPECT_TRUE(f.ParkedOn(0, f.core0, 1, s_oid, 1))
        << "the parent DELETE did not park on the parent row: " << f.core0.Reply(0);

    ASSERT_TRUE(f.RunTo(1, f.core1, 3)) << f.core1.Reply(2);
    ASSERT_TRUE(f.RunTo(0, f.core0, 1));
    EXPECT_NE(f.core0.Reply(0).find("FK_VIOLATION"), std::string::npos) << f.core0.Reply(0);
}

// AY-Q3's other half (`raft-marks-2026-09-30.md` §3): a self-referencing
// child whose parent row is being written by an undecided transaction
// waits for it, as a hoisted parent does, rather than being refused - and
// passes when it commits.
TEST(FkCrossCoreRigTest, ASelfReferencingChildWaitsOutItsParentsWriterAndPasses) {
    catalog::Oid s_oid = 0;
    ScriptRig f({"BEGIN", "INSERT INTO s VALUES (1, NULL)", "COMMIT"},
                {"INSERT INTO s VALUES (2, 1)"},
                {"CREATE TABLE s (id int64, pid int64 NULL) BTREE"}, [&](TwoCoreRig& rig) {
                    auto oid = rig.core(0).catalog().FindTableOidByName("s");
                    if (!oid.ok()) return false;
                    s_oid = oid.value();
                    auto fk = rig.core(0).catalog().CreateForeignKey(s_oid, 1, s_oid);
                    EXPECT_TRUE(fk.ok()) << fk.status().message();
                    return fk.ok();
                });
    ASSERT_TRUE(f.ok);
    ASSERT_TRUE(f.RunTo(0, f.core0, 2)) << f.core0.Reply(1);
    ASSERT_EQ(f.core0.Reply(1).rfind("INSERTED", 0), 0u) << f.core0.Reply(1);

    EXPECT_TRUE(f.ParkedOn(1, f.core1, 1, s_oid, 1))
        << "the self-referencing child did not wait on its parent's writer: " << f.core1.Reply(0);

    ASSERT_TRUE(f.RunTo(0, f.core0, 3)) << f.core0.Reply(2);
    ASSERT_TRUE(f.RunTo(1, f.core1, 1));
    EXPECT_EQ(f.core1.Reply(0).rfind("INSERTED", 0), 0u) << f.core1.Reply(0);
}

// E3 (v), and AY-Q1's cost. Two transactions each write a child of 7 and
// then update 7: each holds `S(7)` and asks `X(7)` over the other's, and
// the second asker closes the cycle and is refused naming deadlock. Without
// the `S` the two updates serialise on the row's `X`.
TEST(FkCrossCoreRigTest, TwoChildWritersThatThenUpdateTheirParentDeadlock) {
    ScriptRig f(
        {"BEGIN", "INSERT INTO c VALUES (7)", "UPDATE p SET v = 1 WHERE id = 7", "ROLLBACK"},
        {"BEGIN", "INSERT INTO c VALUES (7)", "UPDATE p SET v = 2 WHERE id = 7", "ROLLBACK"});
    ASSERT_TRUE(f.ok);
    ASSERT_TRUE(f.RunTo(0, f.core0, 2)) << f.core0.Reply(1);
    ASSERT_TRUE(f.RunTo(1, f.core1, 2)) << f.core1.Reply(1);

    EXPECT_TRUE(f.ParkedOn(0, f.core0, 3, f.r.parent_oid, 7))
        << "a parent UPDATE ran past another transaction's open child: " << f.core0.Reply(2);
    // The waiter count moves inside the ask and the wait-for edge is drawn
    // just after it, with no suspension between; asked in that gap, core 1
    // would find no cycle yet and core 0 would close it instead.
    std::this_thread::sleep_for(20ms);

    ASSERT_TRUE(f.RunTo(1, f.core1, 3));
    EXPECT_NE(f.core1.Reply(2).find("deadlock"), std::string::npos) << f.core1.Reply(2);

    // The victim's rollback releases its `S`, and the first update proceeds.
    ASSERT_TRUE(f.RunTo(1, f.core1, 4)) << f.core1.Reply(3);
    ASSERT_TRUE(KickUntil(*f.r.rig, 0, [&] { return f.core0.Done(3); }, 4000ms));
    EXPECT_EQ(f.core0.Reply(2), "UPDATED 1");
    ASSERT_TRUE(f.RunTo(0, f.core0, 4)) << f.core0.Reply(3);
}

// E3 (ii), the window itself: a parent deleted **between the child's
// check and its write**, which only the `S` held from the hoist closes
// (`foreign-keys.md` §3a). The window is the insert path's one seam
// (`SetBeforeInsertLogForTest`) on a two-row `INSERT`: both parents are
// resolved before any row, row 1 is placed, and core 1 deletes row 2's
// parent before row 2 exists.
//
// **Where the rows land is what makes it deterministic.** The seam runs
// under the hold that placed row 1 (AT-S21), so the delete's reverse walk
// of `c` stops at row 1's leaf until core 0 goes on. Row 1 is therefore the
// rightmost leaf's and row 2 the leftmost's: the walk has passed row 2's
// leaf before row 2 is written, whatever order the two cores then run in.
//
// The reactors are not started; both dispatchers run on threads of the
// cell's own, `insert_log_crash_rig_test.cpp`'s shape.
TEST(FkCrossCoreRigTest, AParentDeletedBetweenAChildsCheckAndItsWriteLeavesNoOrphan) {
    FkRig r({});
    ASSERT_NE(r.rig, nullptr);
    if (Status seeded = r.Seed(); !seeded.ok()) FAIL() << seeded.message();
    CommandDispatcher& d0 = r.rig->core(0).dispatcher();
    CommandDispatcher& d1 = r.rig->core(1).dispatcher();

    for (const char* row : {"INSERT INTO p VALUES (1, 0)", "INSERT INTO p VALUES (8, 0)"}) {
        ASSERT_EQ(d0.Dispatch(row).response.rfind("INSERTED", 0), 0u) << row;
    }
    // Committed children of 1 at ids 20..10000, so `c` spans several leaves
    // and 10 is free at the far left.
    for (std::uint64_t id = 20; id <= 10000; id += 10) {
        const std::string sql = "INSERT INTO c VALUES (" + std::to_string(id) + ", 1)";
        ASSERT_EQ(d0.Dispatch(sql).response.rfind("INSERTED", 0), 0u) << sql;
    }
    // Core 1's first transaction carves its id window through `Sync()`,
    // which waits out every held dirty frame - core 0's leaf, inside the
    // seam. Carved now, it is not the thing the seam waits on.
    ASSERT_EQ(d1.Dispatch("INSERT INTO p VALUES (9, 0)").response.rfind("INSERTED", 0), 0u);

    std::string deleted;
    std::atomic<bool> delete_done{false};
    std::thread other;
    // One-shot by `other`: resetting the hook from inside it would destroy
    // the function while it runs.
    d0.SetBeforeInsertLogForTest([&] {
        if (other.joinable()) return;
        other = std::thread([&] {
            deleted = d1.Dispatch("DELETE FROM p WHERE id = 8").response;
            delete_done.store(true, std::memory_order_release);
        });
        // Long enough for the walk to pass the leftmost leaf; the delete
        // then waits on row 1's leaf, or on the parent row, or is done.
        const auto until = std::chrono::steady_clock::now() + 500ms;
        while (!delete_done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(2ms);
        }
    });
    const std::string inserted = d0.Dispatch("INSERT INTO c VALUES (100000, 1), (10, 8)").response;
    d0.SetBeforeInsertLogForTest(nullptr);
    ASSERT_TRUE(other.joinable()) << "the seam never ran; the cell tested nothing";
    other.join();

    // At most one of the two may have succeeded: both is a child of 8
    // with no 8.
    const bool child_written = inserted.rfind("INSERTED", 0) == 0;
    const bool parent_gone = deleted.rfind("DELETED 1", 0) == 0;
    EXPECT_FALSE(child_written && parent_gone)
        << "insert: " << inserted << "; delete: " << deleted;
    const std::string orphans = d0.Dispatch("SELECT id FROM c WHERE pid = 8").response;
    const std::string parent = d0.Dispatch("SELECT id FROM p WHERE id = 8").response;
    EXPECT_TRUE(orphans == "id" || parent == "id\\n8")
        << "children of 8: " << orphans << "; parent: " << parent;
}

// AY-Q8. A child `DELETE` holds no `S` on the parent it stops referencing.
// The parent `DELETE` meets its row mid-walk, undecided, and waits for that
// writer rather than being refused; the writer's commit leaves the parent
// unreferenced.
TEST(FkCrossCoreRigTest, AParentDeleteWaitsOutAnOpenChildDeleteAndPassesAtItsCommit) {
    ScriptRig f({"DELETE FROM p WHERE id = 7"}, {"BEGIN", "DELETE FROM c WHERE id = 1", "COMMIT"},
               {"INSERT INTO c VALUES (7)"});
    ASSERT_TRUE(f.ok);
    ASSERT_TRUE(f.RunTo(1, f.core1, 2)) << f.core1.Reply(1);
    ASSERT_EQ(f.core1.Reply(1), "DELETED 1");

    EXPECT_TRUE(f.StillWaiting(0, f.core0, 1))
        << "the parent DELETE was answered with the child's writer open: " << f.core0.Reply(0);

    ASSERT_TRUE(f.RunTo(1, f.core1, 3)) << f.core1.Reply(2);
    ASSERT_TRUE(f.RunTo(0, f.core0, 1));
    EXPECT_EQ(f.core0.Reply(0), "DELETED 1");
}

// AY-Q8's other shape, and the one that is quietly wrong if answered early:
// an `UPDATE` moving the child off 7 holds `S(8)`, not `S(7)`. A parent
// `DELETE` of 7 that reads the moved row and answers "no children" removes
// a parent the child's rollback then references again.
TEST(FkCrossCoreRigTest, AParentDeleteWaitsOutAChildMovedOffItAndRefusesAtItsRollback) {
    ScriptRig f({"DELETE FROM p WHERE id = 7"},
               {"BEGIN", "UPDATE c SET pid = 8 WHERE id = 1", "ROLLBACK"},
               {"INSERT INTO c VALUES (7)"});
    ASSERT_TRUE(f.ok);
    ASSERT_TRUE(f.RunTo(1, f.core1, 2)) << f.core1.Reply(1);
    ASSERT_EQ(f.core1.Reply(1), "UPDATED 1");

    EXPECT_TRUE(f.StillWaiting(0, f.core0, 1))
        << "the parent DELETE was answered with the child's writer open: " << f.core0.Reply(0);

    ASSERT_TRUE(f.RunTo(1, f.core1, 3)) << f.core1.Reply(2);
    ASSERT_TRUE(f.RunTo(0, f.core0, 1));
    EXPECT_NE(f.core0.Reply(0).find("FK_VIOLATION"), std::string::npos) << f.core0.Reply(0);
    // Whatever the answer, the child row references 7 again, so 7 is there.
    EXPECT_EQ(f.r.rig->core(0).dispatcher().Dispatch("SELECT id FROM p WHERE id = 7").response,
              "id\\n7");
}

}  // namespace
}  // namespace kds::server
