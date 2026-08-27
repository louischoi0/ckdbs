#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"

// T1 bulk insert (docs/spec/bulkinsert.md §2, BI2-BI5, BI9; workplan
// BLK01-BLK04).
//
// The two claims everything else hangs off:
//
//   **BI2, no correctness shortcut**: an N-row statement and N single-row
//   statements leave identical relation state, because each bulk row runs
//   the full single-row pipeline in the same order - admission at row k
//   sees rows 1..k-1's reservations, so a statement that breaks a group
//   bound only in aggregate fails at exactly the row that breaks it.
//
//   **BI4, atomicity**: any per-row refusal fails the whole statement with
//   the 1-based ordinal in the message, and the scope's verdict unwinds
//   every row placed before the failure.

namespace kds::server {
namespace {

class BulkInsertTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
        // The manager is load-bearing for BI4: rollback of the placed
        // prefix replays its in-memory trail, and a bulk statement in a
        // manager-less configuration is refused upfront for exactly that
        // reason (production always builds one).
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, /*cabins=*/nullptr, &*mgr_);
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
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- Parse ----------------------------------------------------------------

TEST(BulkInsertParseTest, MultiRowValuesParses) {
    auto parsed = parser::Parse("INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')");
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    const auto& stmt = std::get<parser::InsertStmt>(parsed.value());
    ASSERT_EQ(stmt.rows.size(), 3u);
    EXPECT_EQ(stmt.rows[0].size(), 2u);
    EXPECT_EQ(stmt.rows[2][0].int_val, 3);
    EXPECT_EQ(stmt.rows[2][1].str_val, "c");
}

TEST(BulkInsertParseTest, ASingleRowIsARowsVectorOfOne) {
    auto parsed = parser::Parse("INSERT INTO t VALUES (1)");
    ASSERT_TRUE(parsed.ok());
    const auto& stmt = std::get<parser::InsertStmt>(parsed.value());
    ASSERT_EQ(stmt.rows.size(), 1u);
    EXPECT_EQ(stmt.rows[0].size(), 1u);
}

TEST(BulkInsertParseTest, ATrailingCommaIsATruthfulSyntaxError) {
    EXPECT_EQ(parser::Parse("INSERT INTO t VALUES (1),").status().code(),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(parser::Parse("INSERT INTO t VALUES (1), 2").status().code(),
              StatusCode::kInvalidArgument);
}

// ---- BI5: the fingerprint never sees the row count ------------------------

std::optional<parser::Fingerprint> FingerprintFor(std::string_view sql) {
    parser::Parser p(sql);
    auto parsed = p.Parse();
    EXPECT_TRUE(parsed.ok()) << parsed.status().message();
    return p.fingerprint();
}

TEST(BulkInsertParseTest, RowCountIsNotPartOfThePattern) {
    const auto one = FingerprintFor("INSERT INTO t VALUES (1, 'a')");
    const auto three = FingerprintFor("INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')");
    ASSERT_TRUE(one.has_value());
    ASSERT_TRUE(three.has_value());
    EXPECT_EQ(one->pattern_id, three->pattern_id)
        << "row count fragmented sys.patterns";
    // The later rows' values are still arguments: same shape, different
    // instance.
    EXPECT_NE(one->arg_hash, three->arg_hash);
}

TEST(BulkInsertParseTest, TheDifferentialPathAgreesOnAMultiRowStatement) {
    // FingerprintOf lexes without a parser; the suppression must live in
    // the one accumulator both paths share, and this is the proof.
    const auto folded = FingerprintFor("INSERT INTO t VALUES (7), (8)");
    const auto scanned = parser::FingerprintOf("INSERT INTO t VALUES (7), (8)");
    ASSERT_TRUE(folded.has_value());
    ASSERT_TRUE(scanned.has_value());
    EXPECT_EQ(folded->pattern_id, scanned->pattern_id);
    EXPECT_EQ(folded->arg_hash, scanned->arg_hash);
}

// ---- BI2: pipeline equivalence --------------------------------------------

TEST_F(BulkInsertTest, ABulkStatementEqualsItsSingleRowTwin) {
    Ok("CREATE TABLE a (id int64, v varchar, n int64)");
    Ok("CREATE TABLE b (id int64, v varchar, n int64) BTREE");

    // Same rows, one statement vs three: ids allocate identically from a
    // fresh sequence, so the replies must match byte for byte.
    Ok("INSERT INTO a VALUES ('x', 1), ('y', 2), ('z', 3)");
    Ok("INSERT INTO b VALUES ('x', 1)");
    Ok("INSERT INTO b VALUES ('y', 2)");
    Ok("INSERT INTO b VALUES ('z', 3)");

    EXPECT_EQ(Run("SELECT id, v, n FROM a"), Run("SELECT id, v, n FROM b"));
    EXPECT_EQ(Run("SELECT v FROM a WHERE id = 2"), Run("SELECT v FROM b WHERE id = 2"));
}

TEST_F(BulkInsertTest, TheBulkReplyCarriesTheCountAndTheIdRange) {
    Ok("CREATE TABLE t (id int64, n int64)");
    EXPECT_EQ(Run("INSERT INTO t VALUES (10), (20), (30)"),
              "INSERTED oid=" + std::to_string(catalog::kUserOidStart) +
                  " rows=3 first_id=1 last_id=3");
    // And the single-row reply is byte-shaped as it always was.
    const std::string one = Run("INSERT INTO t VALUES (40)");
    EXPECT_EQ(one.rfind("INSERTED oid=", 0), 0u) << one;
    EXPECT_NE(one.find(" id=4 page="), std::string::npos) << one;
}

// ---- BI4: atomicity, with the ordinal -------------------------------------

TEST_F(BulkInsertTest, AFailingRowUnwindsTheWholeStatement) {
    Ok("CREATE TABLE t (id int64, v varchar, n int64)");
    Ok("INSERT INTO t VALUES ('keep', 1)");

    // Row 2 has the wrong arity: the statement inserts nothing.
    const std::string out = Run("INSERT INTO t VALUES ('a', 2), ('bad'), ('c', 3)");
    EXPECT_EQ(out.rfind("ERR", 0), 0u);
    EXPECT_NE(out.find("(row 2)"), std::string::npos) << out;

    EXPECT_EQ(Run("SELECT v FROM t"), "v\\nkeep");
}

TEST_F(BulkInsertTest, RowsMayNameTheirOwnKeysOrNotWithinOneStatement) {
    // The inverse of what this asserted until 2026-08-25 (§4.1): naming the
    // pk is an arity, not a violation, and BI2's per-row pipeline means one
    // statement may mix the two. Row 1 takes an issued id, row 2 names 2 -
    // which is exactly what the mark had reached, so a heap relation admits
    // it - and row 3 takes the next issued one, above the named one.
    Ok("CREATE TABLE t (id int64, n int64)");
    const std::string out = Run("INSERT INTO t VALUES (1), (2, 99), (3)");
    EXPECT_NE(out.rfind("ERR", 0), 0u) << out;
    EXPECT_EQ(Run("SELECT n FROM t"), "n\\n1\\n99\\n3") << out;
}

TEST_F(BulkInsertTest, ABelowMarkKeyIsRefusedWithItsOrdinal) {
    // What is still refused on a heap relation, and what BI4 does with it:
    // the offending row is named and the whole statement inserts nothing.
    Ok("CREATE TABLE t (id int64, n int64)");
    Ok("INSERT INTO t VALUES (500, 1)");
    const std::string out = Run("INSERT INTO t VALUES (600, 2), (550, 3)");
    EXPECT_EQ(out.rfind("ERR", 0), 0u) << out;
    EXPECT_NE(out.find("must ascend"), std::string::npos) << out;
    EXPECT_NE(out.find("(row 2)"), std::string::npos) << out;
    EXPECT_EQ(Run("SELECT n FROM t"), "n\\n1") << out;
}

// BI9: a refused row burns no id (admission precedes allocation), and an
// aborted statement burns exactly the ids of the rows placed before the
// failure - accepted, documented, and now pinned.
TEST_F(BulkInsertTest, AnAbortBurnsExactlyThePlacedRowsIds) {
    Ok("CREATE TABLE t (id int64, v varchar, n int64)");

    // Rows 1-2 place (ids 1, 2) and unwind; row 3 is refused pre-id.
    const std::string out = Run("INSERT INTO t VALUES ('a', 1), ('b', 2), ('bad')");
    EXPECT_EQ(out.rfind("ERR", 0), 0u);
    EXPECT_NE(out.find("(row 3)"), std::string::npos) << out;
    EXPECT_EQ(Run("SELECT v FROM t"), "v");

    // The next row's id is 3: two burned, none reissued (K1 - an id is a
    // name forever, even a name nothing kept).
    EXPECT_NE(Run("INSERT INTO t VALUES ('c', 3)").find(" id=3 "), std::string::npos);
}

// ---- Assertion accumulation (§2.3's argument, verbatim) -------------------

TEST_F(BulkInsertTest, AGroupBoundIsEnforcedAtTheRowThatBreaksIt) {
    Ok("CREATE TABLE t (id int64, grp int64, amt int64)");
    Ok("CREATE ASSERTION cap ON t GROUP BY (grp) CHECK COUNT(*) <= 2");

    // Three rows into one group: admission at row 3 sees rows 1-2's
    // reservations and refuses - the statement inserts nothing.
    const std::string out = Run("INSERT INTO t VALUES (7, 1), (7, 2), (7, 3)");
    EXPECT_EQ(out.rfind("ERR", 0), 0u);
    EXPECT_NE(out.find("ASSERTION_VIOLATION"), std::string::npos) << out;
    EXPECT_NE(out.find("(row 3)"), std::string::npos) << out;
    EXPECT_EQ(Run("SELECT amt FROM t"), "amt");

    // The same rows minus the third fit, and then the third alone is
    // refused by plain admission - split across statements, same answer.
    Ok("INSERT INTO t VALUES (7, 1), (7, 2)");
    EXPECT_EQ(Run("INSERT INTO t VALUES (7, 3)").rfind("ERR", 0), 0u);
}

// ---- FK per row -----------------------------------------------------------

TEST_F(BulkInsertTest, AForeignKeyIsCheckedPerRowWithTheOrdinal) {
    Ok("CREATE TABLE p (id int64, v int64) BTREE");
    Ok("CREATE TABLE c (id int64, pid int64 REFERENCES p)");
    Ok("INSERT INTO p VALUES (10)");

    const std::string out = Run("INSERT INTO c VALUES (1), (99), (1)");
    EXPECT_EQ(out.rfind("ERR", 0), 0u);
    EXPECT_NE(out.find("FK_VIOLATION"), std::string::npos) << out;
    EXPECT_NE(out.find("(row 2)"), std::string::npos) << out;
    EXPECT_EQ(Run("SELECT pid FROM c"), "pid");

    Ok("INSERT INTO c VALUES (1), (1)");
}

// ---- BI3: the cap ---------------------------------------------------------

TEST(BulkInsertCapTest, AnOverCapStatementIsRefusedNamingCapAndCount) {
    storage::InMemoryPageStore store{kFirstUserPageId};
    auto boot = bootstrap::BootstrapDatabase(store, 1000);
    ASSERT_TRUE(boot.ok());
    txn::TrxIdSequence ids(boot.value().superblock);
    txn::UndoLog undo(store, /*wal=*/nullptr);
    txn::TransactionManager mgr(ids, undo, store, /*wal=*/nullptr);
    CommandDispatcher d(boot.value().superblock, boot.value().catalog, store,
                        /*log=*/nullptr, /*clock=*/nullptr, /*wal=*/nullptr,
                        wal::DurabilityClass::kGroup, exec::Budget(),
                        /*recorder=*/nullptr, /*replay_enabled=*/false,
                        /*access_statistics=*/true, /*cabins=*/nullptr, &mgr,
                        txn::IsolationLevel::kReadCommitted, /*core_id=*/0,
                        /*indexes=*/true, /*max_insert_rows=*/4);
    auto run = [&](const std::string& sql) { return d.Dispatch(sql).response; };

    ASSERT_EQ(run("CREATE TABLE t (id int64, n int64)").substr(0, 7), "CREATED");
    EXPECT_EQ(run("INSERT INTO t VALUES (1), (2), (3), (4)").rfind("ERR", 0),
              std::string::npos);

    const std::string over = run("INSERT INTO t VALUES (1), (2), (3), (4), (5)");
    EXPECT_EQ(over.rfind("ERR", 0), 0u);
    EXPECT_NE(over.find("5 rows"), std::string::npos) << over;
    EXPECT_NE(over.find("max_insert_rows (4)"), std::string::npos) << over;
    // Refused whole: nothing inserted.
    EXPECT_EQ(run("SELECT n FROM t"), "n\\n1\\n2\\n3\\n4");
}

// ---- Index maintenance rides the loop -------------------------------------

TEST_F(BulkInsertTest, IndexesSeeEveryBulkRow) {
    Ok("CREATE TABLE t (id int64, owner int64) BTREE");
    Ok("CREATE INDEX by_owner ON t (owner)");
    Ok("INSERT INTO t VALUES (1), (2), (1), (2), (1)");

    const std::string plan = Run("ANALYZE SELECT id FROM t WHERE owner = 1");
    EXPECT_NE(plan.find("index_scanned="), std::string::npos) << plan;
    EXPECT_EQ(Run("SELECT id FROM t WHERE owner = 1"), "id\\n1\\n3\\n5");
}

// ---- T3: the sorted heap fill (docs/inflight/in-progress/workplan-t3.md) -----------------------
//
// The contract is byte-identical behavior across the gate: a statement
// that takes the sorted fill and one that takes the row loop answer the
// same and leave the same relation state. The gate can only widen.

TEST_F(BulkInsertTest, TheSortedFillMatchesTheRowLoopAcrossTheGate) {
    // `a` is inside T3-2's gate (heap, int-only, nothing maintained);
    // `b` is the same schema pushed outside it by a declared Cabin.
    Ok("CREATE TABLE a (id int64, n int64, m int64)");
    Ok("CREATE TABLE b (id int64, n int64, m int64)");
    Ok("CREATE CABIN ON b(n)");

    const std::string ins = " VALUES (1, 10), (2, 20), (3, 30)";
    const std::string ra = Run("INSERT INTO a" + ins);
    const std::string rb = Run("INSERT INTO b" + ins);
    EXPECT_NE(ra.find(" rows=3 first_id=1 last_id=3"), std::string::npos) << ra;
    EXPECT_NE(rb.find(" rows=3 first_id=1 last_id=3"), std::string::npos) << rb;

    EXPECT_EQ(Run("SELECT n, m FROM a"), Run("SELECT n, m FROM b"));
    EXPECT_EQ(Run("SELECT m FROM a WHERE id = 2"), Run("SELECT m FROM b WHERE id = 2"));
}

TEST_F(BulkInsertTest, TheSortedFillSpansPagesWithExactMinKeys) {
    Ok("CREATE TABLE t (id int64, n int64)");
    std::string stmt = "INSERT INTO t VALUES (0)";
    for (int i = 1; i < 500; ++i) stmt += ", (" + std::to_string(i) + ")";
    const std::string reply = Run(stmt);
    EXPECT_NE(reply.find(" rows=500 first_id=1 last_id=500"), std::string::npos) << reply;

    EXPECT_EQ(Run("SELECT n FROM t WHERE id = 1"), "n\\n0");
    EXPECT_EQ(Run("SELECT n FROM t WHERE id = 500"), "n\\n499");
    EXPECT_EQ(Run("SELECT COUNT(*) FROM t"), "count(*)\\n500");
    // The pk range crosses at least one page boundary and stays exact.
    EXPECT_EQ(Run("SELECT COUNT(*) FROM t WHERE id BETWEEN 190 AND 210"), "count(*)\\n21");
}

TEST_F(BulkInsertTest, ASortedFillRollsBackWhole) {
    Ok("CREATE TABLE t (id int64, n int64)");
    Session session;
    ASSERT_EQ(Run(session, "BEGIN").substr(0, 5), "BEGIN");
    EXPECT_EQ(Run(session, "INSERT INTO t VALUES (1), (2), (3)").rfind("ERR", 0),
              std::string::npos);
    Run(session, "ROLLBACK");
    EXPECT_EQ(Run("SELECT n FROM t"), "n");
}

// BI9 sharpened by T3: the admission-class checks run before the range is
// allocated, so a refused statement burns *nothing* - and a failure past
// the range (a bad literal at encode) burns exactly the range.
TEST_F(BulkInsertTest, TheSortedFillBurnsItsWholeRangeAndANamedKeyDeclinesIt) {
    Ok("CREATE TABLE t (id int64, n int64)");

    // Every row omits its key, so the fill is eligible: it carves 1..3 up
    // front, the encode fails at row 2 on a literal no int column can hold -
    // past the range - and all three ids burn.
    EXPECT_EQ(Run("INSERT INTO t VALUES (1), ('x'), (3)").rfind("ERR", 0), 0u);
    EXPECT_NE(Run("INSERT INTO t VALUES (8)").find(" id=4 "), std::string::npos);

    // A row that names its key makes the statement ineligible for the fill:
    // the carved range has no place for a key the caller chose (§4.1). The
    // statement still runs, through the per-row path, which is why this is
    // ineligibility and not a refusal - and the arity case this test used to
    // check here is unreachable now, since the fill is entered only when
    // every row omits.
    EXPECT_NE(Run("INSERT INTO t VALUES (9), (100, 10)").rfind("ERR", 0), 0u);
    EXPECT_EQ(Run("SELECT n FROM t"), "n\\n8\\n9\\n10");
}

}  // namespace
}  // namespace kds::server
