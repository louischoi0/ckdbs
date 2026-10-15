#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/cabin_expr.hpp"
#include "kds/exec/declaration_relation.hpp"
#include "kds/exec/functions.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"
#include "view_text.hpp"

// AQ-S1: the expression and predicate Cabin's catalog and grammar
// (instructions/v3.0.0/workorder-aq-expression-cabins.md AQ-R1..R3). A
// Cabin's key function `F` is compiled, stored as its canonical text in
// sys.cabin_exprs, recompiled at load, and identified by that text - never
// by its `expr_id` alone (AQ-R2, `[quiet-wrong]`).

namespace kds::server {
namespace {

parser::AstValue PlusOne(std::span<const parser::AstValue* const> args,
                         const exec::StatementContext&) {
    parser::AstValue out;
    out.type = parser::ValueType::kInt;
    out.int_val = args[0]->int_val + 1;
    return out;
}

const exec::FunctionEntry kPlusOne{"plus_one", 1, exec::Purity::kImmutable,
                                   catalog::kTypeValInt64, catalog::kTypeValInt64, &PlusOne};

class CabinExprTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        cabins_.emplace();
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        Reopen(boot_->catalog);
        Ok("CREATE TABLE t (id int64, state varchar, amount int64, ts timestamp NULL, "
           "quiet timestamp NULL NO CABIN, sym varchar)");
    }

    void Reopen(catalog::Catalog& catalog) {
        dispatcher_.reset();
        dispatcher_.emplace(boot_->superblock, catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), /*recorder=*/nullptr, /*replay=*/false,
                            /*access_statistics=*/true, &*cabins_, &*mgr_);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    void Ok(const std::string& sql) {
        const std::string reply = Run(sql);
        ASSERT_NE(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
    }
    testing_view::ViewText Cabins() { return testing_view::ReadViewText(Run("SHOW CABINS")); }

    // The stored text of every sys.cabin_exprs row, in chain order.
    std::vector<std::string> StoredTexts(catalog::Catalog& catalog) {
        auto rows = exec::ScanDeclarations(catalog, store_, catalog::kSysCabinExprsTable);
        EXPECT_TRUE(rows.ok()) << rows.status().message();
        std::vector<std::string> out;
        if (rows.ok()) {
            for (const auto& row : rows.value()) out.push_back(row.source_text);
        }
        return out;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::CabinStore> cabins_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- Creation, listing, storage ------------------------------------------

TEST_F(CabinExprTest, AnExpressionCabinIsCreatedStoredAndListed) {
    const std::string created = Run("CREATE CABIN ON t (DATE(ts))");
    ASSERT_EQ(created.rfind("CREATED CABIN on=t (DATE(ts)) cabin_id=", 0), 0u) << created;

    const auto view = Cabins();
    ASSERT_EQ(view.rows.size(), 1u);
    EXPECT_EQ(view.rows[0][view.Col("shape")], "expression");
    EXPECT_EQ(view.rows[0][view.Col("key")], "DATE(ts)");
    EXPECT_EQ(view.rows[0][view.Col("column")], "NULL");

    // Positions, not names (AQ-Q2): `ts` is column 3.
    EXPECT_EQ(StoredTexts(boot_->catalog), std::vector<std::string>{"date($3)"});

    auto rows = boot_->catalog.ListCabins();
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows.value().size(), 1u);
    EXPECT_EQ(rows.value()[0].shape, catalog::kCabinShapeExpression);
    EXPECT_EQ(rows.value()[0].column_no, 0u);
    EXPECT_NE(rows.value()[0].expr_id, 0u);
}

TEST_F(CabinExprTest, APredicateCabinIsCanonicalAndCommutedIsTheSameKey) {
    ASSERT_EQ(Run("CREATE CABIN ON t (state = 'closed' AND amount > 1000)")
                  .rfind("CREATED CABIN on=t (", 0),
              0u);
    const auto view = Cabins();
    ASSERT_EQ(view.rows.size(), 1u);
    EXPECT_EQ(view.rows[0][view.Col("shape")], "predicate");
    // Conjuncts in canonical order: by their stored text.
    EXPECT_EQ(view.rows[0][view.Col("key")], "state = 'closed' AND amount > 1000");
    EXPECT_EQ(StoredTexts(boot_->catalog),
              std::vector<std::string>{"$1 = s6:closed AND $2 > i1000"});

    // The same conjunction written the other way round, and with a
    // repeated conjunct, is the same `F`.
    const std::string again = Run("CREATE CABIN ON t (amount > 1000 AND state = 'closed' AND "
                                  "amount > 01000)");
    EXPECT_EQ(again.rfind("ERR", 0), 0u) << again;
    EXPECT_NE(again.find("already has a cabin on state = 'closed' AND amount > 1000"),
              std::string::npos)
        << again;
}

TEST_F(CabinExprTest, AColumnCabinIsUnchanged) {
    const std::string created = Run("CREATE CABIN ON t(sym)");
    EXPECT_EQ(created.rfind("CREATED CABIN on=t.sym cabin_id=", 0), 0u) << created;
    EXPECT_NE(created.find(" column=5 observed=0"), std::string::npos) << created;

    const auto view = Cabins();
    ASSERT_EQ(view.rows.size(), 1u);
    EXPECT_EQ(view.rows[0][view.Col("shape")], "column");
    EXPECT_EQ(view.rows[0][view.Col("column")], "sym");
    EXPECT_EQ(view.rows[0][view.Col("key")], "sym");
    EXPECT_TRUE(StoredTexts(boot_->catalog).empty());

    auto rows = boot_->catalog.ListCabins();
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows.value().size(), 1u);
    EXPECT_EQ(rows.value()[0].shape, catalog::kCabinShapeColumn);
    EXPECT_EQ(rows.value()[0].column_no, 5u);
    EXPECT_EQ(rows.value()[0].expr_id, 0u);

    auto access = boot_->catalog.InitTableAccess(rows.value()[0].rel_oid);
    ASSERT_TRUE(access.ok());
    EXPECT_EQ(access.value()->cabin_mask, std::uint64_t{1} << 5);
    EXPECT_TRUE(access.value()->expr_cabins.empty());
}

TEST_F(CabinExprTest, ARowOfTheOldWidthIsRefused) {
    // Superblock 26 refuses every older volume (the standing order of
    // 2026-10-07), so no 28-byte row is ever read as a Cabin.
    std::array<std::byte, 28> old{};
    auto decoded = catalog::SysCabinRow::Decode(old);
    EXPECT_FALSE(decoded.ok());
}

// ---- Refusals, each at its byte -------------------------------------------

TEST_F(CabinExprTest, AFunctionThatIsNotImmutableIsRefused) {
    const std::string sql = "CREATE CABIN ON t (NOW())";
    const std::string out = Run(sql);
    EXPECT_EQ(out.rfind("ERR", 0), 0u) << out;
    EXPECT_NE(out.find("is not immutable"), std::string::npos) << out;
    EXPECT_NE(out.find("(byte " + std::to_string(sql.find("NOW")) + ")"), std::string::npos)
        << out;
}

TEST_F(CabinExprTest, AKeyReadingOnlyTheTupleKeyIsRefused) {
    exec::ScopedTestFunction plus_one(kPlusOne);
    for (const std::string sql : {"CREATE CABIN ON t (plus_one(id))", "CREATE CABIN ON t (id > 5)"}) {
        const std::string out = Run(sql);
        EXPECT_EQ(out.rfind("ERR", 0), 0u) << out;
        EXPECT_NE(out.find("reads only the tuple key"), std::string::npos) << sql << " -> " << out;
        EXPECT_NE(out.find("(byte 19)"), std::string::npos) << out;
    }
    // With another column beside it, the tuple key is an input like any other.
    Ok("CREATE CABIN ON t (id > 5 AND state = 'open')");
}

TEST_F(CabinExprTest, ASecondCabinOnOneKeyIsRefused) {
    Ok("CREATE CABIN ON t (DATE(ts))");
    const std::string out = Run("CREATE CABIN ON t (DATE(ts))");
    EXPECT_EQ(out.rfind("ERR", 0), 0u) << out;
    EXPECT_NE(out.find("already has a cabin on DATE(ts) (byte 19)"), std::string::npos) << out;
}

TEST_F(CabinExprTest, ANoCabinColumnIsRefusedAsAnInput) {
    const std::string sql = "CREATE CABIN ON t (DATE(quiet))";
    const std::string out = Run(sql);
    EXPECT_EQ(out.rfind("ERR", 0), 0u) << out;
    EXPECT_NE(out.find("'quiet' was declared NO CABIN (byte " +
                       std::to_string(sql.find("quiet")) + ")"),
              std::string::npos)
        << out;
}

TEST_F(CabinExprTest, EveryPredicateFormAqR6DoesNotAdmitIsRefused) {
    const char* const kRefused[] = {
        "CREATE CABIN ON t (state = 'a' OR amount > 1)",
        "CREATE CABIN ON t (NOT state = 'a')",
        "CREATE CABIN ON t (amount BETWEEN 1 AND 5)",
        "CREATE CABIN ON t (amount = id)",
        "CREATE CABIN ON t (state LIKE 'a%')",
        "CREATE CABIN ON t (amount IN (1, 2))",
        "CREATE CABIN ON t (amount = NULL)",
        "CREATE CABIN ON t (DATE(ts) = DATE(ts))",
    };
    for (const char* sql : kRefused) {
        const std::string out = Run(sql);
        EXPECT_EQ(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
        EXPECT_NE(out.find("(byte "), std::string::npos) << sql << " -> " << out;
    }
    EXPECT_TRUE(Cabins().rows.empty());
}

// ---- Identity: the compiled `F`, never the hash (AQ-R2) -------------------

TEST_F(CabinExprTest, TwoKeysSharingAnExprIdAreKeptApart) {
    // Literals fold out of `expr_id` (AR1 §6), so these two share one.
    Ok("CREATE CABIN ON t (state = 'open')");
    Ok("CREATE CABIN ON t (state = 'closed')");
    auto rows = boot_->catalog.ListCabins();
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows.value().size(), 2u);
    EXPECT_EQ(rows.value()[0].expr_id, rows.value()[1].expr_id);

    // DROP finds the one whose `F` it names, not the first sharing the hash.
    const std::string closed_id = std::to_string(rows.value()[1].cabin_id);
    const std::string dropped = Run("DROP CABIN ON t (state = 'closed')");
    EXPECT_EQ(dropped, "DROPPED CABIN on=t cabin_id=" + closed_id) << dropped;
    const auto view = Cabins();
    ASSERT_EQ(view.rows.size(), 1u);
    EXPECT_EQ(view.rows[0][view.Col("key")], "state = 'open'");
    EXPECT_EQ(StoredTexts(boot_->catalog), std::vector<std::string>{"$1 = s4:open"});

    // A key nobody declared is NotFound, though its hash is taken.
    const std::string none = Run("DROP CABIN ON t (state = 'shut')");
    EXPECT_EQ(none.rfind("ERR", 0), 0u) << none;
}

TEST_F(CabinExprTest, AStoredTextThatIsNotCanonicalIsCorruption) {
    auto schema = boot_->catalog.InitTableAccess(boot_->catalog.FindTableOidByName("t").value());
    ASSERT_TRUE(schema.ok());
    const catalog::Schema& s = schema.value()->schema;
    EXPECT_TRUE(exec::DecodeCabinExpr(catalog::kCabinShapeExpression, "date($3)", s).ok());
    for (const char* text : {"date($2)",            // not a timestamp
                             "date($9)",            // no such column
                             "now()",               // not immutable
                             "$1 = s6:closed",      // a predicate as an expression
                             "date($3) "}) {        // trailing bytes
        EXPECT_FALSE(exec::DecodeCabinExpr(catalog::kCabinShapeExpression, text, s).ok()) << text;
    }
    EXPECT_TRUE(exec::DecodeCabinExpr(catalog::kCabinShapePredicate,
                                      "$1 = s6:closed AND $2 > i1000", s)
                    .ok());
    EXPECT_FALSE(exec::DecodeCabinExpr(catalog::kCabinShapePredicate,
                                       "$2 > i1000 AND $1 = s6:closed", s)
                     .ok());  // conjuncts out of canonical order
    EXPECT_FALSE(
        exec::DecodeCabinExpr(catalog::kCabinShapePredicate, "$1 = s9:closed", s).ok());
}

// Every literal kind a predicate can hold survives its text: written,
// recompiled by a fresh catalog, and still the same `F`. A negative integer
// once kept its signed digit text, which the reader refused - and the
// relation then failed every fill.
TEST_F(CabinExprTest, EveryLiteralKindRoundTripsThroughItsText) {
    Ok("CREATE TABLE lit (id int64, i int64, u uint64, d decimal(10, 2), w decimal(30, 4), "
       "s varchar, dt date, n int64 NULL)");
    const char* const kKeys[] = {
        "i > -5",
        "i = -0 AND s = 'x'",
        "u = 18446744073709551615",
        "d < -12.30",
        "w >= 123456789012345678901.5",
        "s = 'a AND b'",
        "dt = '1969-12-31'",
        "n IS NULL AND i > 1",
        "n IS NOT NULL AND i > 1",
    };
    for (const char* key : kKeys) Ok(std::string("CREATE CABIN ON lit (") + key + ")");

    catalog::Catalog reopened(store_);
    Reopen(reopened);
    const auto view = Cabins();
    ASSERT_EQ(view.rows.size(), std::size(kKeys)) << Run("SHOW CABINS");
    // The same keys, respelled, are the Cabins already there.
    for (const char* key : {"i > -0005", "i = 0 AND s = 'x'", "u = 018446744073709551615",
                            "d < -12.3", "s = 'a AND b'", "dt = '1969-12-31'"}) {
        const std::string out = Run(std::string("CREATE CABIN ON lit (") + key + ")");
        EXPECT_NE(out.find("already has a cabin"), std::string::npos) << key << " -> " << out;
    }
}

TEST_F(CabinExprTest, AKeyReadingNoColumnIsRefused) {
    const exec::FunctionEntry kSeven{"seven", 0, exec::Purity::kImmutable, 0,
                                     catalog::kTypeValInt64,
                                     [](std::span<const parser::AstValue* const>,
                                        const exec::StatementContext&) {
                                         parser::AstValue v;
                                         v.type = parser::ValueType::kInt;
                                         v.int_val = 7;
                                         return v;
                                     }};
    exec::ScopedTestFunction seven(kSeven);
    const std::string out = Run("CREATE CABIN ON t (seven())");
    EXPECT_NE(out.find("reads no column (byte 19)"), std::string::npos) << out;
}

// ---- Lifecycle --------------------------------------------------------------

TEST_F(CabinExprTest, AReopenedCatalogRecompilesEveryKeyFromItsText) {
    Ok("CREATE CABIN ON t (DATE(ts))");
    Ok("CREATE CABIN ON t (state = 'closed' AND amount > 1000)");

    catalog::Catalog reopened(store_);
    Reopen(reopened);
    const auto view = Cabins();
    ASSERT_EQ(view.rows.size(), 2u);
    EXPECT_EQ(view.Find("shape", "expression", "key"), "DATE(ts)");
    EXPECT_EQ(view.Find("shape", "predicate", "key"), "state = 'closed' AND amount > 1000");

    auto access = reopened.InitTableAccess(reopened.FindTableOidByName("t").value());
    ASSERT_TRUE(access.ok());
    ASSERT_EQ(access.value()->expr_cabins.size(), 2u);
    for (const auto& ref : access.value()->expr_cabins) {
        ASSERT_NE(ref.f, nullptr);
        EXPECT_EQ(ref.f->expr_id, ref.expr_id);
    }
}

TEST_F(CabinExprTest, ARenamedInputKeepsItsCabin) {
    Ok("CREATE CABIN ON t (DATE(ts))");
    Ok("ALTER TABLE t RENAME COLUMN ts TO stamp");
    EXPECT_EQ(Cabins().rows[0][Cabins().Col("key")], "DATE(stamp)");
    // The renamed spelling names the same `F`.
    const std::string again = Run("CREATE CABIN ON t (DATE(stamp))");
    EXPECT_EQ(again.rfind("ERR", 0), 0u) << again;
}

TEST_F(CabinExprTest, DroppingAnInputColumnIsRestricted) {
    Ok("CREATE CABIN ON t (state = 'closed' AND amount > 1000)");
    const std::string out = Run("ALTER TABLE t DROP COLUMN amount");
    EXPECT_EQ(out.rfind("ERR", 0), 0u) << out;
    EXPECT_NE(out.find("a Cabin's key reads it"), std::string::npos) << out;
    Ok("ALTER TABLE t DROP COLUMN sym");
}

TEST_F(CabinExprTest, DroppingTheRelationTakesTheText) {
    Ok("CREATE CABIN ON t (DATE(ts))");
    ASSERT_EQ(StoredTexts(boot_->catalog).size(), 1u);
    Ok("DROP TABLE t");
    EXPECT_TRUE(StoredTexts(boot_->catalog).empty());
    EXPECT_TRUE(Cabins().rows.empty());
}

}  // namespace
}  // namespace kds::server

// ---- AQ-S2: the expression Cabin's witness, serve and record ---------------
//
// Every served read is compared with the walk that answers the same rows
// with no Cabin in reach: the half-open `ts` range of the day, a plain filter
// conjunct no Cabin serves.

namespace kds::server {
namespace {

class CabinExprServeTest : public CabinExprTest {
protected:
    void SetUp() override {
        CabinExprTest::SetUp();
        Ok("INSERT INTO t VALUES ('a', 1, '2026-10-01 00:00:00', NULL, 'x')");
        Ok("INSERT INTO t VALUES ('b', 2, '2026-10-01 23:59:59.999999', NULL, 'y')");
        Ok("INSERT INTO t VALUES ('c', 3, '2026-10-02 00:00:00', NULL, 'z')");
        Ok("INSERT INTO t VALUES ('d', 4, NULL, NULL, 'w')");
    }

    static constexpr const char* kOct1 =
        "SELECT id, amount, ts FROM t WHERE DATE(ts) = '2026-10-01'";
    std::string Walked() {
        return Run("SELECT id, amount, ts FROM t WHERE ts >= '2026-10-01 00:00:00' AND "
                   "ts < '2026-10-02 00:00:00'");
    }
    // The reply and whether a Cabin served it.
    std::pair<std::string, bool> Served(const std::string& sql) {
        const std::string reply = Run(sql);
        const std::string analyzed = Run("ANALYZE " + sql);
        return {reply, analyzed.find("cabin_hits=1") != std::string::npos};
    }
    std::string Entries() {
        const auto view = Cabins();
        return view.rows.empty() ? "" : view.rows[0][view.Col("entries")];
    }
    // Declares the Cabin and observes 2026-10-01: a declared Cabin records on
    // its first probe (`cabin.md` §8.1, n=1).
    void Observe() {
        Ok("CREATE CABIN ON t (DATE(ts))");
        ASSERT_EQ(Run(kOct1), Walked());
    }
};

TEST_F(CabinExprServeTest, AnObservedDayIsServedAndAnswersAsTheWalkDoes) {
    // Before the declaration the conjunct walks: the step is no Cabin probe.
    const std::string before = Run(std::string("ANALYZE ") + kOct1);
    EXPECT_EQ(before.find("CabinProbe"), std::string::npos) << before;
    const std::string walked = Run(kOct1);
    ASSERT_EQ(walked, Walked());

    Observe();
    const auto [reply, served] = Served(kOct1);
    EXPECT_TRUE(served) << Run(std::string("ANALYZE ") + kOct1);
    EXPECT_EQ(reply, walked);
    EXPECT_EQ(Entries(), "2");
}

TEST_F(CabinExprServeTest, AnInsertIntoAnObservedDayIsWitnessed) {
    Observe();
    Ok("INSERT INTO t VALUES ('e', 5, '2026-10-01 12:00:00', NULL, 'v')");
    const auto [reply, served] = Served(kOct1);
    EXPECT_TRUE(served);
    EXPECT_EQ(reply, Walked());
    EXPECT_NE(reply.find("12:00:00"), std::string::npos) << reply;
}

// The two other INSERT shapes reach the same witness: a multi-row VALUES,
// and a column list that leaves a nullable input out (NULL: no key).
TEST_F(CabinExprServeTest, MultiRowAndColumnListInsertsAreWitnessed) {
    Observe();
    Ok("INSERT INTO t VALUES ('e', 5, '2026-10-01 10:00:00', NULL, 'v'), "
       "('f', 6, '2026-10-03 10:00:00', NULL, 'u'), ('g', 7, '2026-10-01 11:00:00', NULL, 't')");
    Ok("INSERT INTO t (sym, ts, state, amount) VALUES ('s', '2026-10-01 13:00:00', 'h', 8)");
    Ok("INSERT INTO t (sym, state, amount) VALUES ('r', 'i', 9)");
    const auto [reply, served] = Served(kOct1);
    EXPECT_TRUE(served);
    EXPECT_EQ(reply, Walked());
    EXPECT_EQ(Entries(), "5");
}

TEST_F(CabinExprServeTest, AnUpdateMovingTheInputIntoAnObservedDayIsWitnessed) {
    Observe();
    Ok("UPDATE t SET ts = '2026-10-01 08:00:00' WHERE id = 3");
    Ok("UPDATE t SET ts = '2026-10-01 09:00:00' WHERE id = 4");  // from NULL
    const auto [reply, served] = Served(kOct1);
    EXPECT_TRUE(served);
    EXPECT_EQ(reply, Walked());
    EXPECT_EQ(Entries(), "4");
}

TEST_F(CabinExprServeTest, AnUpdateMovingTheInputOutIsSubtractedAtTheServe) {
    Observe();
    Ok("UPDATE t SET ts = '2026-10-05 00:00:00' WHERE id = 1");
    const auto [reply, served] = Served(kOct1);
    EXPECT_TRUE(served);
    EXPECT_EQ(reply, Walked());
    EXPECT_EQ(reply.find("2026-10-05"), std::string::npos) << reply;
}

TEST_F(CabinExprServeTest, AnUpdateOfAColumnFNeverReadsAppendsNothing) {
    Observe();
    Ok("UPDATE t SET amount = 99 WHERE id = 1");
    Ok("UPDATE t SET ts = '2026-10-01 00:00:00' WHERE id = 1");  // to the value it holds
    EXPECT_EQ(Entries(), "2");
    const auto [reply, served] = Served(kOct1);
    EXPECT_TRUE(served);
    EXPECT_EQ(reply, Walked());
}

// A surplus entry - a row of another day filed under 2026-10-01 - is what the
// function conjunct, left in `fn_residual`, subtracts: moved out of it when
// served, the reply would carry 2026-10-02's row.
TEST_F(CabinExprServeTest, ASurplusEntryIsReCheckedByTheConjunct) {
    Observe();
    const auto oid = boot_->catalog.FindTableOidByName("t");
    ASSERT_TRUE(oid.ok());
    auto access = boot_->catalog.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    ASSERT_EQ(access.value()->expr_cabins.size(), 1u);
    parser::AstValue day;
    day.type = parser::ValueType::kInt;
    day.int_val = 20727;  // 2026-10-01 in epoch days
    auto key = stats::MakeCabinKey(access.value()->expr_cabins[0].id, day);
    ASSERT_TRUE(key.has_value());
    stats::CabinEntry surplus;
    surplus.tuple_key = 3;  // ts 2026-10-02
    cabins_->NoteWrite(*key, surplus);
    EXPECT_EQ(Entries(), "3");

    const auto [reply, served] = Served(kOct1);
    EXPECT_TRUE(served);
    EXPECT_EQ(reply, Walked());
}

// AQ-R2 at the probe: `DATE(b)` is not `DATE(a)`'s key, though both call one
// function - a probe matched by function alone would serve one column's day
// for the other's.
TEST_F(CabinExprServeTest, AnotherColumnsCallIsNotThisCabinsKey) {
    Ok("CREATE TABLE two (id int64, a timestamp, b timestamp)");
    Ok("INSERT INTO two VALUES ('2026-10-01 01:00:00', '2026-10-09 01:00:00')");
    Ok("INSERT INTO two VALUES ('2026-10-09 01:00:00', '2026-10-01 01:00:00')");
    Ok("CREATE CABIN ON two (DATE(a))");
    ASSERT_EQ(Run("SELECT id FROM two WHERE DATE(a) = '2026-10-01'"), "id\\n1");
    const auto [by_b, served] = Served("SELECT id FROM two WHERE DATE(b) = '2026-10-01'");
    EXPECT_FALSE(served);
    EXPECT_EQ(by_b, "id\\n2");
    EXPECT_TRUE(Served("SELECT id FROM two WHERE DATE(a) = '2026-10-01'").second);
}

// The column shape and the expression shape on one relation: each is served
// for its own conjunct.
TEST_F(CabinExprServeTest, AColumnCabinBesideItIsUnchanged) {
    Ok("CREATE CABIN ON t(sym)");
    Observe();
    ASSERT_EQ(Run("SELECT id FROM t WHERE sym = 'y'"), "id\\n2");
    const auto [reply, served] = Served("SELECT id FROM t WHERE sym = 'y'");
    EXPECT_TRUE(served);
    EXPECT_EQ(reply, "id\\n2");
    EXPECT_TRUE(Served(kOct1).second);
}

}  // namespace
}  // namespace kds::server

// ---- AQ-S3: the predicate shape --------------------------------------------
//
// `p = (state = 'closed' AND amount > 1000)` on `t`; `twin` holds the same
// rows and every write, and no Cabin. Every reply on `t` is compared with the
// same statement on `twin` - the walk - and a served one must equal it.

namespace kds::server {
namespace {

class CabinPredicateTest : public CabinExprTest {
protected:
    void SetUp() override {
        CabinExprTest::SetUp();
        Ok("CREATE TABLE twin (id int64, state varchar, amount int64, ts timestamp NULL, "
           "quiet timestamp NULL NO CABIN, sym varchar)");
        Both("INSERT INTO t VALUES ('closed', 500, NULL, NULL, 'a')");
        Both("INSERT INTO t VALUES ('closed', 1500, NULL, NULL, 'b')");
        Both("INSERT INTO t VALUES ('open', 2000, NULL, NULL, 'c')");
        Both("INSERT INTO t VALUES ('closed', 1001, NULL, NULL, 'd')");
        Both("INSERT INTO t VALUES ('closed', 1000, NULL, NULL, 'e')");
        Ok("CREATE CABIN ON t (state = 'closed' AND amount > 1000)");
    }

    // `sql` on `t`, then on `twin`.
    void Both(const std::string& sql) {
        Ok(sql);
        Ok(OnTwin(sql));
    }
    static std::string OnTwin(std::string sql) {
        const std::string original = sql;
        for (auto at = sql.find(" t "); at != std::string::npos; at = sql.find(" t ", at + 1)) {
            sql.replace(at, 3, " twin ");
        }
        // A statement the rewrite missed would compare `t` with itself.
        EXPECT_NE(sql, original) << "no ' t ' to rewrite in: " << original;
        return sql;
    }
    // Whether `sql` was served - read off one execution's ANALYZE - and,
    // run once more, that it answers as the walk does.
    bool ServedAsWalked(const std::string& sql) {
        const bool served = Run("ANALYZE " + sql).find("cabin_hits=1") != std::string::npos;
        EXPECT_EQ(Run(sql), Run(OnTwin(sql))) << sql;
        return served;
    }
    std::string Entries() {
        const auto view = Cabins();
        return view.rows.empty() ? "" : view.rows[0][view.Col("entries")];
    }
};

constexpr const char* kExact = "SELECT * FROM t WHERE state = 'closed' AND amount > 1000";

TEST_F(CabinPredicateTest, ServedWhenTheWhereCarriesEveryConjunctExactly) {
    EXPECT_FALSE(ServedAsWalked(kExact));  // declared: the first probe records
    EXPECT_TRUE(ServedAsWalked(kExact));
    EXPECT_EQ(Entries(), "2");
    // Either order, and beside other conjuncts.
    EXPECT_TRUE(ServedAsWalked("SELECT * FROM t WHERE amount > 1000 AND state = 'closed'"));
    EXPECT_TRUE(ServedAsWalked(
        "SELECT id FROM t WHERE sym < 'x' AND amount > 1000 AND state = 'closed' AND id > 1"));
}

TEST_F(CabinPredicateTest, NotServedWithoutAnExactMatchOfEveryConjunct) {
    ASSERT_FALSE(ServedAsWalked(kExact));
    const char* const kNot[] = {
        "SELECT * FROM t WHERE state = 'closed'",                         // one missing
        "SELECT * FROM t WHERE amount > 1000",                            // the other
        "SELECT * FROM t WHERE state = 'closed' AND amount > 999",        // another literal
        "SELECT * FROM t WHERE state = 'closed' AND amount >= 1000",      // >= for >
        "SELECT * FROM t WHERE state = 'closed' AND amount >= 1001",      // equivalent, not equal
        "SELECT * FROM t WHERE state = 'closed' AND amount BETWEEN 1000 AND 5000",
        "SELECT * FROM t WHERE state = 'closed' AND (amount > 1000 OR sym = 'a')",
        "SELECT * FROM t WHERE state = 'open' AND amount > 1000",
    };
    for (const char* sql : kNot) EXPECT_FALSE(ServedAsWalked(sql)) << sql;
}

TEST_F(CabinPredicateTest, EveryWriteIsWitnessed) {
    ASSERT_FALSE(ServedAsWalked(kExact));
    const char* const kWrites[] = {
        "INSERT INTO t VALUES ('closed', 3000, NULL, NULL, 'f')",          // into p
        "INSERT INTO t VALUES ('open', 3000, NULL, NULL, 'g')",            // not into p
        "UPDATE t SET state = 'closed' WHERE id = 3",                       // into p
        "UPDATE t SET amount = 1001 WHERE id = 5",                          // into p
        "UPDATE t SET amount = 10 WHERE id = 2",                            // out of p
        "UPDATE t SET sym = 'zz' WHERE id = 4",                             // no input moved
        "DELETE FROM t WHERE id = 4",
        "INSERT INTO t VALUES ('closed', 4000, NULL, NULL, 'j'), ('open', 1, NULL, NULL, 'k')",
        "INSERT INTO t (sym, amount, state) VALUES ('l', 5000, 'closed')",
    };
    for (const char* write : kWrites) {
        Both(write);
        EXPECT_TRUE(ServedAsWalked(kExact)) << "after " << write;
    }
}

TEST_F(CabinPredicateTest, AnUpdateOfAColumnPNeverReadsAppendsNothing) {
    ASSERT_FALSE(ServedAsWalked(kExact));
    Both("UPDATE t SET sym = 'q' WHERE id = 2");
    Both("UPDATE t SET amount = 1500 WHERE id = 2");  // to the value it holds
    EXPECT_EQ(Entries(), "2");
    EXPECT_TRUE(ServedAsWalked(kExact));
}

// Two predicates differing only in a literal share an `expr_id` (literals
// fold out of it, AR1 §6): each statement is served from its own Cabin's set,
// never from the other's - the serve-time half of AQ-R2.
TEST_F(CabinPredicateTest, TwoKeysSharingAnExprIdServeTheirOwnSets) {
    Ok("CREATE CABIN ON t (state = 'open' AND amount > 1000)");
    auto rows = boot_->catalog.ListCabins();
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows.value().size(), 2u);
    ASSERT_EQ(rows.value()[0].expr_id, rows.value()[1].expr_id);

    const std::string open = "SELECT * FROM t WHERE state = 'open' AND amount > 1000";
    for (int round = 0; round < 3; ++round) {
        ServedAsWalked(kExact);
        ServedAsWalked(open);
    }
    EXPECT_TRUE(ServedAsWalked(kExact));
    EXPECT_TRUE(ServedAsWalked(open));
    Both("INSERT INTO t VALUES ('open', 1200, NULL, NULL, 'h')");
    EXPECT_TRUE(ServedAsWalked(open));
    EXPECT_TRUE(ServedAsWalked(kExact));
}

// A function conjunct is a conjunct of `p` like any other.
TEST_F(CabinPredicateTest, AnImmutableCallsComparisonIsAConjunct) {
    Both("UPDATE t SET ts = '2026-10-01 05:00:00' WHERE id = 2");
    Both("UPDATE t SET ts = '2026-10-02 05:00:00' WHERE id = 4");
    Ok("CREATE CABIN ON t (DATE(ts) = '2026-10-01' AND state = 'closed')");
    const std::string sql = "SELECT id FROM t WHERE state = 'closed' AND DATE(ts) = '2026-10-01'";
    EXPECT_FALSE(ServedAsWalked(sql));
    EXPECT_TRUE(ServedAsWalked(sql));
    Both("UPDATE t SET ts = '2026-10-01 07:00:00' WHERE id = 4");
    EXPECT_TRUE(ServedAsWalked(sql));
    EXPECT_FALSE(ServedAsWalked(
        "SELECT id FROM t WHERE state = 'closed' AND DATE(ts) = '2026-10-02'"));
}

// A statement's BETWEEN lowers to its two bounds exactly, so it carries the
// conjuncts a Cabin declared as those bounds (AQ-Q15) - and is served.
TEST_F(CabinPredicateTest, ABetweensTwoBoundsAreItsConjuncts) {
    Ok("CREATE CABIN ON t (amount >= 1000 AND amount <= 1500)");
    const std::string sql = "SELECT id FROM t WHERE amount BETWEEN 1000 AND 1500";
    EXPECT_FALSE(ServedAsWalked(sql));
    EXPECT_TRUE(ServedAsWalked(sql));
    EXPECT_FALSE(ServedAsWalked("SELECT id FROM t WHERE amount BETWEEN 1000 AND 1501"));
}

// Leading zeros are one value, and IS [NOT] NULL is a conjunct like any other.
TEST_F(CabinPredicateTest, SpellingsOfOneLiteralAndNullTestsMatch) {
    EXPECT_FALSE(ServedAsWalked("SELECT id FROM t WHERE state = 'closed' AND amount > 01000"));
    EXPECT_TRUE(ServedAsWalked(kExact));
    Ok("CREATE CABIN ON t (ts IS NULL AND state = 'open')");
    const std::string sql = "SELECT id FROM t WHERE state = 'open' AND ts IS NULL";
    EXPECT_FALSE(ServedAsWalked(sql));
    EXPECT_TRUE(ServedAsWalked(sql));
    EXPECT_FALSE(ServedAsWalked("SELECT id FROM t WHERE state = 'open' AND ts IS NOT NULL"));
}

// A conjunct on an **enclosing** row's columns is not the step's own, though
// it names the same positions: served from `p`'s set, the inner step would
// answer as though its own row passed `p`. Matched only on own columns, the
// step is no Cabin probe and the Cabin changes no answer. (The answer itself
// is wrong with or without a Cabin - an inner build buckets the outer row's
// conjuncts once: `docs/inflight/bugs/an-inner-build-buckets-outer-only-
// conjuncts-for-the-first-outer-row.md` - so this cell compares with the
// Cabin dropped, not with the right answer.)
TEST_F(CabinPredicateTest, AnEnclosingRowsConjunctIsNotTheStepsOwn) {
    ASSERT_FALSE(ServedAsWalked(kExact));
    const std::string sql =
        "SELECT id FROM twin WHERE EXISTS (SELECT id FROM t WHERE twin.state = 'closed' AND "
        "twin.amount > 1000 AND t.sym = twin.sym)";
    const std::string analyzed = Run("ANALYZE " + sql);
    EXPECT_EQ(analyzed.find("CabinProbe"), std::string::npos) << analyzed;
    const std::string reply = Run(sql);
    Ok("DROP CABIN ON t (state = 'closed' AND amount > 1000)");
    EXPECT_EQ(reply, Run(sql)) << "the Cabin changed the answer";
}

}  // namespace
}  // namespace kds::server

// ---- AQ-S4: cover serving for DATE ------------------------------------------
//
// `ts` bounded on both sides is served from `DATE(ts)`'s sets for every day
// the bounds touch, when all are observed (AQ-R8). Every reply is compared
// with the answer the relation gave before any Cabin was declared.

namespace kds::server {
namespace {

class CabinCoverTest : public CabinExprTest {
protected:
    void SetUp() override {
        CabinExprTest::SetUp();
        Ok("INSERT INTO t VALUES ('a', 1, '2026-10-01 00:00:00', NULL, 'x')");
        Ok("INSERT INTO t VALUES ('b', 2, '2026-10-01 23:59:59.999999', NULL, 'y')");
        Ok("INSERT INTO t VALUES ('c', 3, '2026-10-02 00:00:00', NULL, 'z')");
        Ok("INSERT INTO t VALUES ('d', 4, NULL, NULL, 'w')");
        Ok("INSERT INTO t VALUES ('e', 5, '1969-12-31 23:00:00', NULL, 'v')");
        Ok("INSERT INTO t VALUES ('f', 6, '2026-10-03 12:00:00', NULL, 'u')");
    }

    // Records every statement's answer with no Cabin, then declares one.
    void Declare(const std::vector<std::string>& statements) {
        for (const std::string& sql : statements) walked_[sql] = Run(sql);
        Ok("CREATE CABIN ON t (DATE(ts))");
    }
    void ObserveDay(const std::string& day) {
        Ok("SELECT id FROM t WHERE DATE(ts) = '" + day + "'");
    }
    // Whether `sql` was served, after checking it answers as before.
    bool Served(const std::string& sql) {
        const bool served = Run("ANALYZE " + sql).find("cabin_hits=1") != std::string::npos;
        EXPECT_EQ(Run(sql), walked_.at(sql)) << sql;
        return served;
    }

    std::map<std::string, std::string> walked_;
};

const std::string kOneDay =
    "SELECT id, ts FROM t WHERE ts BETWEEN '2026-10-01 00:00:00' AND '2026-10-01 23:59:59.999999'";
const std::string kTwoDays =
    "SELECT id, ts FROM t WHERE ts >= '2026-10-01 12:00:00' AND ts <= '2026-10-02 00:00:00'";
const std::string kManyDays =
    "SELECT id, ts FROM t WHERE ts > '2026-10-01 00:00:00' AND ts < '2026-10-03 23:00:00'";
const std::string kPastObserved =
    "SELECT id, ts FROM t WHERE ts BETWEEN '2026-10-01 00:00:00' AND '2026-10-04 00:00:00'";

TEST_F(CabinCoverTest, OneTwoAndManyObservedDaysAreServed) {
    Declare({kOneDay, kTwoDays, kManyDays});
    for (const char* day : {"2026-10-01", "2026-10-02", "2026-10-03"}) ObserveDay(day);
    EXPECT_TRUE(Served(kOneDay));
    EXPECT_TRUE(Served(kTwoDays));
    EXPECT_TRUE(Served(kManyDays));
    const std::string plan = Run("ANALYZE " + kManyDays);
    EXPECT_NE(plan.find("cover=20727..20729"), std::string::npos) << plan;
}

TEST_F(CabinCoverTest, OneUnobservedDaySendsTheStatementToTheWalk) {
    Declare({kPastObserved, kTwoDays});
    for (const char* day : {"2026-10-01", "2026-10-02", "2026-10-03"}) ObserveDay(day);
    EXPECT_FALSE(Served(kPastObserved));  // 2026-10-04 was never observed
    ObserveDay("2026-10-04");
    EXPECT_TRUE(Served(kPastObserved));
    // A write after observation is in its day's set, and so in the cover:
    // compared with the walk's answer written out, not with another cover.
    Ok("INSERT INTO t VALUES ('g', 7, '2026-10-01 18:00:00', NULL, 't')");
    walked_[kTwoDays] =
        "id,ts\\n2,2026-10-01 23:59:59.999999\\n3,2026-10-02 00:00:00\\n7,2026-10-01 18:00:00";
    EXPECT_TRUE(Served(kTwoDays));
}

// The boundary: an open bound at midnight keeps its own day in the cover - a
// superset the bound itself trims - and a closed one at midnight is that day.
TEST_F(CabinCoverTest, OpenAndClosedBoundsAtMidnight) {
    const std::string open =
        "SELECT id FROM t WHERE ts > '2026-10-01 23:59:59.999999' AND ts < '2026-10-03 00:00:00'";
    const std::string closed =
        "SELECT id FROM t WHERE ts >= '2026-10-02 00:00:00' AND ts <= '2026-10-02 00:00:00'";
    const std::string last_instant =
        "SELECT id FROM t WHERE ts >= '2026-10-01 23:59:59.999999' AND "
        "ts <= '2026-10-01 23:59:59.999999'";
    Declare({open, closed, last_instant});
    for (const char* day : {"2026-10-01", "2026-10-02", "2026-10-03"}) ObserveDay(day);
    EXPECT_TRUE(Served(open));
    EXPECT_TRUE(Served(closed));
    EXPECT_TRUE(Served(last_instant));
    EXPECT_EQ(walked_.at(open), "id\\n3");
    EXPECT_EQ(walked_.at(last_instant), "id\\n2");
}

TEST_F(CabinCoverTest, ADayBeforeTheEpochIsCovered) {
    const std::string sql =
        "SELECT id, ts FROM t WHERE ts BETWEEN '1969-12-31 00:00:00' AND '1969-12-31 23:59:59'";
    Declare({sql});
    ObserveDay("1969-12-31");
    EXPECT_TRUE(Served(sql));
    EXPECT_NE(walked_.at(sql).find("1969-12-31 23:00:00"), std::string::npos) << walked_.at(sql);
}

// A cover wider than `cabin_max_values` could never be all observed, so it
// walks without asking (AQ-R8's cap reuses the setting, adding no name).
TEST_F(CabinCoverTest, ACoverWiderThanTheValueCapWalks) {
    stats::CabinLimits limits;
    limits.max_values = 2;
    cabins_.emplace(limits);
    Reopen(boot_->catalog);
    Declare({kOneDay, kTwoDays, kManyDays});
    ObserveDay("2026-10-01");
    ObserveDay("2026-10-02");
    EXPECT_TRUE(Served(kTwoDays));
    EXPECT_FALSE(Served(kManyDays));  // three days against a cap of two
}

// What the cover arm takes and what it leaves: one bound is no cover, an
// inverted pair holds no row, an equality is a one-day cover, and an index on
// the argument outranks the Cabin (`step_compiler.cpp`'s ladder).
TEST_F(CabinCoverTest, OnlyATwoSidedRangeIsCoveredAndAnIndexWins) {
    const std::string one_sided = "SELECT id FROM t WHERE ts >= '2026-10-01 00:00:00'";
    const std::string inverted =
        "SELECT id FROM t WHERE ts >= '2026-10-02 00:00:00' AND ts <= '2026-10-01 00:00:00'";
    const std::string equality = "SELECT id FROM t WHERE ts = '2026-10-02 00:00:00'";
    Declare({one_sided, inverted, equality});
    for (const char* day : {"2026-10-01", "2026-10-02", "2026-10-03"}) ObserveDay(day);
    EXPECT_FALSE(Served(one_sided));
    EXPECT_FALSE(Served(inverted));
    EXPECT_EQ(walked_.at(inverted), "id");
    EXPECT_TRUE(Served(equality));
    EXPECT_NE(Run("ANALYZE " + equality).find("cover=20728..20728"), std::string::npos);

    // An index on the argument outranks the Cabin. A nullable column takes
    // no index (null.md D2), so this is a NOT NULL one beside its own Cabin.
    Ok("CREATE TABLE ix (id int64, at timestamp)");
    Ok("INSERT INTO ix VALUES ('2026-10-01 05:00:00')");
    Ok("CREATE CABIN ON ix (DATE(at))");
    Ok("SELECT id FROM ix WHERE DATE(at) = '2026-10-01'");
    const std::string ranged =
        "SELECT id FROM ix WHERE at BETWEEN '2026-10-01 00:00:00' AND '2026-10-01 23:59:59'";
    ASSERT_NE(Run("ANALYZE " + ranged).find("cover="), std::string::npos);
    Ok("CREATE INDEX ix_by_at ON ix (at)");
    const std::string analyzed = Run("ANALYZE " + ranged);
    EXPECT_EQ(analyzed.find("CabinProbe"), std::string::npos) << analyzed;
    EXPECT_EQ(Run(ranged), "id\\n1");
}

TEST_F(CabinCoverTest, AnUnobservedDayCountsAMiss) {
    Declare({kTwoDays});
    ObserveDay("2026-10-01");
    const std::string analyzed = Run("ANALYZE " + kTwoDays);
    EXPECT_NE(analyzed.find("cabin_misses=1"), std::string::npos) << analyzed;
    EXPECT_EQ(Run(kTwoDays), walked_.at(kTwoDays));
}

}  // namespace
}  // namespace kds::server
