// **A statement yields at its walk boundary** (BA-S15, BA-R12, BA-Q11,
// `instructions/v3.0.0/workorder-ba-parallelism.md`).
//
// A `SELECT`'s outermost walk gives its core back every
// `SlicePolicy::pages_per_slice` pages, so a second statement on the same
// core runs inside the first one's walk. Each cell below puts that second
// statement there - one core, both tasks submitted before the reactor starts,
// the long walk first - and asks what the walk held across the gap: the
// point read is answered before the walk ends; a schema-word move frees no
// memo the walk bound and the core's epoch stays at the walk's word; a split
// leaves the walk missing and repeating nothing; a cancel ends it. The
// registry and the two bookkeeping halves have cells of their own.

#include "two_core_rig.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/catalog/catalog_cache.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/cancel_registry.hpp"
#include "kds/server/session.hpp"
#include "kds/server/statement_epoch.hpp"
#include "kds/wal/durability.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

// A `varchar(2000)` cell makes a row ~2 KB, so a leaf holds about four and
// `kRows` rows are ~`kRows / 4` leaves - several slices of 64 pages.
constexpr int kRows = 1600;

struct Script {
    Session* session = nullptr;
    std::vector<std::string> lines;
    std::vector<DispatchOutcome> outs;
    std::atomic<std::size_t> done{0};
    // Run inside the script, on the core's thread, after its last line.
    std::function<void()> after;
};

sched::Coro RunScript(CommandDispatcher& d, Script& s) {
    s.outs.resize(s.lines.size());
    for (std::size_t i = 0; i < s.lines.size(); ++i) {
        co_await d.DispatchAsync(s.lines[i], s.session, &s.outs[i]);
        s.done.store(i + 1, std::memory_order_release);
    }
    if (s.after) s.after();
    co_return Status::OK();
}

// The ids a `SELECT id ...` reply carries, from the newline protocol's
// rendering: a header line, then one escaped section per row.
std::vector<std::int64_t> Ids(const std::string& reply) {
    std::vector<std::int64_t> ids;
    std::size_t at = reply.find("\\n");
    while (at != std::string::npos) {
        at += 2;
        const std::size_t end = reply.find("\\n", at);
        const std::string cell = reply.substr(at, end == std::string::npos ? std::string::npos
                                                                            : end - at);
        if (!cell.empty()) ids.push_back(std::stoll(cell));
        at = end;
    }
    return ids;
}

class WalkSliceRigTest : public ::testing::Test {
protected:
    void SetUp() override {
        TwoCoreRig::Options options;
        options.max_idle_block_ms = 20;
        auto opened = TwoCoreRig::Open(options);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        rig_ = std::move(opened.value());
        ASSERT_EQ(d().Dispatch("CREATE TABLE t (id int64, name varchar(2000))").response.rfind(
                      "CREATED", 0),
                  0u);
        for (Session* s : {&long_, &other_}) s->set_durability(wal::DurabilityClass::kRelaxed);
    }
    void TearDown() override {
        if (rig_ != nullptr) rig_->Stop();
    }

    CommandDispatcher& d() { return rig_->core(0).dispatcher(); }

    // `ids` named, a hundred rows a statement.
    void Load(int first, int step, int count) {
        for (int i = 0; i < count;) {
            std::string sql = "INSERT INTO t VALUES ";
            for (int k = 0; k < 100 && i < count; ++k, ++i) {
                if (k != 0) sql += ", ";
                sql += "(" + std::to_string(first + i * step) + ", 'x')";
            }
            const std::string reply = d().Dispatch(sql).response;
            ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
        }
    }

    // Both scripts on core 0, the long one first: it runs to its first
    // slice boundary, and `other` runs there.
    void RunBoth(Script& walk, Script& other) {
        rig_->core(0).scheduler().Submit(
            sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(d(), walk)));
        rig_->core(0).scheduler().Submit(
            sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(d(), other)));
        rig_->Start();
        ASSERT_TRUE(Within(60000ms, [&] {
            return walk.done.load() == walk.lines.size() && other.done.load() == other.lines.size();
        })) << "the scripts never finished";
    }

    std::unique_ptr<TwoCoreRig> rig_;
    Session long_, other_;
};

TEST_F(WalkSliceRigTest, APointReadSharingACoreWithALongWalkIsAnsweredInsideIt) {
    Load(1, 1, kRows);
    Script walk{&long_, {"SELECT id FROM t WHERE name = 'none'"}};
    Script point{&other_, {"SELECT id FROM t WHERE id = 5"}};
    std::atomic<bool> walk_done_at_point{true};
    point.after = [&] { walk_done_at_point.store(walk.done.load() == 1); };
    RunBoth(walk, point);

    EXPECT_FALSE(walk_done_at_point.load())
        << "the point read waited for the whole walk: nothing yielded";
    EXPECT_EQ(Ids(point.outs[0].response), std::vector<std::int64_t>{5});
    EXPECT_TRUE(walk.outs[0].status.ok()) << walk.outs[0].response;
    EXPECT_TRUE(Ids(walk.outs[0].response).empty());
}

TEST_F(WalkSliceRigTest, ASchemaWordMoveDuringASuspensionFreesNoMemoTheWalkHolds) {
    Load(1, 1, kRows);
    StatementEpochs epochs(2);
    d().SetStatementEpochs(&epochs);
    catalog::Catalog& catalog = rig_->core(0).dispatcher().catalog();

    Script walk{&long_, {"SELECT id FROM t"}};
    // A DDL moves the schema word; the next statement's head revalidates,
    // which frees the memo the suspended walk is bound into - or, pinned,
    // retires it.
    Script other{&other_, {"CREATE TABLE u (id int64)", "SELECT id FROM t WHERE id = 1"}};
    std::atomic<bool> walk_running{false};
    std::atomic<std::size_t> retired{0};
    std::atomic<bool> epoch_held{false};
    other.after = [&] {
        walk_running.store(walk.done.load() == 0);
        retired.store(catalog.cache_retired_generations());
        // The walk's word is below the word the DDL published, so the
        // core's slot is too: a drop's reclaim waits for the walk.
        epoch_held.store(!epochs.AllAtLeast(catalog.schema_word_now()));
    };
    RunBoth(walk, other);

    ASSERT_TRUE(walk_running.load()) << "the walk ended before the DDL ran: nothing yielded";
    EXPECT_GT(retired.load(), 0u) << "the revalidation freed the memo under the walk";
    EXPECT_TRUE(epoch_held.load()) << "the core published a word above the suspended walk's";
    ASSERT_TRUE(walk.outs[0].status.ok()) << walk.outs[0].response;
    EXPECT_EQ(Ids(walk.outs[0].response).size(), static_cast<std::size_t>(kRows));
    // Given back with the walk.
    EXPECT_EQ(catalog.cache_retired_generations(), 0u);
    EXPECT_TRUE(epochs.AllAtLeast(catalog.schema_word_now()));
    d().SetStatementEpochs(nullptr);
}

TEST_F(WalkSliceRigTest, ASplitBetweenTwoSlicesMissesAndRepeatsNoRow) {
    // Even ids; the odd ones arrive inside the walk's first suspension and
    // split leaves behind it, at it and ahead of it.
    Load(2, 2, kRows);
    Script walk{&long_, {"SELECT id FROM t"}};
    Script writer{&other_, {}};
    for (int i = 0; i < kRows; ++i) {
        writer.lines.push_back("INSERT INTO t VALUES (" + std::to_string(2 * i + 1) + ", 'x')");
    }
    std::atomic<bool> walk_running{false};
    writer.after = [&] { walk_running.store(walk.done.load() == 0); };
    RunBoth(walk, writer);

    ASSERT_TRUE(walk_running.load()) << "the walk ended before the splits: nothing yielded";
    for (const DispatchOutcome& out : writer.outs) {
        ASSERT_EQ(out.response.rfind("ERR", 0), std::string::npos) << out.response;
    }
    ASSERT_TRUE(walk.outs[0].status.ok()) << walk.outs[0].response;
    std::vector<std::int64_t> ids = Ids(walk.outs[0].response);
    std::sort(ids.begin(), ids.end());
    std::vector<std::int64_t> expected;
    for (int i = 1; i <= kRows; ++i) expected.push_back(2 * i);
    EXPECT_EQ(ids, expected) << "the walk read under its own view and walked every row once";
}

// ---- The reverse walk (CV O2, CV-R3, CV-R4) --------------------------------
//
// `ORDER BY id DESC LIMIT <more than the relation>` is elided onto a walk
// from the high end down, which yields where the forward walk yields and
// steps left by a seek from the last leaf's `min_key`.

int Height(CommandDispatcher& d) {
    const std::vector<std::int64_t> h =
        Ids(d.Dispatch("SELECT height FROM sys.relation_stats WHERE name = 't'").response);
    return h.empty() ? 0 : static_cast<int>(h.front());
}

TEST_F(WalkSliceRigTest, AReverseWalkYieldsAndAPointReadIsAnsweredInsideIt) {
    Load(1, 1, kRows);
    const std::string reverse = "SELECT id FROM t ORDER BY id DESC LIMIT 100000";
    ASSERT_NE(d().Dispatch("ANALYZE " + reverse).response.find("order=tuple_key_desc"),
              std::string::npos);
    Script walk{&long_, {reverse}};
    Script point{&other_, {"SELECT id FROM t WHERE id = 5"}};
    std::atomic<bool> walk_done_at_point{true};
    point.after = [&] { walk_done_at_point.store(walk.done.load() == 1); };
    RunBoth(walk, point);

    EXPECT_FALSE(walk_done_at_point.load())
        << "the point read waited for the whole reverse walk: nothing yielded";
    EXPECT_EQ(Ids(point.outs[0].response), std::vector<std::int64_t>{5});
    ASSERT_TRUE(walk.outs[0].status.ok()) << walk.outs[0].response;
    std::vector<std::int64_t> expected;
    for (int i = kRows; i >= 1; --i) expected.push_back(i);
    EXPECT_EQ(Ids(walk.outs[0].response), expected);
}

TEST_F(WalkSliceRigTest, SplitsAndARootGrowthBehindAReverseWalkMissAndRepeatNoRow) {
    // Even ids; the odd ones arrive inside the walk's suspensions, splitting
    // leaves at, behind and ahead of the walk's position, and enough of them
    // that the tree grows a level.
    Load(2, 2, kRows);
    const int height_before = Height(d());
    Script walk{&long_, {"SELECT id FROM t ORDER BY id DESC LIMIT 100000"}};
    Script writer{&other_, {}};
    for (int i = 0; i < 2 * kRows; ++i) {
        writer.lines.push_back("INSERT INTO t VALUES (" + std::to_string(2 * i + 1) + ", 'x')");
    }
    std::atomic<bool> walk_running{false};
    writer.after = [&] { walk_running.store(walk.done.load() == 0); };
    RunBoth(walk, writer);

    ASSERT_TRUE(walk_running.load()) << "the walk ended before the splits: nothing yielded";
    for (const DispatchOutcome& out : writer.outs) {
        ASSERT_EQ(out.response.rfind("ERR", 0), std::string::npos) << out.response;
    }
    EXPECT_GT(Height(d()), height_before) << "no root growth happened under the walk";
    ASSERT_TRUE(walk.outs[0].status.ok()) << walk.outs[0].response;
    std::vector<std::int64_t> expected;
    for (int i = kRows; i >= 1; --i) expected.push_back(2 * i);
    EXPECT_EQ(Ids(walk.outs[0].response), expected)
        << "the reverse walk read under its own view, in order, every row once";
}

TEST_F(WalkSliceRigTest, UpdatesAndDeletesInsideAReverseWalkLeaveItsViewWhole) {
    // Rows the walk has not reached yet are updated and deleted inside its
    // suspensions, so the walk reads them through their undo under a held
    // leaf: it still returns every row, in order, as its view saw them.
    Load(1, 1, kRows);
    Script walk{&long_, {"SELECT id FROM t WHERE name = 'x' ORDER BY id DESC LIMIT 100000"}};
    Script writer{&other_,
                  {"UPDATE t SET name = 'y' WHERE id BETWEEN 1 AND 400",
                   "DELETE FROM t WHERE id BETWEEN 401 AND 800",
                   "UPDATE t SET name = 'z' WHERE id BETWEEN 1500 AND 1600"}};
    std::atomic<bool> walk_running{false};
    writer.after = [&] { walk_running.store(walk.done.load() == 0); };
    RunBoth(walk, writer);

    ASSERT_TRUE(walk_running.load()) << "the walk ended before the writes: nothing yielded";
    for (const DispatchOutcome& out : writer.outs) {
        ASSERT_EQ(out.response.rfind("ERR", 0), std::string::npos) << out.response;
    }
    ASSERT_TRUE(walk.outs[0].status.ok()) << walk.outs[0].response;
    std::vector<std::int64_t> expected;
    for (int i = kRows; i >= 1; --i) expected.push_back(i);
    EXPECT_EQ(Ids(walk.outs[0].response), expected);
}

TEST_F(WalkSliceRigTest, PurgedSlotsAreNoRowsToAReverseWalk) {
    Load(1, 1, 400);
    ASSERT_EQ(d().Dispatch("DELETE FROM t WHERE id BETWEEN 100 AND 299").response.rfind("ERR", 0),
              std::string::npos);
    const std::string purged = d().Dispatch("PURGE FROM t WHERE id BETWEEN 100 AND 299").response;
    ASSERT_EQ(purged.rfind("ERR", 0), std::string::npos) << purged;
    std::vector<std::int64_t> expected;
    for (int i = 400; i >= 300; --i) expected.push_back(i);
    for (int i = 99; i >= 1; --i) expected.push_back(i);
    EXPECT_EQ(Ids(d().Dispatch("SELECT id FROM t ORDER BY id DESC LIMIT 1000").response), expected);
}

TEST_F(WalkSliceRigTest, ACancelEndsAReverseWalkAtItsNextBoundary) {
    Load(1, 1, kRows);
    Script walk{&long_, {"BEGIN", "SELECT id FROM t ORDER BY id DESC LIMIT 100000"}};
    Script canceller{&other_, {}};
    std::atomic<bool> walk_running{false};
    canceller.after = [&] {
        walk_running.store(walk.done.load() == 1);
        long_.RequestCancel();
    };
    RunBoth(walk, canceller);

    ASSERT_TRUE(walk_running.load()) << "the walk ended before the cancel: nothing yielded";
    EXPECT_EQ(walk.outs[1].status.code(), StatusCode::kCancelled) << walk.outs[1].response;
    EXPECT_TRUE(long_.failed());
    EXPECT_FALSE(long_.TakeCancel());
}

// ---- The ordered index run (CV O1, CV-R3, CV-R4) ---------------------------
//
// An index covering a ~2 KB cell holds about four entries a leaf, so the
// walk of 1,600 entries crosses many slice boundaries. `k` runs against the
// tuple key, so the index order is the reply's and not the relation's.

void LoadOrdered(CommandDispatcher& d, int rows) {
    ASSERT_EQ(d.Dispatch("CREATE TABLE o (id int64, k int64, name varchar(2000))")
                  .response.rfind("CREATED", 0),
              0u);
    ASSERT_EQ(d.Dispatch("CREATE INDEX o_k ON o (k) COVERING (name)").response.rfind("CREATED", 0),
              0u);
    for (int i = 0; i < rows;) {
        std::string sql = "INSERT INTO o VALUES ";
        for (int n = 0; n < 100 && i < rows; ++n, ++i) {
            if (n != 0) sql += ", ";
            sql += "(" + std::to_string(i + 1) + ", " + std::to_string(2 * (rows - i)) + ", 'x')";
        }
        const std::string reply = d.Dispatch(sql).response;
        ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
    }
}

TEST_F(WalkSliceRigTest, AnOrderedIndexWalkYieldsAndMissesAndRepeatsNoRowUnderDivides) {
    LoadOrdered(d(), kRows);
    const std::string ordered = "SELECT id FROM o ORDER BY k LIMIT 100000";
    ASSERT_NE(d().Dispatch("ANALYZE " + ordered).response.find("order=index_asc"),
              std::string::npos);
    Script walk{&long_, {ordered}};
    // Odd keys between every two the walk will read, each a divide of a
    // leaf behind, at or ahead of it, and moves of keys it has not reached.
    Script writer{&other_, {"SELECT id FROM o WHERE id = 5"}};
    for (int i = 0; i < kRows; ++i) {
        writer.lines.push_back("INSERT INTO o VALUES (" + std::to_string(kRows + 1 + i) + ", " +
                               std::to_string(2 * i + 1) + ", 'x')");
    }
    writer.lines.push_back("UPDATE o SET k = 0 WHERE id BETWEEN 1 AND 50");
    std::atomic<bool> walk_running{false};
    writer.after = [&] { walk_running.store(walk.done.load() == 0); };
    RunBoth(walk, writer);

    ASSERT_TRUE(walk_running.load()) << "the walk ended before the writes: nothing yielded";
    for (const DispatchOutcome& out : writer.outs) {
        ASSERT_EQ(out.response.rfind("ERR", 0), std::string::npos) << out.response;
    }
    ASSERT_TRUE(walk.outs[0].status.ok()) << walk.outs[0].response;
    std::vector<std::int64_t> expected;
    for (int i = kRows; i >= 1; --i) expected.push_back(i);
    EXPECT_EQ(Ids(walk.outs[0].response), expected)
        << "the ordered walk read under its own view, in its order, every row once";
}

TEST_F(WalkSliceRigTest, APurgedKeyNamedAgainIsReadAtItsNewKeyOnly) {
    // The purged rows' entries stay (IX1) and name tuple keys a named INSERT
    // binds again, under another order value: each row comes out once, at
    // the key its visible version holds (CV-R2's key check).
    LoadOrdered(d(), 300);
    ASSERT_EQ(d().Dispatch("DELETE FROM o WHERE id BETWEEN 1 AND 100").response.rfind("ERR", 0),
              std::string::npos);
    const std::string purged = d().Dispatch("PURGE FROM o WHERE id BETWEEN 1 AND 100").response;
    ASSERT_EQ(purged.rfind("ERR", 0), std::string::npos) << purged;
    for (int id = 1; id <= 100; ++id) {
        const std::string reply = d().Dispatch("INSERT INTO o VALUES (" + std::to_string(id) +
                                               ", " + std::to_string(10000 + id) + ", 'x')")
                                      .response;
        ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
    }
    std::vector<std::int64_t> expected;
    for (int id = 300; id >= 101; --id) expected.push_back(id);  // k = 2 * (301 - id)
    for (int id = 1; id <= 100; ++id) expected.push_back(id);    // k = 10000 + id
    EXPECT_EQ(Ids(d().Dispatch("SELECT id FROM o ORDER BY k LIMIT 1000").response), expected);
}

TEST_F(WalkSliceRigTest, ACancelEndsTheWalkAtItsNextBoundaryAndFailsTheTransaction) {
    Load(1, 1, kRows);
    Script walk{&long_, {"BEGIN", "SELECT id FROM t"}};
    Script canceller{&other_, {}};
    std::atomic<bool> walk_running{false};
    canceller.after = [&] {
        walk_running.store(walk.done.load() == 1);
        long_.RequestCancel();
    };
    RunBoth(walk, canceller);

    ASSERT_TRUE(walk_running.load()) << "the walk ended before the cancel: nothing yielded";
    EXPECT_EQ(walk.outs[1].status.code(), StatusCode::kCancelled) << walk.outs[1].response;
    EXPECT_EQ(walk.outs[1].response.rfind("ERR", 0), 0u);
    EXPECT_TRUE(long_.failed()) << "a cancelled statement leaves its transaction failed";
    EXPECT_FALSE(long_.TakeCancel()) << "the walk took the cancel: one cancel, one statement";
}

TEST_F(WalkSliceRigTest, AShortWalkNeverSeesACancel) {
    // No boundary, no observation: the flag waits for the session's next
    // frame (`KwpSession::OnFrame`).
    Load(1, 1, 8);
    long_.RequestCancel();
    const DispatchOutcome out = d().Dispatch("SELECT id FROM t", &long_);
    EXPECT_TRUE(out.status.ok()) << out.response;
    EXPECT_TRUE(long_.TakeCancel());
}

// The polls of core 0's `foreground` group one walk took, at each level in
// `levels`, run in turn by one task on the core's own thread: a walk's poll
// count is its yields plus one, so the ratio is the slices'.
sched::Coro CountWalkPolls(CommandDispatcher& d, sched::Scheduler& core, Session& s,
                           std::vector<Priority> levels, std::vector<std::uint64_t>* polls,
                           std::atomic<bool>* done) {
    for (const Priority level : levels) {
        s.set_priority(level);
        DispatchOutcome out;
        const std::uint64_t before = core.polls_total(sched::SchedulingGroup::kForeground);
        co_await d.DispatchAsync("SELECT id FROM t WHERE name = 'none'", &s, &out);
        polls->push_back(core.polls_total(sched::SchedulingGroup::kForeground) - before);
    }
    s.set_priority(Priority::kNormal);
    done->store(true, std::memory_order_release);
    co_return Status::OK();
}

// **The walk slice is the walking session's level's** (BL-R2, BL-S4): an
// `analytic` walk yields every 16 pages, four times as often as a `normal`
// one at 64, which is `exec::SlicePolicy::pages_per_slice` re-scoped.
//
// **The mutation**: drop `PagesPerSlice` from `SelectRun` and both walks
// take the same polls.
TEST_F(WalkSliceRigTest, AnAnalyticWalkYieldsFourTimesAsOftenAsANormalOne) {
    Load(1, 1, kRows);
    std::vector<std::uint64_t> polls;
    std::atomic<bool> done{false};
    rig_->core(0).scheduler().Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        CountWalkPolls(d(), rig_->core(0).scheduler(), long_,
                       {Priority::kNormal, Priority::kAnalytic}, &polls, &done)));
    rig_->Start();
    ASSERT_TRUE(Within(60000ms, [&] { return done.load(std::memory_order_acquire); }))
        << "the walks never finished";
    ASSERT_EQ(polls.size(), 2u);
    EXPECT_GE(polls[0], 3u) << "a normal walk over ~400 leaves did not yield";
    EXPECT_GE(polls[1], 3 * polls[0]) << "normal " << polls[0] << " polls, analytic " << polls[1];
}

TEST_F(WalkSliceRigTest, ACoreTornDownWithAWalkBetweenSlicesReleasesItFirst) {
    // A peer's walk, run by hand to its first boundary and left queued; the
    // rig is then destroyed with it there. The scheduler goes last of the
    // runtime's members, so the walk's frame must be destroyed before the
    // dispatcher, catalog and transaction manager it releases into.
    Load(1, 1, kRows);
    CommandDispatcher& peer = rig_->core(1).dispatcher();
    Script walk{&long_, {"SELECT id FROM t"}};
    rig_->core(1).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, RunScript(peer, walk)));
    // `RunOnce` declares its core on the calling thread and leaves it
    // declared, as a reactor's own thread wants; this one is the test's, and
    // a later cell in the same process must not run as core 1.
    const CurrentCoreGuard as_test_thread(CurrentCore());
    for (int i = 0; i < 4; ++i) (void)rig_->core(1).scheduler().RunOnce();
    ASSERT_EQ(walk.done.load(), 0u) << "the walk finished before the teardown";
    rig_.reset();
}

// ---- The bookkeeping, alone ----------------------------------------------

TEST(StatementEpochHoldTest, TheSlotPublishesTheLowestWordItsCoreHolds) {
    StatementEpochs epochs(1);
    epochs.Enter(0);
    epochs.Publish(0, 5);
    epochs.Hold(0, 5);
    epochs.Leave(0);
    EXPECT_FALSE(epochs.AllAtLeast(6)) << "a held walk is still a statement on the core";
    EXPECT_TRUE(epochs.AllAtLeast(5));

    epochs.Enter(0);
    epochs.Publish(0, 9);
    epochs.Hold(0, 9);
    epochs.Hold(0, 9);
    epochs.Leave(0);
    epochs.Release(0, 5);
    EXPECT_FALSE(epochs.AllAtLeast(10));
    EXPECT_TRUE(epochs.AllAtLeast(9));
    epochs.Release(0, 9);
    EXPECT_FALSE(epochs.AllAtLeast(10)) << "one of two holds of a word was given back";
    epochs.Release(0, 9);
    EXPECT_TRUE(epochs.AllAtLeast(StatementEpochs::kIdle));
}

TEST(CatalogCachePinTest, AnInvalidateUnderAPinRetiresTheMemoWithItsAddresses) {
    catalog::CatalogCache cache;
    catalog::TableAccess access;
    access.oid = 4000;
    const catalog::TableAccess* bound = cache.PutTableAccess(access);
    const std::uint64_t first = cache.Pin();
    cache.Invalidate();
    EXPECT_EQ(cache.FindTableAccess(4000), nullptr) << "the next statement fills afresh";
    EXPECT_EQ(cache.retired_generations(), 1u);
    EXPECT_EQ(bound->oid, 4000u) << "the pinned walk's pointer still names its entry";

    // A later walk overlapping the first pins its own generation; the first
    // generation goes when *its* walk ends, not when the core goes idle.
    cache.PutTableAccess(access);
    const std::uint64_t second = cache.Pin();
    cache.Invalidate();
    EXPECT_EQ(cache.retired_generations(), 2u);
    cache.Unpin(first);
    EXPECT_EQ(cache.retired_generations(), 1u) << "the first walk's generation outlived it";
    cache.Unpin(second);
    EXPECT_EQ(cache.retired_generations(), 0u);

    cache.PutTableAccess(access);
    cache.Invalidate();
    EXPECT_EQ(cache.retired_generations(), 0u) << "with no pin, an invalidate frees at once";
}

TEST(CancelRegistryTest, OnlyTheRightKeySetsTheFlagAndAClosedSessionIsGone) {
    CancelRegistry registry;
    auto flag = std::make_shared<std::atomic<bool>>(false);
    registry.Register(7, 0xABCD, flag);
    EXPECT_FALSE(registry.Cancel(7, 0xABCE));
    EXPECT_FALSE(registry.Cancel(8, 0xABCD));
    EXPECT_FALSE(flag->load());
    EXPECT_TRUE(registry.Cancel(7, 0xABCD));
    EXPECT_TRUE(flag->load());
    flag->store(false);
    registry.Unregister(7);
    EXPECT_FALSE(registry.Cancel(7, 0xABCD));
    EXPECT_FALSE(flag->load());
}

}  // namespace
}  // namespace kds::server
