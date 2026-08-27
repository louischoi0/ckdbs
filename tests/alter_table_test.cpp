#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"

// ALT04 - the oid-identity proof (docs/spec/alter.md AL2).
//
// The claim the feature rests on: every cross-object reference is by oid,
// so a rename dangles nothing. One scenario per reference class, each
// asserting *behavior* across the rename with no re-declaration - an FK
// that still enforces, an index that still serves, a Cabin that still
// answers - plus AL4's RESTRICT and AL7's system-relation refusal.

namespace kds::server {
namespace {

class AlterTableTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
        cabins_.emplace();
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, &*cabins_, &*mgr_);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    std::string Run(Session& s, const std::string& sql) {
        return dispatcher_->Dispatch(sql, &s).response;
    }

    void Ok(const std::string& sql) {
        const std::string out = Run(sql);
        EXPECT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::CabinStore> cabins_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

TEST_F(AlterTableTest, RenameTableEndToEnd) {
    Ok("CREATE TABLE t (id int64, v varchar)");
    Ok("INSERT INTO t VALUES ('one')");
    Ok("INSERT INTO t VALUES ('two')");

    EXPECT_EQ(Run("ALTER TABLE t RENAME TO u"), "RENAMED TABLE t TO u");

    // The new name resolves; the old one is gone - no alias, no history.
    EXPECT_EQ(Run("SELECT v FROM u"), "v\\none\\ntwo");
    EXPECT_EQ(Run("SELECT v FROM t").rfind("ERR", 0), 0u);
    EXPECT_EQ(Run("INSERT INTO u VALUES ('three')").substr(0, 8), "INSERTED");
}

TEST_F(AlterTableTest, RenameColumnEndToEnd) {
    Ok("CREATE TABLE t (id int64, v varchar)");
    Ok("INSERT INTO t VALUES ('one')");

    EXPECT_EQ(Run("ALTER TABLE t RENAME COLUMN v TO w"), "RENAMED COLUMN t.v TO w");
    EXPECT_EQ(Run("SELECT w FROM t"), "w\\none");
    EXPECT_EQ(Run("SELECT w FROM t WHERE w = 'one'"), "w\\none");
    EXPECT_EQ(Run("SELECT v FROM t").rfind("ERR", 0), 0u);
}

// Identity is the Keystone word and position 0, not the spelling
// (invariant 11, AL8) - so the pk column renames like any other, and a
// pk lookup through the new name is still a lookup.
TEST_F(AlterTableTest, ThePkColumnMayBeRenamed) {
    Ok("CREATE TABLE t (id int64, v varchar) BTREE");
    Ok("INSERT INTO t VALUES ('one')");

    EXPECT_EQ(Run("ALTER TABLE t RENAME COLUMN id TO key"), "RENAMED COLUMN t.id TO key");
    EXPECT_EQ(Run("SELECT v FROM t WHERE key = 1"), "v\\none");
    EXPECT_EQ(Run("INSERT INTO t VALUES ('two')").substr(0, 8), "INSERTED");
}

// AL2 for foreign keys: sys.fkeys stores oids, so enforcement survives a
// parent rename with no re-declaration.
TEST_F(AlterTableTest, AForeignKeySurvivesAParentRename) {
    Ok("CREATE TABLE p (id int64, v int64) BTREE");
    Ok("CREATE TABLE c (id int64, pid int64 REFERENCES p)");
    Ok("INSERT INTO p VALUES (10)");

    EXPECT_EQ(Run("ALTER TABLE p RENAME TO p2"), "RENAMED TABLE p TO p2");

    EXPECT_EQ(Run("INSERT INTO c VALUES (1)").substr(0, 8), "INSERTED");
    EXPECT_EQ(Run("INSERT INTO c VALUES (99)").rfind("ERR", 0), 0u)
        << "a dangling reference must still be refused after the rename";
    EXPECT_EQ(Run("DELETE FROM p2 WHERE id = 1").rfind("ERR", 0), 0u)
        << "the reverse check must still see the child after the rename";
}

// AL2 for indexes: sys.indexes stores the relation's oid.
TEST_F(AlterTableTest, AnIndexSurvivesARename) {
    Ok("CREATE TABLE t (id int64, owner int64) BTREE");
    Ok("CREATE INDEX by_owner ON t (owner)");
    for (int i = 0; i < 8; ++i) Ok("INSERT INTO t VALUES (" + std::to_string(i % 2) + ")");

    EXPECT_EQ(Run("ALTER TABLE t RENAME TO u"), "RENAMED TABLE t TO u");

    const std::string plan = Run("ANALYZE SELECT id FROM u WHERE owner = 1");
    EXPECT_NE(plan.find("index_scanned="), std::string::npos)
        << "the index stopped serving after the rename: " << plan;
    EXPECT_EQ(Run("SELECT id FROM u WHERE owner = 1"), "id\\n2\\n4\\n6\\n8");
}

// AL2 for Cabins: sys.cabins stores the oid, and the in-memory sets key
// on the cabin id.
TEST_F(AlterTableTest, ACabinSurvivesARename) {
    Ok("CREATE TABLE t (id int64, sym varchar)");
    Ok("CREATE CABIN ON t(sym)");
    Ok("INSERT INTO t VALUES ('aaa')");
    Ok("INSERT INTO t VALUES ('bbb')");
    Ok("INSERT INTO t VALUES ('aaa')");

    // Declared cabin: one completed walk observes.
    EXPECT_EQ(Run("SELECT id FROM t WHERE sym = 'aaa'"), "id\\n1\\n3");

    EXPECT_EQ(Run("ALTER TABLE t RENAME TO u"), "RENAMED TABLE t TO u");

    const std::string served = Run("ANALYZE SELECT id FROM u WHERE sym = 'aaa'");
    EXPECT_NE(served.find("cabin_hits=1"), std::string::npos)
        << "the observed set stopped serving after the rename: " << served;
    EXPECT_EQ(Run("SELECT id FROM u WHERE sym = 'aaa'"), "id\\n1\\n3");
}

// AL4: an assertion's stored canon is the declaration's text, so both
// rename forms are refused while one exists - and proceed once it is
// dropped. AssertionsOnRelation()'s first live call site.
TEST_F(AlterTableTest, AnAssertionRestrictsBothRenameForms) {
    Ok("CREATE TABLE t (id int64, grp int64, amt int64)");
    Ok("CREATE ASSERTION cap ON t GROUP BY (grp) CHECK COUNT(*) <= 5");

    const std::string table = Run("ALTER TABLE t RENAME TO u");
    EXPECT_EQ(table.rfind("ERR", 0), 0u);
    EXPECT_NE(table.find("cap"), std::string::npos) << table;

    const std::string column = Run("ALTER TABLE t RENAME COLUMN amt TO amount");
    EXPECT_EQ(column.rfind("ERR", 0), 0u);
    EXPECT_NE(column.find("cap"), std::string::npos) << column;

    Ok("DROP ASSERTION cap");
    EXPECT_EQ(Run("ALTER TABLE t RENAME TO u"), "RENAMED TABLE t TO u");
}

TEST_F(AlterTableTest, CollisionsAndAbsencesRefuse) {
    Ok("CREATE TABLE a (id int64, x int64)");
    Ok("CREATE TABLE b (id int64, x int64)");

    EXPECT_EQ(Run("ALTER TABLE a RENAME TO b").rfind("ERR", 0), 0u);
    EXPECT_EQ(Run("ALTER TABLE nope RENAME TO c").rfind("ERR", 0), 0u);
    EXPECT_EQ(Run("ALTER TABLE a RENAME COLUMN x TO id").rfind("ERR", 0), 0u);
    EXPECT_EQ(Run("ALTER TABLE a RENAME COLUMN nope TO y").rfind("ERR", 0), 0u);

    // Nothing above changed anything: both relations still answer.
    Ok("SELECT x FROM a");
    Ok("SELECT x FROM b");
}

// AL3: a declared pattern whose source text names the old name is
// *allowed to die* - it breaks nothing, answers nothing, and the new
// name's traffic registers fresh shapes on the ordinary path. This is
// invariant 8 doing its job: everything pattern-shaped is advisory, so a
// rename may orphan it freely where an assertion (AL4) may not be.
TEST_F(AlterTableTest, ADeclaredPatternDiesQuietlyWithTheOldName) {
    Ok("CREATE TABLE t (id int64, v varchar)");
    Ok("INSERT INTO t VALUES ('one')");
    Ok("CREATE PATTERN watch($k int64) OF SELECT v FROM t WHERE id = $k");

    EXPECT_EQ(Run("ALTER TABLE t RENAME TO u"), "RENAMED TABLE t TO u");

    // The orphaned declaration is still listed and still harmless.
    const std::string patterns = Run("SHOW PATTERNS");
    EXPECT_NE(patterns.rfind("ERR", 0), 0u) << patterns;
    EXPECT_NE(patterns.find("patterns=1"), std::string::npos) << patterns;

    // Old-name traffic fails resolution; new-name traffic answers - and
    // is a different shape, so the dead pattern is never consulted for it.
    EXPECT_EQ(Run("SELECT v FROM t WHERE id = 1").rfind("ERR", 0), 0u);
    EXPECT_EQ(Run("SELECT v FROM u WHERE id = 1"), "v\\none");

    // A fresh declaration against the new name is the specified recovery.
    Ok("CREATE PATTERN watch2($k int64) OF SELECT v FROM u WHERE id = $k");
}

// AL6: admitted inside an explicit transaction with CREATE TABLE's
// caveat - the rename is a catalog write, so ROLLBACK does not undo it.
// Pinned as behavior so the caveat cannot silently become a promise.
TEST_F(AlterTableTest, ARenameInsideATransactionIsNotRolledBack) {
    Ok("CREATE TABLE t (id int64, v varchar)");
    Ok("INSERT INTO t VALUES ('one')");

    Session session;
    ASSERT_EQ(Run(session, "BEGIN").substr(0, 5), "BEGIN");
    EXPECT_EQ(Run(session, "ALTER TABLE t RENAME TO u"), "RENAMED TABLE t TO u");
    EXPECT_EQ(Run(session, "ROLLBACK").rfind("ERR", 0), std::string::npos);

    // The rename survived the rollback: DDL is not transactional (AL6).
    EXPECT_EQ(Run("SELECT v FROM u"), "v\\none");
    EXPECT_EQ(Run("SELECT v FROM t").rfind("ERR", 0), 0u);
}

// AL7: the catalog's names are load-bearing for bootstrap.
TEST_F(AlterTableTest, ASystemRelationIsRefused) {
    const std::string out = Run("ALTER TABLE tables RENAME TO stuff");
    EXPECT_EQ(out.rfind("ERR", 0), 0u);
    EXPECT_NE(out.find("system relation"), std::string::npos) << out;
}

// AL5: the bump is global - a statement compiled after the rename sees
// the new catalog, and DESCRIBE-class surfaces answer with the new name.
TEST_F(AlterTableTest, SysViewsAnswerWithTheNewName) {
    Ok("CREATE TABLE t (id int64, v varchar)");
    EXPECT_EQ(Run("ALTER TABLE t RENAME TO u"), "RENAMED TABLE t TO u");

    const std::string tables = Run("SELECT name FROM sys.tables");
    EXPECT_NE(tables.find("u"), std::string::npos) << tables;
    EXPECT_EQ(tables.find("\\nt\\n"), std::string::npos) << tables;
}

}  // namespace
}  // namespace kds::server
