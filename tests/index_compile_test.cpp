#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// `kIndexProbe` / `kIndexRange` in the compiler (docs/spec/index.md §§8-9,
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

// ---- The descent actually happens (workplan IX11) -----------------------

TEST_F(IndexCompileTest, AnIndexProbeReadsTheMatchingRowsAndNotTheRelation) {
    // The claim the feature exists for, and the one nothing before IX11
    // could make: a probe examines the rows it returns, not every row.
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX ix ON t (a)");
    for (int i = 0; i < 60; ++i) Ok("INSERT INTO t VALUES (" + std::to_string(i % 6) + ")");

    const std::string probe = Plan("SELECT id FROM t WHERE a = 3");
    EXPECT_NE(probe.find("IndexProbe"), std::string::npos) << probe;
    EXPECT_NE(probe.find("examined=10"), std::string::npos)
        << "the probe read the relation instead of the index: " << probe;
    EXPECT_NE(probe.find("index_scanned=10"), std::string::npos) << probe;

    // The unindexed sibling still reads all 60, which is what says the
    // number above is the index and not the data.
    Ok("CREATE TABLE u (id int64, a int64) BTREE");
    for (int i = 0; i < 60; ++i) Ok("INSERT INTO u VALUES (" + std::to_string(i % 6) + ")");
    EXPECT_NE(Plan("SELECT id FROM u WHERE a = 3").find("examined=60"), std::string::npos);
}

TEST_F(IndexCompileTest, ARangeStopsAtItsHighBound) {
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX ix ON t (a)");
    for (int i = 0; i < 100; ++i) Ok("INSERT INTO t VALUES (" + std::to_string(i) + ")");

    const std::string plan = Plan("SELECT id FROM t WHERE a BETWEEN 10 AND 19");
    EXPECT_NE(plan.find("IndexRange"), std::string::npos) << plan;
    EXPECT_NE(plan.find("examined=10"), std::string::npos) << plan;
}

TEST_F(IndexCompileTest, AnUpdatedKeyIsFoundUnderItsNewValueAndNotItsOld) {
    // Maintenance is append-only, so the old entry is still there. The row
    // must come back under the new key and not the old one - which is the
    // read-time key re-check subtracting the surplus (spec §1).
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX ix ON t (a)");
    Ok("INSERT INTO t VALUES (5)");
    Ok("UPDATE t SET a = 6 WHERE id = 1");

    EXPECT_EQ(Run("SELECT id FROM t WHERE a = 5"), Run("SELECT id FROM t WHERE id = 999"))
        << "the stale entry served a row the predicate no longer matches";
    EXPECT_NE(Run("SELECT id FROM t WHERE a = 6").find('1'), std::string::npos);
}

TEST_F(IndexCompileTest, ARoundTrippedKeyEmitsItsRowOnce) {
    // v -> v' -> v leaves two entries naming one pk under append-only
    // maintenance, and resolving both would emit the row twice.
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX ix ON t (a)");
    Ok("INSERT INTO t VALUES (5)");
    Ok("UPDATE t SET a = 6 WHERE id = 1");
    Ok("UPDATE t SET a = 5 WHERE id = 1");

    EXPECT_EQ(Run("SELECT id FROM t WHERE a = 5"), "id\\n1");
}

TEST_F(IndexCompileTest, ADeletedRowIsNotServedFromItsSurvivingEntry) {
    // DELETE leaves the entry (removal is forbidden), so the only thing
    // keeping the row out of the answer is the visibility predicate at
    // AcceptTupleAt - which this step goes through like every other kind.
    Ok("CREATE TABLE t (id int64, a int64) BTREE");
    Ok("CREATE INDEX ix ON t (a)");
    Ok("INSERT INTO t VALUES (5)");
    Ok("INSERT INTO t VALUES (5)");
    Ok("DELETE FROM t WHERE id = 1");

    EXPECT_EQ(Run("SELECT id FROM t WHERE a = 5"), "id\\n2");
}

TEST_F(IndexCompileTest, ACoveredColumnFiltersBeforeTheBaseDescent) {
    // What covering buys, and the only thing it buys: a row the entry
    // already disqualifies never costs a descent. It does *not* skip the
    // base read for a row that survives - there is no visibility witness
    // outside the tuple (spec §7).
    Ok("CREATE TABLE t (id int64, a int64, b int64) BTREE");
    Ok("CREATE INDEX ix ON t (a) COVERING (b)");
    for (int i = 0; i < 40; ++i) {
        Ok("INSERT INTO t VALUES (1, " + std::to_string(i % 4) + ")");
    }

    const std::string plan = Plan("SELECT id FROM t WHERE a = 1 AND b = 2");
    EXPECT_NE(plan.find("IndexProbe"), std::string::npos) << plan;
    EXPECT_NE(plan.find("index_scanned=40"), std::string::npos) << plan;
    EXPECT_NE(plan.find("index_filtered=30"), std::string::npos)
        << "the covered column did not filter: " << plan;
    EXPECT_NE(plan.find("examined=10"), std::string::npos) << plan;

    // And the rows are the ones the uncovered index would have returned.
    Ok("CREATE TABLE u (id int64, a int64, b int64) BTREE");
    Ok("CREATE INDEX ux ON u (a)");
    for (int i = 0; i < 40; ++i) {
        Ok("INSERT INTO u VALUES (1, " + std::to_string(i % 4) + ")");
    }
    EXPECT_EQ(Run("SELECT id FROM t WHERE a = 1 AND b = 2"),
              Run("SELECT id FROM u WHERE a = 1 AND b = 2"));
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

// ---- The read-path switch (workplan IX13) -------------------------------

// One database at a chosen setting of `indexes`. A plain struct rather than
// a second fixture, because the A/B test needs *two* of them at once and a
// gtest fixture cannot be instantiated.
struct Instance {
    storage::InMemoryPageStore store{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot;
    std::optional<server::CommandDispatcher> dispatcher;

    explicit Instance(bool indexes) {
        auto booted = bootstrap::BootstrapDatabase(store, 1000);
        EXPECT_TRUE(booted.ok()) << booted.status().message();
        boot.emplace(std::move(booted.value()));
        dispatcher.emplace(boot->superblock, boot->catalog, store, /*log=*/nullptr,
                           /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                           Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                           /*access_statistics=*/true, /*cabins=*/nullptr, /*txn=*/nullptr,
                           txn::IsolationLevel::kReadCommitted, /*core_id=*/0, indexes);
    }

    std::string Run(const std::string& sql) { return dispatcher->Dispatch(sql).response; }
    void Ok(const std::string& sql) {
        const std::string out = Run(sql);
        EXPECT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    }
    std::string Plan(const std::string& sql) { return Run("ANALYZE " + sql); }
};

TEST(IndexSwitchTest, TheChainIsTheSameAndTheStepWalks) {
    // `indexes = off` steers the branch inside an index step, never which
    // step was compiled - which is what keeps the plan `f(shape, catalog)`
    // and makes this comparison one of execution rather than planning.
    Instance off(/*indexes=*/false);
    off.Ok("CREATE TABLE t (id int64, a int64) BTREE");
    off.Ok("CREATE INDEX ix ON t (a)");
    for (int i = 0; i < 30; ++i) {
        off.Ok("INSERT INTO t VALUES (" + std::to_string(i % 3) + ")");
    }

    const std::string plan = off.Plan("SELECT id FROM t WHERE a = 1");
    EXPECT_NE(plan.find("IndexProbe"), std::string::npos)
        << "the switch changed the compiled chain: " << plan;
    // ...and it walked: every row read, no index counters.
    EXPECT_NE(plan.find("examined=30"), std::string::npos) << plan;
    EXPECT_EQ(plan.find("index_scanned="), std::string::npos) << plan;
}

TEST(IndexSwitchTest, TheRowsAreByteIdenticalEitherWay) {
    // The property the switch exists to check, and the reason IX8a's sort
    // had to happen: an accelerator may cost performance and must never
    // change a query result.
    Instance on(/*indexes=*/true);
    Instance off(/*indexes=*/false);

    const auto load = [](Instance& db) {
        db.Ok("CREATE TABLE t (id int64, a int64, b int64) BTREE");
        db.Ok("CREATE INDEX ix ON t (a) COVERING (b)");
        for (int i = 0; i < 40; ++i) {
            db.Ok("INSERT INTO t VALUES (" + std::to_string(i % 5) + ", " +
                  std::to_string(i % 4) + ")");
        }
        db.Ok("UPDATE t SET a = 9 WHERE id = 3");
        db.Ok("DELETE FROM t WHERE id = 7");
    };
    load(on);
    load(off);

    for (const char* sql : {"SELECT id FROM t WHERE a = 1",
                            "SELECT id FROM t WHERE a = 9",
                            "SELECT id FROM t WHERE a = 1 AND b = 2",
                            "SELECT id FROM t WHERE a BETWEEN 1 AND 3",
                            "SELECT id, a, b FROM t WHERE a = 0",
                            "SELECT COUNT(*) FROM t WHERE a = 2",
                            "SELECT a, COUNT(*) FROM t GROUP BY a",
                            "SELECT id FROM t WHERE a = 99"}) {
        EXPECT_EQ(off.Run(sql), on.Run(sql)) << sql;
    }
}

// ---- Equality propagation reaches the index (docs/spec/parser-v2.md §5) --------

TEST_F(IndexCompileTest, AJoinRestrictionOnTheOtherRelationReachesTheIndex) {
    // bench/results-scenario3-library.md §9's shape: the restriction sits
    // on `u`, the index on `l`. Before the propagation pass this compiled
    // to a full Scan of `l` with a probe per row - 10,086 pages for six
    // rows at the measured size; the derived `l.uid = 3` is what lets the
    // step compiler reach the index it already had.
    Ok("CREATE TABLE u (id int64, code int64) BTREE");
    Ok("CREATE TABLE l (id int64, uid int64, v int64) BTREE");
    Ok("CREATE INDEX ix ON l (uid)");
    for (int i = 0; i < 40; ++i) Ok("INSERT INTO u VALUES (" + std::to_string(100 + i) + ")");
    for (int i = 0; i < 40; ++i) {
        Ok("INSERT INTO l VALUES (" + std::to_string(1 + i % 8) + ", " + std::to_string(i) + ")");
    }

    const std::string written = "SELECT l.v FROM l JOIN u ON l.uid = u.id WHERE u.id = 3";
    const std::string by_hand = "SELECT l.v FROM l JOIN u ON l.uid = u.id WHERE l.uid = 3";

    EXPECT_EQ(KindOf(written), "IndexProbe");
    // Five matching rows entered through the index, not forty through the
    // relation - and the two writings of one question answer identically.
    const std::string plan = Plan(written);
    EXPECT_NE(plan.find("index_scanned=5"), std::string::npos) << plan;
    // The conjunct the probe keys on was derived, and the plan says so -
    // an unmarked line would be a filter the reader cannot find in the
    // statement they wrote.
    EXPECT_NE(plan.find(" derived"), std::string::npos) << plan;
    EXPECT_EQ(Run(written), Run(by_hand));
}

// ---- The correlated probe (spec §8a, IX17) --------------------------------

TEST_F(IndexCompileTest, AJoinKeyWithNoLiteralEntersTheIndexPerOuterRow) {
    // The shape equality propagation cannot reach: no literal exists on the
    // join-key class, so before IX17 the inner side was a full Scan per
    // outer row - 40 rows examined per outer row here, the whole relation.
    Ok("CREATE TABLE u (id int64, code int64) BTREE");
    Ok("CREATE TABLE l (id int64, uid int64, v int64) BTREE");
    Ok("CREATE INDEX ix ON l (uid)");
    for (int i = 0; i < 8; ++i) Ok("INSERT INTO u VALUES (" + std::to_string(100 + i) + ")");
    for (int i = 0; i < 40; ++i) {
        Ok("INSERT INTO l VALUES (" + std::to_string(1 + i % 8) + ", " + std::to_string(i) + ")");
    }

    const std::string sql =
        "SELECT l.v FROM u JOIN l ON l.uid = u.id WHERE u.id BETWEEN 1 AND 4";
    const std::string plan = Plan(sql);
    // The inner step probes the index keyed by the outer row, and the plan
    // says which column keys it.
    EXPECT_NE(plan.find("step 1 IndexProbe"), std::string::npos) << plan;
    EXPECT_NE(plan.find("key=0:0.0"), std::string::npos) << plan;
    // Four outer rows at five matches each: twenty index entries scanned,
    // not four scans of forty rows.
    EXPECT_NE(plan.find("index_scanned=20"), std::string::npos) << plan;

    // The identical rows the walk returns, proven against an unindexed twin
    // of the same data.
    Ok("CREATE TABLE l2 (id int64, uid int64, v int64) BTREE");
    for (int i = 0; i < 40; ++i) {
        Ok("INSERT INTO l2 VALUES (" + std::to_string(1 + i % 8) + ", " + std::to_string(i) +
           ")");
    }
    // Aliased `l` so the two replies' headers match and the comparison is
    // over the rows alone.
    EXPECT_EQ(Run(sql),
              Run("SELECT l.v FROM u JOIN l2 AS l ON l.uid = u.id WHERE u.id BETWEEN 1 AND 4"));
}

TEST_F(IndexCompileTest, TheOnClauseOrientationDoesNotDecideTheCorrelatedProbe) {
    // `ON l.uid = u.id` and `ON u.id = l.uid` are the same join; both must
    // give the inner step the probe, as the pk arm already guarantees for
    // its kinds.
    Ok("CREATE TABLE u (id int64, code int64) BTREE");
    Ok("CREATE TABLE l (id int64, uid int64, v int64) BTREE");
    Ok("CREATE INDEX ix ON l (uid)");
    Ok("INSERT INTO u VALUES (1)");
    Ok("INSERT INTO l VALUES (1, 10)");

    for (const char* on : {"l.uid = u.id", "u.id = l.uid"}) {
        const std::string plan = Plan(std::string("SELECT l.v FROM u JOIN l ON ") + on +
                                      " WHERE u.id BETWEEN 1 AND 2");
        EXPECT_NE(plan.find("step 1 IndexProbe"), std::string::npos) << on << "\n" << plan;
    }
}

TEST_F(IndexCompileTest, ACorrelatedExistsProbesTheIndexInsteadOfWalking) {
    // The other owner of the per-outer-row walk: a correlated sub-chain.
    // Its join equality reaches outward (up == 1), which is exactly the
    // "available before this step runs" case - the enclosing row is bound
    // before the sub-chain opens.
    Ok("CREATE TABLE u (id int64, code int64) BTREE");
    Ok("CREATE TABLE l (id int64, uid int64, v int64) BTREE");
    Ok("CREATE INDEX ix ON l (uid)");
    for (int i = 0; i < 4; ++i) Ok("INSERT INTO u VALUES (" + std::to_string(100 + i) + ")");
    Ok("INSERT INTO l VALUES (2, 20)");

    const std::string sql =
        "SELECT id FROM u WHERE EXISTS (SELECT l.id FROM l WHERE l.uid = u.id)";
    const std::string plan = Plan(sql);
    EXPECT_NE(plan.find("IndexProbe"), std::string::npos) << plan;
    EXPECT_NE(plan.find("key=1:0.0"), std::string::npos) << plan;

    Ok("CREATE TABLE l2 (id int64, uid int64, v int64) BTREE");
    Ok("INSERT INTO l2 VALUES (2, 20)");
    EXPECT_EQ(Run(sql),
              Run("SELECT id FROM u WHERE EXISTS (SELECT l2.id FROM l2 WHERE l2.uid = u.id)"));
}

TEST_F(IndexCompileTest, TheCorrelatedProbeDeclinesAMismatchedDescriptor) {
    // int32 against int64: the executor would encode the outer row's int64
    // value into an int32 key format the index was not built from, so the
    // compiler keeps the walk instead.
    Ok("CREATE TABLE u (id int64, code int64) BTREE");
    Ok("CREATE TABLE ln (id int64, uid int32, v int64) BTREE");
    Ok("CREATE INDEX ixn ON ln (uid)");
    const std::string plan =
        Plan("SELECT ln.v FROM u JOIN ln ON ln.uid = u.id WHERE u.id BETWEEN 1 AND 2");
    EXPECT_EQ(plan.find("step 1 IndexProbe"), std::string::npos) << plan;
    EXPECT_NE(plan.find("step 1 Scan"), std::string::npos) << plan;
}

TEST_F(IndexCompileTest, ALiteralEqualityStillBeatsTheCorrelatedForm) {
    // Propagation gives the inner step a literal on the indexed column; the
    // compile-time-encoded probe wins over the deferred one - same descent,
    // no per-row encode - which the absence of `key=` on the step line is
    // the visible half of.
    Ok("CREATE TABLE u (id int64, code int64) BTREE");
    Ok("CREATE TABLE l (id int64, uid int64, v int64) BTREE");
    Ok("CREATE INDEX ix ON l (uid)");
    Ok("INSERT INTO u VALUES (1)");
    Ok("INSERT INTO l VALUES (1, 10)");

    const std::string plan =
        Plan("SELECT l.v FROM u JOIN l ON l.uid = u.id WHERE u.id = 1");
    const std::size_t at = plan.find("step 1 IndexProbe");
    ASSERT_NE(at, std::string::npos) << plan;
    const std::size_t line_end = plan.find("\\n", at);
    EXPECT_EQ(plan.substr(at, line_end - at).find("key="), std::string::npos) << plan;
}

TEST_F(IndexCompileTest, TheCorrelatedIndexBeatsTheCorrelatedCabin) {
    // Spec §9's ordering, extended to the correlated forms: an index is
    // complete for every key value where a Cabin is authoritative only
    // for observed ones, so a join column carrying both is served by the
    // index and the plan says so.
    Ok("CREATE TABLE u (id int64, code int64) BTREE");
    Ok("CREATE TABLE l (id int64, uid int64, v int64) BTREE");
    Ok("CREATE INDEX ix ON l (uid)");
    Ok("CREATE CABIN ON l(uid)");
    Ok("INSERT INTO u VALUES (1)");
    Ok("INSERT INTO l VALUES (1, 10)");

    const std::string plan =
        Plan("SELECT l.v FROM u JOIN l ON l.uid = u.id WHERE u.id BETWEEN 1 AND 2");
    EXPECT_NE(plan.find("step 1 IndexProbe"), std::string::npos) << plan;
    EXPECT_EQ(plan.find("CabinProbe"), std::string::npos) << plan;
}

TEST_F(IndexCompileTest, PropagationDeclinesAMatchingTypeWithADifferentLen) {
    // decimal(10,2) and decimal(18,2) share type_val and scale - the one
    // reachable pair that passes the column-column compare while differing
    // in descriptor `len` (precision packs into it). The literal was
    // coerced against one column's descriptor and must not cross to the
    // other, so the join side keeps its unpropagated plan.
    Ok("CREATE TABLE dn (id int64, m decimal(10, 2)) BTREE");
    Ok("CREATE TABLE dw (id int64, m decimal(18, 2)) BTREE");
    const std::string sql = "SELECT dw.id FROM dw JOIN dn ON dw.m = dn.m WHERE dn.m = 1.50";
    EXPECT_EQ(KindOf(sql), "Scan");
    EXPECT_EQ(Plan(sql).find(" derived"), std::string::npos) << Plan(sql);
}

TEST_F(IndexCompileTest, TheSwappedWritingGetsALookupAndAnIndexedInnerSide) {
    // The same join written u-first: the written conjunct makes step 0 a
    // Lookup, and the derived one gives the *inner* step an IndexProbe -
    // an inner side a secondary index serves, which no written form
    // without a literal on `l` could produce before.
    Ok("CREATE TABLE u (id int64, code int64) BTREE");
    Ok("CREATE TABLE l (id int64, uid int64, v int64) BTREE");
    Ok("CREATE INDEX ix ON l (uid)");
    for (int i = 0; i < 40; ++i) Ok("INSERT INTO u VALUES (" + std::to_string(100 + i) + ")");
    for (int i = 0; i < 40; ++i) {
        Ok("INSERT INTO l VALUES (" + std::to_string(1 + i % 8) + ", " + std::to_string(i) + ")");
    }

    const std::string swapped = "SELECT l.v FROM u JOIN l ON l.uid = u.id WHERE u.id = 3";
    EXPECT_EQ(KindOf(swapped), "Lookup");
    const std::string plan = Plan(swapped);
    EXPECT_NE(plan.find("step 1 IndexProbe"), std::string::npos) << plan;

    // Same rows as the l-first writing, which is the equivalence the two
    // access kinds must not disturb.
    EXPECT_EQ(Run(swapped), Run("SELECT l.v FROM l JOIN u ON l.uid = u.id WHERE u.id = 3"));
}

}  // namespace
}  // namespace kds::exec
