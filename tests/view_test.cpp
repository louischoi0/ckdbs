#include <cstdlib>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"

// DP - VIEW (instructions/v3.2.0/workorder-dp-view.md).
//
// DP-S1, the object: a view is a name in its namespace's relation name space
// (NS6) with its text, columns and dependencies in `sys.views`; created and
// dropped transactionally (DP-Q4); every change to what it reads RESTRICTed
// (DP-Q16); no write names one (DP-Q9).

namespace kds::server {
namespace {

class ViewTest : public ::testing::Test {
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
    // A refusal, and that its text says `needle`.
    void Refused(const std::string& sql, const std::string& needle) {
        const std::string out = Run(sql);
        EXPECT_EQ(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
        EXPECT_NE(out.find(needle), std::string::npos) << sql << " -> " << out;
    }
    std::uint64_t OidOf(const std::string& reply) {
        const auto at = reply.find("oid=");
        EXPECT_NE(at, std::string::npos) << reply;
        return std::strtoull(reply.c_str() + at + 4, nullptr, 10);
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::CabinStore> cabins_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

TEST_F(ViewTest, ACreatedViewIsListedWithItsTextColumnsAndDependency) {
    Ok("CREATE TABLE t (id int64, a int64, b int64)");
    const std::string created = Run("CREATE VIEW v AS SELECT id, a FROM t WHERE b = 1");
    ASSERT_EQ(created.rfind("CREATED VIEW v oid=", 0), 0u) << created;
    EXPECT_EQ(created.find("WARNING"), std::string::npos) << "an absorbable body is not warned";
    const std::uint64_t t_oid =
        std::strtoull(Run("SELECT oid FROM sys.tables WHERE name = 't'").substr(5).c_str(),
                      nullptr, 10);

    EXPECT_EQ(Run("SELECT name, absorbable, columns, reads, source_text FROM sys.views"),
              "name,absorbable,columns,reads,source_text\\nv,1,id, a," +
                  std::to_string(t_oid) + ",SELECT id, a FROM t WHERE b = 1");
    EXPECT_NE(Run("SHOW VIEWS").find("SELECT id, a FROM t WHERE b = 1"), std::string::npos);
}

TEST_F(ViewTest, AViewAndATableShareOneNameSpace) {
    Ok("CREATE TABLE t (id int64)");
    Refused("CREATE VIEW t AS SELECT id FROM t", "already exists");
    Ok("CREATE VIEW v AS SELECT id FROM t");
    Refused("CREATE TABLE v (id int64)", "a view named 'v' already exists");
    Refused("CREATE VIEW v AS SELECT id FROM t", "a view named 'v' already exists");
    Refused("ALTER TABLE t RENAME TO v", "view");
}

TEST_F(ViewTest, ADropFreesTheNameAndNeverReissuesTheOid) {
    Ok("CREATE TABLE t (id int64)");
    const std::uint64_t first = OidOf(Run("CREATE VIEW v AS SELECT id FROM t"));
    EXPECT_EQ(Run("DROP VIEW v"), "DROPPED VIEW v oid=" + std::to_string(first));
    Refused("DROP VIEW v", "no view named 'v'");
    EXPECT_EQ(Run("SELECT name FROM sys.views"), "name");
    const std::uint64_t second = OidOf(Run("CREATE VIEW v AS SELECT id FROM t"));
    EXPECT_GT(second, first) << "DP-R3: a new body is a new oid, never the tombstone's";
}

TEST_F(ViewTest, WhatAViewReadsIsRestricted) {
    Ok("CREATE TABLE t (id int64, a int64 NULL, b int64 NULL, c int64 NULL) SPARE 1");
    Ok("CREATE VIEW v AS SELECT id, a FROM t WHERE b = 1");

    Refused("DROP TABLE t", "view 'v' reads it");
    Refused("ALTER TABLE t RENAME TO u", "view 'v' reads it");
    Refused("ALTER TABLE t RENAME COLUMN a TO z", "view 'v' reads it");
    Refused("ALTER TABLE t RENAME COLUMN b TO z", "view 'v' reads it");
    Refused("ALTER TABLE t DROP COLUMN b", "view 'v' reads it");
    // DP-Q16: a column the body's text names, renamed to or added, could
    // capture a name the body resolves today.
    Refused("ALTER TABLE t RENAME COLUMN c TO b", "view 'v' reads it");
    Refused("ALTER TABLE t ADD COLUMN t int64 NULL", "view 'v' reads it");

    // A column the body names nowhere is the relation's own business.
    Ok("ALTER TABLE t RENAME COLUMN c TO d");
    Ok("ALTER TABLE t DROP COLUMN d");

    Ok("DROP VIEW v");
    Ok("DROP TABLE t");
}

TEST_F(ViewTest, AStarBodyIsTheColumnsLiveAtItsCreate) {
    Ok("CREATE TABLE t (id int64, a int64) SPARE 1");
    Ok("CREATE VIEW v AS SELECT * FROM t");
    // DP-Q11: stored so - the text carries the columns in the `*`'s place.
    EXPECT_EQ(Run("SELECT source_text FROM sys.views"), "source_text\\nSELECT id, a FROM t");
    Ok("ALTER TABLE t ADD COLUMN z int64 NULL");
    EXPECT_EQ(Run("SELECT columns FROM sys.views"), "columns\\nid, a") << "DP-Q11";
    Refused("ALTER TABLE t DROP COLUMN a", "view 'v' reads it");
}

// A query-layer `*` is not expanded, so its width is its relation's: no
// column is added under it (DP-Q16).
TEST_F(ViewTest, AQueryLayerStarBodyRefusesAnAddedColumn) {
    Ok("CREATE TABLE t (id int64, a int64) SPARE 1");
    Ok("CREATE VIEW v AS SELECT DISTINCT * FROM t");
    Refused("ALTER TABLE t ADD COLUMN z int64 NULL", "view 'v' reads it");
}

// DP-Q6 (a), §6: the forms not built, refused by name at their word.
TEST_F(ViewTest, ReplaceAndMaterializedAreRefusedByName) {
    Ok("CREATE TABLE t (id int64)");
    Refused("CREATE OR REPLACE VIEW v AS SELECT id FROM t", "CREATE OR REPLACE is not supported");
    Refused("CREATE MATERIALIZED VIEW v AS SELECT id FROM t", "materialized view");
    EXPECT_NE(Run("CREATE OR REPLACE VIEW v AS SELECT id FROM t").find("NOT_IMPLEMENTED"),
              std::string::npos);
    EXPECT_NE(Run("CREATE OR REPLACE VIEW v AS SELECT id FROM t").find("byte 7"),
              std::string::npos);
}

// DP-Q11 with DP-Q15: a `*` body's columns are the view's by position, so a
// column list renames them and a later `ADD COLUMN` adds none to the view.
TEST_F(ViewTest, AStarBodyUnderAColumnListIsDescribedAfterAnAdd) {
    Ok("CREATE TABLE t (id int64, a int64) SPARE 1");
    Ok("CREATE VIEW v (x, y) AS SELECT * FROM t");
    Ok("ALTER TABLE t ADD COLUMN z int64 NULL");
    const std::string described = Run("DESCRIBE v");
    EXPECT_EQ(described.rfind("ERR", 0), std::string::npos) << described;
    EXPECT_NE(described.find(",x,"), std::string::npos) << described;
    EXPECT_NE(described.find(",y,"), std::string::npos) << described;
    EXPECT_EQ(described.find(",z,"), std::string::npos) << described;
}

// DP-Q16: a `*` the query layer answers reads every column as a native one
// does, so no column of its relation may go.
TEST_F(ViewTest, AStarBodyTheQueryLayerAnswersRestrictsEveryColumn) {
    Ok("CREATE TABLE t (id int64, a int64 NULL, b int64 NULL)");
    const std::string created = Run("CREATE VIEW v AS SELECT DISTINCT * FROM t");
    ASSERT_NE(created.find("WARNING"), std::string::npos) << created;
    Refused("ALTER TABLE t DROP COLUMN b", "view 'v' reads it");
    Refused("ALTER TABLE t RENAME COLUMN b TO c", "view 'v' reads it");
}

// NS6: a rename onto a view's name is a collision, as onto a table's.
TEST_F(ViewTest, ARenameOntoAViewsNameIsRefused) {
    Ok("CREATE TABLE t (id int64)");
    Ok("CREATE TABLE u (id int64)");
    Ok("CREATE VIEW v AS SELECT id FROM t");
    Refused("ALTER TABLE u RENAME TO v", "a view named 'v' already exists");
}

TEST_F(ViewTest, NoWriteNamesAView) {
    Ok("CREATE TABLE t (id int64, a int64)");
    Ok("CREATE VIEW v AS SELECT id, a FROM t");
    Refused("INSERT INTO v VALUES (1, 2)", "'v' is a view");
    Refused("UPDATE v SET a = 3", "'v' is a view");
    Refused("DELETE FROM v WHERE id = 1", "'v' is a view");
    Refused("CREATE INDEX ix ON v (a)", "'v' is a view");
    Refused("SELECT * FROM v", "'v' is a view");  // DP-S1: nothing reads one
    EXPECT_NE(Run("INSERT INTO v VALUES (1, 2)").find("NOT_IMPLEMENTED"), std::string::npos)
        << Run("INSERT INTO v VALUES (1, 2)");
}

TEST_F(ViewTest, ANonAbsorbableBodyIsWarnedAndSaidSo) {
    Ok("CREATE TABLE t (id int64, a int64)");
    const std::string created =
        Run("CREATE VIEW totals (a, n) AS SELECT a, COUNT(*) FROM t GROUP BY a");
    ASSERT_EQ(created.rfind("CREATED VIEW totals oid=", 0), 0u) << created;
    EXPECT_NE(created.find("WARNING: its body is not absorbable"), std::string::npos);
    EXPECT_EQ(Run("SELECT absorbable, columns FROM sys.views"), "absorbable,columns\\n0,a, n");
}

TEST_F(ViewTest, ColumnNamesAreTheViewsOwnAndUnique) {
    Ok("CREATE TABLE p (id int64, k int64)");
    Ok("CREATE TABLE c (id int64, k int64)");
    Refused("CREATE VIEW v (x) AS SELECT id, k FROM p", "names 1 columns and its body returns 2");
    Refused("CREATE VIEW v AS SELECT p.id, c.id FROM p JOIN c ON p.k = c.k",
            "two columns named 'id'");
    Ok("CREATE VIEW v (pid, cid) AS SELECT p.id, c.id FROM p JOIN c ON p.k = c.k");
    EXPECT_EQ(Run("SELECT columns FROM sys.views"), "columns\\npid, cid");
}

TEST_F(ViewTest, AWrongBodyIsRefusedAtItsCreate) {
    Ok("CREATE TABLE t (id int64)");
    Refused("CREATE VIEW v AS SELECT nope FROM t", "in the view's body");
    Refused("CREATE VIEW v AS SELECT id FROM missing", "in the view's body");
    Refused("CREATE VIEW v AS DELETE FROM t", "a view's body is a SELECT");
    // An unlexable tail is the body's refusal; the parse that drains the
    // statement to its end stops there too.
    Refused("CREATE VIEW v AS SELECT id FROM t WHERE id = 'x", "in the view's body");
    Refused("CREATE VIEW v AS SELECT id FROM t WHERE id = 1 #", "in the view's body");
    EXPECT_EQ(Run("SELECT name FROM sys.views"), "name");
}

TEST_F(ViewTest, DescribeListsAViewsColumnsTyped) {
    Ok("CREATE TABLE t (id int64, a varchar(32), b int64)");
    Ok("CREATE VIEW v (key, label) AS SELECT id, a FROM t");
    const std::string described = Run("DESCRIBE v");
    EXPECT_NE(described.find(",key,"), std::string::npos) << described;
    EXPECT_NE(described.find(",label,"), std::string::npos) << described;
    EXPECT_EQ(described.find(",b,"), std::string::npos) << described;
}

TEST_F(ViewTest, ACreateInsideATransactionIsRolledBack) {
    Ok("CREATE TABLE t (id int64)");
    Session session;
    ASSERT_EQ(Run(session, "BEGIN").substr(0, 5), "BEGIN");
    EXPECT_EQ(Run(session, "CREATE VIEW v AS SELECT id FROM t").rfind("CREATED VIEW", 0), 0u);
    Run(session, "ROLLBACK");
    EXPECT_EQ(Run("SELECT name FROM sys.views"), "name");
    Ok("DROP TABLE t");  // nothing reads it any more
}

TEST_F(ViewTest, ADropInsideATransactionIsRolledBack) {
    Ok("CREATE TABLE t (id int64)");
    Ok("CREATE VIEW v AS SELECT id FROM t");
    Session session;
    ASSERT_EQ(Run(session, "BEGIN").substr(0, 5), "BEGIN");
    EXPECT_EQ(Run(session, "DROP VIEW v").rfind("DROPPED VIEW", 0), 0u);
    Run(session, "ROLLBACK");
    EXPECT_EQ(Run("SELECT name, source_text FROM sys.views"),
              "name,source_text\\nv,SELECT id FROM t");
    Refused("DROP TABLE t", "view 'v' reads it");
}

TEST_F(ViewTest, ANamespaceHoldingAViewIsNotDropped) {
    Ok("CREATE NAMESPACE app");
    Ok("CREATE TABLE t (id int64)");
    Ok("CREATE VIEW app.v AS SELECT id FROM public.t");
    EXPECT_NE(Run("SELECT namespace, name FROM sys.views").find("app,v"), std::string::npos);
    Refused("DROP NAMESPACE app", "view 'v'");
}

}  // namespace
}  // namespace kds::server
