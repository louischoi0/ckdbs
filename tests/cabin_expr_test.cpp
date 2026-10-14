#include <gtest/gtest.h>

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
        Reopen(boot_->catalog);
        Ok("CREATE TABLE t (id int64, state varchar, amount int64, ts timestamp NULL, "
           "quiet timestamp NULL NO CABIN, sym varchar)");
    }

    void Reopen(catalog::Catalog& catalog) {
        dispatcher_.reset();
        dispatcher_.emplace(boot_->superblock, catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), /*recorder=*/nullptr, /*replay=*/false,
                            /*access_statistics=*/true, &*cabins_);
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
