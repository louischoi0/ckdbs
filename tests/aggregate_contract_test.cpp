#include "kds/exec/step_compiler.hpp"

#include <optional>
#include <sstream>
#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// The aggregation contract suite (docs/feat-aggregate.md §9,
// docs/workplan-aggregate.md AG09). Regression-mandatory: every item here
// is a property the feature is defined by, not an example of it working.
//
// Item 1 - **chain identity** - is the one that carries the design. AG1
// places the fold outside the executor, and the whole payoff is that a
// statement's chain does not know it is being aggregated: the steps, kinds,
// residuals, access columns and class compiled for
// `SELECT b, COUNT(*) FROM t GROUP BY b` are those compiled for
// `SELECT b FROM t`. Every property already proved of a chain - trail
// replay, Cabin probes, the scan/probe equivalence, "downgrading any step
// to a scan cannot change the result" - therefore holds for an aggregated
// statement without a new proof. This test is what stops that from
// becoming an aspiration: the day someone teaches the compiler to pick a
// different access kind because a fold is coming, it fails here.
//
// Written in the style of waystone_contract_test.cpp: configurations
// rendered to text and compared byte for byte, so a failure prints the
// difference rather than an index.

namespace kds::exec {
namespace {

class AggregateContractTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));

        // `id` is the pk of each by invariant 11 - the first column always
        // is. `bar` carries a uint64 so §3.3's two uint64 rules have a
        // column to be about.
        Create("CREATE TABLE acct (id int64, name varchar, tier int32)");
        Create("CREATE TABLE trade (id int64, acct_id int64, qty int64, sym varchar)");
        Create("CREATE TABLE bar (id int64, big uint64, qty int64)");
    }

    // Builds the schema the way HandleCreateTableSql does, so these tables
    // are the shape a real CREATE TABLE produces.
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

// ---- Rendering, so a failure is readable --------------------------------
//
// Everything a chain carries **except** the fold and the output labels.
// Those two are exactly what an aggregated statement is allowed to differ
// in; anything else differing is the bug this file exists to catch.

void RenderRef(std::ostringstream& os, const ColumnRef& ref) {
    os << ref.up << ':' << ref.rel_slot << ':' << ref.col_pos;
}

void RenderOperand(std::ostringstream& os, const Operand& op) {
    if (op.kind == OperandKind::kLiteral) {
        os << "lit(" << static_cast<int>(op.literal.type) << ',' << op.literal.int_val << ','
           << op.literal.str_val << ')';
    } else {
        os << "col(";
        RenderRef(os, op.column);
        os << ')';
    }
}

void RenderSteps(std::ostringstream& os, const std::vector<Step>& steps, int indent);

void RenderStep(std::ostringstream& os, const Step& step, int indent) {
    const std::string pad(indent * 2, ' ');
    os << pad << "step " << step.step_id << " rel=" << step.rel_oid << '/' << step.rel_name
       << " kind=" << static_cast<int>(step.kind)
       << " filter_columns=" << step.filter_columns << '\n';
    if (step.key.has_value()) {
        os << pad << "  key=";
        RenderOperand(os, *step.key);
        os << '\n';
    }
    if (step.range.has_value()) {
        os << pad << "  range=[" << step.range->low << ',' << step.range->high << "]\n";
    }
    if (step.cabin.has_value()) {
        os << pad << "  cabin=" << step.cabin->cabin_id << " col=" << step.cabin->col_pos << '\n';
    }
    os << pad << "  access_columns=";
    for (std::uint16_t col : step.access_columns) os << col << ',';
    os << '\n';
    for (const StepPredicate& pred : step.residual) {
        os << pad << "  residual ";
        RenderRef(os, pred.lhs);
        os << ' ' << parser::CompareOpName(pred.op) << ' ';
        RenderOperand(os, pred.rhs);
        os << '\n';
    }
    for (const SubChain& sub : step.sub_chains) {
        os << pad << "  sub kind=" << static_cast<int>(sub.kind)
           << " correlated=" << sub.correlated << '\n';
        RenderSteps(os, sub.steps, indent + 2);
    }
}

void RenderSteps(std::ostringstream& os, const std::vector<Step>& steps, int indent) {
    for (const Step& step : steps) RenderStep(os, step, indent);
}

std::string RenderChainShape(const StepChain& chain) {
    std::ostringstream os;
    os << "class=" << static_cast<int>(chain.klass) << '\n';
    for (const SubChain& sub : chain.hoisted) {
        os << "hoisted kind=" << static_cast<int>(sub.kind) << '\n';
        RenderSteps(os, sub.steps, 1);
    }
    RenderSteps(os, chain.steps, 0);
    return os.str();
}

// ---- §9.1 Chain identity -------------------------------------------------

TEST(AggregateContractShapeTest, RenderingSeesTheFieldsItClaimsTo) {
    // A guard on the guard: if RenderChainShape ever stops distinguishing
    // two genuinely different chains, item 1 passes vacuously. Two
    // statements that differ in access kind must render differently.
    StepChain a;
    StepChain b;
    a.steps.push_back(Step{});
    b.steps.push_back(Step{});
    b.steps[0].kind = AccessKind::kLookup;
    EXPECT_NE(RenderChainShape(a), RenderChainShape(b));
}

TEST_F(AggregateContractTest, TheChainIsIdenticalWithAndWithoutTheFold) {
    struct Case {
        const char* plain;
        const char* folded;
    };
    // One row per access kind the compiler can assign, plus a join - which
    // is spec §9.1's "a corpus spanning lookup, probe, range, filter-scan
    // and join shapes". A cabin probe needs a Cabin in the catalog and is
    // covered where Cabins are built; the kind is assigned from catalog
    // state that the fold cannot reach, which is the same argument.
    const Case cases[] = {
        // kScan
        {"SELECT qty FROM trade",
         "SELECT qty, COUNT(*) FROM trade GROUP BY qty"},
        // kLookup - pk equality against a literal
        {"SELECT qty FROM trade WHERE id = 7",
         "SELECT COUNT(*) FROM trade WHERE id = 7"},
        // kRange - pk BETWEEN
        {"SELECT qty FROM trade WHERE id BETWEEN 3 AND 9",
         "SELECT SUM(qty) FROM trade WHERE id BETWEEN 3 AND 9"},
        // kFilterScan - equality on an unindexed non-pk column
        {"SELECT qty FROM trade WHERE sym = 'AAPL'",
         "SELECT MIN(qty), MAX(qty) FROM trade WHERE sym = 'AAPL'"},
        // A join, whose second step is a kProbe
        {"SELECT a.tier FROM acct AS a JOIN trade AS t ON a.id = t.acct_id",
         "SELECT a.tier, COUNT(*) FROM acct AS a JOIN trade AS t ON a.id = t.acct_id "
         "GROUP BY a.tier"},
        // A join with a predicate, so the residual placement is compared too
        {"SELECT a.tier FROM acct AS a JOIN trade AS t ON a.id = t.acct_id WHERE t.qty > 5",
         "SELECT SUM(t.qty) FROM acct AS a JOIN trade AS t ON a.id = t.acct_id WHERE t.qty > 5"},
        // A predicate-position subquery under a fold
        {"SELECT qty FROM trade WHERE acct_id IN (SELECT id FROM acct WHERE tier = 1)",
         "SELECT COUNT(*) FROM trade WHERE acct_id IN (SELECT id FROM acct WHERE tier = 1)"},
        // The global form - no GROUP BY at all
        {"SELECT qty FROM trade", "SELECT COUNT(*), SUM(qty) FROM trade"},
    };

    for (const Case& c : cases) {
        const StepChain plain = MustCompile(c.plain);
        const StepChain folded = MustCompile(c.folded);
        EXPECT_EQ(RenderChainShape(plain), RenderChainShape(folded))
            << "the fold changed the chain\n  plain:  " << c.plain
            << "\n  folded: " << c.folded;
        EXPECT_FALSE(plain.aggregated()) << c.plain;
        EXPECT_TRUE(folded.aggregated()) << c.folded;
        // AG14: aggregation is consumption shape and never moves the class.
        EXPECT_EQ(plain.klass, folded.klass) << c.folded;
    }
}

TEST_F(AggregateContractTest, AnAggregatedChainIsNotAStar) {
    const StepChain folded = MustCompile("SELECT COUNT(*) FROM trade");
    EXPECT_TRUE(folded.projection.empty());
    EXPECT_FALSE(folded.star())
        << "an aggregated chain with an empty projection must not read as SELECT *";

    const StepChain star = MustCompile("SELECT * FROM trade");
    EXPECT_TRUE(star.star());
    EXPECT_FALSE(star.aggregated());
}

TEST_F(AggregateContractTest, CompilingTheSameStatementTwiceGivesTheSameSpec) {
    // The compile is pure: same statement plus same catalog, same spec.
    // That is what keeps the chain f(shape, catalog) and lets pattern_id
    // go on naming it.
    const char* sql = "SELECT a.tier, COUNT(DISTINCT t.sym), SUM(t.qty) FROM acct AS a "
                      "JOIN trade AS t ON a.id = t.acct_id GROUP BY a.tier";
    const StepChain first = MustCompile(sql);
    const StepChain second = MustCompile(sql);

    ASSERT_TRUE(first.aggregated());
    ASSERT_TRUE(second.aggregated());
    EXPECT_EQ(RenderChainShape(first), RenderChainShape(second));
    EXPECT_EQ(first.column_names, second.column_names);

    const AggregateSpec& a = *first.aggregate;
    const AggregateSpec& b = *second.aggregate;
    ASSERT_EQ(a.items.size(), b.items.size());
    ASSERT_EQ(a.group_keys, b.group_keys);
    for (std::size_t i = 0; i < a.items.size(); ++i) {
        EXPECT_EQ(a.items[i].is_aggregate, b.items[i].is_aggregate);
        EXPECT_EQ(a.items[i].func, b.items[i].func);
        EXPECT_EQ(a.items[i].star_arg, b.items[i].star_arg);
        EXPECT_EQ(a.items[i].distinct, b.items[i].distinct);
        EXPECT_EQ(a.items[i].ref, b.items[i].ref);
        EXPECT_EQ(a.items[i].type_val, b.items[i].type_val);
    }
}

// ---- The compiled spec ---------------------------------------------------

TEST_F(AggregateContractTest, ItemsAndKeysResolveToReferencesInWrittenOrder) {
    const StepChain chain =
        MustCompile("SELECT a.tier, COUNT(*), SUM(t.qty) FROM acct AS a "
                    "JOIN trade AS t ON a.id = t.acct_id GROUP BY a.tier");
    ASSERT_TRUE(chain.aggregated());
    const AggregateSpec& spec = *chain.aggregate;

    ASSERT_EQ(spec.group_keys.size(), 1u);
    EXPECT_EQ(spec.group_keys[0], (ColumnRef{0, 0, 2}));  // acct.tier

    ASSERT_EQ(spec.items.size(), 3u);
    EXPECT_FALSE(spec.items[0].is_aggregate);
    EXPECT_EQ(spec.items[0].ref, (ColumnRef{0, 0, 2}));
    EXPECT_TRUE(spec.items[1].is_aggregate);
    EXPECT_TRUE(spec.items[1].star_arg);
    EXPECT_EQ(spec.items[2].func, parser::AggFunc::kSum);
    EXPECT_EQ(spec.items[2].ref, (ColumnRef{0, 1, 2}));  // trade.qty
    EXPECT_EQ(spec.items[2].type_val, catalog::kTypeValInt64);
}

TEST_F(AggregateContractTest, ColumnNamesLabelTheFoldsOutput) {
    const StepChain chain = MustCompile(
        "SELECT tier, COUNT(*), COUNT(DISTINCT tier), MIN(tier) FROM acct GROUP BY tier");
    const std::vector<std::string> want = {"tier", "count(*)", "count(distinct tier)",
                                           "min(tier)"};
    EXPECT_EQ(chain.column_names, want);
}

TEST_F(AggregateContractTest, TheGlobalFormHasNoGroupKeys) {
    const StepChain chain = MustCompile("SELECT COUNT(*) FROM trade");
    ASSERT_TRUE(chain.aggregated());
    EXPECT_TRUE(chain.aggregate->group_keys.empty());
    EXPECT_EQ(chain.aggregate->items.size(), 1u);
}

// ---- §9.6 Strict grouping and the compile-time refusals -----------------

TEST_F(AggregateContractTest, ABareColumnMustAppearInGroupBy) {
    // AG5. There is no "any row" mode: an answer that depends on scan order
    // is an answer this engine refuses to give.
    auto chain = CompileSql("SELECT name, COUNT(*) FROM acct GROUP BY tier");
    ASSERT_FALSE(chain.ok());
    EXPECT_EQ(chain.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(chain.status().message().find("byte 7"), std::string::npos)
        << chain.status().message();
}

TEST_F(AggregateContractTest, ABareColumnWithNoGroupByAtAllIsRefused) {
    auto chain = CompileSql("SELECT name, COUNT(*) FROM acct");
    ASSERT_FALSE(chain.ok());
    EXPECT_EQ(chain.status().code(), StatusCode::kInvalidArgument);
}

TEST_F(AggregateContractTest, AGroupedColumnResolvesThroughItsQualifier) {
    // `SELECT a.tier ... GROUP BY tier` names one column twice, and the
    // check compares resolved references rather than spellings.
    auto chain = CompileSql("SELECT a.tier, COUNT(*) FROM acct AS a GROUP BY tier");
    EXPECT_TRUE(chain.ok()) << chain.status().message();
}

TEST_F(AggregateContractTest, ADuplicateGroupKeyIsRefused) {
    auto chain = CompileSql("SELECT tier, COUNT(*) FROM acct GROUP BY tier, tier");
    ASSERT_FALSE(chain.ok());
    EXPECT_EQ(chain.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(chain.status().message().find("byte "), std::string::npos);
}

TEST_F(AggregateContractTest, AnUnknownColumnInAnAggregateIsReportedAsSuch) {
    auto chain = CompileSql("SELECT SUM(nope) FROM acct");
    ASSERT_FALSE(chain.ok());
    EXPECT_EQ(chain.status().code(), StatusCode::kInvalidArgument);
}

// ---- §3.3 SUM's documented product constraints --------------------------

TEST_F(AggregateContractTest, SumOverUint64IsDeclined) {
    // Understood and declined, not a type error: half a uint64's range does
    // not fit the int64 accumulator, and a sum of Keystone ids is a
    // statement nobody meant.
    auto chain = CompileSql("SELECT SUM(big) FROM bar");
    ASSERT_FALSE(chain.ok());
    EXPECT_EQ(chain.status().code(), StatusCode::kUnsupported);
    EXPECT_NE(chain.status().message().find("byte 7"), std::string::npos)
        << chain.status().message();
}

TEST_F(AggregateContractTest, SumOverTextIsATypeError) {
    auto chain = CompileSql("SELECT SUM(sym) FROM trade");
    ASSERT_FALSE(chain.ok());
    EXPECT_EQ(chain.status().code(), StatusCode::kInvalidArgument);
}

TEST_F(AggregateContractTest, MinAndMaxOverUint64AreAllowed) {
    // They are exact: comparison goes through the digit-text path rather
    // than through a signed reading that cannot hold the value.
    EXPECT_TRUE(CompileSql("SELECT MIN(big) FROM bar").ok());
    EXPECT_TRUE(CompileSql("SELECT MAX(big) FROM bar").ok());
    EXPECT_TRUE(CompileSql("SELECT COUNT(big) FROM bar").ok());
}

TEST_F(AggregateContractTest, SumOverEachSignedIntegerWidthIsAllowed) {
    EXPECT_TRUE(CompileSql("SELECT SUM(qty) FROM trade").ok());
    EXPECT_TRUE(CompileSql("SELECT SUM(tier) FROM acct").ok());
}

}  // namespace
}  // namespace kds::exec
