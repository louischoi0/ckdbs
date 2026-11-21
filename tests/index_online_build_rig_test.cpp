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

#include "kds/exec/index_maintain.hpp"
#include "kds/sched/coro.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/heap/rowid.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/index/index_tree.hpp"
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
    {
        // A test driver standing in for a client: the reactor runs strict (CJ WE04, I2).
        sched::WaitScope rig_idle(stats::WaitEvent::kIdleClient);
        co_await sched::WaitUntil{&x.start_pred};
    }
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

// DN: an online build over a heap. The high water was a descent by key,
// which a heap's data page refuses; and the A2 bound ended the walk at the
// first page past the high water, which on a heap - whose growth links a
// page after the page that filled, anywhere in the chain - is no bound on
// the pages after it, so every old row behind a page a writer grew during
// the backfill went unindexed. Core 1's growth is pinned mid-chain to put
// such a page ahead of the cursor; every row written before the build must
// still be found through the index.
TEST(IndexOnlineBuildRigTest, AnOnlineBuildOverAHeapIndexesEveryOlderRow) {
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    ASSERT_EQ(d0.Dispatch("CREATE TABLE h (v int64, pad varchar(2000)) HEAP")
                  .response.rfind("CREATED", 0),
              0u);
    constexpr int kRows = 1200;
    for (int i = 0; i < kRows; ++i) {
        const std::string sql = "INSERT INTO h VALUES (" + std::to_string(i) + ", '" +
                                std::string(1500, 'p') + "')";
        ASSERT_EQ(d0.Dispatch(sql).response.rfind("INSERTED", 0), 0u) << sql;
    }
    auto oid = rig->core(0).catalog().FindTableOidByName(catalog::kNamespacePublic, "h");
    ASSERT_TRUE(oid.ok());
    auto access = rig->core(0).catalog().InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    std::vector<PageId> chain;
    for (PageId at = access.value()->desc_page_id; at != kInvalidPageId;) {
        chain.push_back(at);
        auto page = rig->store().GetForRead(at);
        ASSERT_TRUE(page.ok());
        at = heap::PageView(page.value().bytes()).next_page_id();
    }
    ASSERT_GT(chain.size(), 100u);
    // Core 1 fills a full page near the chain's end, so its growth links
    // there, behind the backfill's cursor for most of the build.
    d1.heap_space().SetCurrentPage(oid.value(), 1, chain[chain.size() - 5]);

    Statement build;
    build.sql = "CREATE INDEX hv ON h (v)";
    std::vector<std::unique_ptr<Statement>> writes;
    for (int i = 0; i < 40; ++i) {
        auto x = std::make_unique<Statement>();
        x->sql = "INSERT INTO h VALUES (" + std::to_string(100000 + i) + ", '" +
                 std::string(1500, 'n') + "')";
        writes.push_back(std::move(x));
    }
    Submit(*rig, 0, build);
    rig->Start();
    build.go.store(true, std::memory_order_release);
    for (auto& x : writes) {
        ASSERT_FALSE(RunOn(*rig, 1, *x).rfind("ERR", 0) == 0) << x->sql << " -> " << x->out.response;
    }
    ASSERT_TRUE(
        KickUntil(*rig, 0, [&] { return build.done.load(std::memory_order_acquire); }, 20000ms))
        << "the build never finished";
    ASSERT_EQ(build.out.response.rfind("CREATED INDEX", 0), 0u) << build.out.response;
    rig->Stop();

    ASSERT_NE(d0.Dispatch("ANALYZE SELECT v FROM h WHERE v = 10").response.find("IndexProbe"),
              std::string::npos);
    int missing = 0;
    for (int i = 0; i < kRows; i += 7) {
        const std::string got =
            d0.Dispatch("SELECT v FROM h WHERE v = " + std::to_string(i)).response;
        missing += testing_view::ReadViewText(got).Column("v") != Ids{std::to_string(i)};
    }
    EXPECT_EQ(missing, 0) << "rows written before the build are missing from the index";
    for (int i = 0; i < 40; ++i) {
        EXPECT_EQ(testing_view::ReadViewText(
                      d0.Dispatch("SELECT v FROM h WHERE v = " + std::to_string(100000 + i))
                          .response)
                      .Column("v"),
                  Ids{std::to_string(100000 + i)});
    }
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

// ---- The heap slot release across cores (DN-R2) ----------------------------

// The entries of every index of `oid` - as `catalog` names them now - that
// name a slot flagged free: DN-R2 says there are none.
int EntriesNamingAFreeSlot(storage::PageStore& store, catalog::Catalog& catalog,
                           catalog::Oid oid) {
    auto access = catalog.InitTableAccess(oid);
    EXPECT_TRUE(access.ok());
    if (!access.ok()) return -1;
    int named = 0;
    for (const catalog::TableAccess::IndexRef& ix : access.value()->indexes) {
        const index::IndexLayout layout = exec::IndexLayoutOf(ix);
        std::vector<std::byte> low(layout.sort_key_width(), std::byte{0});
        auto leaf = index::IndexSeekLeaf(store, ix.root_page_id, layout, low);
        EXPECT_TRUE(leaf.ok());
        if (!leaf.ok()) return -1;
        for (PageId at = leaf.value(); at != kInvalidPageId;) {
            std::vector<std::uint64_t> rowids;
            {
                auto page = index::IndexFetchLeaf(store, at, layout, storage::PageAccess::kRead);
                EXPECT_TRUE(page.ok());
                if (!page.ok()) return -1;
                const index::IndexLeafView view(page.value().bytes());
                for (std::uint16_t i = 0; i < view.entry_count(); ++i) {
                    rowids.push_back(index::GetIndexRowid(layout, view.Entry(i).value()));
                }
                at = view.right_sibling();
            }
            for (std::uint64_t rowid : rowids) {
                const heap::Rowid r = heap::DecodeRowid(rowid);
                auto data = store.GetForRead(r.page_id);
                EXPECT_TRUE(data.ok());
                if (!data.ok()) return -1;
                auto info = heap::PageView(data.value().bytes()).DebugSlotInfo(r.slot);
                if (info.ok() && (info.value().flags & heap::kSlotFlagFree) != 0) ++named;
            }
        }
    }
    return named;
}

// **Core 0's release reads the relation's indexes as core 0's memo last saw
// them.** The release is stepped from core 0's `system` tick, which is no
// statement boundary, so nothing revalidated its catalog (catalog.md CT2,
// CT3): an index another core built since - here an `X` build on core 1,
// committed and released before the release's first step - was neither
// purged nor verified, and the slot was freed with that index's entry still
// naming it (DN-R2). The memo is made current under the step's `IX`.
TEST(HeapRowidReleaseRigTest, AnIndexAnotherCoreBuiltIsVerifiedBeforeASlotIsFreed) {
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    ASSERT_EQ(d0.Dispatch("CREATE TABLE h (a int64, b int64) HEAP").response.rfind("CREATED", 0),
              0u);
    for (int i = 0; i < 40; ++i) {
        const std::string sql =
            "INSERT INTO h VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")";
        ASSERT_EQ(d0.Dispatch(sql).response.rfind("INSERTED", 0), 0u) << sql;
    }
    auto oid = rig->core(0).catalog().FindTableOidByName(catalog::kNamespacePublic, "h");
    ASSERT_TRUE(oid.ok());
    // Core 0's memo now holds h with its Keystone index alone.
    ASSERT_TRUE(rig->core(0).catalog().InitTableAccess(oid.value()).ok());

    // Core 1: an index, then a delete whose rows' entries are no leaf's first.
    ASSERT_EQ(d1.Dispatch("CREATE INDEX hb ON h (b)").response.rfind("CREATED", 0), 0u);
    ASSERT_EQ(d1.Dispatch("DELETE FROM h WHERE a >= 10 AND a < 20").response.rfind("DELETED", 0),
              0u);
    // The instance's one queue in production; the rig's cores keep their own.
    d0.heap_space().NoteDead(oid.value());
    bool owed = true;
    for (int i = 0; i < 10000 && owed; ++i) {
        auto stepped = d0.StepHeapRelease(16);
        ASSERT_TRUE(stepped.ok()) << stepped.status().message();
        owed = stepped.value();
    }
    ASSERT_FALSE(owed);
    ASSERT_GT(d0.last_heap_release().freed, 0u);
    EXPECT_EQ(EntriesNamingAFreeSlot(rig->store(), rig->core(1).catalog(), oid.value()), 0)
        << "a slot was freed while an index core 0's memo did not hold named it";
}

}  // namespace
}  // namespace kds::server
