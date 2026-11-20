#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/index_maintain.hpp"
#include "kds/exec/index_purge.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/index/index_tree.hpp"
#include "kds/txn/lock_table.hpp"
#include "kds/txn/manager.hpp"
#include "sim/instance.hpp"
#include "view_text.hpp"

// DD-S1 and DD-S2 (`instructions/v3.2.0/workorder-dd-index-entry-purge.md`):
// the leaf primitive, the judge and `PURGE INDEX <name>`. **A purge changes
// no result**: every probe answers the same bytes before and after it, and
// with it between a reader's `BEGIN` and its probe.

namespace kds::server {
namespace {

using Entry = std::vector<std::byte>;

class IndexPurgeTest : public ::testing::Test {
protected:
    void SetUp() override {
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

    DispatchOutcome Do(const std::string& sql, Session* s = nullptr) {
        return dispatcher_->Dispatch(sql, s);
    }
    std::string Run(const std::string& sql, Session* s = nullptr) { return Do(sql, s).response; }
    void Ok(const std::string& sql, Session* s = nullptr) {
        const std::string out = Run(sql, s);
        ASSERT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    }
    // Entries a `PURGE INDEX` removed, read off its reply.
    std::uint64_t Purge(const std::string& index) {
        const DispatchOutcome out = Do("PURGE INDEX " + index);
        EXPECT_TRUE(out.status.ok()) << out.response;
        EXPECT_EQ(out.response.rfind("PURGED INDEX name=" + index, 0), 0u) << out.response;
        return out.rows_affected;
    }
    std::string Field(const std::string& name, const std::string& column) {
        const std::string shown = Run("SELECT name, " + column + " FROM sys.indexes");
        return testing_view::ReadViewText(shown).Find("name", name, column).value_or("<absent>");
    }
    std::uint64_t Count(const std::string& name) { return std::stoull(Field(name, "entries")); }

    const catalog::TableAccess& Access(const std::string& table) {
        auto oid = boot_->catalog.ResolveRelation("", table, catalog::kNamespacePublic);
        EXPECT_TRUE(oid.ok());
        auto access = boot_->catalog.InitTableAccess(oid.value());
        EXPECT_TRUE(access.ok());
        return *access.value();
    }
    const catalog::TableAccess::IndexRef& Ref(const catalog::TableAccess& ta,
                                             const std::string& name) {
        for (const auto& ix : ta.indexes) {
            if (ix.name == name) return ix;
        }
        ADD_FAILURE() << "no index " << name;
        return ta.indexes.front();
    }
    index::IndexLayout Layout(const catalog::TableAccess::IndexRef& ix) {
        index::IndexLayout layout;
        layout.key_width = ix.key_width;
        layout.covered_width = static_cast<std::uint16_t>(ix.entry_width - ix.key_width -
                                                          index::kIndexTupleKeyWidth);
        return layout;
    }
    std::vector<Entry> Entries(const std::string& table, const std::string& name) {
        const catalog::TableAccess& ta = Access(table);
        const auto& ix = Ref(ta, name);
        auto root = exec::SlotRoot(store_, ta.anchor_page_id, ix.index_oid);
        EXPECT_TRUE(root.ok());
        std::vector<Entry> out;
        EXPECT_TRUE(index::IndexVisit(store_, root.value(), Layout(ix), storage::PageAccess::kRead,
                                      [&](PageId, index::IndexLeafView& leaf, std::uint16_t at)
                                          -> StatusOr<storage::VisitControl> {
                                          auto e = leaf.Entry(at);
                                          if (!e.ok()) return e.status();
                                          out.emplace_back(e.value().begin(), e.value().end());
                                          return storage::VisitControl::kContinue;
                                      })
                        .ok());
        return out;
    }
    std::uint32_t Leaves(const std::string& table, const std::string& name) {
        const catalog::TableAccess& ta = Access(table);
        const auto& ix = Ref(ta, name);
        auto root = exec::SlotRoot(store_, ta.anchor_page_id, ix.index_oid);
        auto n = index::IndexLeafCount(store_, root.value(), Layout(ix));
        EXPECT_TRUE(n.ok());
        return n.value();
    }
    // Every entry of `rebuilt` is served in `purged` by the judge's own
    // comparison: a purged tree is a superset of a rebuild's.
    void ExpectSuperset(const std::string& table, const std::string& purged,
                        const std::string& rebuilt) {
        const catalog::TableAccess& ta = Access(table);
        const auto& ix = Ref(ta, purged);
        const std::vector<Entry> have = Entries(table, purged);
        for (const Entry& e : Entries(table, rebuilt)) {
            EXPECT_TRUE(std::any_of(have.begin(), have.end(), [&](const Entry& h) {
                return exec::SameOwedEntry(ta, ix, h, e);
            })) << "an entry a rebuild keeps is gone from " << purged;
        }
    }

    // The probes a purge must answer alike, as reply text.
    std::vector<std::string> Probes() {
        std::vector<std::string> out;
        for (int v = 0; v < 8; ++v) {
            out.push_back(Run("SELECT id, k, c FROM t WHERE k = " + std::to_string(v) +
                              " ORDER BY id"));
            out.push_back(Run("SELECT id FROM t WHERE k = " + std::to_string(v) + " AND c > 30 "
                              "ORDER BY id"));
        }
        out.push_back(Run("SELECT id FROM t WHERE k BETWEEN 2 AND 105 ORDER BY id"));
        out.push_back(Run("SELECT id FROM t WHERE k = 103 ORDER BY id"));
        return out;
    }

    // A relation whose index carries every kind of surplus: rolled-back
    // inserts, purged rows, a key moved away and back, a covered column
    // churned, and committed deletes.
    void Churned() {
        Ok("CREATE TABLE t (id int64, k int64, c int64) BTREE");
        Ok("CREATE INDEX by_k ON t (k) COVERING (c)");
        for (int i = 1; i <= 400; ++i) {
            Ok("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i % 8) +
               ", " + std::to_string(i) + ")");
        }
        Session s;
        Ok("BEGIN", &s);
        for (int i = 1000; i < 1100; ++i) {
            Ok("INSERT INTO t VALUES (" + std::to_string(i) + ", 3, 3)", &s);
        }
        Ok("ROLLBACK", &s);
        Ok("UPDATE t SET k = k + 100 WHERE id <= 100");
        Ok("UPDATE t SET k = k - 100 WHERE id <= 50");
        Ok("UPDATE t SET c = c + 1000 WHERE id > 100 AND id <= 200");
        Ok("DELETE FROM t WHERE id > 350");
        Ok("DELETE FROM t WHERE id > 380");  // nothing more; a no-op delete
        Ok("PURGE FROM t WHERE id > 390");
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::unique_ptr<txn::LockTable> locks_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- The leaf primitive (DD-S1) -------------------------------------------

TEST(IndexLeafRemoveTest, RemovingClosesTheArrayAndRefusesTheFirstEntry) {
    alignas(8) std::array<std::byte, kPageSize> page{};
    index::IndexLayout layout;
    layout.key_width = 8;
    layout.covered_width = 0;
    auto leaf = index::IndexLeafView::CreateEmpty(page, layout, /*owner_oid=*/7);
    ASSERT_TRUE(leaf.ok());
    for (std::uint8_t i = 1; i <= 4; ++i) {
        Entry e(layout.leaf_entry_width(), std::byte{0});
        e[7] = std::byte{i};
        ASSERT_TRUE(leaf.value().InsertEntry(e).ok());
    }
    EXPECT_EQ(leaf.value().RemoveEntry(0).code(), StatusCode::kInvalidArgument)
        << "DD-Q3 (a): a leaf's first entry is its left neighbour's bound";
    EXPECT_EQ(leaf.value().RemoveEntry(4).code(), StatusCode::kOutOfRange);
    ASSERT_TRUE(leaf.value().RemoveEntry(1).ok());
    ASSERT_EQ(leaf.value().entry_count(), 3u);
    EXPECT_EQ(leaf.value().Entry(0).value()[7], std::byte{1});
    EXPECT_EQ(leaf.value().Entry(1).value()[7], std::byte{3});
    EXPECT_EQ(leaf.value().Entry(2).value()[7], std::byte{4});
    // The vacated slot is zero, so an image depends on the entries alone.
    const std::size_t tail = index::kIndexLeafEntriesOffset + 3 * layout.leaf_entry_width();
    EXPECT_TRUE(std::all_of(page.begin() + tail, page.begin() + tail + layout.leaf_entry_width(),
                            [](std::byte b) { return b == std::byte{0}; }));
}

// ---- The statement (DD-S2) ------------------------------------------------

TEST_F(IndexPurgeTest, APurgeChangesNoResultAndLeavesASupersetOfARebuild) {
    Churned();
    const std::vector<std::string> before = Probes();
    const std::uint64_t had = Count("by_k");
    const std::uint64_t removed = Purge("by_k");
    EXPECT_GT(removed, 0u);
    EXPECT_EQ(Count("by_k"), had - removed);
    EXPECT_EQ(Probes(), before) << "a purge changes no result";

    // A purged tree is a superset of a rebuild's but for the deleted rows a
    // rebuild still walks (DD-Q1 (b)), so those are purged first; and on a
    // quiescent tree - no reader, no undecided writer - the surplus is
    // first entries only.
    Ok("PURGE FROM t WHERE id > 350");
    Ok("CREATE INDEX twin ON t (k) COVERING (c)");
    ExpectSuperset("t", "by_k", "twin");
    EXPECT_LE(Count("by_k"), Count("twin") + Leaves("t", "by_k"))
        << "at most one dead entry a leaf: its first";
    EXPECT_EQ(Purge("by_k"), 0u) << "a second run finds nothing more";

    // Writes after it land where a probe finds them (DD-R4): the coverage
    // check reads a sibling's first entry, which no purge removed.
    for (int i = 2000; i < 2300; ++i) {
        Ok("INSERT INTO t VALUES (" + std::to_string(i) + ", 5, " + std::to_string(i) + ")");
    }
    const std::string fives = Run("SELECT id FROM t WHERE k = 5 ORDER BY id");
    for (int i = 2000; i < 2300; ++i) {
        EXPECT_NE(fives.find(std::to_string(i)), std::string::npos) << i;
    }
}

TEST_F(IndexPurgeTest, OneLeafAnAttemptResumesWhereTheLastEndedAndMissesNothing) {
    // The attempt boundary the statement crosses every
    // `kPurgeIndexLeavesPerAttempt` leaves, crossed at every leaf: each
    // attempt starts at the first entry of the leaf the last one did not
    // reach, which no purge removes.
    Churned();
    const std::vector<std::string> before = Probes();
    const std::uint32_t leaves = Leaves("t", "by_k");
    ASSERT_GT(leaves, 2u) << "the history must span leaves for the boundary to be crossed";
    const catalog::TableAccess& ta = Access("t");
    const auto& ix = Ref(ta, "by_k");
    constexpr std::uint64_t kTxn = 900001;
    txn::LockHoldings holdings;
    auto ix_lock = locks_->TryAcquire(kTxn, txn::LockKey::Relation(ta.oid),
                                      txn::LockMode::kIntentionExclusive, holdings);
    ASSERT_TRUE(ix_lock.ok() && ix_lock.value());
    std::uint64_t removed = 0;
    std::uint32_t attempts = 0;
    std::vector<std::byte> resume;
    do {
        exec::IndexPurgeAttempt attempt;
        ASSERT_TRUE(exec::PurgeIndexLeaves(store_, boot_->catalog, ta, &*mgr_, /*wal=*/nullptr,
                                           *locks_, kTxn, holdings, ix, resume,
                                           /*max_leaves=*/1, attempt)
                        .ok());
        removed += attempt.removed;
        resume = std::move(attempt.resume);
        ++attempts;
    } while (!resume.empty() && attempts <= leaves);
    locks_->Release(kTxn, holdings);
    EXPECT_EQ(attempts, leaves) << "one attempt a leaf, none walked twice, none skipped";
    EXPECT_GT(removed, 0u);
    EXPECT_EQ(Probes(), before);
    EXPECT_EQ(Purge("by_k"), 0u) << "the one-leaf walk left nothing a whole run would take";
}

TEST_F(IndexPurgeTest, AnOldSnapshotStillFindsItsRowThroughTheOldKey) {
    Ok("CREATE TABLE t (id int64, k int64, c int64) BTREE");
    Ok("CREATE INDEX by_k ON t (k) COVERING (c)");
    // Row 3's key sorts first, so the old key's entry is not the leaf's
    // first, which no purge removes (DD-Q3 (a)).
    Ok("INSERT INTO t VALUES (1, 10, 1), (2, 20, 2), (3, 1, 3)");
    Session reader;
    Ok("BEGIN ISOLATION LEVEL REPEATABLE READ", &reader);
    const std::string seen = Run("SELECT id FROM t WHERE k = 10", &reader);
    Ok("UPDATE t SET k = 11 WHERE id = 1");
    EXPECT_EQ(Purge("by_k"), 0u) << "the reader's view still reaches the old version";
    EXPECT_EQ(Run("SELECT id FROM t WHERE k = 10", &reader), seen);
    Ok("COMMIT", &reader);
    EXPECT_EQ(Purge("by_k"), 1u) << "the old key's entry goes once the reader ends";
    EXPECT_EQ(Run("SELECT id FROM t WHERE k = 11"), Run("SELECT id FROM t WHERE id = 1"));
}

TEST_F(IndexPurgeTest, AnUndecidedWritersRowIsSkippedAndItsEntriesStay) {
    // §1.2 F's hazard, forced: an UPDATE back to an earlier key appends
    // nothing - `IndexInsert` finds the old entry byte-identical - so the
    // row relies on the entry a purge judging the committed state would
    // call dead. The writer's `U` refuses the purge's try (DD-Q2 (a)).
    Ok("CREATE TABLE t (id int64, k int64) BTREE");
    Ok("CREATE INDEX by_k ON t (k)");
    Ok("INSERT INTO t VALUES (1, 10)");
    Ok("UPDATE t SET k = 11 WHERE id = 1");  // the entry for 10 is dead now
    Session w;
    Ok("BEGIN", &w);
    Ok("UPDATE t SET k = 10 WHERE id = 1", &w);  // back: relies on the old entry
    const DispatchOutcome out = Do("PURGE INDEX by_k");
    ASSERT_TRUE(out.status.ok()) << out.response;
    EXPECT_NE(out.response.find("skipped_rows=1"), std::string::npos) << out.response;
    Ok("COMMIT", &w);
    EXPECT_NE(Run("SELECT id FROM t WHERE k = 10").find("1"), std::string::npos);
    Purge("by_k");
    EXPECT_NE(Run("SELECT id FROM t WHERE k = 10").find("1"), std::string::npos)
        << "the entry is owed again once the writer commits";
    EXPECT_EQ(Run("SELECT id FROM t WHERE k = 11"), Run("SELECT id FROM t WHERE id = 0"));
}

TEST_F(IndexPurgeTest, ARolledBackAPurgedAndADeletedRowsEntriesGo) {
    // DD-Q1 (b): a delete-marked row whose deleter every reader sees is
    // reached through no version, so its entries go before a row `PURGE`.
    Ok("CREATE TABLE t (id int64, k int64) BTREE");
    Ok("CREATE INDEX by_k ON t (k)");
    Ok("INSERT INTO t VALUES (1, 5), (2, 6), (3, 7), (4, 8)");
    Session s;
    Ok("BEGIN", &s);
    Ok("INSERT INTO t VALUES (10, 1), (11, 2)", &s);
    Ok("ROLLBACK", &s);
    Ok("DELETE FROM t WHERE id >= 3");
    Ok("PURGE FROM t WHERE id = 4");
    EXPECT_EQ(Count("by_k"), 6u);
    // One leaf: its first entry, (1, 10), is the rolled-back insert's and stays.
    EXPECT_EQ(Purge("by_k"), 3u);
    EXPECT_EQ(Count("by_k"), 3u);
    EXPECT_EQ(Run("SELECT id FROM t WHERE k = 7"), Run("SELECT id FROM t WHERE id = 3"));
}

TEST_F(IndexPurgeTest, AUniqueIndexRefusesTheSameDuplicatesAfterAPurge) {
    Ok("CREATE TABLE t (id int64, code int64) BTREE");
    Ok("CREATE UNIQUE INDEX by_code ON t (code)");
    Ok("INSERT INTO t VALUES (1, 7), (2, 8), (3, 9)");
    Ok("UPDATE t SET code = 70 WHERE id = 1");
    Ok("DELETE FROM t WHERE id = 3");
    Purge("by_code");
    EXPECT_EQ(Do("INSERT INTO t VALUES (4, 8)").status.code(), StatusCode::kAlreadyExists);
    EXPECT_EQ(Do("INSERT INTO t VALUES (5, 70)").status.code(), StatusCode::kAlreadyExists);
    EXPECT_TRUE(Do("INSERT INTO t VALUES (6, 7)").status.ok()) << "7 moved off";
    EXPECT_TRUE(Do("INSERT INTO t VALUES (7, 9)").status.ok()) << "9's row was deleted";
}

TEST_F(IndexPurgeTest, ASpilledCoveredValueAssignedAgainKeepsItsEntry) {
    // DD-S0's finding (`index_purge_premise_test.cpp`): the version's cell
    // names a new spill, the write path appended nothing, and the entry the
    // row has is the only one a probe can reach it by.
    Ok("CREATE TABLE t (id int64, k int64, note varchar(16)) BTREE");
    Ok("CREATE INDEX by_k ON t (k) COVERING (note)");
    const std::string long_value(40, 'n');
    for (int i = 1; i <= 20; ++i) {
        Ok("INSERT INTO t VALUES (" + std::to_string(i) + ", 1, '" + long_value + "')");
    }
    Ok("UPDATE t SET note = '" + long_value + "' WHERE id > 1");
    const std::string before = Run("SELECT id FROM t WHERE k = 1 ORDER BY id");
    EXPECT_EQ(Purge("by_k"), 0u);
    EXPECT_EQ(Run("SELECT id FROM t WHERE k = 1 ORDER BY id"), before);
}

TEST_F(IndexPurgeTest, ItIsRefusedInsideATransactionAndForAnIndexThatDoesNotExist) {
    Ok("CREATE TABLE t (id int64, k int64) BTREE");
    Ok("CREATE INDEX by_k ON t (k)");
    Session s;
    Ok("BEGIN", &s);
    EXPECT_EQ(Do("PURGE INDEX by_k", &s).status.code(), StatusCode::kNotImplemented);
    Ok("COMMIT", &s);
    const DispatchOutcome missing = Do("PURGE INDEX nope");
    EXPECT_EQ(missing.status.code(), StatusCode::kNotFound);
    EXPECT_NE(missing.response.find("byte 12"), std::string::npos) << missing.response;
}

TEST(IndexPurgeNoLocksTest, ADispatcherWithNoLockTableIsRefusedNotAnsweredZero) {
    storage::InMemoryPageStore store{kFirstUserPageId};
    auto boot = bootstrap::BootstrapDatabase(store, 1000);
    ASSERT_TRUE(boot.ok());
    txn::TrxIdSequence ids(boot.value().superblock);
    txn::UndoLog undo(store, nullptr);
    txn::TransactionManager mgr(ids, undo, store, nullptr);
    CommandDispatcher d(boot.value().superblock, boot.value().catalog, store, nullptr, nullptr,
                        nullptr, wal::DurabilityClass::kRelaxed, exec::Budget(), nullptr, false,
                        false, nullptr, &mgr);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, k int64) BTREE").response.rfind("ERR", 0),
              std::string::npos);
    ASSERT_EQ(d.Dispatch("CREATE INDEX by_k ON t (k)").response.rfind("ERR", 0),
              std::string::npos);
    EXPECT_EQ(d.Dispatch("PURGE INDEX by_k").status.code(), StatusCode::kUnsupported);
}

// DD-R6: each rewritten leaf is one `FULL_PAGE_IMAGE` at `kNoTxnId`,
// appended under the hold that changed it. A crash after the purge's records
// are durable mounts to the purged tree, replayed with no loser; a crash
// before leaves every entry, which is today's state; either way every probe
// answers as before.
TEST(IndexPurgeCrashTest, APurgedTreeAndAnUnloggedPurgeBothMountToTheSameAnswers) {
    for (const bool synced : {true, false}) {
        sim::SimInstanceOptions options;
        options.lock_table = true;
        auto created = sim::SimInstance::Create(options);
        ASSERT_TRUE(created.ok()) << created.status().message();
        std::unique_ptr<sim::SimInstance> db = std::move(created.value());
        const auto ok = [&](const std::string& sql) {
            const std::string out = db->Execute(sql);
            ASSERT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
        };
        ok("CREATE TABLE t (id int64, k int64, c int64) BTREE");
        ok("CREATE INDEX by_k ON t (k) COVERING (c)");
        for (int i = 1; i <= 600; ++i) {
            ok("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i % 9) +
               ", " + std::to_string(i) + ")");
        }
        ok("UPDATE t SET k = k + 50 WHERE id <= 300");
        ok("DELETE FROM t WHERE id > 550");
        ASSERT_EQ(db->Execute("SYNC"), "OK synced");
        const std::string probe = "SELECT id, k FROM t WHERE k BETWEEN 0 AND 60 ORDER BY id";
        const std::string before = db->Execute(probe);
        if (!synced) ok("SET DURABILITY RELAXED");
        const std::string purged = db->Execute("PURGE INDEX by_k");
        ASSERT_EQ(purged.rfind("PURGED INDEX name=by_k", 0), 0u) << purged;
        ASSERT_EQ(purged.find("entries=0 "), std::string::npos) << purged;
        if (synced) ASSERT_EQ(db->Execute("SYNC"), "OK synced");
        const std::string entries_after =
            testing_view::ReadViewText(db->Execute("SELECT name, entries FROM sys.indexes"))
                .Find("name", "by_k", "entries")
                .value_or("?");

        db->Crash();
        ASSERT_TRUE(db->Reboot().ok());
        EXPECT_EQ(db->Execute(probe), before) << (synced ? "synced" : "unsynced");
        const std::string entries =
            testing_view::ReadViewText(db->Execute("SELECT name, entries FROM sys.indexes"))
                .Find("name", "by_k", "entries")
                .value_or("?");
        if (synced) EXPECT_EQ(entries, entries_after) << "the images were replayed";
        // A purge after the mount converges on the same tree either way.
        db->Execute("PURGE INDEX by_k");
        EXPECT_EQ(testing_view::ReadViewText(db->Execute("SELECT name, entries FROM sys.indexes"))
                      .Find("name", "by_k", "entries")
                      .value_or("?"),
                  entries_after);
        EXPECT_EQ(db->Execute(probe), before);
    }
}

}  // namespace
}  // namespace kds::server
