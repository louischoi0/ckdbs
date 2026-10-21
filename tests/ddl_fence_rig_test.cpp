// AT-S5e and AT-0 item 13: a DDL that reads a relation's rows to build a
// secondary structure fences off its writers **on every core**, on the
// two-core rig (`instructions/v3.0.0/workorder-at-m3-uniformity.md`,
// AT-S5e; `workorder-av-two-core-rig.md`).
//
// **Why these cells have to be on the rig.** A writer that meets another
// transaction's relation `X` would, by the row-level wait, have polled
// `TransactionManager::IsInFlight` - one core's live set until AX-S1 - and
// read a DDL running on the other core as "not in flight": the write would
// be refused rather than held, and before AT-S5e it was worse, admitted into
// an index being built (D6) or past a cabin being built (the unfenced-build
// bug). What a writer waits on is the table's own slot, flipped by the
// release from whichever core releases and kicked across, and only two
// reactors on two threads exercise that.
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
//
// And `alter.md` AL4a's: a rename takes the relation `X` too, so it waits
// for an open writer and for a declaration build on another core.

#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/exec/assertion_catalog.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/stats/lock_wait_log.hpp"
#include "kds/txn/lock_table.hpp"
#include "view_text.hpp"

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
    rig.Post(core, sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                                       RunWhenGo(rig.core(core).dispatcher(), x)));
}

// A relation the peer writes. No core owns it since AT-S9; "a peer" in the
// cells below is the writing session's core.
catalog::Oid PeerRelation(TwoCoreRig& rig, const std::string& name) {
    const std::string made = rig.core(0)
                                 .dispatcher()
                                 .Dispatch("CREATE TABLE " + name + " (id int64, v int64) BTREE")
                                 .response;
    EXPECT_EQ(made.rfind("CREATED", 0), 0u) << made;
    auto oid = rig.core(0).catalog().FindTableOidByName(::kds::catalog::kNamespacePublic, name);
    EXPECT_TRUE(oid.ok()) << oid.status().message();
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
    const std::string shown =
        rig->core(1).dispatcher().Dispatch("SELECT name, entries FROM sys.indexes").response;
    EXPECT_EQ(testing_view::ReadViewText(shown).Find("name", "fix", "entries"), "4") << shown;
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
    // relation while the drop is open and parks on the relation `X`.
    // `ROLLBACK` restores the index, and every round's row must be in it.
    //
    // **Written for a core-local DT9, and weaker since AX-S2.** Core 1
    // could not see core 0's deleter in flight, so its memo left the index
    // out and only the rollback moving the schema word **before** it
    // released the borrows (`Transaction::NoteWroteCatalog`) made the woken
    // insert re-resolve; the rounds were for the race with the DDL path's
    // own bump after the release. That mutation - the pre-release move
    // removed from `TransactionManager::Abort` - was killed 5 in 5. Since
    // AX-S2 core 1 resolves the index while the drop is open, and the same
    // mutation survives 5 in 5 (AX-S3; `known-gaps.md`, Testing): the cell
    // now pins the outcome, not the move.
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

    const std::string shown =
        rig->core(1).dispatcher().Dispatch("SELECT name, entries FROM sys.indexes").response;
    const auto entries = testing_view::ReadViewText(shown).Find("name", "dix", "entries");
    ASSERT_TRUE(entries.has_value()) << "the rollbacks lost the index: " << shown;
    EXPECT_EQ(*entries, std::to_string(3 + kRounds))
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

TEST(DdlFenceRigTest, AReindexOnCoreZeroWaitsForAWriterOnAPeerAndTheNewTreeHoldsItsRow) {
    // **BY-R6's hold.** A `REINDEX` runs under the relation `X`, as `CREATE
    // INDEX` does: a writer on core 1 holding the relation's `IX` in an open
    // transaction parks the rebuild on core 0 until it commits, and the new
    // tree's backfill then reads the writer's row. Core 1 next resolves the
    // new index through the schema word and probes and maintains it.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    const catalog::Oid oid = PeerRelation(*rig, "fenced_rx");
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    ASSERT_EQ(rig->core(0).dispatcher().Dispatch("CREATE INDEX rx ON fenced_rx (v)").response.rfind(
                  "CREATED INDEX", 0),
              0u);
    auto old_ix = rig->core(0).catalog().FindIndexByName("rx");
    ASSERT_TRUE(old_ix.ok());

    Session writer;
    ASSERT_EQ(d1.Dispatch("BEGIN", &writer).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(d1.Dispatch("INSERT INTO fenced_rx VALUES (7)", &writer).response.rfind("INSERTED", 0),
              0u);

    Statement reindex;
    reindex.sql = "REINDEX INDEX rx";
    Statement commit;
    commit.sql = "COMMIT";
    commit.session = &writer;
    Submit(*rig, 0, reindex);
    rig->Start();
    reindex.go.store(true, std::memory_order_release);

    const txn::LockKey rel = txn::LockKey::Relation(oid);
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return rig->locks().WaiterCount(rel) == 1; }))
        << "the rebuild never parked on the writer's relation: " << reindex.out.response;
    EXPECT_FALSE(reindex.done.load(std::memory_order_acquire)) << reindex.out.response;

    Submit(*rig, 1, commit);
    commit.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return commit.done.load(std::memory_order_acquire); }));
    ASSERT_EQ(commit.out.response.rfind("COMMIT", 0), 0u) << commit.out.response;
    ASSERT_TRUE(
        KickUntil(*rig, 0, [&] { return reindex.done.load(std::memory_order_acquire); }, 5000ms))
        << "the rebuild never resumed after the writer committed";
    EXPECT_EQ(reindex.out.response.rfind("REINDEXED INDEX", 0), 0u) << reindex.out.response;
    rig->Stop();

    auto new_ix = rig->core(1).catalog().FindIndexByName("rx");
    ASSERT_TRUE(new_ix.ok());
    EXPECT_NE(new_ix.value().index_oid, old_ix.value().index_oid);
    ASSERT_EQ(d1.Dispatch("INSERT INTO fenced_rx VALUES (8)").response.rfind("INSERTED", 0), 0u);
    const std::string plan = d1.Dispatch("ANALYZE SELECT * FROM fenced_rx WHERE v = 7").response;
    EXPECT_NE(plan.find("IndexProbe"), std::string::npos) << plan;
    EXPECT_NE(d1.Dispatch("SELECT * FROM fenced_rx WHERE v = 7").response.find(",7"),
              std::string::npos)
        << "the new tree missed the writer's row";
    const std::string shown = d1.Dispatch("SELECT name, entries FROM sys.indexes").response;
    EXPECT_EQ(testing_view::ReadViewText(shown).Find("name", "rx", "entries"), "2") << shown;
}

TEST(DdlFenceRigTest, AWriterOnAPeerOutlivesTheFaultNetBehindAnAutocommitBuildOnCoreZero) {
    // **The net measures a build's silence, not its length**
    // (`holder_progress.hpp`). An autocommit `CREATE INDEX` holds the
    // relation `X` for its whole backfill, synchronously on core 0; a writer
    // on core 1 arriving during it waits for the decide (`assertion.md`
    // §8.1a, `index.md`). Before the build reported progress, a backfill
    // longer than the net refused that writer as though the build were
    // stuck. The net is cut to 50 ms and core 1 re-polls every 1 ms, so the
    // writer meets its net several times during the build (about 200 ms
    // over 10,000 rows in Debug). The net must still exceed the build's tail
    // - the catalog write and the commit after its last row, which report
    // nothing - and any stretch the host deschedules core 0 under a
    // parallel suite.
    //
    // **Mutation**, measured: the waiter refusing at the net whatever the
    // holder's progress - killed 1 in 1.
    TwoCoreRig::Options options;
    options.max_idle_block_ms = 1;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    PeerRelation(*rig, "built_ix");
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    constexpr sched::MonoTimeNs kNetNs = 50'000'000;
    d1.set_lock_wait_fault_net_ns(kNetNs);
    for (int batch = 0; batch < 20; ++batch) {
        std::string values;
        for (int i = 0; i < 500; ++i) {
            values += (i == 0 ? "(" : ", (") + std::to_string(batch * 500 + i) + ")";
        }
        ASSERT_EQ(d1.Dispatch("INSERT INTO built_ix VALUES " + values).response.rfind("INSERTED", 0),
                  0u);
    }

    // The log is the process's: a single-process run of the binary carries
    // earlier cells' rows, `fault_net` ones included (lock_family_test's).
    stats::LockWaitLog::ResetForTest();

    Statement build;
    build.sql = "CREATE INDEX bix ON built_ix (v)";
    Statement insert;
    insert.sql = "INSERT INTO built_ix VALUES (-1)";
    Submit(*rig, 0, build);
    Submit(*rig, 1, insert);
    rig->Start();
    build.go.store(true, std::memory_order_release);
    // The writer starts once the build has its `X` and reports progress.
    ASSERT_TRUE(KickUntil(*rig, 0, [&] {
        return rig->locks().progress().Registered() > 0 ||
               build.done.load(std::memory_order_acquire);
    }));
    insert.go.store(true, std::memory_order_release);

    ASSERT_TRUE(
        KickUntil(*rig, 0, [&] { return build.done.load(std::memory_order_acquire); }, 20000ms))
        << "the build never finished";
    ASSERT_EQ(build.out.response.rfind("CREATED INDEX", 0), 0u) << build.out.response;
    ASSERT_TRUE(
        KickUntil(*rig, 1, [&] { return insert.done.load(std::memory_order_acquire); }, 5000ms))
        << "the writer never resumed after the build committed";
    EXPECT_EQ(insert.out.response.rfind("INSERTED", 0), 0u)
        << "the writer was refused behind a build that was advancing: " << insert.out.response;
    rig->Stop();

    // **Not vacuous**: the writer waited past its net and was granted.
    const testing_view::ViewText waits = testing_view::ReadViewText(
        d1.Dispatch("SELECT outcome, waited_us FROM sys.lock_waits").response);
    const std::vector<std::string> outcomes = waits.Column("outcome");
    const std::vector<std::string> waited = waits.Column("waited_us");
    bool past_the_net = false;
    for (std::size_t i = 0; i < outcomes.size(); ++i) {
        EXPECT_NE(outcomes[i], "fault_net") << "a wait behind the build ended at the net";
        past_the_net |= outcomes[i] == "granted" && std::stoull(waited[i]) > kNetNs / 1000;
    }
    EXPECT_TRUE(past_the_net) << "the build ended inside one net, so the cell proved nothing";
    EXPECT_NE(d1.Dispatch("SELECT * FROM built_ix WHERE v = -1").response.find(",-1"),
              std::string::npos);
}

// **AL4a.** A rename takes the relation `X`, so a writer's `IX` held in an
// open transaction on core 1 holds it back on core 0 until the writer
// commits; then it lands. Both forms.
//
// **Mutation**, measured: `HandleAlter` taking no relation `X` - killed 1 in
// 1, each form.
void ExpectARenameOnCoreZeroWaitsForAPeersOpenWriter(const std::string& rename_sql,
                                                    const std::string& reply,
                                                    const std::string& read_after) {
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    const catalog::Oid oid = PeerRelation(*rig, "renamed_held");
    CommandDispatcher& d1 = rig->core(1).dispatcher();

    Session writer;
    ASSERT_EQ(d1.Dispatch("BEGIN", &writer).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(
        d1.Dispatch("INSERT INTO renamed_held VALUES (7)", &writer).response.rfind("INSERTED", 0),
        0u);

    Statement rename;
    rename.sql = rename_sql;
    Statement commit;
    commit.sql = "COMMIT";
    commit.session = &writer;
    Submit(*rig, 0, rename);
    rig->Start();
    rename.go.store(true, std::memory_order_release);

    const txn::LockKey rel = txn::LockKey::Relation(oid);
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return rig->locks().WaiterCount(rel) == 1; }))
        << "the rename never parked on the writer's relation: " << rename.out.response;
    EXPECT_FALSE(rename.done.load(std::memory_order_acquire)) << rename.out.response;

    Submit(*rig, 1, commit);
    commit.go.store(true, std::memory_order_release);
    ASSERT_TRUE(KickUntil(*rig, 1, [&] { return commit.done.load(std::memory_order_acquire); }));
    ASSERT_TRUE(
        KickUntil(*rig, 0, [&] { return rename.done.load(std::memory_order_acquire); }, 5000ms))
        << "the rename never resumed after the writer committed";
    EXPECT_EQ(rename.out.response.rfind(reply, 0), 0u) << rename.out.response;
    rig->Stop();
    EXPECT_EQ(rig->locks().WaiterCount(rel), 0u);
    const std::string read = d1.Dispatch(read_after).response;
    EXPECT_NE(read.find("7"), std::string::npos) << "the writer's row, under the new name: " << read;
}

TEST(DdlFenceRigTest, ARenameColumnOnCoreZeroWaitsForAPeersOpenWriterAndThenLands) {
    ExpectARenameOnCoreZeroWaitsForAPeersOpenWriter(
        "ALTER TABLE renamed_held RENAME COLUMN v TO w", "RENAMED COLUMN",
        "SELECT w FROM renamed_held");
}

TEST(DdlFenceRigTest, ARenameToOnCoreZeroWaitsForAPeersOpenWriterAndThenLands) {
    ExpectARenameOnCoreZeroWaitsForAPeersOpenWriter("ALTER TABLE renamed_held RENAME TO moved",
                                                    "RENAMED TABLE", "SELECT v FROM moved");
}

TEST(DdlFenceRigTest, ARenameOnAPeerWaitsForACreateAssertionOnCoreZeroAndIsRestrictedByIt) {
    // **AL4a's reason.** A `CREATE ASSERTION` building on core 0 has no
    // `sys.assertions` row for AL4 to read until it publishes. A rename of
    // its relation on core 1 used to land inside the build, and the
    // assertion then stored a canon naming a column that no longer existed.
    // It waits for the build's `X` now, and once the assertion is published
    // AL4 restricts it.
    //
    // **Mutation**, measured: `HandleAlter` taking no relation `X` - killed
    // 1 in 1.
    TwoCoreRig::Options options;
    options.max_idle_block_ms = 1;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    const catalog::Oid oid = PeerRelation(*rig, "renamed_cap");
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    for (int batch = 0; batch < 20; ++batch) {
        std::string values;
        for (int i = 0; i < 500; ++i) {
            values += (i == 0 ? "(" : ", (") + std::to_string(batch * 500 + i) + ")";
        }
        ASSERT_EQ(
            d1.Dispatch("INSERT INTO renamed_cap VALUES " + values).response.rfind("INSERTED", 0),
            0u);
    }

    Statement build;
    build.sql = "CREATE ASSERTION one ON renamed_cap GROUP BY (v) CHECK COUNT(*) <= 1";
    Statement rename;
    rename.sql = "ALTER TABLE renamed_cap RENAME COLUMN v TO w";
    Submit(*rig, 0, build);
    Submit(*rig, 1, rename);
    rig->Start();
    build.go.store(true, std::memory_order_release);
    // The rename starts once the build holds its `X`: the inserts committed,
    // so the only entry the table has is the build's.
    ASSERT_TRUE(KickUntil(*rig, 0, [&] {
        return rig->locks().EntryCount() > 0 || build.done.load(std::memory_order_acquire);
    }));
    const bool built_before_rename = build.done.load(std::memory_order_acquire);
    rename.go.store(true, std::memory_order_release);
    // **Not vacuous**: the rename met the build's `X` and parked on it.
    const txn::LockKey rel = txn::LockKey::Relation(oid);
    const bool parked = KickUntil(*rig, 1, [&] {
        return rig->locks().WaiterCount(rel) == 1 ||
               rename.done.load(std::memory_order_acquire);
    }) && !rename.done.load(std::memory_order_acquire);

    ASSERT_TRUE(
        KickUntil(*rig, 0, [&] { return build.done.load(std::memory_order_acquire); }, 20000ms))
        << "the build never finished";
    ASSERT_TRUE(
        KickUntil(*rig, 1, [&] { return rename.done.load(std::memory_order_acquire); }, 5000ms))
        << "the rename never ran";
    rig->Stop();
    ASSERT_FALSE(built_before_rename) << "the build ended before the rename began: vacuous";
    EXPECT_TRUE(parked) << "the rename never parked on the build's X: " << rename.out.response;
    ASSERT_EQ(build.out.response.rfind("CREATED ASSERTION", 0), 0u) << build.out.response;
    EXPECT_NE(rename.out.response.find("assertion 'one'"), std::string::npos)
        << "the rename landed inside the build: " << rename.out.response;
    EXPECT_EQ(d1.Dispatch("SELECT v FROM renamed_cap WHERE v = 7").response.rfind("ERR", 0),
              std::string::npos)
        << "the column the assertion's canon names is gone";
}

TEST(DdlFenceRigTest, ARenameInsideATransactionHoldingTheRelationIsRefusedAndPoisonsNothing) {
    // A rename cannot wait for its own session's transaction: one that wrote
    // the relation holds its `IX` until it decides. The refusal is
    // retryable and leaves the transaction as it was.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    PeerRelation(*rig, "renamed_own");
    CommandDispatcher& d0 = rig->core(0).dispatcher();

    Session s;
    ASSERT_EQ(d0.Dispatch("BEGIN", &s).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(d0.Dispatch("INSERT INTO renamed_own VALUES (7)", &s).response.rfind("INSERTED", 0),
              0u);
    const DispatchOutcome refused =
        d0.Dispatch("ALTER TABLE renamed_own RENAME COLUMN v TO w", &s);
    EXPECT_EQ(refused.status.code(), StatusCode::kTxnConflict) << refused.response;
    EXPECT_NE(refused.response.find("own transaction"), std::string::npos) << refused.response;
    EXPECT_EQ(d0.Dispatch("ALTER TABLE renamed_own RENAME TO moved", &s).status.code(),
              StatusCode::kTxnConflict);
    EXPECT_FALSE(s.failed()) << "the refusal poisoned the transaction";
    ASSERT_EQ(d0.Dispatch("COMMIT", &s).response.rfind("COMMIT", 0), 0u);
    // Once the transaction has ended, the same rename lands.
    EXPECT_EQ(d0.Dispatch("ALTER TABLE renamed_own RENAME COLUMN v TO w", &s).response.rfind(
                  "RENAMED COLUMN", 0),
              0u);
}

TEST(DdlFenceRigTest, ACreateAssertionResolvesItsColumnsUnderItsXNotUnderAStaleMemo) {
    // `alter.md` AL4a from the assertion's side. Core 0's memo holds the
    // relation's `TableAccess`; after its statement boundary settled it, a
    // `RENAME COLUMN` on core 1 takes the `X`, renames and releases, and
    // core 0's `X` is then granted with no wait - no re-run. The build must
    // resolve `v` against the schema the rename left, and refuse; under the
    // stale memo it published a canon naming a column that no longer exists.
    //
    // **Mutation**, measured: `HandleAssertion` not revalidating after the
    // grant - killed 1 in 1.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    PeerRelation(*rig, "stale_cap");
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    ASSERT_EQ(d1.Dispatch("INSERT INTO stale_cap VALUES (7)").response.rfind("INSERTED", 0), 0u);
    // Core 0's memo binds the relation before the rename.
    ASSERT_EQ(d0.Dispatch("SELECT v FROM stale_cap").response.rfind("ERR", 0), std::string::npos);

    bool ran = false;
    std::string renamed;
    d0.SetEpochSeamForTest(CommandDispatcher::EpochSeam::kAfterPublish, [&] {
        if (ran) return;
        ran = true;
        renamed = d1.Dispatch("ALTER TABLE stale_cap RENAME COLUMN v TO w").response;
    });
    const DispatchOutcome created =
        d0.Dispatch("CREATE ASSERTION one ON stale_cap GROUP BY (v) CHECK COUNT(*) <= 1");
    d0.SetEpochSeamForTest(CommandDispatcher::EpochSeam::kAfterPublish, nullptr);
    ASSERT_TRUE(ran);
    ASSERT_EQ(renamed.rfind("RENAMED COLUMN", 0), 0u) << renamed;
    EXPECT_EQ(created.response.rfind("ERR", 0), 0u)
        << "the assertion was built over a column renamed under it: " << created.response;
    EXPECT_EQ(d0.Dispatch("SELECT name FROM sys.assertions").response.find("one"),
              std::string::npos);
}

TEST(DdlFenceRigTest, ARenameToWhoseNameMovedBeforeItsGrantIsRefused) {
    // A `RENAME TO` on core 1 moves the relation off the name core 0's rename
    // resolved; core 0's grant is uncontended, so nothing re-runs. The name
    // is asked again under the `X`, and core 0 refuses rather than renaming
    // the relation core 1 already renamed.
    //
    // **Mutation**, measured: `HandleAlter` not re-resolving after the grant
    // - killed 1 in 1.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    PeerRelation(*rig, "moving");
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    ASSERT_EQ(d0.Dispatch("SELECT v FROM moving").response.rfind("ERR", 0), std::string::npos);

    bool ran = false;
    std::string first;
    d0.SetEpochSeamForTest(CommandDispatcher::EpochSeam::kAfterPublish, [&] {
        if (ran) return;
        ran = true;
        first = d1.Dispatch("ALTER TABLE moving RENAME TO moved_b").response;
    });
    const DispatchOutcome second = d0.Dispatch("ALTER TABLE moving RENAME TO moved_a");
    d0.SetEpochSeamForTest(CommandDispatcher::EpochSeam::kAfterPublish, nullptr);
    ASSERT_TRUE(ran);
    ASSERT_EQ(first.rfind("RENAMED TABLE", 0), 0u) << first;
    EXPECT_EQ(second.response.rfind("ERR", 0), 0u)
        << "renamed a relation no longer named 'moving': " << second.response;
    EXPECT_EQ(d0.Dispatch("SELECT v FROM moved_b").response.rfind("ERR", 0), std::string::npos);
    EXPECT_EQ(d0.Dispatch("SELECT v FROM moved_a").response.rfind("ERR", 0), 0u);
}

}  // namespace
}  // namespace kds::server
