#include "inner_build_fixture.hpp"

#include "kds/server/command_dispatcher.hpp"
#include "kds/wal/manager.hpp"

// JB5's contract (docs/workplan-join-inner-build.md): the build is a
// shortcut with the walk always underneath, so **every configuration of
// `join_build_max_rows` answers the same bytes** — the default (builds and
// probes), 0 (the off-switch: the pure walk), and caps the fixture
// exceeds (the Cabin's fall-back refusal mid-build). No statement errors
// at any value. The cap-0 reference's soundness is the sibling exec
// suite's hand-computed pins over the same fixture — structural, because
// both files include inner_build_fixture.hpp.

namespace kds::exec {
namespace {

class InnerBuildContractTest : public InnerBuildFixture {};

TEST_F(InnerBuildContractTest, EveryCapAnswersTheSameBytes) {
    // The whole contract in one sweep: default (build + probe), 0 (pure
    // walk), 1 and 2 (capped mid-build), 5 (exactly the inner row count -
    // the boundary that must still publish). Byte-for-byte, order
    // included. The EXISTS row cannot deviate today - the stopping class
    // is gated out of the arm at every cap - and is here so the sweep
    // starts failing the day JB6 arms it with the rule wrong.
    const char* statements[] = {
        "SELECT au.name, tr.qty FROM au JOIN tr ON tr.au_id = au.id",
        "SELECT au.name, tr.qty FROM au JOIN tr ON tr.au_id = au.id WHERE tr.qty <= 30",
        "SELECT au.name, tr.qty, ln.amt FROM au JOIN tr ON tr.au_id = au.id "
        "JOIN ln ON ln.tr_qty = tr.qty",
        "SELECT au.name FROM au WHERE EXISTS (SELECT tr.id FROM tr WHERE tr.au_id = au.id)",
    };
    for (const char* sql : statements) {
        const std::vector<std::string> reference = Run(sql, nullptr, /*cap=*/0);
        for (std::size_t cap : {std::size_t{1}, std::size_t{2}, std::size_t{5},
                                kDefaultJoinBuildMaxRows}) {
            EXPECT_EQ(Run(sql, nullptr, cap), reference) << sql << " at cap " << cap;
        }
    }
}

// BM-S3: the scan kernel off and on answer the same rows at every cap. The
// kernel never holds the build's correlated conjunct, and a row it rejects
// is one no bucket and no emission would have kept.
TEST_F(InnerBuildContractTest, TheScanKernelSwitchChangesNoRowAtAnyCap) {
    const char* statements[] = {
        "SELECT au.name, tr.qty FROM au JOIN tr ON tr.au_id = au.id WHERE tr.qty <= 30",
        "SELECT au.name, tr.qty FROM au JOIN tr ON tr.au_id = au.id WHERE tr.qty > 10",
        "SELECT au.name FROM au WHERE EXISTS (SELECT tr.id FROM tr WHERE tr.au_id = au.id "
        "AND tr.qty >= 20)",
    };
    for (const char* sql : statements) {
        for (std::size_t cap : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{5},
                                kDefaultJoinBuildMaxRows}) {
            ExecStats on_stats;
            ExecStats off_stats;
            const auto on = Run(sql, &on_stats, cap, 0, /*scan_kernel=*/true);
            const auto off = Run(sql, &off_stats, cap, 0, /*scan_kernel=*/false);
            EXPECT_EQ(on, off) << sql << " at cap " << cap;
            EXPECT_EQ(on_stats.Total().rows_examined, off_stats.Total().rows_examined)
                << sql << " at cap " << cap;
            EXPECT_EQ(on_stats.Total().rows_matched, off_stats.Total().rows_matched)
                << sql << " at cap " << cap;
            EXPECT_EQ(on_stats.Total().build_rows, off_stats.Total().build_rows)
                << sql << " at cap " << cap;
        }
    }
}

TEST_F(InnerBuildContractTest, TheCapDeclinesMidBuildAndNeverErrors) {
    // Cap 2 against a 5-row inner: the first walk buckets two entries,
    // trips, and the publish site declines the step - later outer rows
    // walk plain (the examined count is the pure walk's), nothing
    // rebuilds, nothing errors.
    ExecStats stats;
    const std::vector<std::string> rows =
        Run("SELECT au.name, tr.qty FROM au JOIN tr ON tr.au_id = au.id", &stats, /*cap=*/2);
    EXPECT_EQ(rows, (std::vector<std::string>{"alice|10", "alice|30", "bob|20", "bob|50"}));
    const StepStats total = stats.Total();
    EXPECT_EQ(total.inner_builds, 0u);
    EXPECT_EQ(total.build_rows, 2u) << "bucketed to the cap once, never re-attempted";
    EXPECT_EQ(total.build_probes, 0u);
    ASSERT_GE(stats.steps.size(), 2u);
    EXPECT_EQ(stats.steps[1].rows_examined, 15u) << "per-row walks for the rest, k*N";
}

TEST_F(InnerBuildContractTest, CapZeroIsThePureWalk) {
    // The off-switch: nothing arms, nothing buckets, the statement is the
    // pre-build executor's byte for byte and cost for cost.
    ExecStats stats;
    const std::vector<std::string> rows =
        Run("SELECT au.name, tr.qty FROM au JOIN tr ON tr.au_id = au.id", &stats, /*cap=*/0);
    EXPECT_EQ(rows, (std::vector<std::string>{"alice|10", "alice|30", "bob|20", "bob|50"}));
    const StepStats total = stats.Total();
    EXPECT_EQ(total.inner_builds, 0u);
    EXPECT_EQ(total.build_rows, 0u);
    EXPECT_EQ(total.build_probes, 0u);
    ASSERT_GE(stats.steps.size(), 2u);
    EXPECT_EQ(stats.steps[1].rows_examined, 15u);
}

TEST_F(InnerBuildContractTest, TheExactFitCapStillPublishes) {
    // Five rows, cap five: the trip fires only on a row that would
    // *exceed* the cap, so an exact fit publishes and probes - the
    // boundary the off-by-one would break.
    ExecStats stats;
    Run("SELECT au.name, tr.qty FROM au JOIN tr ON tr.au_id = au.id", &stats, /*cap=*/5);
    const StepStats total = stats.Total();
    EXPECT_EQ(total.inner_builds, 1u);
    EXPECT_EQ(total.build_rows, 5u);
    EXPECT_EQ(total.build_probes, 2u);
}

// The config route, dispatcher-level (the house contract suites'
// convention): `set_join_build_max_rows` mutates the dispatcher's budget
// template - the same path an expeditor-parsed kds.conf key takes - and
// the template rides into every Execute. The VM-level tests above cannot
// see this plumbing; this one drives it end to end.
std::string RunThroughDispatcher(bool knob_off, bool with_null_row) {
    storage::InMemoryPageStore store{server::kFirstUserPageId};
    auto boot = bootstrap::BootstrapDatabase(store, 1000);
    EXPECT_TRUE(boot.ok()) << boot.status().message();
    server::CommandDispatcher dispatcher(
        boot.value().superblock, boot.value().catalog, store, /*log=*/nullptr,
        /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup, exec::Budget(),
        /*recorder=*/nullptr, /*replay_enabled=*/false, /*access_statistics=*/true,
        /*cabins=*/nullptr);
    if (knob_off) dispatcher.set_join_build_max_rows(0);

    EXPECT_EQ(dispatcher.Dispatch("CREATE TABLE au (id int64, name varchar)")
                  .response.substr(0, 7),
              "CREATED");
    EXPECT_EQ(dispatcher.Dispatch("CREATE TABLE tr (id int64, au_id int64 NULL, qty int64)")
                  .response.substr(0, 7),
              "CREATED");
    // Single-row inserts: the bare dispatcher here has no transaction
    // manager, and a multi-row INSERT refuses without one.
    for (const char* insert : {"INSERT INTO au VALUES ('alice')", "INSERT INTO au VALUES ('bob')",
                               "INSERT INTO tr VALUES (1, 10)", "INSERT INTO tr VALUES (2, 20)",
                               "INSERT INTO tr VALUES (1, 30)"}) {
        EXPECT_EQ(dispatcher.Dispatch(insert).response.substr(0, 8), "INSERTED") << insert;
    }
    if (with_null_row) {
        EXPECT_EQ(dispatcher.Dispatch("INSERT INTO tr VALUES (NULL, 40)").response.substr(0, 8),
                  "INSERTED");
    }
    return dispatcher.Dispatch("SELECT au.name, tr.qty FROM au JOIN tr ON tr.au_id = au.id")
        .response;
}

TEST_F(InnerBuildContractTest, TheDispatcherKnobReachesTheExecutor) {
    const std::string built = RunThroughDispatcher(/*knob_off=*/false, /*with_null_row=*/false);
    const std::string walked = RunThroughDispatcher(/*knob_off=*/true, /*with_null_row=*/false);
    EXPECT_EQ(built, walked);
    EXPECT_NE(built.find("alice"), std::string::npos) << built;
}

TEST_F(InnerBuildContractTest, ANullJoinValueAnswersTheSameBuiltOrWalked) {
    // NULL storage landed upstream (NU1-NU8) the same day as the build,
    // which makes the once-unreachable paths real: a NULL inner join
    // value enters no bucket (MakeValueKey refuses kNull) and matches no
    // equality on the walk - three-valued WHERE filters it either way -
    // so the built and walked replies must agree byte for byte, and the
    // NULL row's qty must appear in neither.
    const std::string built = RunThroughDispatcher(/*knob_off=*/false, /*with_null_row=*/true);
    const std::string walked = RunThroughDispatcher(/*knob_off=*/true, /*with_null_row=*/true);
    EXPECT_EQ(built, walked);
    EXPECT_NE(built.find("alice"), std::string::npos) << built;
    EXPECT_EQ(built.find("40"), std::string::npos)
        << "a NULL key matches no equality: " << built;
}

TEST_F(InnerBuildContractTest, ANullProbeNotInStopsItsWalkAndAnswersTheSameBuiltOrWalked) {
    // A NULL probe settles NOT IN at the first inner row of its bucket, so
    // the first outer row's walk stops early and leaves a partial build;
    // the next outer row's match (tr (2,50)) lies past where it stopped.
    // np = (1, NULL) (2, 50) (3, 99): row 1 is unknown, row 2 matches,
    // row 3's bucket is empty - only 3 is admitted, at every cap.
    catalog::Schema schema;
    for (std::uint32_t pos : {0u, 1u}) {
        catalog::SysColumnRow row{};
        row.pos = pos;
        catalog::SetName(row.name, pos == 0 ? "id" : "v");
        row.type_val = catalog::kTypeValInt64;
        row.len = 8;
        row.notnull = pos == 0;
        schema.columns.push_back(row);
    }
    ASSERT_TRUE(boot_->catalog
                    .CreateTable(catalog::kNamespacePublic, "np", schema,
                                 catalog::ClusteredType::kBtree)
                    .ok());
    parser::AstValue null_value;
    null_value.type = parser::ValueType::kNull;
    Insert("np", {null_value});
    Insert("np", {Int(50)});
    Insert("np", {Int(99)});

    const char* sql =
        "SELECT np.id FROM np WHERE np.v NOT IN (SELECT tr.qty FROM tr WHERE tr.au_id = np.id)";
    const std::vector<std::string> reference = Run(sql, nullptr, /*cap=*/0);
    EXPECT_EQ(reference, (std::vector<std::string>{"3"}));
    for (std::size_t cap : {std::size_t{1}, std::size_t{2}, std::size_t{5},
                            kDefaultJoinBuildMaxRows}) {
        EXPECT_EQ(Run(sql, nullptr, cap), reference) << "cap " << cap;
    }
}

TEST_F(InnerBuildContractTest, OneCappedStepDoesNotDeclineItsSibling) {
    // The [OPEN] scoping question's built behavior, pinned: one knob,
    // checked per build (CLAUDE.md's index carries the open half). Cap 4
    // against tr (5 rows) and ln (4): the tr step trips and declines to
    // per-row walks; the ln step is an exact fit, publishes, and probes -
    // independently, with the reply byte-identical to the walk's.
    ExecStats stats;
    const std::vector<std::string> rows =
        Run("SELECT au.name, tr.qty, ln.amt FROM au JOIN tr ON tr.au_id = au.id "
            "JOIN ln ON ln.tr_qty = tr.qty",
            &stats, /*cap=*/4);
    EXPECT_EQ(rows, (std::vector<std::string>{"alice|10|100", "alice|10|101", "alice|30|300",
                                              "bob|50|500"}));
    ASSERT_GE(stats.steps.size(), 3u);
    EXPECT_EQ(stats.steps[1].inner_builds, 0u);
    EXPECT_EQ(stats.steps[1].build_rows, 4u) << "capped: bucketed to 4 of 5, once";
    EXPECT_EQ(stats.steps[1].build_probes, 0u);
    EXPECT_EQ(stats.steps[1].rows_examined, 15u) << "declined: the k*N walk";
    EXPECT_EQ(stats.steps[2].inner_builds, 1u);
    EXPECT_EQ(stats.steps[2].build_rows, 4u) << "exact fit: published";
    EXPECT_EQ(stats.steps[2].build_probes, 3u);
    EXPECT_EQ(stats.steps[2].rows_examined, 6u) << "one build walk plus bucket entries";
}

}  // namespace
}  // namespace kds::exec
