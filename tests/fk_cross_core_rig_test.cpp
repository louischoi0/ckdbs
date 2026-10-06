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
#include "kds/exec/cabin_optimizer_exec.hpp"
#include "kds/stats/cabin_optimizer.hpp"
#include "kds/stats/cabin_store.hpp"
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
//
// **E3 (ii), the check-to-write window, left this file at BB-S3's review.**
// Its cell put a second row behind the `DELETE`'s walk by naming a key below
// `c`'s mark - the leftmost leaf, while the insert seam held row 1's
// rightmost - and BB-R3 refuses such a key before the row is written, so the
// cell passed with nothing tested. Every SQL insert lands on the rightmost
// leaf since BB-R3, which that seam holds, so no seam here can put a row
// behind the walk: a two-core cell needs one between the hoist and the first
// row's descent, which the engine does not have. The window is pinned on one
// thread, `FkParentHoldTest.AParentDeletedBetweenAChildsCheckAndItsWriteIsRefused`.

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
// pins the unit because the interval before the write is covered by nothing
// else across cores (the window's cell is one thread's, `fk_parent_hold_test.cpp`).
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
// is written through `Catalog::CreateForeignKey` directly.
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

// ---- The Cabin's "no children" answer (AY-S6) ----------------------------
//
// An observed value's set is a superset of the pks that carry it (`cabin.md`
// §1), so a set whose every entry fails to be a live child of 7 is an
// authoritative "no children" - F6's whole reason. AT-S5f took that answer
// away while the store was a dispatcher's own, a child written on another
// core never reaching this core's set; the store is the instance's since
// AT-S7, and **since AY-S6 the answer is back**, sound because D9(a) keeps
// every child writer that sets the fk to 7 on `S(7)`, which the `DELETE`'s
// `X(7)` refuses (`foreign-keys.md` §3a).

// `uses=` of the `SHOW ACCESS` line for `kind` on `rel`, or 0 without one.
std::uint64_t AccessUses(CommandDispatcher& d, const std::string& kind, const std::string& rel) {
    const std::string shown = d.Dispatch("SHOW ACCESS").response;
    const std::string anchor = "kind=" + kind + " rel=" + rel + " ";
    const std::size_t at = shown.find(anchor);
    if (at == std::string::npos) return 0;
    const std::size_t uses = shown.find("uses=", at);
    return uses == std::string::npos ? 0 : std::stoull(shown.substr(uses + 5));
}

// A child committed on core 1 is found in core 0's banked set, and once the
// set holds no live child the parent's `DELETE` is cleared from it - a
// `CabinProbe` of `c`, and no `FilterScan` walk. **Red at `2d2d96b`**: the
// clearing return was absent and the pass walked `c`.
//
// **Mutation**: the walk kept after an exhausted set - the code this stage
// replaced - and the pass records a `FilterScan`.
TEST(FkCrossCoreRigTest, ADrainedCabinSetClearsTheParentAndAChildOnAnotherCoreIsFoundInIt) {
    FkRig r({});
    ASSERT_NE(r.rig, nullptr);
    if (Status seeded = r.Seed(); !seeded.ok()) FAIL() << seeded.message();
    CommandDispatcher& d0 = r.rig->core(0).dispatcher();

    ASSERT_EQ(d0.Dispatch("INSERT INTO p VALUES (7, 0)").response.rfind("INSERTED", 0), 0u);
    ASSERT_EQ(d0.Dispatch("CREATE CABIN ON c(pid)").response.rfind("CRE", 0), 0u);
    // A child of 7 and the read that banks the set for 7; the child then
    // goes, leaving its pk in the set as a surplus entry.
    ASSERT_EQ(d0.Dispatch("INSERT INTO c VALUES (7)").response.rfind("INSERTED", 0), 0u);
    ASSERT_EQ(d0.Dispatch("SELECT id FROM c WHERE pid = 7").response, "id\\n1");
    ASSERT_EQ(d0.Dispatch("DELETE FROM c WHERE pid = 7").response, "DELETED 1");

    // A new child of 7, written on core 1 into the instance's one store.
    r.one.statement = "INSERT INTO c VALUES (7)";
    r.rig->core(1).scheduler().Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground, RunOne(r.rig->core(1).dispatcher(), r.one)));
    r.rig->Start();
    ASSERT_TRUE(KickUntil(*r.rig, 1, [&] { return r.one.done.load(std::memory_order_acquire); },
                          4000ms))
        << r.one.out.response;
    ASSERT_EQ(r.one.out.response.rfind("INSERTED", 0), 0u) << r.one.out.response;

    // Found in the set: refused, and from the Cabin.
    const std::uint64_t probes_before = AccessUses(d0, "CabinProbe", "c");
    EXPECT_NE(d0.Dispatch("DELETE FROM p WHERE id = 7").response.find("FK_VIOLATION"),
              std::string::npos);
    EXPECT_EQ(AccessUses(d0, "CabinProbe", "c"), probes_before + 1);

    // The child goes too; the set now holds two dead entries and is drained.
    ASSERT_EQ(d0.Dispatch("DELETE FROM c WHERE pid = 7").response, "DELETED 1");
    const std::uint64_t walks_before = AccessUses(d0, "FilterScan", "c");
    EXPECT_EQ(d0.Dispatch("DELETE FROM p WHERE id = 7").response, "DELETED 1");
    EXPECT_EQ(AccessUses(d0, "CabinProbe", "c"), probes_before + 2)
        << "the pass was not answered from the set";
    EXPECT_EQ(AccessUses(d0, "FilterScan", "c"), walks_before)
        << "the pass walked the child relation although the set cleared it";
}

// A read that would bank 7's set while a child insert of 7 is open on core 1
// is declined by the banking gate (`cabin.md` §6a): the insert's hook found
// 7 unobserved and appended nothing, so a set banked then would miss it and
// clear its parent once it commits. The walk answers instead.
TEST(FkCrossCoreRigTest, ASetBankedWhileAChildInsertIsOpenIsDeclinedAndTheWalkAnswers) {
    ScriptRig f({"SELECT id FROM c WHERE pid = 7", "DELETE FROM p WHERE id = 7"},
                {"BEGIN", "INSERT INTO c VALUES (7)", "COMMIT"}, {"CREATE CABIN ON c(pid)"});
    ASSERT_TRUE(f.ok);
    ASSERT_TRUE(f.RunTo(1, f.core1, 2)) << f.core1.Reply(1);
    ASSERT_EQ(f.core1.Reply(1).rfind("INSERTED", 0), 0u) << f.core1.Reply(1);

    ASSERT_TRUE(f.RunTo(0, f.core0, 1)) << f.core0.Reply(0);
    EXPECT_EQ(f.core0.Reply(0), "id");  // the open child is not this view's

    ASSERT_TRUE(f.RunTo(1, f.core1, 3)) << f.core1.Reply(2);
    CommandDispatcher& d0 = f.r.rig->core(0).dispatcher();
    const std::uint64_t walks_before = AccessUses(d0, "FilterScan", "c");
    ASSERT_TRUE(f.RunTo(0, f.core0, 2)) << f.core0.Reply(1);
    EXPECT_NE(f.core0.Reply(1).find("FK_VIOLATION"), std::string::npos) << f.core0.Reply(1);
    EXPECT_EQ(AccessUses(d0, "FilterScan", "c"), walks_before + 1)
        << "the parent's DELETE was answered from a set banked while its child was open";
}

// **The controller's build banks what it did not read, since AY-S6** - by
// announcing each seed's set before it walks, as the serve path's build
// does (`cabin.md` §6), and asking the banking gate before it walks
// (`raft-marks-2026-09-30.md` §12). Until then `BuildSeededSets` walked and
// then committed: a child of 7 written on another core behind the walk -
// on a leaf it had already read - found 7 unobserved, so its hook appended
// nothing, and the set banked without it. With the clearing return that is
// an orphan: the parent's `DELETE` is cleared from the set.
//
// The walk is driven from the cell's thread over the rig's one store, with
// core 0's catalog, the instance's Cabin store and core 0's manager, and
// its page-boundary switch (`enabled`) is the seam: at the third leaf,
// core 1's dispatcher moves child 10 - the first leaf's lowest key - from
// parent 1 to 7. Not at the second: the walk's scan ring holds the leaf it
// last fetched shared until its next fetch, so the first leaf is free only
// once the second is fetched - which is when another core's writer,
// waiting on that latch, would get it.
//
// **The write is an `UPDATE` since BB-S3; it was an `INSERT` of child 5.**
// BB-R3, on BB-Q8 (b), refuses a named key below the relation's mark, and
// an insert the mark admits lands on the rightmost leaf - ahead of the
// walk, where the build's view finds it busy and defers - so no insert
// reaches a leaf the walk has read. An update of the Cabin column writes
// in place, behind the walk, through the same hook, which keeps the
// subject. **Red at `445e00d`** in the insert shape: the set banked empty
// and `DELETED 1`. The update shape has not been run against that commit.
TEST(FkCrossCoreRigTest, AChildMovedOntoASeedBehindTheControllersBuildIsInTheSetItBanks) {
    FkRig r({});
    ASSERT_NE(r.rig, nullptr);
    if (Status seeded = r.Seed(); !seeded.ok()) FAIL() << seeded.message();
    CommandDispatcher& d0 = r.rig->core(0).dispatcher();
    CommandDispatcher& d1 = r.rig->core(1).dispatcher();

    for (const char* row : {"INSERT INTO p VALUES (1, 0)", "INSERT INTO p VALUES (7, 0)"}) {
        ASSERT_EQ(d0.Dispatch(row).response.rfind("INSERTED", 0), 0u) << row;
    }
    // Children of 1 at ids 10..10000, so `c` spans several leaves and 10
    // is the first's lowest key.
    for (std::uint64_t id = 10; id <= 10000; id += 10) {
        const std::string sql = "INSERT INTO c VALUES (" + std::to_string(id) + ", 1)";
        ASSERT_EQ(d0.Dispatch(sql).response.rfind("INSERTED", 0), 0u) << sql;
    }
    // Core 1's id window, carved before the seam.
    ASSERT_EQ(d1.Dispatch("INSERT INTO p VALUES (9, 0)").response.rfind("INSERTED", 0), 0u);

    // An optimizer-owned Cabin on `c.pid`, and 7 sighted once: seeded, not
    // observed (the auto threshold is two).
    catalog::Catalog& catalog = r.rig->core(0).catalog();
    auto c_oid = catalog.FindTableOidByName("c");
    ASSERT_TRUE(c_oid.ok());
    auto cabin = catalog.CreateCabin(c_oid.value(), /*col_pos=*/1, catalog::kCabinOriginAuto);
    ASSERT_TRUE(cabin.ok()) << cabin.status().message();
    ASSERT_EQ(d0.Dispatch("SELECT id FROM c WHERE pid = 7").response, "id");
    stats::CabinStore& cabins = *r.rig->core(0).cabins();
    ASSERT_EQ(cabins.SightedUnobservedOf(cabin.value()).size(), 1u);

    stats::ActionItem extend;
    extend.action = stats::CabinAction::kExtend;
    extend.reason = stats::ActionReason::kCoverageExpansion;
    extend.cabin_id = cabin.value();
    extend.rel_oid = c_oid.value();
    extend.col_pos = 1;
    std::string written;
    int boundaries = 0;
    // The action's boundary, then one per leaf: the fourth is the third leaf.
    const std::function<bool()> enabled = [&] {
        if (++boundaries == 4) written = d1.Dispatch("UPDATE c SET pid = 7 WHERE id = 10").response;
        return true;
    };
    stats::CabinOptimizer controller;
    exec::CabinOptimizerExecutor executor(catalog, r.rig->store(), cabins, controller,
                                          &r.rig->core(0).transactions());
    ASSERT_TRUE(executor.Apply({extend}, enabled).ok());
    ASSERT_GE(boundaries, 4) << "the walk read under three leaves; the cell tested nothing";
    ASSERT_EQ(written, "UPDATED 1");

    parser::AstValue seven;
    seven.type = parser::ValueType::kInt;
    seven.int_val = 7;
    auto key = stats::MakeCabinKey(cabin.value(), seven);
    ASSERT_TRUE(key.has_value());
    const stats::CabinSet set = cabins.Find(*key);
    ASSERT_TRUE(set.valid()) << "the build banked nothing";
    EXPECT_EQ(set.size(), 1u) << "the child moved behind the walk is not in the set";

    const std::string deleted = d0.Dispatch("DELETE FROM p WHERE id = 7").response;
    EXPECT_NE(deleted.find("FK_VIOLATION"), std::string::npos)
        << "the parent was cleared from a set missing its child: " << deleted;
}

}  // namespace
}  // namespace kds::server
