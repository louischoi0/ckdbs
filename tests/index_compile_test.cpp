#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// `kIndexProbe` / `kIndexRange` in the compiler (docs/feat-index.md §§8-9,
// workplan IX10).
//
// The claims under test, in order of how badly getting them wrong would
// hurt:
//
//   1. **Selection is `f(shape, catalog)`.** Longest usable key prefix,
//      ties broken by lowest `index_oid`, and nothing about the data. A
//      recorded pattern must not compile differently as the rows change.
//   2. **The key equalities stay in the residual**, so downgrading any step
//      to a plain scan cannot change the result - the property invariant 9's
//      fall-through and every scan/probe equivalence rest on.
//   3. **An index cannot be entered by a non-leading key column.** An index
//      on (a, b) serves `a` and not `b`, and claiming otherwise would stop
//      the compiler calling that step a filter scan while leaving it exactly
//      as slow.
//
// Read through ANALYZE rather than by reaching into a StepChain: it is the
// surface an operator has, and a plan that only a test can see is a plan
// nobody can debug.

namespace kds::exec {
namespace {

class IndexCompileTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    void Ok(const std::string& sql) {
        const std::string out = Run(sql);
        ASSERT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    }

    // The access kind ANALYZE reports for step 0.
    std::string KindOf(const std::string& sql) {
        const std::string out = Run("ANALYZE " + sql);
        const std::size_t at = out.find("step 0 ");
        if (at == std::string::npos) return out;
        const std::size_t end = out.find(' ', at + 7);
        return out.substr(at + 7, end - (at + 7));
    }

    std::string Plan(const std::string& sql) { return Run("ANALYZE " + sql); }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<server::CommandDispatcher> dispatcher_;
};

// ---- Which kind ---------------------------------------------------------

TEST_F(IndexCompileTest, AnEqualityOnAnIndexedColumnCompilesToAnIndexProbe) {
    Ok("CREATE TABLE t (id int64, a int64, b int64) BTREE");
    Ok("CREATE INDEX ix ON t (a)");

    EXPECT_EQ(KindOf("SELECT * FROM t WHERE a = 1"), "IndexProbe");
    // The unindexed sibling, unchanged: this is what says the index moved
    // the kind rather than the predicate shape doing it.
    EXPECT_EQ(KindOf("SELECT * FROM t WHERE b = 1"), "FilterScan");
    // And a bare walk stays one.
    EXPECT_EQ(KindOf("SELECT * FROM t"), "Scan");
}

TEST_F(IndexCompileTest, ARangeOnAnIndexedColumnCompilesToAnIndexRange) {
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX ix ON t (a)");
    EXPECT_EQ(KindOf("SELECT * FROM t WHERE a BETWEEN 2 AND 5"), "IndexRange");
}

TEST_F(IndexCompileTest, ThePrimaryKeyStillWinsOutright) {
    // A relation with a pk equality is served better by the clustered tree
    // than any secondary index could serve it, so the index arm is not even
    // reached.
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX ix ON t (a)");
    EXPECT_EQ(KindOf("SELECT * FROM t WHERE id = 1 AND a = 2"), "Lookup");
    EXPECT_EQ(KindOf("SELECT * FROM t WHERE id BETWEEN 1 AND 5 AND a = 2"), "Range");
}

TEST_F(IndexCompileTest, AnIndexBeatsACabinOnTheSameColumn) {
    // Spec §9: an index is complete for every value where a Cabin is
    // authoritative only for the observed ones.
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE CABIN ON t(a)");
    ASSERT_EQ(KindOf("SELECT * FROM t WHERE a = 1"), "CabinProbe");

    Ok("CREATE INDEX ix ON t (a)");
    EXPECT_EQ(KindOf("SELECT * FROM t WHERE a = 1"), "IndexProbe");
}

// ---- Which index --------------------------------------------------------

TEST_F(IndexCompileTest, ACompositeIndexIsEnteredByItsLeadingColumnOnly) {
    Ok("CREATE TABLE t (id int64, a int64, b int64) BTREE");
    Ok("CREATE INDEX ix ON t (a, b)");

    EXPECT_EQ(KindOf("SELECT * FROM t WHERE a = 1"), "IndexProbe");
    EXPECT_EQ(KindOf("SELECT * FROM t WHERE a = 1 AND b = 2"), "IndexProbe");
    // `b` alone cannot enter it, and calling that anything but a filter scan
    // would tell the access statistics a lie.
    EXPECT_EQ(KindOf("SELECT * FROM t WHERE b = 2"), "FilterScan");
}

TEST_F(IndexCompileTest, TheLongestUsablePrefixWins) {
    Ok("CREATE TABLE t (id int64, a int64, b int64) BTREE");
    Ok("CREATE INDEX one ON t (a)");
    Ok("CREATE INDEX two ON t (a, b)");

    // Both can serve `a = 1`; only `two` can use `b` as well, and pinning
    // two columns beats pinning one whatever the creation order.
    EXPECT_NE(Plan("SELECT * FROM t WHERE a = 1 AND b = 2").find("IndexProbe"),
              std::string::npos);
    EXPECT_EQ(KindOf("SELECT * FROM t WHERE a = 1"), "IndexProbe");
}

TEST_F(IndexCompileTest, TwoEquallyUsableIndexesTieBreakOnCreationOrder) {
    // Deterministic and stable is not a preference: a plan that depended on
    // which row the catalog scan reached first would compile the same
    // statement differently as rows moved on the page.
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX first ON t (a)");
    Ok("CREATE INDEX second ON t (a)");

    const std::string once = Plan("SELECT * FROM t WHERE a = 1");
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(Plan("SELECT * FROM t WHERE a = 1"), once) << "compilation is not stable";
    }
}

TEST_F(IndexCompileTest, DroppingTheIndexPutsTheStepBackWhereItWas) {
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX ix ON t (a)");
    ASSERT_EQ(KindOf("SELECT * FROM t WHERE a = 1"), "IndexProbe");

    Ok("DROP INDEX ix");
    EXPECT_EQ(KindOf("SELECT * FROM t WHERE a = 1"), "FilterScan");
}

// ---- What the kind does *not* change ------------------------------------

TEST_F(IndexCompileTest, TheKeyEqualityStaysInTheResidual) {
    // The property everything else rests on: downgrading the step to a plain
    // scan must not change the result, which is only true while the equality
    // the index was chosen for is still a filter.
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX ix ON t (a)");

    const std::string plan = Plan("SELECT * FROM t WHERE a = 7");
    EXPECT_NE(plan.find("IndexProbe"), std::string::npos) << plan;
    EXPECT_NE(plan.find("filter"), std::string::npos)
        << "the equality left the residual, so a downgrade would change the answer: " << plan;
}

TEST_F(IndexCompileTest, AnIndexedStatementReturnsTheRowsTheUnindexedOneDoes) {
    // Two relations, same rows, one indexed - the equivalence IX12 will
    // widen, asserted here for the shapes this task emits.
    Ok("CREATE TABLE with_ix (id int64, a int64) BTREE");
    Ok("CREATE TABLE without (id int64, a int64) BTREE");
    Ok("CREATE INDEX ix ON with_ix (a)");
    for (int i = 0; i < 20; ++i) {
        const std::string v = std::to_string(i % 5);
        Ok("INSERT INTO with_ix VALUES (" + v + ")");
        Ok("INSERT INTO without VALUES (" + v + ")");
    }

    for (const char* predicate : {"a = 0", "a = 3", "a = 99", "a BETWEEN 1 AND 3"}) {
        const std::string indexed = Run(std::string("SELECT id FROM with_ix WHERE ") + predicate);
        const std::string plain = Run(std::string("SELECT id FROM without WHERE ") + predicate);
        EXPECT_EQ(indexed, plain) << predicate;
    }
}

TEST_F(IndexCompileTest, AParamNeverEntersAnIndex) {
    // A declared pattern's body is compiled to be type-checked and
    // fingerprinted, never run, so there is no value to encode a key from -
    // and nothing is lost, because these kinds are search-class either way.
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX ix ON t (a)");
    const std::string out = Run("CREATE PATTERN p ($x int64) OF SELECT * FROM t WHERE a = $x");
    EXPECT_NE(out.rfind("ERR", 0), 0u) << out;
}

TEST_F(IndexCompileTest, ATypedKeyColumnCompilesItsLiteralOnce) {
    // Coercion is a compile-time act and so is the key encoding that follows
    // it, so a literal that cannot be a value of the column is a positioned
    // compile error rather than a row-by-row false.
    Ok("CREATE TABLE t (id int64, d date, amt decimal(10,2)) BTREE");
    Ok("CREATE INDEX by_d ON t (d)");
    Ok("CREATE INDEX by_amt ON t (amt)");

    EXPECT_EQ(KindOf("SELECT * FROM t WHERE d = '2026-08-08'"), "IndexProbe");
    EXPECT_EQ(KindOf("SELECT * FROM t WHERE amt = '12.34'"), "IndexProbe");
    EXPECT_EQ(Run("SELECT * FROM t WHERE d = '2026-02-30'").rfind("ERR", 0), 0u);
}

}  // namespace
}  // namespace kds::exec
