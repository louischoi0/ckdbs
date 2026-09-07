// AO-S5(b): the cross-core foreign-key wait, on the two-core rig
// (`instructions/v3.0.0/workorder-ao-m2-lock-family.md`, the AO-S5(b)
// section). A child on core 1 whose parent row is being written by an
// in-flight transaction on core 0 used to be refused `TxnConflict` by the
// parent's busy answer; now the parent's core parks the probe until the
// writer decides and answers from a fresh view. The same shape as AO-S3's
// same-core cells (`Txn2pcBlockedWriterTest`), with the parent on the
// other core.

#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/session.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

// Core 0's session: opens a transaction, inserts the parent row, holds it
// until told how to end.
struct ParentWriter {
    Session session;
    DispatchOutcome begin_out;
    DispatchOutcome insert_out;
    DispatchOutcome end_out;
    std::function<bool()> end_pred;
    std::atomic<bool> holding{false};
    std::atomic<bool> may_end{false};
    std::atomic<bool> ended{false};
    std::string ending = "COMMIT";
};

sched::Coro WriteParent(CommandDispatcher& d, ParentWriter& p) {
    co_await d.DispatchAsync("BEGIN", &p.session, &p.begin_out);
    co_await d.DispatchAsync("INSERT INTO p VALUES (7, 0)", &p.session, &p.insert_out);
    p.holding.store(true, std::memory_order_release);
    p.end_pred = [&p] { return p.may_end.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&p.end_pred};
    co_await d.DispatchAsync(p.ending, &p.session, &p.end_out);
    p.ended.store(true, std::memory_order_release);
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

    void Submit(const char* ending) {
        parent.ending = ending;
        rig->core(0).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, WriteParent(rig->core(0).dispatcher(), parent)));
        rig->core(1).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, WriteChild(rig->core(1).dispatcher(), child)));
        rig->Start();
    }

    ParentWriter parent;
    ChildWriter child;
    std::unique_ptr<TwoCoreRig> rig;
};

// The parent held, the child started, and the child parked on the probe:
// what both cells begin from. Returns whether it was reached.
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
    EXPECT_EQ(r.rig->core(0).fk_probe_server()->probe_wait_expiries(), 0u)
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

}  // namespace
}  // namespace kds::server
