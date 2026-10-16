#include <cstdlib>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/anchor_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "view_text.hpp"

// BY-S3 (`instructions/v3.2.0/workorder-by-reindex-drop-index-reclaim.md`):
// `REINDEX INDEX <name>` drops the index and builds it again under a new oid
// and the same name, in one autocommit statement (BY-R6, BY-Q4 (a)), with
// the name rule that lets the new row take the old one's (BY-R8). The old
// tree's reclaim is BY-S2's and its cells are `index_reclaim_job_test.cpp`'s.

namespace kds::exec {
namespace {

class ReindexTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, &*mgr_);
    }

    server::DispatchOutcome Do(const std::string& sql, server::Session* session = nullptr) {
        return dispatcher_->Dispatch(sql, session);
    }
    std::string Run(const std::string& sql, server::Session* session = nullptr) {
        return Do(sql, session).response;
    }
    void Ok(const std::string& sql, server::Session* session = nullptr) {
        const std::string out = Run(sql, session);
        ASSERT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    }

    std::string IndexField(const std::string& name, const std::string& column) {
        const std::string shown = Run("SELECT name, " + column + " FROM sys.indexes");
        const auto value = testing_view::ReadViewText(shown).Find("name", name, column);
        EXPECT_TRUE(value.has_value()) << shown;
        return value.value_or("<absent>");
    }

    // `sys.*` views do not aggregate (AG12), so the rows are counted here.
    std::size_t IndexCount() {
        return testing_view::ReadViewText(Run("SELECT name FROM sys.indexes")).rows.size();
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<server::CommandDispatcher> dispatcher_;
};

TEST_F(ReindexTest, ARebuildDropsTheEntriesOfRowsThatLeftTheRelationUnderANewOidAndName) {
    // What a rebuild reclaims: the entries of versions no longer in the
    // relation - a rolled-back insert's, a purged row's - which IX2 keeps for
    // good. An `UPDATE`'s superseded value is shed too once no reader
    // reaches it (`ABuildOverRecycledUndo...` below).
    Ok("CREATE TABLE t (id int64, owner int64, note int64) BTREE");
    Ok("CREATE INDEX by_owner ON t (owner) COVERING (note)");
    for (int i = 1; i <= 20; ++i) Ok("INSERT INTO t VALUES (" + std::to_string(i) + ", 0)");
    server::Session s;
    Ok("BEGIN", &s);
    for (int i = 100; i < 110; ++i) Ok("INSERT INTO t VALUES (" + std::to_string(i) + ", 0)", &s);
    Ok("ROLLBACK", &s);
    Ok("DELETE FROM t WHERE id <= 5");
    Ok("PURGE FROM t WHERE id <= 5");
    ASSERT_EQ(IndexField("by_owner", "entries"), "30") << "20 inserted, 10 rolled back";
    const std::string old_oid = IndexField("by_owner", "index_oid");
    const std::string before = Run("SELECT id, owner, note FROM t WHERE owner = 7");
    const std::string ranged = Run("SELECT id FROM t WHERE owner BETWEEN 3 AND 12");

    const server::DispatchOutcome out = Do("REINDEX INDEX by_owner");
    ASSERT_TRUE(out.status.ok()) << out.response;
    EXPECT_EQ(out.response.rfind("REINDEXED INDEX name=by_owner", 0), 0u) << out.response;

    EXPECT_EQ(IndexCount(), 1u) << "one index by the name, not two";
    EXPECT_NE(IndexField("by_owner", "index_oid"), old_oid) << "BY-Q2 (a): a new oid";
    EXPECT_EQ(IndexField("by_owner", "entries"), "15") << "one entry per row still there";
    EXPECT_EQ(IndexField("by_owner", "covering"), "note");
    EXPECT_EQ(Run("SELECT id, owner, note FROM t WHERE owner = 7"), before);
    EXPECT_EQ(Run("SELECT id FROM t WHERE owner BETWEEN 3 AND 12"), ranged);
    Ok("INSERT INTO t VALUES (99, 0)");
    EXPECT_NE(Run("SELECT id FROM t WHERE owner = 99").find("\\n"), std::string::npos)
        << "the new tree is maintained";
}

TEST_F(ReindexTest, AUniqueIndexIsRebuiltUniqueAndStillRefusesADuplicate) {
    Ok("CREATE TABLE t (id int64, code int64) BTREE");
    Ok("CREATE UNIQUE INDEX by_code ON t (code)");
    Ok("INSERT INTO t VALUES (7), (8)");
    Ok("UPDATE t SET code = 9 WHERE id = 2");
    Ok("REINDEX INDEX by_code");
    EXPECT_EQ(IndexField("by_code", "unique"), "1");
    EXPECT_EQ(Do("INSERT INTO t VALUES (7)").status.code(), StatusCode::kAlreadyExists);
    EXPECT_EQ(Do("INSERT INTO t VALUES (8)").status.code(), StatusCode::kOk) << "8 moved off";
}

TEST_F(ReindexTest, REINDEXInsideATransactionIsRefusedAndTheTransactionGoesOn) {
    // BY-Q4 (a): autocommit only, refused before a scope opens, so the
    // transaction is not poisoned.
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX by_a ON t (a)");
    server::Session s;
    Ok("BEGIN", &s);
    const server::DispatchOutcome out = Do("REINDEX INDEX by_a", &s);
    EXPECT_EQ(out.status.code(), StatusCode::kNotImplemented) << out.response;
    EXPECT_NE(out.response.find("byte 0"), std::string::npos) << out.response;
    Ok("INSERT INTO t VALUES (1)", &s);
    Ok("COMMIT", &s);
}

TEST_F(ReindexTest, AnUnknownIndexOrAMissingWordIsRefusedAndChangesNothing) {
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX by_a ON t (a)");
    const std::string oid = IndexField("by_a", "index_oid");
    const server::DispatchOutcome unknown = Do("REINDEX INDEX nope");
    EXPECT_EQ(unknown.status.code(), StatusCode::kNotFound) << unknown.response;
    EXPECT_NE(unknown.response.find("byte 14"), std::string::npos) << unknown.response;
    const server::DispatchOutcome table = Do("REINDEX TABLE t");
    EXPECT_EQ(table.status.code(), StatusCode::kInvalidArgument) << table.response;
    EXPECT_EQ(IndexField("by_a", "index_oid"), oid);
}

TEST_F(ReindexTest, ADropAndACreateOfOneNameInsideOneTransactionIsAdmitted) {
    // BY-R8's stated scope: the transaction's own mark frees the name for it.
    Ok("CREATE TABLE t (id int64, a int64, b int64) BTREE");
    Ok("CREATE INDEX x ON t (a)");
    server::Session s;
    Ok("BEGIN", &s);
    Ok("DROP INDEX x", &s);
    Ok("CREATE INDEX x ON t (b)", &s);
    Ok("COMMIT", &s);
    EXPECT_EQ(IndexCount(), 1u);
    EXPECT_EQ(IndexField("x", "keys"), "b");

    // Rolled back, the old index stands and the new one is gone.
    Ok("BEGIN", &s);
    Ok("DROP INDEX x", &s);
    Ok("CREATE INDEX x ON t (a)", &s);
    Ok("ROLLBACK", &s);
    EXPECT_EQ(IndexCount(), 1u);
    EXPECT_EQ(IndexField("x", "keys"), "b");
}

TEST_F(ReindexTest, AnotherTransactionsOpenDropStillHoldsTheName) {
    // BY-R8 frees a name for its own transaction only.
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX x ON t (a)");
    server::Session s;
    Ok("BEGIN", &s);
    Ok("DROP INDEX x", &s);
    EXPECT_NE(Run("CREATE INDEX x ON t (a)").rfind("ERR", 0), std::string::npos);
    Ok("ROLLBACK", &s);
}

TEST_F(ReindexTest, AtTheSlotCapARebuildIsRefusedAndTheOldIndexStands) {
    // A known limit, stated in `index.md` §12: the new tree needs a slot of
    // its own while the old one's is still held - the old slot goes only
    // when the reclaimer frees its tree (BY-R5) - so a `REINDEX` on a
    // relation whose anchor is full is refused `RESOURCE_EXHAUSTED` before
    // any page is built, and the refusal changes nothing (BY-R7). This
    // fixture runs no reclaimer, so every dropped index's slot stays held.
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX x ON t (a)");
    for (std::size_t i = 1; i < storage::kAnchorMaxIndexEntries; ++i) {
        Ok("CREATE INDEX f" + std::to_string(i) + " ON t (a)");
        Ok("DROP INDEX f" + std::to_string(i));
    }
    Ok("INSERT INTO t VALUES (7), (8)");
    const std::string oid = IndexField("x", "index_oid");
    const std::size_t pages = store_.page_count();

    const server::DispatchOutcome out = Do("REINDEX INDEX x");
    EXPECT_EQ(out.status.code(), StatusCode::kResourceExhausted) << out.response;
    EXPECT_EQ(IndexCount(), 1u);
    EXPECT_EQ(IndexField("x", "index_oid"), oid);
    EXPECT_EQ(IndexField("x", "entries"), "2");
    EXPECT_EQ(store_.page_count(), pages) << "a refused rebuild built a tree";
    EXPECT_EQ(Run("SELECT id FROM t WHERE a = 8"), "id\\n2");
}

TEST_F(ReindexTest, ABuildOverRecycledUndoStopsWhereNoReaderReachesAndARebuildShedsUpdates) {
    // The backfill once followed every undo chain to its end, where a reader
    // stops at the first version it can see - and the purge recycles a
    // record once its writer is resolved for every reader (`undo_log.hpp`),
    // so a long-settled chain points into pages since reused: the build was
    // refused `Corruption` or indexed another row's history. 5,000
    // autocommit updates of one row grow the undo log past several pages,
    // every writer settled. The build stops at the current version; and an
    // index maintained through further updates - one entry per value, IX2 -
    // sheds them all on `REINDEX`.
    //
    // **Mutation**: `unreachable_below` answering false - the build is
    // refused (`has type 0`) or counts 5,001 entries.
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("INSERT INTO t VALUES (0)");
    for (int i = 1; i <= 5000; ++i) Ok("UPDATE t SET a = " + std::to_string(i) + " WHERE id = 1");
    const server::DispatchOutcome built = Do("CREATE INDEX by_a ON t (a)");
    ASSERT_TRUE(built.status.ok()) << built.response;
    EXPECT_EQ(IndexField("by_a", "entries"), "1") << "no reader reaches an older version";
    EXPECT_EQ(Run("SELECT id FROM t WHERE a = 5000"), "id\\n1");

    for (int i = 5001; i <= 5100; ++i) {
        Ok("UPDATE t SET a = " + std::to_string(i) + " WHERE id = 1");
    }
    ASSERT_EQ(IndexField("by_a", "entries"), "101");
    const server::DispatchOutcome rebuilt = Do("REINDEX INDEX by_a");
    ASSERT_TRUE(rebuilt.status.ok()) << rebuilt.response;
    EXPECT_EQ(IndexField("by_a", "entries"), "1");
    EXPECT_EQ(Run("SELECT id FROM t WHERE a = 5100"), "id\\n1");
    EXPECT_EQ(Run("SELECT id FROM t WHERE a = 5050"), "id");
}

}  // namespace
}  // namespace kds::exec
