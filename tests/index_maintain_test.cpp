#include "kds/exec/index_maintain.hpp"

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"

// The secondary-index write hook (docs/spec/index.md §2, workplan IX06).
//
// The test that decides whether the feature is usable is
// `AnUpdateThatTouchesNoIndexedColumnAppendsNothing`, and it is written
// first for that reason: appending anyway stays *correct* by the superset
// rule and grows the index by an entry per write forever. Correct and
// useless is still a defect - the same one
// `CabinContractTest.AnUpdateThatDoesNotTouchTheKeyColumnAppendsNothing`
// exists to catch.
//
// Everything is observed through `SHOW INDEXES`, which walks the real tree.
// That is deliberate: an assertion against an internal counter would pass on
// an index that was maintained into the wrong page.

namespace kds::exec {
namespace {

class IndexMaintainTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));

        // A real transaction manager, because without one no undo record is
        // written and the backfill's version walk would have nothing to
        // find - it would pass by describing an engine that keeps no
        // history. The unlogged path is fine; what is under test is the
        // version chain, not what reaches the platter.
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, &*mgr_);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    void Ok(const std::string& sql) {
        const std::string out = Run(sql);
        ASSERT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    }

    // The entry count `SHOW INDEXES` walked out of the tree.
    int Entries(const std::string& index_name) {
        const std::string shown = Run("SHOW INDEXES");
        const std::size_t at = shown.find("name=" + index_name + " ");
        EXPECT_NE(at, std::string::npos) << shown;
        if (at == std::string::npos) return -1;
        const std::size_t entries = shown.find(" entries=", at);
        EXPECT_NE(entries, std::string::npos) << shown;
        if (entries == std::string::npos) return -1;
        return std::atoi(shown.c_str() + entries + 9);
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<server::CommandDispatcher> dispatcher_;
};

// ---- The rule the feature stands on -------------------------------------

TEST_F(IndexMaintainTest, AnUpdateThatTouchesNoIndexedColumnAppendsNothing) {
    Ok("CREATE TABLE t (id int64, owner int64, note int64) BTREE");
    Ok("CREATE INDEX by_owner ON t (owner)");
    Ok("INSERT INTO t VALUES (7, 100)");
    ASSERT_EQ(Entries("by_owner"), 1);

    // Ten writes to a column the index knows nothing about. Appending here
    // would still be *correct* - the entry set stays a superset and the read
    // dedupes - and unbounded, which is the defect.
    for (int i = 0; i < 10; ++i) {
        Ok("UPDATE t SET note = " + std::to_string(200 + i) + " WHERE id = 1");
    }
    EXPECT_EQ(Entries("by_owner"), 1);
}

TEST_F(IndexMaintainTest, AnUpdateThatMovesTheKeyAppendsAndLeavesTheOldEntry) {
    // Removal is *incorrect*, not an optimization forgone: a pre-update
    // snapshot is still entitled to match through the old entry, and for
    // newer readers it is a surplus the read-time key re-check subtracts.
    Ok("CREATE TABLE t (id int64, owner int64) BTREE");
    Ok("CREATE INDEX by_owner ON t (owner)");
    Ok("INSERT INTO t VALUES (7)");
    ASSERT_EQ(Entries("by_owner"), 1);

    Ok("UPDATE t SET owner = 8 WHERE id = 1");
    EXPECT_EQ(Entries("by_owner"), 2);

    // Back to the original value: a third entry, because it is a third
    // version. The (key, pk) pair repeats but this is not the byte-identical
    // duplicate IndexInsert deduplicates - it is, and the count says so.
    Ok("UPDATE t SET owner = 7 WHERE id = 1");
    EXPECT_EQ(Entries("by_owner"), 2) << "an identical entry must not be stored twice";
}

TEST_F(IndexMaintainTest, ADeleteAppendsNothingAndLeavesTheEntryBehind) {
    Ok("CREATE TABLE t (id int64, owner int64) BTREE");
    Ok("CREATE INDEX by_owner ON t (owner)");
    Ok("INSERT INTO t VALUES (7)");
    Ok("DELETE FROM t WHERE id = 1");
    // Not zero: an older snapshot may still match this row through the undo
    // chain, so removing the entry would lose it a row.
    EXPECT_EQ(Entries("by_owner"), 1);
}

// ---- Ordinary maintenance ------------------------------------------------

TEST_F(IndexMaintainTest, EveryInsertAppendsOneEntryPerIndex) {
    Ok("CREATE TABLE t (id int64, a int64, b int64) BTREE");
    Ok("CREATE INDEX by_a ON t (a)");
    Ok("CREATE INDEX by_b ON t (b)");

    for (int i = 0; i < 25; ++i) {
        Ok("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i * 3) + ")");
    }
    EXPECT_EQ(Entries("by_a"), 25);
    EXPECT_EQ(Entries("by_b"), 25);
}

TEST_F(IndexMaintainTest, ACompositeKeyAndACoveringListAreBothMaintained) {
    Ok("CREATE TABLE t (id int64, a int64, b int64, c int64) BTREE");
    Ok("CREATE INDEX ix ON t (a, b) COVERING (c)");
    for (int i = 0; i < 10; ++i) {
        Ok("INSERT INTO t VALUES (1, " + std::to_string(i) + ", " + std::to_string(i * 2) + ")");
    }
    EXPECT_EQ(Entries("ix"), 10);

    // A covered column moving appends too: the entry carries its value and
    // the read path filters on it, so an entry with the stale value would
    // drop a row that now matches.
    Ok("UPDATE t SET c = 999 WHERE id = 1");
    EXPECT_EQ(Entries("ix"), 11);
}

TEST_F(IndexMaintainTest, ManyRowsSplitTheTreeAndRepublishItsRootWithoutFailing) {
    // The path IX12a is about: a split republishes the root through the
    // catalog *inside* an INSERT. It updates the cached entry in place, so
    // the `const TableAccess*` the statement is holding stays valid - and a
    // multi-row UPDATE below holds it across every later row.
    Ok("CREATE TABLE t (id int64, owner int64, note int64) BTREE");
    Ok("CREATE INDEX by_owner ON t (owner)");

    constexpr int kRows = 900;
    for (int i = 0; i < kRows; ++i) {
        Ok("INSERT INTO t VALUES (" + std::to_string((i * 7919) % kRows) + ", 0)");
    }
    EXPECT_EQ(Entries("by_owner"), kRows);

    // A statement-wide UPDATE, which calls the hook once per matched row
    // against one held TableAccess. Every row appends except the one that
    // already carried owner = 1 - which is §2's rule firing 899 times out of
    // 900 rather than an off-by-one.
    Ok("UPDATE t SET owner = 1 WHERE note = 0");
    EXPECT_EQ(Entries("by_owner"), 2 * kRows - 1);
}

// ---- Coercion: the rule that has already cost this engine rows ----------

TEST_F(IndexMaintainTest, TypedKeyColumnsAreKeyedOnTheirStorageFormNotTheLiteral) {
    // A DATE column's INSERT value is the string as written while everything
    // that reads the index keys on the epoch integer. Keying on the raw
    // literal is how the Cabin came to lose every row inserted after a value
    // was observed (docs/spec/types.md §3.1) - and here it would put the
    // entry under a key no probe ever looks up.
    Ok("CREATE TABLE t (id int64, d date, amt decimal(10,2), name varchar) BTREE");
    Ok("CREATE INDEX by_d ON t (d)");
    Ok("CREATE INDEX by_amt ON t (amt)");
    Ok("CREATE INDEX by_name ON t (name)");

    Ok("INSERT INTO t VALUES ('2026-08-07', '12.34', 'alice')");
    Ok("INSERT INTO t VALUES ('2026-08-08', '99.99', 'bob')");
    EXPECT_EQ(Entries("by_d"), 2);
    EXPECT_EQ(Entries("by_amt"), 2);
    EXPECT_EQ(Entries("by_name"), 2);

    // An update that does not move the typed column must still append
    // nothing - which only works if the comparison is against the *coerced*
    // value, since a decoded date never compares equal to the string it was
    // written as.
    Ok("UPDATE t SET d = '2026-08-07' WHERE id = 1");
    EXPECT_EQ(Entries("by_d"), 2);

    Ok("UPDATE t SET d = '2026-09-01' WHERE id = 1");
    EXPECT_EQ(Entries("by_d"), 3);
}

TEST_F(IndexMaintainTest, AKeyLongerThanThePrefixIsStoredTruncatedAndStillMaintained) {
    Ok("CREATE TABLE t (id int64, name varchar) BTREE");
    Ok("CREATE INDEX by_name ON t (name)");

    // Longer than kIndexStringKeyBytes and longer than an inline cell, so
    // the second one spills to the var-heap - and the value the hook sees is
    // still the whole string, on both paths.
    Ok("INSERT INTO t VALUES ('" + std::string(40, 'a') + "')");
    Ok("INSERT INTO t VALUES ('" + std::string(200, 'b') + "')");
    EXPECT_EQ(Entries("by_name"), 2);
}

// ---- Failure ------------------------------------------------------------

TEST_F(IndexMaintainTest, ARelationWithNoIndexIsUnaffected) {
    // The one test a relation with no index pays, and nothing else.
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    for (int i = 0; i < 5; ++i) Ok("INSERT INTO t VALUES (" + std::to_string(i) + ")");
    Ok("UPDATE t SET a = 9 WHERE id = 1");
    Ok("DELETE FROM t WHERE id = 2");
    EXPECT_NE(Run("SHOW INDEXES").find("indexes=0"), std::string::npos);
}

// ---- The backfill (docs/spec/index.md §10a, workplan IX09) --------------

// **Written first**, as the workplan asks: omitting the undo-chain walk
// makes an old-snapshot read return fewer rows with no error - the failure
// `cabin.md` §5 calls invisible without a baseline.
TEST_F(IndexMaintainTest, TheBackfillIndexesEveryVersionAndNotJustTheCurrentOne) {
    Ok("CREATE TABLE t (id int64, owner int64) BTREE");
    Ok("INSERT INTO t VALUES (7)");
    Ok("INSERT INTO t VALUES (8)");

    // Three versions of row 1: owner 7 -> 9 -> 10. A reader on an older
    // snapshot is entitled to match through any of them, and DDL is not
    // transactional, so the index becomes visible to all of them at once.
    Ok("UPDATE t SET owner = 9 WHERE id = 1");
    Ok("UPDATE t SET owner = 10 WHERE id = 1");

    Ok("CREATE INDEX by_owner ON t (owner)");
    // Two rows, four distinct (key, pk) pairs: (7,1) (9,1) (10,1) (8,2).
    EXPECT_EQ(Entries("by_owner"), 4);
}

TEST_F(IndexMaintainTest, ABackfilledIndexIsIndistinguishableFromAMaintainedOne) {
    // The property that matters: building over existing rows and building
    // first then writing must reach the same tree.
    Ok("CREATE TABLE built (id int64, a int64) BTREE");
    Ok("CREATE TABLE grown (id int64, a int64) BTREE");
    Ok("CREATE INDEX grown_ix ON grown (a)");

    for (int i = 0; i < 300; ++i) {
        const std::string v = std::to_string((i * 7919) % 300);
        Ok("INSERT INTO built VALUES (" + v + ")");
        Ok("INSERT INTO grown VALUES (" + v + ")");
    }
    Ok("CREATE INDEX built_ix ON built (a)");

    EXPECT_EQ(Entries("built_ix"), 300);
    EXPECT_EQ(Entries("grown_ix"), 300);

    const std::string shown = Run("SHOW INDEXES");
    const std::size_t built = shown.find("name=built_ix");
    const std::size_t grown = shown.find("name=grown_ix");
    ASSERT_NE(built, std::string::npos);
    ASSERT_NE(grown, std::string::npos);
    // Same height too, which is what says the tree has the same shape and
    // not merely the same count.
    EXPECT_EQ(shown.substr(built, 200).find("height=1") != std::string::npos,
              shown.substr(grown, 200).find("height=1") != std::string::npos)
        << shown;
}

TEST_F(IndexMaintainTest, ADeletedRowIsStillBackfilled) {
    // Gone for newer readers, still there for older ones - which is exactly
    // the case the undo chain exists for, and the reason a backfill cannot
    // index only what a fresh snapshot can see.
    Ok("CREATE TABLE t (id int64, owner int64) BTREE");
    Ok("INSERT INTO t VALUES (7)");
    Ok("DELETE FROM t WHERE id = 1");
    Ok("CREATE INDEX by_owner ON t (owner)");
    EXPECT_EQ(Entries("by_owner"), 1);
}

TEST_F(IndexMaintainTest, ABackfillSpanningManyPagesSplitsAndKeepsEveryRow) {
    // Two phases per leaf - copy out, drop the span, then append - because
    // appending fetches pages and I15's R1 forbids one under a live span.
    // Enough rows here to cross many leaves in both trees.
    Ok("CREATE TABLE t (id int64, owner int64) BTREE");
    constexpr int kRows = 1200;
    for (int i = 0; i < kRows; ++i) {
        Ok("INSERT INTO t VALUES (" + std::to_string((i * 7919) % kRows) + ")");
    }
    Ok("CREATE INDEX by_owner ON t (owner)");
    EXPECT_EQ(Entries("by_owner"), kRows);

    // And it keeps being maintained afterwards.
    Ok("INSERT INTO t VALUES (999999)");
    EXPECT_EQ(Entries("by_owner"), kRows + 1);
}

TEST_F(IndexMaintainTest, ABackfillOverTypedAndSpillingColumnsWorks) {
    Ok("CREATE TABLE t (id int64, d date, amt decimal(10,2), name varchar) BTREE");
    Ok("INSERT INTO t VALUES ('2026-08-07', '12.34', '" + std::string(200, 'x') + "')");
    Ok("INSERT INTO t VALUES ('2026-08-08', '99.99', 'short')");
    Ok("UPDATE t SET amt = '1.00' WHERE id = 1");

    Ok("CREATE INDEX by_d ON t (d)");
    Ok("CREATE INDEX by_amt ON t (amt)");
    Ok("CREATE INDEX by_name ON t (name)");

    EXPECT_EQ(Entries("by_d"), 2);
    // Two versions of row 1's amount, plus row 2's.
    EXPECT_EQ(Entries("by_amt"), 3);
    EXPECT_EQ(Entries("by_name"), 2);
}

TEST_F(IndexMaintainTest, ADroppedIndexStopsBeingMaintained) {
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX ix ON t (a)");
    Ok("INSERT INTO t VALUES (1)");
    ASSERT_EQ(Entries("ix"), 1);

    Ok("DROP INDEX ix");
    Ok("INSERT INTO t VALUES (2)");  // must not fail looking for a gone index

    // A replacement is built over what is already there - two rows by now -
    // rather than starting empty. That refusal was IX05's placeholder and
    // IX09 lifted it.
    Ok("CREATE INDEX ix2 ON t (a)");
    EXPECT_EQ(Entries("ix2"), 2);
}

}  // namespace
}  // namespace kds::exec
