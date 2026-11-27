#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/index_maintain.hpp"
#include "kds/exec/index_purge_driver.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/stats/relation_stats.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/heap/rowid.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/index/index_tree.hpp"
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
    dispatcher_->EnableIndexPurgeDriver(/*min_writes=*/100);
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
    // And the write path counts nothing for a driver that is not there.
    Ok("UPDATE t SET k = k + 100 WHERE id <= 100");
    EXPECT_EQ(stats::RelationStats::Snapshot()[Oid("t")].key_updates.get(), 0u);
    EXPECT_EQ(stats::RelationStats::Snapshot()[Oid("t")].updates.get(), 100u);
}

TEST_F(IndexPurgeDriverTest, ARelationIsQueuedOnlyOnceItsDeletesReachTheThreshold) {
    dispatcher_->EnableIndexPurgeDriver(/*min_writes=*/50);
    Table(600);
    Ok("DELETE FROM t WHERE id > 551");  // 49 rows
    EXPECT_EQ(Drive(), 0u);
    EXPECT_EQ(Entries("by_k"), 600u);
    // An INSERT is no part of the signal.
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
    dispatcher_->EnableIndexPurgeDriver(/*min_writes=*/40);
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
    dispatcher_->EnableIndexPurgeDriver(/*min_writes=*/40);
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
    dispatcher_->EnableIndexPurgeDriver(/*min_writes=*/100);
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
    dispatcher_->EnableIndexPurgeDriver(/*min_writes=*/100);
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

TEST_F(IndexPurgeDriverTest, ADroppedRelationEndsItsPassWithNothingWalked) {
    dispatcher_->EnableIndexPurgeDriver(/*min_writes=*/10);
    Table(200);
    Ok("DELETE FROM t WHERE id > 100");
    dispatcher_->QueueIndexPurges();
    Ok("DROP TABLE t");
    Drive();
    EXPECT_EQ(Counts().passes, 1u);
    EXPECT_EQ(Counts().read, 0u);
    // Not offered again by counts that no longer move.
    EXPECT_EQ(Drive(), 0u);
}

// ---- A moved key's entry, with no retired slot (DW-S2) ------------------------

// DR-Q18's gap: an `UPDATE` of a key column appends an entry and leaves the
// old one, and where no row is ever deleted nothing took it out. The driver
// counts updates, so a relation whose keys only move returns to the size a
// rebuild gives it, but for a leaf's first entry.
TEST_F(IndexPurgeDriverTest, ABtreeWhoseKeysOnlyMoveReturnsToItsRebuiltSize) {
    dispatcher_->EnableIndexPurgeDriver(/*min_writes=*/100);
    Table(1000);
    for (int round = 0; round < 3; ++round) Ok("UPDATE t SET k = k + 100");
    ASSERT_EQ(Entries("by_k"), 4000u);
    const std::string probe = "SELECT id FROM t WHERE k = 303 ORDER BY id";
    const std::string before = Run(probe);
    EXPECT_EQ(Drive(), 1u);
    Ok("CREATE INDEX rebuilt ON t (k)");
    ASSERT_EQ(Entries("rebuilt"), 1000u);
    // The churned tree has more leaves than the rebuilt one, and each keeps
    // its first entry (IX2c).
    EXPECT_LE(Entries("by_k"), 1000u + 40u) << "a rebuild's entries, and a first entry a leaf";
    EXPECT_EQ(Run(probe), before);
    EXPECT_EQ(Run("SELECT id FROM t WHERE k = 3 ORDER BY id"),
              Run("SELECT id FROM t WHERE id < 0"))
        << "no row is found by a key it moved off";
}

TEST_F(IndexPurgeDriverTest, AHeapWhoseKeysOnlyMoveReturnsToItsRebuiltSize) {
    dispatcher_->EnableIndexPurgeDriver(/*min_writes=*/100);
    Ok("CREATE TABLE h (a int64, b int64) HEAP");
    Ok("CREATE INDEX hb ON h (b)");
    for (int at = 1; at <= 1000; at += 100) {
        std::string sql = "INSERT INTO h VALUES ";
        for (int i = at; i < at + 100; ++i) {
            if (i != at) sql += ", ";
            sql += "(" + std::to_string(i) + ", " + std::to_string(i % 8) + ")";
        }
        Ok(sql);
    }
    for (int round = 0; round < 3; ++round) Ok("UPDATE h SET b = b + 100");
    ASSERT_EQ(Entries("hb"), 4000u);
    const std::string probe = "SELECT a FROM h WHERE b = 303 ORDER BY a";
    const std::string before = Run(probe);
    // No slot was retired, so the heap's own release has nothing queued.
    auto released = dispatcher_->StepHeapRelease(16);
    ASSERT_TRUE(released.ok());
    EXPECT_FALSE(released.value());
    EXPECT_EQ(Entries("hb"), 4000u);
    EXPECT_EQ(Drive(), 1u);
    Ok("CREATE INDEX rebuilt ON h (b)");
    ASSERT_EQ(Entries("rebuilt"), 1000u);
    EXPECT_LE(Entries("hb"), 1000u + 40u);
    EXPECT_EQ(Run(probe), before);
}

// A heap's deletes are its slot release's to purge for: once the driver has
// read the relation's kind - at its first pass - they buy no pass of its own.
TEST_F(IndexPurgeDriverTest, AHeapsDeletesBuyNoPassOnceItsKindIsRead) {
    dispatcher_->EnableIndexPurgeDriver(/*min_writes=*/10);
    Ok("CREATE TABLE h (a int64, b int64) HEAP");
    Ok("CREATE INDEX hb ON h (b)");
    for (int i = 0; i < 100; ++i) {
        Ok("INSERT INTO h VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")");
    }
    Ok("UPDATE h SET b = b + 1000 WHERE a < 20");
    EXPECT_EQ(Drive(), 1u) << "its first pass, bought by updates, reads its kind";
    Ok("DELETE FROM h WHERE a >= 50");
    EXPECT_EQ(Drive(), 0u) << "50 deletes on a heap buy nothing";
    Ok("UPDATE h SET b = b + 1000 WHERE a < 20");
    EXPECT_EQ(Drive(), 1u) << "20 updates do";
}

// A primary key that is not the Keystone is the unique index `$pkey_<oid>`,
// its value free to move (DR): the moved value's old entry goes unasked too.
TEST_F(IndexPurgeDriverTest, AMovedPrimaryKeysOldEntriesGoUnasked) {
    dispatcher_->EnableIndexPurgeDriver(/*min_writes=*/100);
    Ok("CREATE TABLE w (k varchar(40), n int64, v int64, PRIMARY KEY (k, n))");
    for (int at = 1; at <= 300; at += 100) {
        std::string sql = "INSERT INTO w VALUES ";
        for (int i = at; i < at + 100; ++i) {
            if (i != at) sql += ", ";
            sql += "('key" + std::to_string(i) + "', 1, " + std::to_string(i) + ")";
        }
        Ok(sql);
    }
    Ok("UPDATE w SET n = n + 1");
    const std::string shown = Run("SELECT name, entries FROM sys.indexes");
    std::string pkey;
    for (const std::string& name : testing_view::ReadViewText(shown).Column("name")) {
        if (name.rfind("$pkey_", 0) == 0) pkey = name;
    }
    ASSERT_FALSE(pkey.empty()) << shown;
    ASSERT_EQ(Entries(pkey), 600u);
    EXPECT_EQ(Drive(), 1u);
    EXPECT_LE(Entries(pkey), 300u + 10u);
    EXPECT_NE(Run("SELECT v FROM w WHERE k = 'key7' AND n = 2").find("7"), std::string::npos);
    EXPECT_EQ(Run("SELECT v FROM w WHERE k = 'key7' AND n = 1"),
              Run("SELECT v FROM w WHERE k = 'none' AND n = 1"));
    EXPECT_EQ(dispatcher_->Dispatch("INSERT INTO w VALUES ('key7', 2, 0)").status.code(),
              StatusCode::kAlreadyExists);
    EXPECT_TRUE(dispatcher_->Dispatch("INSERT INTO w VALUES ('key7', 1, 0)").status.ok())
        << "the value the key moved off is free";
}

// DW-Q7 (b): what buys a pass beside a delete is an UPDATE that moved what
// an index carries - a key or a covered value - counted at the row. One that
// moved nothing leaves no entry and buys nothing, however many there are.
TEST_F(IndexPurgeDriverTest, OnlyAnUpdateThatMovesWhatAnIndexCarriesBuysAPass) {
    dispatcher_->EnableIndexPurgeDriver(/*min_writes=*/40);
    Ok("CREATE TABLE t (id int64, k int64, c int64, w int64) BTREE");
    Ok("CREATE INDEX by_k ON t (k) COVERING (c)");
    for (int i = 1; i <= 200; ++i) {
        Ok("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i % 8) +
           ", 0, 0)");
    }
    for (int round = 0; round < 20; ++round) Ok("UPDATE t SET w = w + 1");  // 4,000 updates
    Ok("UPDATE t SET k = k WHERE id <= 100");  // a key assigned the value it has
    EXPECT_EQ(Drive(), 0u) << "no update moved what the index carries";
    EXPECT_EQ(Entries("by_k"), 200u);

    Ok("UPDATE t SET k = k + 100 WHERE id <= 39");
    EXPECT_EQ(Drive(), 0u) << "39 are under the floor";
    Ok("UPDATE t SET c = c + 1 WHERE id = 40");  // a covered value moves the entry too
    ASSERT_EQ(Entries("by_k"), 240u);
    EXPECT_EQ(Drive(), 1u);
    EXPECT_GE(Counts().removed, 39u);
    EXPECT_LE(Entries("by_k"), 201u);
}

// A value that recurs on one row - a status that flips - appends an entry
// the first time it is taken and finds it there every time after. The dead
// entries are one a row an index however often it flips, and so is the count.
TEST_F(IndexPurgeDriverTest, ARowFlippingBetweenTwoValuesIsCountedOncePerEntryItLeaves) {
    dispatcher_->EnableIndexPurgeDriver(/*min_writes=*/40);
    Table(200);
    for (int flip = 0; flip < 5; ++flip) {
        Ok("UPDATE t SET k = k + 100 WHERE id <= 10");
        Ok("UPDATE t SET k = k - 100 WHERE id <= 10");
    }
    EXPECT_EQ(stats::RelationStats::Snapshot()[Oid("t")].updates.get(), 100u);
    EXPECT_EQ(stats::RelationStats::Snapshot()[Oid("t")].key_updates.get(), 10u);
    EXPECT_EQ(Entries("by_k"), 210u);
    EXPECT_EQ(Drive(), 0u) << "100 updates left 10 dead entries, under the floor";
}

// A row an `UPDATE` wrote is counted where it is written, so a statement a
// `ROLLBACK` undoes is counted too - rightly: nothing on the write path
// removes an entry, a rollback included (IX2), so the entry the undone
// update appended is the dead one.
TEST_F(IndexPurgeDriverTest, ARolledBackUpdatesEntriesAreCountedAndGo) {
    dispatcher_->EnableIndexPurgeDriver(/*min_writes=*/40);
    Table(200);
    Session writer;
    Ok("BEGIN", &writer);
    Ok("UPDATE t SET k = k + 100 WHERE id <= 50", &writer);
    Ok("ROLLBACK", &writer);
    ASSERT_EQ(Entries("by_k"), 250u);
    const std::vector<std::string> before = Probes();
    EXPECT_EQ(Drive(), 1u);
    EXPECT_LE(Entries("by_k"), 201u);
    EXPECT_EQ(Probes(), before);
}

// DW-R5 for a moved key: the row's old version is one an older view reads,
// through the old key's entry, which a pass under that view keeps.
TEST_F(IndexPurgeDriverTest, AnOlderViewFindsAMovedRowThroughItsOldKey) {
    // 150 rows: a fifth of what the index keeps stays under the floor of 40.
    dispatcher_->EnableIndexPurgeDriver(/*min_writes=*/40);
    Table(150);
    Session reader;
    Ok("BEGIN ISOLATION LEVEL REPEATABLE READ", &reader);
    const std::vector<std::string> seen = Probes(&reader);
    Ok("UPDATE t SET k = k + 100 WHERE id <= 40");
    EXPECT_EQ(Drive(), 1u);
    EXPECT_EQ(Counts().removed, 0u);
    EXPECT_EQ(Probes(&reader), seen) << "the reader's rows are still under their old keys";
    Ok("COMMIT", &reader);
    Ok("UPDATE t SET k = k + 100 WHERE id > 40 AND id <= 80");
    EXPECT_EQ(Drive(), 1u);
    EXPECT_GE(Counts().removed, 78u) << "both updates' old entries, the reader gone";
}

// Every entry of every index of `table`, the ones naming a free slot
// counted: DN-R2 says there is none.
std::uint64_t EntriesNamingAFreeSlot(storage::PageStore& store, catalog::Catalog& catalog,
                                     catalog::Oid table) {
    catalog.Revalidate();
    auto access = catalog.InitTableAccess(table);
    EXPECT_TRUE(access.ok());
    if (!access.ok()) return 0;
    std::uint64_t named = 0;
    for (const catalog::TableAccess::IndexRef& ix : access.value()->indexes) {
        const index::IndexLayout layout = exec::IndexLayoutOf(ix);
        const std::vector<std::byte> low(layout.sort_key_width(), std::byte{0});
        auto leaf = index::IndexSeekLeaf(store, ix.root_page_id, layout, low);
        EXPECT_TRUE(leaf.ok());
        for (PageId at = leaf.ok() ? leaf.value() : kInvalidPageId; at != kInvalidPageId;) {
            auto page = index::IndexFetchLeaf(store, at, layout, storage::PageAccess::kRead);
            EXPECT_TRUE(page.ok());
            if (!page.ok()) return named;
            const index::IndexLeafView view(page.value().bytes());
            for (std::uint16_t i = 0; i < view.entry_count(); ++i) {
                const heap::Rowid rowid =
                    heap::DecodeRowid(index::GetIndexRowid(layout, view.Entry(i).value()));
                auto data = store.GetForRead(rowid.page_id);
                EXPECT_TRUE(data.ok());
                if (!data.ok()) continue;
                const heap::PageView slots(data.value().bytes());
                auto info = slots.DebugSlotInfo(rowid.slot);
                if (info.ok() && (info.value().flags & heap::kSlotFlagFree) != 0) ++named;
            }
            at = view.right_sibling();
        }
    }
    return named;
}

// A heap's slot release and a driver pass on one relation, a one-page or
// one-leaf step of each by turns: the release's verify runs between the
// pass's slices and the pass between its phases, and no slot an entry still
// names is freed (DN-R2) - the freed slots are then taken again.
TEST_F(IndexPurgeDriverTest, AHeapsReleaseAndAPassSteppedByTurnsFreeNoSlotAnEntryNames) {
    dispatcher_->EnableIndexPurgeDriver(/*min_writes=*/100);
    Ok("CREATE TABLE h (a int64, b int64) HEAP");
    Ok("CREATE INDEX hb ON h (b)");
    for (int at = 0; at < 1500; at += 100) {
        std::string sql = "INSERT INTO h VALUES ";
        for (int i = at; i < at + 100; ++i) {
            if (i != at) sql += ", ";
            sql += "(" + std::to_string(i) + ", " + std::to_string(i % 9) + ")";
        }
        Ok(sql);
    }
    Ok("DELETE FROM h WHERE a >= 500 AND a < 1000");
    Ok("UPDATE h SET b = b + 100 WHERE a < 300");
    dispatcher_->QueueIndexPurges();
    bool releasing = true;
    bool purging = true;
    for (int steps = 0; (releasing || purging) && steps < 100000; ++steps) {
        if (releasing) {
            auto owed = dispatcher_->StepHeapRelease(1);
            ASSERT_TRUE(owed.ok()) << owed.status().message();
            releasing = owed.value();
        }
        if (purging) {
            auto owed = dispatcher_->StepIndexPurge(1);
            ASSERT_TRUE(owed.ok()) << owed.status().message();
            purging = owed.value();
        }
        EXPECT_EQ(EntriesNamingAFreeSlot(store_, boot_->catalog, Oid("h")), 0u) << "step " << steps;
    }
    EXPECT_EQ(Counts().passes, 1u);
    EXPECT_GE(dispatcher_->last_heap_release().freed, 490u);
    EXPECT_LE(Entries("hb"), 1000u + 40u);
    for (int at = 0; at < 400; at += 100) {
        std::string sql = "INSERT INTO h VALUES ";
        for (int i = at; i < at + 100; ++i) {
            if (i != at) sql += ", ";
            sql += "(" + std::to_string(10000 + i) + ", 7)";
        }
        Ok(sql);
    }
    for (const char* b : {"7", "103", "3"}) {
        EXPECT_EQ(Run(std::string("SELECT a FROM h WHERE b = ") + b + " ORDER BY a"),
                  Run(std::string("SELECT a FROM h WHERE b + 0 = ") + b + " ORDER BY a"))
            << "b = " << b;
    }
}

// A declared Keystone's tombstone keeps its Keystone entry through a pass -
// it is what refuses the key named again until `PURGE` - while the deleted
// row's other entries and a moved value's old one go.
TEST_F(IndexPurgeDriverTest, ADeclaredKeystonesTombstoneKeepsItsKeyThroughAPass) {
    dispatcher_->EnableIndexPurgeDriver(/*min_writes=*/40);
    Ok("CREATE TABLE p (id int64 PRIMARY KEY, v int64) HEAP");
    Ok("CREATE INDEX pv ON p (v)");
    std::string sql = "INSERT INTO p VALUES ";
    for (int i = 1; i <= 100; ++i) {
        if (i != 1) sql += ", ";
        sql += "(" + std::to_string(i) + ", " + std::to_string(i) + ")";
    }
    Ok(sql);
    Ok("DELETE FROM p WHERE id > 50");
    Ok("UPDATE p SET v = v + 1000 WHERE id <= 20");
    EXPECT_EQ(Drive(), 1u);
    EXPECT_LE(Entries("pv"), 50u + 2u);
    EXPECT_EQ(dispatcher_->Dispatch("INSERT INTO p VALUES (60, 0)").status.code(),
              StatusCode::kAlreadyExists)
        << "a deleted key stays bound until PURGE";
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
    d.EnableIndexPurgeDriver(/*min_writes=*/1);
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
    exec::IndexPurgeDriver driver(/*min_writes=*/10);
    driver.Offer(7, /*deletes=*/5, /*key_updates=*/4);
    EXPECT_FALSE(driver.owed());
    driver.Offer(7, 5, 5);  // the two add up
    driver.Offer(7, 500, 5);
    EXPECT_EQ(driver.queued(), 1u);
    ASSERT_EQ(driver.Current(), std::optional<catalog::Oid>(7));
    EXPECT_EQ(driver.queued(), 0u);
    driver.Offer(7, 5000, 5);
    EXPECT_EQ(driver.queued(), 0u) << "its pass is running";
    EXPECT_EQ(driver.Current(), std::optional<catalog::Oid>(7)) << "the same pass, not a new one";
}

// A pass over a relation with two indexes reads every entry twice; what buys
// the next is a fifth of what one index kept - the rows - not of their sum.
TEST_F(IndexPurgeDriverTest, TheNextPassCostsAFifthOfTheRowsHoweverManyIndexes) {
    dispatcher_->EnableIndexPurgeDriver(/*min_writes=*/10);
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
    d0.EnableIndexPurgeDriver(/*min_writes=*/100);
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
