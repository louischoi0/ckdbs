// AT-S5e and AT-0 item 13: a DDL that reads a relation's rows to build a
// secondary structure fences off its writers **on every core**, on the
// two-core rig (`instructions/v3.0.0/workorder-at-m3-uniformity.md`,
// AT-S5e; `workorder-av-two-core-rig.md`).
//
// **Why these cells have to be on the rig.** A writer that meets another
// transaction's relation `X` would, by the row-level wait, poll
// `TransactionManager::IsInFlight` - one core's live set - and read a DDL
// running on the other core as "not in flight": the write would be refused
// rather than held, and before AT-S5e it was worse, admitted into an index
// being built (D6) or past a cabin being built (the unfenced-build bug).
// What a writer waits on is the table's own slot, flipped by the release
// from whichever core releases, and only two reactors on two threads
// exercise that.
//
// The three cells:
//
//   - **An INSERT on core 1 waits for a CREATE INDEX on core 0, and its
//     row is in the index** - the work order's own done-condition. The
//     index is held open by an explicit transaction, which is the only way
//     a synchronous build stays open long enough to meet anything.
//   - **A CREATE ASSERTION on core 0 waits for a writer on core 1**, and
//     then counts that writer's row: the build's side of the same fence.
//   - **A writer parks on a relation `X` before its admission**, which is
//     item 13's other half: an `INSERT` takes the relation's `IX` ahead of
//     the assertion check, so a build cannot slip between the two.

#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "kds/exec/assertion_catalog.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/txn/lock_table.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

// One statement run on a core's reactor once `go` is set, through the
// served path - `DispatchAsync`, which is the path that can park.
struct Statement {
    Session own;
    Session* session = &own;  // another test-scope session, where one is named
    DispatchOutcome out;
    std::string sql;
    std::function<bool()> start_pred;
    std::atomic<bool> go{false};
    std::atomic<bool> done{false};
};

sched::Coro RunWhenGo(CommandDispatcher& d, Statement& x) {
    x.start_pred = [&x] { return x.go.load(std::memory_order_acquire); };
    co_await sched::WaitUntil{&x.start_pred};
    co_await d.DispatchAsync(x.sql, x.session, &x.out);
    x.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

void Submit(TwoCoreRig& rig, std::uint32_t core, Statement& x) {
    rig.core(core).scheduler().Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground, RunWhenGo(rig.core(core).dispatcher(), x)));
}

// `r` on core 1: `kRotate` at two cores places every relation there.
catalog::Oid PeerRelation(TwoCoreRig& rig, const std::string& name) {
    const std::string made = rig.core(0)
                                 .dispatcher()
                                 .Dispatch("CREATE TABLE " + name + " (id int64, v int64) BTREE")
                                 .response;
    EXPECT_EQ(made.rfind("CREATED", 0), 0u) << made;
    auto oid = rig.core(0).catalog().FindTableOidByName(name);
    EXPECT_TRUE(oid.ok()) << oid.status().message();
    auto row = rig.core(0).catalog().GetSysTableRow(oid.value());
    EXPECT_TRUE(row.ok());
    EXPECT_EQ(row.value().owner_core, 1u) << name << " is not the peer's";
    // Row ids for the peer's inserts: this rig leaves the refill tick off.
    EXPECT_TRUE(rig.FundPeerRelation(oid.value(), 64).ok());
    return oid.value();
}

TEST(DdlFenceRigTest, AnInsertOnAPeerWaitsForACreateIndexOnCoreZeroAndItsRowIsInTheIndex) {
    // **D6.** Until AT-S5e a local `CREATE INDEX` opened no window and a
    // peer's window was invisible to core 0, so a row written on another
    // core during a backfill was missing from the finished index - which
    // then reported itself complete, and a descent missed a row that
    // exists. The build takes the relation `X` now and the writer's `IX`
    // waits on it, from the other core.
    //
    // **Mutations**, measured: `HandleIndex` taking no relation `X`, and
    // `BorrowChain` registering no wake on a refused relation `IX` - each
    // killed 1 in 1.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    const catalog::Oid oid = PeerRelation(*rig, "fenced_ix");
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    for (int v : {10, 20, 30}) {
        ASSERT_EQ(rig->core(1)
                      .dispatcher()
                      .Dispatch("INSERT INTO fenced_ix VALUES (" + std::to_string(v) + ")")
                      .response.rfind("INSERTED", 0),
                  0u);
    }

    // The build, held open by its transaction on core 0.
    Session ddl;
    ASSERT_EQ(d0.Dispatch("BEGIN", &ddl).response.rfind("BEGIN", 0), 0u);
    const std::string created = d0.Dispatch("CREATE INDEX fix ON fenced_ix (v)", &ddl).response;
    ASSERT_EQ(created.rfind("CREATED INDEX", 0), 0u) << created;

    Statement insert;
    insert.sql = "INSERT INTO fenced_ix VALUES (99)";
    Statement commit;
    commit.sql = "COMMIT";
    Submit(*rig, 1, insert);
    rig->Start();
    insert.go.store(true, std::memory_order_release);

    const txn::LockKey rel = txn::LockKey::Relation(oid);
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return rig->locks().WaiterCount(rel) == 1; }))
        << "the insert never parked on the relation the build holds: " << insert.out.response;
    EXPECT_FALSE(insert.done.load(std::memory_order_acquire))
        << "the insert ran past a CREATE INDEX on the other core: " << insert.out.response;

    // `COMMIT` on core 0's own reactor, which is where the session lives.
    commit.session = &ddl;
    Submit(*rig, 0, commit);
    commit.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return commit.done.load(std::memory_order_acquire); }))
        << "the build's commit never ran";
    ASSERT_EQ(commit.out.response.rfind("COMMIT", 0), 0u) << commit.out.response;
    ASSERT_TRUE(
        KickUntil(*rig, 1, [&] { return insert.done.load(std::memory_order_acquire); }, 5000ms))
        << "the insert never resumed after the build committed";
    EXPECT_EQ(insert.out.response.rfind("INSERTED", 0), 0u) << insert.out.response;
    rig->Stop();

    // **In the index**: a keyed read goes through it and finds the row, and
    // the index counts all four.
    const std::string plan =
        rig->core(1).dispatcher().Dispatch("ANALYZE SELECT * FROM fenced_ix WHERE v = 99").response;
    EXPECT_NE(plan.find("IndexProbe"), std::string::npos) << plan;
    const std::string found =
        rig->core(1).dispatcher().Dispatch("SELECT * FROM fenced_ix WHERE v = 99").response;
    EXPECT_NE(found.find(",99"), std::string::npos) << "the index missed the fenced row: " << found;
    const std::string shown = rig->core(1).dispatcher().Dispatch("SHOW INDEXES").response;
    EXPECT_NE(shown.find("entries=4"), std::string::npos) << shown;
}

TEST(DdlFenceRigTest, ACreateAssertionOnCoreZeroWaitsForAWriterOnAPeerAndCountsItsRow) {
    // **AT-0 item 13, the build's side.** A writer on core 1 holds the
    // relation's `IX` in an open transaction; `CREATE ASSERTION` on core 0
    // asks for the relation `X` and parks on the table's slot until the
    // writer commits, then builds - and the cabin counts the writer's row,
    // so the next row in its group is refused.
    //
    // **Mutation**, measured: the build taking no relation `X`, killed 1 in
    // 1.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    const catalog::Oid oid = PeerRelation(*rig, "fenced_cap");
    CommandDispatcher& d1 = rig->core(1).dispatcher();

    Session writer;
    ASSERT_EQ(d1.Dispatch("BEGIN", &writer).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(d1.Dispatch("INSERT INTO fenced_cap VALUES (7)", &writer).response.rfind("INSERTED", 0),
              0u);

    Statement create;
    create.sql = "CREATE ASSERTION one ON fenced_cap GROUP BY (v) CHECK COUNT(*) <= 1";
    Statement commit;
    commit.sql = "COMMIT";
    commit.session = &writer;
    Submit(*rig, 0, create);
    rig->Start();
    create.go.store(true, std::memory_order_release);

    const txn::LockKey rel = txn::LockKey::Relation(oid);
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return rig->locks().WaiterCount(rel) == 1; }))
        << "the build never parked on the writer's relation: " << create.out.response;
    EXPECT_FALSE(create.done.load(std::memory_order_acquire)) << create.out.response;

    Submit(*rig, 1, commit);
    commit.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return commit.done.load(std::memory_order_acquire); }));
    ASSERT_EQ(commit.out.response.rfind("COMMIT", 0), 0u) << commit.out.response;
    ASSERT_TRUE(
        KickUntil(*rig, 0, [&] { return create.done.load(std::memory_order_acquire); }, 5000ms))
        << "the build never resumed after the writer committed";
    EXPECT_EQ(create.out.response.rfind("CREATED ASSERTION", 0), 0u) << create.out.response;
    EXPECT_NE(create.out.response.find("rows=1"), std::string::npos) << create.out.response;
    rig->Stop();

    const std::string second = d1.Dispatch("INSERT INTO fenced_cap VALUES (7)").response;
    EXPECT_NE(second.find("ASSERTION_VIOLATION"), std::string::npos)
        << "the build did not count the writer's row: " << second;
}

TEST(DdlFenceRigTest, AWriterParksOnARelationXBeforeItsAssertionAdmission) {
    // **AT-0 item 13, the writer's side.** An `INSERT` took its relation
    // `IX` at its first id borrow, *after* its assertion admission - so a
    // writer could be admitted against no assertion, let a build take the
    // `X`, finish and publish, and then place a row the cabin never
    // counted. The `IX` is asked ahead of the admission now
    // (`InsertParsed`), so a writer that meets a relation `X` has checked
    // nothing yet: the assertion's check counter reads 0 while it waits.
    //
    // The `X` is taken directly on the instance's table under an id no
    // statement uses, which is what a DDL's borrow is to the writer.
    //
    // **Mutation**, measured: the ask removed from `InsertParsed`, leaving
    // the id borrow's `IX` after the admission, killed 1 in 1.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    const catalog::Oid oid = PeerRelation(*rig, "early_ix");
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    const std::string declared =
        d0.Dispatch("CREATE ASSERTION cap ON early_ix GROUP BY (v) CHECK COUNT(*) <= 5").response;
    ASSERT_EQ(declared.rfind("CREATED ASSERTION", 0), 0u) << declared;
    auto defs = exec::ListAssertions(rig->core(0).catalog(), rig->store());
    ASSERT_TRUE(defs.ok() && defs.value().size() == 1);
    const std::uint64_t assertion_id = defs.value().front().id;
    exec::AssertionEnforcer& registry = d0.assertions();
    ASSERT_EQ(&registry, &rig->core(1).dispatcher().assertions()) << "the rig's cores hold two";

    const txn::LockKey rel = txn::LockKey::Relation(oid);
    constexpr std::uint64_t kDdlHolder = 424242;
    txn::LockHoldings ddl_holdings;
    auto held = rig->locks().TryAcquire(kDdlHolder, rel, txn::LockMode::kExclusive, ddl_holdings);
    ASSERT_TRUE(held.ok() && held.value());

    Statement insert;
    insert.sql = "INSERT INTO early_ix VALUES (3)";
    Submit(*rig, 1, insert);
    rig->Start();
    insert.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return rig->locks().WaiterCount(rel) == 1; }))
        << "the insert never parked on the relation `X`: " << insert.out.response;
    const auto parked = registry.CountersOf(assertion_id);
    ASSERT_TRUE(parked.has_value());
    EXPECT_EQ(parked->checks, 0u) << "the writer was admitted before it asked for the relation";

    rig->locks().Release(kDdlHolder, ddl_holdings);
    ASSERT_TRUE(
        KickUntil(*rig, 1, [&] { return insert.done.load(std::memory_order_acquire); }, 5000ms))
        << "the insert never resumed after the relation was released";
    EXPECT_EQ(insert.out.response.rfind("INSERTED", 0), 0u) << insert.out.response;
    rig->Stop();
    const auto after = registry.CountersOf(assertion_id);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->checks, 1u);
}

// One statement on a started rig's core, run to its end: submitted, told to
// go, and waited for by kicking that core.
std::string RunOn(TwoCoreRig& rig, std::uint32_t core, Session* session, const std::string& sql) {
    auto x = std::make_unique<Statement>();
    x->sql = sql;
    if (session != nullptr) x->session = session;
    Submit(rig, core, *x);
    x->go.store(true, std::memory_order_release);
    EXPECT_TRUE(KickUntil(rig, core, [&] { return x->done.load(std::memory_order_acquire); },
                          5000ms))
        << sql << " never finished on core " << core;
    return x->out.response;
}

TEST(DdlFenceRigTest, ADropIndexRolledBackOnCoreZeroKeepsTheRowAPeerWroteWhileItWasOpen) {
    // **The AT-S5e review's C1.** A `DROP INDEX` inside a transaction on
    // core 0 delete-marks the index; an `INSERT` on core 1 resolves the
    // relation while the drop is open, and core 1 cannot see core 0's
    // deleter in flight (DT9's predicate is one core's), so its memo leaves
    // the index out. The insert parks on the relation `X`. `ROLLBACK`
    // restores the index - and the insert, woken, must re-resolve before it
    // writes, or its row is missing from the index the rollback kept. What
    // makes it re-resolve is the decide moving the schema word **before**
    // it releases the borrows (`Transaction::NoteWroteCatalog`); the DDL
    // path's own bump comes after the release, in a race with the woken
    // insert's re-run - so the cell runs the sequence for several rounds,
    // and every round's row must be in the index.
    //
    // **Mutation**, measured: the pre-release word move removed from
    // `TransactionManager::Abort` - killed 5 runs in 5 at this round count,
    // where one round killed it once in three.
    constexpr int kRounds = 8;
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    const catalog::Oid oid = PeerRelation(*rig, "dropped_ix");
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    for (int v : {10, 20, 30}) {
        ASSERT_EQ(rig->core(1)
                      .dispatcher()
                      .Dispatch("INSERT INTO dropped_ix VALUES (" + std::to_string(v) + ")")
                      .response.rfind("INSERTED", 0),
                  0u);
    }
    ASSERT_EQ(d0.Dispatch("CREATE INDEX dix ON dropped_ix (v)").response.rfind("CREATED INDEX", 0),
              0u);
    const txn::LockKey rel = txn::LockKey::Relation(oid);
    rig->Start();

    for (int round = 0; round < kRounds; ++round) {
        Session ddl;
        ASSERT_EQ(RunOn(*rig, 0, &ddl, "BEGIN").rfind("BEGIN", 0), 0u);
        const std::string dropped = RunOn(*rig, 0, &ddl, "DROP INDEX dix");
        ASSERT_NE(dropped.find("DROPPED INDEX"), std::string::npos) << dropped;

        Statement insert;
        insert.sql = "INSERT INTO dropped_ix VALUES (" + std::to_string(100 + round) + ")";
        Submit(*rig, 1, insert);
        insert.go.store(true, std::memory_order_release);
        ASSERT_TRUE(KickUntil(*rig, 1, [&] { return rig->locks().WaiterCount(rel) == 1; }))
            << "round " << round << ": the insert never parked on the relation the drop holds: "
            << insert.out.response;

        const std::string rolled = RunOn(*rig, 0, &ddl, "ROLLBACK");
        ASSERT_EQ(rolled.rfind("ROLLBACK", 0), 0u) << rolled;
        ASSERT_TRUE(KickUntil(*rig, 1, [&] { return insert.done.load(std::memory_order_acquire); },
                              5000ms))
            << "round " << round << ": the insert never resumed after the rollback";
        ASSERT_EQ(insert.out.response.rfind("INSERTED", 0), 0u) << insert.out.response;
    }
    rig->Stop();

    const std::string shown = rig->core(1).dispatcher().Dispatch("SHOW INDEXES").response;
    EXPECT_NE(shown.find("name=dix"), std::string::npos) << "the rollbacks lost the index: " << shown;
    EXPECT_NE(shown.find("entries=" + std::to_string(3 + kRounds)), std::string::npos)
        << "a row written while a drop was open is missing from the index it kept: " << shown;
}

TEST(DdlFenceRigTest, AnUpdateOnAPeerWaitsForACreateIndexOnCoreZeroAndMaintainsIt) {
    // The per-row path: `WHERE id = k` declares no range (a window of one
    // key stays per row), so the `UPDATE`'s first relation intention is its
    // tuple borrow at the qualifying row - after its resolution and after
    // its walk reached the row. It parks there on the build's `X`, re-runs
    // after the commit, and the value it moves is in the new index.
    //
    // **Mutation**, measured: `BorrowChain` registering no wake on a refused
    // relation `IX` - killed 1 in 1.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    const catalog::Oid oid = PeerRelation(*rig, "updated_ix");
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    ASSERT_EQ(rig->core(1).dispatcher().Dispatch("INSERT INTO updated_ix VALUES (10)")
                  .response.rfind("INSERTED", 0),
              0u);

    Session ddl;
    ASSERT_EQ(d0.Dispatch("BEGIN", &ddl).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(d0.Dispatch("CREATE INDEX uix ON updated_ix (v)", &ddl)
                  .response.rfind("CREATED INDEX", 0),
              0u);

    Statement update;
    update.sql = "UPDATE updated_ix SET v = 77 WHERE id = 1";
    Statement commit;
    commit.sql = "COMMIT";
    commit.session = &ddl;
    Submit(*rig, 1, update);
    rig->Start();
    update.go.store(true, std::memory_order_release);
    const txn::LockKey rel = txn::LockKey::Relation(oid);
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return rig->locks().WaiterCount(rel) == 1; }))
        << "the update never parked on the relation the build holds: " << update.out.response;

    Submit(*rig, 0, commit);
    commit.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return commit.done.load(std::memory_order_acquire); }));
    ASSERT_TRUE(
        KickUntil(*rig, 1, [&] { return update.done.load(std::memory_order_acquire); }, 5000ms))
        << "the update never resumed after the build committed";
    EXPECT_EQ(update.out.response.rfind("UPDATED", 0), 0u) << update.out.response;
    rig->Stop();

    const std::string plan =
        rig->core(1).dispatcher().Dispatch("ANALYZE SELECT * FROM updated_ix WHERE v = 77").response;
    EXPECT_NE(plan.find("IndexProbe"), std::string::npos) << plan;
    const std::string found =
        rig->core(1).dispatcher().Dispatch("SELECT * FROM updated_ix WHERE v = 77").response;
    EXPECT_NE(found.find(",77"), std::string::npos) << "the index missed the moved value: " << found;
}

}  // namespace
}  // namespace kds::server
