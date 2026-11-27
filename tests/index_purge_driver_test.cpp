#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/index_purge_driver.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/stats/relation_stats.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/lock_table.hpp"
#include "kds/txn/manager.hpp"
#include "sim/instance.hpp"
#include "two_core_rig.hpp"
#include "view_text.hpp"

// DW-S1 (`instructions/v3.2.0/workorder-dw-btree-purge-driver.md`): the
// unasked index purge. The driver chooses what `PURGE INDEX`'s pass runs on
// and when; **what a pass removes is the statement's own judge's to say**
// (DW-R1, `index_purge_test.cpp`), so these cells pin the choosing, the
// pacing (DW-R2), the switch (DW-R3) and the horizon (DW-R5), and that no
// result moves.

namespace kds::server {
namespace {

class IndexPurgeDriverTest : public ::testing::Test {
protected:
    void SetUp() override {
        stats::RelationStats::ResetForTest();
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        auto table = txn::LockTable::Create(/*core_count=*/1, /*max_locks_per_txn=*/4096);
        ASSERT_TRUE(table.ok()) << table.status().message();
        locks_ = std::move(table.value());
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr, /*visibility=*/nullptr, /*core=*/0,
                     locks_.get());
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, &*mgr_);
        dispatcher_->set_locks(locks_.get());
    }
    void TearDown() override { stats::RelationStats::ResetForTest(); }

    std::string Run(const std::string& sql, Session* s = nullptr) {
        return dispatcher_->Dispatch(sql, s).response;
    }
    void Ok(const std::string& sql, Session* s = nullptr) {
        const std::string out = Run(sql, s);
        ASSERT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    }
    std::uint64_t Entries(const std::string& index) {
        const std::string shown = Run("SELECT name, entries FROM sys.indexes");
        return std::stoull(
            testing_view::ReadViewText(shown).Find("name", index, "entries").value_or("0"));
    }
    catalog::Oid Oid(const std::string& table) {
        auto oid = boot_->catalog.ResolveRelation("", table, catalog::kNamespacePublic);
        EXPECT_TRUE(oid.ok());
        return oid.value();
    }
    const exec::IndexPurgeDriverCounts& Counts() {
        return dispatcher_->index_purge_driver()->counts();
    }

    // `t (id, k)` with `by_k`, `rows` rows of `k = id % 8`: 16-byte entries,
    // about 500 a leaf.
    void Table(int rows) {
        Ok("CREATE TABLE t (id int64, k int64) BTREE");
        Ok("CREATE INDEX by_k ON t (k)");
        for (int at = 1; at <= rows; at += 100) {
            std::string sql = "INSERT INTO t VALUES ";
            for (int i = at; i < at + 100 && i <= rows; ++i) {
                if (i != at) sql += ", ";
                sql += "(" + std::to_string(i) + ", " + std::to_string(i % 8) + ")";
            }
            Ok(sql);
        }
    }
    std::vector<std::string> Probes(Session* s = nullptr) {
        std::vector<std::string> out;
        for (int v = 0; v < 8; ++v) {
            out.push_back(Run("SELECT id FROM t WHERE k = " + std::to_string(v) + " ORDER BY id",
                              s));
        }
        return out;
    }
    // Offers, then steps until nothing is owed; the passes that ended.
    std::uint64_t Drive(std::uint32_t budget = 16) {
        const std::uint64_t had = Counts().passes;
        dispatcher_->QueueIndexPurges();
        for (int steps = 0; steps < 100000; ++steps) {
            auto owed = dispatcher_->StepIndexPurge(budget);
            EXPECT_TRUE(owed.ok()) << owed.status().message();
            if (!owed.ok() || !owed.value()) break;
        }
        return Counts().passes - had;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::unique_ptr<txn::LockTable> locks_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

TEST_F(IndexPurgeDriverTest, ADeletedRowsEntriesLeaveUnaskedAndNoResultMoves) {
    dispatcher_->EnableIndexPurgeDriver(/*min_deletes=*/100);
    Table(2000);
    Ok("DELETE FROM t WHERE id > 1000");
    const std::vector<std::string> before = Probes();
    ASSERT_EQ(Entries("by_k"), 2000u);
    Drive();
    // Every deleted row's entry but a leaf's first (IX2c).
    EXPECT_LT(Entries("by_k"), 1010u);
    EXPECT_GE(Entries("by_k"), 1000u);
    EXPECT_EQ(Probes(), before) << "a pass changes no result";
    EXPECT_EQ(Counts().passes, 1u);
    EXPECT_EQ(Counts().read, 2000u);
    EXPECT_EQ(Counts().removed, 2000u - Entries("by_k"));
    const std::string meta = Run("SHOW META");
    EXPECT_NE(meta.find("index_purge_entries_removed," + std::to_string(Counts().removed)),
              std::string::npos)
        << meta;
    // What the statement would take now is nothing: the pass was its pass.
    EXPECT_EQ(dispatcher_->Dispatch("PURGE INDEX by_k").rows_affected, 0u);
}

TEST_F(IndexPurgeDriverTest, SwitchedOffNothingIsOfferedSteppedOrShown) {
    // DW-R3: no driver is built, and the engine is the engine without one.
    Table(600);
    Ok("DELETE FROM t WHERE id > 100");
    dispatcher_->QueueIndexPurges();
    auto owed = dispatcher_->StepIndexPurge(16);
    ASSERT_TRUE(owed.ok());
    EXPECT_FALSE(owed.value());
    EXPECT_EQ(dispatcher_->index_purge_driver(), nullptr);
    EXPECT_EQ(Entries("by_k"), 600u);
    EXPECT_EQ(Run("SHOW META").find("index_purge"), std::string::npos);
}

TEST_F(IndexPurgeDriverTest, ARelationIsQueuedOnlyOnceItsDeletesReachTheThreshold) {
    dispatcher_->EnableIndexPurgeDriver(/*min_deletes=*/50);
    Table(600);
    Ok("DELETE FROM t WHERE id > 551");  // 49 rows
    EXPECT_EQ(Drive(), 0u);
    EXPECT_EQ(Entries("by_k"), 600u);
    // An UPDATE and an INSERT are no part of DW-S1's signal.
    Ok("UPDATE t SET k = k + 100 WHERE id <= 100");
    Ok("INSERT INTO t VALUES (9000, 1), (9001, 2)");
    EXPECT_EQ(Drive(), 0u);
    Ok("DELETE FROM t WHERE id = 551");  // the 50th
    EXPECT_EQ(Drive(), 1u);
    EXPECT_GT(Counts().removed, 49u);
}

// DW-R5: the driver reads the horizon as the statement does. A row a view
// still sees is owed its entries, so a pass under that view removes none of
// them, and the reader finds every row it saw. The deletes that bought the
// fruitless pass are spent: the next is bought at the same price, and with
// the reader gone it takes them all.
TEST_F(IndexPurgeDriverTest, AnOldViewKeepsItsRowsEntriesAndTheNextPassTakesThem) {
    dispatcher_->EnableIndexPurgeDriver(/*min_deletes=*/40);
    Table(200);
    Session reader;
    Ok("BEGIN ISOLATION LEVEL REPEATABLE READ", &reader);
    const std::vector<std::string> seen = Probes(&reader);
    Ok("DELETE FROM t WHERE id > 160");  // 40 rows the reader still sees
    EXPECT_EQ(Drive(), 1u);
    EXPECT_EQ(Counts().removed, 0u) << "every deleted row is still owed to the reader's view";
    EXPECT_EQ(Entries("by_k"), 200u);
    EXPECT_EQ(Probes(&reader), seen) << "the reader finds its rows through their entries";
    Ok("COMMIT", &reader);
    EXPECT_EQ(Drive(), 0u) << "the 40 deletes were spent on the fruitless pass";

    Ok("DELETE FROM t WHERE id > 120");  // 40 more buy the next
    EXPECT_EQ(Drive(), 1u);
    EXPECT_GE(Counts().removed, 78u) << "both deletes' entries, the reader gone";
}

// IX2b through the driver: a row an undecided writer holds is passed over,
// its entries kept and counted, and nothing waits.
TEST_F(IndexPurgeDriverTest, AnUndecidedDeletersRowsAreSkippedAndCounted) {
    dispatcher_->EnableIndexPurgeDriver(/*min_deletes=*/40);
    Table(200);
    Ok("DELETE FROM t WHERE id > 160");
    Session writer;
    Ok("BEGIN", &writer);
    Ok("DELETE FROM t WHERE id > 150 AND id <= 160", &writer);  // undecided
    EXPECT_EQ(Drive(), 1u);
    EXPECT_EQ(Counts().skipped, 10u);
    EXPECT_GE(Counts().removed, 39u);
    EXPECT_GE(Entries("by_k"), 160u) << "the undecided rows' entries stay";
    Ok("ROLLBACK", &writer);
    EXPECT_EQ(Run("SELECT id FROM t WHERE id > 150 ORDER BY id").find("ERR"), std::string::npos);
    EXPECT_EQ(Probes().size(), 8u);
    EXPECT_NE(Run("SELECT id FROM t WHERE k = 0 ORDER BY id").find("160"), std::string::npos)
        << "a rolled-back delete's row is found through its entry";
}

// DD-R8 in the pass both the driver and a heap's release run: an index still
// being built is passed over, since its backfill appends with no row unit.
TEST_F(IndexPurgeDriverTest, AnIndexStillBeingBuiltIsPassedOver) {
    Table(200);
    Ok("CREATE INDEX by_k2 ON t (k)");
    Ok("DELETE FROM t WHERE id > 100");
    auto access = boot_->catalog.InitTableAccess(Oid("t"));
    ASSERT_TRUE(access.ok());
    catalog::TableAccess building = *access.value();
    ASSERT_EQ(building.indexes.size(), 2u);
    building.indexes[0].building = true;
    constexpr std::uint64_t kHolder = 900002;
    txn::LockHoldings holdings;
    auto ix = locks_->TryAcquire(kHolder, txn::LockKey::Relation(building.oid),
                                 txn::LockMode::kIntentionExclusive, holdings);
    ASSERT_TRUE(ix.ok() && ix.value());
    exec::RelationIndexPurge pass;
    std::uint64_t read = 0;
    for (int i = 0; i < 100 && !pass.done(); ++i) {
        exec::IndexPurgeAttempt attempt;
        ASSERT_TRUE(pass.Step(store_, boot_->catalog, building, &*mgr_, /*wal=*/nullptr, *locks_,
                              kHolder, holdings, /*max_leaves=*/16, attempt)
                        .ok());
        read += attempt.read;
    }
    locks_->Release(kHolder, holdings);
    EXPECT_TRUE(pass.done());
    EXPECT_EQ(read, 200u) << "one index walked, the building one not";
    EXPECT_EQ(Entries("by_k"), 200u) << "the building index kept every entry";
    EXPECT_LT(Entries("by_k2"), 110u);
}

// DW-R2: a pass is slices, and between two of them the driver holds nothing -
// so a relation `X`, which a DDL on any core asks of the instance's one lock
// table, is granted there. While one is held the driver's slice is skipped,
// not waited for, and the pass goes on after it.
TEST_F(IndexPurgeDriverTest, NothingIsHeldBetweenTwoSlicesAndAHeldRelationSkipsTheSlice) {
    dispatcher_->EnableIndexPurgeDriver(/*min_deletes=*/100);
    Table(3000);
    Ok("DELETE FROM t WHERE id > 500");
    const std::vector<std::string> before = Probes();
    dispatcher_->QueueIndexPurges();
    constexpr std::uint64_t kDdl = 900001;
    const txn::LockKey relation = txn::LockKey::Relation(Oid("t"));
    int slices = 0;
    int skipped = 0;
    for (bool owed = true; owed && slices < 1000; ++slices) {
        auto stepped = dispatcher_->StepIndexPurge(/*budget=*/1);
        ASSERT_TRUE(stepped.ok()) << stepped.status().message();
        owed = stepped.value();
        // The gap: an `X` is free to take.
        txn::LockHoldings holdings;
        auto x = locks_->TryAcquire(kDdl, relation, txn::LockMode::kExclusive, holdings);
        ASSERT_TRUE(x.ok());
        ASSERT_TRUE(x.value()) << "the driver held the relation across slice " << slices;
        if (owed && slices == 2) {
            // Held across a tick: the slice is skipped and nothing is removed.
            const std::uint64_t removed = Counts().removed;
            auto held = dispatcher_->StepIndexPurge(1);
            ASSERT_TRUE(held.ok());
            EXPECT_TRUE(held.value()) << "the pass is still owed";
            EXPECT_EQ(Counts().removed, removed);
            ++skipped;
        }
        locks_->Release(kDdl, holdings);
    }
    EXPECT_GT(slices, 3) << "the index must span leaves for a gap to exist";
    EXPECT_EQ(skipped, 1);
    EXPECT_LT(Entries("by_k"), 510u);
    EXPECT_EQ(Probes(), before);
}

TEST_F(IndexPurgeDriverTest, AnIndexDroppedMidPassIsPassedOverAndOneCreatedMidPassIsWalked) {
    dispatcher_->EnableIndexPurgeDriver(/*min_deletes=*/100);
    Table(3000);
    Ok("CREATE INDEX by_k2 ON t (k)");
    Ok("DELETE FROM t WHERE id > 500");
    dispatcher_->QueueIndexPurges();
    auto first = dispatcher_->StepIndexPurge(1);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(first.value());
    Ok("DROP INDEX by_k");                // the index the pass is on
    Ok("CREATE INDEX by_k3 ON t (k)");    // a later oid: the pass reaches it
    for (int i = 0; i < 1000; ++i) {
        auto owed = dispatcher_->StepIndexPurge(1);
        ASSERT_TRUE(owed.ok()) << owed.status().message();
        if (!owed.value()) break;
    }
    EXPECT_EQ(Counts().passes, 1u);
    EXPECT_LT(Entries("by_k2"), 510u);
    // A build appends an entry for a delete-marked row too (DD-Q1 (b)), so
    // the new index had them, and the pass that reached it took them.
    EXPECT_LT(Entries("by_k3"), 510u);
    EXPECT_EQ(Run("SELECT id FROM t WHERE k = 3 ORDER BY id"),
              Run("SELECT id FROM t WHERE id <= 500 AND k = 3 ORDER BY id"));
}

TEST_F(IndexPurgeDriverTest, ADroppedRelationAndAHeapEndTheirPassesWithNothingWalked) {
    dispatcher_->EnableIndexPurgeDriver(/*min_deletes=*/10);
    Table(200);
    Ok("CREATE TABLE h (a int64, b int64) HEAP");
    Ok("CREATE INDEX hb ON h (b)");
    for (int i = 0; i < 40; ++i) {
        Ok("INSERT INTO h VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")");
    }
    Ok("DELETE FROM t WHERE id > 100");
    Ok("DELETE FROM h WHERE a >= 10");
    dispatcher_->QueueIndexPurges();
    Ok("DROP TABLE t");
    Drive();
    EXPECT_EQ(Counts().passes, 2u);
    EXPECT_EQ(Counts().read, 0u) << "a heap's indexes are its slot release's";
    // Neither is offered again by counts that no longer move.
    EXPECT_EQ(Drive(), 0u);
}

TEST(IndexPurgeDriverNoLocksTest, ADispatcherWithNoLockTableRunsNoPass) {
    // No fence to judge under (DD-R8): `PURGE INDEX` is refused there, and
    // the driver does nothing rather than judge unfenced.
    stats::RelationStats::ResetForTest();
    storage::InMemoryPageStore store{kFirstUserPageId};
    auto boot = bootstrap::BootstrapDatabase(store, 1000);
    ASSERT_TRUE(boot.ok());
    txn::TrxIdSequence ids(boot.value().superblock);
    txn::UndoLog undo(store, nullptr);
    txn::TransactionManager mgr(ids, undo, store, nullptr);
    CommandDispatcher d(boot.value().superblock, boot.value().catalog, store, nullptr, nullptr,
                        nullptr, wal::DurabilityClass::kRelaxed, exec::Budget(), nullptr, false,
                        false, nullptr, &mgr);
    d.EnableIndexPurgeDriver(/*min_deletes=*/1);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, k int64) BTREE").response.rfind("ERR", 0),
              std::string::npos);
    ASSERT_EQ(d.Dispatch("CREATE INDEX by_k ON t (k)").response.rfind("ERR", 0),
              std::string::npos);
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (1, 1), (2, 2), (3, 3)").response.rfind("ERR", 0),
              std::string::npos);
    ASSERT_EQ(d.Dispatch("DELETE FROM t WHERE id > 1").response.rfind("ERR", 0),
              std::string::npos);
    d.QueueIndexPurges();
    auto owed = d.StepIndexPurge(16);
    ASSERT_TRUE(owed.ok());
    EXPECT_FALSE(owed.value());
    EXPECT_EQ(d.index_purge_driver()->counts().read, 0u);
    stats::RelationStats::ResetForTest();
}

// ---- The threshold, on the driver alone -------------------------------------

TEST(IndexPurgeDriverThresholdTest, ARelationIsQueuedOnceAndNotAgainWhileItsPassRuns) {
    exec::IndexPurgeDriver driver(/*min_deletes=*/10);
    driver.Offer(7, 9);
    EXPECT_FALSE(driver.owed());
    driver.Offer(7, 10);
    driver.Offer(7, 500);
    EXPECT_EQ(driver.queued(), 1u);
    ASSERT_EQ(driver.Current(), std::optional<catalog::Oid>(7));
    EXPECT_EQ(driver.queued(), 0u);
    driver.Offer(7, 5000);
    EXPECT_EQ(driver.queued(), 0u) << "its pass is running";
    EXPECT_EQ(driver.Current(), std::optional<catalog::Oid>(7)) << "the same pass, not a new one";
}

// A pass over a relation with two indexes reads every entry twice; what buys
// the next is a fifth of what one index kept - the rows - not of their sum.
TEST_F(IndexPurgeDriverTest, TheNextPassCostsAFifthOfTheRowsHoweverManyIndexes) {
    dispatcher_->EnableIndexPurgeDriver(/*min_deletes=*/10);
    Table(1000);
    Ok("CREATE INDEX by_k2 ON t (k)");
    Ok("DELETE FROM t WHERE id > 900");  // 100: over the floor
    EXPECT_EQ(Drive(), 1u);
    EXPECT_EQ(Counts().read, 2000u);
    // About 900 rows are left an index: 180 deletes buy the next, not 360.
    Ok("DELETE FROM t WHERE id > 730");  // 170
    EXPECT_EQ(Drive(), 0u);
    Ok("DELETE FROM t WHERE id > 710");  // 190 since the last pass was bought
    EXPECT_EQ(Drive(), 1u);
}

// ---- Beside a DDL on another core (DW-R2) -------------------------------------

// Core 0's tick drives the pass; core 1 writes, and between two of core 0's
// slices runs a DDL that takes the relation `X` - granted, since nothing is
// held there. The index it built is in no memo of core 0's: the next slice
// makes the memo current under its `IX` and the pass walks that index too.
TEST(IndexPurgeDriverRigTest, ADdlOnAnotherCoreIsGrantedBetweenTwoSlicesAndItsIndexIsWalked) {
    stats::RelationStats::ResetForTest();
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    d0.EnableIndexPurgeDriver(/*min_deletes=*/100);
    const auto ok = [](CommandDispatcher& d, const std::string& sql) {
        const std::string out = d.Dispatch(sql).response;
        ASSERT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    };
    ok(d0, "CREATE TABLE t (id int64, k int64, u int64) BTREE");
    ok(d0, "CREATE INDEX by_k ON t (k)");
    for (int at = 1; at <= 3000; at += 100) {
        std::string sql = "INSERT INTO t VALUES ";
        for (int i = at; i < at + 100; ++i) {
            if (i != at) sql += ", ";
            sql += "(" + std::to_string(i) + ", " + std::to_string(i % 8) + ", " +
                   std::to_string(i) + ")";
        }
        ok(d1, sql);
    }
    ok(d1, "DELETE FROM t WHERE id > 500");
    const std::string probe = "SELECT id FROM t WHERE k = 5 ORDER BY id";
    const std::string before = d1.Dispatch(probe).response;

    d0.QueueIndexPurges();
    auto first = d0.StepIndexPurge(1);
    ASSERT_TRUE(first.ok()) << first.status().message();
    ASSERT_TRUE(first.value()) << "the index must span leaves";
    // `UNIQUE` keeps the `X` build (`index.md` §10.3).
    ok(d1, "CREATE UNIQUE INDEX by_u ON t (u)");
    for (int i = 0; i < 1000; ++i) {
        auto owed = d0.StepIndexPurge(1);
        ASSERT_TRUE(owed.ok()) << owed.status().message();
        if (!owed.value()) break;
    }
    const auto entries = [&](const std::string& index) {
        const std::string shown = d1.Dispatch("SELECT name, entries FROM sys.indexes").response;
        return std::stoull(
            testing_view::ReadViewText(shown).Find("name", index, "entries").value_or("0"));
    };
    EXPECT_LT(entries("by_k"), 510u);
    EXPECT_LT(entries("by_u"), 510u) << "the index core 1 built mid-pass was walked";
    EXPECT_EQ(d0.Dispatch(probe).response, before);
    EXPECT_EQ(d1.Dispatch(probe).response, before);

    // DW-R5 across cores: a view core 1 holds is one the horizon core 0's
    // pass reads, so the rows it still sees keep their entries.
    Session reader;
    ASSERT_EQ(d1.Dispatch("BEGIN ISOLATION LEVEL REPEATABLE READ", &reader).response.rfind("ERR", 0),
              std::string::npos);
    const std::string seen = d1.Dispatch(probe, &reader).response;
    ok(d1, "DELETE FROM t WHERE id > 200");  // 300 rows: over the floor of 100
    const std::uint64_t held = entries("by_k");
    const std::uint64_t removed = d0.index_purge_driver()->counts().removed;
    d0.QueueIndexPurges();
    for (int i = 0; i < 1000; ++i) {
        auto owed = d0.StepIndexPurge(16);
        ASSERT_TRUE(owed.ok()) << owed.status().message();
        if (!owed.value()) break;
    }
    EXPECT_EQ(d0.index_purge_driver()->counts().passes, 2u);
    EXPECT_EQ(d0.index_purge_driver()->counts().removed, removed)
        << "a pass took entries core 1's view still reaches its rows by";
    EXPECT_EQ(entries("by_k"), held);
    EXPECT_EQ(d1.Dispatch(probe, &reader).response, seen);
    ASSERT_EQ(d1.Dispatch("COMMIT", &reader).response.rfind("ERR", 0), std::string::npos);
    stats::RelationStats::ResetForTest();
}

// ---- Under a crash -----------------------------------------------------------

// A pass's records are `PURGE INDEX`'s (`FULL_PAGE_IMAGE` at `kNoTxnId`), so
// what a crash does to one is what it does to the statement's: replayed
// whole where durable, absent where not, every probe answering alike. **The
// driver keeps nothing across the restart**, its counts and the statistics it
// reads being memory's: an unlogged pass's entries stay until new deletes buy
// the next, and that pass converges on the tree a durable one left.
TEST(IndexPurgeDriverCrashTest, AnUnaskedPassCrashedDurableOrNotMountsToTheSameAnswers) {
    std::string converged;
    for (const bool synced : {true, false}) {
        sim::SimInstanceOptions options;
        options.lock_table = true;
        options.index_purge_driver = true;
        auto created = sim::SimInstance::Create(options);
        ASSERT_TRUE(created.ok()) << created.status().message();
        std::unique_ptr<sim::SimInstance> db = std::move(created.value());
        const auto ok = [&](const std::string& sql) {
            const std::string out = db->Execute(sql);
            ASSERT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
        };
        const auto entries = [&] {
            return testing_view::ReadViewText(db->Execute("SELECT name, entries FROM sys.indexes"))
                .Find("name", "by_k", "entries")
                .value_or("?");
        };
        // The harness's own step, as core 0's tick: offers, then one slice.
        const auto drive = [&] {
            for (int i = 0; i < 1000; ++i) {
                ASSERT_TRUE(db->ReclaimStep().ok());
                if (!db->dispatcher().index_purge_driver()->owed()) break;
            }
        };
        ok("CREATE TABLE t (id int64, k int64) BTREE");
        ok("CREATE INDEX by_k ON t (k)");
        for (int i = 1; i <= 600; ++i) {
            ok("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i % 9) + ")");
        }
        ok("DELETE FROM t WHERE id > 300");
        ASSERT_EQ(db->Execute("SYNC"), "OK synced");
        const std::string probe = "SELECT id, k FROM t WHERE k BETWEEN 0 AND 9 ORDER BY id";
        const std::string before = db->Execute(probe);
        if (!synced) ok("SET DURABILITY RELAXED");
        drive();
        ASSERT_GT(db->dispatcher().index_purge_driver()->counts().removed, 0u);
        const std::string purged = entries();
        if (synced) ASSERT_EQ(db->Execute("SYNC"), "OK synced");

        db->Crash();
        ASSERT_TRUE(db->Reboot().ok());
        EXPECT_EQ(db->Execute(probe), before) << (synced ? "synced" : "unsynced");
        if (synced) EXPECT_EQ(entries(), purged) << "the images were replayed";
        const std::string mounted = entries();
        drive();
        EXPECT_EQ(db->dispatcher().index_purge_driver()->counts().passes, 0u)
            << "no delete of this boot bought a pass";
        EXPECT_EQ(entries(), mounted);

        // New deletes buy one, and both histories end at one tree.
        ok("DELETE FROM t WHERE id > 200");
        ASSERT_EQ(db->Execute("SYNC"), "OK synced");
        const std::string after = db->Execute(probe);
        drive();
        EXPECT_EQ(db->dispatcher().index_purge_driver()->counts().passes, 1u);
        EXPECT_EQ(db->Execute(probe), after);
        if (synced) {
            converged = entries();
        } else {
            EXPECT_EQ(entries(), converged) << "the unlogged pass's entries went with the next";
        }
    }
    stats::RelationStats::ResetForTest();
}

}  // namespace
}  // namespace kds::server
