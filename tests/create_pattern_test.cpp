#include "kds/exec/pattern_ddl.hpp"

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/pattern_defs.hpp"
#include "kds/stats/waystone_dir.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// `CREATE PATTERN` / `DROP PATTERN`
// (docs/spec/create-pattern-user-defined-patterns-v1.md).
//
// The property the whole feature rests on - that a declared body and the
// live traffic it means to match hash to one pattern_id - is pinned in
// fingerprint_test.cpp, where it belongs: it is a property of the
// fingerprint, not of the catalog. What is pinned here is section 6's
// validation chain, one test per check, plus the two things a declaration
// does that an observation cannot: adopt an auto-registered row without
// discarding its trails, and pre-size a directory.

namespace kds::exec {
namespace {

class CreatePatternTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));

        // One relation to declare patterns against: an integer pk, an
        // integer column and a text one, so every arm of the coercibility
        // matrix is reachable.
        server::CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
        ASSERT_EQ(d.Dispatch("CREATE TABLE account (id int64, flag int64, name varchar)")
                      .response.substr(0, 7),
                  "CREATED");
    }

    // Parses `sql` and runs the validation chain over it.
    StatusOr<PatternDdlResult> Declare(const std::string& sql) {
        auto parsed = parser::Parse(sql);
        if (!parsed.ok()) return parsed.status();
        const auto* stmt = std::get_if<parser::CreatePatternStmt>(&parsed.value());
        if (stmt == nullptr) return Status::InvalidArgument("not a CREATE PATTERN statement");
        return CreatePattern(boot_->catalog, store_, nullptr, *stmt);
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
};

// ---- The happy path -------------------------------------------------------

TEST_F(CreatePatternTest, ADeclarationRegistersAPinnedUserPatternWithADirectory) {
    auto out = Declare("CREATE PATTERN p($id int64) OF SELECT id FROM account WHERE id = $id");
    ASSERT_TRUE(out.ok()) << out.status().message();
    EXPECT_FALSE(out.value().adopted);
    EXPECT_TRUE(out.value().warnings.empty())
        << "an exact-typed pk lookup has nothing to warn about";

    auto row = boot_->catalog.GetSysPatternRow(out.value().pattern_id);
    ASSERT_TRUE(row.ok());
    // Declared and pinned by default: letting retention silently evict a
    // pattern an operator declared would defeat the declaration.
    EXPECT_EQ(row.value().origin, catalog::kOriginUser);
    EXPECT_NE(row.value().flags & catalog::kPatternPinned, 0);
    EXPECT_TRUE(catalog::HasWaystoneDirectory(row.value()));
    EXPECT_EQ(row.value().dir_depth, 1);

    // The definition carries the whole statement, not the body: it is the
    // canon a fingerprint version bump re-registers from.
    auto def = stats::FindPatternDefByName(boot_->catalog, store_, "p");
    ASSERT_TRUE(def.ok());
    ASSERT_TRUE(def.value().has_value());
    EXPECT_EQ(def.value()->source_text,
              "CREATE PATTERN p($id int64) OF SELECT id FROM account WHERE id = $id");
    EXPECT_EQ(def.value()->param_count, 1u);
}

TEST_F(CreatePatternTest, TheDeclaredBodyHashesAsTheLiveStatementItMeansToMatch) {
    auto out = Declare(
        "CREATE PATTERN p($f int64, $n varchar) "
        "OF SELECT id FROM account AS a WHERE a.flag = $f AND a.name = $n");
    ASSERT_TRUE(out.ok()) << out.status().message();

    // The end-to-end form of section 3.2: what CREATE PATTERN stored is the
    // id an ordinary statement produces. If these ever diverge the feature
    // is dead and nothing else fails.
    auto live = parser::FingerprintOf(
        "SELECT id FROM account AS a WHERE a.flag = 42 AND a.name = 'x'");
    ASSERT_TRUE(live.has_value());
    EXPECT_EQ(out.value().pattern_id, live->pattern_id);
}

TEST_F(CreatePatternTest, AnEmptyParameterListIsLegal) {
    // Such a pattern has exactly one instance: the arg_hash of an empty
    // argument stream.
    auto out = Declare("CREATE PATTERN p() OF SELECT id FROM account");
    ASSERT_TRUE(out.ok()) << out.status().message();
    EXPECT_EQ(out.value().param_count, 0u);
}

TEST_F(CreatePatternTest, ArityCountsValueSlotsNotDeclaredParameters) {
    // Section 3.3: each *occurrence* is a slot, and an inline literal is a
    // slot too. One declared parameter, used twice, beside one literal.
    auto out = Declare(
        "CREATE PATTERN p($f int64) "
        "OF SELECT id FROM account AS a WHERE a.flag = $f AND a.id = $f AND a.flag = 1");
    ASSERT_TRUE(out.ok()) << out.status().message();
    EXPECT_EQ(out.value().param_count, 3u);
}

// ---- Section 6, check by check -------------------------------------------

TEST_F(CreatePatternTest, Check2RefusesAnUndeclaredParameter) {
    auto out = Declare("CREATE PATTERN p($a int64) OF SELECT id FROM account WHERE flag = $b");
    ASSERT_FALSE(out.ok());
    EXPECT_EQ(out.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(out.status().message().find("check 2"), std::string::npos);
    EXPECT_NE(out.status().message().find("$b"), std::string::npos);
}

TEST_F(CreatePatternTest, Check3RefusesADuplicateOrUnknownlyTypedParameter) {
    auto dup = Declare(
        "CREATE PATTERN p($a int64, $a int64) OF SELECT id FROM account WHERE flag = $a");
    ASSERT_FALSE(dup.ok());
    EXPECT_NE(dup.status().message().find("check 3"), std::string::npos);

    // Resolved at CREATE, never deferred: an unknown type names nothing, so
    // check 6 would have nothing to check against.
    auto unknown =
        Declare("CREATE PATTERN p($a widget) OF SELECT id FROM account WHERE flag = $a");
    ASSERT_FALSE(unknown.ok());
    EXPECT_NE(unknown.status().message().find("check 3"), std::string::npos);
    EXPECT_NE(unknown.status().message().find("widget"), std::string::npos);
}

TEST_F(CreatePatternTest, Check4RefusesAnUnusedParameter) {
    // Harmless today, but it desynchronizes the declared arity from the
    // body's value-slot count.
    auto out = Declare("CREATE PATTERN p($a int64) OF SELECT id FROM account WHERE flag = 1");
    ASSERT_FALSE(out.ok());
    EXPECT_NE(out.status().message().find("check 4"), std::string::npos);
}

TEST_F(CreatePatternTest, Check5PropagatesTheCompilersOwnRefusal) {
    auto out = Declare("CREATE PATTERN p($a int64) OF SELECT id FROM missing WHERE id = $a");
    ASSERT_FALSE(out.ok());
    EXPECT_NE(out.status().message().find("check 5"), std::string::npos);
    // The compiler's message survives: it knows what failed, and rewording
    // it here would lose that.
    EXPECT_NE(out.status().message().find("no table with this name"), std::string::npos);
}

TEST_F(CreatePatternTest, Check6WarnsOnACoercibleMismatch) {
    // bool against int64: the comparison works and converts on every
    // execution. A cost and a likely mistake, not an invalid pattern - so
    // the declaration succeeds and says so.
    auto out = Declare("CREATE PATTERN p($f bool) OF SELECT id FROM account WHERE flag = $f");
    ASSERT_TRUE(out.ok()) << out.status().message();

    // Searched by name rather than by index: this body is also scan-only,
    // so check 8 contributes a warning of its own. Two independent
    // observations about one declaration is the normal case, and a test
    // that counted them would break every time a check is added.
    bool found = false;
    for (const std::string& warning : out.value().warnings) {
        if (warning.find("implicit conversion") == std::string::npos) continue;
        EXPECT_NE(warning.find("$f"), std::string::npos);
        EXPECT_NE(warning.find("bool"), std::string::npos);
        EXPECT_NE(warning.find("int64"), std::string::npos);
        found = true;
    }
    EXPECT_TRUE(found) << "bool against an int64 column must warn, not pass silently";
}

TEST_F(CreatePatternTest, Check6ErrorsOnAnIncoercibleMismatch) {
    // varchar against an int64 column. CompareValues answers false for a
    // type mismatch, so this predicate could never match - the pattern
    // could never do what it says.
    auto out = Declare("CREATE PATTERN p($f varchar) OF SELECT id FROM account WHERE flag = $f");
    ASSERT_FALSE(out.ok());
    EXPECT_EQ(out.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(out.status().message().find("check 6"), std::string::npos);
    EXPECT_NE(out.status().message().find("could never match"), std::string::npos);
}

TEST_F(CreatePatternTest, Check6ChecksEveryOccurrenceOfOneParameter) {
    // The declared type is a single contract every predicate has to
    // satisfy, so a parameter used in two places is checked in both - and
    // gets a line per offending occurrence, since which one is wrong is
    // exactly what the operator needs.
    auto out = Declare(
        "CREATE PATTERN p($f bool) "
        "OF SELECT id FROM account AS a WHERE a.flag = $f AND a.id = $f");
    ASSERT_TRUE(out.ok()) << out.status().message();
    EXPECT_EQ(out.value().warnings.size(), 2u);
}

TEST_F(CreatePatternTest, Check6SkipsAConjunctEqualityPropagationDerived) {
    // Propagation (docs/spec/parser-v2.md §5) derives `other.aid = $f` from the
    // ON equality and the written `account.flag = $f`. Check 6 must name
    // only what the client wrote - a line about `aid` points at a predicate
    // they cannot find - and skipping the derived occurrence loses nothing:
    // the descriptor guard makes both columns the same type, so its verdict
    // is never different.
    server::CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE other (id int64, aid int64)").response.substr(0, 7),
              "CREATED");

    // The warning half: one implicit-conversion line, for the written
    // occurrence alone.
    auto warned = Declare(
        "CREATE PATTERN p($f bool) OF SELECT other.id FROM other "
        "JOIN account ON other.aid = account.flag WHERE account.flag = $f");
    ASSERT_TRUE(warned.ok()) << warned.status().message();
    // Counted by kind rather than in total, because this body also earns
    // check 8's replayability warning; what is pinned is that no line of
    // any kind names the derived column.
    std::size_t conversions = 0;
    for (const std::string& w : warned.value().warnings) {
        EXPECT_EQ(w.find("aid"), std::string::npos) << w;
        if (w.find("implicit conversion") != std::string::npos) ++conversions;
    }
    EXPECT_EQ(conversions, 1u);

    // The refusal half: named at the written column, not the derived one.
    auto refused = Declare(
        "CREATE PATTERN q($f varchar) OF SELECT other.id FROM other "
        "JOIN account ON other.aid = account.flag WHERE account.flag = $f");
    ASSERT_FALSE(refused.ok());
    EXPECT_NE(refused.status().message().find("flag"), std::string::npos)
        << refused.status().message();
    EXPECT_EQ(refused.status().message().find("aid"), std::string::npos)
        << refused.status().message();
}

TEST_F(CreatePatternTest, Check8WarnsWhenNoStepCouldEverReplay) {
    // A non-pk predicate is a search, and invariant 9 forbids a trail from
    // replacing one. Legal to declare; being surprised later is not.
    auto scan = Declare("CREATE PATTERN p($f int64) OF SELECT id FROM account WHERE flag = $f");
    ASSERT_TRUE(scan.ok()) << scan.status().message();
    ASSERT_EQ(scan.value().warnings.size(), 1u);
    EXPECT_NE(scan.value().warnings[0].find("can never replay"), std::string::npos);
}

TEST_F(CreatePatternTest, APkEqualityAgainstAParameterIsStillALookup) {
    // The compiler change this feature needed. A `$param` stands for the
    // integer traffic will supply, so it is pk-eligible; if it were not,
    // every declared point lookup - the exact shape declaring exists to
    // make replayable - would be reported as un-replayable.
    auto out = Declare("CREATE PATTERN p($id int64) OF SELECT id FROM account WHERE id = $id");
    ASSERT_TRUE(out.ok()) << out.status().message();
    EXPECT_TRUE(out.value().warnings.empty());
}

TEST_F(CreatePatternTest, Check9RefusesADuplicateName) {
    ASSERT_TRUE(Declare("CREATE PATTERN p($a int64) OF SELECT id FROM account WHERE id = $a").ok());
    auto out = Declare("CREATE PATTERN p($a int64) OF SELECT id FROM account WHERE flag = $a");
    ASSERT_FALSE(out.ok());
    EXPECT_NE(out.status().message().find("check 9"), std::string::npos);
}

TEST_F(CreatePatternTest, Check10RefusesTheSameShapeTwiceAndNamesTheExistingOne) {
    ASSERT_TRUE(
        Declare("CREATE PATTERN first($a int64) OF SELECT id FROM account WHERE id = $a").ok());

    auto out =
        Declare("CREATE PATTERN second($b int64) OF SELECT id FROM account WHERE id = $b");
    ASSERT_FALSE(out.ok());
    EXPECT_NE(out.status().message().find("check 10"), std::string::npos);
    // Under a different name, so the message has to say which - otherwise
    // the operator is told there is a duplicate and cannot find it.
    EXPECT_NE(out.status().message().find("first"), std::string::npos);
}

TEST_F(CreatePatternTest, Check11ValidatesEveryOption) {
    auto unknown = Declare(
        "CREATE PATTERN p($a int64) WITH (turbo = on) OF SELECT id FROM account WHERE id = $a");
    ASSERT_FALSE(unknown.ok());
    EXPECT_NE(unknown.status().message().find("check 11"), std::string::npos);
    EXPECT_NE(unknown.status().message().find("turbo"), std::string::npos);

    auto bad_pinned = Declare(
        "CREATE PATTERN p($a int64) WITH (pinned = maybe) OF SELECT id FROM account WHERE id = $a");
    ASSERT_FALSE(bad_pinned.ok());
    EXPECT_NE(bad_pinned.status().message().find("check 11"), std::string::npos);

    auto bad_instances = Declare(
        "CREATE PATTERN p($a int64) WITH (expected_instances = 0) "
        "OF SELECT id FROM account WHERE id = $a");
    ASSERT_FALSE(bad_instances.ok());
    EXPECT_NE(bad_instances.status().message().find("check 11"), std::string::npos);
}

TEST_F(CreatePatternTest, PinnedOffClearsThePinButLeavesTheOriginUser) {
    // The two are separate fields on purpose: a declared pattern may be
    // created unpinned, and an auto one may be pinned later.
    auto out = Declare(
        "CREATE PATTERN p($a int64) WITH (pinned = off) OF SELECT id FROM account WHERE id = $a");
    ASSERT_TRUE(out.ok()) << out.status().message();

    auto row = boot_->catalog.GetSysPatternRow(out.value().pattern_id);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().flags & catalog::kPatternPinned, 0);
    EXPECT_EQ(row.value().origin, catalog::kOriginUser);
}

// ---- Pre-sizing the directory (section 4.3) -------------------------------

TEST_F(CreatePatternTest, ExpectedInstancesPicksTheDirectoryDepth) {
    // The option exposes an instance count, not a depth: the operator
    // should not have to know the 2048 fanout. Growth is a cache flush
    // (waystone_dir.hpp), which is what makes pre-sizing the mitigation
    // rather than a convenience.
    struct Case {
        const char* instances;
        int depth;
    };
    const Case cases[] = {
        {"1", 1}, {"2048", 1}, {"2049", 2}, {"4194304", 2}, {"4194305", 3},
        // Past what six levels address; clamped rather than refused.
        {"99999999999999999999", 6},
    };

    int n = 0;
    for (const Case& c : cases) {
        // Each body needs a *distinct shape*, and varying a literal does not
        // give one - a literal is an argument, not shape, so `flag = 1` and
        // `flag = 2` are one pattern and the second declaration would be
        // refused by check 10. Varying the number of conjuncts does.
        std::string body = "SELECT id FROM account WHERE id = $a";
        for (int i = 0; i < n; ++i) body += " AND flag = 1";

        const std::string name = "p" + std::to_string(n++);
        auto out = Declare("CREATE PATTERN " + name + "($a int64) WITH (expected_instances = " +
                           c.instances + ") OF " + body);
        ASSERT_TRUE(out.ok()) << c.instances << ": " << out.status().message();
        EXPECT_EQ(out.value().dir_depth, c.depth) << "for expected_instances = " << c.instances;
    }
}

// ---- Adoption (check 10's middle branch) ---------------------------------

TEST_F(CreatePatternTest, DeclaringAnAutoRegisteredShapeAdoptsItAndKeepsItsTrails) {
    const std::string body = "SELECT id FROM account WHERE id = 7";
    auto fingerprint = parser::FingerprintOf(body);
    ASSERT_TRUE(fingerprint.has_value());

    // Stand in for auto-registration, which does not exist yet: an unpinned
    // kOriginAuto row with a directory already built by traffic.
    ASSERT_TRUE(boot_->catalog
                    .RegisterPattern(fingerprint->pattern_id, catalog::kStmtClassUnclassified)
                    .ok());
    auto root = stats::CreateDirPage(store_);
    ASSERT_TRUE(root.ok());
    ASSERT_TRUE(
        boot_->catalog.SetPatternWaystoneRoot(fingerprint->pattern_id, root.value(), 3).ok());

    auto out = Declare("CREATE PATTERN warm($a int64) OF SELECT id FROM account WHERE id = $a");
    ASSERT_TRUE(out.ok()) << out.status().message();
    EXPECT_TRUE(out.value().adopted);
    EXPECT_EQ(out.value().pattern_id, fingerprint->pattern_id);

    auto row = boot_->catalog.GetSysPatternRow(fingerprint->pattern_id);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.value().origin, catalog::kOriginUser);
    EXPECT_NE(row.value().flags & catalog::kPatternPinned, 0);

    // **The warm cache survives.** Adoption must not throw away the trails
    // traffic already recorded, or declaring a hot pattern would be a
    // performance regression. The depth is left alone too - regrowing it
    // would flush the directory, which an operator can ask for explicitly
    // by dropping and re-creating.
    EXPECT_EQ(row.value().waystone_root, root.value());
    EXPECT_EQ(row.value().dir_depth, 3);
    EXPECT_EQ(out.value().dir_depth, 3);
}

// ---- DROP -----------------------------------------------------------------

TEST_F(CreatePatternTest, DropRemovesBothRowsAndFreesTheName) {
    auto out = Declare("CREATE PATTERN p($a int64) OF SELECT id FROM account WHERE id = $a");
    ASSERT_TRUE(out.ok()) << out.status().message();

    auto dropped = DropPattern(boot_->catalog, store_, nullptr, "p");
    ASSERT_TRUE(dropped.ok()) << dropped.status().message();
    EXPECT_EQ(dropped.value(), out.value().pattern_id);

    // Both rows are gone: the definition and the sys.patterns row. Leaving
    // the latter would silently turn the declaration into an observation.
    auto def = stats::FindPatternDefByName(boot_->catalog, store_, "p");
    ASSERT_TRUE(def.ok());
    EXPECT_FALSE(def.value().has_value());
    EXPECT_EQ(boot_->catalog.GetSysPatternRow(out.value().pattern_id).status().code(),
              StatusCode::kNotFound);

    // And the shape is declarable again - which is why the rows are retired
    // rather than delete-marked.
    EXPECT_TRUE(
        Declare("CREATE PATTERN p($a int64) OF SELECT id FROM account WHERE id = $a").ok());
}

TEST_F(CreatePatternTest, DroppingAnUnknownNameIsNotFound) {
    EXPECT_EQ(DropPattern(boot_->catalog, store_, nullptr, "nope").status().code(), StatusCode::kNotFound);
}

TEST_F(CreatePatternTest, DropIsCaseInsensitiveLikeEveryOtherIdentifier) {
    ASSERT_TRUE(
        Declare("CREATE PATTERN AcctTrades($a int64) OF SELECT id FROM account WHERE id = $a")
            .ok());
    EXPECT_TRUE(DropPattern(boot_->catalog, store_, nullptr, "accttrades").ok());
}

// ---- A declared chain is not an executable one ---------------------------

TEST_F(CreatePatternTest, AParameterOutsideADeclarationIsRefusedByTheParser) {
    // Section 3.1: the token exists and fingerprints - it is reserved for
    // the extended protocol's named binds - but no other production takes
    // one, so a chain carrying an unbound parameter can never be built from
    // ordinary traffic in the first place.
    auto parsed = parser::Parse("SELECT * FROM account WHERE id = $x");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_NE(parsed.status().message().find("CREATE PATTERN"), std::string::npos);
}

}  // namespace
}  // namespace kds::exec
