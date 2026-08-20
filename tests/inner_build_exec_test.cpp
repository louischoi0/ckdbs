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

// JB3 (docs/workplan-join-inner-build.md) — the lazy build at the executor's
// walked-join site. The two done-conditions, each pinned here:
//
//   1. The first outer row's reply — and every reply — is byte-identical to
//      the un-built walk's: emission goes through the same full residual,
//      split into the same conjuncts, evaluated by the same body
//      (EvaluateConjunct). The expected vectors below are hand-computed
//      walk-order results, exactly what the pre-JB3 executor answered.
//
//   2. The map after the first outer row holds every inner row passing the
//      step's NON-correlated residual — matching the outer key or not —
//      pinned through `build_rows` against hand-computed relations.

namespace kds::exec {
namespace {

class InnerBuildExecTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 4000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));

        // au(id, name) drives; tr(id, au_id, qty) is the walked inner —
        // au_id is non-pk, unindexed, uncabined, so the join compiles to
        // the annotated kScan (JB1) and nothing else serves it.
        Create("CREATE TABLE au (id int64, name varchar)");
        Create("CREATE TABLE tr (id int64, au_id int64, qty int64)");
        Insert("au", {Str("alice")});  // id 1
        Insert("au", {Str("bob")});    // id 2
        Insert("au", {Str("carol")});  // id 3
        // Walk order is insertion order here (ASSIGNED ids ascend).
        Insert("tr", {Int(1), Int(10)});
        Insert("tr", {Int(2), Int(20)});
        Insert("tr", {Int(1), Int(30)});
        Insert("tr", {Int(9), Int(5)});  // matches no au row, ever
        Insert("tr", {Int(2), Int(50)});
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
        auto created = boot_->catalog.CreateTable(catalog::kNamespacePublic, ct.table_name,
                                                  schema, ct.clustered, catalog::KeyMode::kAssigned);
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

    StepChain CompileSql(const std::string& sql) {
        auto parsed = parser::Parse(sql);
        EXPECT_TRUE(parsed.ok()) << parsed.status().message();
        auto chain = Compile(boot_->catalog, std::get<parser::SelectStmt>(parsed.value()));
        EXPECT_TRUE(chain.ok()) << chain.status().message();
        return chain.ok() ? chain.value() : StepChain{};
    }

    // Runs and renders rows as "a|b", walk order preserved; `stats` filled.
    std::vector<std::string> Run(const std::string& sql, ExecStats& stats,
                                 std::size_t stop_after = 0) {
        const StepChain chain = CompileSql(sql);
        std::vector<std::string> rows;
        Status ran = Execute(
            boot_->catalog, store_, chain,
            [&](const ChainFrame& frame) -> StatusOr<storage::VisitControl> {
                std::string row;
                for (const ColumnRef& ref : chain.projection) {
                    if (!row.empty()) row += '|';
                    row += FormatValue(/*type_val=*/0, frame.Get(ref));
                }
                rows.push_back(std::move(row));
                return (stop_after != 0 && rows.size() >= stop_after)
                           ? storage::VisitControl::kStop
                           : storage::VisitControl::kContinue;
            },
            &stats);
        EXPECT_TRUE(ran.ok()) << ran.message();
        return rows;
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
};

TEST_F(InnerBuildExecTest, TheWalkedJoinAnswersIdenticallyAndBuildsOnce) {
    // Done-condition 1: hand-computed, walk order — per outer row (au 1,
    // 2, 3), the inner relation in insertion order, full residual applied.
    // The pre-JB3 executor's answer verbatim.
    // Done-condition 2, count form: no non-correlated residual, so every
    // visible tr row enters the map — including (au_id=9), which matches
    // no outer row ever — and three outer rows build once: 5, not 15.
    ExecStats stats;
    const std::vector<std::string> rows =
        Run("SELECT au.name, tr.qty FROM au JOIN tr ON tr.au_id = au.id", stats);
    EXPECT_EQ(rows, (std::vector<std::string>{"alice|10", "alice|30", "bob|20", "bob|50"}));
    const StepStats total = stats.Total();
    EXPECT_EQ(total.inner_builds, 1u);
    EXPECT_EQ(total.build_rows, 5u);
}

TEST_F(InnerBuildExecTest, ANestedAnnotatedStepBuildsUnderALiveOuterBuild) {
    // The `building_` save/restore, proven rather than asserted: two
    // annotated steps, the deeper one arming its build while the outer
    // one's is live. The outer map still buckets all five tr rows —
    // including the ones walked after the nested build ran — and each
    // step publishes exactly once.
    Create("CREATE TABLE ln (id int64, tr_qty int64, amt int64)");
    Insert("ln", {Int(10), Int(100)});
    Insert("ln", {Int(10), Int(101)});
    Insert("ln", {Int(30), Int(300)});
    Insert("ln", {Int(50), Int(500)});

    ExecStats stats;
    const std::vector<std::string> rows =
        Run("SELECT au.name, tr.qty, ln.amt FROM au JOIN tr ON tr.au_id = au.id "
            "JOIN ln ON ln.tr_qty = tr.qty",
            stats);
    EXPECT_EQ(rows, (std::vector<std::string>{"alice|10|100", "alice|10|101", "alice|30|300",
                                              "bob|50|500"}));
    EXPECT_EQ(stats.Total().inner_builds, 2u);
    ASSERT_GE(stats.steps.size(), 3u);
    EXPECT_EQ(stats.steps[1].build_rows, 5u) << "the outer build bucketed every tr row";
    EXPECT_EQ(stats.steps[2].build_rows, 4u) << "the nested build bucketed every ln row";
}

TEST_F(InnerBuildExecTest, TheMapHoldsExactlyTheRowsPassingTheNonCorrelatedResidual) {
    // A non-correlated conjunct joins the step: `tr.qty <= 30` buckets
    // qty 10, 20, 30 and 5 — the last matching no outer key, bucketed
    // under its own value regardless — and excludes qty 50 outright.
    // Emission still applies the full residual: bob's qty 50 row is gone
    // from the reply too, by the same conjunct that kept it out of the map.
    ExecStats stats;
    const std::vector<std::string> rows = Run(
        "SELECT au.name, tr.qty FROM au JOIN tr ON tr.au_id = au.id WHERE tr.qty <= 30", stats);
    EXPECT_EQ(rows, (std::vector<std::string>{"alice|10", "alice|30", "bob|20"}));
    const StepStats total = stats.Total();
    EXPECT_EQ(total.inner_builds, 1u);
    EXPECT_EQ(total.build_rows, 4u);
}

TEST_F(InnerBuildExecTest, AStoppedFirstWalkPublishesNoMap) {
    // The sink stops the statement on its first row, mid-way through the
    // first inner walk: the rows after the stop were never bucketed, so
    // the partial map must not publish — WalkAndRecord's completed-walk
    // rule, applied to the build.
    ExecStats stats;
    const std::vector<std::string> rows =
        Run("SELECT au.name, tr.qty FROM au JOIN tr ON tr.au_id = au.id", stats,
            /*stop_after=*/1);
    EXPECT_EQ(rows, (std::vector<std::string>{"alice|10"}));
    EXPECT_EQ(stats.Total().inner_builds, 0u);
}

TEST_F(InnerBuildExecTest, AnExistsSubChainBuildsNothingUntilJB6) {
    // The stopping class is gated out wholesale: a prefix map that served
    // a miss as an absence is the one state the plan forbids, and the
    // gate at the dispatch is the enforcement. The reply is the plain
    // stopping walk's.
    ExecStats stats;
    const std::vector<std::string> rows = Run(
        "SELECT au.name FROM au WHERE EXISTS (SELECT tr.id FROM tr WHERE tr.au_id = au.id)",
        stats);
    EXPECT_EQ(rows, (std::vector<std::string>{"alice", "bob"}));
    const StepStats total = stats.Total();
    EXPECT_EQ(total.inner_builds, 0u);
    EXPECT_EQ(total.build_rows, 0u);
}

}  // namespace
}  // namespace kds::exec
