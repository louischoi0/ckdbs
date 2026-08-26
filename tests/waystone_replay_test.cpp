#include "kds/exec/trail_replay.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/step_compiler.hpp"
#include "kds/exec/step_vm.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/stats/trail_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// Replay: serving a keyed step from a recorded location instead of
// descending for it (docs/inflight/in-progress/waystone-workplan.md P11/P13).
//
// The hit path is one test. The rest of this file is **misses**, because a
// trail is advisory and every way it can be wrong has to end in the same
// place - the authoritative path, with the right answer. Spec section 2
// rule 4 says a miss falls through *for that step alone*; these tests are
// what makes that a fact rather than an intention.

namespace kds::exec {
namespace {

class WaystoneReplayTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        recorder_.emplace(boot_->catalog, store_);
        MakeDispatcher(/*record=*/true, /*replay=*/true);

        ASSERT_EQ(Run("CREATE TABLE t (id int64, v int64) BTREE").substr(0, 7), "CREATED");
        ASSERT_EQ(Run("CREATE TABLE h (id int64, v int64)").substr(0, 7), "CREATED");
        for (int i = 1; i <= 5; ++i) {
            ASSERT_EQ(Run("INSERT INTO t VALUES (" + std::to_string(i * 10) + ")").substr(0, 8),
                      "INSERTED");
            ASSERT_EQ(Run("INSERT INTO h VALUES (" + std::to_string(i * 10) + ")").substr(0, 8),
                      "INSERTED");
        }
    }

    void MakeDispatcher(bool record, bool replay) {
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            Budget(), record ? &*recorder_ : nullptr, replay);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    // Runs `sql` until its instance has a trail (n=2 for an observed
    // pattern), then returns the reply of one more run.
    std::string RunUntilRecorded(const std::string& sql) {
        Run(sql);
        Run(sql);
        return Run(sql);
    }

    static stats::InstanceKey KeyOf(const std::string& sql) {
        auto fp = parser::FingerprintOf(sql);
        EXPECT_TRUE(fp.has_value());
        return stats::InstanceKey{fp->pattern_id, fp->arg_hash};
    }

    // The pattern's directory pair, for tests that rewrite a trail by hand.
    std::pair<PageId, std::uint8_t> DirectoryOf(const std::string& sql) {
        auto pattern = boot_->catalog.FindPattern(KeyOf(sql).pattern_id);
        EXPECT_TRUE(pattern.ok());
        return {pattern.value()->waystone_root, pattern.value()->dir_depth};
    }

    // Replaces `sql`'s trail with one entry pointing wherever we say.
    void PoisonTrail(const std::string& sql, const exec::TouchedTuple& entry) {
        auto [root, depth] = DirectoryOf(sql);
        std::vector<exec::TouchedTuple> one{entry};
        ASSERT_TRUE(stats::WriteTrail(store_, root, depth, KeyOf(sql), one, /*ts=*/1).ok());
    }

    exec::TouchedTuple EntryOf(const std::string& sql) {
        auto [root, depth] = DirectoryOf(sql);
        auto read = stats::ReadTrail(store_, root, depth, KeyOf(sql));
        EXPECT_TRUE(read.ok());
        EXPECT_FALSE(read.value().empty());
        exec::TouchedTuple t;
        if (!read.value().empty()) {
            t.rel_oid = read.value()[0].rel_oid;
            t.pk = read.value()[0].pk;
            t.page_id = read.value()[0].page_id;
            t.slot = read.value()[0].slot;
            t.step_id = read.value()[0].step_id;
        }
        return t;
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::TrailRecorder> recorder_;
    std::optional<server::CommandDispatcher> dispatcher_;
};

// ---- The hit path --------------------------------------------------------

TEST_F(WaystoneReplayTest, AReplayedStepReturnsExactlyWhatTheDescentReturned) {
    const std::string sql = "SELECT * FROM t WHERE id = 3";

    const std::string first = Run(sql);   // descends; counts only
    const std::string second = Run(sql);  // descends; records
    const std::string third = Run(sql);   // replays

    EXPECT_EQ(first, second);
    EXPECT_EQ(second, third) << "a replayed row must be the row the descent found";

    // **And the replay actually happened.** Without this the three
    // comparisons above would pass just as happily if replay never fired,
    // which is the way a test like this rots into proving nothing. ANALYZE
    // runs the identical path, so its counters describe the real execution.
    const std::string analyzed = Run("ANALYZE " + sql);
    EXPECT_NE(analyzed.find("replays=1"), std::string::npos)
        << "expected the step to be served from the trail; got: " << analyzed;
}

TEST_F(WaystoneReplayTest, ReplayTurnsAHeapChainScanIntoOneRead) {
    // The case spec section 7 calls large: a heap relation has no pk index,
    // so the authoritative path for `WHERE id = n` is a full chain scan.
    const std::string sql = "SELECT * FROM h WHERE id = 4";
    const std::string replayed = RunUntilRecorded(sql);

    // Same answer as the scan gave.
    MakeDispatcher(/*record=*/false, /*replay=*/false);
    EXPECT_EQ(Run(sql), replayed);
}

// ---- The miss causes, one test each --------------------------------------

TEST_F(WaystoneReplayTest, AnInstanceWithNoTrailDescendsAndIsStillRight) {
    // The ordinary cold case. Nothing recorded, so nothing to consult.
    MakeDispatcher(/*record=*/false, /*replay=*/true);
    const std::string sql = "SELECT * FROM t WHERE id = 2";
    const std::string with_replay_on = Run(sql);

    MakeDispatcher(/*record=*/false, /*replay=*/false);
    EXPECT_EQ(with_replay_on, Run(sql));
}

TEST_F(WaystoneReplayTest, AWrongKeystoneAtTheTargetIsAMissNotAWrongRow) {
    // **The load-bearing miss.** The entry names a real, live tuple - just
    // not the one it claims. Without spec section 2 rule 1's identity check
    // this returns row 5 for a query asking for row 3; with it, the entry
    // misses and the descent answers.
    const std::string sql = "SELECT * FROM t WHERE id = 3";
    const std::string correct = RunUntilRecorded(sql);

    exec::TouchedTuple entry = EntryOf(sql);
    const std::uint16_t original_slot = entry.slot;
    entry.slot = static_cast<std::uint16_t>(original_slot + 2);  // row 5's slot
    ASSERT_NE(entry.slot, original_slot);
    PoisonTrail(sql, entry);

    EXPECT_EQ(Run(sql), correct) << "a trail pointing at the wrong tuple must miss";
}

TEST_F(WaystoneReplayTest, APageThatWentAwayIsAMiss) {
    const std::string sql = "SELECT * FROM t WHERE id = 3";
    const std::string correct = RunUntilRecorded(sql);

    exec::TouchedTuple entry = EntryOf(sql);
    entry.page_id = 999999;  // never allocated
    PoisonTrail(sql, entry);

    EXPECT_EQ(Run(sql), correct);
}

TEST_F(WaystoneReplayTest, ASlotPastTheEndOfThePageIsAMiss) {
    const std::string sql = "SELECT * FROM t WHERE id = 3";
    const std::string correct = RunUntilRecorded(sql);

    exec::TouchedTuple entry = EntryOf(sql);
    entry.slot = 30000;
    PoisonTrail(sql, entry);

    EXPECT_EQ(Run(sql), correct);
}

TEST_F(WaystoneReplayTest, AnEntryRecordedAgainstAnotherRelationIsDroppedAtBuild) {
    // Spec section 2 rule 1's relation half. An entry whose rel_oid is not
    // its step's would, if consulted, decode one relation's page with
    // another's schema - the same class of bug the probe memo had.
    const std::string sql = "SELECT * FROM t WHERE id = 3";
    const std::string correct = RunUntilRecorded(sql);

    exec::TouchedTuple entry = EntryOf(sql);
    auto other = boot_->catalog.FindTableOidByName("h");
    ASSERT_TRUE(other.ok());
    entry.rel_oid = other.value();
    PoisonTrail(sql, entry);

    EXPECT_EQ(Run(sql), correct);
}

// ---- Rule 0, the one storage cannot supply --------------------------------

TEST_F(WaystoneReplayTest, AnEntryForADifferentKeyIsNeverConsulted) {
    // Rule 0 is the lookup key here: the index is keyed on (step_id, pk) and
    // the caller looks up by the key its step just derived. An entry
    // recorded for another key is not "checked and rejected", it is not
    // found - which is the point, since a check can be forgotten and a key
    // cannot.
    const std::string sql = "SELECT * FROM t WHERE id = 3";
    const std::string correct = RunUntilRecorded(sql);

    exec::TouchedTuple entry = EntryOf(sql);
    entry.pk = 5;  // a real row, but not this statement's
    PoisonTrail(sql, entry);

    EXPECT_EQ(Run(sql), correct);
}

// ---- Cross-relation replay (P13) -----------------------------------------

TEST_F(WaystoneReplayTest, AJoinReplaysEveryKeyedStepAndReturnsTheSameRows) {
    ASSERT_EQ(Run("CREATE TABLE u (id int64, w int64) BTREE").substr(0, 7), "CREATED");
    for (int i = 1; i <= 5; ++i) {
        ASSERT_EQ(Run("INSERT INTO u VALUES (" + std::to_string(i * 100) + ")").substr(0, 8),
                  "INSERTED");
    }

    // Lookup on t, then a pk probe into u: both steps keyed, so both are
    // replayable and the trail spans two relations.
    const std::string sql =
        "SELECT a.v, b.w FROM t AS a JOIN u AS b ON a.id = b.id WHERE a.id = 3";

    const std::string descended = Run(sql);
    Run(sql);
    const std::string replayed = Run(sql);
    EXPECT_EQ(descended, replayed);

    auto [root, depth] = DirectoryOf(sql);
    auto trail = stats::ReadTrail(store_, root, depth, KeyOf(sql));
    ASSERT_TRUE(trail.ok());
    ASSERT_EQ(trail.value().size(), 2u);
    EXPECT_EQ(trail.value()[0].step_id, 0u);
    EXPECT_EQ(trail.value()[1].step_id, 1u);
}

// ---- What must never be replayed -----------------------------------------

TEST_F(WaystoneReplayTest, AScanStepIsNeverServedFromATrail) {
    // Invariant 9: a trail may replace a lookup, never a search. A non-pk
    // predicate is a search however often it repeats, and this is the
    // instrumented half of spec section 11-4 - the *work not done* is the
    // only evidence, since a wrongly skipped search returns plausible rows.
    const std::string sql = "SELECT * FROM t WHERE v = 30";
    Run(sql);
    Run(sql);

    auto parsed = parser::Parse(sql);
    ASSERT_TRUE(parsed.ok());
    auto chain = Compile(boot_->catalog, std::get<parser::SelectStmt>(parsed.value()));
    ASSERT_TRUE(chain.ok());

    // Nothing was recorded for it, so there is nothing to replay from...
    auto pattern = boot_->catalog.FindPattern(KeyOf(sql).pattern_id);
    EXPECT_FALSE(pattern.ok()) << "a scan-only statement registers no pattern at all";

    // ...and even handed a fabricated trail naming its step, the executor
    // must not use one: TrailReplay::Build drops entries for steps that are
    // not trail-replayable.
    stats::WaystoneEntry fake{};
    fake.pk = 3;
    fake.rel_oid = chain.value().steps[0].rel_oid;
    fake.page_id = 129;
    fake.slot = 0;
    fake.step_id = 0;
    fake.flags = stats::kWaystoneEntryValid;
    TrailReplay replay;
    replay.Build(chain.value(), {&fake, 1});
    EXPECT_TRUE(replay.empty()) << "a Scan step's entry must never be indexed";
}

}  // namespace
}  // namespace kds::exec
