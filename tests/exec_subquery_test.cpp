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

// V18 - the step VM's nested half: sub-chains, negation, cardinality
// (docs/inflight/in-progress/parser-v2-workplan.md).
//
// Three things here are easy to get subtly wrong, and each has its own
// section below:
//
//   negation      `NOT IN` is **not** `!IN`. Under SQL's three-valued
//                 semantics a NULL in the subquery result makes
//                 "matched nothing" UNKNOWN rather than TRUE, so a
//                 boolean negation is wrong the day NULLs become
//                 storable. The evaluator computes a tri-state and
//                 collapses it in exactly one place (spec I16).
//
//   cardinality   a scalar subquery returning two rows is a runtime
//                 error, not a first-row pick - which would make the
//                 answer depend on physical order.
//
//   work avoided  EXISTS stops at the first qualifying row, and a false
//                 uncorrelated EXISTS answers the whole statement without
//                 opening the outer relation at all. Both are claims
//                 about work *not* done, so they are checked with the
//                 execution counters rather than by inspecting results.

namespace kds::exec {
namespace {

class ExecSubqueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 4000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));

        Create("CREATE TABLE acct (id int64, name varchar, tier varchar)");
        Create("CREATE TABLE trade (id int64, acct_id int64, sym varchar)");

        Insert("acct", {Str("alice"), Str("gold")});    // id 1
        Insert("acct", {Str("bob"), Str("silver")});    // id 2
        Insert("acct", {Str("carol"), Str("gold")});    // id 3
        Insert("trade", {Int(1), Str("AAPL")});         // alice
        Insert("trade", {Int(3), Str("MSFT")});         // carol
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
        auto placed = heap::ChainInsert(store_, access.value()->desc_page_id, id.value(),
                                        payload.value(), /*trx_id=*/1, access.value()->oid);
        ASSERT_TRUE(placed.ok()) << placed.status().message();
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

    StatusOr<std::vector<std::string>> TryRun(const std::string& sql, ExecStats* stats = nullptr) {
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
            stats);
        if (!ran.ok()) return ran;
        return rows;
    }

    std::vector<std::string> Run(const std::string& sql, ExecStats* stats = nullptr) {
        auto rows = TryRun(sql, stats);
        EXPECT_TRUE(rows.ok()) << sql << ": " << rows.status().message();
        return rows.ok() ? rows.value() : std::vector<std::string>{};
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
};

// ---- EXISTS / NOT EXISTS, uncorrelated and correlated --------------------

TEST_F(ExecSubqueryTest, AnUncorrelatedExistsAdmitsOrExcludesEveryRow) {
    // True for all three rows: the subquery does not depend on any of them.
    EXPECT_EQ(Run("SELECT acct.name FROM acct WHERE EXISTS (SELECT trade.id FROM trade)"),
              (std::vector<std::string>{"alice", "bob", "carol"}));

    // False for all three, so the statement returns nothing.
    EXPECT_TRUE(
        Run("SELECT acct.name FROM acct WHERE EXISTS (SELECT trade.id FROM trade "
            "WHERE trade.sym = 'NONE')")
            .empty());
}

TEST_F(ExecSubqueryTest, ACorrelatedExistsFiltersPerRow) {
    // alice and carol have trades; bob does not.
    EXPECT_EQ(Run("SELECT acct.name FROM acct WHERE EXISTS "
                  "(SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)"),
              (std::vector<std::string>{"alice", "carol"}));
}

TEST_F(ExecSubqueryTest, NotExistsIsTheSameWalkNegated) {
    EXPECT_EQ(Run("SELECT acct.name FROM acct WHERE NOT EXISTS "
                  "(SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)"),
              (std::vector<std::string>{"bob"}));

    // And the two partition the relation between them - which is the
    // property that would break if NOT EXISTS were implemented as
    // anything other than the same walk with the answer inverted.
    const auto positive = Run("SELECT acct.name FROM acct WHERE EXISTS "
                              "(SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)");
    const auto negative = Run("SELECT acct.name FROM acct WHERE NOT EXISTS "
                              "(SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)");
    EXPECT_EQ(positive.size() + negative.size(), 3u);
}

TEST_F(ExecSubqueryTest, ACorrelatedSubqueryReadsTheOuterRowThroughTheFrameStack) {
    // Correlation values are read through the frame stack and never
    // written into the AST - which is shared, so mutating it per outer row
    // would make the statement's meaning depend on how far execution got.
    // Observable as: the same statement gives per-row answers, and running
    // it twice gives the same answers.
    const auto first = Run("SELECT acct.name FROM acct WHERE EXISTS "
                           "(SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)");
    const auto second = Run("SELECT acct.name FROM acct WHERE EXISTS "
                            "(SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)");
    EXPECT_EQ(first, second);
    EXPECT_EQ(first, (std::vector<std::string>{"alice", "carol"}));
}

// ---- IN / NOT IN ----------------------------------------------------------

TEST_F(ExecSubqueryTest, InTestsTheOuterColumnAgainstTheInnerValues) {
    EXPECT_EQ(Run("SELECT acct.name FROM acct WHERE id IN (SELECT acct_id FROM trade)"),
              (std::vector<std::string>{"alice", "carol"}));
}

TEST_F(ExecSubqueryTest, NotInExcludesExactlyWhatInAdmits) {
    EXPECT_EQ(Run("SELECT acct.name FROM acct WHERE id NOT IN (SELECT acct_id FROM trade)"),
              (std::vector<std::string>{"bob"}));
}

TEST_F(ExecSubqueryTest, NotInOverAnEmptySubqueryIsTrueForEveryRow) {
    // No rows means no NULLs either, so this is TRUE rather than UNKNOWN -
    // the case that separates "empty" from "contains a NULL".
    EXPECT_EQ(Run("SELECT acct.name FROM acct WHERE id NOT IN "
                  "(SELECT acct_id FROM trade WHERE trade.sym = 'NONE')"),
              (std::vector<std::string>{"alice", "bob", "carol"}));
}

TEST_F(ExecSubqueryTest, ACorrelatedInIsEvaluatedPerOuterRow) {
    EXPECT_EQ(Run("SELECT acct.name FROM acct WHERE id IN "
                  "(SELECT acct_id FROM trade WHERE trade.acct_id = acct.id)"),
              (std::vector<std::string>{"alice", "carol"}));
}

// ---- NULL, and the collapse point ----------------------------------------

TEST_F(ExecSubqueryTest, AZeroRowScalarSubqueryIsNullAndThereforeNeverTrue) {
    // Zero rows is NULL, and a comparison against NULL is not true - so
    // the predicate excludes every row rather than matching some default.
    EXPECT_TRUE(Run("SELECT acct.name FROM acct WHERE id = "
                    "(SELECT acct_id FROM trade WHERE trade.sym = 'NONE')")
                    .empty());

    // The same NULL under `!=` is *also* never true. That asymmetry with
    // two-valued logic is the whole reason UNKNOWN exists: a two-valued
    // `!=` would have admitted every row here.
    EXPECT_TRUE(Run("SELECT acct.name FROM acct WHERE id != "
                    "(SELECT acct_id FROM trade WHERE trade.sym = 'NONE')")
                    .empty());
}

TEST_F(ExecSubqueryTest, AComparisonAgainstAnInlineNullIsNeverTrue) {
    // The only NULL reachable from stored data today (spec I16): an
    // inline literal. Every comparison against it collapses to false.
    EXPECT_TRUE(Run("SELECT acct.name FROM acct WHERE tier = NULL").empty());
    EXPECT_TRUE(Run("SELECT acct.name FROM acct WHERE tier != NULL").empty());
}

// ---- Cardinality ----------------------------------------------------------

TEST_F(ExecSubqueryTest, AScalarSubqueryReturningOneRowCompares) {
    EXPECT_EQ(Run("SELECT acct.name FROM acct WHERE id = "
                  "(SELECT acct_id FROM trade WHERE trade.sym = 'MSFT')"),
              (std::vector<std::string>{"carol"}));
}

TEST_F(ExecSubqueryTest, AScalarSubqueryReturningTwoRowsFailsWithItsOwnCode) {
    // Not a first-row pick: that would make the answer depend on physical
    // order, so two identical databases with different page layouts would
    // disagree. Parse time cannot prove cardinality, so this is per
    // execution.
    auto rows = TryRun("SELECT acct.name FROM acct WHERE id = (SELECT acct_id FROM trade)");
    ASSERT_FALSE(rows.ok());
    EXPECT_EQ(rows.status().code(), StatusCode::kCardinalityViolation);
    EXPECT_FALSE(rows.status().retryable()) << "re-running it reads the same extra rows";
}

TEST_F(ExecSubqueryTest, ACorrelatedScalarSubqueryIsCheckedPerRow) {
    // One row per outer row here, so it succeeds - the same statement
    // shape that failed above, made single-valued by the correlation.
    EXPECT_EQ(Run("SELECT acct.name FROM acct WHERE id = "
                  "(SELECT acct_id FROM trade WHERE trade.acct_id = acct.id)"),
              (std::vector<std::string>{"alice", "carol"}));
}

// ---- Work not done --------------------------------------------------------

TEST_F(ExecSubqueryTest, AFalseUncorrelatedExistsOpensZeroPagesOfTheOuterRelation) {
    // The payoff for deciding hoisting structurally at compile: the whole
    // statement is answered before the outer relation is touched. On a
    // large outer relation that is the difference between a scan and
    // nothing at all.
    ExecStats stats;
    EXPECT_TRUE(Run("SELECT acct.name FROM acct WHERE EXISTS "
                    "(SELECT trade.id FROM trade WHERE trade.sym = 'NONE')",
                    &stats)
                    .empty());

    // One open: the sub-chain's own relation. The outer relation is never
    // reached, so no row of it is ever examined.
    EXPECT_EQ(stats.Total().sub_chain_runs, 1u);
    EXPECT_EQ(stats.Total().relation_opens, 1u) << "the outer relation must not have been opened";
    EXPECT_EQ(stats.Total().rows_examined, 2u) << "only the sub-chain's own rows";
}

TEST_F(ExecSubqueryTest, AnUncorrelatedExistsRunsOnceNotOncePerOuterRow) {
    // Three outer rows, one sub-chain evaluation. Running it per row
    // would compute the same answer three times.
    ExecStats stats;
    EXPECT_EQ(Run("SELECT acct.name FROM acct WHERE EXISTS (SELECT trade.id FROM trade)", &stats)
                  .size(),
              3u);
    EXPECT_EQ(stats.Total().sub_chain_runs, 1u);
}

TEST_F(ExecSubqueryTest, ACorrelatedExistsRunsOncePerSurvivingOuterRow) {
    ExecStats stats;
    Run("SELECT acct.name FROM acct WHERE EXISTS "
        "(SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)",
        &stats);
    EXPECT_EQ(stats.Total().sub_chain_runs, 3u) << "one per outer row, which is what correlation costs";
}

TEST_F(ExecSubqueryTest, ACheapPredicateRejectsBeforeTheSubqueryIsPaidFor) {
    // A sub-chain is the expensive conjunct, so an ordinary predicate that
    // already rejected the row must run first. Only the one surviving row
    // pays for the subquery.
    ExecStats stats;
    EXPECT_EQ(Run("SELECT acct.name FROM acct WHERE tier = 'silver' AND EXISTS "
                  "(SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)",
                  &stats),
              (std::vector<std::string>{}));
    EXPECT_EQ(stats.Total().sub_chain_runs, 1u) << "only bob survived `tier = silver`";
}

TEST_F(ExecSubqueryTest, ExistsShortCircuitsAtTheFirstQualifyingRow) {
    // A relation with many rows, all of which qualify. EXISTS must stop
    // at the first, so the rows examined inside the sub-chain is 1 rather
    // than the whole relation.
    Create("CREATE TABLE big (id int64, tag varchar)");
    for (int i = 0; i < 20; ++i) Insert("big", {Str("x")});

    ExecStats stats;
    EXPECT_EQ(Run("SELECT acct.name FROM acct WHERE EXISTS (SELECT big.id FROM big)", &stats)
                  .size(),
              3u);
    // 1 from the sub-chain (it stopped) + 3 outer rows.
    EXPECT_EQ(stats.Total().rows_examined, 4u)
        << "EXISTS read more than one row of a relation where the first already qualified";
}

// ---- Nesting --------------------------------------------------------------

TEST_F(ExecSubqueryTest, ASubqueryNestedInsideASubqueryExecutes) {
    EXPECT_EQ(Run("SELECT acct.name FROM acct WHERE EXISTS ("
                  "  SELECT trade.id FROM trade WHERE trade.acct_id = acct.id "
                  "  AND EXISTS (SELECT acct.id FROM acct WHERE acct.tier = 'gold'))"),
              (std::vector<std::string>{"alice", "carol"}));
}

TEST_F(ExecSubqueryTest, NestingAtTheDepthCapExecutes) {
    std::string sql = "SELECT acct.name FROM acct WHERE EXISTS (SELECT trade.id FROM trade";
    int opened = 1;
    for (std::uint32_t i = 1; i < parser::kMaxSubqueryDepth; ++i) {
        sql += " WHERE EXISTS (SELECT trade.id FROM trade";
        ++opened;
    }
    for (int i = 0; i < opened; ++i) sql += ")";

    auto rows = TryRun(sql);
    ASSERT_TRUE(rows.ok()) << rows.status().message();
    EXPECT_EQ(rows.value().size(), 3u);
}

TEST_F(ExecSubqueryTest, TheR1GuardStaysUntrippedWithSubChainsInPlay) {
    // The nested case is the one R1 exists for: a sub-chain fetches pages
    // while the outer walk is mid-flight. The outer step must have
    // released its span before descending.
    ResetPageSpanGuard();
    ASSERT_FALSE(PageSpanGuardTripped());

    Run("SELECT acct.name FROM acct WHERE EXISTS "
        "(SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)");
    EXPECT_FALSE(PageSpanGuardTripped())
        << "a page was fetched while a page-frame span was still registered live";
}

}  // namespace
}  // namespace kds::exec
