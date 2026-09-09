// AO-S6e-b: `DROP TABLE` waits for a positioned reader **on a peer**, on
// the two-core rig (`instructions/v3.0.0/workorder-ao-m2-lock-family.md`
// AO-S6, the row's fourth cell; `workorder-av-two-core-rig.md`).
//
// **Why this cell has to be on the rig and not beside the others.** Every
// other wait in the dispatcher polls `TransactionManager::IsInFlight`,
// which walks *this core's* live set (`manager.hpp`) - so a holder running
// on another core reads as "not in flight" from the first poll, and a wait
// built on that predicate is a re-run per reactor iteration rather than a
// wait. The read borrow is exactly such a holder: reads run on the
// relation's owner, DDL runs on core 0 (CC13), and the two are different
// cores whenever the relation is a peer's. What the DDL waits on instead is
// the table's own slot, flipped by the release from whichever core releases
// and carried across by AU-S2's write-then-kick.
//
// **The reader's borrow is taken here directly, not by a running `SELECT`,
// and that is a property of the engine rather than a shortcut.** A local
// read is synchronous - `exec::Execute` drives its coroutine to completion
// inline - so no cell can hold a statement mid-walk on a reactor it shares
// with the thing it is testing. What is held is exactly what a walk takes:
// `Relation(oid)` in `IS` under a read holder id
// (`command_dispatcher.cpp`'s `ReadBorrow`), and
// `LockDeadlockTest.AReadDeclaresItsPositionAndGivesItBack` is what says a
// real read takes and releases it.

#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/txn/lock_table.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

// The reader's half: takes the position and holds it until the cell says
// to let go. The predicate is a member because `WaitUntil` holds a pointer
// across the park; the flags are atomic because the rig's own thread reads
// them while this runs on core 1's reactor.
struct Reader {
    txn::LockHoldings holdings;
    std::function<bool()> release_pred;
    std::atomic<bool> holding{false};
    std::atomic<bool> may_release{false};
    std::atomic<bool> released{false};
};

sched::Coro HoldPosition(txn::LockTable& table, Reader& r, catalog::Oid rel, std::uint64_t id) {
    auto took =
        table.Acquire(id, txn::LockKey::Relation(rel), txn::LockMode::kIntentionShared, r.holdings);
    if (!took.ok()) co_return took.status();
    if (!took.value().granted) {
        co_return Status::InvalidArgument("the reader's position was refused on the peer");
    }
    r.release_pred = [&r] { return r.may_release.load(std::memory_order_acquire); };
    r.holding.store(true, std::memory_order_release);
    co_await sched::WaitUntil{&r.release_pred};
    table.Release(id, r.holdings);
    r.released.store(true, std::memory_order_release);
    co_return Status::OK();
}

// The DDL's half, on core 0. It starts only once the reader holds, so the
// cell tests a drop that meets a position rather than a race between two
// reactors' first polls (AV-R3: a state a cell needs first is reached by a
// barrier).
struct Dropper {
    Session session;
    DispatchOutcome out;
    std::string sql;
    std::function<bool()> start_pred;
    const std::atomic<bool>* wait_for = nullptr;
    std::atomic<bool> done{false};
};

sched::Coro DropWhenHeld(CommandDispatcher& d, Dropper& x) {
    x.start_pred = [&x] { return x.wait_for->load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&x.start_pred};
    co_await d.DispatchAsync(x.sql, &x.session, &x.out);
    x.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

// ---- AT-S5: a write runs where the session is ---------------------------

TEST(ReadBorrowRigTest, APeerCreatesARelationAndCoreZeroResolvesItAtItsBoundary) {
    // The catalog pages had one writer until AT-S5. A `CREATE TABLE` on
    // core 1 writes them now, under the page latch, and core 0's next
    // statement resolves the relation through the schema word (AT-S2).
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    Session on_peer;
    const std::string created =
        rig->core(1).dispatcher().Dispatch("CREATE TABLE made_on_one (id int64, v int64) BTREE",
                                           &on_peer).response;
    ASSERT_EQ(created.rfind("CREATED", 0), 0u) << created;
    Session on_zero;
    const std::string described =
        rig->core(0).dispatcher().Dispatch("DESCRIBE made_on_one", &on_zero).response;
    EXPECT_NE(described.rfind("ERR", 0), 0u) << described;
}

TEST(ReadBorrowRigTest, ANamedKeyAdmitsOnAPeer) {
    // A peer refused a named key until AT-S5, because admitting one writes
    // the relation's `sys.tables` row. It admits it now under the page
    // latch, and the mark it advances is the one every core reads.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    ASSERT_EQ(d0.Dispatch("CREATE TABLE nk_peer (id int64, v int64) BTREE").response.rfind("CRE", 0),
              0u);
    Session on_peer;
    const std::string written =
        rig->core(1).dispatcher().Dispatch("INSERT INTO nk_peer VALUES (7, 1)", &on_peer).response;
    EXPECT_EQ(written.rfind("INSERTED", 0), 0u) << written;
    auto oid = rig->core(0).catalog().FindTableOidByName("nk_peer");
    ASSERT_TRUE(oid.ok());
    rig->core(0).catalog().Revalidate();
    auto row = rig->core(0).catalog().GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok());
    EXPECT_GE(row.value().next_id, 8u) << "the peer's admission did not move the shared mark";
}

TEST(ReadBorrowRigTest, TwoCoresCreatingRelationsIssueDistinctOids) {
    // D0, found by AT-S5's review: the object-oid sequence was one in-memory
    // counter *per catalog*, seeded from the pages on first use and
    // incremented in memory after that. Sound while core 0 alone created
    // objects, and two relations with one oid the moment a peer runs
    // `CREATE TABLE`.
    //
    // **The shape matters, and the first draft of this cell had it wrong.**
    // One create per core does not collide: each catalog seeds *lazily*, so
    // the second one to run scans the pages the first has already written
    // and starts above them. What collides is a core's **second** create -
    // it issues from a counter seeded before the other core's rows existed
    // and never re-read. So the cell is create-create-create, alternating,
    // and the third is where the two counters meet.
    //
    // **Mutation**: `if (false && oid_sequence_ != nullptr)` in
    // `GenerateUserOid`, which is the per-catalog counter exactly.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    Session a;
    Session b;
    const char* kSql[] = {"CREATE TABLE oid_zero_one (id int64, v int64) BTREE",
                          "CREATE TABLE oid_one_one (id int64, v int64) BTREE",
                          "CREATE TABLE oid_zero_two (id int64, v int64) BTREE"};
    const char* kName[] = {"oid_zero_one", "oid_one_one", "oid_zero_two"};
    const std::uint32_t kOn[] = {0, 1, 0};
    for (int n = 0; n < 3; ++n) {
        Session& s = kOn[n] == 0 ? a : b;
        const std::string made =
            rig->core(kOn[n]).dispatcher().Dispatch(kSql[n], &s).response;
        ASSERT_EQ(made.rfind("CREATED", 0), 0u) << kName[n] << ": " << made;
    }

    rig->core(0).catalog().Revalidate();
    std::vector<catalog::Oid> oids;
    for (const char* name : kName) {
        auto oid = rig->core(0).catalog().FindTableOidByName(name);
        ASSERT_TRUE(oid.ok()) << name << ": " << oid.status().message();
        oids.push_back(oid.value());
    }
    // Pairwise, so the message names which two met rather than only that
    // the set is small.
    for (std::size_t i = 0; i < oids.size(); ++i) {
        for (std::size_t j = i + 1; j < oids.size(); ++j) {
            EXPECT_NE(oids[i], oids[j])
                << kName[i] << " and " << kName[j] << " were issued one oid (" << oids[i] << ")";
        }
    }
}

TEST(ReadBorrowRigTest, APeersDeleteMarksAreSweptByTheCoreThatSweeps) {
    // D7, found by AT-S5's review. The purge runs on one core - a placement
    // since AT-S5, so two cores do not walk the same chains at once - and
    // its gate was a counter *per catalog*. A peer's `DROP TABLE` raised its
    // own core's counter, core 0's stayed at zero, its gate returned early
    // and the marks waited for the next mount. The counter is the
    // instance's now (`Catalog::SetMarkCounter`).
    //
    // **Mutation**: drop `config.mark_counter` from the rig's config and
    // core 0 purges nothing.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    Session on_peer;
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    ASSERT_EQ(d1.Dispatch("CREATE TABLE swept (id int64, v int64) BTREE", &on_peer)
                  .response.rfind("CREATED", 0),
              0u);
    const std::string dropped = d1.Dispatch("DROP TABLE swept", &on_peer).response;
    ASSERT_NE(dropped.rfind("ERR", 0), 0u) << dropped;

    // Core 0's own DDL is what reaches `EndDdlScope`, which is where the
    // sweep runs. Nothing about this statement touches the peer's rows; it
    // is the gate that has to see them.
    Session on_zero;
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    ASSERT_EQ(d0.Dispatch("CREATE TABLE sweeper (id int64, v int64) BTREE", &on_zero)
                  .response.rfind("CREATED", 0),
              0u);
    const std::string meta = d0.Dispatch("SHOW META", &on_zero).response;
    EXPECT_NE(meta.find("catalog_marks_purged="), std::string::npos) << meta;
    EXPECT_EQ(meta.find("catalog_marks_purged=0"), std::string::npos)
        << "core 0's sweep did not see the peer's marks: " << meta;
}

TEST(ReadBorrowRigTest, ADropOnCoreZeroWaitsForAPositionedReaderOnAPeer) {
    TwoCoreRig::Options options;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());

    // `r` on core 1, by the recipe every cross-owner cell here uses: one
    // relation placed on the creating core, then the rotation puts the next
    // on the peer.
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    rig->core(0).catalog().SetPlacementPolicy(catalog::PlacementPolicy::kCreatingCore);
    ASSERT_EQ(d0.Dispatch("CREATE TABLE anchor (id int64, v int64) BTREE").response.rfind("CRE", 0),
              0u);
    rig->core(0).catalog().SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    ASSERT_EQ(d0.Dispatch("CREATE TABLE r (id int64, v int64) BTREE").response.rfind("CRE", 0), 0u);

    auto oid = rig->core(0).catalog().FindTableOidByName("r");
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto row = rig->core(0).catalog().GetSysTableRow(oid.value());
    ASSERT_TRUE(row.ok()) << row.status().message();
    ASSERT_EQ(row.value().owner_core, 1u) << "r is not the peer's, so no reader of it is either";
    ASSERT_TRUE(rig->store().FlushPages(catalog::kEveryCatalogPage).ok());

    Reader reader;
    Dropper drop;
    drop.sql = "DROP TABLE r";
    drop.wait_for = &reader.holding;
    const std::uint64_t reader_id = txn::kReadHolderBit | 11;
    const txn::LockKey rel = txn::LockKey::Relation(oid.value());

    rig->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                            HoldPosition(rig->locks(), reader, oid.value(), reader_id)));
    rig->core(0).scheduler().Submit(sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                                                        DropWhenHeld(d0, drop)));
    rig->Start();

    ASSERT_TRUE(Within(2000ms, [&] { return reader.holding.load(std::memory_order_acquire); }))
        << "the peer never took the position the drop is supposed to wait for";

    // **The drop asked, was refused, and parked** - and that is read from
    // the table rather than from the absence of a reply, which a reactor
    // that had not run yet would also produce. The registration is the
    // wait: one waiter on the relation entry, left by a `TryAcquire` that
    // asked for a wake.
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return rig->locks().WaiterCount(rel) == 1; }))
        << "the drop never registered a wake on the relation it could not take";
    EXPECT_FALSE(drop.done.load(std::memory_order_acquire))
        << "the drop ran over a positioned reader on the peer: " << drop.out.response;

    // The release happens on core 1 and the waiter is on core 0: the flip
    // crosses cores, and the kick that carries it is AU-S2's.
    reader.may_release.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return reader.released.load(std::memory_order_acquire); }))
        << "the peer never released";
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return drop.done.load(std::memory_order_acquire); },
                          5000ms))
        << "the drop never resumed after the peer's reader released";
    EXPECT_EQ(drop.out.response.rfind("DROPPED TABLE r", 0), 0u) << drop.out.response;
    EXPECT_EQ(rig->locks().WaiterCount(rel), 0u) << "the wake registration outlived its wait";

    rig->Stop();
}

}  // namespace
}  // namespace kds::server
