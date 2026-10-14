#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/catalog_spills.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/tagged_cell.hpp"
#include "kds/storage/varheap.hpp"
#include "kds/txn/lock_table.hpp"
#include "kds/txn/manager.hpp"
#include "kds/wire/row_codec.hpp"
#include "sim/instance.hpp"

// BW (`instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`):
// `ALTER TABLE ... DROP COLUMN` flags the column's catalog row and touches no
// tuple (BW-R6). The column leaves every SQL surface, every other column
// reads what it read, the dependants that name it refuse the drop (BW-R8),
// and the drop is the statement's transaction's (BW-R7).

namespace kds::server {
namespace {

bool IsErr(const std::string& reply) { return reply.rfind("ERR", 0) == 0; }

class AlterColumnTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
        cabins_.emplace();
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        // The lock family, so the relation `X` is a fence in this rig (BW-R7).
        auto table = txn::LockTable::Create(/*core_count=*/1, /*max_locks_per_txn=*/1024);
        ASSERT_TRUE(table.ok()) << table.status().message();
        locks_ = std::move(table.value());
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr, /*visibility=*/nullptr, /*core=*/0,
                     locks_.get());
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, &*cabins_, &*mgr_);
        dispatcher_->set_locks(locks_.get());
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    std::string Run(Session& s, const std::string& sql) {
        return dispatcher_->Dispatch(sql, &s).response;
    }
    void Ok(const std::string& sql) {
        const std::string out = Run(sql);
        EXPECT_FALSE(IsErr(out)) << sql << " -> " << out;
    }
    void Refused(const std::string& sql, const std::string& needle) {
        const std::string out = Run(sql);
        EXPECT_TRUE(IsErr(out)) << sql << " -> " << out;
        EXPECT_NE(out.find(needle), std::string::npos) << sql << " -> " << out;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::CabinStore> cabins_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::unique_ptr<txn::LockTable> locks_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- Gone from every surface; nothing else moves --------------------------

TEST_F(AlterColumnTest, ADroppedColumnLeavesEverySurfaceAndTheOthersReadAsBefore) {
    Ok("CREATE TABLE t (id int64, a int32, b varchar)");
    Ok("INSERT INTO t VALUES (10, 'one')");
    Ok("INSERT INTO t VALUES (20, 'two')");

    EXPECT_EQ(Run("ALTER TABLE t DROP COLUMN a"), "DROPPED COLUMN t.a");

    EXPECT_EQ(Run("SELECT * FROM t"), "id,b\\n1,one\\n2,two");
    EXPECT_EQ(Run("SELECT b FROM t WHERE b = 'two'"), "b\\ntwo");
    EXPECT_TRUE(IsErr(Run("SELECT a FROM t")));
    EXPECT_TRUE(IsErr(Run("SELECT * FROM t WHERE a = 10")));
    EXPECT_TRUE(IsErr(Run("UPDATE t SET a = 1")));
    const std::string described = Run("DESCRIBE t");
    EXPECT_EQ(described.find("|a|"), std::string::npos) << described;
    EXPECT_EQ(described.find(",a,"), std::string::npos) << described;
    EXPECT_EQ(Run("SELECT name FROM sys.columns WHERE rel_name = 't'"), "name\\nid\\nb");

    // Positional VALUES count live columns: the body, or the key and the body.
    Ok("INSERT INTO t VALUES ('three')");
    Ok("INSERT INTO t VALUES (9, 'nine')");
    Refused("INSERT INTO t VALUES (1, 'x', 'y')", "expected 2 value(s)");
    Ok("UPDATE t SET b = 'TWO' WHERE id = 2");
    EXPECT_EQ(Run("SELECT * FROM t"), "id,b\\n1,one\\n2,TWO\\n3,three\\n9,nine");
}

// A dropped NOT NULL varchar's cell is zero - the `kNull` tag - in every row
// written after the drop; a decode that read it would refuse the row.
TEST_F(AlterColumnTest, ADroppedNotNullVarcharIsNeverDecoded) {
    Ok("CREATE TABLE t (id int64, s varchar, n int32)");
    Ok("INSERT INTO t VALUES ('before', 1)");
    Ok("ALTER TABLE t DROP s");
    Ok("INSERT INTO t VALUES (2)");
    EXPECT_EQ(Run("SELECT * FROM t"), "id,n\\n1,1\\n2,2");
    EXPECT_EQ(Run("SELECT COUNT(*) FROM t WHERE n > 0"), "count(*)\\n2");
}

TEST_F(AlterColumnTest, ADroppedNameIsFreeAndUnreachable) {
    Ok("CREATE TABLE t (id int64, a int32, b int32)");
    Ok("ALTER TABLE t DROP COLUMN a");
    Refused("ALTER TABLE t DROP COLUMN a", "no column named 'a'");
    Refused("CREATE INDEX ix ON t (a)", "no column 'a'");
    Refused("CREATE CABIN ON t(a)", "'a'");
    EXPECT_EQ(Run("ALTER TABLE t RENAME COLUMN b TO a"), "RENAMED COLUMN t.b TO a");
    Ok("INSERT INTO t VALUES (5)");
    EXPECT_EQ(Run("SELECT a FROM t"), "a\\n5");
}

TEST_F(AlterColumnTest, ThePkAnAbsentColumnAndASystemRelationAreRefused) {
    Ok("CREATE TABLE t (id int64, a int32)");
    Refused("ALTER TABLE t DROP COLUMN id", "primary key");
    Refused("ALTER TABLE t DROP COLUMN zz", "byte 26");
    Refused("ALTER TABLE tables DROP COLUMN name", "system relation");
}

// ---- BW-R8: what names the column refuses the drop -------------------------

TEST_F(AlterColumnTest, EveryDependantRefusesTheDropNamingItself) {
    Ok("CREATE TABLE p (id int64, v int64) BTREE");
    Ok("CREATE TABLE t (id int64, k int64, c int64, pid int64 REFERENCES p, st varchar, "
       "cab int64, free int64) BTREE");
    Ok("CREATE INDEX ix_k ON t (k) COVERING (c)");
    Ok("CREATE TRANSITION flow ON t(st) ON INSERT ('new') ON UPDATE ('new' => 'done')");
    Ok("CREATE CABIN ON t(cab)");

    Refused("ALTER TABLE t DROP COLUMN k", "index 'ix_k'");
    Refused("ALTER TABLE t DROP COLUMN c", "index 'ix_k'");
    Refused("ALTER TABLE t DROP COLUMN pid", "references 'p'");
    Refused("ALTER TABLE t DROP COLUMN st", "transition 'flow'");
    Refused("ALTER TABLE t DROP COLUMN cab", "Cabin");
    EXPECT_EQ(Run("ALTER TABLE t DROP COLUMN free"), "DROPPED COLUMN t.free");

    Ok("CREATE TABLE q (id int64, v int64, w int64)");
    Ok("CREATE ASSERTION cap ON q GROUP BY (v) CHECK COUNT(*) <= 5");
    Refused("ALTER TABLE q DROP COLUMN w", "assertion 'cap'");
}

// ---- BW-R7: the statement's transaction's ---------------------------------

TEST_F(AlterColumnTest, ARolledBackDropRestoresTheColumnWithItsValues) {
    Ok("CREATE TABLE t (id int64, a int32, b varchar)");
    Ok("INSERT INTO t VALUES (10, 'one')");

    Session session;
    ASSERT_FALSE(IsErr(Run(session, "BEGIN")));
    EXPECT_EQ(Run(session, "ALTER TABLE t DROP COLUMN a"), "DROPPED COLUMN t.a");
    EXPECT_EQ(Run(session, "SELECT * FROM t"), "id,b\\n1,one");
    ASSERT_FALSE(IsErr(Run(session, "INSERT INTO t VALUES ('two')")));
    ASSERT_FALSE(IsErr(Run(session, "ROLLBACK")));

    EXPECT_EQ(Run("SELECT * FROM t"), "id,a,b\\n1,10,one");
    Ok("INSERT INTO t VALUES (30, 'three')");
    EXPECT_EQ(Run("SELECT a FROM t WHERE b = 'three'"), "a\\n30");
}

// `RENAME COLUMN` is transactionless, so a name an undecided drop frees is
// not free yet: taken, it would outlive the rollback as a second live `a`.
TEST_F(AlterColumnTest, AnUndecidedDropKeepsItsNameFromARename) {
    Ok("CREATE TABLE t (id int64, a int32, b int32)");
    Ok("INSERT INTO t VALUES (10, 20)");

    Session session;
    ASSERT_FALSE(IsErr(Run(session, "BEGIN")));
    EXPECT_EQ(Run(session, "ALTER TABLE t DROP COLUMN a"), "DROPPED COLUMN t.a");
    const std::string renamed = Run(session, "ALTER TABLE t RENAME COLUMN b TO a");
    EXPECT_TRUE(IsErr(renamed)) << renamed;
    ASSERT_FALSE(IsErr(Run(session, "ROLLBACK")));

    EXPECT_EQ(Run("SELECT * FROM t"), "id,a,b\\n1,10,20");
    EXPECT_EQ(Run("SELECT name FROM sys.columns WHERE rel_name = 't'"), "name\\nid\\na\\nb");
}

// The overwrite is in place, so another session resolves an open drop's
// schema; its `INSERT` meets the `X` before it counts values against that
// schema, rather than being refused on a column set that rolls back.
TEST_F(AlterColumnTest, AnOpenDropFencesAnotherWriterBeforeItCountsValues) {
    Ok("CREATE TABLE t (id int64, a int32, b varchar)");

    Session dropper;
    Session writer;
    ASSERT_FALSE(IsErr(Run(dropper, "BEGIN")));
    EXPECT_EQ(Run(dropper, "ALTER TABLE t DROP COLUMN a"), "DROPPED COLUMN t.a");
    // Three values: the arity of the schema the drop may yet roll back to.
    const std::string fenced = Run(writer, "INSERT INTO t VALUES (7, 10, 'one')");
    EXPECT_TRUE(IsErr(fenced)) << fenced;
    EXPECT_EQ(fenced.find("value(s)"), std::string::npos) << fenced;
    ASSERT_FALSE(IsErr(Run(dropper, "ROLLBACK")));

    EXPECT_FALSE(IsErr(Run(writer, "INSERT INTO t VALUES (7, 10, 'one')")));
    EXPECT_EQ(Run("SELECT * FROM t"), "id,a,b\\n7,10,one");
}

// ---- Storage still walks a dropped column -----------------------------------

// `RowSpills` is purge's and the sweep's reader: a dropped varchar's spill is
// still a reference, so it is still released when its row is reclaimed.
TEST(AlterColumnStorageTest, ADroppedVarcharsSpillIsStillAReference) {
    catalog::TableAccess access{};
    catalog::SysColumnRow id{};
    id.type_val = catalog::kTypeValInt64;
    catalog::SysColumnRow s{};
    s.pos = 1;
    s.type_val = catalog::kTypeValVarchar;
    s.flags = catalog::SysColumnRow::kColumnDropped;
    access.schema.columns = {id, s};
    auto layout = catalog::RowLayout::Build(access.schema, storage::kDefaultInlineCellWidth);
    ASSERT_TRUE(layout.ok()) << layout.status().message();
    access.layout = layout.value();

    std::vector<std::byte> payload(access.layout.row_size, std::byte{0});
    const varheap::VarHeapPtr ptr{77, 3};
    ASSERT_TRUE(storage::EncodeSpilledCell(std::span(payload).subspan(access.layout.offsets[1],
                                                                      storage::kDefaultInlineCellWidth),
                                           500, varheap::EncodePtr(ptr))
                    .ok());
    std::vector<exec::SpillRef> spills;
    ASSERT_TRUE(exec::RowSpills(access, payload, spills).ok());
    ASSERT_EQ(spills.size(), 1u);
    EXPECT_EQ(spills[0], exec::SpillRef(77, 3));
}

// A KWP load's rows carry the fields the descriptor names, and the insert
// path expands them: a dropped column is not among them.
TEST(AlterColumnStorageTest, TheWireDescriptorNamesLiveColumnsOnly) {
    catalog::Schema schema;
    catalog::SysColumnRow id{};
    id.type_val = catalog::kTypeValInt64;
    catalog::SysColumnRow a{};
    a.pos = 1;
    a.type_val = catalog::kTypeValInt32;
    catalog::SetName(a.name, "a");
    a.flags = catalog::SysColumnRow::kColumnDropped;
    catalog::SysColumnRow b{};
    b.pos = 2;
    b.type_val = catalog::kTypeValInt32;
    catalog::SetName(b.name, "b");
    schema.columns = {id, a, b};
    const auto fields = wire::DescribeSchema(schema);
    ASSERT_EQ(fields.size(), 2u);
    EXPECT_EQ(fields[1].name, "b");
}

// ---- Durable, and undone at mount ------------------------------------------

TEST(AlterColumnRecoveryTest, ACommittedDropSurvivesACrashAndAnOpenOneIsUndone) {
    auto created = sim::SimInstance::Create();
    ASSERT_TRUE(created.ok()) << created.status().message();
    sim::SimInstance& db = *created.value();
    ASSERT_FALSE(IsErr(db.Execute("CREATE TABLE t (id int64, a int32, b int32, c int32)")));
    ASSERT_FALSE(IsErr(db.Execute("INSERT INTO t VALUES (1, 2, 3)")));
    ASSERT_EQ(db.Execute("ALTER TABLE t DROP COLUMN a"), "DROPPED COLUMN t.a");

    // An open transaction's drop, its records synced, then the power cut.
    ASSERT_FALSE(IsErr(db.Execute("BEGIN")));
    ASSERT_EQ(db.Execute("ALTER TABLE t DROP COLUMN b"), "DROPPED COLUMN t.b");
    ASSERT_FALSE(IsErr(db.Execute("SYNC")));
    db.Crash();
    const Status rebooted = db.Reboot();
    ASSERT_TRUE(rebooted.ok()) << rebooted.message();

    EXPECT_EQ(db.Execute("SELECT * FROM t"), "id,b,c\\n1,2,3");
}

}  // namespace
}  // namespace kds::server
