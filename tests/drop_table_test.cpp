#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"

// DT04 - DROP TABLE (docs/spec/drop-table.md DT1-DT6).
//
// The two claims that carry the feature: **the oid tombstone** (DT2 - a
// dropped oid is never reissued, pinned through the `oid=` in INSERTED
// replies, because a reissued oid could serve a dead table's row as a
// live answer through a stale advisory structure) and **RESTRICT in,
// dependents out** (DT3 - a referencing fk or an assertion blocks by
// name; the relation's own indexes, cabins and child-side fkeys drop
// with it).

namespace kds::server {
namespace {

class DropTableTest : public ::testing::Test {
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

    // The oid a fresh insert reports, which is how DT2 is observable.
    std::uint64_t OidOf(const std::string& insert_reply) {
        const auto at = insert_reply.find("oid=");
        EXPECT_NE(at, std::string::npos) << insert_reply;
        return std::strtoull(insert_reply.c_str() + at + 4, nullptr, 10);
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::CabinStore> cabins_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

TEST_F(DropTableTest, DropEndToEndAndTheNameFreesImmediately) {
    Ok("CREATE TABLE t (id int64, v varchar)");
    const std::uint64_t dead_oid = OidOf(Run("INSERT INTO t VALUES ('one')"));

    EXPECT_EQ(Run("DROP TABLE t"), "DROPPED TABLE t oid=" + std::to_string(dead_oid));
    EXPECT_EQ(Run("SELECT v FROM t").rfind("ERR", 0), 0u);
    EXPECT_EQ(Run("DROP TABLE t").rfind("ERR", 0), 0u) << "a second drop finds nothing";

    // The name frees at once; the new relation is empty and is a *new*
    // relation - DT2: its oid is fresh, never the tombstone's.
    Ok("CREATE TABLE t (id int64, v varchar)");
    EXPECT_EQ(Run("SELECT v FROM t"), "v");
    const std::uint64_t new_oid = OidOf(Run("INSERT INTO t VALUES ('two')"));
    EXPECT_GT(new_oid, dead_oid) << "a dropped oid was reissued";
    EXPECT_EQ(Run("SELECT v FROM t"), "v\\ntwo");
}

TEST_F(DropTableTest, AReferencingForeignKeyRestrictsByName) {
    Ok("CREATE TABLE p (id int64, v int64) BTREE");
    Ok("CREATE TABLE c (id int64, pid int64 REFERENCES p)");

    // Declared-level: the child is empty and still blocks.
    const std::string out = Run("DROP TABLE p");
    EXPECT_EQ(out.rfind("ERR", 0), 0u);
    EXPECT_NE(out.find("'c'"), std::string::npos) << out;

    // Dropping the child retires its fk rows with it; the parent frees.
    EXPECT_EQ(Run("DROP TABLE c").rfind("ERR", 0), std::string::npos);
    EXPECT_EQ(Run("DROP TABLE p").rfind("ERR", 0), std::string::npos);
    EXPECT_EQ(Run("SHOW FKEYS"), "fkeys=0");
}

TEST_F(DropTableTest, AnAssertionRestrictsByName) {
    Ok("CREATE TABLE t (id int64, grp int64)");
    Ok("CREATE ASSERTION cap ON t GROUP BY (grp) CHECK COUNT(*) <= 5");

    const std::string out = Run("DROP TABLE t");
    EXPECT_EQ(out.rfind("ERR", 0), 0u);
    EXPECT_NE(out.find("cap"), std::string::npos) << out;

    Ok("DROP ASSERTION cap");
    EXPECT_EQ(Run("DROP TABLE t").rfind("ERR", 0), std::string::npos);
}

TEST_F(DropTableTest, DependentsDropWithTheRelation) {
    Ok("CREATE TABLE t (id int64, owner int64, sym varchar) BTREE");
    Ok("CREATE INDEX by_owner ON t (owner)");
    Ok("CREATE CABIN ON t(sym)");
    Ok("INSERT INTO t VALUES (1, 'aaa')");
    // Observe the cabin so an in-memory set exists to forget.
    Ok("SELECT id FROM t WHERE sym = 'aaa'");

    EXPECT_EQ(Run("DROP TABLE t").rfind("ERR", 0), std::string::npos);
    EXPECT_EQ(Run("SHOW INDEXES").find("by_owner"), std::string::npos);
    EXPECT_EQ(Run("SHOW CABINS").find("t.sym"), std::string::npos);

    // A same-name successor starts clean: no index, no cabin, no rows.
    Ok("CREATE TABLE t (id int64, owner int64, sym varchar) BTREE");
    EXPECT_EQ(Run("SELECT id FROM t WHERE sym = 'aaa'"), "id");
}

TEST_F(DropTableTest, APatternGhostIsHarmless) {
    Ok("CREATE TABLE t (id int64, v varchar)");
    Ok("CREATE PATTERN watch($k int64) OF SELECT v FROM t WHERE id = $k");

    EXPECT_EQ(Run("DROP TABLE t").rfind("ERR", 0), std::string::npos);
    EXPECT_NE(Run("SHOW PATTERNS").rfind("ERR", 0), 0u);

    // The successor answers normally; the ghost declaration names a dead
    // oid and can never mis-attribute (DT4).
    Ok("CREATE TABLE t (id int64, v varchar)");
    Ok("INSERT INTO t VALUES ('x')");
    EXPECT_EQ(Run("SELECT v FROM t WHERE id = 1"), "v\\nx");
}

TEST_F(DropTableTest, ASystemRelationIsRefused) {
    const std::string out = Run("DROP TABLE tables");
    EXPECT_EQ(out.rfind("ERR", 0), 0u);
    EXPECT_NE(out.find("system relation"), std::string::npos) << out;
}

// **This test asserted the opposite until 2026-08-16**, and it was right
// to: `docs/spec/drop-table.md` DT5 called a drop "a catalog write like
// all DDL - ROLLBACK does not resurrect it". Transactional DDL's own DT5
// (`docs/inflight/in-progress/workplan-ddl-transactional.md`, a different numbering - cite the
// file) made that false on purpose: a drop inside a transaction
// delete-marks its dependent rows and records the tombstone retype's
// before-image on the trail, so `Abort` puts both back.
//
// Kept and inverted rather than deleted: the statement it pins is still
// the interesting one, and a reader coming from the drop-table spec needs
// to find the contradiction here rather than infer it.
TEST_F(DropTableTest, ADropInsideATransactionIsRolledBack) {
    Ok("CREATE TABLE t (id int64, v varchar)");
    Ok("INSERT INTO t VALUES ('kept')");

    Session session;
    ASSERT_EQ(Run(session, "BEGIN").substr(0, 5), "BEGIN");
    EXPECT_EQ(Run(session, "DROP TABLE t").rfind("ERR", 0), std::string::npos);
    Run(session, "ROLLBACK");

    // The relation is back, and so is its row - a drop retires catalog
    // rows, never data pages.
    EXPECT_EQ(Run("SELECT v FROM t"), "v\\nkept");
}

}  // namespace
}  // namespace kds::server
