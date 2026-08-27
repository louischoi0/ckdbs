#include "kds/exec/assertion_catalog.hpp"

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// `sys.assertions` — the catalog half of ASSERTION (docs/spec/assertion.md
// §8.2, workplan AST03).
//
// What this file is really pinning, beyond "the row round-trips".
//
// **The relation is bootstrapped, so a data file either has it or does not
// mount.** It is the second catalog relation stored in ordinary user tuple
// format, on fixed page 14 — an id that used to be the first of the catalog
// *overflow* range, which is why it cost superblock format version 12 → 13
// and why every pre-existing data file stops mounting. A test that finds
// sys.assertions missing is a test running against a file an older build
// wrote, and the version is what turns that into a refusal at the door.
//
// **`source_text` is the canon.** The parsed declaration — group columns,
// aggregate, operator, bound — is stored nowhere; it is recovered by
// re-parsing the text. That is what makes the `GROUP BY` list uncapped, and
// it is why the round-trip test checks the text byte for byte rather than
// checking decoded fields that do not exist.
//
// **Nothing here enforces anything.** AST03 records a declaration; the Bound
// Cabin is AST04's and the write-path check is AST07's. `enforcing=0` in the
// reply is asserted on purpose — a constraint that silently does not run is
// not a degraded mode, and the reply is where a client finds that out.

namespace kds::exec {
namespace {

class AssertionCatalogTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
    }

    server::DispatchOutcome Run(const std::string& sql) {
        server::CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
        return d.Dispatch(sql);
    }

    // The relation every test declares against.
    void CreatePurchases() {
        ASSERT_EQ(Run("CREATE TABLE purchases (id int64, user_id int64, product_id int64, "
                      "amount int64, note varchar)")
                      .response.substr(0, 3),
                  "CRE");
    }

    catalog::Catalog& catalog() { return boot_->catalog; }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
};

// ---- The relation exists at all -----------------------------------------

TEST_F(AssertionCatalogTest, BootstrapCreatesTheRelationWithItsSixColumns) {
    auto access = catalog().InitTableAccess(catalog::kSysAssertionsTable);
    ASSERT_TRUE(access.ok()) << access.status().message();
    ASSERT_EQ(access.value()->schema.columns.size(), 6u);

    // The shape is §8.2's table with the Keystone pk added, and the order is
    // what `src/exec/assertion_catalog.cpp`'s kCol* constants index by - so
    // this is the test that fails if the two ever disagree.
    EXPECT_EQ(catalog::NameView(access.value()->schema.columns[0].name), "id");
    EXPECT_EQ(catalog::NameView(access.value()->schema.columns[1].name), "target_oid");
    EXPECT_EQ(catalog::NameView(access.value()->schema.columns[2].name), "cabin_root");
    EXPECT_EQ(catalog::NameView(access.value()->schema.columns[3].name), "flags");
    EXPECT_EQ(catalog::NameView(access.value()->schema.columns[4].name), "name");
    EXPECT_EQ(catalog::NameView(access.value()->schema.columns[5].name), "source_text");

    // A var-heap chain, because two columns are varchar. Without one, a
    // declaration longer than one inline cell would have nowhere to go.
    EXPECT_NE(access.value()->varheap_page_id, kInvalidPageId);

    // Rooted on its fixed page, which is what makes it findable at bootstrap
    // without first reading the catalog it is part of.
    EXPECT_EQ(access.value()->desc_page_id, catalog::kCatalogPageAssertions);

    EXPECT_TRUE(ListAssertions(catalog(), store_).ok());
    EXPECT_EQ(ListAssertions(catalog(), store_).value().size(), 0u);
}

TEST_F(AssertionCatalogTest, TheFormatVersionMovedAndTheOverflowRangeMovedWithIt) {
    // Both halves of the 12 -> 13 bump, asserted so neither can be quietly
    // undone. Page 14 is sys.assertions' root; it was the first id a catalog
    // chain grew into, so a version-12 file that outgrew any catalog root has
    // a page there that this build would overwrite.
    //
    // `>=` rather than `== 13`: this test pins that the bump *happened*, and
    // an exact pin makes every later bump fail here for no reason - which is
    // exactly what it did at 13 -> 14 (sys.tables' key_mode,
    // docs/workplan-key-mode.md PK01). A version below 13 is the real
    // regression, and that is what this now says.
    EXPECT_GE(server::kSuperBlockVersion, 13u);
    EXPECT_EQ(catalog::kCatalogPageAssertions, 14u);
    EXPECT_EQ(catalog::kCatalogOverflowFirst, 15u);
    EXPECT_GT(catalog::kCatalogOverflowFirst, catalog::kCatalogPageAssertions);
}

// ---- Round trip ---------------------------------------------------------

TEST_F(AssertionCatalogTest, ADeclarationIsRecordedAndReadBackVerbatim) {
    CreatePurchases();

    const std::string sql =
        "CREATE ASSERTION user_product_purchase_limit ON purchases "
        "GROUP BY (user_id, product_id) CHECK COUNT(*) <= 5";
    const std::string created = Run(sql).response;
    EXPECT_NE(created.find("CREATED ASSERTION name=user_product_purchase_limit"),
              std::string::npos)
        << created;
    // Enforcing, as of AST07: the structure is built, the write path
    // checks, and this dispatcher's registry holds the directory - all
    // three, which is what the field is the conjunction of.
    EXPECT_NE(created.find("enforcing=1"), std::string::npos) << created;

    auto defs = ListAssertions(catalog(), store_);
    ASSERT_TRUE(defs.ok()) << defs.status().message();
    ASSERT_EQ(defs.value().size(), 1u);

    const AssertionDef& def = defs.value()[0];
    EXPECT_EQ(def.name, "user_product_purchase_limit");
    // The canon, byte for byte. Everything the parser derived is recoverable
    // from this and is deliberately stored nowhere else.
    EXPECT_EQ(def.source_text, sql);
    // AST06: the publish carries the built chain's root. An unset root now
    // means a failed or absent build, not a pending task.
    EXPECT_NE(def.cabin_root, kInvalidPageId);
    EXPECT_EQ(def.flags, 0u);
    EXPECT_GT(def.id, 0u);

    auto oid = catalog().FindTableOidByName("purchases");
    ASSERT_TRUE(oid.ok());
    EXPECT_EQ(def.target_oid, oid.value());
}

TEST_F(AssertionCatalogTest, ADeclarationLongerThanOneInlineCellSpillsAndComesBackWhole) {
    CreatePurchases();

    // Eleven group columns is well past any inline cell, so `source_text`
    // spills to the var-heap and the read path has to resolve it *after*
    // releasing its page span (I15 R1). A truncated answer here would be the
    // spill resolution silently not happening.
    std::string sql = "CREATE ASSERTION wide ON purchases GROUP BY (user_id";
    for (int i = 0; i < 40; ++i) sql += ", product_id";
    sql += ") CHECK COUNT(*) <= 5";
    ASSERT_GT(sql.size(), 200u);

    const std::string created = Run(sql).response;
    ASSERT_NE(created.find("CREATED ASSERTION"), std::string::npos) << created;

    auto found = FindAssertionByName(catalog(), store_, "wide");
    ASSERT_TRUE(found.ok()) << found.status().message();
    ASSERT_TRUE(found.value().has_value());
    EXPECT_EQ(found.value()->source_text, sql);
}

TEST_F(AssertionCatalogTest, LookupByNameIsCaseInsensitiveAndLookupByTargetIsExact) {
    CreatePurchases();
    ASSERT_EQ(Run("CREATE TABLE other (id int64, k int64)").response.substr(0, 3), "CRE");

    ASSERT_NE(Run("CREATE ASSERTION AcctLimit ON purchases GROUP BY (user_id) "
                  "CHECK COUNT(*) <= 5")
                  .response.find("CREATED"),
              std::string::npos);
    ASSERT_NE(Run("CREATE ASSERTION other_limit ON other GROUP BY (k) CHECK COUNT(*) <= 2")
                  .response.find("CREATED"),
              std::string::npos);

    // Case-insensitive, because every other object name in this engine is: a
    // declaration made as `AcctLimit` and dropped as `acctlimit` has to be
    // the same assertion, or DROP would silently miss.
    for (const char* spelling : {"AcctLimit", "acctlimit", "ACCTLIMIT"}) {
        auto found = FindAssertionByName(catalog(), store_, spelling);
        ASSERT_TRUE(found.ok()) << spelling;
        ASSERT_TRUE(found.value().has_value()) << spelling;
        EXPECT_EQ(found.value()->name, "AcctLimit") << spelling;
    }
    EXPECT_FALSE(FindAssertionByName(catalog(), store_, "nope").value().has_value());

    auto purchases = catalog().FindTableOidByName("purchases");
    auto other = catalog().FindTableOidByName("other");
    ASSERT_TRUE(purchases.ok() && other.ok());

    auto on_purchases = AssertionsOnRelation(catalog(), store_, purchases.value());
    ASSERT_TRUE(on_purchases.ok());
    ASSERT_EQ(on_purchases.value().size(), 1u);
    EXPECT_EQ(on_purchases.value()[0].name, "AcctLimit");

    auto on_other = AssertionsOnRelation(catalog(), store_, other.value());
    ASSERT_TRUE(on_other.ok());
    ASSERT_EQ(on_other.value().size(), 1u);
    EXPECT_EQ(on_other.value()[0].name, "other_limit");
}

// This is the RESTRICT predicate of §8.3 and AS10, tested as far as it can
// be: **there is no DROP TABLE in this engine**, so there is no call site to
// hook it into. What a hook would ask is exactly this question, and the
// answer is non-empty while an assertion references the relation and empty
// once it is dropped.
TEST_F(AssertionCatalogTest, TheRestrictPredicateAnswersWhileAnAssertionReferencesTheRelation) {
    CreatePurchases();
    auto oid = catalog().FindTableOidByName("purchases");
    ASSERT_TRUE(oid.ok());

    EXPECT_TRUE(AssertionsOnRelation(catalog(), store_, oid.value()).value().empty());

    ASSERT_NE(Run("CREATE ASSERTION lim ON purchases GROUP BY (user_id) CHECK COUNT(*) <= 5")
                  .response.find("CREATED"),
              std::string::npos);
    EXPECT_EQ(AssertionsOnRelation(catalog(), store_, oid.value()).value().size(), 1u);

    ASSERT_NE(Run("DROP ASSERTION lim").response.find("DROPPED"), std::string::npos);
    EXPECT_TRUE(AssertionsOnRelation(catalog(), store_, oid.value()).value().empty());

    // A relation nothing references answers empty rather than failing, which
    // is what lets the hook be an unconditional call at the drop site.
    EXPECT_TRUE(AssertionsOnRelation(catalog(), store_, 999999).value().empty());
}

// ---- Drop ---------------------------------------------------------------

TEST_F(AssertionCatalogTest, DropRetiresTheRowSoTheNameIsFreeAgain) {
    CreatePurchases();

    ASSERT_NE(Run("CREATE ASSERTION lim ON purchases GROUP BY (user_id) CHECK COUNT(*) <= 5")
                  .response.find("CREATED"),
              std::string::npos);

    const std::string dropped = Run("DROP ASSERTION lim").response;
    EXPECT_NE(dropped.find("DROPPED ASSERTION name=lim"), std::string::npos) << dropped;
    EXPECT_EQ(ListAssertions(catalog(), store_).value().size(), 0u);

    // The row is *retired*, not delete-marked. Catalog reads have no snapshot
    // to filter a mark against, so a marked row would still be found by name
    // and this re-creation would collide with a row nobody can see.
    EXPECT_NE(Run("CREATE ASSERTION lim ON purchases GROUP BY (user_id) CHECK COUNT(*) <= 3")
                  .response.find("CREATED"),
              std::string::npos);
    ASSERT_EQ(ListAssertions(catalog(), store_).value().size(), 1u);
    EXPECT_NE(ListAssertions(catalog(), store_).value()[0].source_text.find("<= 3"),
              std::string::npos);

    // Dropping what is not there is NotFound, not a silent success.
    ASSERT_NE(Run("DROP ASSERTION lim").response.find("DROPPED"), std::string::npos);
    const std::string again = Run("DROP ASSERTION lim").response;
    EXPECT_EQ(again.substr(0, 3), "ERR") << again;
}

// ---- The catalog's half of §3.1's validation ----------------------------

TEST_F(AssertionCatalogTest, EveryCheckOnlyTheCatalogCanMakeIsMade) {
    CreatePurchases();

    struct Case {
        const char* sql;
        const char* mentions;
    };
    const Case cases[] = {
        // The relation must exist.
        {"CREATE ASSERTION a ON nosuch GROUP BY (user_id) CHECK COUNT(*) <= 1",
         "no relation named 'nosuch'"},
        // Every GROUP BY column must exist - a declaration naming one that is
        // not there could never be enforced.
        {"CREATE ASSERTION a ON purchases GROUP BY (user_id, nope) CHECK COUNT(*) <= 1",
         "has no column 'nope'"},
        // The SUM column must exist...
        {"CREATE ASSERTION a ON purchases GROUP BY (user_id) CHECK SUM(nope) <= 1",
         "has no column 'nope'"},
        // ...and be int64. A varchar is not a number this engine can total.
        {"CREATE ASSERTION a ON purchases GROUP BY (user_id) CHECK SUM(note) <= 1",
         "must be int64"},
    };
    for (const Case& c : cases) {
        const std::string out = Run(c.sql).response;
        EXPECT_EQ(out.substr(0, 3), "ERR") << c.sql << " -> " << out;
        EXPECT_NE(out.find(c.mentions), std::string::npos) << c.sql << " -> " << out;
        // Nothing was recorded by a refused declaration.
        EXPECT_EQ(ListAssertions(catalog(), store_).value().size(), 0u) << c.sql;
    }
}

TEST_F(AssertionCatalogTest, ASumOverUint64IsUnsupportedRatherThanWrong) {
    // §10 names this case separately from "not int64", and for AG3's reason:
    // half of a uint64's range does not fit the int64 accumulator a group
    // header keeps, so it is a form the engine understands and declines
    // rather than a type error.
    ASSERT_EQ(Run("CREATE TABLE u (id int64, k int64, big uint64)").response.substr(0, 3),
              "CRE");
    const std::string out = Run("CREATE ASSERTION a ON u GROUP BY (k) CHECK SUM(big) <= 1")
                                .response;
    EXPECT_EQ(out.substr(0, 3), "ERR") << out;
    EXPECT_NE(out.find("uint64"), std::string::npos) << out;
}

TEST_F(AssertionCatalogTest, ADuplicateNameIsRefusedAndLeavesTheFirstAlone) {
    CreatePurchases();

    ASSERT_NE(Run("CREATE ASSERTION lim ON purchases GROUP BY (user_id) CHECK COUNT(*) <= 5")
                  .response.find("CREATED"),
              std::string::npos);

    // §3.1's duplicate check. Case-insensitive, matching the lookup - two
    // spellings of one name must not become two assertions.
    for (const char* sql : {
             "CREATE ASSERTION lim ON purchases GROUP BY (product_id) CHECK COUNT(*) <= 9",
             "CREATE ASSERTION LIM ON purchases GROUP BY (product_id) CHECK COUNT(*) <= 9",
         }) {
        const std::string out = Run(sql).response;
        EXPECT_EQ(out.substr(0, 3), "ERR") << out;
        EXPECT_NE(out.find("already exists"), std::string::npos) << out;
    }

    auto defs = ListAssertions(catalog(), store_);
    ASSERT_EQ(defs.value().size(), 1u);
    EXPECT_NE(defs.value()[0].source_text.find("<= 5"), std::string::npos);
}

// ---- The inspection surface ---------------------------------------------

TEST_F(AssertionCatalogTest, ShowAssertionsNamesTheRelationAndPrintsTheDeclaration) {
    CreatePurchases();
    EXPECT_NE(Run("SHOW ASSERTIONS").response.find("assertions=0"), std::string::npos);

    ASSERT_NE(Run("CREATE ASSERTION lim ON purchases GROUP BY (user_id) CHECK COUNT(*) <= 5")
                  .response.find("CREATED"),
              std::string::npos);

    const std::string shown = Run("SHOW ASSERTIONS").response;
    EXPECT_NE(shown.find("assertions=1"), std::string::npos) << shown;
    EXPECT_NE(shown.find("name=lim"), std::string::npos) << shown;
    // The relation by name, resolved at print time rather than stored - the
    // row holds an oid so it stays narrow.
    EXPECT_NE(shown.find("rel=purchases"), std::string::npos) << shown;
    // 0, and correctly so: this fixture's Run() builds a fresh dispatcher
    // per statement, so SHOW here runs on an empty registry - which is the
    // restart case, and an assertion whose directory is gone is not
    // enforced however durable its catalog row is. The same-dispatcher
    // answer (1) is assertion_enforce_test's to pin.
    EXPECT_NE(shown.find("enforcing=0"), std::string::npos) << shown;
    EXPECT_NE(shown.find("CHECK COUNT(*) <= 5"), std::string::npos) << shown;
}

// ---- Durability ---------------------------------------------------------

TEST_F(AssertionCatalogTest, ADeclarationSurvivesAReopenOfTheSameStore) {
    CreatePurchases();
    ASSERT_NE(Run("CREATE ASSERTION lim ON purchases GROUP BY (user_id) CHECK SUM(amount) <= 100")
                  .response.find("CREATED"),
              std::string::npos);

    // A second Catalog over the same pages, which is what a restart looks
    // like from the catalog's side: the rows are on the page, not in the
    // cache. (Instance-scoped coherency means this is a *fresh read*, which
    // is exactly what is being tested.)
    catalog::Catalog reopened(store_);
    auto defs = ListAssertions(reopened, store_);
    ASSERT_TRUE(defs.ok()) << defs.status().message();
    ASSERT_EQ(defs.value().size(), 1u);
    EXPECT_EQ(defs.value()[0].name, "lim");
    EXPECT_NE(defs.value()[0].source_text.find("SUM(amount) <= 100"), std::string::npos);
}

}  // namespace
}  // namespace kds::exec
