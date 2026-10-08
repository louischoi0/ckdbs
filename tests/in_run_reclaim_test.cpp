// **BF-S5: reclaim within the run** (BF-R9, BF-Q9 (b), BF-Q11 (a);
// `instructions/v3.0.0/workorder-bf-drop-table-page-reclaim.md`).
//
// A drop committed during the run is queued at its commit and freed once
// the durable redo start is past the commit and every statement that could
// have bound the relation has ended - the statement epoch each core
// publishes at its statement head, before it reads the schema word.
//
// The rig cells run two cores' dispatchers over one store on the test's
// thread (`TwoCoreRig`, not started), with the reclaimer and the epochs
// wired as `Expeditor` wires them. The durable redo start is passed as
// "past everything": what is under test is the epoch, and BF-S3's cells
// own the gate.

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include "file_rig_crash.hpp"
#include "reclaim_census.hpp"
#include "two_core_rig.hpp"

#include "kds/base/current_core.hpp"
#include "kds/server/page_reclaim.hpp"
#include "kds/server/session.hpp"
#include "kds/server/statement_epoch.hpp"
#include "kds/storage/page_header.hpp"

namespace kds::server {
namespace {

using crash_rig::Ok;
using reclaim_census::Fill;

constexpr wal::Lsn kPastEverything = std::numeric_limits<wal::Lsn>::max();

// Two cores' dispatchers, one reclaimer and one set of epoch slots.
class InRunReclaimRig {
public:
    InRunReclaimRig() {
        auto opened = TwoCoreRig::Open();
        EXPECT_TRUE(opened.ok()) << opened.status().message();
        rig_ = std::move(opened.value());
        reclaimer_.emplace(rig_->core(0).catalog(), rig_->store(), counters_, /*log=*/nullptr);
        for (std::uint32_t core = 0; core < 2; ++core) {
            rig_->core(core).dispatcher().SetStatementEpochs(&epochs_);
            rig_->core(core).dispatcher().SetReclaimQueue(&*reclaimer_);
        }
    }

    CommandDispatcher& d(std::uint32_t core) { return rig_->core(core).dispatcher(); }
    catalog::Catalog& catalog() { return rig_->core(0).catalog(); }
    storage::DevicePageStore& store() { return rig_->store(); }
    const ReclaimCounters& counters() const { return counters_; }

    // One reclaim step on core 0, the gate passed.
    void Step() {
        CurrentCoreGuard as(0);
        ASSERT_TRUE(reclaimer_->Step(kPastEverything, &epochs_).ok());
    }
    void Settle() {
        for (int i = 0; i < 1000 && counters_.pending.load() != 0; ++i) Step();
    }

    std::set<PageId> PagesOf(std::uint64_t oid) { return reclaim_census::PagesOf(store(), oid); }

private:
    std::unique_ptr<TwoCoreRig> rig_;
    StatementEpochs epochs_{2};
    ReclaimCounters counters_;
    std::optional<PageReclaimer> reclaimer_;
};

std::uint64_t OidOf(CommandDispatcher& d, const std::string& table) {
    return reclaim_census::DescribeField(d, table, "oid");
}

TEST(InRunReclaimTest, ADropCommittedInTheRunIsFreedInTheRun) {
    InRunReclaimRig rig;
    CurrentCoreGuard as(0);
    Ok(rig.d(0), "CREATE TABLE t (id int64, v varchar)");
    Fill(rig.d(0), "t", 1000);
    const std::uint64_t oid = OidOf(rig.d(0), "t");
    const std::set<PageId> pages = rig.PagesOf(oid);
    ASSERT_FALSE(pages.empty());
    Ok(rig.d(0), "DROP TABLE t");
    EXPECT_EQ(rig.counters().pending.load(), 1u) << "the commit arm queued nothing";
    rig.Settle();
    EXPECT_EQ(rig.counters().pending.load(), 0u);
    for (const PageId id : pages) EXPECT_FALSE(rig.store().IsAllocated(id)) << "page " << id;
}

TEST(InRunReclaimTest, ADropRolledBackWithItsReclaimQueuedFreesNothing) {
    // Only the commit arm queues (BF-R9): a rollback leaves nothing behind,
    // and the restored relation's pages are its own.
    InRunReclaimRig rig;
    CurrentCoreGuard as(0);
    Ok(rig.d(0), "CREATE TABLE t (id int64, v varchar)");
    Fill(rig.d(0), "t", 1000);
    const std::set<PageId> pages = rig.PagesOf(OidOf(rig.d(0), "t"));
    Ok(rig.d(0), "BEGIN");
    Ok(rig.d(0), "DROP TABLE t");
    Ok(rig.d(0), "ROLLBACK");
    rig.Settle();
    EXPECT_EQ(rig.counters().pending.load(), 0u);
    for (const PageId id : pages) EXPECT_TRUE(rig.store().IsAllocated(id)) << "page " << id;
    EXPECT_EQ(rig.d(0).Dispatch("SELECT COUNT(*) FROM t").response, "count(*)\\n1000");
}

// The Census B paths, each in flight on core 1 across the drop's commit on
// core 0: core 1's statement revalidates, the seam drops the relation on
// core 0 and runs the reclaim, and the statement then binds from the memo it
// built before the drop. The reclaim must wait for it - at every seam:
// just after the read and before the publish the slot reads `kEntering`,
// after it the slot holds the word the memo was built at, below the drop's.
// The first is what tells a publish before the read from one after it.
struct InFlightCase {
    const char* name;
    const char* statement;
};

void RunInFlight(const InFlightCase& c, CommandDispatcher::EpochSeam at) {
    const char* where = at == CommandDispatcher::EpochSeam::kAfterRead       ? ", after the read"
                        : at == CommandDispatcher::EpochSeam::kBeforePublish ? ", before the publish"
                                                                             : ", after the publish";
    SCOPED_TRACE(std::string(c.name) + where);
    InRunReclaimRig rig;
    {
        CurrentCoreGuard as(0);
        Ok(rig.d(0), "CREATE TABLE p (id int64, v varchar)");
        Ok(rig.d(0), "CREATE TABLE t (id int64, pid int64 REFERENCES p)");
        Fill(rig.d(0), "p", 10);
        for (int k = 1; k <= 300; ++k) {
            Ok(rig.d(0), "INSERT INTO t VALUES (" + std::to_string(1 + k % 10) + ")");
        }
    }
    const std::uint64_t oid = OidOf(rig.d(0), "t");
    const std::set<PageId> pages = rig.PagesOf(oid);
    {
        CurrentCoreGuard as(1);
        // Core 1's memo holds t, and p's `fkeys_in` names it.
        (void)rig.d(1).Dispatch("SELECT COUNT(*) FROM t");
        (void)rig.d(1).Dispatch("SELECT COUNT(*) FROM p");
    }
    bool fired = false;
    rig.d(1).SetEpochSeamForTest(at, [&] {
        if (fired) return;
        fired = true;
        CurrentCoreGuard as(0);
        Ok(rig.d(0), "DROP TABLE t");
        rig.Settle();
        // Core 1's statement holds a memo from before the drop.
        for (const PageId id : pages) EXPECT_TRUE(rig.store().IsAllocated(id)) << "page " << id;
        // And what a free would hand out, a new relation takes.
        Ok(rig.d(0), "CREATE TABLE u (id int64, v varchar)");
        Fill(rig.d(0), "u", 300);
    });
    std::string reply;
    {
        CurrentCoreGuard as(1);
        reply = rig.d(1).Dispatch(c.statement).response;
    }
    rig.d(1).SetEpochSeamForTest(at, nullptr);
    ASSERT_TRUE(fired);
    EXPECT_EQ(reply.rfind("ERR", 0) == 0 && reply.find("Corruption") != std::string::npos, false)
        << reply;
    rig.Settle();
    EXPECT_EQ(rig.counters().pending.load(), 0u) << "the reclaim never ran after the statement";
    for (const PageId id : pages) {
        if (!rig.store().IsAllocated(id)) continue;
        auto page = rig.store().GetForRead(id);
        ASSERT_TRUE(page.ok());
        EXPECT_NE(storage::GetOwnerOid(page.value().bytes()), oid) << "page " << id;
    }
}

void RunInFlightAtEverySeam(const InFlightCase& c) {
    RunInFlight(c, CommandDispatcher::EpochSeam::kAfterRead);
    RunInFlight(c, CommandDispatcher::EpochSeam::kBeforePublish);
    RunInFlight(c, CommandDispatcher::EpochSeam::kAfterPublish);
}

TEST(InRunReclaimTest, AStaleMemoReadInFlightHoldsTheReclaim) {
    RunInFlightAtEverySeam({"stale-memo read", "SELECT COUNT(*) FROM t"});
}

TEST(InRunReclaimTest, ADescribeInFlightHoldsTheReclaim) {
    RunInFlightAtEverySeam({"DESCRIBE", "DESCRIBE t"});
}

TEST(InRunReclaimTest, AReverseForeignKeyWalkInFlightHoldsTheReclaim) {
    RunInFlightAtEverySeam({"reverse foreign-key walk", "DELETE FROM p WHERE id = 3"});
}

TEST(InRunReclaimTest, AChildsOpenCreateHoldsItsParentsIntention) {
    // BF-Q11 (a): a child's `CREATE TABLE` takes its parent's `IS` with the
    // foreign key it declares, held to its decide, so a parent's drop
    // arriving between the child's lookup and its foreign-key write is
    // refused its `X`. That window has no seam on this rig - the drop's
    // first RESTRICT ask already sees the child's foreign-key row, which a
    // catalog write makes visible at once - so the cell asks the lock table
    // directly, and then checks the end-to-end refusal.
    InRunReclaimRig rig;
    {
        CurrentCoreGuard as(0);
        Ok(rig.d(0), "CREATE TABLE p (id int64, v int64)");
    }
    const std::uint64_t parent = OidOf(rig.d(0), "p");
    Session child;
    {
        CurrentCoreGuard as(1);
        Ok(rig.d(1), "BEGIN", &child);
        Ok(rig.d(1), "CREATE TABLE c (id int64, pid int64 REFERENCES p)", &child);
        ASSERT_NE(child.transaction(), nullptr);
        EXPECT_TRUE(child.transaction()->borrows().Holds(
            txn::LockKey::Relation(static_cast<catalog::Oid>(parent))))
            << "the open child create holds no intention on its parent";
    }
    {
        CurrentCoreGuard as(0);
        const std::string held = rig.d(0).Dispatch("DROP TABLE p").response;
        EXPECT_EQ(held.rfind("ERR", 0), 0u) << "the parent dropped under an open child create: " << held;
    }
    {
        CurrentCoreGuard as(1);
        Ok(rig.d(1), "COMMIT", &child);
    }
    CurrentCoreGuard as(0);
    const std::string refused = rig.d(0).Dispatch("DROP TABLE p").response;
    EXPECT_NE(refused.find("referenced by a foreign key"), std::string::npos) << refused;
}

TEST(InRunReclaimTest, AForeignKeyCreatedBeforeTheDropsExclusiveStillRefusesIt) {
    // BF-Q11 (a)'s second ask: a child committed between the drop's first
    // RESTRICT ask and its `X` grant is refused under the `X`, or the parent
    // is reclaimed under a foreign key that names it.
    InRunReclaimRig rig;
    {
        CurrentCoreGuard as(0);
        Ok(rig.d(0), "CREATE TABLE p (id int64, v int64)");
    }
    bool fired = false;
    rig.d(0).SetBeforeDropExclusiveForTest([&] {
        if (fired) return;
        fired = true;
        CurrentCoreGuard as(1);
        Ok(rig.d(1), "CREATE TABLE c (id int64, pid int64 REFERENCES p)");
    });
    std::string reply;
    {
        CurrentCoreGuard as(0);
        reply = rig.d(0).Dispatch("DROP TABLE p").response;
    }
    rig.d(0).SetBeforeDropExclusiveForTest(nullptr);
    ASSERT_TRUE(fired);
    EXPECT_NE(reply.find("referenced by a foreign key on 'c'"), std::string::npos) << reply;
    CurrentCoreGuard as(0);
    EXPECT_EQ(rig.d(0).Dispatch("SELECT COUNT(*) FROM p").response, "count(*)\\n0");
}

TEST(InRunReclaimTest, ABindPastAGateDefectIsRefusedByTheRootCheck) {
    // BF-R10's walk check is defence in depth: it answers only when the
    // epoch gate is wrong. Core 1 is left out of the epochs - a binder the
    // gate cannot see - so the reclaim frees t, and u takes its pages,
    // under core 1's memo. Core 1's bind must be refused naming the
    // reclaim rather than read u's rows or a freed page.
    InRunReclaimRig rig;
    rig.d(1).SetStatementEpochs(nullptr);
    {
        CurrentCoreGuard as(0);
        Ok(rig.d(0), "CREATE TABLE t (id int64, v varchar)");
        Fill(rig.d(0), "t", 300);
    }
    const std::uint64_t oid = OidOf(rig.d(0), "t");
    const std::set<PageId> pages = rig.PagesOf(oid);
    PageId roots[2] = {kInvalidPageId, kInvalidPageId};
    {
        CurrentCoreGuard as(0);
        auto access = rig.catalog().InitTableAccess(oid);
        ASSERT_TRUE(access.ok()) << access.status().message();
        roots[0] = access.value()->anchor_page_id;
        roots[1] = access.value()->desc_page_id;
    }
    {
        CurrentCoreGuard as(1);
        (void)rig.d(1).Dispatch("SELECT COUNT(*) FROM t");
    }
    bool fired = false;
    rig.d(1).SetEpochSeamForTest(CommandDispatcher::EpochSeam::kAfterPublish, [&] {
        if (fired) return;
        fired = true;
        CurrentCoreGuard as(0);
        Ok(rig.d(0), "DROP TABLE t");
        rig.Settle();
        for (const PageId id : pages) EXPECT_FALSE(rig.store().IsAllocated(id)) << "page " << id;
        Ok(rig.d(0), "CREATE TABLE u (id int64, v varchar)");
        Fill(rig.d(0), "u", 300);
        // The premise: u took the two pages the root check reads.
        const std::set<PageId> taken = rig.PagesOf(OidOf(rig.d(0), "u"));
        for (const PageId id : roots) EXPECT_EQ(taken.count(id), 1u) << "page " << id;
    });
    std::string reply;
    {
        CurrentCoreGuard as(1);
        reply = rig.d(1).Dispatch("SELECT COUNT(*) FROM t").response;
    }
    rig.d(1).SetEpochSeamForTest(CommandDispatcher::EpochSeam::kAfterPublish, nullptr);
    ASSERT_TRUE(fired);
    EXPECT_NE(reply.find("was reclaimed"), std::string::npos) << reply;
}

}  // namespace
}  // namespace kds::server
