#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/lock_table.hpp"
#include "kds/txn/manager.hpp"

// Column names are unique ignoring case (raft-marks-2026-10-14.md §13,
// option (a)): CREATE TABLE, ALTER TABLE ADD COLUMN and RENAME COLUMN refuse
// a name equal to a live one ignoring case. A relation created before the
// rule can still hold `id` beside `ID`; a read naming such a pair is refused
// as ambiguous rather than answered with the first column's values, which
// is what it did before ([quiet-wrong]).

namespace kds::server {
namespace {

bool IsErr(const std::string& reply) { return reply.rfind("ERR", 0) == 0; }

class ColumnNameCaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
        cabins_.emplace();
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
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
    void Refused(const std::string& sql, std::initializer_list<std::string_view> needles) {
        const std::string out = Run(sql);
        EXPECT_TRUE(IsErr(out)) << sql << " -> " << out;
        for (std::string_view needle : needles) {
            EXPECT_NE(out.find(needle), std::string::npos) << sql << " -> " << out;
        }
    }

    // A relation as a build before the rule could leave it: every
    // `sys.columns` name `from` is rewritten in place to `to` (same length),
    // past every door that now refuses it, and the catalog's memo dropped.
    void RewriteColumnName(std::string_view from, std::string_view to) {
        ASSERT_EQ(from.size(), to.size());
        int rewritten = 0;
        for (PageId at = catalog::kCatalogPageColumns; at != kInvalidPageId;) {
            auto page = store_.Get(at);
            ASSERT_TRUE(page.ok());
            auto bytes = page.value().bytes();
            // The name field is NUL-padded, so `from` followed by a NUL is
            // the whole name.
            for (std::size_t i = 0; i + from.size() < bytes.size(); ++i) {
                if (std::memcmp(bytes.data() + i, from.data(), from.size()) == 0 &&
                    bytes[i + from.size()] == std::byte{0}) {
                    std::memcpy(bytes.data() + i, to.data(), to.size());
                    ++rewritten;
                }
            }
            at = heap::PageView(bytes).next_page_id();
        }
        ASSERT_EQ(rewritten, 1) << "expected one column named '" << from << "'";
        boot_->catalog.DropCache();
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

// ---- The one rule ----------------------------------------------------------

TEST(ColumnNamesCollide, EqualIgnoringAsciiCaseAndNothingElse) {
    EXPECT_TRUE(catalog::ColumnNamesCollide("id", "ID"));
    EXPECT_TRUE(catalog::ColumnNamesCollide("Id", "iD"));
    EXPECT_TRUE(catalog::ColumnNamesCollide("a", "a"));
    EXPECT_FALSE(catalog::ColumnNamesCollide("a", "b"));
    EXPECT_FALSE(catalog::ColumnNamesCollide("id", "id_"));
}

// ---- CREATE TABLE ----------------------------------------------------------

TEST_F(ColumnNameCaseTest, CreateTableRefusesARepeatedNameAtTheLaterNamesByte) {
    //            0         1         2         3
    //            0123456789012345678901234567890123456
    Refused("CREATE TABLE t (id int64, a int64, a int64)",
            {"column 'a' repeats column 'a'", "ignoring case", "byte 35"});
    Refused("CREATE TABLE t (id int64, a int64, A int64)",
            {"column 'A' repeats column 'a'", "byte 35"});
    Refused("CREATE TABLE t (id int64, ID int64)", {"column 'ID' repeats column 'id'", "byte 26"});
    // Nothing was created.
    EXPECT_TRUE(IsErr(Run("SELECT * FROM t")));
    Ok("CREATE TABLE t (id int64, a int64, b int64)");
}

// The catalog's door refuses on its own, for a schema no parser built.
TEST_F(ColumnNameCaseTest, TheCatalogDoorRefusesATwinWithoutTheDispatcher) {
    catalog::Schema schema;
    for (const char* name : {"id", "v", "V"}) {
        catalog::SysColumnRow row{};
        row.pos = static_cast<std::uint32_t>(schema.columns.size());
        row.type_val = catalog::kTypeValInt64;
        row.len = 8;
        catalog::SetName(row.name, name);
        schema.columns.push_back(row);
    }
    auto oid = boot_->catalog.CreateTable(catalog::kNamespacePublic, "twins", schema,
                                          catalog::ClusteredType::kBtree);
    ASSERT_FALSE(oid.ok());
    EXPECT_EQ(oid.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(oid.status().message().find("column 'V' repeats column 'v'"), std::string::npos)
        << oid.status().message();
}

// ---- ALTER TABLE ADD COLUMN and RENAME COLUMN -----------------------------

TEST_F(ColumnNameCaseTest, AddColumnRefusesACaseTwinNamingTheColumnAsDeclared) {
    Ok("CREATE TABLE t (id int64, a int64) SPARE 2");
    //            0         1         2
    //            0123456789012345678901234
    Refused("ALTER TABLE t ADD COLUMN A int64 NULL",
            {"already has a column named 'a'", "ignoring case", "byte 25"});
    Refused("ALTER TABLE t ADD COLUMN ID int64 NULL", {"named 'id'"});
    Ok("ALTER TABLE t ADD COLUMN b int64 NULL");
    EXPECT_EQ(Run("SELECT name FROM sys.columns WHERE rel_name = 't'"), "name\\nid\\na\\nb");
}

TEST_F(ColumnNameCaseTest, RenameColumnRefusesACaseTwinButNotItsOwnCaseVariant) {
    Ok("CREATE TABLE t (id int64, a int64, b int64)");
    Ok("INSERT INTO t VALUES (10, 20)");
    //            0         1         2         3
    //            0123456789012345678901234567890123
    Refused("ALTER TABLE t RENAME COLUMN b TO A",
            {"already has a column named 'a'", "ignoring case", "byte 33"});
    Refused("ALTER TABLE t RENAME COLUMN b TO Id", {"named 'id'"});
    // A column collides only with itself: `a` may become `A`.
    EXPECT_EQ(Run("ALTER TABLE t RENAME COLUMN a TO A"), "RENAMED COLUMN t.a TO A");
    EXPECT_EQ(Run("SELECT A, b FROM t"), "A,b\\n10,20");
    // One column matching ignoring case reads as before.
    EXPECT_EQ(Run("SELECT a FROM t WHERE B = 20"), "a\\n10");
}

// A dropped column has no reachable name (BW-R6), in any case.
TEST_F(ColumnNameCaseTest, ADroppedColumnsNameIsFreeInAnyCase) {
    Ok("CREATE TABLE t (id int64, a int64 NULL, b int64) SPARE 2");
    Ok("ALTER TABLE t DROP COLUMN a");
    Ok("ALTER TABLE t ADD COLUMN A int64 NULL");
    Ok("ALTER TABLE t DROP COLUMN A");
    EXPECT_EQ(Run("ALTER TABLE t RENAME COLUMN b TO A"), "RENAMED COLUMN t.b TO A");
    EXPECT_EQ(Run("SELECT name FROM sys.columns WHERE rel_name = 't'"), "name\\nid\\nA");
}

// The in-flight-drop rule (`Catalog::RenameColumn`) holds for a case variant
// too: taken from an undecided drop, `A` would outlive the rollback beside a
// restored `a`.
TEST_F(ColumnNameCaseTest, AnUndecidedDropKeepsItsNameInEveryCaseFromARename) {
    Ok("CREATE TABLE t (id int64, a int32, b int32)");
    Session session;
    ASSERT_FALSE(IsErr(Run(session, "BEGIN")));
    EXPECT_EQ(Run(session, "ALTER TABLE t DROP COLUMN a"), "DROPPED COLUMN t.a");
    const std::string renamed = Run(session, "ALTER TABLE t RENAME COLUMN b TO A");
    EXPECT_TRUE(IsErr(renamed)) << renamed;
    EXPECT_NE(renamed.find("named 'a'"), std::string::npos) << renamed;
    ASSERT_FALSE(IsErr(Run(session, "ROLLBACK")));
    EXPECT_EQ(Run("SELECT name FROM sys.columns WHERE rel_name = 't'"), "name\\nid\\na\\nb");
}

// ---- A relation from before the rule ---------------------------------------

// The bug's reproduction: on t (id, ID, Id), `SELECT ID` returned `id`'s
// values with no error. Now every read naming the pair is refused at the
// name's byte, and a name matching one column reads as before.
TEST_F(ColumnNameCaseTest, AReadMatchingTwoColumnsIgnoringCaseIsRefusedAsAmbiguous) {
    Ok("CREATE TABLE t (id int64, qa int64, qb int64, v int64)");
    Ok("INSERT INTO t VALUES (100, 200, 7)");
    Ok("INSERT INTO t VALUES (101, 201, 8)");
    RewriteColumnName("qa", "ID");
    RewriteColumnName("qb", "Id");
    EXPECT_EQ(Run("SELECT name FROM sys.columns WHERE rel_name = 't'"),
              "name\\nid\\nID\\nId\\nv");

    //            0         1
    //            0123456789012345
    Refused("SELECT ID FROM t",
            {"column name 'ID' is ambiguous: it matches 'id' (position 0), 'ID' (position 1) "
             "and 'Id' (position 2) ignoring case",
             "byte 7"});
    Refused("SELECT id FROM t", {"is ambiguous", "byte 7"});
    Refused("SELECT t.iD FROM t", {"is ambiguous"});
    Refused("SELECT v FROM t WHERE ID = 1", {"is ambiguous", "byte 22"});
    Refused("SELECT v FROM t ORDER BY id", {"is ambiguous"});
    Refused("SELECT COUNT(*) FROM t GROUP BY ID", {"is ambiguous"});
    // A write's WHERE: the tuple-key fast paths decline the name, and the
    // compiler refuses it - nothing is written.
    Refused("UPDATE t SET v = 0 WHERE ID = 1", {"is ambiguous"});
    Refused("DELETE FROM t WHERE ID = 1", {"is ambiguous"});
    Refused("UPDATE t SET v = 0 WHERE id BETWEEN 1 AND 2", {"is ambiguous"});

    // A name matching one column reads as before, and so does `*`.
    EXPECT_EQ(Run("SELECT V FROM t WHERE v = 8"), "V\\n8");
    EXPECT_EQ(Run("SELECT * FROM t"), "id,ID,Id,v\\n1,100,200,7\\n2,101,201,8");
    // Writes resolve their own names exactly, as they always did.
    Ok("UPDATE t SET Id = 5 WHERE v = 7");
    Ok("INSERT INTO t (Id, ID, v) VALUES (2, 1, 9)");
    EXPECT_EQ(Run("SELECT * FROM t"), "id,ID,Id,v\\n1,100,5,7\\n2,101,201,8\\n3,1,2,9");
}

// PURGE has no compiler behind its fold: a tuple-key name the fold declines
// as ambiguous is refused as ambiguous, never as a form nobody built.
TEST_F(ColumnNameCaseTest, PurgeRefusesAnAmbiguousNameAsAmbiguous) {
    Ok("CREATE TABLE t (id int64, qa int64, v int64)");
    Ok("INSERT INTO t VALUES (100, 7)");
    Ok("DELETE FROM t WHERE v = 7");
    RewriteColumnName("qa", "ID");
    //            0         1
    //            01234567890123456789
    for (const char* sql : {"PURGE FROM t WHERE ID = 1", "PURGE FROM t WHERE id BETWEEN 1 AND 2"}) {
        const std::string out = Run(sql);
        EXPECT_NE(out.find("is ambiguous"), std::string::npos) << sql << " -> " << out;
        EXPECT_NE(out.find("byte 19"), std::string::npos) << sql << " -> " << out;
        EXPECT_EQ(out.find("PURGE admits only"), std::string::npos) << sql << " -> " << out;
    }
}

// A column name the catalog's 63-byte field cannot hold is refused at its
// byte, never stored truncated - where two names sharing their first 63
// bytes became one, refused by the catalog door with no byte.
TEST_F(ColumnNameCaseTest, AColumnNameLongerThanTheCatalogHoldsIsRefusedAtItsByte) {
    const std::string x63(63, 'x');
    //   "CREATE TABLE t (id int64, " is 26 bytes.
    Refused("CREATE TABLE t (id int64, " + x63 + "a int64, " + x63 + "b int64)",
            {"is longer than 63 bytes", "byte 26"});
    EXPECT_TRUE(IsErr(Run("SELECT * FROM t")));
    // 63 bytes is the longest name, and it reads back as written.
    Ok("CREATE TABLE t (id int64, " + x63 + " int64) SPARE 1");
    Ok("INSERT INTO t VALUES (5)");
    EXPECT_EQ(Run("SELECT " + x63 + " FROM t"), x63 + "\\n5");
    //   "ALTER TABLE t ADD COLUMN " is 25 bytes.
    Refused("ALTER TABLE t ADD COLUMN " + x63 + "y int64 NULL",
            {"is longer than 63 bytes", "byte 25"});
}

// A column read inside a SET value resolves as every read does: ignoring
// case, a twin refused. Before, it matched exactly, so a column named
// `CURRENT_DATE` lost to the function for `current_date` - a write of today
// where WHERE would have read the column ([quiet-wrong]).
TEST_F(ColumnNameCaseTest, ASetValueReadsAColumnIgnoringCaseAsEveryReadDoes) {
    Ok("CREATE TABLE c (id int64, CURRENT_DATE date, d date)");
    Ok("INSERT INTO c VALUES ('2001-02-03', '1999-01-01')");
    Ok("UPDATE c SET d = current_date");
    EXPECT_EQ(Run("SELECT d FROM c"), "d\\n2001-02-03");

    Ok("CREATE TABLE w (id int64, Amount int64, b int64)");
    Ok("INSERT INTO w VALUES (3, 0)");
    Ok("UPDATE w SET b = amount + 1 WHERE AMOUNT = 3");
    EXPECT_EQ(Run("SELECT b FROM w"), "b\\n4");

    Ok("CREATE TABLE t (id int64, qa int64, v int64)");
    Ok("INSERT INTO t VALUES (100, 7)");
    RewriteColumnName("qa", "ID");
    //            0         1         2
    //            012345678901234567890
    Refused("UPDATE t SET v = ID + 1", {"column name 'ID' is ambiguous", "byte 17"});
    EXPECT_EQ(Run("SELECT v FROM t WHERE v = 7"), "v\\n7");
}

// A relation from before CREATE TABLE refused a repeated name can hold two
// columns named exactly `aa`. Every resolution that matches exactly took the
// first ([quiet-wrong]); each now refuses the name as ambiguous, and RENAME
// of one copy to a fresh name is the way out.
TEST_F(ColumnNameCaseTest, AnExactTwinIsRefusedByEveryExactResolution) {
    Ok("CREATE TABLE t (id int64, aa int64, qa int64, v int64)");
    Ok("INSERT INTO t VALUES (10, 20, 7)");
    RewriteColumnName("qa", "aa");

    Refused("SELECT aa FROM t",
            {"'aa' (position 1) and 'aa' (position 2) ignoring case", "byte 7"});
    //            0         1
    //            0123456789012345
    Refused("UPDATE t SET aa = 0", {"is ambiguous", "exactly", "byte 13"});
    Refused("UPDATE t SET v = aa", {"is ambiguous"});
    Refused("INSERT INTO t (aa, v) VALUES (1, 2)", {"is ambiguous", "exactly", "byte 15"});
    Refused("CREATE INDEX t_aa ON t (aa)", {"is ambiguous", "exactly"});
    //            0         1         2
    //            012345678901234567890123456
    Refused("ALTER TABLE t DROP COLUMN aa", {"is ambiguous", "exactly", "byte 26"});
    EXPECT_EQ(Run("SELECT * FROM t"), "id,aa,aa,v\\n1,10,20,7");

    // The catalog's door refuses a drop of the pair on its own.
    auto oid = boot_->catalog.FindTableOidByName("t");
    ASSERT_TRUE(oid.ok());
    const Status dropped =
        boot_->catalog.DropColumn(oid.value(), "aa", catalog::kBootstrapXid, nullptr);
    EXPECT_EQ(dropped.code(), StatusCode::kInvalidArgument) << dropped.message();

    // RENAME takes the first copy; onto a case variant the second still
    // collides, onto a fresh name the pair is undone.
    Refused("ALTER TABLE t RENAME COLUMN aa TO AA", {"named 'aa'"});
    EXPECT_EQ(Run("ALTER TABLE t RENAME COLUMN aa TO w"), "RENAMED COLUMN t.aa TO w");
    EXPECT_EQ(Run("SELECT w, aa FROM t"), "w,aa\\n10,20");
    Ok("UPDATE t SET aa = 21");
    EXPECT_EQ(Run("SELECT * FROM t"), "id,w,aa,v\\n1,10,21,7");
}

// RENAME COLUMN's refusals carry their token's byte, and a rename onto the
// name the column already has is refused, as before a column stopped
// colliding with itself.
TEST_F(ColumnNameCaseTest, RenameColumnRefusalsCarryTheirBytes) {
    Ok("CREATE TABLE t (id int64, a int64)");
    //            0         1         2         3
    //            0123456789012345678901234567890123
    Refused("ALTER TABLE t RENAME COLUMN a TO a", {"already named 'a'", "byte 33"});
    Refused("ALTER TABLE t RENAME COLUMN z TO b", {"no column named 'z'", "byte 28"});
    Refused("ALTER TABLE t RENAME COLUMN a TO " + std::string(64, 'y'),
            {"longer than 63 bytes", "byte 33"});
    EXPECT_EQ(Run("ALTER TABLE t RENAME COLUMN a TO A"), "RENAMED COLUMN t.a TO A");
}

}  // namespace
}  // namespace kds::server
