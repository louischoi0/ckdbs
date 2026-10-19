#include <atomic>
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
#include "kds/server/statement_epoch.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_page.hpp"
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

// One stored row, read off its leaf: its header, and whether every dropped
// cell is clean - zero, with the null bit set where the column has one - and
// the spills its cells name.
struct StoredRow {
    std::uint64_t trx_id = 0;
    bool deleted = false;
    bool clean = false;
    std::vector<exec::SpillRef> spills;
};
StoredRow ReadStoredRow(storage::PageStore& store, const catalog::TableAccess& t,
                        std::uint64_t key) {
    StoredRow row;
    auto at = btree::BtreeLookup(store, t.desc_page_id, key);
    EXPECT_TRUE(at.ok()) << key << ": " << at.status().message();
    if (!at.ok()) return row;
    heap::PageView leaf(at.value().leaf.bytes());
    auto tuple = leaf.ReadTuple(at.value().slot);
    EXPECT_TRUE(tuple.ok()) << tuple.status().message();
    if (!tuple.ok()) return row;
    row.trx_id = tuple.value().trx_id;
    row.deleted = tuple.value().deleted;
    const std::span<const std::byte> payload = tuple.value().payload;
    EXPECT_TRUE(exec::RowSpills(t, payload, row.spills).ok());
    row.clean = true;
    for (std::size_t i = 1; i < t.schema.columns.size(); ++i) {
        if (!t.schema.columns[i].dropped()) continue;
        if (t.layout.null_bits[i] != catalog::kNoNullBit &&
            !catalog::NullBitIsSet(payload, t.layout, i)) {
            row.clean = false;
        }
        for (std::uint32_t b = t.layout.offsets[i]; b < t.layout.cell_ends[i]; ++b) {
            if (payload[b] != std::byte{0}) row.clean = false;
        }
    }
    return row;
}

// Whether a var-heap value is still there to read.
bool SpillLive(storage::PageStore& store, const exec::SpillRef& spill) {
    storage::PageRef pin;
    return varheap::Fetch(store, varheap::VarHeapPtr{spill.first, spill.second}, pin).ok();
}

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
    const catalog::TableAccess* Access(const std::string& table) {
        const auto oid = boot_->catalog.FindTableOidByName(::kds::catalog::kNamespacePublic, table);
        EXPECT_TRUE(oid.ok()) << oid.status().message();
        if (!oid.ok()) return nullptr;
        auto access = boot_->catalog.InitTableAccess(oid.value());
        EXPECT_TRUE(access.ok()) << access.status().message();
        return access.ok() ? access.value() : nullptr;
    }
    // One KWP load chunk of one row, through the load's own seam: the shape
    // it described at LOAD BEGIN, re-resolved by name.
    std::string LoadChunk(const parser::DescribedShape& described,
                          std::vector<parser::AstValue> row) {
        parser::InsertStmt stmt;
        stmt.table_name = "t";
        stmt.described = described;
        stmt.rows.push_back(std::move(row));
        Session session;
        return dispatcher_->ExecuteInsert(stmt, session).response;
    }
    bool DroppedCellsCleanAt(const catalog::TableAccess& t, std::uint64_t key) {
        return ReadStoredRow(store_, t, key).clean;
    }
    // `t (id, s varchar NULL, n int32)`, rows 1..3 each with a spilled `s`,
    // and `s` dropped: three dirty rows for a clean pass.
    const catalog::TableAccess* ThreeDirtyRows() {
        Ok("CREATE TABLE t (id int64, s varchar NULL, n int32)");
        for (int i = 1; i <= 3; ++i) {
            Ok("INSERT INTO t VALUES (" + std::to_string(i) + ", '" + std::string(100, 'x') +
               "', " + std::to_string(i) + ")");
        }
        Ok("ALTER TABLE t DROP COLUMN s");
        return Access("t");
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
    Ok("INSERT INTO t VALUES (NULL, 'three')");
    Ok("INSERT INTO t VALUES (9, 'nine')");
    Refused("INSERT INTO t VALUES (1, 'x', 'y')", "expected 2 value(s)");
    Ok("UPDATE t SET b = 'TWO' WHERE id = 2");
    EXPECT_EQ(Run("SELECT * FROM t"), "id,b\\n1,one\\n2,TWO\\n3,three\\n9,nine");
}

// BW-Q9 [quiet-wrong]: after an ADD, a row written for the old columns -
// key named - has the length the short, key-issued form had; after a DROP,
// a key-issued row has the full form's. Either would put every value one
// column over with no error, so a changed relation takes every live column,
// the key NULL to have it issued.
TEST_F(AlterColumnTest, AChangedColumnSetRefusesTheShortFormThatWouldShiftValues) {
    Ok("CREATE TABLE t (id int64, v int64) SPARE 2");
    Ok("INSERT INTO t VALUES (8)");      // short form, key issued: allowed before the change
    Ok("INSERT INTO t VALUES (NULL, 9)");  // a NULL key asks for one too
    Ok("ALTER TABLE t ADD COLUMN a int64 NULL");
    Refused("INSERT INTO t VALUES (9, 8)", "changed by ALTER TABLE");
    Ok("INSERT INTO t VALUES (NULL, 7, 6)");
    Ok("INSERT INTO t VALUES (20, 5, 4)");
    EXPECT_EQ(Run("SELECT * FROM t"), "id,v,a\\n1,8,NULL\\n2,9,NULL\\n3,7,6\\n20,5,4");

    Ok("CREATE TABLE u (id int64, a int64, b int64)");
    Ok("ALTER TABLE u DROP COLUMN a");
    Refused("INSERT INTO u VALUES (5)", "changed by ALTER TABLE");
    Ok("INSERT INTO u VALUES (NULL, 5)");
    EXPECT_EQ(Run("SELECT * FROM u"), "id,b\\n1,5");
}

// BW-Q10 [quiet-wrong]: a KWP load's rows are positional against the shape
// it described at LOAD BEGIN. A drop and an add of equal net width before a
// chunk would put a dropped column's value in the added one, so the chunk -
// which re-resolves by name - is refused unless the relation is the one, in
// the shape, it described. Driven through the load's own seam.
TEST_F(AlterColumnTest, ALoadChunkAgainstAChangedShapeIsRefused) {
    Ok("CREATE TABLE t (id int64, a int64, b int64) SPARE 1");
    const auto oid = boot_->catalog.FindTableOidByName(::kds::catalog::kNamespacePublic, "t");
    ASSERT_TRUE(oid.ok());
    auto access = boot_->catalog.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    const parser::DescribedShape described{oid.value(), 3, access.value()->layout.live_columns};

    const auto chunk = [&](std::int64_t a, std::int64_t b) {
        std::vector<parser::AstValue> row(3);  // a NULL key: issue one
        row[1].type = parser::ValueType::kInt;
        row[1].int_val = a;
        row[2].type = parser::ValueType::kInt;
        row[2].int_val = b;
        return LoadChunk(described, std::move(row));
    };
    EXPECT_FALSE(IsErr(chunk(1, 2)));

    Ok("ALTER TABLE t DROP COLUMN a");
    Ok("ALTER TABLE t ADD COLUMN c int64 NULL");  // three live columns again
    const std::string refused = chunk(3, 4);
    EXPECT_TRUE(IsErr(refused)) << refused;
    EXPECT_NE(refused.find("begin a new load"), std::string::npos) << refused;
    EXPECT_EQ(Run("SELECT * FROM t"), "id,b,c\\n1,2,NULL");
}

// A dropped NOT NULL varchar's cell is zero - the `kNull` tag - in every row
// written after the drop; a decode that read it would refuse the row.
TEST_F(AlterColumnTest, ADroppedNotNullVarcharIsNeverDecoded) {
    Ok("CREATE TABLE t (id int64, s varchar, n int32)");
    Ok("INSERT INTO t VALUES ('before', 1)");
    Ok("ALTER TABLE t DROP s");
    Ok("INSERT INTO t VALUES (NULL, 2)");
    EXPECT_EQ(Run("SELECT * FROM t"), "id,n\\n1,1\\n2,2");
    EXPECT_EQ(Run("SELECT COUNT(*) FROM t WHERE n > 0"), "count(*)\\n2");
}

// CD-S1: every row written after a drop is born clean - each dropped cell
// zero and, where the column has a null bit, the bit set - on every path that
// places a row (a prepared statement binds by text, so it is the literal
// path). An UPDATE copies the before-image, so it carries the bit; a row from
// before the drop keeps the bit clear until a clean pass, and reads alike.
TEST_F(AlterColumnTest, ARowWrittenAfterADropIsBornClean) {
    Ok("CREATE TABLE t (id int64, a int32 NULL, s varchar NULL, k int64, n int32) SPARE 1");
    Ok("ALTER TABLE t ADD COLUMN x varchar(16) NULL");
    Ok("INSERT INTO t VALUES (1, 9, 'before-the-drop-and-long-enough-to-spill', 8, 1, 'xx')");
    Ok("ALTER TABLE t DROP COLUMN a");
    Ok("ALTER TABLE t DROP COLUMN s");
    Ok("ALTER TABLE t DROP COLUMN k");  // NOT NULL: no bit, a zero cell
    Ok("ALTER TABLE t DROP COLUMN x");
    const catalog::TableAccess* t = Access("t");
    ASSERT_NE(t, nullptr);

    Ok("INSERT INTO t VALUES (NULL, 2)");
    Ok("INSERT INTO t VALUES (NULL, 3), (NULL, 4)");
    Ok("INSERT INTO t (n) VALUES (5)");
    Ok("INSERT INTO t VALUES (6, 6) ON CONFLICT DO NOTHING");
    Ok("INSERT INTO t SELECT id + 10, n FROM t WHERE n = 2");
    std::vector<parser::AstValue> row(2);  // the KWP load's chunk, its key named
    row[0].type = parser::ValueType::kInt;
    row[0].int_val = 7;
    row[1].type = parser::ValueType::kInt;
    row[1].int_val = 7;
    EXPECT_FALSE(IsErr(LoadChunk(parser::DescribedShape{t->oid,
                                                        static_cast<std::uint32_t>(
                                                            t->schema.columns.size()),
                                                        t->layout.live_columns},
                                 std::move(row))));
    Ok("UPDATE t SET n = n + 100 WHERE id = 2");

    EXPECT_FALSE(DroppedCellsCleanAt(*t, 1));  // written before the drop; its bytes stay
    for (const std::uint64_t key : {2, 3, 4, 5, 6, 7, 12}) {
        EXPECT_TRUE(DroppedCellsCleanAt(*t, key)) << key;
    }
    EXPECT_EQ(Run("SELECT * FROM t"),
              "id,n\\n1,1\\n2,102\\n3,3\\n4,4\\n5,5\\n6,6\\n7,7\\n12,2");
}

TEST_F(AlterColumnTest, ADroppedNameIsFreeAndUnreachable) {
    Ok("CREATE TABLE t (id int64, a int32, b int32)");
    Ok("ALTER TABLE t DROP COLUMN a");
    Refused("ALTER TABLE t DROP COLUMN a", "no column named 'a'");
    Refused("CREATE INDEX ix ON t (a)", "no column 'a'");
    Refused("CREATE CABIN ON t(a)", "'a'");
    EXPECT_EQ(Run("ALTER TABLE t RENAME COLUMN b TO a"), "RENAMED COLUMN t.b TO a");
    Ok("INSERT INTO t VALUES (NULL, 5)");
    EXPECT_EQ(Run("SELECT a FROM t"), "a\\n5");
}

TEST_F(AlterColumnTest, ThePkAnAbsentColumnAndASystemRelationAreRefused) {
    Ok("CREATE TABLE t (id int64, a int32)");
    Refused("ALTER TABLE t DROP COLUMN id", "tuple key");
    Refused("ALTER TABLE t DROP COLUMN zz", "byte 26");
    Refused("ALTER TABLE sys.tables DROP COLUMN name", "catalog relation");
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
    ASSERT_FALSE(IsErr(Run(session, "INSERT INTO t VALUES (NULL, 'two')")));
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

// ---- CD-S2: PURGE DROPPED COLUMNS cleans dropped cells in place ------------
//
// `instructions/v3.2.0/workorder-cd-dropped-column-cleaning.md` §2-§3. The
// waits on the asynchronous path are `lock_family_test.cpp`'s
// `CleanDroppedWaitTest`; here the synchronous path answers a wait with its
// refusal at once.

// Every dropped cell - a spilled varchar, a nullable int, a NOT NULL int with
// no bit, a slot varchar - of every row, a delete-marked one included, is
// zeroed with its bit set; the header's writer and delete mark stay; every
// spill a dropped cell named is released, once. A re-run writes nothing.
TEST_F(AlterColumnTest, ACleanZeroesEveryDroppedCellOnceAndReleasesItsSpills) {
    Ok("CREATE TABLE t (id int64, s varchar NULL, a int32 NULL, k int64, n int32) SPARE 1");
    Ok("ALTER TABLE t ADD COLUMN x varchar(16) NULL");
    const std::string spilled(100, 'x');
    for (int i = 1; i <= 3; ++i) {
        Ok("INSERT INTO t VALUES (" + std::to_string(i) + ", '" + spilled + "', 7, 8, " +
           std::to_string(i) + ", 'longer-than-thirteen')");
    }
    Ok("DELETE FROM t WHERE id = 3");
    for (const char* c : {"s", "a", "k", "x"}) Ok(std::string("ALTER TABLE t DROP COLUMN ") + c);
    const catalog::TableAccess* t = Access("t");
    ASSERT_NE(t, nullptr);

    std::vector<StoredRow> before;
    for (std::uint64_t key = 1; key <= 3; ++key) {
        before.push_back(ReadStoredRow(store_, *t, key));
        ASSERT_FALSE(before.back().clean) << key;
        ASSERT_EQ(before.back().spills.size(), 2u) << key;
    }
    EXPECT_EQ(Run("PURGE DROPPED COLUMNS FROM t"),
              "PURGED DROPPED COLUMNS t: 4 column(s) clean, 3 row(s) cleaned, 6 spill(s) "
              "released");
    for (std::uint64_t key = 1; key <= 3; ++key) {
        const StoredRow after = ReadStoredRow(store_, *t, key);
        EXPECT_TRUE(after.clean) << key;
        EXPECT_TRUE(after.spills.empty()) << key;
        EXPECT_EQ(after.trx_id, before[key - 1].trx_id) << key;
        EXPECT_EQ(after.deleted, key == 3) << key;
        for (const exec::SpillRef& spill : before[key - 1].spills) {
            EXPECT_FALSE(SpillLive(store_, spill)) << key;
        }
    }
    EXPECT_EQ(Run("PURGE DROPPED COLUMNS FROM t"),
              "PURGED DROPPED COLUMNS t: 4 column(s) clean, 0 row(s) cleaned, 0 spill(s) "
              "released");
    EXPECT_EQ(Run("SELECT * FROM t"), "id,n\\n1,1\\n2,2");
}

// More rows than two batches (512 each): the walk resumes at the first dirty
// key past each batch and reaches every row.
TEST_F(AlterColumnTest, ACleanCrossesItsBatchesAndReachesEveryRow) {
    Ok("CREATE TABLE t (id int64, a int32 NULL, n int32)");
    constexpr int kRows = 1100;
    for (int base = 0; base < kRows; base += 100) {
        std::string values;
        for (int i = base + 1; i <= base + 100; ++i) {
            values += (values.empty() ? "(" : ", (") + std::to_string(i) + ", 7, 1)";
        }
        Ok("INSERT INTO t VALUES " + values);
    }
    Ok("ALTER TABLE t DROP COLUMN a");
    const catalog::TableAccess* t = Access("t");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(Run("PURGE DROPPED COLUMNS FROM t"),
              "PURGED DROPPED COLUMNS t: 1 column(s) clean, 1100 row(s) cleaned, 0 spill(s) "
              "released");
    for (std::uint64_t key = 1; key <= kRows; ++key) {
        ASSERT_TRUE(DroppedCellsCleanAt(*t, key)) << key;
    }
    EXPECT_EQ(Run("PURGE DROPPED COLUMNS FROM t"),
              "PURGED DROPPED COLUMNS t: 1 column(s) clean, 0 row(s) cleaned, 0 spill(s) "
              "released");
}

TEST_F(AlterColumnTest, ACleanIsRefusedWithItsByte) {
    Ok("CREATE TABLE u (id int64, n int32)");
    Refused("PURGE DROPPED COLUMNS FROM u", "nothing to clean (byte 27)");
    Refused("PURGE DROPPED COLUMNS FROM nope", "byte 27");
    ASSERT_NE(ThreeDirtyRows(), nullptr);
    Refused("PURGE DROPPED COLUMNS FROM t WHERE id = 1", "a WHERE is not implemented (byte 29)");
    Refused("PURGE DROPPED FROM t", "expected 'COLUMNS', got 'FROM' at byte 14");
    Session s;
    ASSERT_FALSE(IsErr(Run(s, "BEGIN")));
    const std::string inside = Run(s, "PURGE DROPPED COLUMNS FROM t");
    EXPECT_TRUE(IsErr(inside)) << inside;
    EXPECT_EQ(StatusFromErrorReply(inside).code(), StatusCode::kNotImplemented) << inside;
    EXPECT_NE(inside.find("(byte 0)"), std::string::npos) << inside;
    ASSERT_FALSE(IsErr(Run(s, "ROLLBACK")));
    EXPECT_EQ(StatusFromErrorReply(Run("PURGE DROPPED COLUMNS FROM u")).code(),
              StatusCode::kNotImplemented);
}

// PURGE's key refusals - a heap's hidden key, a Transition's history - do not
// apply: the clean retires no key (§1 item 1). An FK child is cleaned too.
TEST_F(AlterColumnTest, AHeapAGovernedRelationAndAnFkChildAreCleaned) {
    const std::string spilled(100, 'y');
    Ok("CREATE TABLE h (v int32, w varchar NULL) HEAP");
    Ok("INSERT INTO h VALUES (1, '" + spilled + "')");
    Ok("ALTER TABLE h DROP COLUMN w");
    EXPECT_EQ(Run("PURGE DROPPED COLUMNS FROM h"),
              "PURGED DROPPED COLUMNS h: 1 column(s) clean, 1 row(s) cleaned, 1 spill(s) "
              "released");

    Ok("CREATE TABLE g (id int64, st varchar, c varchar NULL)");
    Ok("CREATE TRANSITION flow ON g(st) ON INSERT ('new') ON UPDATE ('new' => 'done')");
    Ok("INSERT INTO g VALUES (1, 'new', '" + spilled + "')");
    Ok("ALTER TABLE g DROP COLUMN c");
    EXPECT_EQ(Run("PURGE DROPPED COLUMNS FROM g"),
              "PURGED DROPPED COLUMNS g: 1 column(s) clean, 1 row(s) cleaned, 1 spill(s) "
              "released");

    Ok("CREATE TABLE p (id int64, v int32)");
    Ok("INSERT INTO p VALUES (1, 1)");
    Ok("CREATE TABLE c (id int64, pid int64 REFERENCES p, z varchar NULL)");
    Ok("INSERT INTO c VALUES (1, 1, '" + spilled + "')");
    Ok("ALTER TABLE c DROP COLUMN z");
    EXPECT_EQ(Run("PURGE DROPPED COLUMNS FROM c"),
              "PURGED DROPPED COLUMNS c: 1 column(s) clean, 1 row(s) cleaned, 1 spill(s) "
              "released");
    EXPECT_EQ(Run("SELECT * FROM c"), "id,pid\\n1,1");
}

// The per-row gate, `[quiet-wrong]` (§3 point 1): an undecided writer copied
// the dirty row into its undo, and its rollback writes that image back, so
// its head is not cleaned until it decides. The rows around it are, and stay
// clean; a re-run after either decide finishes the relation.
TEST_F(AlterColumnTest, AnUndecidedWritersRowIsCleanedOnlyAfterItsDecide) {
    const catalog::TableAccess* t = ThreeDirtyRows();
    ASSERT_NE(t, nullptr);
    Session rolls_back;
    Session commits;
    ASSERT_FALSE(IsErr(Run(rolls_back, "BEGIN")));
    ASSERT_FALSE(IsErr(Run(rolls_back, "UPDATE t SET n = n + 10 WHERE id = 1")));
    ASSERT_FALSE(IsErr(Run(commits, "BEGIN")));
    ASSERT_FALSE(IsErr(Run(commits, "UPDATE t SET n = n + 10 WHERE id = 2")));
    const std::vector<exec::SpillRef> one = ReadStoredRow(store_, *t, 1).spills;
    const std::vector<exec::SpillRef> two = ReadStoredRow(store_, *t, 2).spills;
    ASSERT_EQ(one.size(), 1u);
    ASSERT_EQ(two.size(), 1u);

    const std::string refused = Run("PURGE DROPPED COLUMNS FROM t");
    EXPECT_EQ(StatusFromErrorReply(refused).code(), StatusCode::kTxnConflict) << refused;
    EXPECT_NE(refused.find("tuple key 1"), std::string::npos) << refused;
    EXPECT_NE(refused.find("has not decided"), std::string::npos) << refused;
    EXPECT_NE(refused.find("2 row(s) are blocked, 1 were cleaned"), std::string::npos) << refused;
    EXPECT_FALSE(DroppedCellsCleanAt(*t, 1));
    EXPECT_FALSE(DroppedCellsCleanAt(*t, 2));
    EXPECT_TRUE(DroppedCellsCleanAt(*t, 3));
    EXPECT_TRUE(SpillLive(store_, one.front()));
    EXPECT_TRUE(SpillLive(store_, two.front()));

    ASSERT_FALSE(IsErr(Run(rolls_back, "ROLLBACK")));
    ASSERT_FALSE(IsErr(Run(commits, "COMMIT")));
    EXPECT_FALSE(DroppedCellsCleanAt(*t, 1));  // the rollback wrote the dirty image back
    EXPECT_EQ(Run("PURGE DROPPED COLUMNS FROM t"),
              "PURGED DROPPED COLUMNS t: 1 column(s) clean, 2 row(s) cleaned, 2 spill(s) "
              "released");
    for (std::uint64_t key = 1; key <= 3; ++key) EXPECT_TRUE(DroppedCellsCleanAt(*t, key)) << key;
    EXPECT_FALSE(SpillLive(store_, one.front()));
    EXPECT_FALSE(SpillLive(store_, two.front()));
    EXPECT_EQ(Run("SELECT * FROM t"), "id,n\\n1,1\\n2,12\\n3,3");
}

// The reader arm of the gate: a repeatable-read snapshot older than a row's
// writer may walk that row's undo chain, so the row and its spill stay until
// the snapshot ends; the reader still reads its version.
TEST_F(AlterColumnTest, AnOlderSnapshotKeepsItsRowDirtyAndItsSpillLive) {
    const catalog::TableAccess* t = ThreeDirtyRows();
    ASSERT_NE(t, nullptr);
    Session r;
    ASSERT_FALSE(IsErr(Run(r, "BEGIN ISOLATION LEVEL REPEATABLE READ")));
    ASSERT_EQ(Run(r, "SELECT n FROM t WHERE id = 1"), "n\\n1");
    Ok("UPDATE t SET n = 21 WHERE id = 1");
    const std::vector<exec::SpillRef> held = ReadStoredRow(store_, *t, 1).spills;
    ASSERT_EQ(held.size(), 1u);

    const std::string refused = Run("PURGE DROPPED COLUMNS FROM t");
    EXPECT_EQ(StatusFromErrorReply(refused).code(), StatusCode::kTxnConflict) << refused;
    EXPECT_NE(refused.find("tuple key 1"), std::string::npos) << refused;
    EXPECT_NE(refused.find("snapshot"), std::string::npos) << refused;
    EXPECT_FALSE(DroppedCellsCleanAt(*t, 1));
    EXPECT_TRUE(SpillLive(store_, held.front()));
    EXPECT_TRUE(DroppedCellsCleanAt(*t, 2));
    EXPECT_EQ(Run(r, "SELECT n FROM t WHERE id = 1"), "n\\n1");

    ASSERT_FALSE(IsErr(Run(r, "COMMIT")));
    EXPECT_EQ(Run("PURGE DROPPED COLUMNS FROM t"),
              "PURGED DROPPED COLUMNS t: 1 column(s) clean, 1 row(s) cleaned, 1 spill(s) "
              "released");
    EXPECT_FALSE(SpillLive(store_, held.front()));
}

// The statement epoch, `[quiet-wrong]` (§3 point 3): a statement on another
// core that revalidated below the drop may still decode the dropped column as
// live, so nothing is cleaned until its slot moves on. Core 1's slot is
// driven by hand.
TEST_F(AlterColumnTest, AStatementOnAnOlderSchemaWordHoldsTheClean) {
    std::atomic<std::uint64_t> word{1};
    StatementEpochs epochs(/*cores=*/2);
    boot_->catalog.SetSchemaWord(&word);
    dispatcher_->SetStatementEpochs(&epochs);
    struct Unwire {
        CommandDispatcher* dispatcher;
        catalog::Catalog* catalog;
        ~Unwire() {
            dispatcher->SetStatementEpochs(nullptr);
            catalog->SetSchemaWord(nullptr);
        }
    } unwire{&*dispatcher_, &boot_->catalog};
    Ok("CREATE TABLE t (id int64, s varchar NULL, n int32)");
    Ok("INSERT INTO t VALUES (1, '" + std::string(100, 'x') + "', 1)");
    epochs.Enter(1);
    epochs.Publish(1, word.load());  // bound before the drop
    Ok("ALTER TABLE t DROP COLUMN s");

    const std::string refused = Run("PURGE DROPPED COLUMNS FROM t");
    EXPECT_EQ(StatusFromErrorReply(refused).code(), StatusCode::kTxnConflict) << refused;
    EXPECT_NE(refused.find("older statement"), std::string::npos) << refused;
    const catalog::TableAccess* t = Access("t");
    ASSERT_NE(t, nullptr);
    EXPECT_FALSE(DroppedCellsCleanAt(*t, 1));

    epochs.Leave(1);
    EXPECT_EQ(Run("PURGE DROPPED COLUMNS FROM t"),
              "PURGED DROPPED COLUMNS t: 1 column(s) clean, 1 row(s) cleaned, 1 spill(s) "
              "released");
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


// ---- ADD COLUMN (BW-S3) ------------------------------------------------------

TEST_F(AlterColumnTest, RowsThatPredateAnAddedColumnReadNullInEveryType) {
    Ok("CREATE TABLE t (id int64, v int32) SPARE 8");
    Ok("INSERT INTO t VALUES (1)");
    Ok("UPDATE t SET v = 2 WHERE id = 1");  // an updated row carries the vacant bits
    for (const char* add : {"ALTER TABLE t ADD COLUMN a int64 NULL",
                            "ALTER TABLE t ADD b bool NULL",
                            "ALTER TABLE t ADD COLUMN c date NULL",
                            "ALTER TABLE t ADD COLUMN d decimal(30, 2) NULL",
                            "ALTER TABLE t ADD COLUMN e char(4) NULL",
                            "ALTER TABLE t ADD COLUMN f varchar(16) NULL"}) {
        const std::string out = Run(add);
        EXPECT_EQ(out.rfind("ADDED COLUMN t.", 0), 0u) << add << " -> " << out;
    }
    EXPECT_EQ(Run("SELECT * FROM t"), "id,v,a,b,c,d,e,f\\n1,2,NULL,NULL,NULL,NULL,NULL,NULL");
    EXPECT_EQ(Run("SELECT COUNT(*) FROM t WHERE a IS NULL AND f IS NULL"), "count(*)\\n1");
    EXPECT_EQ(Run("SELECT name, spare_slots, spare_free FROM sys.tables WHERE name = 't'"),
              "name,spare_slots,spare_free\\nt,8,2");
}

TEST_F(AlterColumnTest, AnAddedColumnTakesValuesInlineAndSpilledAndIsUpdated) {
    Ok("CREATE TABLE t (id int64, v int32) SPARE 2");
    Ok("INSERT INTO t VALUES (1)");
    Ok("ALTER TABLE t ADD COLUMN s varchar(16) NULL");
    Ok("ALTER TABLE t ADD COLUMN n int64 NULL");
    const std::string longer(100, 'z');
    Ok("INSERT INTO t VALUES (NULL, 2, 'short', 7)");
    Ok("INSERT INTO t VALUES (NULL, 3, '" + longer + "', NULL)");
    EXPECT_EQ(Run("SELECT s, n FROM t WHERE id = 2"), "s,n\\nshort,7");
    EXPECT_EQ(Run("SELECT s FROM t WHERE id = 3"), "s\\n" + longer);
    Ok("UPDATE t SET n = 9, s = 'fill' WHERE id = 1");
    EXPECT_EQ(Run("SELECT * FROM t"),
              "id,v,s,n\\n1,1,fill,9\\n2,2,short,7\\n3,3," + longer + ",NULL");
    EXPECT_EQ(Run("SELECT id FROM t WHERE n = 9"), "id\\n1");
}

TEST_F(AlterColumnTest, AnAddAfterADropTakesTheNextSlotNeverTheDroppedOne) {
    Ok("CREATE TABLE t (id int64, v int32) SPARE 2");
    Ok("ALTER TABLE t ADD COLUMN a int64 NULL");
    Ok("INSERT INTO t VALUES (NULL, 1, 11)");
    Ok("ALTER TABLE t DROP COLUMN a");
    Ok("ALTER TABLE t ADD COLUMN a int64 NULL");  // the name is free; the slot is not
    // Slot 0's bytes still hold 11; the new `a` lives in slot 1 and reads NULL.
    EXPECT_EQ(Run("SELECT * FROM t"), "id,v,a\\n1,1,NULL");
    Refused("ALTER TABLE t ADD COLUMN b int64 NULL", "column slots are taken");
}

TEST_F(AlterColumnTest, WhatAnAddCannotHoldIsRefusedWithItsByte) {
    Ok("CREATE TABLE plain (id int64, v int32)");
    Refused("ALTER TABLE plain ADD COLUMN a int64 NULL", "reserved no column slots");

    Ok("CREATE TABLE t (id int64, v int32) SPARE 2");
    Refused("ALTER TABLE t ADD COLUMN a int64", "must be declared NULL");
    Refused("ALTER TABLE t ADD COLUMN a int64 NOT NULL", "must be declared NULL");
    Refused("ALTER TABLE t ADD COLUMN s varchar NULL", "varchar(16)");
    Refused("ALTER TABLE t ADD COLUMN s varchar(32) NULL", "varchar(16)");
    Refused("ALTER TABLE t ADD COLUMN c char(17) NULL", "16 bytes");
    Refused("ALTER TABLE t ADD COLUMN v int64 NULL", "already has a column named 'v'");
    Refused("ALTER TABLE t ADD COLUMN p int64 NULL REFERENCES t", "CREATE TABLE only");
    Refused("ALTER TABLE t ADD COLUMN q int64 NULL CABIN", "CREATE CABIN");
    Refused("ALTER TABLE t ADD COLUMN f float NULL", "float");
    Refused("ALTER TABLE sys.tables ADD COLUMN x int64 NULL", "catalog relation");
    // Nothing was taken by a refusal.
    EXPECT_EQ(Run("SELECT name, spare_free FROM sys.tables WHERE name = 't'"),
              "name,spare_free\\nt,2");
}

TEST_F(AlterColumnTest, ARolledBackAddFreesItsSlotAndTakesItsRowsWithIt) {
    Ok("CREATE TABLE t (id int64, v int32) SPARE 1");
    Ok("INSERT INTO t VALUES (1)");

    Session session;
    ASSERT_FALSE(IsErr(Run(session, "BEGIN")));
    EXPECT_EQ(Run(session, "ALTER TABLE t ADD COLUMN a int64 NULL"), "ADDED COLUMN t.a");
    ASSERT_FALSE(IsErr(Run(session, "INSERT INTO t VALUES (NULL, 2, 22)")));
    EXPECT_EQ(Run(session, "SELECT * FROM t"), "id,v,a\\n1,1,NULL\\n2,2,22");
    ASSERT_FALSE(IsErr(Run(session, "ROLLBACK")));

    EXPECT_EQ(Run("SELECT * FROM t"), "id,v\\n1,1");
    EXPECT_EQ(Run("ALTER TABLE t ADD COLUMN b int64 NULL"), "ADDED COLUMN t.b");
    EXPECT_EQ(Run("SELECT * FROM t"), "id,v,b\\n1,1,NULL");
}

TEST_F(AlterColumnTest, ACabinServesAnAddedColumnAndANullableOnlyObjectIsRefused) {
    Ok("CREATE TABLE t (id int64, v int32) BTREE SPARE 3");
    Ok("INSERT INTO t VALUES (1)");
    Ok("ALTER TABLE t ADD COLUMN st varchar(16) NULL");
    Ok("ALTER TABLE t ADD COLUMN c int64 NULL");
    Ok("CREATE CABIN ON t(c)");
    Ok("INSERT INTO t VALUES (NULL, 2, NULL, 5)");
    EXPECT_EQ(Run("SELECT id FROM t WHERE c = 5"), "id\\n2");
    Ok("INSERT INTO t VALUES (NULL, 3, 'new', 6)");
    Ok("UPDATE t SET st = 'done' WHERE id = 3");
    EXPECT_EQ(Run("SELECT st FROM t WHERE id = 3"), "st\\ndone");
    // An added column is nullable (BW-R5), and neither an index (null.md D2)
    // nor a Transition (CN-4) takes a nullable column.
    Refused("CREATE INDEX ix ON t (c)", "nullable");
    Refused("CREATE TRANSITION flow ON t(st) ON INSERT ('new') ON UPDATE ('new' => 'done')",
            "nullable");
}

// ---- Durable, and undone at mount (ADD) -----------------------------------

TEST(AlterColumnRecoveryTest, ACommittedAddSurvivesACrashAndAnOpenOneIsUndone) {
    auto created = sim::SimInstance::Create();
    ASSERT_TRUE(created.ok()) << created.status().message();
    sim::SimInstance& db = *created.value();
    ASSERT_FALSE(IsErr(db.Execute("CREATE TABLE t (id int64, v int32) SPARE 2")));
    ASSERT_FALSE(IsErr(db.Execute("INSERT INTO t VALUES (1)")));
    ASSERT_EQ(db.Execute("ALTER TABLE t ADD COLUMN a int64 NULL"), "ADDED COLUMN t.a");
    ASSERT_FALSE(IsErr(db.Execute("INSERT INTO t VALUES (NULL, 2, 20)")));

    ASSERT_FALSE(IsErr(db.Execute("BEGIN")));
    ASSERT_EQ(db.Execute("ALTER TABLE t ADD COLUMN b int64 NULL"), "ADDED COLUMN t.b");
    ASSERT_FALSE(IsErr(db.Execute("INSERT INTO t VALUES (NULL, 3, 30, 300)")));
    ASSERT_FALSE(IsErr(db.Execute("SYNC")));
    db.Crash();
    const Status rebooted = db.Reboot();
    ASSERT_TRUE(rebooted.ok()) << rebooted.message();

    EXPECT_EQ(db.Execute("SELECT * FROM t"), "id,v,a\\n1,1,NULL\\n2,2,20");
    EXPECT_EQ(db.Execute("ALTER TABLE t ADD COLUMN b int64 NULL"), "ADDED COLUMN t.b");
}

// ---- CD-S2: a clean is durable, and is no transaction's (P1) ---------------

// CD-S0 P1, pinned: each clean is a HEAP_OVERWRITE on a B+ tree leaf at
// `kNoTxnId` carrying the header's writer. The inserts are checkpointed, so
// their commits lie below the scan and a record naming one of them would make
// it a loser; the crash keeps the log and loses every page written since, so
// redo must replay each clean. With the pass half done - one row held by an
// open writer - every clean replays with its header unchanged, the open
// writer is the only loser and its row alone is undone, and a re-run
// finishes the relation.
TEST(AlterColumnRecoveryTest, ACleanSurvivesACrashAndMakesNoLoser) {
    auto created = sim::SimInstance::Create();
    ASSERT_TRUE(created.ok()) << created.status().message();
    sim::SimInstance& db = *created.value();
    ASSERT_FALSE(IsErr(db.Execute("CREATE TABLE t (id int64, s varchar NULL, n int32)")));
    for (int i = 1; i <= 3; ++i) {
        ASSERT_FALSE(IsErr(db.Execute("INSERT INTO t VALUES (" + std::to_string(i) + ", '" +
                                      std::string(100, 'x') + "', " + std::to_string(i) + ")")));
    }
    ASSERT_FALSE(IsErr(db.Execute("ALTER TABLE t DROP COLUMN s")));
    ASSERT_FALSE(IsErr(db.Execute("SYNC")));
    ASSERT_TRUE(db.RunCheckpoint().ok());
    const auto table = [&db]() -> const catalog::TableAccess* {
        auto oid = db.catalog().FindTableOidByName(::kds::catalog::kNamespacePublic, "t");
        EXPECT_TRUE(oid.ok());
        auto access = db.catalog().InitTableAccess(oid.value());
        EXPECT_TRUE(access.ok());
        return access.ok() ? access.value() : nullptr;
    };
    std::vector<std::uint64_t> writers;
    for (std::uint64_t key = 1; key <= 3; ++key) {
        writers.push_back(ReadStoredRow(db.store(), *table(), key).trx_id);
    }

    Session w;
    ASSERT_FALSE(IsErr(db.dispatcher().Dispatch("BEGIN", &w).response));
    ASSERT_FALSE(IsErr(db.dispatcher().Dispatch("UPDATE t SET n = 20 WHERE id = 2", &w).response));
    const std::string refused = db.Execute("PURGE DROPPED COLUMNS FROM t");
    ASSERT_NE(refused.find("2 were cleaned"), std::string::npos) << refused;
    // A strict commit syncs the log through every clean, and no page.
    ASSERT_FALSE(IsErr(db.Execute("CREATE TABLE fence (id int64)")));
    ASSERT_FALSE(IsErr(db.Execute("SET DURABILITY strict")));
    ASSERT_FALSE(IsErr(db.Execute("INSERT INTO fence VALUES (1)")));
    db.Crash();
    const Status rebooted = db.Reboot();
    ASSERT_TRUE(rebooted.ok()) << rebooted.message();

    EXPECT_EQ(db.recovery().losers, 1u) << "a clean's record named a writer";
    for (std::uint64_t key = 1; key <= 3; ++key) {
        const StoredRow row = ReadStoredRow(db.store(), *table(), key);
        EXPECT_EQ(row.clean, key != 2) << key;
        EXPECT_EQ(row.trx_id, writers[key - 1]) << key;
    }
    EXPECT_EQ(db.Execute("SELECT * FROM t"), "id,n\\n1,1\\n2,2\\n3,3");
    EXPECT_EQ(db.Execute("PURGE DROPPED COLUMNS FROM t"),
              "PURGED DROPPED COLUMNS t: 1 column(s) clean, 1 row(s) cleaned, 1 spill(s) "
              "released");
}

}  // namespace
}  // namespace kds::server
