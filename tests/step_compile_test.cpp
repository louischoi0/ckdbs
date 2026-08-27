#include "kds/exec/step_compiler.hpp"

#include <optional>
#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// V14 - the step compiler (docs/inflight/in-progress/parser-v2-workplan.md).
//
// Two properties carry this task, and neither is about any single chain:
//
//   purity      same statement plus same catalog gives the same chain,
//               bit for bit. The blueprint parser's acceptance criterion
//               (phase V-6) is that it emits *identical* chains, which is
//               a checkable statement only if identical means something.
//
//   the kind    a step is Lookup/Probe iff its equality binds the pk. That
//               single line is simultaneously the executor's probe
//               strategy and Waystone's replayable/not-replayable
//               boundary - one decision with two consumers, so getting it
//               wrong is not a slow query, it is a trail that may be
//               trusted where it must not be.

namespace kds::exec {
namespace {

class StepCompileTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));

        // acct(id, name, tier) and trade(id, acct_id, sym). `id` is the
        // pk of each by invariant 11 - the first column always is.
        Create("CREATE TABLE acct (id int64, name varchar, tier varchar)");
        Create("CREATE TABLE trade (id int64, acct_id int64, sym varchar)");
    }

    // Builds the schema the way HandleCreateTableSql does - through
    // ResolveTypeByName against sys.types - so these tables are the same
    // shape a real CREATE TABLE produces.
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

// ---- Access kinds: the line that is also Waystone's trust boundary -------

TEST_F(StepCompileTest, PkEqualityAgainstALiteralCompilesToLookup) {
    const StepChain chain = MustCompile("SELECT * FROM acct WHERE id = 7");
    ASSERT_EQ(chain.steps.size(), 1u);
    EXPECT_EQ(chain.steps[0].kind, AccessKind::kLookup);
    ASSERT_TRUE(chain.steps[0].key.has_value());
    EXPECT_EQ(chain.steps[0].key->kind, OperandKind::kLiteral);
    EXPECT_EQ(chain.steps[0].key->literal.int_val, 7);
    EXPECT_TRUE(IsTrailReplayable(chain.steps[0].kind));
}

TEST_F(StepCompileTest, EqualityOnANonPkColumnIsAFilterScanHoweverSelective) {
    // `name` may be unique in practice; it is not the pk, and only the pk
    // can be addressed by a descent (invariant 11).
    //
    // It is a **kFilterScan** rather than a bare kScan - a walk that exists
    // to evaluate a filter, which is the shape a physical optimizer wants
    // to hear about. That is a statistics distinction and nothing more:
    // the step still walks the whole relation, and the assertion that
    // matters is the last one. A filter scan is a *search*, and a search is
    // never trail-replayable, so promoting it would be a correctness bug in
    // Waystone rather than a missed optimization.
    const StepChain chain = MustCompile("SELECT * FROM acct WHERE name = 'alice'");
    ASSERT_EQ(chain.steps.size(), 1u);
    EXPECT_EQ(chain.steps[0].kind, AccessKind::kFilterScan);
    EXPECT_FALSE(chain.steps[0].key.has_value());
    EXPECT_FALSE(IsTrailReplayable(chain.steps[0].kind));

    // And the column it was classified for is recorded, which is what the
    // access statistics key on.
    ASSERT_EQ(chain.steps[0].access_columns.size(), 1u);
    EXPECT_NE(chain.steps[0].access_columns[0], 0) << "the pk is not a filter column";
}

TEST_F(StepCompileTest, ABareSelectIsAPlainScanWithNoAccessColumns) {
    // The other half of the split: nothing steered this walk, so it is a
    // kScan and its access shape is empty.
    const StepChain chain = MustCompile("SELECT * FROM acct");
    ASSERT_EQ(chain.steps.size(), 1u);
    EXPECT_EQ(chain.steps[0].kind, AccessKind::kScan);
    EXPECT_TRUE(chain.steps[0].access_columns.empty());
}

TEST_F(StepCompileTest, BetweenOnThePkCompilesToARangeWithItsBoundsKept) {
    const StepChain chain = MustCompile("SELECT * FROM acct WHERE id BETWEEN 10 AND 20");
    ASSERT_EQ(chain.steps.size(), 1u);
    EXPECT_EQ(chain.steps[0].kind, AccessKind::kRange);
    ASSERT_TRUE(chain.steps[0].range.has_value());
    EXPECT_EQ(chain.steps[0].range->low, 10u);
    EXPECT_EQ(chain.steps[0].range->high, 20u);

    // **The bounds are also still conjuncts.** The range is a hint on top
    // of the residual, never a replacement for it - which is what keeps
    // "downgrading any step to a plain scan cannot change the result"
    // true, and that property is what invariant 9's fall-through rests on.
    EXPECT_EQ(chain.steps[0].residual.size(), 2u);
    EXPECT_FALSE(IsTrailReplayable(chain.steps[0].kind)) << "a range is a search";

    // Spelling it out by hand is the same statement, so it gets the same
    // range. An optimizer that rewarded phrasing would be a worse one.
    const StepChain spelled = MustCompile("SELECT * FROM acct WHERE id >= 10 AND id <= 20");
    EXPECT_EQ(spelled.steps[0].kind, AccessKind::kRange);
    ASSERT_TRUE(spelled.steps[0].range.has_value());
    EXPECT_EQ(spelled.steps[0].range->low, 10u);
    EXPECT_EQ(spelled.steps[0].range->high, 20u);
}

TEST_F(StepCompileTest, BetweenOnANonPkColumnIsNotARange) {
    // There is no structure to exploit: the relation is ordered by pk, so
    // a range over any other column is a search that has to look at every
    // row. Calling it a Range would be a promise the storage cannot keep.
    const StepChain chain = MustCompile("SELECT * FROM acct WHERE tier BETWEEN 'a' AND 'z'");
    ASSERT_EQ(chain.steps.size(), 1u);
    EXPECT_NE(chain.steps[0].kind, AccessKind::kRange);
    EXPECT_FALSE(chain.steps[0].range.has_value());
    EXPECT_EQ(chain.steps[0].residual.size(), 2u);
}

TEST_F(StepCompileTest, AnInvertedRangeStaysAPlainScan) {
    // `BETWEEN 20 AND 10` matches nothing and is legal to write. The
    // residual already returns the correct empty answer, so there is
    // nothing for a range walk to do but special-case it.
    const StepChain chain = MustCompile("SELECT * FROM acct WHERE id BETWEEN 20 AND 10");
    ASSERT_EQ(chain.steps.size(), 1u);
    EXPECT_FALSE(chain.steps[0].range.has_value());
}

TEST_F(StepCompileTest, AJoinOntoAPkCompilesToProbe) {
    // The shape the whole execution model is built for: the second step
    // descends to exactly one row per outer row.
    const StepChain chain =
        MustCompile("SELECT acct.name, trade.sym FROM trade JOIN acct ON trade.acct_id = acct.id");
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[0].kind, AccessKind::kScan) << "the driving relation has no key";
    EXPECT_EQ(chain.steps[1].kind, AccessKind::kProbe);

    ASSERT_TRUE(chain.steps[1].key.has_value());
    EXPECT_EQ(chain.steps[1].key->kind, OperandKind::kColumn);
    // The key comes from step 0 - an earlier step, which is what makes it
    // available when the descent happens.
    EXPECT_EQ(chain.steps[1].key->column.rel_slot, 0);
    EXPECT_EQ(chain.steps[1].key->column.up, 0);
}

TEST_F(StepCompileTest, AJoinOnANonPkColumnCompilesToScan) {
    // Same statement shape, different column: `acct.name` is not a pk, so
    // the second relation must be walked. The workplan names this pair
    // explicitly because the two must not be conflated.
    const StepChain chain =
        MustCompile("SELECT acct.id, trade.id FROM trade JOIN acct ON trade.sym = acct.name");
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[1].kind, AccessKind::kScan);
    EXPECT_FALSE(chain.steps[1].key.has_value());
}

TEST_F(StepCompileTest, AProbeKeyMustComeFromAnEarlierStepNotALaterOne) {
    // Written the other way round: `acct` is the driving relation and the
    // ON binds acct.id, which is step 0's own pk against step 1's column.
    // Step 0 cannot probe on a value step 1 has not produced yet, so it
    // stays a scan - and step 1 is a scan too, since trade.acct_id is not
    // trade's pk.
    const StepChain chain =
        MustCompile("SELECT acct.name, trade.sym FROM acct JOIN trade ON acct.id = trade.acct_id");
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[0].kind, AccessKind::kScan);
    EXPECT_EQ(chain.steps[1].kind, AccessKind::kScan);
}

TEST_F(StepCompileTest, ANegativeLiteralPkIsAScanRatherThanAProbeIntoAHugeKey) {
    // Ids are zero-extended 40-bit values (invariant 7), so `id = -1` can
    // never hold. Compiling it to a lookup would cast the literal to an
    // enormous unsigned key; as a scan with the residual intact it
    // returns the correct empty answer.
    const StepChain chain = MustCompile("SELECT * FROM acct WHERE id = -1");
    EXPECT_EQ(chain.steps[0].kind, AccessKind::kScan);
    ASSERT_EQ(chain.steps[0].residual.size(), 1u);
    EXPECT_EQ(chain.steps[0].residual[0].rhs.literal.int_val, -1);
}

TEST_F(StepCompileTest, AWhereClauseDoesNotDowngradeALookupToAScan) {
    // The PkEqualityTarget trap the workplan calls out. That helper
    // refuses whenever the WHERE holds more than one condition - correct
    // for a point statement, wrong for a chain step, which only locates a
    // candidate and evaluates the rest on the located row.
    const StepChain chain = MustCompile("SELECT * FROM acct WHERE id = 7 AND tier = 'gold'");
    ASSERT_EQ(chain.steps.size(), 1u);
    EXPECT_EQ(chain.steps[0].kind, AccessKind::kLookup)
        << "a second predicate must not degrade the step to a full scan";
    EXPECT_EQ(chain.steps[0].residual.size(), 2u);
}

// ---- The residual list ----------------------------------------------------

TEST_F(StepCompileTest, TheKeyIsAlsoKeptAsAResidualSoAProbeAndAScanAgree) {
    // The property this repetition buys: the residual list alone fully
    // expresses the statement's predicate, so downgrading any Lookup or
    // Probe to a Scan cannot change the result. That is what makes a
    // Waystone miss safe to fall through (invariant 9) - the fallback
    // walk filters on exactly the same list.
    const StepChain chain = MustCompile("SELECT * FROM acct WHERE id = 7");
    ASSERT_EQ(chain.steps[0].residual.size(), 1u);
    EXPECT_EQ(chain.steps[0].residual[0].lhs.col_pos, 0);
    EXPECT_EQ(chain.steps[0].residual[0].op, parser::CompareOp::kEq);
    EXPECT_EQ(chain.steps[0].residual[0].rhs.literal.int_val, 7);
}

TEST_F(StepCompileTest, EachConjunctLandsOnTheStepThatMakesItEvaluable) {
    const StepChain chain = MustCompile(
        "SELECT acct.name, trade.sym FROM trade JOIN acct ON trade.acct_id = acct.id "
        "WHERE trade.sym = 'AAPL' AND acct.tier = 'gold'");
    ASSERT_EQ(chain.steps.size(), 2u);

    // trade.sym is readable at step 0, so filtering there stops rows from
    // reaching the probe at all. acct.tier needs step 1's row.
    ASSERT_EQ(chain.steps[0].residual.size(), 1u);
    EXPECT_EQ(chain.steps[0].residual[0].lhs.rel_slot, 0);

    // Step 1 carries the join equality plus its own conjunct.
    ASSERT_EQ(chain.steps[1].residual.size(), 2u);

    // The invariant that actually matters, for every step: a predicate
    // may not reference a relation the chain has not reached yet. Note
    // this is NOT "both sides live on step i" - the join equality on step
    // 1 has its left side on relation 0, which is exactly why it becomes
    // evaluable only once relation 1 is bound.
    for (std::size_t i = 0; i < chain.steps.size(); ++i) {
        for (const StepPredicate& pred : chain.steps[i].residual) {
            EXPECT_LE(pred.lhs.rel_slot, i) << "step " << i << " reads a later relation";
            if (pred.rhs.kind == OperandKind::kColumn) {
                EXPECT_LE(pred.rhs.column.rel_slot, i) << "step " << i << " reads a later relation";
            }
        }
    }
}

TEST_F(StepCompileTest, AJoinPredicateIsAConjunctLikeAnyOther) {
    // ON and WHERE become one flat list: to the executor they are the
    // same thing, a condition the row must satisfy. Only an outer join
    // would make them differ, and outer joins are Unsupported.
    const StepChain chain =
        MustCompile("SELECT acct.id, trade.id FROM trade JOIN acct ON trade.sym = acct.name");
    ASSERT_EQ(chain.steps[1].residual.size(), 1u);
    EXPECT_EQ(chain.steps[1].residual[0].rhs.kind, OperandKind::kColumn);
}

// ---- Equality propagation (docs/spec/parser-v2.md §5) --------------------------

TEST_F(StepCompileTest, ALiteralOnTheJoinKeyPropagatesToTheOtherSide) {
    // `acct.id = 7` plus `trade.acct_id = acct.id` implies
    // `trade.acct_id = 7`, and the derived conjunct lands on step 0 - the
    // step that can be keyed on it. bench/results-scenario3-library.md §9
    // is the measured shape this exists for.
    const StepChain chain = MustCompile(
        "SELECT trade.sym FROM trade JOIN acct ON trade.acct_id = acct.id "
        "WHERE acct.id = 7");
    ASSERT_EQ(chain.steps.size(), 2u);

    // Step 0 gained the derived equality. No index exists in this fixture,
    // so the kind is the filter scan the conjunct names; an index on
    // trade(acct_id) is what would key on it (index_compile_test.cpp).
    ASSERT_EQ(chain.steps[0].residual.size(), 1u);
    EXPECT_EQ(chain.steps[0].residual[0].lhs.col_pos, 1);
    EXPECT_EQ(chain.steps[0].residual[0].op, parser::CompareOp::kEq);
    EXPECT_EQ(chain.steps[0].residual[0].rhs.kind, OperandKind::kLiteral);
    EXPECT_EQ(chain.steps[0].residual[0].rhs.literal.int_val, 7);
    EXPECT_EQ(chain.steps[0].kind, AccessKind::kFilterScan);

    // Step 1 is untouched: the probe the written conjuncts chose, and both
    // written conjuncts still in its residual - a derived conjunct may
    // upgrade a step, never displace what was written.
    EXPECT_EQ(chain.steps[1].kind, AccessKind::kProbe);
    EXPECT_EQ(chain.steps[1].residual.size(), 2u);
}

TEST_F(StepCompileTest, PropagationOntoThePkMakesTheStepALookup) {
    // The restriction written against the non-pk side: the implied
    // `acct.id = 7` turns step 0 from the scan
    // AProbeKeyMustComeFromAnEarlierStepNotALaterOne pins into a lookup.
    const StepChain chain = MustCompile(
        "SELECT acct.name FROM acct JOIN trade ON acct.id = trade.acct_id "
        "WHERE trade.acct_id = 7");
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[0].kind, AccessKind::kLookup);
    ASSERT_TRUE(chain.steps[0].key.has_value());
    EXPECT_EQ(chain.steps[0].key->kind, OperandKind::kLiteral);
    EXPECT_EQ(chain.steps[0].key->literal.int_val, 7);
}

TEST_F(StepCompileTest, PropagationCrossesAChainOfJoinKeys) {
    // The class is transitive: one literal on a key three relations share
    // reaches all three, not just the written one's neighbour.
    const StepChain chain = MustCompile(
        "SELECT a.sym FROM trade AS a JOIN trade AS b ON a.acct_id = b.acct_id "
        "JOIN trade AS c ON b.acct_id = c.acct_id WHERE c.acct_id = 5");
    ASSERT_EQ(chain.steps.size(), 3u);
    ASSERT_EQ(chain.steps[0].residual.size(), 1u);
    EXPECT_EQ(chain.steps[0].residual[0].rhs.literal.int_val, 5);
    // Step 1 carries its ON conjunct plus the derived equality.
    ASSERT_EQ(chain.steps[1].residual.size(), 2u);
    EXPECT_EQ(chain.steps[1].residual[1].rhs.kind, OperandKind::kLiteral);
}

TEST_F(StepCompileTest, PropagationRequiresAnIdenticalTypeDescriptor) {
    // The literal was coerced against the column it was written on, and it
    // is copied bytes-for-bytes - so an int64-to-int32 join key does not
    // propagate, it keeps today's plan.
    Create("CREATE TABLE narrow (id int64, small int32)");
    const StepChain chain = MustCompile(
        "SELECT trade.sym FROM trade JOIN narrow ON trade.acct_id = narrow.small "
        "WHERE narrow.small = 7");
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[0].residual.size(), 0u);
    EXPECT_EQ(chain.steps[0].kind, AccessKind::kScan);
}

TEST_F(StepCompileTest, AWrittenConjunctIsNeverDerivedAgain) {
    // Both forms written by hand: nothing to add, and no duplicate residual
    // to evaluate per row.
    const StepChain chain = MustCompile(
        "SELECT trade.sym FROM trade JOIN acct ON trade.acct_id = acct.id "
        "WHERE acct.id = 7 AND trade.acct_id = 7");
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[0].residual.size(), 1u);
    EXPECT_EQ(chain.steps[1].residual.size(), 2u);
}

TEST_F(StepCompileTest, AColumnWithItsOwnWrittenLiteralDerivesNothing) {
    // `acct.id = 7 AND trade.acct_id = 8` is a contradiction, and both
    // columns already carry a written equality-to-literal - so propagation
    // appends nothing. A second literal on a column is plan-inert (a keyed
    // candidate already exists there) and result-inert (the written
    // conjuncts fully express the predicate, contradiction included), and
    // deriving it anyway is the unbounded blowup §5's one-per-column cap
    // exists to stop. The statement still answers empty, by its residual.
    const StepChain chain = MustCompile(
        "SELECT trade.sym FROM trade JOIN acct ON trade.acct_id = acct.id "
        "WHERE acct.id = 7 AND trade.acct_id = 8");
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[0].residual.size(), 1u);
    EXPECT_EQ(chain.steps[1].residual.size(), 2u);
}

TEST_F(StepCompileTest, ADerivedEqualityPromotesAWrittenRangeToALookup) {
    // §5 "plans may only be strengthened": the written conjuncts give
    // step 0 a kRange; the derived `acct.id = 5` gives it a kLookup - a
    // strictly stronger trust class for the same rows - and both written
    // bounds stay in the residual, so the located row is still checked
    // against them.
    const StepChain chain = MustCompile(
        "SELECT acct.name FROM acct JOIN trade ON acct.id = trade.acct_id "
        "WHERE acct.id BETWEEN 1 AND 10 AND trade.acct_id = 5");
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[0].kind, AccessKind::kLookup);
    EXPECT_FALSE(chain.steps[0].range.has_value());
    // The two written bounds plus the derived equality, marked as such.
    ASSERT_EQ(chain.steps[0].residual.size(), 3u);
    EXPECT_TRUE(chain.steps[0].residual[2].derived);
}

TEST_F(StepCompileTest, AParamOnTheJoinKeyPropagatesAndIsMarkedDerived) {
    // §5: a declared pattern's body must compile to the plan the traffic's
    // literal form takes, so `$p` propagates - and the derived conjunct is
    // marked, which is what lets CREATE PATTERN's checks and ANALYZE name
    // only what the client wrote. A `$param` parses only inside a pattern
    // body, so the body is compiled through the declaration's AST.
    auto parsed = parser::Parse(
        "CREATE PATTERN jp($p int64) OF "
        "SELECT acct.name FROM acct JOIN trade ON acct.id = trade.acct_id "
        "WHERE trade.acct_id = $p");
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    const auto& decl = std::get<parser::CreatePatternStmt>(parsed.value());
    auto compiled = Compile(boot_->catalog, *decl.body);
    ASSERT_TRUE(compiled.ok()) << compiled.status().message();
    const StepChain& chain = compiled.value();

    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[0].kind, AccessKind::kLookup);
    ASSERT_EQ(chain.steps[0].residual.size(), 1u);
    EXPECT_TRUE(chain.steps[0].residual[0].derived);
    EXPECT_EQ(chain.steps[0].residual[0].rhs.literal.type, parser::ValueType::kParam);
}

TEST_F(StepCompileTest, PropagationStaysInsideItsOwnBlock) {
    // The sub-chain's `trade.acct_id = acct.id` reaches outward (up == 1),
    // which is not a same-chain edge: no class forms across a block
    // boundary, and the outer block's literal derives nothing inside.
    const StepChain chain = MustCompile(
        "SELECT * FROM acct WHERE id = 7 AND EXISTS "
        "(SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)");
    ASSERT_EQ(chain.steps.size(), 1u);
    ASSERT_EQ(chain.steps[0].sub_chains.size(), 1u);
    const std::vector<Step>& inner = chain.steps[0].sub_chains[0].steps;
    ASSERT_EQ(inner.size(), 1u);
    ASSERT_EQ(inner[0].residual.size(), 1u);
    EXPECT_FALSE(inner[0].residual[0].derived);
}

// ---- Resolution -----------------------------------------------------------

TEST_F(StepCompileTest, ACompiledChainCarriesNoColumnOrRelationNames) {
    // The done-condition: no identifier on any execute path. Names
    // survive only as `column_names`, which labels the result set and is
    // never read while rows are produced.
    const StepChain chain = MustCompile(
        "SELECT acct.name FROM trade JOIN acct ON trade.acct_id = acct.id WHERE acct.tier = 'x'");
    for (const Step& step : chain.steps) {
        EXPECT_NE(step.rel_oid, 0u) << "a relation is an oid here, not a name";
        for (const StepPredicate& pred : step.residual) {
            // A ColumnRef is three integers. There is nowhere for a name
            // to hide - this assertion is really about the type existing
            // in the shape it does.
            EXPECT_LT(pred.lhs.col_pos, 16u);
        }
    }
    EXPECT_EQ(chain.column_names.size(), 1u);
    EXPECT_EQ(chain.column_names[0], "acct.name");
}

TEST_F(StepCompileTest, AliasesResolveAndTheCatalogIsStillLookedUpByTableName) {
    const StepChain chain =
        MustCompile("SELECT a.name, b.sym FROM trade AS b JOIN acct AS a ON b.acct_id = a.id");
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[1].kind, AccessKind::kProbe);
    // Written order: trade is step 0 because it was written first, alias
    // or not.
    EXPECT_NE(chain.steps[0].rel_oid, chain.steps[1].rel_oid);
}

TEST_F(StepCompileTest, ASelfJoinResolvesEachAliasToItsOwnStep) {
    const StepChain chain =
        MustCompile("SELECT a.name, b.name FROM acct AS a JOIN acct AS b ON a.id = b.id");
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[0].rel_oid, chain.steps[1].rel_oid) << "one table";
    ASSERT_EQ(chain.projection.size(), 2u);
    EXPECT_EQ(chain.projection[0].rel_slot, 0) << "but two relations";
    EXPECT_EQ(chain.projection[1].rel_slot, 1);
}

TEST_F(StepCompileTest, AnUnqualifiedNameResolvesWhenExactlyOneRelationHasIt) {
    const StepChain chain =
        MustCompile("SELECT sym, tier FROM trade JOIN acct ON trade.acct_id = acct.id");
    ASSERT_EQ(chain.projection.size(), 2u);
    EXPECT_EQ(chain.projection[0].rel_slot, 0) << "sym is trade's";
    EXPECT_EQ(chain.projection[1].rel_slot, 1) << "tier is acct's";
}

TEST_F(StepCompileTest, AnAmbiguousUnqualifiedNameIsAnErrorNotAChoice) {
    // Both relations have `id`. Picking the first would make the answer
    // depend on written order in a way the client never asked for.
    auto chain = CompileSql("SELECT id FROM trade JOIN acct ON trade.acct_id = acct.id");
    ASSERT_FALSE(chain.ok());
    EXPECT_EQ(chain.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(chain.status().message().find("ambiguous"), std::string::npos)
        << chain.status().message();
    EXPECT_NE(chain.status().message().find("byte"), std::string::npos)
        << "the position is what makes the message actionable: " << chain.status().message();
}

TEST_F(StepCompileTest, AQualifierNamingNoRelationIsAnErrorWithItsPosition) {
    auto chain = CompileSql("SELECT * FROM acct WHERE nosuch.id = 1");
    ASSERT_FALSE(chain.ok());
    EXPECT_EQ(chain.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(chain.status().message().find("names no relation"), std::string::npos)
        << chain.status().message();
}

TEST_F(StepCompileTest, AKnownRelationWithAnUnknownColumnSaysWhichIsWhich) {
    auto chain = CompileSql("SELECT acct.nosuchcol FROM acct");
    ASSERT_FALSE(chain.ok());
    EXPECT_NE(chain.status().message().find("has no column"), std::string::npos)
        << chain.status().message();
}

TEST_F(StepCompileTest, AnUnknownRelationFailsBeforeAnythingElse) {
    auto chain = CompileSql("SELECT * FROM nosuchtable");
    ASSERT_FALSE(chain.ok());
    EXPECT_EQ(chain.status().code(), StatusCode::kNotFound);
}

// ---- Numbering, purity, class --------------------------------------------

TEST_F(StepCompileTest, StepsAreNumberedGloballyInCompileOrder) {
    const StepChain chain = MustCompile(
        "SELECT a.id, b.id FROM acct AS a JOIN trade AS b ON a.id = b.acct_id "
        "JOIN acct AS c ON b.acct_id = c.id");
    ASSERT_EQ(chain.steps.size(), 3u);
    for (std::size_t i = 0; i < chain.steps.size(); ++i) {
        EXPECT_EQ(chain.steps[i].step_id, i) << "a trail entry's step_id must be unambiguous";
    }
}

TEST_F(StepCompileTest, WrittenOrderIsChainOrder) {
    // Reversing the FROM list must reverse the chain. Nothing decides a
    // better order - that is the contract, and it is what makes a
    // recorded trail replayable across executions.
    const StepChain ab =
        MustCompile("SELECT a.id, b.id FROM acct AS a JOIN trade AS b ON b.acct_id = a.id");
    const StepChain ba =
        MustCompile("SELECT a.id, b.id FROM trade AS b JOIN acct AS a ON b.acct_id = a.id");

    EXPECT_NE(ab.steps[0].rel_oid, ba.steps[0].rel_oid)
        << "the compiler reordered a chain; written order is a client contract";
    // And the kinds differ with it: only the second spelling can probe.
    EXPECT_EQ(ab.steps[1].kind, AccessKind::kScan);
    EXPECT_EQ(ba.steps[1].kind, AccessKind::kProbe);
}

TEST_F(StepCompileTest, CompilingTwiceGivesTheSameChain) {
    // Purity, stated as the done-condition states it: two compiles of one
    // statement are identical. A chain that varied with call order, or
    // with an address, or with anything but the AST and the catalog would
    // make a recorded trail meaningless.
    const char* sql =
        "SELECT a.name, b.sym FROM trade AS b JOIN acct AS a ON b.acct_id = a.id "
        "WHERE b.sym = 'AAPL' AND a.id = 3";
    const StepChain first = MustCompile(sql);
    const StepChain second = MustCompile(sql);

    ASSERT_EQ(first.steps.size(), second.steps.size());
    EXPECT_EQ(first.klass, second.klass);
    EXPECT_EQ(first.projection, second.projection);
    for (std::size_t i = 0; i < first.steps.size(); ++i) {
        EXPECT_EQ(first.steps[i].step_id, second.steps[i].step_id);
        EXPECT_EQ(first.steps[i].rel_oid, second.steps[i].rel_oid);
        EXPECT_EQ(first.steps[i].kind, second.steps[i].kind);
        ASSERT_EQ(first.steps[i].residual.size(), second.steps[i].residual.size());
        for (std::size_t p = 0; p < first.steps[i].residual.size(); ++p) {
            EXPECT_EQ(first.steps[i].residual[p].lhs, second.steps[i].residual[p].lhs);
            EXPECT_EQ(first.steps[i].residual[p].op, second.steps[i].residual[p].op);
        }
    }
}

TEST_F(StepCompileTest, ProjectionShapeDoesNotAffectTheClass) {
    // Two statements differing only in which columns they name read the
    // same rows by the same access path, so they are the same kind of
    // statement. The workplan states this as a constraint on V14 rather
    // than a property to discover.
    EXPECT_EQ(MustCompile("SELECT * FROM acct WHERE id = 1").klass,
              MustCompile("SELECT name FROM acct WHERE id = 1").klass);
    EXPECT_EQ(MustCompile("SELECT name FROM acct WHERE id = 1").klass,
              MustCompile("SELECT tier, name FROM acct WHERE id = 1").klass);
}

TEST_F(StepCompileTest, EveryMultiRelationStatementIsJoinSelect) {
    // J3: the class absorbs the shape, so the enum does not grow.
    EXPECT_EQ(MustCompile("SELECT a.id, b.id FROM acct AS a JOIN trade AS b ON b.acct_id = a.id")
                  .klass,
              StatementClass::kJoinSelect);
    EXPECT_EQ(MustCompile("SELECT * FROM acct WHERE id = 1").klass, StatementClass::kPointSelect);
}

TEST_F(StepCompileTest, StarProjectionIsEmptyButStillNamesItsColumns) {
    const StepChain chain = MustCompile("SELECT * FROM acct");
    EXPECT_TRUE(chain.star());
    EXPECT_TRUE(chain.projection.empty());
    ASSERT_EQ(chain.column_names.size(), 3u) << "a result set still has to label its columns";
    EXPECT_EQ(chain.column_names[0], "id");
}

TEST_F(StepCompileTest, ASubqueryPredicateLowersToASubChain) {
    // V14 refused these outright; V15 lowers them. The property that
    // survives from the refusal is that a subquery predicate is never
    // silently *dropped* - a predicate missing from the chain is a wrong
    // answer with nothing looking odd.
    const StepChain chain =
        MustCompile("SELECT * FROM acct WHERE id IN (SELECT acct_id FROM trade)");
    ASSERT_EQ(chain.steps.size(), 1u);
    // Attached to the step, not hoisted: `IN` tests an outer column, so
    // even though its inner set is row-independent the *comparison* is
    // per row. Only EXISTS and NOT EXISTS, which have no outer column,
    // can leave the row loop entirely.
    ASSERT_EQ(chain.steps[0].sub_chains.size(), 1u);
    EXPECT_EQ(chain.steps[0].sub_chains[0].kind, parser::PredicateKind::kInSubquery);
    EXPECT_FALSE(chain.steps[0].sub_chains[0].correlated) << "uncorrelated, but still per-row";
    EXPECT_TRUE(chain.hoisted.empty());
}

// ---- read_columns: what a step decodes after it has filtered (AP01) -----
//
// The mask is a **superset of every reader**, and the failure mode if it is
// not is the reason these tests are per-shape rather than one smoke test: a
// column outside the mask keeps the *previous* row's value in its slot, so a
// miss is a silently wrong answer, not a crash. Anything the compiler cannot
// prove it knows about answers kAllColumns.

// Bit for one column position, for readability below.
constexpr std::uint64_t Col(std::uint16_t pos) { return std::uint64_t{1} << pos; }

TEST_F(StepCompileTest, ACountStarOverAScanReadsNoColumn) {
    // The statement AP01 exists for: no residual, no projection, no fold
    // argument. The walk decodes nothing per row.
    const StepChain chain = MustCompile("SELECT COUNT(*) FROM trade");
    ASSERT_EQ(chain.steps.size(), 1u);
    EXPECT_EQ(chain.steps[0].read_columns, 0u)
        << "COUNT(*) reads no column and must decode none";
}

TEST_F(StepCompileTest, AFoldReadsItsArgumentsAndItsGroupKeys) {
    // trade(id, acct_id, sym) - group by acct_id (1), sum over id (0).
    const StepChain chain =
        MustCompile("SELECT acct_id, COUNT(*), MIN(sym) FROM trade GROUP BY acct_id");
    ASSERT_EQ(chain.steps.size(), 1u);
    EXPECT_EQ(chain.steps[0].read_columns, Col(1) | Col(2));
}

TEST_F(StepCompileTest, AProjectionReadsExactlyWhatItNames) {
    const StepChain chain = MustCompile("SELECT sym FROM trade");
    ASSERT_EQ(chain.steps.size(), 1u);
    EXPECT_EQ(chain.steps[0].read_columns, Col(2));
}

TEST_F(StepCompileTest, StarReadsEveryColumn) {
    const StepChain chain = MustCompile("SELECT * FROM trade");
    ASSERT_EQ(chain.steps.size(), 1u);
    EXPECT_EQ(chain.steps[0].read_columns, Step::kAllColumns)
        << "SELECT * emits every column by definition";
}

TEST_F(StepCompileTest, ALookupReadsThePkForTheTrail) {
    // A replayable step records the Keystone id of every row it accepts,
    // and reads it out of the frame. The kind is a compile-time fact, so
    // only the steps that can record one pay for the column.
    const StepChain chain = MustCompile("SELECT COUNT(*) FROM trade WHERE id = 7");
    ASSERT_EQ(chain.steps.size(), 1u);
    EXPECT_EQ(chain.steps[0].kind, AccessKind::kLookup);
    EXPECT_TRUE(chain.steps[0].read_columns & Col(0))
        << "the trail reads column 0 from the frame";
}

TEST_F(StepCompileTest, AJoinsEarlierStepIsReadByTheLaterStepsPredicate) {
    // **The trap this mask has to avoid.** The join predicate is attached
    // to step 1 and reads step 0's column out of the frame, so step 0 must
    // decode it even though step 0's own residual never mentions it.
    const StepChain chain = MustCompile(
        "SELECT COUNT(*) FROM acct AS a JOIN trade AS t ON a.id = t.acct_id");
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_TRUE(chain.steps[0].read_columns & Col(0))
        << "step 0's pk is read by step 1's probe key and predicate";
}

TEST_F(StepCompileTest, AProbeKeyIsCountedAsAReadOfTheStepItPointsAt) {
    const StepChain chain = MustCompile(
        "SELECT t.sym FROM trade AS t JOIN acct AS a ON t.acct_id = a.id");
    ASSERT_EQ(chain.steps.size(), 2u);
    // step 0 is trade; acct_id (1) feeds step 1's probe, sym (2) is projected.
    EXPECT_TRUE(chain.steps[0].read_columns & Col(1)) << "the probe key";
    EXPECT_TRUE(chain.steps[0].read_columns & Col(2)) << "the projection";
}

TEST_F(StepCompileTest, ASubChainAnywhereMakesEveryStepReadEverything) {
    // A correlated reference reaches outward into an earlier step's row and
    // is invisible from out here - it lives inside the sub-chain with
    // up > 0. Rather than map it back, the whole chain keeps every column.
    const StepChain chain = MustCompile(
        "SELECT COUNT(*) FROM acct AS a "
        "WHERE EXISTS (SELECT t.id FROM trade AS t WHERE t.acct_id = a.id)");
    for (const Step& step : chain.steps) {
        EXPECT_EQ(step.read_columns, Step::kAllColumns)
            << "a chain with a sub-chain must not narrow any step";
    }
}

TEST_F(StepCompileTest, AFilteredColumnStaysReadableAfterTheFilter) {
    // filter_columns and read_columns overlap rather than partition: the
    // VM decodes the filter's mask first and only `read & ~filter` after,
    // so a column in both is decoded exactly once and is present either way.
    const StepChain chain = MustCompile("SELECT sym FROM trade WHERE sym = 'AAPL'");
    ASSERT_EQ(chain.steps.size(), 1u);
    EXPECT_TRUE(chain.steps[0].filter_columns & Col(2));
    EXPECT_TRUE(chain.steps[0].read_columns & Col(2));
}

TEST_F(StepCompileTest, ACabinStepsKeyColumnIsAlwaysInItsFilterMask) {
    // The invariant the VM's recording path stands on (step_vm.cpp,
    // WalkAndRecord's guard): a kCabinProbe step's key column sits in
    // `filter_columns`, because the cabined equality is a residual conjunct
    // and the mask is derived from that same residual. This fails the day
    // someone reorders the mask pass past the kind assignment - which is
    // the only way the recording could read a stale slot.
    auto trade_oid = boot_->catalog.FindTableOidByName("trade", nullptr);
    ASSERT_TRUE(trade_oid.ok());
    ASSERT_TRUE(boot_->catalog.CreateCabin(trade_oid.value(), /*col_pos=*/1).ok());

    const StepChain chain = MustCompile("SELECT sym FROM trade WHERE acct_id = 7");
    ASSERT_EQ(chain.steps.size(), 1u);
    EXPECT_EQ(chain.steps[0].kind, AccessKind::kCabinProbe);
    EXPECT_TRUE(chain.steps[0].filter_columns & Col(1));
}

TEST_F(StepCompileTest, AJoinOnACabinedColumnCompilesToACorrelatedCabinProbe) {
    // cabin.md §4a: the cabined join column with no literal anywhere -
    // the shape that walked the inner relation once per outer row, and the
    // only accelerable shape a heap relation's join column has. Both ON
    // orientations must give the same probe, per the pk arm's argument.
    auto trade_oid = boot_->catalog.FindTableOidByName("trade", nullptr);
    ASSERT_TRUE(trade_oid.ok());
    ASSERT_TRUE(boot_->catalog.CreateCabin(trade_oid.value(), /*col_pos=*/1).ok());

    for (const char* on : {"trade.acct_id = acct.id", "acct.id = trade.acct_id"}) {
        const StepChain chain = MustCompile(
            std::string("SELECT trade.sym FROM acct JOIN trade ON ") + on +
            " WHERE acct.id BETWEEN 1 AND 4");
        ASSERT_EQ(chain.steps.size(), 2u);
        EXPECT_EQ(chain.steps[1].kind, AccessKind::kCabinProbe) << on;
        ASSERT_TRUE(chain.steps[1].cabin.has_value()) << on;
        ASSERT_TRUE(chain.steps[1].cabin->key_from.has_value()) << on;
        EXPECT_EQ(*chain.steps[1].cabin->key_from, (ColumnRef{0, 0, 0})) << on;
    }
}

TEST_F(StepCompileTest, ALiteralCabinEqualityBeatsTheCorrelatedForm) {
    // A compile-time key needs no per-row read, so the literal arm stays
    // first: `trade.acct_id = 7` wins over the join equality on the same
    // cabined column, and the plan carries a value, not a key source.
    auto trade_oid = boot_->catalog.FindTableOidByName("trade", nullptr);
    ASSERT_TRUE(trade_oid.ok());
    ASSERT_TRUE(boot_->catalog.CreateCabin(trade_oid.value(), /*col_pos=*/1).ok());

    const StepChain chain = MustCompile(
        "SELECT trade.sym FROM acct JOIN trade ON trade.acct_id = acct.id "
        "WHERE acct.id BETWEEN 1 AND 4 AND trade.acct_id = 7");
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[1].kind, AccessKind::kCabinProbe);
    ASSERT_TRUE(chain.steps[1].cabin.has_value());
    EXPECT_FALSE(chain.steps[1].cabin->key_from.has_value());
    EXPECT_EQ(chain.steps[1].cabin->value.int_val, 7);
}

TEST_F(StepCompileTest, TheCorrelatedCabinDeclinesAMismatchedDescriptor) {
    // int64 against int32: the write hook observed values coerced to the
    // cabin column's type, and only an identical descriptor makes the
    // outer row's decoded value the form the set was keyed on.
    Create("CREATE TABLE narrow2 (id int64, small int32)");
    auto trade_oid = boot_->catalog.FindTableOidByName("trade", nullptr);
    ASSERT_TRUE(trade_oid.ok());
    ASSERT_TRUE(boot_->catalog.CreateCabin(trade_oid.value(), /*col_pos=*/1).ok());

    const StepChain chain = MustCompile(
        "SELECT trade.sym FROM narrow2 JOIN trade ON trade.acct_id = narrow2.small "
        "WHERE narrow2.id BETWEEN 1 AND 4");
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[1].kind, AccessKind::kScan);
    EXPECT_FALSE(chain.steps[1].cabin.has_value());
}

TEST_F(StepCompileTest, ACorrelatedSubChainProbesTheCabinThroughTheFrame) {
    // The other owner of the per-outer-row walk: the sub-chain's join
    // equality reaches outward (up == 1), which is the "available before
    // this step runs" case - the enclosing row is bound before the
    // sub-chain opens.
    auto trade_oid = boot_->catalog.FindTableOidByName("trade", nullptr);
    ASSERT_TRUE(trade_oid.ok());
    ASSERT_TRUE(boot_->catalog.CreateCabin(trade_oid.value(), /*col_pos=*/1).ok());

    const StepChain chain = MustCompile(
        "SELECT * FROM acct WHERE EXISTS "
        "(SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)");
    ASSERT_EQ(chain.steps.size(), 1u);
    ASSERT_EQ(chain.steps[0].sub_chains.size(), 1u);
    const std::vector<Step>& inner = chain.steps[0].sub_chains[0].steps;
    ASSERT_EQ(inner.size(), 1u);
    EXPECT_EQ(inner[0].kind, AccessKind::kCabinProbe);
    ASSERT_TRUE(inner[0].cabin.has_value());
    ASSERT_TRUE(inner[0].cabin->key_from.has_value());
    EXPECT_EQ(inner[0].cabin->key_from->up, 1u);
}

// ---- The walked-join build annotation (workplan JB1) ----------------------
//
// The compile half of the statement-local inner build
// (docs/spec/join-inner-build.md). Two contracts, and every test here pins
// one of them: **exactly** the walked-join shape carries the annotation, and
// an annotated step is a kScan by every other measure - kinds, residuals,
// read_columns and class are what they were before the arm existed, by
// construction and not by audit.

TEST_F(StepCompileTest, TheWalkedJoinCarriesTheBuildAnnotation) {
    // The shape nothing serves: a join on a non-pk column with no index and
    // no Cabin. Before JB1 this compiled to a bare kScan walked once per
    // outer row; it still compiles to that kScan, now carrying the one
    // piece of state the executor needs to build once instead (JB3).
    for (const char* on : {"trade.acct_id = acct.id", "acct.id = trade.acct_id"}) {
        const StepChain chain =
            MustCompile(std::string("SELECT COUNT(*) FROM acct JOIN trade ON ") + on);
        ASSERT_EQ(chain.steps.size(), 2u);
        const Step& inner = chain.steps[1];
        EXPECT_EQ(inner.kind, AccessKind::kScan) << on;
        ASSERT_TRUE(inner.build.has_value()) << on;
        EXPECT_EQ(inner.build->col_pos, 1u) << on;
        EXPECT_EQ(inner.build->key_from, (ColumnRef{0, 0, 0})) << on;
        // The residual position names a conjunct that really is the join
        // equality, so JB3 can partition the residual around it.
        ASSERT_LT(inner.build->residual_pos, inner.residual.size()) << on;
        EXPECT_EQ(inner.residual[inner.build->residual_pos].op, parser::CompareOp::kEq) << on;
    }
}

TEST_F(StepCompileTest, TheAnnotatedStepIsAScanByEveryOtherMeasure) {
    // The chain-identity half of JB1's done-condition, on the main chain:
    // kind, every per-kind payload, the statistics shape, the masks and the
    // class are exactly the bare walk's - the annotation is the single
    // delta. The masks are hand-computed the way every AP01 test above is.
    const StepChain chain =
        MustCompile("SELECT trade.sym FROM acct JOIN trade ON trade.acct_id = acct.id");
    ASSERT_EQ(chain.steps.size(), 2u);
    const Step& inner = chain.steps[1];
    ASSERT_TRUE(inner.build.has_value());
    EXPECT_EQ(inner.kind, AccessKind::kScan);
    EXPECT_FALSE(inner.key.has_value());
    EXPECT_FALSE(inner.range.has_value());
    EXPECT_FALSE(inner.cabin.has_value());
    EXPECT_FALSE(inner.index.has_value());
    EXPECT_TRUE(inner.access_columns.empty()) << "kScan's statistics shape stays empty";
    EXPECT_FALSE(IsTrailReplayable(inner.kind));
    EXPECT_EQ(chain.klass, StatementClass::kJoinSelect);
    EXPECT_EQ(inner.filter_columns, Col(1)) << "the join conjunct's own column";
    EXPECT_EQ(inner.read_columns, Col(1) | Col(2)) << "the conjunct plus the projection";
}

TEST_F(StepCompileTest, OnlyTheWalkedJoinShapeIsAnnotated) {
    // Each row here reaches the build arm and must leave it empty-handed;
    // shapes decided before the arm (lookups, filter scans) are pinned by
    // the priority tests below, not repeated here.
    for (const char* sql : {
             "SELECT * FROM trade",  // bare scan, nothing correlated
             // The pk-probe join: the correlated equality binds the inner
             // relation's pk, which the probe arm serves outright.
             "SELECT a.name FROM trade AS t JOIN acct AS a ON t.acct_id = a.id",
             // An uncorrelated inner set: no equality reaches outward, so
             // there is no key to build a map on.
             "SELECT * FROM acct WHERE id IN (SELECT acct_id FROM trade)",
         }) {
        const StepChain chain = MustCompile(sql);
        for (const Step& step : chain.steps) {
            EXPECT_FALSE(step.build.has_value()) << sql;
            for (const SubChain& sub : step.sub_chains) {
                for (const Step& s : sub.steps) EXPECT_FALSE(s.build.has_value()) << sql;
            }
        }
        for (const SubChain& sub : chain.hoisted) {
            for (const Step& s : sub.steps) EXPECT_FALSE(s.build.has_value()) << sql;
        }
    }
}

TEST_F(StepCompileTest, AnExistsSubChainsWalkedInnerIsAnnotatedThroughTheFrame) {
    // The other owner of the per-outer-row walk (JB6's class): the
    // sub-chain's join equality reaches outward, up == 1, same as the
    // correlated cabin test above - but with no Cabin declared, the build
    // annotation is what the shape gets.
    const StepChain chain = MustCompile(
        "SELECT * FROM acct WHERE EXISTS "
        "(SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)");
    ASSERT_EQ(chain.steps.size(), 1u);
    ASSERT_EQ(chain.steps[0].sub_chains.size(), 1u);
    const std::vector<Step>& inner = chain.steps[0].sub_chains[0].steps;
    ASSERT_EQ(inner.size(), 1u);
    EXPECT_EQ(inner[0].kind, AccessKind::kScan);
    ASSERT_TRUE(inner[0].build.has_value());
    EXPECT_EQ(inner[0].build->col_pos, 1u);
    EXPECT_EQ(inner[0].build->key_from.up, 1u);
}

TEST_F(StepCompileTest, TheResidualPositionNamesTheConjunctAmongOthers) {
    // The one field JB3 partitions the residual around, pinned where it
    // can actually fail: the correlated equality sits *second* in written
    // order, behind a non-equality literal that keeps the step a kScan. A
    // BuildKeyOf that hardcoded position 0 passes every other test here.
    const StepChain chain = MustCompile(
        "SELECT * FROM acct WHERE EXISTS "
        "(SELECT trade.id FROM trade WHERE trade.acct_id > 0 AND trade.acct_id = acct.id)");
    ASSERT_EQ(chain.steps.size(), 1u);
    ASSERT_EQ(chain.steps[0].sub_chains.size(), 1u);
    const std::vector<Step>& inner = chain.steps[0].sub_chains[0].steps;
    ASSERT_EQ(inner.size(), 1u);
    EXPECT_EQ(inner[0].kind, AccessKind::kScan);
    ASSERT_TRUE(inner[0].build.has_value());
    ASSERT_EQ(inner[0].residual.size(), 2u);
    EXPECT_EQ(inner[0].build->residual_pos, 1u);
    EXPECT_EQ(inner[0].build->col_pos, 1u);
}

TEST_F(StepCompileTest, TheBuildDeclinesAMultiColumnJoinKey) {
    // Spec §8, CB12's scope rule: two correlated equalities on one step are
    // a key the statement wrote as two columns, and v1 declines rather than
    // keying a map on half of it.
    const StepChain chain = MustCompile(
        "SELECT COUNT(*) FROM acct JOIN trade ON trade.acct_id = acct.id "
        "WHERE trade.sym = acct.name");
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[1].kind, AccessKind::kScan);
    EXPECT_FALSE(chain.steps[1].build.has_value());
}

TEST_F(StepCompileTest, TheBuildDeclinesANonEqualityCorrelation) {
    // Spec §8: only an equality buckets. ON is equality-only by grammar, so
    // the sub-chain form is where a non-equality correlation can be written.
    const StepChain chain = MustCompile(
        "SELECT * FROM acct WHERE EXISTS "
        "(SELECT trade.id FROM trade WHERE trade.acct_id > acct.id)");
    ASSERT_EQ(chain.steps.size(), 1u);
    ASSERT_EQ(chain.steps[0].sub_chains.size(), 1u);
    const std::vector<Step>& inner = chain.steps[0].sub_chains[0].steps;
    ASSERT_EQ(inner.size(), 1u);
    EXPECT_EQ(inner[0].kind, AccessKind::kScan);
    EXPECT_FALSE(inner[0].build.has_value());
}

TEST_F(StepCompileTest, TheBuildDeclinesAMismatchedDescriptor) {
    // int64 against int32, the correlated cabin's decline read the same
    // way: the frame value must be byte-valid under the map column's
    // descriptor, and only an identical (type_val, len) makes it so.
    Create("CREATE TABLE narrow3 (id int64, small int32)");
    const StepChain chain = MustCompile(
        "SELECT COUNT(*) FROM narrow3 JOIN trade ON trade.acct_id = narrow3.small");
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[1].kind, AccessKind::kScan);
    EXPECT_FALSE(chain.steps[1].build.has_value());
}

TEST_F(StepCompileTest, AFilterScanStillWinsOverTheBuild) {
    // Spec §8, a decision rather than an impossibility: the step's own
    // literal already bounds what a walk visits, and v1 declines to also
    // build for it. The kind arm order is the test.
    const StepChain chain = MustCompile(
        "SELECT COUNT(*) FROM acct JOIN trade ON trade.acct_id = acct.id "
        "WHERE trade.sym = 'AAPL'");
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[1].kind, AccessKind::kFilterScan);
    EXPECT_FALSE(chain.steps[1].build.has_value());
}

TEST_F(StepCompileTest, ABankedStructureStillWinsOverTheBuild) {
    // Ladder order is the economics (spec §5): a converged Cabin serve
    // beats any per-statement rebuild, so the banked arm stays ahead.
    auto trade_oid = boot_->catalog.FindTableOidByName("trade", nullptr);
    ASSERT_TRUE(trade_oid.ok());
    ASSERT_TRUE(boot_->catalog.CreateCabin(trade_oid.value(), /*col_pos=*/1).ok());

    const StepChain chain =
        MustCompile("SELECT trade.sym FROM acct JOIN trade ON trade.acct_id = acct.id");
    ASSERT_EQ(chain.steps.size(), 2u);
    EXPECT_EQ(chain.steps[1].kind, AccessKind::kCabinProbe);
    EXPECT_FALSE(chain.steps[1].build.has_value());
}

TEST_F(StepCompileTest, ADmlSubChainDeclinesAndIsOtherwiseByteIdentical) {
    // Two refusal rows and the identity contract in one shape. v1 is
    // SELECT-only (spec §4: a DML statement's own writes between outer rows
    // are what would invalidate a map), so the same subquery text compiles
    // annotated under SELECT and bare under CompileWhere - which is also
    // the with/without-the-arm comparison JB1's done-condition asks for:
    // every field the identity contract names is byte-identical, and the
    // annotation is the single delta.
    const char* subquery = "EXISTS (SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)";

    const StepChain with = MustCompile(std::string("SELECT * FROM acct WHERE ") + subquery);
    ASSERT_EQ(with.steps.size(), 1u);
    ASSERT_EQ(with.steps[0].sub_chains.size(), 1u);
    ASSERT_EQ(with.steps[0].sub_chains[0].steps.size(), 1u);
    const Step& annotated = with.steps[0].sub_chains[0].steps[0];
    ASSERT_TRUE(annotated.build.has_value());

    auto parsed =
        parser::Parse(std::string("UPDATE acct SET name = 'x' WHERE ") + subquery);
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    const auto& update = std::get<parser::UpdateStmt>(parsed.value());
    auto oid = boot_->catalog.FindTableOidByName("acct", nullptr);
    ASSERT_TRUE(oid.ok());
    auto access = boot_->catalog.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    auto where_step = CompileWhere(boot_->catalog, *access.value(), "acct", update.where);
    ASSERT_TRUE(where_step.ok()) << where_step.status().message();
    ASSERT_EQ(where_step.value().sub_chains.size(), 1u);
    ASSERT_EQ(where_step.value().sub_chains[0].steps.size(), 1u);
    const Step& bare = where_step.value().sub_chains[0].steps[0];
    EXPECT_FALSE(bare.build.has_value());

    EXPECT_EQ(annotated.step_id, bare.step_id);
    EXPECT_EQ(annotated.rel_oid, bare.rel_oid);
    EXPECT_EQ(annotated.rel_name, bare.rel_name);
    EXPECT_EQ(annotated.kind, bare.kind);
    EXPECT_EQ(annotated.filter_columns, bare.filter_columns);
    EXPECT_EQ(annotated.read_columns, bare.read_columns);
    EXPECT_EQ(annotated.access_columns, bare.access_columns);
    ASSERT_EQ(annotated.residual.size(), bare.residual.size());
    for (std::size_t p = 0; p < annotated.residual.size(); ++p) {
        EXPECT_EQ(annotated.residual[p].lhs, bare.residual[p].lhs);
        EXPECT_EQ(annotated.residual[p].op, bare.residual[p].op);
        EXPECT_EQ(annotated.residual[p].rhs.kind, bare.residual[p].rhs.kind);
    }
}

TEST_F(StepCompileTest, AScalarSubChainIsNeverAnnotated) {
    // Spec §6: a hit in a prefix map is conclusive only under `Exists`
    // semantics; a scalar sub-chain's cardinality check has none, so its
    // steps never take the annotation - and neither does anything nested
    // under one.
    const StepChain chain = MustCompile(
        "SELECT * FROM acct WHERE name = "
        "(SELECT trade.sym FROM trade WHERE trade.acct_id = acct.id)");
    ASSERT_EQ(chain.steps.size(), 1u);
    ASSERT_EQ(chain.steps[0].sub_chains.size(), 1u);
    EXPECT_EQ(chain.steps[0].sub_chains[0].kind, parser::PredicateKind::kCompareSubquery);
    const std::vector<Step>& inner = chain.steps[0].sub_chains[0].steps;
    ASSERT_EQ(inner.size(), 1u);
    EXPECT_EQ(inner[0].kind, AccessKind::kScan);
    EXPECT_FALSE(inner[0].build.has_value());
}

TEST_F(StepCompileTest, ARelationWiderThanSixtyFourColumnsGetsNoMask) {
    // **A latent correctness bug, found while building AP01 and fixed with
    // it.** A uint64 mask cannot name column 64, and DecodeColumnsInto stops
    // at that bound - its comment says "the caller decodes fully", which was
    // true of every caller *except* the partial decode, where the tail would
    // silently keep the previous row's values. So a wide relation gets
    // kAllColumns and takes the whole-row path.
    std::string sql = "CREATE TABLE huge (id int64";
    for (int i = 1; i < 70; ++i) sql += ", c" + std::to_string(i) + " int64";
    sql += ")";
    Create(sql);

    const StepChain chain = MustCompile("SELECT c1 FROM huge WHERE c2 = 1");
    ASSERT_EQ(chain.steps.size(), 1u);
    EXPECT_EQ(chain.steps[0].filter_columns, Step::kAllColumns);
    EXPECT_EQ(chain.steps[0].read_columns, Step::kAllColumns)
        << "a mask cannot describe column 64 and beyond, so there must be no mask";
}

TEST_F(StepCompileTest, ADefaultConstructedStepDecodesEverything) {
    // The zero-initialised default is the *opposite* of the mask, on
    // purpose: a Step built by anything other than the compiler is slow
    // rather than wrong.
    const Step fresh;
    EXPECT_EQ(fresh.read_columns, Step::kAllColumns);
    EXPECT_EQ(fresh.filter_columns, Step::kAllColumns);
}

// ---- The pagination tail (spec I11, V09; the sort is OB3) -----------------
//
// `limit`/`offset` ride the chain for the dispatcher's emission quota, and
// since OB3 the resolved `ORDER BY` keys ride it too, for the dispatcher's
// sort. The one form that still does not is the one V09 was written for:
// a single ascending key on the driving relation's pk names the order the
// chain already emits, so the compiler *elides* it and the statement pays
// nothing.

TEST_F(StepCompileTest, LimitAndOffsetSurviveCompilation) {
    const StepChain chain = MustCompile("SELECT name FROM acct LIMIT 3 OFFSET 1");
    ASSERT_TRUE(chain.limit.has_value());
    EXPECT_EQ(chain.limit.value(), 3u);
    EXPECT_EQ(chain.offset, 1u);
}

TEST_F(StepCompileTest, AChainWithoutATailCarriesNone) {
    const StepChain chain = MustCompile("SELECT name FROM acct");
    EXPECT_FALSE(chain.limit.has_value());
    EXPECT_EQ(chain.offset, 0u);
}

// AG1's argument, applied to the quota: the tail must change nothing the
// executor reads. Steps, kinds, keys and residuals are the unlimited
// twin's, so every property already proved of the chain - trail trust,
// probe equivalence, access statistics - holds for a limited statement
// with no new proof.
TEST_F(StepCompileTest, TheTailChangesNoStep) {
    const StepChain plain = MustCompile("SELECT name FROM acct WHERE id = 7");
    const StepChain limited =
        MustCompile("SELECT name FROM acct WHERE id = 7 ORDER BY id LIMIT 1");
    ASSERT_EQ(limited.steps.size(), plain.steps.size());
    EXPECT_EQ(limited.steps[0].kind, plain.steps[0].kind);
    EXPECT_EQ(limited.steps[0].residual.size(), plain.steps[0].residual.size());
    EXPECT_EQ(limited.steps[0].key.has_value(), plain.steps[0].key.has_value());
    EXPECT_EQ(limited.klass, plain.klass);
}

// The elision, and the whole reason it is worth having: the statement that
// V09 accepted compiles to no sort at all, so its cost is unchanged rather
// than merely small.
TEST_F(StepCompileTest, OrderByThePkIsElidedNotSorted) {
    const StepChain chain = MustCompile("SELECT name FROM acct ORDER BY id");
    ASSERT_EQ(chain.steps.size(), 1u);
    EXPECT_EQ(chain.steps[0].kind, AccessKind::kScan);
    EXPECT_FALSE(chain.sorted());
}

TEST_F(StepCompileTest, OrderByTheQualifiedPkOfTheDrivingRelationIsElided) {
    const StepChain chain = MustCompile(
        "SELECT a.name, t.sym FROM acct AS a JOIN trade AS t ON t.acct_id = a.id "
        "ORDER BY a.id LIMIT 5");
    ASSERT_EQ(chain.steps.size(), 2u);
    ASSERT_TRUE(chain.limit.has_value());
    EXPECT_EQ(chain.limit.value(), 5u);
    EXPECT_FALSE(chain.sorted());
}

// ...and the elision is exactly that narrow. Descending on the same pk is
// not an order any chain emits, because every chain links forward only.
TEST_F(StepCompileTest, OrderByThePkDescendingIsSorted) {
    const StepChain chain = MustCompile("SELECT name FROM acct ORDER BY id DESC");
    ASSERT_EQ(chain.sort_keys.size(), 1u);
    EXPECT_TRUE(chain.sort_keys[0].descending);
    EXPECT_EQ(chain.sort_keys[0].ref.col_pos, 0u);
}

TEST_F(StepCompileTest, OrderByANonPkColumnCompilesToASortKey) {
    const StepChain chain = MustCompile("SELECT name FROM acct ORDER BY name");
    ASSERT_EQ(chain.sort_keys.size(), 1u);
    EXPECT_EQ(chain.sort_keys[0].ref.rel_slot, 0u);
    EXPECT_NE(chain.sort_keys[0].ref.col_pos, 0u);
    EXPECT_FALSE(chain.sort_keys[0].descending);
}

// A joined relation's column is orderable: by the time the sink sees a row
// the frame holds every step's values, so which step a key came from costs
// the sort nothing.
TEST_F(StepCompileTest, OrderByAJoinedRelationsColumnResolvesToItsSlot) {
    const StepChain chain = MustCompile(
        "SELECT a.name, t.sym FROM acct AS a JOIN trade AS t ON t.acct_id = a.id "
        "ORDER BY t.sym DESC, a.name");
    ASSERT_EQ(chain.sort_keys.size(), 2u);
    EXPECT_EQ(chain.sort_keys[0].ref.rel_slot, 1u);
    EXPECT_TRUE(chain.sort_keys[0].descending);
    EXPECT_EQ(chain.sort_keys[1].ref.rel_slot, 0u);
    EXPECT_FALSE(chain.sort_keys[1].descending);
}

// A key must be decoded to be compared, and it is decoded by the step that
// owns it - the one respect in which a sorted chain differs from its
// unsorted twin (OB3).
TEST_F(StepCompileTest, ASortKeyIsAddedToItsOwnStepsReadColumns) {
    const StepChain plain = MustCompile("SELECT name FROM acct");
    const StepChain sorted = MustCompile("SELECT name FROM acct ORDER BY tier");
    ASSERT_EQ(sorted.steps.size(), plain.steps.size());
    EXPECT_EQ(sorted.steps[0].kind, plain.steps[0].kind);
    EXPECT_EQ(sorted.steps[0].residual.size(), plain.steps[0].residual.size());
    EXPECT_EQ(sorted.klass, plain.klass);
    // The ordered-by column is not projected, so it is read only because
    // the sort named it.
    EXPECT_NE(sorted.steps[0].read_columns, plain.steps[0].read_columns);
}

TEST_F(StepCompileTest, OrderByAnUnknownColumnIsAResolutionError) {
    const auto chain = CompileSql("SELECT name FROM acct ORDER BY nope");
    ASSERT_FALSE(chain.ok());
    EXPECT_EQ(chain.status().code(), StatusCode::kInvalidArgument);
}

// ---- The SET list: K-M3's compiler half ---------------------------------
//
// `CompileAssignments` is the other half of an UPDATE's compile, beside
// CompileWhere. The tests below are about the *codes*, not the messages:
// the split between kUnsupported and kInvalidArgument is the policy
// rules.md states, and it is what a client library reads.

class CompileAssignmentsTest : public StepCompileTest {
protected:
    Status Check(const std::string& sql) {
        auto parsed = parser::Parse(sql);
        EXPECT_TRUE(parsed.ok()) << sql << ": " << parsed.status().message();
        if (!parsed.ok()) return parsed.status();
        const auto& up = std::get<parser::UpdateStmt>(parsed.value());

        auto oid = boot_->catalog.FindTableOidByName(up.table_name);
        EXPECT_TRUE(oid.ok()) << oid.status().message();
        if (!oid.ok()) return oid.status();

        auto access = boot_->catalog.InitTableAccess(oid.value());
        EXPECT_TRUE(access.ok()) << access.status().message();
        if (!access.ok()) return access.status();
        return CompileAssignments(*access.value(), up.assignments);
    }
};

TEST_F(CompileAssignmentsTest, AssigningThePrimaryKeyIsUnsupportedNotInvalid) {
    // K2: the id names the tuple in the clustered tree, in every index and
    // Cabin entry, and in every recorded trail. The column exists and the
    // value would encode - the statement is understood and declined, which
    // is exactly what kUnsupported means and kInvalidArgument does not.
    const Status s = Check("UPDATE acct SET id = 99");
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnsupported) << s.message();
    EXPECT_NE(s.message().find("at byte "), std::string::npos) << s.message();
}

TEST_F(CompileAssignmentsTest, ThePkIsRefusedWhereverItSitsInTheSetList) {
    // The loop must not stop at the first legal target.
    const Status s = Check("UPDATE acct SET name = 'x', tier = 'y', id = 99");
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnsupported) << s.message();
}

TEST_F(CompileAssignmentsTest, TheRefusalNamesTheColumnsOwnByte) {
    // "UPDATE acct SET name = 'x', id = 99"
    //  0123456789...
    const std::string sql = "UPDATE acct SET name = 'x', id = 99";
    const Status s = Check(sql);
    ASSERT_FALSE(s.ok());
    EXPECT_NE(s.message().find("at byte " + std::to_string(sql.find("id = 99"))),
              std::string::npos)
        << s.message();
}

TEST_F(CompileAssignmentsTest, AnUnknownColumnIsInvalidArgumentAndCarriesItsByte) {
    const std::string sql = "UPDATE acct SET nope = 1";
    const Status s = Check(sql);
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument) << s.message();
    EXPECT_NE(s.message().find("at byte " + std::to_string(sql.find("nope"))), std::string::npos)
        << s.message();
}

TEST_F(CompileAssignmentsTest, AnOrdinarySetListCompiles) {
    // The control. Without it every assertion above would pass against a
    // function that refused everything.
    EXPECT_TRUE(Check("UPDATE acct SET name = 'x', tier = 'y'").ok());
}

TEST_F(CompileAssignmentsTest, APkNamedOnAnotherRelationIsNotThisRelationsPk) {
    // `acct_id` is trade's second column and an ordinary field. Nothing
    // about the *name* is what makes a column refusable - position 0 is.
    EXPECT_TRUE(Check("UPDATE trade SET acct_id = 3").ok());
}

}  // namespace
}  // namespace kds::exec
