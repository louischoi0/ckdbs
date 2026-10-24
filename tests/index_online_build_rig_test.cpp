// **An online `CREATE INDEX`** (CI, `instructions/v3.2.0/workorder-ci-online-index-build.md`).
//
// An autocommit `CREATE INDEX` holds the relation `IS`, not `X`, across its
// backfill: it announces its row flagged building (H3), drains the writers
// that were granted the relation's `IX` before that announce (CI-Q3 (b)),
// backfills while writers keep writing (CI-R2), and clears the flag (R).
// The cells put a writer on core 1 against a build on core 0 and ask: the
// drain waits for a writer that began before the build and only for it; a
// writer that never decides refuses the build at the fault net and leaves
// no index behind; writers that run during the backfill neither wait nor
// lose a row, under a view taken before the build and one taken after it;
// and a writer whose transaction then asks the relation `X` closes a cycle
// through the drain and is refused as a deadlock instead of waiting out
// the net.

#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/txn/lock_table.hpp"
#include "view_text.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

struct Statement {
    Session own;
    Session* session = &own;
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

// Runs `x.sql` and waits for it on `core`'s reactor.
std::string RunOn(TwoCoreRig& rig, std::uint32_t core, Statement& x) {
    Submit(rig, core, x);
    x.go.store(true, std::memory_order_release);
    EXPECT_TRUE(KickUntil(rig, core, [&] { return x.done.load(std::memory_order_acquire); },
                          10000ms))
        << x.sql << " never finished";
    return x.out.response;
}

// `t (id, v, pad varchar(2000))`: a row is ~2 KB, so `rows` rows are about
// `rows / 4` leaves and a build of a few hundred rows takes several slices.
catalog::Oid Relation(TwoCoreRig& rig, int rows) {
    CommandDispatcher& d0 = rig.core(0).dispatcher();
    EXPECT_EQ(d0.Dispatch("CREATE TABLE t (id int64, v int64, pad varchar(2000)) BTREE")
                  .response.rfind("CREATED", 0),
              0u);
    for (int i = 0; i < rows; ++i) {
        const std::string sql = "INSERT INTO t VALUES (" + std::to_string(i + 1) + ", " +
                                std::to_string(i * 10) + ", '" + std::string(1500, 'p') + "')";
        EXPECT_EQ(d0.Dispatch(sql).response.rfind("INSERTED", 0), 0u) << sql;
    }
    auto oid = rig.core(0).catalog().FindTableOidByName(catalog::kNamespacePublic, "t");
    EXPECT_TRUE(oid.ok());
    return oid.value();
}

std::string Entries(CommandDispatcher& d, const std::string& name) {
    const std::string shown = d.Dispatch("SELECT name, entries FROM sys.indexes").response;
    return testing_view::ReadViewText(shown).Find("name", name, "entries").value_or("-");
}

// The `id`s `SELECT id FROM t WHERE v = <v>` answers, through `d` and
// `session`.
std::vector<std::string> IdsAt(CommandDispatcher& d, std::int64_t v, Session* session = nullptr) {
    const std::string sql = "SELECT id FROM t WHERE v = " + std::to_string(v);
    return testing_view::ReadViewText(d.Dispatch(sql, session).response).Column("id");
}

using Ids = std::vector<std::string>;

TEST(IndexOnlineBuildRigTest, TheDrainWaitsForAWriterThatBeganBeforeTheBuild) {
    // CI-R2: a writer granted the relation's `IX` before H3 may have
    // resolved the index list without the index, so the build waits for its
    // decide - and then indexes its row. **Mutation**, to measure: a drain
    // that waits for nobody.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    const catalog::Oid oid = Relation(*rig, 40);
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    Session writer;
    ASSERT_EQ(d1.Dispatch("BEGIN", &writer).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(d1.Dispatch("INSERT INTO t VALUES (500, 5000, 'w')", &writer)
                  .response.rfind("INSERTED", 0),
              0u);

    Statement build;
    build.sql = "CREATE INDEX tix ON t (v)";
    Submit(*rig, 0, build);
    rig->Start();
    build.go.store(true, std::memory_order_release);
    // The build is past its announce and parked in the drain on the writer.
    const txn::LockKey rel = txn::LockKey::Relation(oid);
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return rig->locks().WaiterCount(rel) == 1; }))
        << "the build never parked on the writer: " << build.out.response;
    EXPECT_FALSE(build.done.load(std::memory_order_acquire))
        << "the build ran past a writer that began before it: " << build.out.response;

    Statement commit;
    commit.sql = "COMMIT";
    commit.session = &writer;
    ASSERT_EQ(RunOn(*rig, 1, commit).rfind("COMMIT", 0), 0u) << commit.out.response;
    ASSERT_TRUE(
        KickUntil(*rig, 0, [&] { return build.done.load(std::memory_order_acquire); }, 10000ms))
        << "the build never resumed after the writer committed";
    ASSERT_EQ(build.out.response.rfind("CREATED INDEX", 0), 0u) << build.out.response;
    rig->Stop();

    EXPECT_EQ(Entries(d1, "tix"), "41");
    const std::string plan = d1.Dispatch("ANALYZE SELECT id FROM t WHERE v = 5000").response;
    EXPECT_NE(plan.find("IndexProbe"), std::string::npos) << plan;
    EXPECT_EQ(IdsAt(d1, 5000), Ids{"500"});
}

TEST(IndexOnlineBuildRigTest, AWriterThatNeverDecidesRefusesTheBuildAtTheNetAndLeavesNoIndex) {
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    (void)Relation(*rig, 8);
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    d0.set_lock_wait_fault_net_ns(50'000'000);
    Session writer;
    ASSERT_EQ(d1.Dispatch("BEGIN", &writer).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(d1.Dispatch("INSERT INTO t VALUES (500, 5000, 'w')", &writer)
                  .response.rfind("INSERTED", 0),
              0u);

    rig->Start();
    Statement build;
    build.sql = "CREATE INDEX tix ON t (v)";
    const std::string refused = RunOn(*rig, 0, build);
    EXPECT_EQ(build.out.status.code(), StatusCode::kTxnConflict) << refused;

    Statement commit;
    commit.sql = "COMMIT";
    commit.session = &writer;
    ASSERT_EQ(RunOn(*rig, 1, commit).rfind("COMMIT", 0), 0u) << commit.out.response;
    // No index survives the refusal, and its name is free again.
    Statement again;
    again.sql = "CREATE INDEX tix ON t (v)";
    ASSERT_EQ(RunOn(*rig, 0, again).rfind("CREATED INDEX", 0), 0u) << again.out.response;
    rig->Stop();
    EXPECT_EQ(Entries(d1, "tix"), "9");
}

TEST(IndexOnlineBuildRigTest, WritersDuringTheBackfillLoseNoRowUnderAnOldViewOrANewOne) {
    // CI-R1 and CI-R2: writers on core 1 insert, update the key of and
    // delete rows while the backfill runs on core 0, and neither waits. Then
    // every row is found through the index - by a reader whose snapshot was
    // taken before the build, which must still find each row at the key it
    // had then, and by one after.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    constexpr int kRows = 1200;
    (void)Relation(*rig, kRows);
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    Session old_reader;
    ASSERT_FALSE(d1.Dispatch("BEGIN ISOLATION LEVEL REPEATABLE READ", &old_reader)
                     .response.rfind("ERR", 0) == 0);
    ASSERT_NE(d1.Dispatch("SELECT id FROM t WHERE id = 1", &old_reader).response.find("1"),
              std::string::npos);

    // Written after the old view's snapshot and before the build: the page
    // holds each row's new version, and the one the old view reads is in
    // undo alone, which the backfill must walk (IX10a).
    for (int i = 0; i < 30; ++i) {
        ASSERT_EQ(d1.Dispatch("UPDATE t SET v = " + std::to_string(300000 + i) + " WHERE id = " +
                              std::to_string(3 + i * 37))
                      .response.rfind("UPDATED", 0),
                  0u);
        ASSERT_EQ(d1.Dispatch("DELETE FROM t WHERE id = " + std::to_string(4 + i * 37))
                      .response.rfind("DELETED", 0),
                  0u);
    }

    Statement build;
    build.sql = "CREATE INDEX tix ON t (v)";
    // The writers' script, each statement after the previous one, while the
    // build runs.
    std::vector<std::unique_ptr<Statement>> writes;
    for (int i = 0; i < 30; ++i) {
        auto add = [&](std::string sql) {
            auto x = std::make_unique<Statement>();
            x->sql = std::move(sql);
            writes.push_back(std::move(x));
        };
        add("INSERT INTO t VALUES (" + std::to_string(10000 + i) + ", " +
            std::to_string(100000 + i) + ", 'n')");
        add("UPDATE t SET v = " + std::to_string(200000 + i) + " WHERE id = " +
            std::to_string(1 + i * 37));
        add("DELETE FROM t WHERE id = " + std::to_string(2 + i * 37));
    }
    Submit(*rig, 0, build);
    rig->Start();
    build.go.store(true, std::memory_order_release);
    std::size_t ran_during_build = 0;
    for (auto& x : writes) {
        const bool during = !build.done.load(std::memory_order_acquire);
        ASSERT_FALSE(RunOn(*rig, 1, *x).rfind("ERR", 0) == 0) << x->sql << " -> " << x->out.response;
        if (during && !build.done.load(std::memory_order_acquire)) ++ran_during_build;
    }
    ASSERT_TRUE(
        KickUntil(*rig, 0, [&] { return build.done.load(std::memory_order_acquire); }, 20000ms))
        << "the build never finished";
    ASSERT_EQ(build.out.response.rfind("CREATED INDEX", 0), 0u) << build.out.response;
    EXPECT_GT(ran_during_build, 0u) << "no writer ran inside the build: the cell proves nothing";
    rig->Stop();

    CommandDispatcher& d0 = rig->core(0).dispatcher();
    ASSERT_NE(d0.Dispatch("ANALYZE SELECT id FROM t WHERE v = 10").response.find("IndexProbe"),
              std::string::npos);
    for (int i = 0; i < 30; ++i) {
        const std::string moved = std::to_string(1 + i * 37);
        const std::string gone = std::to_string(2 + i * 37);
        const std::int64_t moved_was = std::int64_t{i} * 37 * 10;
        const std::int64_t gone_was = (1 + std::int64_t{i} * 37) * 10;
        // The new view: the inserted row at its key, the updated row at its
        // new key and not its old one, the deleted row nowhere.
        EXPECT_EQ(IdsAt(d0, 100000 + i), Ids{std::to_string(10000 + i)});
        EXPECT_EQ(IdsAt(d0, 200000 + i), Ids{moved}) << "the updated row at its new key";
        EXPECT_EQ(IdsAt(d0, moved_was), Ids{}) << "the updated row at its old key";
        EXPECT_EQ(IdsAt(d0, gone_was), Ids{}) << "the deleted row";
        // The old view: the moved and the deleted rows at their keys of
        // then, through the index the build published after the view.
        EXPECT_EQ(IdsAt(d1, moved_was, &old_reader), Ids{moved}) << "an old view lost a row";
        EXPECT_EQ(IdsAt(d1, gone_was, &old_reader), Ids{gone})
            << "an old view lost a deleted row";
        EXPECT_EQ(IdsAt(d1, 100000 + i, &old_reader), Ids{}) << "an old view saw a new row";
        // The rows written before the build, whose old versions only undo
        // holds.
        const std::string pre_moved = std::to_string(3 + i * 37);
        const std::string pre_gone = std::to_string(4 + i * 37);
        EXPECT_EQ(IdsAt(d0, 300000 + i), Ids{pre_moved});
        EXPECT_EQ(IdsAt(d1, (2 + std::int64_t{i} * 37) * 10, &old_reader), Ids{pre_moved})
            << "an old view lost a row whose old version is in undo alone";
        EXPECT_EQ(IdsAt(d1, (3 + std::int64_t{i} * 37) * 10, &old_reader), Ids{pre_gone})
            << "an old view lost a deleted row whose old version is in undo alone";
        EXPECT_EQ(IdsAt(d0, (3 + std::int64_t{i} * 37) * 10), Ids{});
    }
    const std::string plan_old =
        d1.Dispatch("ANALYZE SELECT id FROM t WHERE v = 10", &old_reader).response;
    EXPECT_NE(plan_old.find("IndexProbe"), std::string::npos) << plan_old;
}

TEST(IndexOnlineBuildRigTest, AWriterAskingTheRelationXDuringTheDrainIsTheDeadlockVictim) {
    // §3.5 A9: the drain draws its edge to the writer it waits on, so that
    // writer's transaction asking the relation `X` the build's `IS` refuses
    // closes a cycle and is refused as one, instead of both waiting out the
    // net. The build then drains and finishes.
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    const catalog::Oid oid = Relation(*rig, 8);
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    Session writer;
    ASSERT_EQ(d1.Dispatch("BEGIN", &writer).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(d1.Dispatch("INSERT INTO t VALUES (500, 5000, 'w')", &writer)
                  .response.rfind("INSERTED", 0),
              0u);

    Statement build;
    build.sql = "CREATE INDEX tix ON t (v)";
    Submit(*rig, 0, build);
    rig->Start();
    build.go.store(true, std::memory_order_release);
    const txn::LockKey rel = txn::LockKey::Relation(oid);
    ASSERT_TRUE(KickUntil(*rig, 0, [&] { return rig->locks().WaiterCount(rel) == 1; }))
        << "the build never parked on the writer: " << build.out.response;

    // A create inside `BEGIN` keeps the `X` build (CI-Q1 (b)).
    Statement ddl;
    ddl.sql = "CREATE INDEX wix ON t (pad)";
    ddl.session = &writer;
    const std::string victim = RunOn(*rig, 1, ddl);
    EXPECT_NE(victim.find("deadlock"), std::string::npos) << victim;

    Statement rollback;
    rollback.sql = "ROLLBACK";
    rollback.session = &writer;
    (void)RunOn(*rig, 1, rollback);
    ASSERT_TRUE(
        KickUntil(*rig, 0, [&] { return build.done.load(std::memory_order_acquire); }, 10000ms))
        << "the build never resumed after the writer rolled back";
    EXPECT_EQ(build.out.response.rfind("CREATED INDEX", 0), 0u) << build.out.response;
    rig->Stop();
    EXPECT_EQ(Entries(d1, "tix"), "8");
}

}  // namespace
}  // namespace kds::server
