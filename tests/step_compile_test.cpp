#include "kds/exec/step_compiler.hpp"

#include <optional>
#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// V14 - the step compiler (docs/parser-v2-workplan.md).
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

}  // namespace
}  // namespace kds::exec
