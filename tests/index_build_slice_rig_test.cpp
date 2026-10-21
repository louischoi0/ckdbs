// **An index build yields its core between slices** (A1 stage 2, `sched.md`
// §3; `BuildRun`).
//
// A `CREATE INDEX` or `REINDEX INDEX` backfill gives its core back every
// `SlicePolicy::pages_per_slice` relation leaves, as a `SELECT`'s walk does
// (`walk_slice_rig_test.cpp`), so a second statement on the same core runs
// inside the build. Each cell below puts that second statement there - one
// core, both tasks submitted before the reactor starts, the build first -
// and asks what the build held across the gap: another relation's point read
// is answered before the build ends; a writer of the built relation waits on
// the `X` the build still holds and its row is in the finished index; a
// schema-word move frees no memo the build reads and the core's epoch stays
// at the build's word; a cancel ends the build with nothing published, and
// a cancelled rebuild leaves the old index as it was; a core torn down
// between slices aborts it. A crash cell asks that the row the tail writes
// carries its undo record, which the tail's hook writes. The declaration
// builds - `CREATE ASSERTION`, `CREATE TRANSITION` - have the same cells
// (A1 stage 2b), and a crash between an assertion build's slices mounts
// without it.

#include "file_rig_crash.hpp"
#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/session.hpp"
#include "kds/server/statement_epoch.hpp"
#include "kds/txn/lock_table.hpp"
#include "kds/wal/durability.hpp"
#include "view_text.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

// A `varchar(2000)` cell makes a row ~2 KB, so a leaf holds about four and
// `kRows` rows are ~`kRows / 4` leaves - several slices of 64 leaves.
constexpr int kRows = 1600;

struct Script {
    Session* session = nullptr;
    std::vector<std::string> lines;
    std::vector<DispatchOutcome> outs;
    std::atomic<std::size_t> done{0};
    // Run inside the script, on the core's thread, before its first line
    // and after its last.
    std::function<void()> before;
    std::function<void()> after;
};

sched::Coro RunScript(CommandDispatcher& d, Script& s) {
    s.outs.resize(s.lines.size());
    if (s.before) s.before();
    for (std::size_t i = 0; i < s.lines.size(); ++i) {
        co_await d.DispatchAsync(s.lines[i], s.session, &s.outs[i]);
        s.done.store(i + 1, std::memory_order_release);
    }
    if (s.after) s.after();
    co_return Status::OK();
}

class IndexBuildSliceRigTest : public ::testing::Test {
protected:
    void SetUp() override {
        TwoCoreRig::Options options;
        options.max_idle_block_ms = 20;
        auto opened = TwoCoreRig::Open(options);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        rig_ = std::move(opened.value());
        for (const char* table : {"CREATE TABLE t (id int64, v int64, name varchar(2000))",
                                  "CREATE TABLE u (id int64)"}) {
            ASSERT_EQ(d().Dispatch(table).response.rfind("CREATED", 0), 0u);
        }
        ASSERT_EQ(d().Dispatch("INSERT INTO u VALUES (5)").response.rfind("INSERTED", 0), 0u);
        for (Session* s : {&build_, &other_}) s->set_durability(wal::DurabilityClass::kRelaxed);
        for (int i = 1; i <= kRows;) {
            std::string sql = "INSERT INTO t VALUES ";
            for (int k = 0; k < 100 && i <= kRows; ++k, ++i) {
                if (k != 0) sql += ", ";
                sql += "(" + std::to_string(i) + ", " + std::to_string(i) + ", 'x')";
            }
            const std::string reply = d().Dispatch(sql).response;
            ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
        }
    }
    void TearDown() override {
        if (rig_ != nullptr) rig_->Stop();
    }

    CommandDispatcher& d() { return rig_->core(0).dispatcher(); }

    // Both scripts on core 0, the build first: it runs to its first slice
    // boundary, and `other` runs there.
    void RunBoth(Script& build, Script& other) {
        rig_->core(0).scheduler().Submit(
            sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(d(), build)));
        rig_->core(0).scheduler().Submit(
            sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(d(), other)));
        rig_->Start();
        ASSERT_TRUE(Within(60000ms, [&] {
            return build.done.load() == build.lines.size() &&
                   other.done.load() == other.lines.size();
        })) << "the scripts never finished";
        rig_->Stop();
    }

    // The entries `sys.indexes` reports for `name`.
    std::string Entries(const std::string& name) {
        const std::string shown = d().Dispatch("SELECT name, entries FROM sys.indexes").response;
        return testing_view::ReadViewText(shown).Find("name", name, "entries").value_or("");
    }

    // Declared before the rig, so it outlives every statement the rig ran.
    StatementEpochs epochs_{2};
    std::unique_ptr<TwoCoreRig> rig_;
    Session build_, other_;
};

TEST_F(IndexBuildSliceRigTest, APointReadSharingACoreWithABuildIsAnsweredInsideIt) {
    // **Mutation**, measured: `BuildRun::HandOn` running the walk whole
    // where the statement may park - killed 1 in 1.
    Script build{&build_, {"CREATE INDEX tix ON t (v)"}};
    Script point{&other_, {"SELECT id FROM u WHERE id = 5"}};
    std::atomic<bool> build_done_at_point{true};
    point.after = [&] { build_done_at_point.store(build.done.load() == 1); };
    RunBoth(build, point);

    EXPECT_FALSE(build_done_at_point.load())
        << "the point read waited for the whole build: nothing yielded";
    EXPECT_NE(point.outs[0].response.find("5"), std::string::npos) << point.outs[0].response;
    ASSERT_EQ(build.outs[0].response.rfind("CREATED INDEX", 0), 0u) << build.outs[0].response;
    EXPECT_EQ(Entries("tix"), std::to_string(kRows));
    const std::string plan = d().Dispatch("ANALYZE SELECT id FROM t WHERE v = 777").response;
    EXPECT_NE(plan.find("IndexProbe"), std::string::npos) << plan;
    EXPECT_NE(d().Dispatch("SELECT id FROM t WHERE v = 777").response.find("777"),
              std::string::npos);
}

TEST_F(IndexBuildSliceRigTest, AWriterOnTheBuildsCoreWaitsForItsXAndItsRowIsInTheIndex) {
    // The `X` is held across every yield: a writer of the relation that
    // runs inside the build parks on it, and resumes into the finished
    // index once the build's tail has decided.
    Script build{&build_, {"CREATE INDEX tix ON t (v)"}};
    Script writer{&other_, {"INSERT INTO t VALUES (99999, 99999, 'y')"}};
    std::atomic<bool> build_running_at_write{false};
    std::atomic<bool> build_done_after_write{false};
    writer.before = [&] { build_running_at_write.store(build.done.load() == 0); };
    writer.after = [&] { build_done_after_write.store(build.done.load() == 1); };
    RunBoth(build, writer);

    ASSERT_TRUE(build_running_at_write.load()) << "the writer ran after the build: nothing yielded";
    ASSERT_EQ(build.outs[0].response.rfind("CREATED INDEX", 0), 0u) << build.outs[0].response;
    ASSERT_EQ(writer.outs[0].response.rfind("INSERTED", 0), 0u) << writer.outs[0].response;
    EXPECT_TRUE(build_done_after_write.load()) << "the writer ran past the build's X";
    EXPECT_EQ(Entries("tix"), std::to_string(kRows + 1));
    EXPECT_NE(d().Dispatch("SELECT id FROM t WHERE v = 99999").response.find("99999"),
              std::string::npos)
        << "the writer's row is missing from the finished index";
}

TEST_F(IndexBuildSliceRigTest, ASchemaWordMoveDuringASuspensionFreesNoMemoTheBuildHolds) {
    StatementEpochs& epochs = epochs_;
    d().SetStatementEpochs(&epochs);
    catalog::Catalog& catalog = d().catalog();

    Script build{&build_, {"CREATE INDEX tix ON t (v)"}};
    // A DDL moves the schema word; the next statement's head revalidates,
    // which frees the memo the suspended build reads - or, pinned, retires
    // it.
    Script other{&other_, {"CREATE TABLE w (id int64)", "SELECT id FROM u WHERE id = 5"}};
    std::atomic<bool> build_running{false};
    std::atomic<std::size_t> retired{0};
    std::atomic<bool> epoch_held{false};
    other.after = [&] {
        build_running.store(build.done.load() == 0);
        retired.store(catalog.cache_retired_generations());
        epoch_held.store(!epochs.AllAtLeast(catalog.schema_word_now()));
    };
    RunBoth(build, other);

    ASSERT_TRUE(build_running.load()) << "the build ended before the DDL ran: nothing yielded";
    EXPECT_GT(retired.load(), 0u) << "the revalidation freed the memo under the build";
    EXPECT_TRUE(epoch_held.load()) << "the core published a word above the suspended build's";
    ASSERT_EQ(build.outs[0].response.rfind("CREATED INDEX", 0), 0u) << build.outs[0].response;
    EXPECT_EQ(Entries("tix"), std::to_string(kRows));
    // Given back with the build.
    EXPECT_EQ(catalog.cache_retired_generations(), 0u);
    EXPECT_TRUE(epochs.AllAtLeast(catalog.schema_word_now()));
    d().SetStatementEpochs(nullptr);
}

TEST_F(IndexBuildSliceRigTest, ACancelEndsTheBuildAtItsNextBoundaryWithNothingPublished) {
    // **Mutation**, measured: the slice boundary taking no cancel - killed
    // 1 in 1.
    Script build{&build_, {"CREATE INDEX tix ON t (v)"}};
    Script canceller{&other_, {}};
    std::atomic<bool> build_running{false};
    canceller.after = [&] {
        build_running.store(build.done.load() == 0);
        build_.RequestCancel();
    };
    RunBoth(build, canceller);

    ASSERT_TRUE(build_running.load()) << "the build ended before the cancel: nothing yielded";
    EXPECT_EQ(build.outs[0].status.code(), StatusCode::kCancelled) << build.outs[0].response;
    EXPECT_EQ(build.outs[0].response.rfind("ERR", 0), 0u);
    EXPECT_FALSE(build_.TakeCancel()) << "the build took the cancel: one cancel, one statement";
    EXPECT_FALSE(d().catalog().FindIndexByName("tix").ok()) << "a cancelled build published";
    EXPECT_EQ(rig_->locks().progress().Registered(), 0u);
    // The `X` went with the abort: a writer is admitted at once, and the
    // name is free for the next build.
    EXPECT_EQ(
        d().Dispatch("INSERT INTO t VALUES (99999, 99999, 'y')").response.rfind("INSERTED", 0),
        0u);
    EXPECT_EQ(d().Dispatch("CREATE INDEX tix ON t (v)").response.rfind("CREATED INDEX", 0), 0u);
}

TEST_F(IndexBuildSliceRigTest, ACancelFailsTheExplicitTransactionTheBuildRanIn) {
    Script build{&build_, {"BEGIN", "CREATE INDEX tix ON t (v)"}};
    Script canceller{&other_, {}};
    std::atomic<bool> build_running{false};
    canceller.after = [&] {
        build_running.store(build.done.load() == 1);
        build_.RequestCancel();
    };
    RunBoth(build, canceller);

    ASSERT_TRUE(build_running.load()) << "the build ended before the cancel: nothing yielded";
    EXPECT_EQ(build.outs[1].status.code(), StatusCode::kCancelled) << build.outs[1].response;
    EXPECT_TRUE(build_.failed()) << "a cancelled statement leaves its transaction failed";
    EXPECT_EQ(d().Dispatch("ROLLBACK", &build_).response.rfind("ROLLBACK", 0), 0u);
    EXPECT_FALSE(d().catalog().FindIndexByName("tix").ok());
}

TEST_F(IndexBuildSliceRigTest, AReindexYieldsAndReplacesTheIndex) {
    ASSERT_EQ(d().Dispatch("CREATE INDEX tix ON t (v)").response.rfind("CREATED INDEX", 0), 0u);
    auto old_ix = d().catalog().FindIndexByName("tix");
    ASSERT_TRUE(old_ix.ok());
    Script build{&build_, {"REINDEX INDEX tix"}};
    Script point{&other_, {"SELECT id FROM u WHERE id = 5"}};
    std::atomic<bool> build_done_at_point{true};
    point.after = [&] { build_done_at_point.store(build.done.load() == 1); };
    RunBoth(build, point);

    EXPECT_FALSE(build_done_at_point.load()) << "the rebuild never yielded";
    ASSERT_EQ(build.outs[0].response.rfind("REINDEXED INDEX", 0), 0u) << build.outs[0].response;
    auto new_ix = d().catalog().FindIndexByName("tix");
    ASSERT_TRUE(new_ix.ok());
    EXPECT_NE(new_ix.value().index_oid, old_ix.value().index_oid);
    EXPECT_EQ(Entries("tix"), std::to_string(kRows));
}

TEST_F(IndexBuildSliceRigTest, ACancelledReindexLeavesTheOldIndexAsItWas) {
    // The rebuild delete-marks the old row in its head, before the first
    // yield; the cancel's abort compensates the mark, so the old index is
    // the relation's again, under its old oid, and a probe still uses it.
    ASSERT_EQ(d().Dispatch("CREATE INDEX tix ON t (v)").response.rfind("CREATED INDEX", 0), 0u);
    auto old_ix = d().catalog().FindIndexByName("tix");
    ASSERT_TRUE(old_ix.ok());
    Script build{&build_, {"REINDEX INDEX tix"}};
    Script canceller{&other_, {}};
    std::atomic<bool> build_running{false};
    canceller.after = [&] {
        build_running.store(build.done.load() == 0);
        build_.RequestCancel();
    };
    RunBoth(build, canceller);

    ASSERT_TRUE(build_running.load()) << "the rebuild ended before the cancel: nothing yielded";
    EXPECT_EQ(build.outs[0].status.code(), StatusCode::kCancelled) << build.outs[0].response;
    auto kept = d().catalog().FindIndexByName("tix");
    ASSERT_TRUE(kept.ok()) << "the cancelled rebuild left the relation without its index";
    EXPECT_EQ(kept.value().index_oid, old_ix.value().index_oid);
    EXPECT_EQ(Entries("tix"), std::to_string(kRows));
    const std::string plan = d().Dispatch("ANALYZE SELECT id FROM t WHERE v = 777").response;
    EXPECT_NE(plan.find("IndexProbe"), std::string::npos) << plan;
}

TEST_F(IndexBuildSliceRigTest, ACoreTornDownWithABuildBetweenSlicesAbortsIt) {
    // A peer's build, run by hand to its first boundary and left queued, then
    // its frame destroyed as a core's teardown destroys it
    // (`CoreRuntime`'s `DiscardTasks`). The abort is what a frame destroyed
    // with nobody left to decide owes: the `X` released, the registration
    // withdrawn, nothing published.
    //
    // **Mutation**, measured: `~IndexBuildRun` aborting nothing - killed 1
    // in 1.
    CommandDispatcher& peer = rig_->core(1).dispatcher();
    Script build{&build_, {"CREATE INDEX tix ON t (v)"}};
    rig_->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(peer, build)));
    {
        // `RunOnce` declares its core on the calling thread and leaves it
        // declared; this thread is the test's, and goes on as core 0.
        const CurrentCoreGuard as_test_thread(CurrentCore());
        for (int i = 0; i < 4; ++i) (void)rig_->core(1).scheduler().RunOnce();
        ASSERT_EQ(build.done.load(), 0u) << "the build finished before the teardown";
        ASSERT_EQ(rig_->locks().progress().Registered(), 1u)
            << "the build is not between slices";
        // As `~CoreRuntime` runs it: as the core whose tasks they are.
        const CurrentCoreGuard as_core_1(1);
        rig_->core(1).scheduler().DiscardTasks();
    }

    EXPECT_EQ(build.done.load(), 0u);
    EXPECT_EQ(rig_->locks().progress().Registered(), 0u);
    EXPECT_FALSE(d().catalog().FindIndexByName("tix").ok());
    EXPECT_EQ(
        d().Dispatch("INSERT INTO t VALUES (99999, 99999, 'y')").response.rfind("INSERTED", 0),
        0u)
        << "the relation's X outlived the build's frame";
    EXPECT_EQ(d().Dispatch("CREATE INDEX tix ON t (v)").response.rfind("CREATED INDEX", 0), 0u);
}

// ---- `CREATE ASSERTION` and `CREATE TRANSITION` (A1 stage 2b) ----------------

constexpr const char* kCap = "CREATE ASSERTION cap ON t GROUP BY (v) CHECK COUNT(*) <= 1";
constexpr const char* kStates =
    "CREATE TRANSITION states ON t(name) ON INSERT ('x') ON UPDATE ('x' => 'y')";

TEST_F(IndexBuildSliceRigTest, AnAssertionBuildYieldsAndAPeerWriterMeetsItOnceItIsPublished) {
    // A point read of another relation is answered inside the build, and a
    // writer of the built relation on the build's core parks on the `X` and
    // meets the published assertion.
    //
    // **Mutation**, measured: `BuildRun::HandOn` running the walk whole where
    // it may park - killed 1 in 1.
    Script build{&build_, {kCap}};
    Script other{&other_, {"SELECT id FROM u WHERE id = 5", "INSERT INTO t VALUES (99999, 5, 'x')"}};
    std::atomic<bool> build_running_at_other{false};
    other.before = [&] { build_running_at_other.store(build.done.load() == 0); };
    RunBoth(build, other);

    ASSERT_TRUE(build_running_at_other.load()) << "the build never yielded";
    ASSERT_EQ(build.outs[0].response.rfind("CREATED ASSERTION", 0), 0u) << build.outs[0].response;
    EXPECT_NE(build.outs[0].response.find("rows=" + std::to_string(kRows)), std::string::npos)
        << build.outs[0].response;
    EXPECT_NE(other.outs[0].response.find("5"), std::string::npos) << other.outs[0].response;
    EXPECT_NE(other.outs[1].response.find("ASSERTION_VIOLATION"), std::string::npos)
        << "the writer ran past the build's X: " << other.outs[1].response;
}

TEST_F(IndexBuildSliceRigTest, AnAssertionsViolationInALaterSliceIsRefusedAndAdoptsNothing) {
    // The bound is broken only by the last row, slices after the first.
    ASSERT_EQ(d().Dispatch("INSERT INTO t VALUES (99999, 7, 'x')").response.rfind("INSERTED", 0),
              0u);
    Script build{&build_, {kCap}};
    Script point{&other_, {"SELECT id FROM u WHERE id = 5"}};
    std::atomic<bool> build_running_at_point{false};
    point.before = [&] { build_running_at_point.store(build.done.load() == 0); };
    RunBoth(build, point);

    ASSERT_TRUE(build_running_at_point.load()) << "the build never yielded";
    EXPECT_EQ(build.outs[0].status.code(), StatusCode::kAssertionViolation)
        << build.outs[0].response;
    EXPECT_EQ(d().Dispatch("SELECT name FROM sys.assertions").response.find("cap"),
              std::string::npos);
    EXPECT_EQ(d().Dispatch("INSERT INTO t VALUES (99998, 7, 'x')").response.rfind("INSERTED", 0),
              0u)
        << "a refused build left the relation fenced or enforced";
}

TEST_F(IndexBuildSliceRigTest, AnAssertionNameTakenWhileTheBuildYieldedIsRefusedBeforeAdoption) {
    // Another session takes the name on another relation inside the build;
    // the build's publish asks again and refuses with the head's message.
    //
    // **Mutation**, measured: `AssertionBuild::Finish` not asking the name
    // again - killed 1 in 1 (the loser was adopted, then evicted).
    ASSERT_EQ(d().Dispatch("CREATE TABLE u2 (id int64, k int64)").response.rfind("CREATED", 0),
              0u);
    ASSERT_EQ(d().Dispatch("INSERT INTO u2 VALUES (1, 5)").response.rfind("INSERTED", 0), 0u);
    int published = 0;
    d().SetAfterAssertionPublishRunForTest([&] { ++published; });
    Script build{&build_, {kCap}};
    Script other{&other_, {"CREATE ASSERTION cap ON u2 GROUP BY (k) CHECK COUNT(*) <= 1"}};
    std::atomic<bool> build_running_at_other{false};
    other.before = [&] { build_running_at_other.store(build.done.load() == 0); };
    RunBoth(build, other);
    d().SetAfterAssertionPublishRunForTest(nullptr);
    EXPECT_EQ(published, 1) << "the loser was adopted before its name was asked again";

    ASSERT_TRUE(build_running_at_other.load()) << "the build never yielded";
    ASSERT_EQ(other.outs[0].response.rfind("CREATED ASSERTION", 0), 0u) << other.outs[0].response;
    EXPECT_EQ(build.outs[0].status.code(), StatusCode::kAlreadyExists) << build.outs[0].response;
    EXPECT_NE(build.outs[0].response.find("assertion \"cap\" already exists"), std::string::npos)
        << build.outs[0].response;
    // The relation the loser built on is unenforced and free; the winner
    // still enforces.
    EXPECT_EQ(d().Dispatch("INSERT INTO t VALUES (99999, 5, 'x')").response.rfind("INSERTED", 0),
              0u);
    EXPECT_NE(d().Dispatch("INSERT INTO u2 VALUES (2, 5)").response.find("ASSERTION_VIOLATION"),
              std::string::npos);
}

TEST_F(IndexBuildSliceRigTest, ACancelEndsAnAssertionBuildWithNothingPublished) {
    // **Mutation**, measured: the boundary taking no cancel - killed 1 in 1.
    Script build{&build_, {kCap}};
    Script canceller{&other_, {}};
    std::atomic<bool> build_running{false};
    canceller.after = [&] {
        build_running.store(build.done.load() == 0);
        build_.RequestCancel();
    };
    RunBoth(build, canceller);

    ASSERT_TRUE(build_running.load()) << "the build ended before the cancel: nothing yielded";
    EXPECT_EQ(build.outs[0].status.code(), StatusCode::kCancelled) << build.outs[0].response;
    EXPECT_FALSE(build_.TakeCancel()) << "one cancel, one statement";
    EXPECT_EQ(d().Dispatch("SELECT name FROM sys.assertions").response.find("cap"),
              std::string::npos);
    EXPECT_EQ(rig_->locks().progress().Registered(), 0u);
    EXPECT_EQ(d().Dispatch("INSERT INTO t VALUES (99999, 5, 'x')").response.rfind("INSERTED", 0),
              0u)
        << "the X outlived the cancelled build";
    EXPECT_EQ(d().Dispatch("DELETE FROM t WHERE id = 99999").response.rfind("ERR", 0),
              std::string::npos);
    EXPECT_EQ(d().Dispatch(kCap).response.rfind("CREATED ASSERTION", 0), 0u);
}

TEST_F(IndexBuildSliceRigTest, ACancelledDeclarationBuildFailsTheExplicitTransaction) {
    // The operator's ruling of 2026-10-21: a cancel leaves an open
    // transaction failed, though a declaration's own refusals poison nothing.
    //
    // **Mutation**, measured: the tail frame not poisoning on a cancel -
    // killed 1 in 1.
    Script build{&build_, {"BEGIN", kStates}};
    Script canceller{&other_, {}};
    std::atomic<bool> build_running{false};
    canceller.after = [&] {
        build_running.store(build.done.load() == 1);
        build_.RequestCancel();
    };
    RunBoth(build, canceller);

    ASSERT_TRUE(build_running.load()) << "the build ended before the cancel: nothing yielded";
    EXPECT_EQ(build.outs[1].status.code(), StatusCode::kCancelled) << build.outs[1].response;
    EXPECT_TRUE(build_.failed()) << "a cancelled statement leaves its transaction failed";
    EXPECT_EQ(d().Dispatch("ROLLBACK", &build_).response.rfind("ROLLBACK", 0), 0u);
    EXPECT_EQ(d().Dispatch("SELECT name FROM sys.transitions").response.find("states"),
              std::string::npos);
}

TEST_F(IndexBuildSliceRigTest, ATransitionBuildYieldsAndAPeerWriterMeetsItOnceItIsPublished) {
    Script build{&build_, {kStates}};
    Script other{&other_,
                 {"SELECT id FROM u WHERE id = 5", "INSERT INTO t VALUES (99999, 5, 'z')"}};
    std::atomic<bool> build_running_at_other{false};
    other.before = [&] { build_running_at_other.store(build.done.load() == 0); };
    RunBoth(build, other);

    ASSERT_TRUE(build_running_at_other.load()) << "the build never yielded";
    ASSERT_EQ(build.outs[0].response.rfind("CREATED TRANSITION", 0), 0u) << build.outs[0].response;
    EXPECT_NE(build.outs[0].response.find("rows=" + std::to_string(kRows)), std::string::npos)
        << build.outs[0].response;
    EXPECT_NE(other.outs[1].response.find("TRANSITION_VIOLATION"), std::string::npos)
        << "the writer ran past the build's X: " << other.outs[1].response;
}

TEST_F(IndexBuildSliceRigTest, ACoreTornDownWithADeclarationBuildBetweenSlicesReleasesIt) {
    // An assertion's build and a transition's, each run by hand on core 1 to
    // its first boundary, then its frame destroyed as a core's teardown
    // destroys it. The `X` is released, the registration withdrawn, nothing
    // published, and the same declaration is accepted afterwards. The
    // destructor bodies' own releases repeat what the members' destructors
    // do; what they add is the core identity, which no check here observes.
    CommandDispatcher& peer = rig_->core(1).dispatcher();
    int id = 99990;
    for (const char* sql : {kCap, kStates}) {
        Script build{&build_, {sql}};
        rig_->core(1).scheduler().Submit(
            sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(peer, build)));
        {
            const CurrentCoreGuard as_test_thread(CurrentCore());
            for (int i = 0; i < 4; ++i) (void)rig_->core(1).scheduler().RunOnce();
            ASSERT_EQ(build.done.load(), 0u) << sql << ": finished before the teardown";
            ASSERT_EQ(rig_->locks().progress().Registered(), 1u) << sql << ": not between slices";
            const CurrentCoreGuard as_core_1(1);
            rig_->core(1).scheduler().DiscardTasks();
        }
        EXPECT_EQ(rig_->locks().progress().Registered(), 0u) << sql;
        ++id;
        const std::string row = "(" + std::to_string(id) + ", " + std::to_string(id) + ", 'x')";
        EXPECT_EQ(d().Dispatch("INSERT INTO t VALUES " + row).response.rfind("INSERTED", 0), 0u)
            << sql << ": the relation's X outlived the build's frame";
    }
    EXPECT_EQ(d().Dispatch("SELECT name FROM sys.assertions").response.find("cap"),
              std::string::npos);
    EXPECT_EQ(d().Dispatch("SELECT name FROM sys.transitions").response.find("states"),
              std::string::npos);
    EXPECT_EQ(d().Dispatch(kCap).response.rfind("CREATED ASSERTION", 0), 0u);
    EXPECT_EQ(d().Dispatch(kStates).response.rfind("CREATED TRANSITION", 0), 0u);
}

TEST_F(IndexBuildSliceRigTest, ATransitionsViolationInALaterSliceIsRefusedAndPoisonsNothing) {
    // The last row breaks the state set, slices after the first; inside an
    // explicit transaction the refusal leaves it as it was - a declaration's
    // own refusals poison nothing.
    ASSERT_EQ(d().Dispatch("UPDATE t SET name = 'q' WHERE id = " + std::to_string(kRows))
                  .response.rfind("ERR", 0),
              std::string::npos);
    Script build{&build_, {"BEGIN", kStates}};
    Script point{&other_, {"SELECT id FROM u WHERE id = 5"}};
    std::atomic<bool> build_running_at_point{false};
    point.before = [&] { build_running_at_point.store(build.done.load() == 1); };
    RunBoth(build, point);

    ASSERT_TRUE(build_running_at_point.load()) << "the build never yielded";
    EXPECT_NE(build.outs[1].response.find("TRANSITION_VIOLATION"), std::string::npos)
        << build.outs[1].response;
    EXPECT_FALSE(build_.failed()) << "a refused declaration poisoned the transaction";
    EXPECT_EQ(d().Dispatch("COMMIT", &build_).response.rfind("COMMIT", 0), 0u);
    EXPECT_EQ(d().Dispatch("SELECT name FROM sys.transitions").response.find("states"),
              std::string::npos);
    EXPECT_EQ(d().Dispatch("INSERT INTO t VALUES (99999, 5, 'z')").response.rfind("INSERTED", 0),
              0u);
}

TEST_F(IndexBuildSliceRigTest, ATransitionNameTakenWhileTheBuildYieldedIsRefusedWithItsByte) {
    // **Mutation**, measured: `TransitionBuild::Finish` not asking the name
    // again - killed 1 in 1 (the catalog's own guard refuses with no byte).
    ASSERT_EQ(d().Dispatch("CREATE TABLE w (id int64, s varchar(32))").response.rfind("CREATED", 0),
              0u);
    Script build{&build_, {kStates}};
    Script other{&other_, {"CREATE TRANSITION states ON w(s) ON INSERT ('x') ON UPDATE ('x' => 'y')"}};
    std::atomic<bool> build_running_at_other{false};
    other.before = [&] { build_running_at_other.store(build.done.load() == 0); };
    RunBoth(build, other);

    ASSERT_TRUE(build_running_at_other.load()) << "the build never yielded";
    ASSERT_EQ(other.outs[0].response.rfind("CREATED TRANSITION", 0), 0u) << other.outs[0].response;
    EXPECT_EQ(build.outs[0].status.code(), StatusCode::kAlreadyExists) << build.outs[0].response;
    EXPECT_NE(build.outs[0].response.find("(byte "), std::string::npos) << build.outs[0].response;
    EXPECT_EQ(d().Dispatch("INSERT INTO t VALUES (99999, 5, 'z')").response.rfind("INSERTED", 0),
              0u);
}

TEST_F(IndexBuildSliceRigTest, ACancelEndsATransitionBuildInAutocommitWithNothingPublished) {
    Script build{&build_, {kStates}};
    Script canceller{&other_, {}};
    std::atomic<bool> build_running{false};
    canceller.after = [&] {
        build_running.store(build.done.load() == 0);
        build_.RequestCancel();
    };
    RunBoth(build, canceller);

    ASSERT_TRUE(build_running.load()) << "the build ended before the cancel: nothing yielded";
    EXPECT_EQ(build.outs[0].status.code(), StatusCode::kCancelled) << build.outs[0].response;
    EXPECT_FALSE(build_.failed());
    EXPECT_EQ(rig_->locks().progress().Registered(), 0u);
    EXPECT_EQ(d().Dispatch("INSERT INTO t VALUES (99999, 5, 'z')").response.rfind("INSERTED", 0),
              0u);
    EXPECT_EQ(d().Dispatch("DELETE FROM t WHERE id = 99999").response.rfind("ERR", 0),
              std::string::npos);
    EXPECT_EQ(d().Dispatch(kStates).response.rfind("CREATED TRANSITION", 0), 0u);
}

TEST_F(IndexBuildSliceRigTest, ACancelPendingAtTheHeadEndsTheBuildWithNothingPublished) {
    // The cancel arrives before the statement: the head runs, and the build
    // ends at once rather than after a slice of work (`BuildRun::Execute`
    // takes it before the first).
    Script build{&build_, {kCap}};
    build.before = [&] { build_.RequestCancel(); };
    Script point{&other_, {"SELECT id FROM u WHERE id = 5"}};
    RunBoth(build, point);

    EXPECT_EQ(build.outs[0].status.code(), StatusCode::kCancelled) << build.outs[0].response;
    EXPECT_EQ(d().Dispatch("SELECT name FROM sys.assertions").response.find("cap"),
              std::string::npos);
    EXPECT_EQ(rig_->locks().progress().Registered(), 0u);
    EXPECT_EQ(d().Dispatch(kCap).response.rfind("CREATED ASSERTION", 0), 0u);
}

TEST_F(IndexBuildSliceRigTest, WithoutALockTableABuildRunsWholeInItsHead) {
    // Nothing holds the relation `X`, so nothing keeps the leaf a walk would
    // resume at in place: `BuildRun::HandOn` runs it whole.
    //
    // **Mutation**, measured: `HandOn` handing on whether or not the `X` is
    // held - killed 1 in 1.
    d().set_locks(nullptr);
    // All three in one script: a build that never suspends lets the script
    // run on to its next line, so the point read runs after the last.
    Script build{&build_, {"CREATE INDEX tix ON t (v)", kCap, kStates}};
    Script point{&other_, {"SELECT id FROM u WHERE id = 5"}};
    std::atomic<std::size_t> builds_done_at_point{0};
    point.before = [&] { builds_done_at_point.store(build.done.load()); };
    RunBoth(build, point);
    EXPECT_EQ(builds_done_at_point.load(), 3u) << "a build yielded with no X held";
    for (const DispatchOutcome& out : build.outs) {
        EXPECT_EQ(out.response.rfind("CREATED", 0), 0u) << out.response;
    }
    d().set_locks(&rig_->locks());
}

TEST(IndexBuildSliceCrashTest, AnUncommittedBuildsRowIsRolledBackByTheMount) {
    // **The tail installs the undo hook again** (`IndexBuildRun::FramedTail`).
    // `InDdlStatement` uninstalls it before the first yield, and another
    // session's DDL on the same core installs and uninstalls its own inside
    // the build. The row the tail writes must carry its undo record, or the
    // mount's redo puts back an index whose transaction never committed: a
    // loser's row with no record for the undo phase to retire it by.
    //
    // **Mutation**, measured: the tail not installing the hook - killed 1
    // in 1.
    crash_rig::TempDir snap;
    std::unique_ptr<TwoCoreRig> rig = crash_rig::OpenFileRig();
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d = rig->core(0).dispatcher();
    crash_rig::Ok(d, "CREATE TABLE t (id int64, v int64, name varchar(2000))");
    for (int i = 1; i <= kRows;) {
        std::string sql = "INSERT INTO t VALUES ";
        for (int k = 0; k < 100 && i <= kRows; ++k, ++i) {
            if (k != 0) sql += ", ";
            sql += "(" + std::to_string(i) + ", " + std::to_string(i) + ", 'x')";
        }
        crash_rig::Ok(d, sql);
    }

    Session builder, other;
    Script build{&builder, {"BEGIN", "CREATE INDEX tix ON t (v)"}};
    Script ddl{&other, {"CREATE TABLE w (id int64)"}};
    std::atomic<bool> build_running_at_ddl{false};
    ddl.before = [&] { build_running_at_ddl.store(build.done.load() == 1); };
    rig->core(0).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(d, build)));
    rig->core(0).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(d, ddl)));
    rig->Start();
    ASSERT_TRUE(Within(60000ms, [&] {
        return build.done.load() == build.lines.size() && ddl.done.load() == ddl.lines.size();
    })) << "the scripts never finished";
    rig->Stop();
    ASSERT_TRUE(build_running_at_ddl.load()) << "the DDL ran after the build: nothing yielded";
    ASSERT_EQ(build.outs[1].response.rfind("CREATED INDEX", 0), 0u) << build.outs[1].response;
    ASSERT_EQ(ddl.outs[0].response.rfind("CREATED", 0), 0u) << ddl.outs[0].response;

    // The crash, with the build's transaction still open.
    ASSERT_TRUE(rig->Snapshot(snap.path).ok());
    auto mounted = crash_rig::Mount(snap.path);
    ASSERT_TRUE(mounted.ok()) << "the mount refused the crash: " << mounted.status().message();
    CommandDispatcher& after = mounted.value()->dispatcher();
    EXPECT_EQ(after.Dispatch("SELECT name FROM sys.indexes").response.find("tix"),
              std::string::npos)
        << "the mount brought back an uncommitted build's index";
    EXPECT_TRUE(after.catalog().FindTableOidByName(catalog::kNamespacePublic, "w").ok())
        << "the committed DDL inside the build was lost";
}

TEST(IndexBuildSliceCrashTest, AnAssertionBuildCrashedBetweenSlicesMountsWithoutIt) {
    // An assertion's build on core 1, run by hand past its first slice -
    // `ASSERT_BUILD` records in the log, no `sys.assertions` row - and the
    // crash taken there. The mount skips the records of an id no row names,
    // and the same declaration is accepted afterwards.
    crash_rig::TempDir snap;
    std::unique_ptr<TwoCoreRig> rig = crash_rig::OpenFileRig();
    ASSERT_NE(rig, nullptr);
    CommandDispatcher& d = rig->core(0).dispatcher();
    crash_rig::Ok(d, "CREATE TABLE t (id int64, v int64, name varchar(2000))");
    for (int i = 1; i <= kRows;) {
        std::string sql = "INSERT INTO t VALUES ";
        for (int k = 0; k < 100 && i <= kRows; ++k, ++i) {
            if (k != 0) sql += ", ";
            sql += "(" + std::to_string(i) + ", " + std::to_string(i) + ", 'x')";
        }
        crash_rig::Ok(d, sql);
    }

    Session builder;
    Script build{&builder, {kCap}};
    rig->core(1).scheduler().Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground, RunScript(rig->core(1).dispatcher(), build)));
    {
        const CurrentCoreGuard as_test_thread(CurrentCore());
        for (int i = 0; i < 4; ++i) (void)rig->core(1).scheduler().RunOnce();
    }
    ASSERT_EQ(build.done.load(), 0u) << "the build finished before the crash";
    ASSERT_EQ(rig->locks().progress().Registered(), 1u) << "the build is not between slices";
    ASSERT_TRUE(rig->Snapshot(snap.path).ok());

    auto mounted = crash_rig::Mount(snap.path);
    ASSERT_TRUE(mounted.ok()) << "the mount refused the crash: " << mounted.status().message();
    CommandDispatcher& after = mounted.value()->dispatcher();
    EXPECT_EQ(after.Dispatch("SELECT name FROM sys.assertions").response.find("cap"),
              std::string::npos);
    const std::string recreated = after.Dispatch(kCap).response;
    EXPECT_EQ(recreated.rfind("CREATED ASSERTION", 0), 0u) << recreated;
    EXPECT_NE(recreated.find("rows=" + std::to_string(kRows)), std::string::npos) << recreated;
    // The orphaned records counted nothing into the new cabin.
    EXPECT_NE(after.Dispatch("INSERT INTO t VALUES (99999, 5, 'x')").response.find(
                  "ASSERTION_VIOLATION"),
              std::string::npos);
    EXPECT_EQ(after.Dispatch("INSERT INTO t VALUES (99998, 99998, 'x')").response.rfind(
                  "INSERTED", 0),
              0u);
    // The rig is torn down with the build still queued on core 1.
    const CurrentCoreGuard as_core_1(1);
    rig.reset();
}

}  // namespace
}  // namespace kds::server
