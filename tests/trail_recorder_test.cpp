#include "kds/stats/trail_recorder.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/pattern_ddl.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/trail_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// The trail recorder end to end (docs/inflight/in-progress/waystone-workplan.md P09/P10): what
// decides an instance is worth remembering, and what it writes.
//
// The last test in this file is the one that matters most. Everything else
// checks that recording happens; that one checks that it **changes
// nothing**, which is the property invariant 8 makes non-negotiable and the
// only one a bug here could break silently.

namespace kds::stats {
namespace {

class TrailRecorderTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));

        recorder_.emplace(boot_->catalog, store_);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), &*recorder_);

        ASSERT_EQ(Run("CREATE TABLE t (id int64, v int64) BTREE").substr(0, 7), "CREATED");
        for (int i = 1; i <= 5; ++i) {
            ASSERT_EQ(Run("INSERT INTO t VALUES (" + std::to_string(i * 10) + ")").substr(0, 8),
                      "INSERTED");
        }
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    static InstanceKey KeyOf(const std::string& sql) {
        auto fp = parser::FingerprintOf(sql);
        EXPECT_TRUE(fp.has_value());
        return InstanceKey{fp->pattern_id, fp->arg_hash};
    }

    // The trail recorded for `sql`'s instance, or empty when there is none.
    std::vector<WaystoneEntry> TrailFor(const std::string& sql) {
        const InstanceKey key = KeyOf(sql);
        auto pattern = boot_->catalog.FindPattern(key.pattern_id);
        if (!pattern.ok() || !pattern.value()->has_waystone_directory()) return {};
        auto read = ReadTrail(store_, pattern.value()->waystone_root,
                              pattern.value()->dir_depth, key);
        EXPECT_TRUE(read.ok());
        return read.ok() ? read.value() : std::vector<WaystoneEntry>{};
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<TrailRecorder> recorder_;
    std::optional<server::CommandDispatcher> dispatcher_;
};

// ---- The policy ----------------------------------------------------------

TEST_F(TrailRecorderTest, AnObservedInstanceRecordsOnItsSecondExecution) {
    const std::string sql = "SELECT * FROM t WHERE id = 3";

    Run(sql);
    // The first execution only counts. Recording it would pay a page write
    // for every one-shot query a client ever sends, which is exactly what
    // n=2 exists to avoid - and with no pattern row yet, a one-shot
    // statement leaves no catalog trace at all.
    EXPECT_TRUE(TrailFor(sql).empty());
    EXPECT_FALSE(boot_->catalog.FindPattern(KeyOf(sql).pattern_id).ok())
        << "a shape seen once is not yet worth a sys.patterns row";

    Run(sql);
    EXPECT_EQ(TrailFor(sql).size(), 1u) << "the second execution records";
    EXPECT_EQ(recorder_->stats().trails_written, 1u);
    EXPECT_EQ(recorder_->stats().patterns_registered, 1u);
}

TEST_F(TrailRecorderTest, ADeclaredPatternRecordsOnItsFirstExecution) {
    // A declaration *is* the evidence n=2 waits for
    // (create-pattern-user-defined-patterns-v1.md section 7), so
    // making an operator prove it again with traffic asks a question they
    // already answered.
    ASSERT_EQ(Run("CREATE PATTERN p($id int64) OF SELECT * FROM t WHERE id = $id").substr(0, 7),
              "CREATED");

    const std::string sql = "SELECT * FROM t WHERE id = 3";
    Run(sql);
    EXPECT_EQ(TrailFor(sql).size(), 1u);
    // Declared, so nothing had to be auto-registered.
    EXPECT_EQ(recorder_->stats().patterns_registered, 0u);
}

TEST_F(TrailRecorderTest, TwoInstancesOfOnePatternCountSeparately) {
    // The sighting count is per *instance*, not per pattern: one hot
    // argument must not make a cold one look hot.
    Run("SELECT * FROM t WHERE id = 3");
    Run("SELECT * FROM t WHERE id = 4");
    EXPECT_TRUE(TrailFor("SELECT * FROM t WHERE id = 3").empty());
    EXPECT_TRUE(TrailFor("SELECT * FROM t WHERE id = 4").empty());

    Run("SELECT * FROM t WHERE id = 3");
    EXPECT_EQ(TrailFor("SELECT * FROM t WHERE id = 3").size(), 1u);
    EXPECT_TRUE(TrailFor("SELECT * FROM t WHERE id = 4").empty())
        << "the other instance has still only been seen once";
}

TEST_F(TrailRecorderTest, WouldRecordIsTheOnePlaceThePolicyLives) {
    EXPECT_FALSE(recorder_->WouldRecord(0, catalog::kOriginAuto));
    EXPECT_FALSE(recorder_->WouldRecord(1, catalog::kOriginAuto));
    EXPECT_TRUE(recorder_->WouldRecord(2, catalog::kOriginAuto));

    EXPECT_FALSE(recorder_->WouldRecord(0, catalog::kOriginUser));
    EXPECT_TRUE(recorder_->WouldRecord(1, catalog::kOriginUser));
}

// ---- What is in a trail --------------------------------------------------

TEST_F(TrailRecorderTest, OnlyTrailReplayableStepsAreRecorded) {
    // A pk equality on a btree relation is a Lookup, and Lookup is
    // replayable - so its row is recorded.
    const std::string point = "SELECT * FROM t WHERE id = 3";
    Run(point);
    Run(point);
    ASSERT_EQ(TrailFor(point).size(), 1u);

    // A non-pk predicate is a Scan, and invariant 9 forbids a trail from
    // replacing a search. Its rows could only ever be prefetched, and
    // nothing prefetches - so recording them would be paying a write per
    // scanned row for a read nobody makes.
    const std::string scan = "SELECT * FROM t WHERE v = 30";
    Run(scan);
    Run(scan);
    EXPECT_TRUE(TrailFor(scan).empty())
        << "a scan-only statement must record nothing at all";
}

TEST_F(TrailRecorderTest, ARecordedEntryNamesWhereTheRowActuallyWas) {
    const std::string sql = "SELECT * FROM t WHERE id = 3";
    Run(sql);
    Run(sql);

    auto trail = TrailFor(sql);
    ASSERT_EQ(trail.size(), 1u);
    EXPECT_EQ(trail[0].pk, 3u);
    EXPECT_NE(trail[0].page_id, kInvalidPageId);
    EXPECT_NE(trail[0].flags & kWaystoneEntryValid, 0);

    auto oid = boot_->catalog.FindTableOidByName("t");
    ASSERT_TRUE(oid.ok());
    EXPECT_EQ(trail[0].rel_oid, oid.value());
}

TEST_F(TrailRecorderTest, AJoinRecordsOneEntryPerReplayableStepInOrder) {
    ASSERT_EQ(Run("CREATE TABLE u (id int64, w int64) BTREE").substr(0, 7), "CREATED");
    for (int i = 1; i <= 5; ++i) {
        ASSERT_EQ(Run("INSERT INTO u VALUES (" + std::to_string(i) + ")").substr(0, 8),
                  "INSERTED");
    }

    // Lookup on `t`, then a pk probe into `u`. Both steps lookup-class, so
    // both contribute - and the step_id is what tells a future replay which
    // entry is the driving relation and which is the probe result.
    //
    // Joined on `a.id = b.id`, not on `a.v`: ids are engine-assigned 1..5 in
    // both relations, where `v` holds 10..50 and would probe for a `u.id`
    // that does not exist - which records one entry, not two, and would
    // have this test measuring the empty-probe case instead.
    const std::string sql = "SELECT a.id FROM t AS a JOIN u AS b ON a.id = b.id WHERE a.id = 3";
    Run(sql);
    Run(sql);

    auto trail = TrailFor(sql);
    ASSERT_EQ(trail.size(), 2u);
    EXPECT_EQ(trail[0].step_id, 0u);
    EXPECT_EQ(trail[1].step_id, 1u);
    EXPECT_NE(trail[0].rel_oid, trail[1].rel_oid) << "one trail, two relations";
}

// ---- What must not be recorded -------------------------------------------

TEST_F(TrailRecorderTest, AFailedStatementRecordsNothing) {
    const std::string bad = "SELECT * FROM nosuchtable WHERE id = 1";
    EXPECT_EQ(Run(bad).substr(0, 3), "ERR");
    EXPECT_EQ(Run(bad).substr(0, 3), "ERR");

    // A trail from a statement that errored describes a state no reader
    // should ever be pointed at (workplan P10). Nothing was even counted:
    // the recorder is not reached on the failure path.
    EXPECT_EQ(recorder_->stats().trails_written, 0u);
    EXPECT_EQ(recorder_->stats().sightings, 0u);
}

TEST_F(TrailRecorderTest, AStatementThatCollectsNothingIsNotEvenCounted) {
    const std::string sql = "SELECT * FROM t WHERE id = 99999";
    Run(sql);
    Run(sql);
    EXPECT_TRUE(TrailFor(sql).empty());
    EXPECT_EQ(recorder_->stats().trails_written, 0u);

    // **Not counted as a sighting either**, and that is a cost decision
    // rather than an oversight. An execution that located no tuple can
    // never produce a trail however often it repeats, so counting it buys
    // a number nothing will read - and reaching the recorder at all costs
    // a second lex of the statement (the fingerprint bolt-on), which is
    // the single most expensive thing on the recording path.
    //
    // Nothing is lost: if the row is inserted later, the instance starts
    // collecting tuples and starts counting from then.
    EXPECT_EQ(recorder_->stats().sightings, 0u);

    // The same reasoning covers a scan-only statement, which collects
    // nothing because invariant 9 forbids replaying a search.
    Run("SELECT * FROM t WHERE v = 20");
    Run("SELECT * FROM t WHERE v = 20");
    EXPECT_EQ(recorder_->stats().sightings, 0u);
}

TEST_F(TrailRecorderTest, ClearingTheSightingTableOnlyRestartsCounting) {
    // Spec section 9: eviction here is a performance event, never a
    // correctness one. Driven directly rather than by executing 4096
    // distinct statements, which would test the same line far more slowly.
    exec::TrailCollector trail(kMaxTrailEntries);
    exec::TouchedTuple t;
    t.rel_oid = 1;
    t.pk = 1;
    t.page_id = 200;
    trail.Add(t);

    for (std::size_t i = 0; i < TrailRecorder::kMaxSightings + 1; ++i) {
        recorder_->OnPatternResult(InstanceKey{1000 + i, i}, trail,
                                   catalog::kStmtClassUnclassified);
    }
    EXPECT_GE(recorder_->stats().sighting_table_clears, 1u);
    // Nothing recorded: every one of those instances was seen exactly once.
    EXPECT_EQ(recorder_->stats().trails_written, 0u);
}

TEST_F(TrailRecorderTest, AnOverflowedCollectorRecordsNothingRatherThanAPartialTrail) {
    // A collector that dropped tuples holds an incomplete account of the
    // execution, and a partial trail is indistinguishable from a complete
    // one to every reader (trail_store.hpp).
    exec::TrailCollector tiny(1);
    exec::TouchedTuple t;
    t.rel_oid = 1;
    t.pk = 1;
    t.page_id = 200;
    tiny.Add(t);
    tiny.Add(t);
    ASSERT_TRUE(tiny.overflowed());

    const InstanceKey key{4242, 1};
    recorder_->OnPatternResult(key, tiny, catalog::kStmtClassUnclassified);
    recorder_->OnPatternResult(key, tiny, catalog::kStmtClassUnclassified);
    EXPECT_EQ(recorder_->stats().trails_written, 0u);
    EXPECT_GE(recorder_->stats().skipped_overflow, 1u);
}

// ---- Heat ----------------------------------------------------------------

TEST_F(TrailRecorderTest, RecordingAdvancesTheHeatCountersSHOWPATTERNSReports) {
    const std::string sql = "SELECT * FROM t WHERE id = 3";
    Run(sql);
    Run(sql);
    Run(sql);

    auto row = boot_->catalog.GetSysPatternRow(KeyOf(sql).pattern_id);
    ASSERT_TRUE(row.ok());
    // Two recorded executions after the one that only counted. Before this
    // change nothing in the engine ever incremented this field, so
    // SHOW PATTERNS reported uses=0 forever.
    EXPECT_EQ(row.value().use_count, 2u);
}

// ---- The one that matters ------------------------------------------------

TEST_F(TrailRecorderTest, ResultsAreByteIdenticalWithRecordingOnAndOff) {
    // Spec section 11-3's first two configurations, and the reason this
    // whole feature can ship before replay: a trail is advisory, so
    // recording one must not change a single byte of a single reply
    // (invariant 8). Nothing reads a trail yet, which makes this easy to
    // pass today and exactly why it is worth pinning *now* - it is the
    // regression that a future replayer must not introduce.
    const std::vector<std::string> queries = {
        "SELECT * FROM t",
        "SELECT * FROM t WHERE id = 3",
        "SELECT * FROM t WHERE id = 99999",
        "SELECT * FROM t WHERE v = 30",
        "SELECT id FROM t WHERE id = 1",
    };

    // A second database, identical but for the recorder.
    storage::InMemoryPageStore quiet_store{server::kFirstUserPageId};
    auto quiet_boot = bootstrap::BootstrapDatabase(quiet_store, 1000);
    ASSERT_TRUE(quiet_boot.ok());
    server::CommandDispatcher quiet(quiet_boot.value().superblock, quiet_boot.value().catalog,
                                    quiet_store, /*log=*/nullptr, /*clock=*/nullptr,
                                    /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                                    exec::Budget(), /*recorder=*/nullptr);
    ASSERT_EQ(quiet.Dispatch("CREATE TABLE t (id int64, v int64) BTREE").response.substr(0, 7),
              "CREATED");
    for (int i = 1; i <= 5; ++i) {
        ASSERT_EQ(
            quiet.Dispatch("INSERT INTO t VALUES (" + std::to_string(i * 10) + ")")
                .response.substr(0, 8),
            "INSERTED");
    }

    // Three rounds, so the comparison spans the execution that starts
    // recording and every one after it - the interesting window is not the
    // first run, it is the run *after* a trail exists.
    for (int round = 0; round < 3; ++round) {
        for (const std::string& sql : queries) {
            EXPECT_EQ(Run(sql), quiet.Dispatch(sql).response)
                << "round " << round << ", statement: " << sql;
        }
    }

    // And recording really was on for the recorded side, or the comparison
    // above proved nothing.
    EXPECT_GT(recorder_->stats().trails_written, 0u);
}

}  // namespace
}  // namespace kds::stats
