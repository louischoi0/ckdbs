#include "kds/stats/optimizer_signals.hpp"

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/sched/clock.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// The cabin optimizer's input signals (physical-optimizer.md §II.2,
// workplan PHY01). Three claims carry the acceptance: decayed accumulation
// is exact where the decay contract says it is, a snapshot is an immutable
// versioned value, and the counters are correct under a scripted workload
// end to end - dispatcher in, snapshot out.

namespace kds::server {
namespace {

constexpr sched::MonoTimeNs kHalfLife = 600'000'000'000ULL;

TEST(OptimizerSignalsTest, ExecutionSignalKeepsADecayedMeanPair) {
    sched::ManualClock clock;
    stats::OptimizerSignals signals(&clock, kHalfLife);

    for (int i = 0; i < 4; ++i) signals.NoteExecution(/*pattern_id=*/7, /*pages=*/10);
    clock.Advance(kHalfLife);

    const stats::OptimizerSnapshot snap = signals.Snapshot();
    ASSERT_EQ(snap.fingerprints.size(), 1u);
    EXPECT_EQ(snap.fingerprints[0].pattern_id, 7u);
    // Both halves halve on the same clock, so the pair reads 2 executions
    // and 20 pages - and the mean they imply, 10 pages per execution, is
    // exactly what was fed in. The ratio's stability under idleness is the
    // whole reason S2 is a pair and not a stored quotient.
    EXPECT_EQ(snap.fingerprints[0].frequency_q8, 2 * stats::kDecayScoreScale);
    EXPECT_EQ(snap.fingerprints[0].pages_q8, 20 * stats::kDecayScoreScale);
}

TEST(OptimizerSignalsTest, SnapshotIsVersionedStampedSortedAndImmutable) {
    sched::ManualClock clock(1000);
    stats::OptimizerSignals signals(&clock, kHalfLife);

    signals.NoteExecution(9, 1);
    signals.NoteExecution(3, 1);
    signals.NoteCabinLookup(5, /*served=*/true);
    signals.NoteCabinLookup(2, /*served=*/false);

    const stats::OptimizerSnapshot first = signals.Snapshot();
    EXPECT_EQ(first.version, 1u);
    EXPECT_EQ(first.decay_epoch, 1000u);
    ASSERT_EQ(first.fingerprints.size(), 2u);
    EXPECT_LT(first.fingerprints[0].pattern_id, first.fingerprints[1].pattern_id);
    ASSERT_EQ(first.cabins.size(), 2u);
    EXPECT_LT(first.cabins[0].cabin_id, first.cabins[1].cabin_id);

    // A snapshot is a value: later touches move the collector, never the
    // snapshot already taken.
    signals.NoteExecution(9, 50);
    clock.Advance(17);
    const stats::OptimizerSnapshot second = signals.Snapshot();
    EXPECT_EQ(second.version, 2u);
    EXPECT_EQ(first.fingerprints[1].frequency_q8, stats::kDecayScoreScale)
        << "the first snapshot moved after it was taken";
    EXPECT_GT(second.fingerprints[1].frequency_q8, first.fingerprints[1].frequency_q8);
}

TEST(OptimizerSignalsTest, CabinSignalsFoldLookupsFailuresAndCoverage) {
    stats::OptimizerSignals signals(/*clock=*/nullptr, kHalfLife);

    signals.NoteCabinLookup(11, /*served=*/true);
    signals.NoteCabinLookup(11, /*served=*/true);
    signals.NoteCabinLookup(11, /*served=*/false);  // coverage miss
    signals.NoteCabinHint(11, /*ok=*/true);          // a success moves nothing
    signals.NoteCabinHint(11, /*ok=*/false);

    const stats::OptimizerSnapshot snap = signals.Snapshot();
    ASSERT_EQ(snap.cabins.size(), 1u);
    EXPECT_EQ(snap.cabins[0].lookups_q8, 3 * stats::kDecayScoreScale);
    EXPECT_EQ(snap.cabins[0].coverage_misses_q8, 1 * stats::kDecayScoreScale);
    EXPECT_EQ(snap.cabins[0].hint_failures_q8, 1 * stats::kDecayScoreScale);
}

TEST(OptimizerSignalsTest, TheFingerprintTableEvictsItsColdestAtTheCap) {
    sched::ManualClock clock;
    stats::OptimizerSignals signals(&clock, kHalfLife);

    // Fill to the cap; make fingerprint 0 the unambiguously coldest.
    for (std::uint64_t id = 0; id < stats::kMaxTrackedFingerprints; ++id) {
        signals.NoteExecution(id, 1);
        if (id != 0) signals.NoteExecution(id, 1);
    }
    ASSERT_EQ(signals.tracked_fingerprints(), stats::kMaxTrackedFingerprints);

    // A newcomer evicts the coldest rather than being refused: the table
    // stays at the cap and the new fingerprint is tracked.
    signals.NoteExecution(999'999, 1);
    EXPECT_EQ(signals.tracked_fingerprints(), stats::kMaxTrackedFingerprints);

    const stats::OptimizerSnapshot snap = signals.Snapshot();
    bool newcomer = false;
    bool coldest = false;
    for (const stats::SnapshotFingerprint& fp : snap.fingerprints) {
        if (fp.pattern_id == 999'999) newcomer = true;
        if (fp.pattern_id == 0) coldest = true;
    }
    EXPECT_TRUE(newcomer);
    EXPECT_FALSE(coldest) << "the coldest entry survived the eviction that admitted the newcomer";
}

TEST(OptimizerSignalsTest, EvictionPicksTheColdestAmongEntriesTheLinearScoreFlattens) {
    // A full table is mostly *idle* entries, and Q24.8 reads every score
    // older than ~16 half-lives as exactly 0 - so the eviction scan used
    // to find a tie among all of them and keep whichever the hash map
    // happened to yield first. The victim was arbitrary among the cold: a
    // fingerprint idle for an hour could outlive one idle for a week.
    // Ranking in the log domain restores the intent, and this is the case
    // that tells the two implementations apart.
    sched::ManualClock clock(1);
    stats::OptimizerSignals signals(&clock, kHalfLife);

    // One ancient entry, then the rest merely stale - every one of them
    // far enough past the linear floor to read zero.
    signals.NoteExecution(/*pattern_id=*/1, /*pages_fetched=*/1);
    clock.Advance(60 * kHalfLife);
    for (std::uint64_t id = 2; id <= stats::kMaxTrackedFingerprints; ++id) {
        signals.NoteExecution(id, 1);
    }
    clock.Advance(25 * kHalfLife);
    ASSERT_EQ(signals.tracked_fingerprints(), stats::kMaxTrackedFingerprints);

    const stats::OptimizerSnapshot before = signals.Snapshot();
    for (const stats::SnapshotFingerprint& fp : before.fingerprints) {
        ASSERT_EQ(fp.frequency_q8, 0u)
            << "fingerprint " << fp.pattern_id << " still has linear resolution; "
            << "the tie this test is about would not arise";
    }

    signals.NoteExecution(/*pattern_id=*/999'999, 1);

    const stats::OptimizerSnapshot after = signals.Snapshot();
    bool ancient_survived = false;
    for (const stats::SnapshotFingerprint& fp : after.fingerprints) {
        if (fp.pattern_id == 1) ancient_survived = true;
    }
    EXPECT_FALSE(ancient_survived)
        << "the eviction kept the entry idle 85 half-lives and dropped a fresher one";
}

// ---- The scripted workload (the acceptance's end-to-end half) -----------

class Instance {
public:
    Instance() : signals_(/*clock=*/nullptr, kHalfLife) {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        EXPECT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        cabins_.emplace();
        cabins_->set_signals(&signals_);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, &*cabins_);
        dispatcher_->set_optimizer_signals(&signals_);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    stats::OptimizerSignals& signals() { return signals_; }
    catalog::Catalog& catalog() { return boot_->catalog; }
    storage::InMemoryPageStore& store() { return store_; }

private:
    storage::InMemoryPageStore store_{kFirstUserPageId};
    stats::OptimizerSignals signals_;
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::CabinStore> cabins_;
    std::optional<CommandDispatcher> dispatcher_;
};

TEST(OptimizerSignalsTest, AScriptedWorkloadLandsInTheSnapshot) {
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE b (id int64, sym varchar, qty int64) BTREE").substr(0, 7),
              "CREATED");
    ASSERT_EQ(db.Run("CREATE CABIN ON b(sym)").substr(0, 7), "CREATED");
    const char* kSyms[] = {"aaa", "bbb", "aaa", "ccc"};
    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(db.Run(std::string("INSERT INTO b VALUES ('") + kSyms[i] + "', " +
                         std::to_string(i * 10) + ")")
                      .substr(0, 8),
                  "INSERTED");
    }

    // S1/S2: two executions of one shape. No clock anywhere, so every
    // signal is a raw count and the assertions are exact.
    const std::string scan = "SELECT * FROM b";
    db.Run(scan);
    db.Run(scan);

    // S3: three probes of one value. A *declared* Cabin records at n=1, so
    // the first probe is the one coverage miss (it records as it walks) and
    // the next two serve through C6 hints, which verify and so count no
    // failure.
    const std::string probe = "SELECT * FROM b WHERE sym = 'aaa'";
    db.Run(probe);
    db.Run(probe);
    db.Run(probe);

    const stats::OptimizerSnapshot snap = db.signals().Snapshot();

    auto fp = parser::FingerprintOf(scan);
    ASSERT_TRUE(fp.has_value());
    const stats::SnapshotFingerprint* scan_signal = nullptr;
    for (const stats::SnapshotFingerprint& s : snap.fingerprints) {
        if (s.pattern_id == fp->pattern_id) scan_signal = &s;
    }
    ASSERT_NE(scan_signal, nullptr) << "the scan's fingerprint was not tracked";
    EXPECT_EQ(scan_signal->frequency_q8, 2 * stats::kDecayScoreScale);
    // Four rows fit one page: each execution walked exactly one page, and
    // S2's decayed sum says so.
    EXPECT_EQ(scan_signal->pages_q8, 2 * stats::kDecayScoreScale);

    ASSERT_EQ(snap.cabins.size(), 1u);
    EXPECT_EQ(snap.cabins[0].lookups_q8, 3 * stats::kDecayScoreScale);
    EXPECT_EQ(snap.cabins[0].coverage_misses_q8, 1 * stats::kDecayScoreScale);
    EXPECT_EQ(snap.cabins[0].hint_failures_q8, 0u) << "a verified hint counted as a failure";

    // The S2 counter is operator-visible too: the same execution's pages
    // show in ANALYZE, which is where a number this model prices gets
    // sanity-checked by a human.
    const std::string analyzed = db.Run("ANALYZE " + scan);
    EXPECT_NE(analyzed.find(" pages="), std::string::npos) << analyzed;
}

// ---- PHY05: the runtime switch --------------------------------------------

TEST(OptimizerSignalsTest, SetCabinOptimizerTogglesAndShowMetaReports) {
    Instance db;
    EXPECT_NE(db.Run("SHOW META").find("cabin_optimizer=off"), std::string::npos);

    EXPECT_EQ(db.Run("SET CABIN_OPTIMIZER ON"), "OK cabin_optimizer=on");
    EXPECT_NE(db.Run("SHOW META").find("cabin_optimizer=on"), std::string::npos);

    // Both spellings read naturally on a terminal; both work.
    EXPECT_EQ(db.Run("SET CABIN_OPTIMIZER = OFF"), "OK cabin_optimizer=off");
    EXPECT_NE(db.Run("SHOW META").find("cabin_optimizer=off"), std::string::npos);

    EXPECT_EQ(db.Run("SET CABIN_OPTIMIZER maybe").substr(0, 3), "ERR");
    EXPECT_EQ(db.Run("SET CABIN_OPTIMIZER on off").substr(0, 3), "ERR");
}

// ---- PHY03's catalog half: the ownership tag ------------------------------

TEST(OptimizerSignalsTest, AnOptimizerOwnedCabinRowSurvivesARestartWithItsTag) {
    // PO1's ownership tag is `kCabinOriginAuto` - the value sys.cabins
    // reserved for "a future promotion pipeline", which the cabin
    // optimizer is. No new catalog format was needed; this pins that the
    // tag round-trips a restart, which is the whole of PHY03's stated
    // crash posture: the row persists as any Observational Cabin's does,
    // the memory-resident sets and controller state re-derive from
    // re-observation, and a BUILDING that nothing persisted is discarded
    // by construction.
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE b (id int64, sym varchar) BTREE").substr(0, 7), "CREATED");
    auto oid = db.catalog().FindTableOidByName("b");
    ASSERT_TRUE(oid.ok());

    auto created = db.catalog().CreateCabin(oid.value(), /*col_pos=*/1,
                                            catalog::kCabinOriginAuto);
    ASSERT_TRUE(created.ok()) << created.status().message();

    // A second Catalog over the same pages is what a restart looks like.
    catalog::Catalog reopened(db.store());
    auto rows = reopened.ListCabins();
    ASSERT_TRUE(rows.ok());
    bool found = false;
    for (const catalog::SysCabinRow& row : rows.value()) {
        if (row.cabin_id != created.value()) continue;
        found = true;
        EXPECT_EQ(row.origin, catalog::kCabinOriginAuto);
        EXPECT_EQ(row.rel_oid, oid.value());
        EXPECT_EQ(row.column_no, 1u);
    }
    EXPECT_TRUE(found) << "the optimizer-owned row did not survive the restart";
}

}  // namespace
}  // namespace kds::server
