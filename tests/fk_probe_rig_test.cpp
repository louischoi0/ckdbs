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

// Rows on a peer-owned child relation, which only a reactor can write: a
// child `INSERT` carrying a foreign parent raises a probe, and the
// synchronous `Dispatch` has nowhere to park it, so it is refused. Seeded
// through `DispatchAsync` like everything else that crosses.
struct RowSeeder {
    Session session;
    DispatchOutcome out;
    std::atomic<bool> done{false};
    std::vector<std::string> statements;
};

sched::Coro SeedRows(CommandDispatcher& d, RowSeeder& r) {
    for (const std::string& sql : r.statements) {
        co_await d.DispatchAsync(sql, &r.session, &r.out);
        if (r.out.response.rfind("INSERTED", 0) != 0) break;
    }
    r.done.store(true, std::memory_order_release);
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

    // AO-S6d's shape: the parent **committed** on core 0 before anything
    // runs, so the probe passes and the only thing the child's resume can
    // meet is the fence; the fence and the child both on core 1, the fence
    // taken first. `parent` holds the fence here rather than a parent row -
    // the struct is the same session-plus-atomics and only the statement
    // differs, which its own comment says.
    Status SubmitFenced(const char* ending, bool in_txn = false) {
        if (Status s = Expect("insert parent",
                              rig->core(0).dispatcher().Dispatch("INSERT INTO p VALUES (7, 0)")
                                  .response,
                              "INSERTED");
            !s.ok()) {
            return s;
        }
        parent.statement = "DELETE FROM c WHERE id > 0 AND pid = 999";
        parent.ending = ending;
        child.in_txn = in_txn;
        rig->core(1).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, WriteParent(rig->core(1).dispatcher(), parent)));
        rig->core(1).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, WriteChild(rig->core(1).dispatcher(), child)));
        rig->Start();
        return Status::OK();
    }

    // AO-S6d's B1 shape: three child rows on core 1, a holder taking the
    // middle one, and a victim whose `SET` names a **foreign** parent - so
    // its first dispatch raises a probe and its walk happens on the resume,
    // which is where the mid-walk park is now withheld.
    Status SubmitMidWalkOnResume() {
        for (const char* sql : {"INSERT INTO p VALUES (7, 0)", "INSERT INTO p VALUES (8, 0)"}) {
            if (Status s = Expect("insert parent",
                                  rig->core(0).dispatcher().Dispatch(sql).response, "INSERTED");
                !s.ok()) {
                return s;
            }
        }
        // **Issued keys, not named ones**: a caller-supplied primary key is
        // refused on a peer (`workplan-peer-writer.md` §7a), so the three
        // rows take 1, 2 and 3 from the block `Seed` funded, and the holder
        // names the middle one by a *window* rather than by a key. The
        // `UPDATED 1` the cell asserts is what makes that arithmetic
        // checked rather than assumed.
        seeder.statements = {"INSERT INTO c VALUES (7)", "INSERT INTO c VALUES (7)",
                             "INSERT INTO c VALUES (7)"};
        parent.gated = true;
        parent.statement = "UPDATE c SET pid = 7 WHERE id > 1 AND id < 3";
        child.in_txn = true;
        child.statement = "UPDATE c SET pid = 8 WHERE pid = 7";
        child.after = "ROLLBACK";
        // Read back through the holder, after its own decide: the victim's
        // session is the one that must not be believed here.
        parent.after = "SELECT * FROM c";
        rig->core(1).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, SeedRows(rig->core(1).dispatcher(), seeder)));
        rig->core(1).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, WriteParent(rig->core(1).dispatcher(), parent)));
        rig->core(1).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, WriteChild(rig->core(1).dispatcher(), child)));
        rig->Start();
        return Status::OK();
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
    RowSeeder seeder;
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

// `ATransactionWritingItsOwnParentAcrossCoresPassesItsChildWithoutAPark` stood here until AT-S5: it pinned a transaction that shipped its parent INSERT to the parent's owner and probed it from the child's core; a write runs where the session is since AT-S5, so the parent is the transaction's own on one core and the check reads it locally.

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

// ---- AO-S6d item 16: the wait a probe's resume may now make ------------
//
// `DispatchAsync` ran the write-block wait before the probe arm and never
// after it, so a statement that parked on a foreign parent and then met a
// held row on its resume was refused where the same statement dispatched
// directly would have waited. Inside an explicit transaction that refusal
// carried the poison `EndWrite` withholds for a wait still to come, which
// is a client told `ERR` over a transaction that then commits. All three
// cells begin from a parent that is already **committed**, so the probe
// passes and the only thing the resume can meet is the fence.

// The two decides, which differ by one word: a fence holder that commits
// and one that rolls back both release a window inside which they wrote
// nothing, so the child's re-run meets the state it would have met had the
// fence never existed. That is the point of waiting on a holder that is not
// the row's writer at all.
//
// **The mutation**, for both: return the resumed outcome past the wait -
// drop either `MayParkScope` around the resume or the `AwaitWriteBlock`
// after it - and the child is refused `TxnConflict` while the fence is
// still up, so the cell fails at "refused instead of waiting".
void AChildResumedFromItsProbeWaitsOutAFence(const char* ending) {
    FkRig r({});
    ASSERT_NE(r.rig, nullptr);
    if (Status seeded = r.Seed(); !seeded.ok()) FAIL() << seeded.message();
    if (Status submitted = r.SubmitFenced(ending); !submitted.ok()) FAIL() << submitted.message();

    ASSERT_TRUE(
        KickUntil(*r.rig, 1, [&] { return r.parent.holding.load(std::memory_order_acquire); }))
        << "the fence was never taken: " << r.parent.insert_out.response;
    ASSERT_EQ(r.parent.insert_out.response, "DELETED 0")
        << "the fencing statement must declare a window and write nothing in it: "
        << r.parent.insert_out.response;

    // The child ships its probe to core 0, which answers at once; the
    // resume then meets the fence.
    r.child.go.store(true, std::memory_order_release);
    EXPECT_FALSE(KickUntil(*r.rig, 1,
                           [&] { return r.child.done.load(std::memory_order_acquire); }, 800ms))
        << "the resumed child was refused instead of waiting on the fence: "
        << r.child.out.response;

    // The wait ends where every wait in this family ends - at the holder's
    // decide - and the re-run raises a **second** probe, which the arm
    // below the wait has to collect: a child that answers `INSERTED` is a
    // child whose second round was waited for rather than discarded.
    r.parent.may_end.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*r.rig, 1, [&] { return r.parent.ended.load(std::memory_order_acquire); },
                          3000ms))
        << r.parent.end_out.response;
    EXPECT_EQ(r.parent.end_out.response.rfind(ending, 0), 0u) << r.parent.end_out.response;
    EXPECT_TRUE(KickUntil(*r.rig, 1, [&] { return r.child.done.load(std::memory_order_acquire); },
                          4000ms))
        << "the child never resumed after the fence was released";
    EXPECT_EQ(r.child.out.response.rfind("INSERTED", 0), 0u) << r.child.out.response;
}

TEST(FkProbeRigTest, AChildResumedFromItsProbeWaitsOutAFenceOnItsOwnCore) {
    AChildResumedFromItsProbeWaitsOutAFence("COMMIT");
}

TEST(FkProbeRigTest, AChildResumedFromItsProbeAlsoWaitsOutAFenceThatRollsBack) {
    AChildResumedFromItsProbeWaitsOutAFence("ROLLBACK");
}

TEST(FkProbeRigTest, AResumedChildInsideATransactionWaitsRatherThanBeingToldErrAndCommitting) {
    // Item 16's `[quiet-wrong]` shape. **The mutation**: keep `may_park_`
    // around the resume and drop the `AwaitWriteBlock` after it. The
    // blocker is then recorded, `EndWrite` withholds the poison for a wait
    // that never happens, and the pair below reads `ERR TXN_CONFLICT` for
    // the INSERT and `COMMIT` for the COMMIT - a transaction told it failed
    // and committed anyway, which is the failure atomicity txn.md §6
    // states. The tick is on because this transaction's COMMIT prepares
    // across cores and a prepare parks on its own device sync.
    FkRig r(Ticking());
    ASSERT_NE(r.rig, nullptr);
    if (Status seeded = r.Seed(); !seeded.ok()) FAIL() << seeded.message();
    if (Status submitted = r.SubmitFenced("COMMIT", /*in_txn=*/true); !submitted.ok()) {
        FAIL() << submitted.message();
    }

    ASSERT_TRUE(
        KickUntil(*r.rig, 1, [&] { return r.parent.holding.load(std::memory_order_acquire); }))
        << r.parent.insert_out.response;
    ASSERT_EQ(r.parent.insert_out.response, "DELETED 0") << r.parent.insert_out.response;

    r.child.go.store(true, std::memory_order_release);
    EXPECT_FALSE(KickUntil(*r.rig, 1,
                           [&] { return r.child.inserted.load(std::memory_order_acquire); },
                           800ms))
        << "the resumed child was answered instead of waiting: " << r.child.out.response;

    r.parent.may_end.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*r.rig, 1, [&] { return r.parent.ended.load(std::memory_order_acquire); },
                          3000ms))
        << r.parent.end_out.response;
    ASSERT_TRUE(KickUntil(*r.rig, 1,
                          [&] { return r.child.done.load(std::memory_order_acquire); },
                          5000ms))
        << "the transaction never finished: insert=[" << r.child.out.response
        << "] commit=[" << r.child.commit_out.response << "]";
    EXPECT_EQ(r.child.out.response.rfind("INSERTED", 0), 0u)
        << r.child.out.response;
    EXPECT_EQ(r.child.commit_out.response.rfind("COMMIT", 0), 0u)
        << r.child.commit_out.response;
}

TEST(FkProbeRigTest, AResumedWriteThatHasAlreadyWrittenRowsIsRefusedRatherThanParkedMidWalk) {
    // The item-16 review's B1. Letting a probe's resume park is right;
    // letting it park **mid-walk** is not, and the difference is that a
    // mid-walk park leaves the walk's position and its count on the session
    // while the wait's re-run is a whole statement that re-resolves every
    // foreign parent - so the re-run raises a fresh probe before it reaches
    // the walk, `AbandonWriteForShipping` keeps the rows inside an explicit
    // transaction, and the parked write is gone.
    //
    // **The mutation**: drop `resumed_from_fk_probe_` from `UpdateInner`'s
    // park test. The victim below parks instead of being refused, so the
    // `KickUntil` fails - and let it run on to the holder's commit and it
    // answers, measured rather than predicted, `UPDATED 2` followed by
    // `COMMIT`: it wrote row 1 in the round that parked, lost its cursor to
    // the fresh probe the wait's re-run raised, walked again from the start
    // where its own row 1 no longer matches `pid = 7`, and committed three
    // written rows having reported two. That is the silent partial count
    // AO-S3b exists to prevent, made durable.
    FkRig r(Ticking());
    ASSERT_NE(r.rig, nullptr);
    if (Status seeded = r.Seed(); !seeded.ok()) FAIL() << seeded.message();
    if (Status submitted = r.SubmitMidWalkOnResume(); !submitted.ok()) {
        FAIL() << submitted.message();
    }

    ASSERT_TRUE(KickUntil(*r.rig, 1, [&] { return r.seeder.done.load(std::memory_order_acquire); },
                          5000ms))
        << "the child rows were never seeded: " << r.seeder.out.response;
    ASSERT_EQ(r.seeder.out.response.rfind("INSERTED", 0), 0u) << r.seeder.out.response;

    // The holder takes row 2, the middle of the victim's walk.
    r.parent.go.store(true, std::memory_order_release);
    ASSERT_TRUE(
        KickUntil(*r.rig, 1, [&] { return r.parent.holding.load(std::memory_order_acquire); },
                  5000ms))
        << r.parent.insert_out.response;
    ASSERT_EQ(r.parent.insert_out.response, "UPDATED 1") << r.parent.insert_out.response;

    // The victim: `SET pid = 8` names a parent on core 0, so round 1 raises
    // the probe having written nothing and the walk runs on the resume,
    // writing row 1 and then meeting row 2.
    r.child.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*r.rig, 1, [&] { return r.child.inserted.load(std::memory_order_acquire); },
                          3000ms))
        << "the resumed write parked mid-walk instead of being refused";
    EXPECT_EQ(StatusFromErrorReply(r.child.out.response).code(), StatusCode::kTxnConflict)
        << r.child.out.response;
    // **Named row 2, which is what makes this the *middle* of the walk**
    // rather than a refusal before it: row 1 sorts first, matches the same
    // predicate, and is written before the walk can reach row 2 at all. A
    // cell asserting only the code would pass for a statement refused
    // having written nothing, which is the case this one is not about.
    EXPECT_NE(r.child.out.response.find("id=2"), std::string::npos)
        << "the refusal did not name the held row, so the walk may not have reached it: "
        << r.child.out.response;

    // And the refusal is a failed statement inside an explicit transaction,
    // so the rows it did write stay and the client must ROLLBACK - §6's
    // failure atomicity, which is what the refusal falls back to.
    ASSERT_TRUE(KickUntil(*r.rig, 1, [&] { return r.child.done.load(std::memory_order_acquire); },
                          3000ms))
        << r.child.commit_out.response;
    EXPECT_NE(r.child.commit_out.response.find("aborted"), std::string::npos)
        << "the failed statement did not poison its transaction: "
        << r.child.commit_out.response;
    EXPECT_EQ(r.child.after_out.response.rfind("ROLLBACK", 0), 0u)
        << "a poisoned COMMIT refuses without unwinding, so the ROLLBACK is what ends it: "
        << r.child.after_out.response;

    // And what the ROLLBACK undid is the row the refused statement had
    // already written - which is the other half of "refused, not parked":
    // a statement that parked would have gone on to write the rest.
    r.parent.may_end.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*r.rig, 1,
                          [&] { return r.parent.after_done.load(std::memory_order_acquire); },
                          4000ms))
        << r.parent.end_out.response << " / " << r.parent.after_out.response;
    EXPECT_EQ(r.parent.end_out.response.rfind("COMMIT", 0), 0u) << r.parent.end_out.response;
    EXPECT_EQ(r.parent.after_out.response.find(",8"), std::string::npos)
        << "the victim's write survived its ROLLBACK: " << r.parent.after_out.response;
}

}  // namespace
}  // namespace kds::server
