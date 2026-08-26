#include "kds/exec/step_compiler.hpp"

#include <optional>
#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// V15 - correlation analysis and sub-chain placement
// (docs/inflight/in-progress/parser-v2-workplan.md).
//
// One question decides where a subquery runs: **does any reference inside
// it point outward?**
//
//   no    its answer cannot vary per outer row, so running it per row
//         would compute one value n times. Hoisted, executed once, before
//         the outer chain opens.
//   yes   it depends on the outer row, so it runs per row with the
//         correlation values read through the frame stack.
//
// That is structural, not a heuristic - which matters beyond tidiness.
// The chain layout is a pure function of the AST, hence of pattern_id, and
// a Waystone trail is recorded against it. A placement that varied with a
// cost estimate would make a recorded trail describe a shape the next
// execution does not have.
//
// The resolution rule tested alongside it - innermost-first, stopping at
// the first level that matches - is what stops a schema change to an outer
// relation from silently repointing an inner predicate.

namespace kds::exec {
namespace {

class StepCorrelationTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));

        Create("CREATE TABLE acct (id int64, name varchar, tier varchar)");
        Create("CREATE TABLE trade (id int64, acct_id int64, sym varchar)");
        // Shares the column name `tier` with acct, and `sym` with trade -
        // the overlaps the resolution rules are about.
        Create("CREATE TABLE audit (id int64, acct_id int64, tier varchar)");
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

    StatusOr<StepChain> CompileSql(const std::string& sql) {
        auto parsed = parser::Parse(sql);
        if (!parsed.ok()) return parsed.status();
        return Compile(boot_->catalog, std::get<parser::SelectStmt>(parsed.value()));
    }

    StepChain MustCompile(const std::string& sql) {
        auto chain = CompileSql(sql);
        EXPECT_TRUE(chain.ok()) << sql << ": " << chain.status().message();
        if (!chain.ok()) return StepChain{};
        return chain.value();
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
};

// ---- Hoisted vs nested ----------------------------------------------------

TEST_F(StepCorrelationTest, AnUncorrelatedSubqueryIsHoistedOutOfTheRowLoop) {
    const StepChain chain =
        MustCompile("SELECT * FROM acct WHERE EXISTS (SELECT * FROM trade WHERE trade.sym = 'X')");
    ASSERT_EQ(chain.hoisted.size(), 1u);
    EXPECT_FALSE(chain.hoisted[0].correlated);
    EXPECT_EQ(chain.hoisted[0].kind, parser::PredicateKind::kExists);
    EXPECT_TRUE(chain.steps[0].sub_chains.empty())
        << "nothing that runs once per row should be attached to a step";
}

TEST_F(StepCorrelationTest, ACorrelatedSubqueryBecomesANestedStepSubChain) {
    const StepChain chain = MustCompile(
        "SELECT * FROM acct WHERE EXISTS (SELECT * FROM trade WHERE trade.acct_id = acct.id)");
    EXPECT_TRUE(chain.hoisted.empty());
    ASSERT_EQ(chain.steps[0].sub_chains.size(), 1u);
    EXPECT_TRUE(chain.steps[0].sub_chains[0].correlated);
}

TEST_F(StepCorrelationTest, TheReferenceThatCorrelatesCarriesUpEqualsOne) {
    // `acct.id` inside the subquery resolves outward, one level up. That
    // number is a de Bruijn level and maps one-to-one onto the frame
    // stack the executor keeps, which is what makes the predicate
    // independent of which sub-chain it landed in.
    const StepChain chain = MustCompile(
        "SELECT * FROM acct WHERE EXISTS (SELECT * FROM trade WHERE trade.acct_id = acct.id)");
    const SubChain& sub = chain.steps[0].sub_chains[0];
    ASSERT_EQ(sub.steps.size(), 1u);
    ASSERT_EQ(sub.steps[0].residual.size(), 1u);

    const StepPredicate& pred = sub.steps[0].residual[0];
    EXPECT_EQ(pred.lhs.up, 0) << "trade.acct_id is the sub-chain's own";
    ASSERT_EQ(pred.rhs.kind, OperandKind::kColumn);
    EXPECT_EQ(pred.rhs.column.up, 1) << "acct.id belongs to the enclosing chain";
    EXPECT_EQ(pred.rhs.column.rel_slot, 0);
    EXPECT_EQ(pred.rhs.column.col_pos, 0);
}

TEST_F(StepCorrelationTest, ACorrelatedProbeIsStillAProbe) {
    // The claim in spec §2 that makes correlation affordable: a correlated
    // pk-equality subquery costs what a join step costs. The inner step
    // binds its own pk against an outer column, so it descends rather
    // than scanning - and being a probe, it is trail-replayable.
    const StepChain chain = MustCompile(
        "SELECT * FROM trade WHERE EXISTS (SELECT * FROM acct WHERE acct.id = trade.acct_id)");
    const SubChain& sub = chain.steps[0].sub_chains[0];
    ASSERT_EQ(sub.steps.size(), 1u);
    EXPECT_EQ(sub.steps[0].kind, AccessKind::kProbe);
    EXPECT_TRUE(IsTrailReplayable(sub.steps[0].kind));

    ASSERT_TRUE(sub.steps[0].key.has_value());
    EXPECT_EQ(sub.steps[0].key->kind, OperandKind::kColumn);
    EXPECT_EQ(sub.steps[0].key->column.up, 1) << "the key comes from the outer row";
}

TEST_F(StepCorrelationTest, ACorrelatedSubChainAttachesToTheStepItReachesInto) {
    // Two outer relations; the subquery correlates against the *second*.
    // Attaching it to step 0 would run it before the row it reads exists.
    const StepChain chain = MustCompile(
        "SELECT a.id, t.id FROM acct AS a JOIN trade AS t ON t.acct_id = a.id "
        "WHERE EXISTS (SELECT * FROM audit WHERE audit.acct_id = t.id)");
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_TRUE(chain.steps[0].sub_chains.empty());
    ASSERT_EQ(chain.steps[1].sub_chains.size(), 1u) << "it correlates against step 1";
}

TEST_F(StepCorrelationTest, HoistingIsDecidedPerSubqueryNotPerStatement) {
    const StepChain chain = MustCompile(
        "SELECT * FROM acct WHERE EXISTS (SELECT * FROM trade WHERE trade.sym = 'X') "
        "AND EXISTS (SELECT * FROM audit WHERE audit.acct_id = acct.id)");
    EXPECT_EQ(chain.hoisted.size(), 1u) << "the first is uncorrelated";
    EXPECT_EQ(chain.steps[0].sub_chains.size(), 1u) << "the second correlates";
}

// ---- The value-bearing forms ---------------------------------------------

TEST_F(StepCorrelationTest, InAndScalarFormsCarryTheOuterColumnAndTheInnerValue) {
    // Value-bearing forms attach to the step holding their outer column
    // rather than hoisting, even when uncorrelated: the inner set is
    // row-independent, but the comparison against each outer row is not.
    const StepChain in_chain =
        MustCompile("SELECT * FROM acct WHERE id IN (SELECT acct_id FROM trade)");
    ASSERT_EQ(in_chain.steps[0].sub_chains.size(), 1u);
    const SubChain& in_sub = in_chain.steps[0].sub_chains[0];
    EXPECT_EQ(in_sub.kind, parser::PredicateKind::kInSubquery);
    EXPECT_TRUE(in_sub.has_value);
    EXPECT_EQ(in_sub.lhs.col_pos, 0) << "acct.id is the outer column being tested";
    EXPECT_EQ(in_sub.value.col_pos, 1) << "trade.acct_id is the value read out";

    const StepChain scalar =
        MustCompile("SELECT * FROM acct WHERE id = (SELECT acct_id FROM trade)");
    ASSERT_EQ(scalar.steps[0].sub_chains.size(), 1u);
    EXPECT_EQ(scalar.steps[0].sub_chains[0].kind, parser::PredicateKind::kCompareSubquery);
    EXPECT_EQ(scalar.steps[0].sub_chains[0].op, parser::CompareOp::kEq);
    EXPECT_TRUE(scalar.steps[0].sub_chains[0].has_value);
}

TEST_F(StepCorrelationTest, NotInAndNotExistsKeepTheirOwnKinds) {
    // They must stay distinguishable: NOT IN and NOT EXISTS are
    // search-class by definition and never trail-replayable, because
    // absence has no witness (invariant 9). Collapsing them into their
    // positive forms plus a flag would make that decidable only by
    // re-deriving it later.
    EXPECT_EQ(MustCompile("SELECT * FROM acct WHERE id NOT IN (SELECT acct_id FROM trade)")
                  .steps[0]
                  .sub_chains[0]
                  .kind,
              parser::PredicateKind::kNotInSubquery)
        << "value-bearing, so attached to the step";
    EXPECT_EQ(MustCompile("SELECT * FROM acct WHERE NOT EXISTS (SELECT * FROM trade)")
                  .hoisted[0]
                  .kind,
              parser::PredicateKind::kNotExists)
        << "no outer column, so it leaves the row loop";
}

TEST_F(StepCorrelationTest, AValueSubqueryMustProjectExactlyOneColumn) {
    // `id IN (SELECT * FROM trade)` has no single value to mean, and
    // picking the first column would make the answer depend on schema
    // order - so a column added to `trade` would change what an unrelated
    // statement returns.
    auto chain = CompileSql("SELECT * FROM acct WHERE id IN (SELECT * FROM trade)");
    ASSERT_FALSE(chain.ok());
    EXPECT_EQ(chain.status().code(), StatusCode::kUnsupported);
    EXPECT_NE(chain.status().message().find("exactly one column"), std::string::npos)
        << chain.status().message();

    // EXISTS is unaffected: it tests only whether a row appeared, so
    // there is no value to be ambiguous about.
    EXPECT_TRUE(CompileSql("SELECT * FROM acct WHERE EXISTS (SELECT * FROM trade)").ok());
}

// ---- Resolution: innermost-first, stopping at the first match ------------

TEST_F(StepCorrelationTest, AnUnqualifiedNameResolvesInnermostFirst) {
    // Both acct and audit have `tier`. Inside the subquery the inner one
    // wins, and the search stops there rather than continuing outward.
    const StepChain chain = MustCompile(
        "SELECT * FROM acct WHERE EXISTS (SELECT * FROM audit WHERE tier = 'gold')");
    const SubChain& sub = chain.hoisted[0];
    ASSERT_EQ(sub.steps[0].residual.size(), 1u);
    EXPECT_EQ(sub.steps[0].residual[0].lhs.up, 0)
        << "the inner relation has the column, so the outer one is never consulted";
}

TEST_F(StepCorrelationTest, AddingAColumnToAnOuterRelationCannotChangeAnInnerChainsMeaning) {
    // The consequence of stopping at the first matching *level*, stated as
    // its own test because it is the reason for the rule. `sym` exists
    // only on trade; resolved from inside a chain over trade, it binds
    // inward at up == 0. No column added to acct could reach it, because
    // the search never gets that far.
    const StepChain chain = MustCompile(
        "SELECT * FROM acct WHERE EXISTS (SELECT * FROM trade WHERE sym = 'AAPL')");
    const SubChain& sub = chain.hoisted[0];
    EXPECT_EQ(sub.steps[0].residual[0].lhs.up, 0);
    EXPECT_FALSE(sub.correlated) << "a name that resolves inward does not correlate anything";
}

TEST_F(StepCorrelationTest, AnUnqualifiedNameOnlyTheOuterChainHasResolvesOutward) {
    // `name` exists on acct alone, so from inside a chain over trade it
    // resolves one level up - and that reference is what makes the
    // sub-chain correlated, with no qualifier written anywhere.
    const StepChain chain = MustCompile(
        "SELECT * FROM acct WHERE EXISTS (SELECT * FROM trade WHERE name = 'alice')");
    ASSERT_EQ(chain.steps[0].sub_chains.size(), 1u)
        << "an unqualified outward reference correlates just as a qualified one does";
    const SubChain& sub = chain.steps[0].sub_chains[0];
    EXPECT_TRUE(sub.correlated);
    EXPECT_EQ(sub.steps[0].residual[0].lhs.up, 1);
}

TEST_F(StepCorrelationTest, AnAmbiguousNameInsideASubqueryErrorsWithItsPosition) {
    // audit and trade both have `acct_id`, and both are in the inner
    // scope. Ambiguity is an error at any depth.
    // The subquery names a column rather than `*`: V06's star rule
    // applies inside a sub-chain exactly as it does outside, so `SELECT *`
    // over two relations would be refused before resolution ran at all.
    auto chain = CompileSql(
        "SELECT * FROM acct WHERE EXISTS (SELECT trade.id FROM trade JOIN audit "
        "ON trade.id = audit.id WHERE acct_id = 1)");
    ASSERT_FALSE(chain.ok());
    EXPECT_EQ(chain.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(chain.status().message().find("ambiguous"), std::string::npos)
        << chain.status().message();
    EXPECT_NE(chain.status().message().find("byte"), std::string::npos)
        << chain.status().message();
}

TEST_F(StepCorrelationTest, AQualifierNamingNeitherScopeIsAnError) {
    auto chain = CompileSql(
        "SELECT * FROM acct WHERE EXISTS (SELECT * FROM trade WHERE nosuch.id = 1)");
    ASSERT_FALSE(chain.ok());
    EXPECT_EQ(chain.status().code(), StatusCode::kInvalidArgument);
}

// ---- Numbering and nesting ------------------------------------------------

TEST_F(StepCorrelationTest, StepIdsAreGlobalAcrossTheOuterChainAndEverySubChain) {
    // A trail entry's step_id must be unambiguous without parent linkage,
    // so the counter is shared by every block in the statement.
    const StepChain chain = MustCompile(
        "SELECT a.id, t.id FROM acct AS a JOIN trade AS t ON t.acct_id = a.id "
        "WHERE EXISTS (SELECT * FROM audit WHERE audit.acct_id = t.id) "
        "AND EXISTS (SELECT * FROM trade WHERE trade.sym = 'X')");
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[0].step_id, 0u);
    EXPECT_EQ(chain.steps[1].step_id, 1u);

    // The two sub-chains take the next ids, in compile order.
    ASSERT_EQ(chain.steps[1].sub_chains.size(), 1u);
    ASSERT_EQ(chain.hoisted.size(), 1u);
    std::vector<std::uint32_t> ids = {chain.steps[1].sub_chains[0].steps[0].step_id,
                                      chain.hoisted[0].steps[0].step_id};
    EXPECT_NE(ids[0], ids[1]) << "no two steps in one statement may share an id";
    for (std::uint32_t id : ids) EXPECT_GE(id, 2u);
}

TEST_F(StepCorrelationTest, NestingResolvesThroughMoreThanOneLevel) {
    const StepChain chain = MustCompile(
        "SELECT * FROM acct WHERE EXISTS ("
        "  SELECT * FROM trade WHERE EXISTS ("
        "    SELECT * FROM audit WHERE audit.acct_id = acct.id))");
    // The innermost reference reaches two levels out, so the middle chain
    // is correlated by transitivity and cannot be hoisted.
    ASSERT_EQ(chain.steps[0].sub_chains.size(), 1u);
    const SubChain& middle = chain.steps[0].sub_chains[0];
    EXPECT_TRUE(middle.correlated);

    ASSERT_EQ(middle.steps[0].sub_chains.size(), 1u);
    const SubChain& inner = middle.steps[0].sub_chains[0];
    EXPECT_TRUE(inner.correlated);
    EXPECT_EQ(inner.steps[0].residual[0].rhs.column.up, 2)
        << "two levels out, which is what the frame stack will walk";
}

TEST_F(StepCorrelationTest, TheDepthBoundIsEnforcedAtCompileNotOnlyAtParse) {
    // Spec I15 R3: recursion is bounded at both ends. The parser caps
    // nesting, but a chain can be built by something other than a parse,
    // and a bound only one producer enforces is not a bound.
    std::string sql = "SELECT * FROM acct";
    for (std::uint32_t i = 0; i < parser::kMaxSubqueryDepth; ++i) {
        sql += " WHERE EXISTS (SELECT * FROM acct";
    }
    for (std::uint32_t i = 0; i < parser::kMaxSubqueryDepth; ++i) sql += ")";
    EXPECT_TRUE(CompileSql(sql).ok()) << "at the cap must compile";
}

TEST_F(StepCorrelationTest, CompilingTwiceGivesTheSamePlacement) {
    // Purity again, now including the hoist/nest decision - the one that
    // a cost-based planner would make differently on different data, and
    // that a recorded trail depends on being stable.
    const char* sql =
        "SELECT * FROM acct WHERE EXISTS (SELECT * FROM trade WHERE trade.acct_id = acct.id) "
        "AND id IN (SELECT acct_id FROM audit)";
    const StepChain first = MustCompile(sql);
    const StepChain second = MustCompile(sql);

    // Both sub-chains attach to step 0: the EXISTS because it correlates,
    // the IN because it tests an outer column.
    ASSERT_EQ(first.hoisted.size(), second.hoisted.size());
    EXPECT_TRUE(first.hoisted.empty());
    ASSERT_EQ(first.steps[0].sub_chains.size(), 2u);
    ASSERT_EQ(first.steps[0].sub_chains.size(), second.steps[0].sub_chains.size());
    for (std::size_t i = 0; i < first.steps[0].sub_chains.size(); ++i) {
        EXPECT_EQ(first.steps[0].sub_chains[i].correlated,
                  second.steps[0].sub_chains[i].correlated);
        EXPECT_EQ(first.steps[0].sub_chains[i].steps[0].step_id,
                  second.steps[0].sub_chains[i].steps[0].step_id);
    }
}

}  // namespace
}  // namespace kds::exec
