#include "kds/stats/optimizer_signals.hpp"

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/sched/clock.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "view_text.hpp"

// The cabin optimizer's input signals (cabin-controller.md §II.2,
// workplan PHY01). Three claims carry the acceptance: decayed accumulation
// is exact where the decay contract says it is, a snapshot is an immutable
// versioned value, and the counters are correct under a scripted workload
// end to end - dispatcher in, snapshot out. Since DL-S1 two more
// (`docs/rules/rules.md` §3a): a note takes its own core's part and waits
// for nothing, and a switched-off controller collects nothing.

namespace kds::server {
namespace {

constexpr sched::MonoTimeNs kHalfLife = 600'000'000'000ULL;

TEST(OptimizerSignalsTest, ExecutionSignalKeepsADecayedMeanPair) {
    sched::ManualClock clock;
    stats::OptimizerSignals signals(&clock, kHalfLife);

    for (int i = 0; i < 4; ++i) signals.NoteExecution(/*core=*/0, /*pattern_id=*/7, /*pages=*/10);
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

    signals.NoteExecution(/*core=*/0, 9, 1);
    signals.NoteExecution(/*core=*/0, 3, 1);
    signals.NoteCabinLookup(/*core=*/0, 5, /*served=*/true);
    signals.NoteCabinLookup(/*core=*/0, 2, /*served=*/false);

    const stats::OptimizerSnapshot first = signals.Snapshot();
    EXPECT_EQ(first.version, 1u);
    EXPECT_EQ(first.decay_epoch, 1000u);
    ASSERT_EQ(first.fingerprints.size(), 2u);
    EXPECT_LT(first.fingerprints[0].pattern_id, first.fingerprints[1].pattern_id);
    ASSERT_EQ(first.cabins.size(), 2u);
    EXPECT_LT(first.cabins[0].cabin_id, first.cabins[1].cabin_id);

    // A snapshot is a value: later touches move the collector, never the
    // snapshot already taken.
    signals.NoteExecution(/*core=*/0, 9, 50);
    clock.Advance(17);
    const stats::OptimizerSnapshot second = signals.Snapshot();
    EXPECT_EQ(second.version, 2u);
    EXPECT_EQ(first.fingerprints[1].frequency_q8, stats::kDecayScoreScale)
        << "the first snapshot moved after it was taken";
    EXPECT_GT(second.fingerprints[1].frequency_q8, first.fingerprints[1].frequency_q8);
}

TEST(OptimizerSignalsTest, CabinSignalsFoldLookupsFailuresAndCoverage) {
    stats::OptimizerSignals signals(/*clock=*/nullptr, kHalfLife);

    signals.NoteCabinLookup(/*core=*/0, 11, /*served=*/true);
    signals.NoteCabinLookup(/*core=*/0, 11, /*served=*/true);
    signals.NoteCabinLookup(/*core=*/0, 11, /*served=*/false);  // coverage miss
    signals.NoteCabinHint(/*core=*/0, 11, /*ok=*/true);          // a success moves nothing
    signals.NoteCabinHint(/*core=*/0, 11, /*ok=*/false);

    const stats::OptimizerSnapshot snap = signals.Snapshot();
    ASSERT_EQ(snap.cabins.size(), 1u);
    EXPECT_EQ(snap.cabins[0].lookups_q8, 3 * stats::kDecayScoreScale);
    EXPECT_EQ(snap.cabins[0].coverage_misses_q8, 1 * stats::kDecayScoreScale);
    EXPECT_EQ(snap.cabins[0].hint_failures_q8, 1 * stats::kDecayScoreScale);
}

TEST(OptimizerSignalsTest, TheFingerprintTableEvictsItsColdestAtTheCap) {
    sched::ManualClock clock;
    stats::OptimizerSignals signals(&clock, kHalfLife);

    // Fill to the cap; make fingerprint 0 the unambiguously coldest. Folded
    // as a tick would, a part's worth at a time: a part holds what one core
    // noted since the last fold, and the fold is where the table evicts.
    for (std::uint64_t id = 0; id < stats::kMaxTrackedFingerprints; ++id) {
        signals.NoteExecution(/*core=*/0, id, 1);
        if (id != 0) signals.NoteExecution(/*core=*/0, id, 1);
        if (id % 512 == 511) signals.Fold();
    }
    signals.Fold();
    ASSERT_EQ(signals.tracked_fingerprints(), stats::kMaxTrackedFingerprints);
    ASSERT_EQ(signals.drops().full, 0u);

    // A newcomer evicts the coldest rather than being refused: the table
    // stays at the cap and the new fingerprint is tracked.
    signals.NoteExecution(/*core=*/0, 999'999, 1);
    signals.Fold();
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
    signals.NoteExecution(/*core=*/0, /*pattern_id=*/1, /*pages_fetched=*/1);
    clock.Advance(60 * kHalfLife);
    for (std::uint64_t id = 2; id <= stats::kMaxTrackedFingerprints; ++id) {
        signals.NoteExecution(/*core=*/0, id, 1);
        if (id % 512 == 0) signals.Fold();
    }
    signals.Fold();
    clock.Advance(25 * kHalfLife);
    ASSERT_EQ(signals.tracked_fingerprints(), stats::kMaxTrackedFingerprints);

    const stats::OptimizerSnapshot before = signals.Snapshot();
    for (const stats::SnapshotFingerprint& fp : before.fingerprints) {
        ASSERT_EQ(fp.frequency_q8, 0u)
            << "fingerprint " << fp.pattern_id << " still has linear resolution; "
            << "the tie this test is about would not arise";
    }

    signals.NoteExecution(/*core=*/0, /*pattern_id=*/999'999, 1);

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
    // One switch, the dispatcher's and the collector's, as an instance wires
    // it; on unless a test asks for the shipped default.
    explicit Instance(bool optimizer_on = true, bool gate_collector = true)
        : signals_(/*clock=*/nullptr, kHalfLife), on_(optimizer_on) {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        EXPECT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        cabins_.emplace();
        cabins_->set_signals(&signals_);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, &*cabins_);
        if (gate_collector) signals_.set_gate(&on_);
        OptimizerSurface surface;
        surface.signals = &signals_;
        surface.cabin_optimizer_on = &on_;
        dispatcher_->set_optimizer_surface(surface);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    stats::OptimizerSignals& signals() { return signals_; }
    stats::CabinStore& cabins() { return *cabins_; }
    catalog::Catalog& catalog() { return boot_->catalog; }
    storage::InMemoryPageStore& store() { return store_; }

private:
    storage::InMemoryPageStore store_{kFirstUserPageId};
    stats::OptimizerSignals signals_;
    std::atomic<bool> on_;
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

// AP-S2 moved the trail to fetch_id and left this model on pattern_id: a
// `SUM(x)` and an `x` over one fetch are one trail and two plans, and the
// cabin optimizer prices plans. Two select lists over one fetch are two
// fingerprints here.
TEST(OptimizerSignalsTest, TwoSelectListsOverOneFetchAreTwoFingerprints) {
    Instance db;
    ASSERT_EQ(db.Run("CREATE TABLE b (id int64, qty int64) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(db.Run("INSERT INTO b VALUES (5)").substr(0, 8), "INSERTED");

    const std::string plain = "SELECT qty FROM b WHERE id = 1";
    const std::string folded = "SELECT SUM(qty) FROM b WHERE id = 1";
    auto plain_fp = parser::FingerprintOf(plain);
    auto folded_fp = parser::FingerprintOf(folded);
    ASSERT_TRUE(plain_fp.has_value() && folded_fp.has_value());
    ASSERT_EQ(plain_fp->fetch_id, folded_fp->fetch_id);
    db.Run(plain);
    db.Run(folded);
    db.Run(folded);

    std::uint64_t plain_q8 = 0, folded_q8 = 0;
    for (const stats::SnapshotFingerprint& s : db.signals().Snapshot().fingerprints) {
        if (s.pattern_id == plain_fp->pattern_id) plain_q8 = s.frequency_q8;
        if (s.pattern_id == folded_fp->pattern_id) folded_q8 = s.frequency_q8;
    }
    EXPECT_EQ(plain_q8, 1 * stats::kDecayScoreScale);
    EXPECT_EQ(folded_q8, 2 * stats::kDecayScoreScale);
}

// ---- PHY05: the runtime switch --------------------------------------------

TEST(OptimizerSignalsTest, SetCabinOptimizerTogglesAndShowMetaReports) {
    Instance db(/*optimizer_on=*/false);
    EXPECT_EQ(testing_view::MetaField(db.Run("SHOW META"), "cabin_optimizer"), "off");

    EXPECT_EQ(db.Run("SET CABIN_OPTIMIZER ON"), "OK cabin_optimizer=on");
    EXPECT_EQ(testing_view::MetaField(db.Run("SHOW META"), "cabin_optimizer"), "on");

    // Both spellings read naturally on a terminal; both work.
    EXPECT_EQ(db.Run("SET CABIN_OPTIMIZER = OFF"), "OK cabin_optimizer=off");
    EXPECT_EQ(testing_view::MetaField(db.Run("SHOW META"), "cabin_optimizer"), "off");

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
    auto oid = db.catalog().FindTableOidByName(::kds::catalog::kNamespacePublic, "b");
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


// ---- DL-S1: the two rules (`docs/rules/rules.md` §3a) ------------------------

// **A switched-off consumer collects nothing.** The shipped default: the same
// scripted workload, the controller off. No shape and no Cabin reaches the
// collector, no note was even tried (nothing dropped), and turning the
// controller on starts it from no evidence - the next statement is the
// first it has seen. The Cabin store's own counts, which `SHOW CABINS`
// reads and no switch turns off, are taken all the same.
TEST(OptimizerSignalsTest, ASwitchedOffControllerCollectsNothing) {
    Instance db(/*optimizer_on=*/false);
    ASSERT_EQ(db.Run("CREATE TABLE b (id int64, sym varchar, qty int64) BTREE").substr(0, 7),
              "CREATED");
    ASSERT_EQ(db.Run("CREATE CABIN ON b(sym)").substr(0, 7), "CREATED");
    ASSERT_EQ(db.Run("INSERT INTO b VALUES ('aaa', 1)").substr(0, 8), "INSERTED");
    const std::string scan = "SELECT * FROM b";
    const std::string probe = "SELECT * FROM b WHERE sym = 'aaa'";
    for (int i = 0; i < 3; ++i) {
        db.Run(scan);
        db.Run(probe);
    }

    stats::OptimizerSnapshot snap = db.signals().Snapshot();
    EXPECT_TRUE(snap.fingerprints.empty()) << "a shape was noted for a controller that is off";
    EXPECT_TRUE(snap.cabins.empty()) << "a Cabin probe was forwarded to a controller that is off";
    EXPECT_EQ(db.signals().drops().busy + db.signals().drops().full, 0u);
    EXPECT_EQ(testing_view::MetaField(db.Run("SHOW META"), "cabin_optimizer_notes_full"), "0");
    const stats::CabinStore::Stats counted = db.cabins().stats();
    EXPECT_EQ(counted.hits + counted.misses, 3u) << "the store's own probe counts have no switch";

    // On: from no evidence. One run of each is one execution of each.
    ASSERT_EQ(db.Run("SET CABIN_OPTIMIZER ON"), "OK cabin_optimizer=on");
    db.Run(scan);
    db.Run(probe);
    snap = db.signals().Snapshot();
    ASSERT_EQ(snap.fingerprints.size(), 2u);
    for (const stats::SnapshotFingerprint& fp : snap.fingerprints) {
        EXPECT_EQ(fp.frequency_q8, 1 * stats::kDecayScoreScale) << "evidence from the off period";
    }
    ASSERT_EQ(snap.cabins.size(), 1u);
    EXPECT_EQ(snap.cabins[0].lookups_q8, 1 * stats::kDecayScoreScale);

    // Off again: what was folded stays (DL-Q13 (a)) and nothing is added.
    ASSERT_EQ(db.Run("SET CABIN_OPTIMIZER OFF"), "OK cabin_optimizer=off");
    db.Run(scan);
    db.Run(probe);
    snap = db.signals().Snapshot();
    ASSERT_EQ(snap.fingerprints.size(), 2u);
    for (const stats::SnapshotFingerprint& fp : snap.fingerprints) {
        EXPECT_EQ(fp.frequency_q8, 1 * stats::kDecayScoreScale);
    }
}

// The dispatcher's own gate, told apart from the collector's: the collector
// is left ungated here - it would take any note it was handed - and the
// dispatcher's switch is off. No statement hands it one: no identity is
// built and no execution noted on its behalf. The Cabin store's forward is
// the collector's to gate, and with no gate it lands - which is what shows
// the workload ran.
TEST(OptimizerSignalsTest, TheDispatcherBuildsNothingForASwitchedOffController) {
    Instance db(/*optimizer_on=*/false, /*gate_collector=*/false);
    ASSERT_EQ(db.Run("CREATE TABLE b (id int64, sym varchar, qty int64) BTREE").substr(0, 7),
              "CREATED");
    ASSERT_EQ(db.Run("CREATE CABIN ON b(sym)").substr(0, 7), "CREATED");
    ASSERT_EQ(db.Run("INSERT INTO b VALUES ('aaa', 1)").substr(0, 8), "INSERTED");
    for (int i = 0; i < 3; ++i) {
        db.Run("SELECT * FROM b");
        db.Run("SELECT * FROM b WHERE sym = 'aaa'");
    }
    const stats::OptimizerSnapshot snap = db.signals().Snapshot();
    EXPECT_TRUE(snap.fingerprints.empty())
        << "the dispatcher noted an execution for a controller that is off";
    ASSERT_EQ(snap.cabins.size(), 1u);
    EXPECT_EQ(snap.cabins[0].lookups_q8, 3 * stats::kDecayScoreScale);
}

// **A note waits for nothing.** The collector is the instance's and a part a
// core: a note takes its own core's part by a try, and the one other taker
// is the tick's fold. Core 0's part is held here the way the fold holds it,
// for as long as the test likes: a note of core 0's on a second thread
// returns under the hold, dropped and counted, and a note into core 1's
// part lands as if nothing were held. **Mutation**: make `Enter` wait (the
// blocking `LatchGuard`) and the peer does not return under the hold.
TEST(OptimizerSignalsTest, ANoteNeverWaitsForAHeldPartAndIsCountedWhereItIsLost) {
    stats::OptimizerSignals signals(/*clock=*/nullptr, kHalfLife, /*cores=*/2);
    Latch* part0 = signals.PartLatchForTest(0);
    ASSERT_NE(part0, nullptr);

    std::atomic<bool> returned{false};
    std::optional<LatchGuard> held(std::in_place, part0);
    std::thread peer([&] {
        signals.NoteExecution(/*core=*/0, /*pattern_id=*/7, /*pages=*/1);
        signals.NoteCabinLookup(/*core=*/0, /*cabin_id=*/3, /*served=*/true);
        signals.NoteWaitSample(/*core=*/0, /*pattern_id=*/7);
        signals.NoteCabinHint(/*core=*/0, /*cabin_id=*/3, /*ok=*/false);
        signals.NoteExecution(/*core=*/1, /*pattern_id=*/9, /*pages=*/1);
        returned.store(true);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!returned.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool under_the_hold = returned.load();
    held.reset();  // let a peer that did wait finish, so the join returns
    peer.join();
    ASSERT_TRUE(under_the_hold) << "a note waited for its part's latch";

    EXPECT_EQ(signals.drops().busy, 4u) << "each note that met the hold is counted";
    EXPECT_EQ(signals.drops().full, 0u);
    const stats::OptimizerSnapshot snap = signals.Snapshot();
    ASSERT_EQ(snap.fingerprints.size(), 1u) << "core 1's note was not touched by core 0's hold";
    EXPECT_EQ(snap.fingerprints[0].pattern_id, 9u);
    EXPECT_TRUE(snap.cabins.empty());
}

// Notes and folds side by side: every note either lands or is counted as
// lost to a fold's hold, none twice and none nowhere.
TEST(OptimizerSignalsTest, EveryNoteBesideAFoldLandsOrIsCounted) {
    sched::ManualClock clock;
    stats::OptimizerSignals signals(&clock, /*half_life_ns=*/1'000'000'000, /*cores=*/2);
    signals.NoteCabinLookup(/*core=*/0, /*cabin_id=*/7, /*served=*/true);

    std::atomic<int> stage{0};
    std::thread peer([&] {
        while (stage.load() != 1) std::this_thread::yield();
        for (int i = 0; i < 20000; ++i) {
            signals.NoteCabinLookup(/*core=*/0, /*cabin_id=*/7, /*served=*/true);
            signals.NoteExecution(/*core=*/1, /*pattern_id=*/5, /*pages=*/1);
        }
        stage.store(2);
    });
    stage.store(1);
    std::uint64_t folds = 0;
    while (stage.load() != 2) {
        signals.Fold();
        ++folds;
    }
    peer.join();

    const stats::OptimizerSnapshot snap = signals.Snapshot();
    ASSERT_EQ(snap.cabins.size(), 1u);
    ASSERT_EQ(snap.fingerprints.size(), 1u);
    const stats::OptimizerSignals::Drops lost = signals.drops();
    EXPECT_EQ(lost.full, 0u);
    // The clock is parked, so a merge is an exact sum: 256 a note, and
    // 40,001 notes stay far below the saturating ceiling.
    const std::uint64_t landed =
        (snap.cabins[0].lookups_q8 + snap.fingerprints[0].frequency_q8) / stats::kDecayScoreScale;
    EXPECT_EQ(landed + lost.busy, 40001u) << "folds=" << folds;
}

// A full table makes room for a part's newcomers in one pass: the coldest
// go together, however many arrive, and the survivors are the hottest.
TEST(OptimizerSignalsTest, AFoldEvictsTheColdestForEveryNewcomerOfAPartAtOnce) {
    sched::ManualClock clock(1);
    stats::OptimizerSignals signals(&clock, kHalfLife);
    // Ids 1..cap, each touched (id) times modulo a small spread, so lower
    // ids are colder: id i gets 1 + i / 512 touches.
    for (std::uint64_t id = 1; id <= stats::kMaxTrackedFingerprints; ++id) {
        for (std::uint64_t t = 0; t <= id / 512; ++t) signals.NoteExecution(/*core=*/0, id, 1);
        if (id % 512 == 0) signals.Fold();
    }
    signals.Fold();
    ASSERT_EQ(signals.tracked_fingerprints(), stats::kMaxTrackedFingerprints);

    // 300 newcomers in one part, each hotter than the coldest band.
    for (std::uint64_t n = 0; n < 300; ++n) {
        for (int t = 0; t < 4; ++t) signals.NoteExecution(/*core=*/0, 1'000'000 + n, 1);
    }
    const stats::OptimizerSnapshot snap = signals.Snapshot();
    EXPECT_EQ(snap.fingerprints.size(), stats::kMaxTrackedFingerprints);
    std::size_t newcomers = 0;
    std::size_t coldest_band = 0;  // ids 1..511: one touch each
    for (const stats::SnapshotFingerprint& fp : snap.fingerprints) {
        if (fp.pattern_id >= 1'000'000) ++newcomers;
        if (fp.pattern_id < 512) ++coldest_band;
    }
    EXPECT_EQ(newcomers, 300u) << "a newcomer was refused or evicted by a later one";
    // The 300 victims are the lowest ids of the one-touch band (a tie goes
    // to the lower key): 211 of its 511 are left.
    EXPECT_EQ(coldest_band, 211u);
}

// A note names its core, and a core the collector has no part for is
// dropped and counted - never written into another core's part.
TEST(OptimizerSignalsTest, ANoteForACoreWithNoPartIsDroppedAndCounted) {
    stats::OptimizerSignals signals(/*clock=*/nullptr, kHalfLife, /*cores=*/2);
    signals.NoteExecution(/*core=*/5, /*pattern_id=*/1, /*pages=*/1);
    EXPECT_TRUE(signals.Snapshot().fingerprints.empty());
    EXPECT_EQ(signals.drops().full, 1u);
}

// What one core notes between two folds is bounded: past it a new shape is
// dropped and counted, a shape the part already holds still counts, and the
// fold makes room.
TEST(OptimizerSignalsTest, APartPastItsBoundDropsNewShapesUntilTheFold) {
    stats::OptimizerSignals signals(/*clock=*/nullptr, kHalfLife);
    const std::uint64_t bound = stats::OptimizerSignals::kNotedShapesPerCore;
    for (std::uint64_t id = 1; id <= bound; ++id) signals.NoteExecution(/*core=*/0, id, 1);
    EXPECT_EQ(signals.drops().full, 0u);
    signals.NoteExecution(/*core=*/0, bound + 1, 1);
    signals.NoteExecution(/*core=*/0, /*pattern_id=*/1, 1);
    EXPECT_EQ(signals.drops().full, 1u);
    signals.Fold();
    EXPECT_EQ(signals.tracked_fingerprints(), bound);
    signals.NoteExecution(/*core=*/0, bound + 1, 1);
    signals.Fold();
    EXPECT_EQ(signals.tracked_fingerprints(), bound + 1);
    EXPECT_EQ(signals.drops().full, 1u);
}

// Two cores' notes of one shape fold to what one core's would have been:
// decay is linear, so the parts' pairs merge exactly at whole half-lives.
TEST(OptimizerSignalsTest, TwoCoresNotesOfOneShapeFoldAsOne) {
    sched::ManualClock clock;
    stats::OptimizerSignals split(&clock, kHalfLife, /*cores=*/2);
    stats::OptimizerSignals one(&clock, kHalfLife);
    for (int i = 0; i < 4; ++i) {
        split.NoteExecution(/*core=*/static_cast<std::uint32_t>(i % 2), /*pattern_id=*/7, /*pages=*/10);
        one.NoteExecution(/*core=*/0, /*pattern_id=*/7, /*pages=*/10);
    }
    clock.Advance(kHalfLife);
    split.NoteExecution(/*core=*/1, 7, 10);
    one.NoteExecution(/*core=*/0, 7, 10);
    clock.Advance(kHalfLife);
    const stats::OptimizerSnapshot a = split.Snapshot();
    const stats::OptimizerSnapshot b = one.Snapshot();
    ASSERT_EQ(a.fingerprints.size(), 1u);
    ASSERT_EQ(b.fingerprints.size(), 1u);
    EXPECT_EQ(a.fingerprints[0].frequency_q8, b.fingerprints[0].frequency_q8);
    EXPECT_EQ(a.fingerprints[0].pages_q8, b.fingerprints[0].pages_q8);
    // 4 executions halved twice, 1 halved once: 1.5.
    EXPECT_EQ(b.fingerprints[0].frequency_q8, 3 * stats::kDecayScoreScale / 2);
}

// ---- S4: the ASH wait signal (CJ's WE13, [EXPERIMENTAL]) ---------------------

TEST(OptimizerSignalsTest, WaitSamplesCountOnlyTrackedShapesAndOnlyWhileTheSwitchIsOn) {
    sched::ManualClock clock;
    stats::OptimizerSignals signals(&clock, kHalfLife);
    std::atomic<bool> on{true};
    signals.set_gate(&on);
    signals.NoteExecution(/*core=*/0, 7, 10);
    signals.NoteWaitSample(/*core=*/0, 7);
    signals.NoteWaitSample(/*core=*/0, 7);
    signals.NoteWaitSample(/*core=*/0, 99);  // never executed: admitted nowhere
    on.store(false);
    signals.NoteWaitSample(/*core=*/0, 7);  // switch off: not counted
    const stats::OptimizerSnapshot snap = signals.Snapshot();
    ASSERT_EQ(snap.fingerprints.size(), 1u) << "a wait sample admits no shape";
    EXPECT_EQ(snap.fingerprints[0].wait_samples_q8, 2 * stats::kDecayScoreScale);
}

TEST(OptimizerSignalsTest, TheWaitSignalMovesNothingTheControllerReads) {
    // Two collectors fed the same executions, one also fed waits: every field a
    // decision reads is identical.
    sched::ManualClock clock;
    stats::OptimizerSignals plain(&clock, kHalfLife), waited(&clock, kHalfLife);
    for (auto* s : {&plain, &waited}) {
        s->NoteExecution(/*core=*/0, 5, 3);
        s->NoteExecution(/*core=*/0, 5, 3);
    }
    for (int i = 0; i < 100; ++i) waited.NoteWaitSample(/*core=*/0, 5);
    const auto a = plain.Snapshot().fingerprints.at(0);
    const auto b = waited.Snapshot().fingerprints.at(0);
    EXPECT_EQ(a.frequency_q8, b.frequency_q8);
    EXPECT_EQ(a.pages_q8, b.pages_q8);
    EXPECT_EQ(a.frequency_log2, b.frequency_log2);
    EXPECT_EQ(a.pages_log2, b.pages_log2);
    EXPECT_EQ(a.wait_samples_q8, 0u);
    EXPECT_GT(b.wait_samples_q8, 0u);
}

}  // namespace
}  // namespace kds::server
