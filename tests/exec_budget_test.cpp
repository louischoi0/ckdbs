#include "kds/exec/budget.hpp"

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/step_compiler.hpp"
#include "kds/exec/step_vm.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// V19 - cost guards and meters (docs/inflight/in-progress/parser-v2-workplan.md).
//
// Two things are being tested, and only one of them is about performance.
//
//   the budget   nothing suspends mid-statement on a cooperative core, so
//                an unbounded statement holds that core against every
//                other client on it. A correlated subquery over a non-pk
//                column is n² decodes from one line of SQL. The budget
//                turns that from a hang into a bounded failure, which is
//                the kinder answer: a client can rewrite a statement it
//                got an error for, and cannot diagnose one that never
//                replies.
//
//   the memo     a one-entry cache on the last probe key. It is an
//                accelerator, so the property that matters is not that it
//                is faster but that it is **invisible**: results must be
//                byte-identical with it hitting and with it missing. It
//                caches a location rather than a row precisely so a hit
//                re-reads and re-filters exactly what a fresh descent
//                would have handed to the same code.

namespace kds::exec {
namespace {

class ExecBudgetTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 4000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
    }

    void Create(const std::string& sql) {
        auto parsed = parser::Parse(sql);
        ASSERT_TRUE(parsed.ok()) << parsed.status().message();
        const auto& ct = std::get<parser::CreateTableStmt>(parsed.value());

        catalog::Schema schema;
        std::uint32_t pos = 0;
        for (const auto& col : ct.columns) {
            auto type_row = boot_->catalog.ResolveTypeByName(col.type_name);
            ASSERT_TRUE(type_row.ok()) << type_row.status().message();
            catalog::SysColumnRow row{};
            row.pos = pos++;
            catalog::SetName(row.name, col.name);
            row.type_val = type_row.value().type_val;
            row.len = type_row.value().len;
            row.notnull = true;
            schema.columns.push_back(row);
        }
        auto created = boot_->catalog.CreateTable(catalog::kNamespacePublic, ct.table_name, schema,
                                                  ct.clustered);
        ASSERT_TRUE(created.ok()) << created.status().message();
    }

    void Insert(const std::string& table, const std::vector<parser::AstValue>& body) {
        auto oid = boot_->catalog.FindTableOidByName(table);
        ASSERT_TRUE(oid.ok()) << oid.status().message();
        auto access = boot_->catalog.InitTableAccess(oid.value());
        ASSERT_TRUE(access.ok()) << access.status().message();
        auto id = boot_->catalog.AllocateRowId(oid.value());
        ASSERT_TRUE(id.ok()) << id.status().message();

        auto payload = EncodeRow(access.value()->schema, access.value()->layout, id.value(), body);
        ASSERT_TRUE(payload.ok()) << payload.status().message();
        if (access.value()->clustered_type == catalog::ClusteredType::kBtree) {
            auto placed = btree::BtreeInsert(store_, access.value()->desc_page_id, id.value(),
                                             payload.value(), /*trx_id=*/1, access.value()->oid);
            ASSERT_TRUE(placed.ok()) << placed.status().message();
        } else {
            auto placed = heap::ChainInsert(store_, access.value()->desc_page_id, id.value(),
                                            payload.value(), /*trx_id=*/1, access.value()->oid);
            ASSERT_TRUE(placed.ok()) << placed.status().message();
        }
    }

    static parser::AstValue Int(std::int64_t v) {
        parser::AstValue out;
        out.type = parser::ValueType::kInt;
        out.int_val = v;
        out.raw_int_text = std::to_string(v);
        return out;
    }
    static parser::AstValue Str(std::string v) {
        parser::AstValue out;
        out.type = parser::ValueType::kStr;
        out.str_val = std::move(v);
        return out;
    }

    StatusOr<std::vector<std::string>> TryRun(const std::string& sql, const Budget& budget,
                                              ExecStats* stats = nullptr) {
        auto parsed = parser::Parse(sql);
        if (!parsed.ok()) return parsed.status();
        auto chain = Compile(boot_->catalog, std::get<parser::SelectStmt>(parsed.value()));
        if (!chain.ok()) return chain.status();

        std::vector<std::string> rows;
        Status ran = Execute(
            boot_->catalog, store_, chain.value(),
            [&](const ChainFrame& frame) -> StatusOr<storage::VisitControl> {
                std::string row;
                for (const ColumnRef& ref : chain.value().projection) {
                    if (!row.empty()) row += '|';
                    row += FormatValue(/*type_val=*/0, frame.Get(ref));
                }
                rows.push_back(std::move(row));
                return storage::VisitControl::kContinue;
            },
            stats, budget);
        if (!ran.ok()) return ran;
        return rows;
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
};

// ---- The Budget type itself ----------------------------------------------

TEST_F(ExecBudgetTest, ABudgetAllowsExactlyItsLimitAndThenRefuses) {
    Budget budget(3);
    EXPECT_TRUE(budget.ChargeRow().ok());
    EXPECT_TRUE(budget.ChargeRow().ok());
    EXPECT_TRUE(budget.ChargeRow().ok()) << "the limit itself is allowed, not the limit minus one";

    Status over = budget.ChargeRow();
    EXPECT_FALSE(over.ok());
    EXPECT_EQ(over.code(), StatusCode::kResourceExhausted);
    EXPECT_FALSE(over.retryable()) << "re-running does the same work and stops in the same place";
    // The message has to name the limit and the key: the operator reading
    // it is deciding whether the statement is wrong or the ceiling is, and
    // neither is answerable from "budget exceeded".
    EXPECT_NE(over.message().find("3"), std::string::npos) << over.message();
    EXPECT_NE(over.message().find("max_rows_touched"), std::string::npos) << over.message();
}

TEST_F(ExecBudgetTest, AnExhaustedBudgetKeepsRefusing) {
    // The caller stops at the first refusal, but a caller that does not
    // must not see the count wrap around into permission.
    Budget budget(1);
    EXPECT_TRUE(budget.ChargeRow().ok());
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(budget.ChargeRow().code(), StatusCode::kResourceExhausted);
    }
    EXPECT_EQ(budget.touched(), 6u);
}

TEST_F(ExecBudgetTest, ZeroMeansUnlimited) {
    Budget budget(kUnlimitedRowTouchBudget);
    EXPECT_TRUE(budget.unlimited());
    for (int i = 0; i < 10000; ++i) ASSERT_TRUE(budget.ChargeRow().ok());
}

// ---- The budget in the executor ------------------------------------------

TEST_F(ExecBudgetTest, ARunawayCorrelatedScanFailsInsteadOfRunningToCompletion) {
    // The shape the budget exists for: a correlated subquery on a **non-pk**
    // column reads the whole inner relation once per outer row. 40 x 40 is
    // 1,600 decodes plus the outer 40 - small here, but it is the same
    // statement that is n² at any size, and one line of SQL.
    Create("CREATE TABLE outer_t (id int64, tag int64)");
    Create("CREATE TABLE inner_t (id int64, tag int64)");
    for (int i = 0; i < 40; ++i) {
        Insert("outer_t", {Int(i)});
        Insert("inner_t", {Int(i)});
    }

    const char* sql =
        "SELECT outer_t.id FROM outer_t WHERE EXISTS "
        "(SELECT inner_t.id FROM inner_t WHERE inner_t.tag = outer_t.tag)";

    // **With the inner build off**, which is what this shape cost before
    // JB6 and what it costs whenever the map declines: unbounded, it
    // completes and the meters show why it was expensive.
    Budget unbuilt(kUnlimitedRowTouchBudget);
    unbuilt.set_join_build_max_rows(0);
    ExecStats unbounded;
    auto full = TryRun(sql, unbuilt, &unbounded);
    ASSERT_TRUE(full.ok()) << full.status().message();
    EXPECT_EQ(full.value().size(), 40u);
    EXPECT_EQ(unbounded.Total().correlated_scans, 40u)
        << "one inner walk per outer row - the counter that names the shape";

    // 860, and the exact number is worth pinning because it is not the
    // naive 40 x 40. Outer row i matches inner row i, and EXISTS stops at
    // the first qualifying row, so the i-th walk reads i+1 rows:
    // 40 outer + sum(1..40) = 40 + 820. The short-circuit is doing real
    // work here even while the shape stays quadratic in the worst case -
    // which is exactly why the budget bounds the shape rather than
    // trusting the average.
    EXPECT_EQ(unbounded.Total().rows_examined, 860u);

    // Bounded well below that, it is refused rather than run.
    Budget capped_budget(100);
    capped_budget.set_join_build_max_rows(0);
    ExecStats bounded;
    auto capped = TryRun(sql, capped_budget, &bounded);
    ASSERT_FALSE(capped.ok());
    EXPECT_EQ(capped.status().code(), StatusCode::kResourceExhausted);
    // And it stopped near the ceiling rather than after doing the work: a
    // budget checked only at the end would bound nothing.
    EXPECT_LE(bounded.Total().rows_examined, 110u)
        << "the statement kept reading past its budget: " << bounded.Total().rows_examined;

    // **With the build on (the default), the same statement is no longer
    // this shape at all** (workplan JB6): the prefix map visits each inner
    // row at most once across the whole statement, so 820 inner decodes
    // become 40 and the budget that refused the walk now completes the
    // work. The budget is unchanged and still bounds what is left - the
    // statement got cheaper, not exempt.
    ExecStats built;
    auto with_build = TryRun(sql, Budget(100), &built);
    ASSERT_TRUE(with_build.ok()) << with_build.status().message();
    EXPECT_EQ(with_build.value().size(), 40u) << "same answer, fewer rows read";
    EXPECT_EQ(built.Total().rows_examined, 80u)
        << "40 outer plus 40 inner, each inner row visited once (spec §6)";
}

TEST_F(ExecBudgetTest, TheBudgetIsPerStatementNotPerChain) {
    // A per-chain budget would let a correlated subquery spend the full
    // allowance once per outer row - which is precisely the shape being
    // bounded, so the bound would do nothing at all.
    Create("CREATE TABLE o (id int64, tag int64)");
    Create("CREATE TABLE i (id int64, tag int64)");
    for (int n = 0; n < 30; ++n) {
        Insert("o", {Int(n)});
        Insert("i", {Int(n)});
    }

    // 30 outer rows; a per-chain budget of 50 would admit each inner walk
    // (30 rows) forever. A per-statement one stops in the first few.
    auto capped = TryRun("SELECT o.id FROM o WHERE EXISTS "
                         "(SELECT i.id FROM i WHERE i.tag = o.tag)",
                         Budget(50));
    ASSERT_FALSE(capped.ok());
    EXPECT_EQ(capped.status().code(), StatusCode::kResourceExhausted);
}

TEST_F(ExecBudgetTest, AnOrdinaryStatementIsUnaffectedByTheDefaultBudget) {
    // The ceiling must be nowhere near anything deliberate, or it stops
    // being a guard and becomes a limit people work around.
    Create("CREATE TABLE t (id int64, name varchar)");
    for (int i = 0; i < 200; ++i) Insert("t", {Str("n" + std::to_string(i))});

    auto rows = TryRun("SELECT t.id FROM t", Budget());
    ASSERT_TRUE(rows.ok()) << rows.status().message();
    EXPECT_EQ(rows.value().size(), 200u);
}

TEST_F(ExecBudgetTest, TheBudgetDoesNotCarryFromOneStatementToTheNext) {
    // Execute copies the caller's limit into a fresh counter, so a
    // dispatcher can hold one Budget for its lifetime without having to
    // remember to reset it between statements.
    Create("CREATE TABLE t (id int64, name varchar)");
    for (int i = 0; i < 20; ++i) Insert("t", {Str("x")});

    const Budget budget(100);
    for (int run = 0; run < 5; ++run) {
        auto rows = TryRun("SELECT t.id FROM t", budget);
        ASSERT_TRUE(rows.ok()) << "run " << run << ": " << rows.status().message();
        EXPECT_EQ(rows.value().size(), 20u);
    }
}

// ---- The probe memo -------------------------------------------------------

TEST_F(ExecBudgetTest, ResultsAreIdenticalWithTheMemoHittingAndMissing) {
    // The property that matters for an accelerator: it is invisible. The
    // memo hits when consecutive outer rows carry the same probe key, so
    // the two orderings below exercise it and defeat it over the same
    // data, and the rows must match exactly.
    Create("CREATE TABLE parent (id int64, label varchar) BTREE");
    Create("CREATE TABLE clustered_child (id int64, parent_id int64, tag varchar)");
    Create("CREATE TABLE alternating_child (id int64, parent_id int64, tag varchar)");

    for (int i = 0; i < 4; ++i) Insert("parent", {Str("p" + std::to_string(i))});

    // Consecutive repeats: 1,1,1,2,2,2,... - the memo hits on 2 of every 3.
    for (int p = 1; p <= 4; ++p) {
        for (int r = 0; r < 3; ++r) {
            Insert("clustered_child", {Int(p), Str("t" + std::to_string(p))});
        }
    }
    // The same 12 pairings, interleaved: 1,2,3,4,1,2,3,4,... - the key
    // changes every row, so the memo never hits.
    for (int r = 0; r < 3; ++r) {
        for (int p = 1; p <= 4; ++p) {
            Insert("alternating_child", {Int(p), Str("t" + std::to_string(p))});
        }
    }

    ExecStats clustered_stats;
    auto clustered = TryRun("SELECT c.tag, parent.label FROM clustered_child AS c "
                            "JOIN parent ON c.parent_id = parent.id",
                            Budget(), &clustered_stats);
    ASSERT_TRUE(clustered.ok()) << clustered.status().message();

    ExecStats alternating_stats;
    auto alternating = TryRun("SELECT c.tag, parent.label FROM alternating_child AS c "
                              "JOIN parent ON c.parent_id = parent.id",
                              Budget(), &alternating_stats);
    ASSERT_TRUE(alternating.ok()) << alternating.status().message();

    // The memo did its job on one and not the other...
    EXPECT_GT(clustered_stats.Total().probe_memo_hits, 0u)
        << "consecutive repeated keys should hit the memo";
    EXPECT_EQ(alternating_stats.Total().probe_memo_hits, 0u)
        << "a key that changes every row cannot hit a one-entry memo";

    // ...and the answers are the same multiset either way. Sorted, because
    // the two child relations were inserted in different orders on
    // purpose - it is the *rows* that must match, not the order the
    // written chain produced them in.
    std::vector<std::string> a = clustered.value();
    std::vector<std::string> b = alternating.value();
    ASSERT_EQ(a.size(), 12u);
    ASSERT_EQ(b.size(), 12u);
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    EXPECT_EQ(a, b) << "the memo changed the answer";
}

TEST_F(ExecBudgetTest, TheMemoIsPerStepAndNeverServesOneRelationsLocationToAnother) {
    // A regression, and the bug it pins was a silent wrong-row one.
    //
    // A ChainRunner owns *one* memo but runs every step of its chain
    // through it. Keyed on the probe key alone, a chain with two
    // pk-descending steps whose keys coincide served the second step the
    // first relation's cached (page, slot) - and the row was then decoded
    // with the wrong relation's schema. Different row widths gave a
    // Corruption error; **equal widths returned a row from the wrong
    // table with no error at all**, which is the case this test is shaped
    // to catch.
    //
    // Both relations below are deliberately the same width - two int64
    // columns - so a leaked location decodes cleanly into a plausible,
    // wrong answer.
    Create("CREATE TABLE lhs (id int64, other_id int64) BTREE");
    Create("CREATE TABLE rhs (id int64, marker int64) BTREE");

    // rhs.id 1..4 with markers 1000+i, so a row from rhs is unmistakable.
    for (int i = 1; i <= 4; ++i) Insert("rhs", {Int(1000 + i)});
    // lhs row 2 points at rhs row 2: the step-0 lookup key and the step-1
    // probe key are then *both* 2, which is exactly the coincidence.
    for (int i = 1; i <= 4; ++i) Insert("lhs", {Int(i)});

    auto rows = TryRun("SELECT r.marker FROM lhs AS l JOIN rhs AS r ON l.other_id = r.id "
                       "WHERE l.id = 2",
                       Budget());
    ASSERT_TRUE(rows.ok()) << rows.status().message();
    ASSERT_EQ(rows.value().size(), 1u);
    // 1002 is rhs row 2's marker. Before the fix this read lhs row 2 - the
    // memo's cached location - and projected its second column, 2.
    EXPECT_EQ(rows.value()[0], "1002")
        << "the probe returned a row from the driving relation, not the probed one";
}

TEST_F(ExecBudgetTest, AMemoHitReturnsTheSameRowAFreshDescentWould) {
    // Stronger than the multiset check: the same statement over the same
    // data, run twice, must be byte-identical run to run. The memo carries
    // state across rows within one execution, so a memo that returned a
    // stale row would show up here as a difference between the first pass
    // (cold) and the second (warm).
    Create("CREATE TABLE p (id int64, label varchar) BTREE");
    Create("CREATE TABLE c (id int64, p_id int64, tag varchar)");
    for (int i = 0; i < 3; ++i) Insert("p", {Str("label" + std::to_string(i))});
    for (int r = 0; r < 4; ++r) Insert("c", {Int(2), Str("t" + std::to_string(r))});

    const char* sql = "SELECT c.tag, p.label FROM c JOIN p ON c.p_id = p.id";
    ExecStats stats;
    auto first = TryRun(sql, Budget(), &stats);
    auto second = TryRun(sql, Budget());
    ASSERT_TRUE(first.ok()) << first.status().message();
    ASSERT_TRUE(second.ok()) << second.status().message();

    EXPECT_EQ(first.value(), second.value());
    EXPECT_EQ(first.value().size(), 4u);
    // Every child row carries the same key, so all but the first descent
    // is a memo hit.
    EXPECT_EQ(stats.Total().probe_memo_hits, 3u);
    for (const std::string& row : first.value()) {
        EXPECT_NE(row.find("label1"), std::string::npos)
            << "id 2 is the second row inserted, whose label is label1: " << row;
    }
}

TEST_F(ExecBudgetTest, TheMemoDoesNotFireOnAHeapRelation) {
    // A heap relation has no pk index, so a Probe step falls through to a
    // chain walk (step_vm.cpp's RunPointStep) and there is no descent to
    // memoize. Worth pinning: a memo that "hit" here would be caching
    // something the code never looked up.
    Create("CREATE TABLE hp (id int64, label varchar)");
    Create("CREATE TABLE hc (id int64, p_id int64, tag varchar)");
    for (int i = 0; i < 3; ++i) Insert("hp", {Str("l" + std::to_string(i))});
    for (int r = 0; r < 4; ++r) Insert("hc", {Int(1), Str("t")});

    ExecStats stats;
    auto rows = TryRun("SELECT hc.tag, hp.label FROM hc JOIN hp ON hc.p_id = hp.id",
                       Budget(), &stats);
    ASSERT_TRUE(rows.ok()) << rows.status().message();
    EXPECT_EQ(rows.value().size(), 4u);
    EXPECT_EQ(stats.Total().probe_memo_hits, 0u);
}

// ---- The meters -----------------------------------------------------------

TEST_F(ExecBudgetTest, TheCorrelatedScanCounterNamesTheExpensiveShape) {
    Create("CREATE TABLE o2 (id int64, tag int64)");
    Create("CREATE TABLE i2 (id int64, tag int64)");
    for (int i = 0; i < 5; ++i) {
        Insert("o2", {Int(i)});
        Insert("i2", {Int(i)});
    }

    // Correlated on a non-pk column: the inner step is a Scan, counted.
    ExecStats scanning;
    ASSERT_TRUE(TryRun("SELECT o2.id FROM o2 WHERE EXISTS "
                       "(SELECT i2.id FROM i2 WHERE i2.tag = o2.tag)",
                       Budget(), &scanning)
                    .ok());
    EXPECT_EQ(scanning.Total().correlated_scans, 5u);

    // Correlated on the pk of a BTREE inner: a real descent per outer row,
    // not a walk - not counted. That distinction is the point of the
    // counter: it names the quadratic shape, not correlation.
    Create("CREATE TABLE i3 (id int64, tag int64) BTREE");
    for (int i = 0; i < 5; ++i) Insert("i3", {Int(i)});
    ExecStats probing;
    ASSERT_TRUE(TryRun("SELECT o2.id FROM o2 WHERE EXISTS "
                       "(SELECT i3.id FROM i3 WHERE i3.id = o2.tag)",
                       Budget(), &probing)
                    .ok());
    EXPECT_EQ(probing.Total().correlated_scans, 0u);

    // The same shape over the HEAP inner is a different execution: a heap
    // pk probe has no descent and falls through to the walk, so it *is*
    // the quadratic shape - and the walk-level counting (2026-08-19,
    // step_vm.hpp) says so, where the old compile-time kind test called it
    // a Probe and reported zero for a statement that walked the relation
    // once per outer row.
    ExecStats heap_probing;
    ASSERT_TRUE(TryRun("SELECT o2.id FROM o2 WHERE EXISTS "
                       "(SELECT i2.id FROM i2 WHERE i2.id = o2.tag)",
                       Budget(), &heap_probing)
                    .ok());
    EXPECT_EQ(heap_probing.Total().correlated_scans, 5u);
}

}  // namespace
}  // namespace kds::exec
