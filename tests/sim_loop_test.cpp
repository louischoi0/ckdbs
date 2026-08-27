#include "sim/loop.hpp"

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "sim/faults.hpp"
#include "sim/instance.hpp"
#include "sim/integrity.hpp"
#include "sim/minimize.hpp"
#include "sim/oracle.hpp"
#include "sim/reply.hpp"
#include "sim/rng.hpp"
#include "sim/workload.hpp"

#include "kds/catalog/catalog.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/tagged_cell.hpp"
#include "kds/storage/varheap.hpp"

// The simulation harness's own regression suite (bench/workplan-teststrategy
// SIM01-SIM04). The committed seed corpus in tests/testdata/sim_seeds.txt is
// regression-mandatory: a seed added there runs here forever, and removing
// one takes the same justification as deleting a test.

namespace kds::sim {
namespace {

std::vector<std::uint64_t> CommittedSeeds() {
    std::ifstream in(std::string(KDS_SOURCE_DIR) + "/tests/testdata/sim_seeds.txt");
    std::vector<std::uint64_t> seeds;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        seeds.push_back(std::stoull(line));
    }
    return seeds;
}

// ---- SIM01/SIM03: determinism ---------------------------------------------

std::vector<std::string> OpLog(std::uint64_t seed, std::size_t n) {
    Workload workload(Rng(seed).Fork("workload"), Profile::kUniform);
    std::vector<std::string> log;
    log.reserve(n);
    for (std::size_t i = 0; i < n; ++i) log.push_back(workload.Next().sql);
    return log;
}

TEST(SimWorkload, SameSeedYieldsTheByteIdenticalOperationLog) {
    EXPECT_EQ(OpLog(42, 2000), OpLog(42, 2000));
}

TEST(SimWorkload, DifferentSeedsYieldDifferentOperationLogs) {
    EXPECT_NE(OpLog(42, 2000), OpLog(43, 2000));
}

// The corpus must keep both clustered types inside the tested surface; a
// reseeding that loses one would silently halve what the loop covers.
TEST(SimWorkload, TheCommittedCorpusCoversBothClusteredTypes) {
    bool saw_heap = false, saw_btree = false;
    for (const std::uint64_t seed : CommittedSeeds()) {
        Workload workload(Rng(seed).Fork("iteration/0").Fork("workload"), Profile::kUniform);
        for (int i = 0; i < 4; ++i) {
            const Op op = workload.Next();
            if (op.kind != Op::Kind::kCreateTable) break;
            (op.btree ? saw_btree : saw_heap) = true;
        }
    }
    EXPECT_TRUE(saw_heap);
    EXPECT_TRUE(saw_btree);
}

// ---- SIM03/SIM04: the loop over the committed corpus ----------------------

TEST(SimLoop, TenThousandOpCleanRunAgreesWithTheOracleOnEveryRead) {
    SimConfig config;
    config.seed = 1;
    config.ops = 10000;
    config.mode = SimMode::kClean;
    const SimVerdict verdict = RunSimulation(config);
    EXPECT_TRUE(verdict.ok) << verdict.Summary(config);
    EXPECT_GT(verdict.reads_checked, 1000u);
    EXPECT_EQ(verdict.gated_missing_rows, 0u);
}

TEST(SimLoop, CleanRunsHoldOnEveryCommittedSeed) {
    for (const std::uint64_t seed : CommittedSeeds()) {
        SimConfig config;
        config.seed = seed;
        config.ops = 2000;
        config.mode = SimMode::kClean;
        const SimVerdict verdict = RunSimulation(config);
        EXPECT_TRUE(verdict.ok) << verdict.Summary(config);
        EXPECT_EQ(verdict.gated_missing_rows, 0u) << verdict.Summary(config);
    }
}

TEST(SimLoop, SyncCrashKeepsEverySyncedRowOnEveryCommittedSeed) {
    for (const std::uint64_t seed : CommittedSeeds()) {
        SimConfig config;
        config.seed = seed;
        config.ops = 1500;
        config.mode = SimMode::kSyncCrash;
        config.iterations = 3;
        const SimVerdict verdict = RunSimulation(config);
        EXPECT_TRUE(verdict.ok) << verdict.Summary(config);
    }
}

TEST(SimLoop, CrashAnywhereFabricatesNothingOnEveryCommittedSeed) {
    for (const std::uint64_t seed : CommittedSeeds()) {
        for (const Profile profile :
             {Profile::kUniform, Profile::kZipfian, Profile::kColliding}) {
            SimConfig config;
            config.seed = seed;
            config.ops = 1500;
            config.mode = SimMode::kCrash;
            config.profile = profile;
            config.iterations = 3;
            const SimVerdict verdict = RunSimulation(config);
            EXPECT_TRUE(verdict.ok) << verdict.Summary(config);
        }
    }
}

// A run long enough to roll a WAL segment and to grow a var-heap chain, which
// **the corpus above never does**: at 1500 ops the stream stays inside its
// first 1 MiB segment and a spilled value rarely fills a var-heap page. Two
// recovery defects lived in exactly that gap and the suite was green over both
// of them (2026-08-12):
//
//   - a VARHEAP_APPEND naming a page no PAGE_INIT created, because var-heap
//     growth was unlogged: the mount **refused**;
//   - a segment sealed with no room for a PAD read as a torn tail, so every
//     record in every later segment was dropped: rows **missing** after the
//     restart.
//
// Seed 24 at 3500 ops is where the second one was caught. One seed and one
// iteration, kept cheap on purpose - what this guards is the two boundaries,
// and the breadth is the corpus's job.
TEST(SimLoop, ALongRunRollsASegmentAndStillRecoversEveryAcknowledgedRow) {
    SimConfig config;
    config.seed = 24;
    config.ops = 3500;
    config.mode = SimMode::kCrash;
    config.iterations = 1;
    const SimVerdict verdict = RunSimulation(config);
    EXPECT_TRUE(verdict.ok) << verdict.Summary(config);
    EXPECT_EQ(verdict.gated_missing_rows, 0u);
}

// A clean shutdown has to publish an anchor, or the next mount re-reads
// everything the last run wrote.
//
// RC08 bounded the mount after a *crash* by checkpointing at the end of
// recovery. A graceful stop had no equivalent: it synced and left the anchor
// wherever the last cadence tick put it, so the first mount afterwards rescanned
// every record since - measured as a cleanly stopped 2000-row instance re-reading
// all 10,883 of its own records (`bench/results-wal-recovery.md`). The harness
// runs no cadence checkpointer at all, which makes it the sharpest place to
// assert this: without the shutdown checkpoint the anchor here would still be the
// *mount's* one, and every row written after it would be rescanned.
TEST(SimInstanceTest, AMountAfterACleanStopDoesNotRereadTheRunsWholeLog) {
    auto instance = SimInstance::Create();
    ASSERT_TRUE(instance.ok()) << instance.status().message();
    SimInstance& db = *instance.value();

    ASSERT_EQ(db.Execute("CREATE TABLE t (id int64, v int64)").rfind("CREATED", 0), 0u);
    for (int i = 0; i < 200; ++i) {
        const std::string reply = db.Execute("INSERT INTO t VALUES (" + std::to_string(i) + ")");
        ASSERT_EQ(reply.rfind("INSERTED", 0), 0u) << reply;
    }

    ASSERT_TRUE(db.CleanShutdown().ok());
    ASSERT_TRUE(db.Reboot().ok());

    // The 200 inserts are below the shutdown checkpoint's anchor, so the mount
    // sees only what followed it: the checkpoint's own two records. The exact
    // number is not the property - "far fewer than were written" is - so this
    // asserts the bound rather than the constant.
    EXPECT_LT(db.recovery().records, 20u)
        << "the mount re-read " << db.recovery().records
        << " records after a clean stop, so the shutdown published no usable anchor";
    EXPECT_EQ(db.recovery().redo_applied, 0u)
        << "a cleanly stopped instance has nothing to redo";

    // And the rows are all there, which is what makes the cheap mount honest
    // rather than a mount that skipped work it owed.
    const std::string count = db.Execute("SELECT COUNT(*) FROM t");
    EXPECT_NE(count.find("200"), std::string::npos) << count;
}

// The durability assertion must be able to fire — a gate that cannot fail is
// not a gate (docs/workplan-wal-recovery.md RC10).
//
// **How this test had to change when recovery landed.** It used to run seed 4
// with the gate off, assert that the seed lost acknowledged rows, then arm the
// gate and watch the same run fail. That premise is gone: with recovery
// running at mount, seed 4 loses nothing, and the old test failed on its own
// `ASSERT_GT(gated_missing_rows, 0)` — "this seed no longer loses rows; pick
// one that does or the gate test is vacuous". Which was the harness correctly
// reporting that the engine had improved underneath it.
//
// So the violating image is hand-fed now, per RC10: `skip_recovery` boots the
// same crashed devices *without* the phase, which is exactly the engine as it
// stood before RV1 — and the armed assertion must fail on it, naming rows.
// The pair is what carries the proof: same seed, same crash, recovery the only
// difference.
TEST(SimLoop, TheDurabilityAssertionFiresOnARecoverylessBoot) {
    SimConfig armed;
    armed.seed = 4;
    armed.ops = 500;
    armed.mode = SimMode::kCrash;
    armed.iterations = 3;
    armed.assert_recovery = true;

    const SimVerdict recovered = RunSimulation(armed);
    EXPECT_TRUE(recovered.ok) << recovered.Summary(armed);
    EXPECT_EQ(recovered.gated_missing_rows, 0u);

    SimConfig without = armed;
    without.skip_recovery = true;
    const SimVerdict fired = RunSimulation(without);
    ASSERT_FALSE(fired.ok) << "the assertion cannot fail, so it proves nothing";
    EXPECT_NE(fired.detail.find("missing"), std::string::npos) << fired.detail;
}

// ---- SIM02: each corruption is caught by exactly its category -------------

class SimIntegrityCorruption : public ::testing::Test {
protected:
    void SetUp() override {
        auto instance = SimInstance::Create();
        ASSERT_TRUE(instance.ok()) << instance.status().message();
        instance_ = std::move(instance.value());
    }

    // A short varchar stays inline; ~200 bytes spills. The two tables give
    // the heap and btree walks one relation each.
    void MakeTables() {
        ASSERT_EQ(instance_->Execute("CREATE TABLE h (id int64, v int64, name varchar) HEAP")
                      .substr(0, 7),
                  "CREATED");
        ASSERT_EQ(instance_->Execute("CREATE TABLE b (id int64, v int64, name varchar) BTREE")
                      .substr(0, 7),
                  "CREATED");
    }

    InsertedAt Insert(const std::string& table, std::int64_t v, const std::string& name) {
        const std::string reply = instance_->Execute("INSERT INTO " + table + " VALUES (" +
                                                     std::to_string(v) + ", '" + name + "')");
        auto at = ParseInserted(reply);
        EXPECT_TRUE(at.has_value()) << reply;
        return at.value_or(InsertedAt{});
    }

    // The tuple's stored bytes: header (20 B) then payload. Returned as a
    // mutable pointer into the resident frame.
    std::byte* TupleBase(const InsertedAt& at) {
        auto page = instance_->store().Get(at.page);
        EXPECT_TRUE(page.ok());
        heap::PageView view(page.value().bytes());
        auto tuple = view.ReadTuple(at.slot);
        EXPECT_TRUE(tuple.ok());
        const std::byte* payload = tuple.value().payload.data();
        return page.value().bytes().data() + (payload - page.value().bytes().data()) -
               heap::kTupleHeaderOnDiskSize;
    }

    IntegrityReport Check() {
        return CheckInstance(instance_->store(), instance_->catalog());
    }

    // Exactly one category fires, and it is `kind`.
    void ExpectOnly(const IntegrityReport& report, CheckKind kind) {
        EXPECT_EQ(report.CountOf(kind), report.findings.size()) << report.Summary();
        EXPECT_GE(report.CountOf(kind), 1u) << report.Summary();
    }

    std::unique_ptr<SimInstance> instance_;
};

TEST_F(SimIntegrityCorruption, AValidInstancePassesWithCoverage) {
    MakeTables();
    for (int i = 0; i < 20; ++i) {
        Insert("h", i, "short");
        Insert("b", i, std::string(200, 'x'));
    }
    const IntegrityReport report = Check();
    EXPECT_TRUE(report.ok()) << report.Summary();
    EXPECT_EQ(report.relations_swept, 2u);
    EXPECT_GT(report.pages_swept, 0u);
    EXPECT_GE(report.tuples_swept, 40u);
}

TEST_F(SimIntegrityCorruption, NonzeroKeystoneReservedBitsAreAKeystoneFinding) {
    MakeTables();
    const InsertedAt at = Insert("h", 7, "short");
    // reserved is the top 16 bits of the word: bytes 6-7 little-endian.
    std::byte* keystone = TupleBase(at) + heap::kTupleHeaderOnDiskSize;
    keystone[6] = std::byte{0xAB};
    ExpectOnly(Check(), CheckKind::kKeystone);
}

TEST_F(SimIntegrityCorruption, ANeverIssuedTrxIdIsATrxIdFinding) {
    MakeTables();
    const InsertedAt at = Insert("h", 7, "short");
    const std::uint64_t huge = (1ull << 47);
    std::memcpy(TupleBase(at), &huge, sizeof huge);  // trx_id is header offset 0
    ExpectOnly(Check(), CheckKind::kTrxId);
}

TEST_F(SimIntegrityCorruption, AnImplausibleUndoPtrIsAnUndoPtrFinding) {
    MakeTables();
    const InsertedAt at = Insert("h", 7, "short");
    // Offset 5 is inside the undo page header — structurally impossible.
    const std::uint64_t bogus = (3ull << 16) | 5;
    std::memcpy(TupleBase(at) + 8, &bogus, sizeof bogus);  // undo_ptr is header offset 8
    ExpectOnly(Check(), CheckKind::kUndoPtr);
}

TEST_F(SimIntegrityCorruption, AnIdBelowItsPageMinKeyIsAChainOrderFinding) {
    MakeTables();
    InsertedAt first = Insert("h", 0, "pad");
    InsertedAt victim{};
    // Grow the chain until a tuple lands on a second page; its min_key is
    // the id that caused the growth, so id 1 is below it by construction.
    for (int i = 1; i < 300 && victim.page == 0; ++i) {
        const InsertedAt at = Insert("h", i, "pad");
        if (at.page != first.page) victim = at;
    }
    ASSERT_NE(victim.page, 0u) << "chain never grew";
    const std::uint64_t low_id = 1;  // valid Keystone encoding, wrong page
    std::memcpy(TupleBase(victim) + heap::kTupleHeaderOnDiskSize, &low_id, sizeof low_id);
    ExpectOnly(Check(), CheckKind::kChainOrder);
}

TEST_F(SimIntegrityCorruption, ASpilledCellPointingOffChainIsAVarHeapFinding) {
    MakeTables();
    const InsertedAt at = Insert("h", 7, std::string(200, 'y'));  // spills

    // Named, not iterated straight off the call: `StatusOr::value()` returns
    // a reference, so binding a range-for to it leaves the temporary
    // StatusOr - and the vector inside it - dead before the first
    // iteration (ASan: stack-use-after-scope). C++23 extends the
    // temporary's life here; C++20 does not.
    auto tables = instance_->catalog().ListTables();
    ASSERT_TRUE(tables.ok());
    catalog::Oid h_oid = 0;
    for (const auto& row : tables.value()) {
        if (catalog::NameView(row.name) == "h") h_oid = row.oid;
    }
    ASSERT_NE(h_oid, 0u);
    auto access = instance_->catalog().InitTableAccess(h_oid);
    ASSERT_TRUE(access.ok());
    // The varchar column's cell offset from the layout — column 2.
    const std::uint32_t cell_offset = access.value()->layout.offsets[2];

    std::byte* cell = TupleBase(at) + heap::kTupleHeaderOnDiskSize + cell_offset;
    ASSERT_EQ(cell[0], std::byte{2}) << "expected a spilled cell";
    const std::uint64_t bogus_ptr = varheap::EncodePtr(varheap::VarHeapPtr{3, 0});
    std::memcpy(cell + storage::kCellSpilledPtrOffset, &bogus_ptr, sizeof bogus_ptr);
    ExpectOnly(Check(), CheckKind::kVarHeap);
}

// The sweep's category, proved on a **recoveryless** boot (RC10's fault
// injection): the flush runs in page-id order, so the torn write lands on
// a catalog page - and since RV3 logs catalog mutations, a *recovered*
// mount no longer serves that page at all (the test below pins that).
// Skipping recovery is what still boots the corrupt store the sweep needs.
TEST_F(SimIntegrityCorruption, ATornPageWriteSurfacesAsAPageHeaderFinding) {
    auto without = SimInstance::Create({.skip_recovery = true});
    ASSERT_TRUE(without.ok()) << without.status().message();
    instance_ = std::move(without.value());
    MakeTables();
    for (int i = 0; i < 20; ++i) Insert("h", i, "short");

    // The next page write reaches the platter half-done, then power is
    // lost. On the rebooted store the torn page fails its checksum on
    // first read — the device-backed sweep's category.
    instance_->page_device().TearNextWrite(100);
    ASSERT_EQ(instance_->Execute("SYNC"), "OK synced");
    instance_->Crash();
    ASSERT_TRUE(instance_->Reboot().ok());

    const IntegrityReport report =
        CheckInstance(instance_->store(), instance_->page_device(), instance_->catalog());
    EXPECT_GE(report.CountOf(CheckKind::kPageHeader), 1u) << report.Summary();
}

// RV3's stronger arm of the same scenario: with catalog mutations logged,
// redo now *names* the torn catalog page, finds no full page image to
// heal it with (wal.md §10's first-write-per-checkpoint FPI is still
// unbuilt for every page class), and **refuses the mount** - the same
// contract a torn heap page already lives under, extended to the catalog.
// Before RV3 this boot succeeded and served the corruption; refusing is
// the honest interim until §10's FPI cadence exists.
TEST_F(SimIntegrityCorruption, ATornCatalogPageRefusesTheMountInsteadOfServingIt) {
    MakeTables();
    for (int i = 0; i < 20; ++i) Insert("h", i, "short");

    instance_->page_device().TearNextWrite(100);
    ASSERT_EQ(instance_->Execute("SYNC"), "OK synced");
    instance_->Crash();

    Status rebooted = instance_->Reboot();
    ASSERT_FALSE(rebooted.ok()) << "a recovered mount served a torn catalog page";
    EXPECT_NE(rebooted.message().find("no full page image"), std::string::npos)
        << rebooted.message();
}

// ---- RV3: DDL is durable (docs/workplan-rv3-catalog-recovery.md) ----------
//
// Four crash shapes, each the smallest statement of one guarantee. kStrict
// where the test needs "acknowledged means durable in the log"; pages are
// deliberately never flushed unless the test flushes them, because redo
// rebuilding catalog pages from the log alone is the feature.

class Rv3CrashTest : public ::testing::Test {
protected:
    void MakeStrict() {
        auto instance = SimInstance::Create({.durability = wal::DurabilityClass::kStrict});
        ASSERT_TRUE(instance.ok()) << instance.status().message();
        instance_ = std::move(instance.value());
    }
    std::string Run(std::string_view sql) { return instance_->Execute(sql); }

    std::unique_ptr<SimInstance> instance_;
};

TEST_F(Rv3CrashTest, ACommittedCreateTableSurvivesACrashItsPagesNeverReached) {
    MakeStrict();
    ASSERT_EQ(Run("CREATE TABLE t (id int64, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO t VALUES (7)").substr(0, 8), "INSERTED");

    // No SYNC: every catalog page dies with the crash, and redo alone must
    // bring the relation back - which is RV3's entire promise, and exactly
    // what `ddl_durable=0` used to disclaim.
    instance_->Crash();
    Status rebooted = instance_->Reboot();
    ASSERT_TRUE(rebooted.ok()) << rebooted.message();
    EXPECT_EQ(Run("DESCRIBE t").rfind("ERR", 0), std::string::npos)
        << "a committed CREATE TABLE did not survive the crash";
    EXPECT_NE(Run("SELECT id, v FROM t WHERE id = 1").find("7"), std::string::npos);
}

TEST_F(Rv3CrashTest, AnUncommittedCreateAtCrashLeavesNoRelation) {
    MakeStrict();
    ASSERT_EQ(Run("BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run("CREATE TABLE ghost (id int64, v int64)").substr(0, 7), "CREATED");
    // The hostile flush: the uncommitted rows reach the platter, WAL and
    // pages both. Recovery's undo phase - walking the undo records the
    // catalog's hook appended - is the only thing that can remove them.
    ASSERT_EQ(Run("SYNC"), "OK synced");
    instance_->Crash();
    ASSERT_TRUE(instance_->Reboot().ok());
    EXPECT_EQ(Run("DESCRIBE ghost").rfind("ERR", 0), 0u)
        << "an uncommitted CREATE TABLE survived the crash";
    // And the name is free - the half-created relation left no debris a
    // re-create trips on.
    EXPECT_EQ(Run("CREATE TABLE ghost (id int64, v int64)").substr(0, 7), "CREATED");
}

TEST_F(Rv3CrashTest, AnUncommittedDropAtCrashLeavesTheRelationWhole) {
    MakeStrict();
    ASSERT_EQ(Run("CREATE TABLE keep (id int64, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO keep VALUES (9)").substr(0, 8), "INSERTED");

    ASSERT_EQ(Run("BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run("DROP TABLE keep").rfind("ERR", 0), std::string::npos);
    // Same hostile flush: the retype and the delete-marks are on the
    // platter. Rolling them back at mount is the kOverwrite undo record
    // (the prior image is its only surviving copy) and the kDeleteMark
    // ones, through the ordinary recovery-undo machinery.
    ASSERT_EQ(Run("SYNC"), "OK synced");
    instance_->Crash();
    ASSERT_TRUE(instance_->Reboot().ok());
    EXPECT_EQ(Run("DESCRIBE keep").rfind("ERR", 0), std::string::npos)
        << "an uncommitted DROP TABLE stayed dropped across the crash";
    EXPECT_NE(Run("SELECT id, v FROM keep WHERE id = 1").find("9"), std::string::npos);
}

TEST_F(Rv3CrashTest, ACommittedCreateIndexAnswersProbesAfterACrash) {
    MakeStrict();
    ASSERT_EQ(Run("CREATE TABLE t (id int64, owner int64) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO t VALUES (10)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("CREATE INDEX by_owner ON t (owner)").rfind("ERR", 0), std::string::npos);

    instance_->Crash();
    Status rebooted = instance_->Reboot();
    ASSERT_TRUE(rebooted.ok()) << rebooted.message();

    EXPECT_NE(Run("SHOW INDEXES").find("by_owner"), std::string::npos)
        << "a committed CREATE INDEX did not survive the crash";
    // The control index_contract_test.cpp calls for: the SELECT proves the
    // tree only while it actually probes it.
    EXPECT_NE(Run("ANALYZE SELECT id, owner FROM t WHERE owner = 10").find("IndexProbe"),
              std::string::npos);
    EXPECT_NE(Run("SELECT id, owner FROM t WHERE owner = 10").find("10"), std::string::npos)
        << "the recovered index lost the backfilled row";
}

// RV3's loudest remainder, closed: the sys.assertions row is what RC07
// rebuilds the *enforcing* registry from at mount, and it used to be
// ChainInsert-unlogged - so an acknowledged CREATE ASSERTION followed by
// a crash silently lost a constraint the operator was told existed.
TEST_F(Rv3CrashTest, ACommittedCreateAssertionEnforcesAfterACrash) {
    MakeStrict();
    ASSERT_EQ(Run("CREATE TABLE trades (id int64, account int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK COUNT(*) <= 2")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7)").substr(0, 8), "INSERTED");

    // No SYNC: the declaration's pages die with the crash, and the log
    // alone must bring back both the row and the registry built from it.
    instance_->Crash();
    Status rebooted = instance_->Reboot();
    ASSERT_TRUE(rebooted.ok()) << rebooted.message();

    const std::string shown = Run("SHOW ASSERTIONS");
    EXPECT_NE(shown.find("cap"), std::string::npos)
        << "an acknowledged CREATE ASSERTION vanished across the crash: " << shown;
    EXPECT_NE(shown.find("enforcing=1"), std::string::npos) << shown;

    // The constraint must not merely be listed - it must refuse. Two more
    // rows into the surviving group of one crosses the bound of 2.
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7)").substr(0, 8), "INSERTED");
    const std::string refused = Run("INSERT INTO trades VALUES (7)");
    EXPECT_EQ(refused.substr(0, 23), "ERR ASSERTION_VIOLATION")
        << "the recovered assertion is listed but not enforcing: " << refused;
}

// The pattern twin: advisory (invariant 8), so what a crash used to cost
// was a re-learned pattern - but the definition is a durable declaration
// the operator made, and it survives like one now.
TEST_F(Rv3CrashTest, ACommittedCreatePatternSurvivesACrash) {
    MakeStrict();
    ASSERT_EQ(Run("CREATE TABLE t (id int64, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("CREATE PATTERN watch($k int64) OF SELECT v FROM t WHERE id = $k")
                  .rfind("ERR", 0),
              std::string::npos);

    instance_->Crash();
    Status rebooted = instance_->Reboot();
    ASSERT_TRUE(rebooted.ok()) << rebooted.message();
    EXPECT_NE(Run("SHOW PATTERNS").find("watch"), std::string::npos)
        << "an acknowledged CREATE PATTERN vanished across the crash";
}

// The SLOT_RETIRE half of the same remainder: a dropped declaration must
// stay dropped, or a crash resurrects an assertion the operator removed -
// which for an enforcing constraint is a refusal nobody asked for.
TEST_F(Rv3CrashTest, ADroppedDeclarationStaysDroppedAcrossACrash) {
    MakeStrict();
    ASSERT_EQ(Run("CREATE TABLE trades (id int64, account int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK COUNT(*) <= 1")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("DROP ASSERTION cap").rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(Run("CREATE PATTERN watch($k int64) OF SELECT id FROM trades WHERE id = $k")
                  .rfind("ERR", 0),
              std::string::npos);
    ASSERT_EQ(Run("DROP PATTERN watch").rfind("ERR", 0), std::string::npos);

    instance_->Crash();
    Status rebooted = instance_->Reboot();
    ASSERT_TRUE(rebooted.ok()) << rebooted.message();

    EXPECT_EQ(Run("SHOW ASSERTIONS").find("cap"), std::string::npos)
        << "a dropped assertion resurrected across the crash";
    EXPECT_EQ(Run("SHOW PATTERNS").find("watch"), std::string::npos)
        << "a dropped pattern resurrected across the crash";
    // And the dropped assertion must not refuse: two rows into one group
    // crosses its old bound of 1, which only a resurrected cap would mind.
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7)").substr(0, 8), "INSERTED");
    EXPECT_EQ(Run("INSERT INTO trades VALUES (7)").substr(0, 8), "INSERTED");
}

TEST_F(Rv3CrashTest, ACommittedDropStaysDroppedAcrossACrash) {
    MakeStrict();
    ASSERT_EQ(Run("CREATE TABLE gone (id int64, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("DROP TABLE gone").rfind("ERR", 0), std::string::npos);

    instance_->Crash();
    ASSERT_TRUE(instance_->Reboot().ok());
    EXPECT_EQ(Run("DESCRIBE gone").rfind("ERR", 0), 0u)
        << "a committed DROP TABLE resurrected across the crash";
    EXPECT_EQ(Run("CREATE TABLE gone (id int64, v int64)").substr(0, 7), "CREATED");
}



// ---- SIM05: the fault schedule -------------------------------------------

std::vector<std::string> ScheduleOf(std::uint64_t seed, std::size_t ops) {
    const FaultSchedule schedule(Rng(seed).Fork("faults"), ops, FaultProfile::kIo, 40);
    std::vector<std::string> out;
    for (const ScheduledFault& fault : schedule.all()) out.push_back(fault.Describe());
    return out;
}

TEST(SimFaults, TheScheduleIsAPureFunctionOfTheSeed) {
    EXPECT_EQ(ScheduleOf(7, 2000), ScheduleOf(7, 2000));
    EXPECT_NE(ScheduleOf(7, 2000), ScheduleOf(8, 2000));
    EXPECT_FALSE(ScheduleOf(7, 2000).empty());
    // kNone injects nothing, which is what makes it the free default.
    EXPECT_TRUE(FaultSchedule(Rng(7), 2000, FaultProfile::kNone, 40).empty());
    // And a run too short to round up to one fault still gets one: a
    // profile that silently injected nothing would be a fault run that
    // proved nothing.
    EXPECT_EQ(FaultSchedule(Rng(7), 10, FaultProfile::kIo, 40).size(), 1u);
}

// The contract SIM05 exists to assert: under injected device errors every
// statement either succeeds or answers truthfully, the instance is healthy
// once the schedule runs out (the quiescence probe), and the restart still
// owes everything the engine acknowledged.
TEST(SimFaults, EveryModeSurvivesInjectedDeviceErrorsOnEveryCommittedSeed) {
    for (const std::uint64_t seed : CommittedSeeds()) {
        for (const SimMode mode : {SimMode::kClean, SimMode::kSyncCrash, SimMode::kCrash}) {
            SimConfig config;
            config.seed = seed;
            config.ops = 800;
            config.mode = mode;
            config.iterations = 2;
            config.faults = FaultProfile::kIo;
            config.fault_rate = 40;
            const SimVerdict verdict = RunSimulation(config);
            EXPECT_TRUE(verdict.ok) << verdict.Summary(config);
            EXPECT_GT(verdict.faults_armed, 0u) << verdict.Summary(config);
        }
    }
}

// A fault run whose injections never fired proves nothing, so the corpus
// has to be shown to really disturb the engine — and the disturbance has to
// reach the *statement*, not just the device.
TEST(SimFaults, TheCorpusReallyFiresInjectionsAndErrorsStatements) {
    SimConfig config;
    config.seed = 1;
    config.ops = 1500;
    config.mode = SimMode::kClean;
    config.iterations = 3;
    config.faults = FaultProfile::kIo;
    config.fault_rate = 40;
    const SimVerdict verdict = RunSimulation(config);
    ASSERT_TRUE(verdict.ok) << verdict.Summary(config);
    EXPECT_GT(verdict.faults_fired, 0u) << verdict.Summary(config);
    EXPECT_GT(verdict.errored_ops, 0u) << verdict.Summary(config);
}

// The quiescence probe must be able to fail: an engine that survives a
// fault run by refusing everything afterwards would pass every other check
// in the loop. Hand-fed here by dropping a relation behind the oracle's
// back, which is the shape of "the instance stopped answering for something
// the client was promised".
TEST(SimFaults, TheQuiescenceProbeFiresOnAnInstanceThatStoppedAnswering) {
    auto made = SimInstance::Create();
    ASSERT_TRUE(made.ok()) << made.status().message();
    SimInstance& db = *made.value();

    Oracle oracle;
    ASSERT_EQ(db.Execute("CREATE TABLE t (id int64, v int64, name varchar) HEAP").substr(0, 7),
              "CREATED");
    oracle.CreateTable("t");
    for (int i = 0; i < 5; ++i) {
        const std::string reply =
            db.Execute("INSERT INTO t VALUES (" + std::to_string(i) + ", 'x')");
        const std::optional<std::uint64_t> id = ParseInsertedId(reply);
        ASSERT_TRUE(id.has_value()) << reply;
        oracle.ApplyInsert("t", *id, OracleRow{i, "x"});
    }

    std::string why;
    ASSERT_TRUE(ScanAgreesWithOracle(db, oracle, why)) << why;

    ASSERT_EQ(db.Execute("DROP TABLE t").rfind("ERR", 0), std::string::npos);
    EXPECT_FALSE(ScanAgreesWithOracle(db, oracle, why))
        << "the probe accepted an instance that cannot answer for a relation the client "
           "was promised";
    EXPECT_NE(why.find("'t'"), std::string::npos) << why;
}

// ---- SIM06: mutations, transactions, and the advisory toggles -------------

TEST(SimWorkload, TheGeneratedStreamCoversEveryV2Shape) {
    Workload workload(Rng(11).Fork("workload"), Profile::kUniform);
    std::map<Op::Kind, int> seen;
    for (int i = 0; i < 4000; ++i) ++seen[workload.Next().kind];

    for (const Op::Kind kind :
         {Op::Kind::kInsert, Op::Kind::kUpdate, Op::Kind::kDelete, Op::Kind::kSelectPk,
          Op::Kind::kSelectRange, Op::Kind::kFilterScan, Op::Kind::kSync, Op::Kind::kBegin,
          Op::Kind::kCommit, Op::Kind::kRollback, Op::Kind::kCreateCabin,
          Op::Kind::kCreatePattern}) {
        EXPECT_GT(seen[kind], 0) << "the stream never generates " << OpKindName(kind);
    }

    // Both mutation shapes, since the engine's rules differ across the line
    // (sim/workload.hpp): a SET on the column a Cabin and an index are
    // keyed on, and one on the varchar that may spill.
    Workload again(Rng(11).Fork("workload"), Profile::kUniform);
    bool set_v = false, set_name = false, by_pk = false, by_value = false;
    for (int i = 0; i < 4000; ++i) {
        const Op op = again.Next();
        if (op.kind != Op::Kind::kUpdate) continue;
        (op.set_name ? set_name : set_v) = true;
        (op.by_pk ? by_pk : by_value) = true;
    }
    EXPECT_TRUE(set_v);
    EXPECT_TRUE(set_name);
    EXPECT_TRUE(by_pk);
    EXPECT_TRUE(by_value);
}

// The count a mutation reports is compared against the oracle's own count
// of the rows the predicate matches — the sharpest single assertion the
// harness makes. This pins that the corpus actually reaches it.
TEST(SimLoop, MutationCountsAndTransactionsAreCheckedOnEveryCommittedSeed) {
    for (const std::uint64_t seed : CommittedSeeds()) {
        SimConfig config;
        config.seed = seed;
        config.ops = 1500;
        config.mode = SimMode::kClean;
        const SimVerdict verdict = RunSimulation(config);
        EXPECT_TRUE(verdict.ok) << verdict.Summary(config);
        EXPECT_GT(verdict.writes_checked, 0u) << verdict.Summary(config);
        EXPECT_GT(verdict.transactions, 0u) << verdict.Summary(config);
    }
}

// Invariant 8's promise, generalized to all three advisory switches and to
// a generated workload: two instances differing only in them answer the
// same op stream identically.
TEST(SimLoop, TheAdvisoryFeaturesChangeNoAnswerOnEveryCommittedSeed) {
    for (const std::uint64_t seed : CommittedSeeds()) {
        SimConfig config;
        config.seed = seed;
        config.ops = 1200;
        const SimVerdict verdict = RunTogglePairing(config);
        EXPECT_TRUE(verdict.ok) << verdict.Summary(config);
        EXPECT_GT(verdict.ops_run, 0u);
    }
}

// The pairing's comparison has three tiers and its exceptions are where the
// risk is, so they are pinned directly rather than only through runs that
// happen to pass.
TEST(SimLoop, ThePairingComparatorDiscriminates) {
    Op read;
    read.kind = Op::Kind::kSelectPk;
    EXPECT_TRUE(SameOutcome(read, "id,v,name\\n1,2,a", "id,v,name\\n1,2,a"));
    EXPECT_FALSE(SameOutcome(read, "id,v,name\\n1,2,a", "id,v,name\\n1,3,a"))
        << "a differing row passed as the same answer";

    Op insert;
    insert.kind = Op::Kind::kInsert;
    EXPECT_TRUE(SameOutcome(insert, "INSERTED oid=4000 id=7 page=134 slot=0",
                            "INSERTED oid=4000 id=7 page=137 slot=2"))
        << "placement is not an answer";
    EXPECT_FALSE(SameOutcome(insert, "INSERTED oid=4000 id=7 page=134 slot=0",
                             "INSERTED oid=4000 id=8 page=134 slot=0"))
        << "a differing id passed as the same answer";

    Op pattern;
    pattern.kind = Op::Kind::kCreatePattern;
    EXPECT_TRUE(SameOutcome(pattern, "CREATED PATTERN name=p0 pattern_id=0x1 dir_depth=1",
                            "ADOPTED PATTERN name=p0 pattern_id=0x1 dir_depth=1"));
    EXPECT_FALSE(SameOutcome(pattern, "CREATED PATTERN name=p0", "ERR nope"));
}

// ---- SIM07: the plan, the case file, the minimizer ------------------------

TEST(SimPlanTest, APlanIsAPureFunctionOfTheSeedAndTheIteration) {
    SimConfig config;
    config.seed = 5;
    config.ops = 500;
    config.mode = SimMode::kCrash;
    config.faults = FaultProfile::kIo;

    const SimPlan a = BuildPlan(config, 0);
    const SimPlan b = BuildPlan(config, 0);
    ASSERT_EQ(a.entries.size(), b.entries.size());
    for (std::size_t i = 0; i < a.entries.size(); ++i) {
        EXPECT_EQ(a.entries[i].op.sql, b.entries[i].op.sql);
        EXPECT_EQ(a.entries[i].faults, b.entries[i].faults);
    }

    // The crash point is inside the plan, not applied to it: a crash-mode
    // plan is at most the op budget, a clean one is exactly it.
    EXPECT_LE(a.entries.size(), config.ops);
    SimConfig clean = config;
    clean.mode = SimMode::kClean;
    EXPECT_EQ(BuildPlan(clean, 0).entries.size(), clean.ops);
}

TEST(SimCase, ACaseFileRoundTripsAndReplays) {
    SimConfig config;
    config.seed = 2;
    config.ops = 300;
    config.mode = SimMode::kCrash;
    config.profile = Profile::kColliding;
    config.faults = FaultProfile::kIo;

    const SimPlan plan = BuildPlan(config, 0);
    const std::string path = std::string(KDS_BINARY_DIR) + "/roundtrip.sim";
    ASSERT_TRUE(WriteCase(path, config, plan, "a signature").ok());

    auto loaded = ReadCase(path);
    ASSERT_TRUE(loaded.ok()) << loaded.status().message();
    ASSERT_EQ(loaded.value().plan.entries.size(), plan.entries.size());
    for (std::size_t i = 0; i < plan.entries.size(); ++i) {
        const SimPlan::Entry& want = plan.entries[i];
        const SimPlan::Entry& got = loaded.value().plan.entries[i];
        ASSERT_EQ(got.op.sql, want.op.sql) << "op " << i;
        EXPECT_EQ(got.op.kind, want.op.kind);
        EXPECT_EQ(got.op.table, want.op.table);
        EXPECT_EQ(got.op.name, want.op.name);
        EXPECT_EQ(got.op.key, want.op.key);
        EXPECT_EQ(got.op.v, want.op.v);
        EXPECT_EQ(got.op.pred_v, want.op.pred_v);
        EXPECT_EQ(got.op.by_pk, want.op.by_pk);
        EXPECT_EQ(got.op.set_name, want.op.set_name);
        EXPECT_EQ(got.faults, want.faults);
    }
    EXPECT_EQ(loaded.value().config.seed, config.seed);
    EXPECT_EQ(loaded.value().plan.toggles.Describe(), plan.toggles.Describe());

    // And the reloaded case runs, which is the property that makes it a
    // case rather than a log.
    const SimVerdict replayed = RunPlan(loaded.value().config, loaded.value().plan);
    EXPECT_TRUE(replayed.ok) << replayed.Summary(loaded.value().config);
    EXPECT_EQ(replayed.ops_run, plan.entries.size());
}

TEST(SimCase, AMalformedCaseFileIsRefusedRatherThanGuessedAt) {
    const std::string path = std::string(KDS_BINARY_DIR) + "/broken.sim";
    {
        std::ofstream out(path);
        out << "config\tseed=1\tmode=clean\tprofile=uniform\tfaults=none\ttoggles=000\n";
        out << "op\tno-such-kind\tt0\t0\t0\t0\t0\t0\t0\t0\t0\t\tSELECT 1\n";
    }
    EXPECT_FALSE(ReadCase(path).ok());
}

TEST(SimMinimizeTest, TheSignatureKeepsTheShapeAndDropsTheRun) {
    SimVerdict a;
    a.ok = false;
    a.detail = "seed=2 iteration=0: op 41 [SELECT * FROM t0] returned a row the oracle "
               "never accepted: '17,3,abc' [faults: armed op 3 page-fail-read]";
    SimVerdict b;
    b.ok = false;
    b.detail = "seed=9 iteration=2: op 8003 [SELECT * FROM t1] returned a row the oracle "
               "never accepted: '4,9,zz' [faults: armed op 900 log-fail-sync]";
    EXPECT_EQ(FailureSignature(a), FailureSignature(b));

    SimVerdict other;
    other.ok = false;
    other.detail = "seed=2 iteration=0: reboot failed: recovery of core 0";
    EXPECT_NE(FailureSignature(a), FailureSignature(other));

    SimVerdict clean;
    EXPECT_EQ(FailureSignature(clean), "");
}

// The minimizer, on a failure the harness can produce on demand: booting
// the crashed devices *without* recovery loses acknowledged rows (RC10's
// injection), and the shrunk case must still fail the same way. What this
// pins is that shrinking preserves the failure rather than wandering to
// another one — the property that makes a minimized case worth reading.
TEST(SimMinimizeTest, AFailureShrinksAndTheShrunkCaseStillFailsTheSameWay) {
    SimConfig config;
    config.seed = 4;
    config.ops = 400;
    config.mode = SimMode::kCrash;
    config.iterations = 1;
    config.skip_recovery = true;

    const MinimizeOutcome outcome = MinimizeFailure(config, /*max_replays=*/200);
    ASSERT_TRUE(outcome.found_failure) << outcome.Summary();
    EXPECT_LT(outcome.ops_after, outcome.ops_before) << outcome.Summary();
    EXPECT_GT(outcome.replays, 0u);

    const SimVerdict replayed = RunPlan(config, outcome.plan, outcome.iteration);
    ASSERT_FALSE(replayed.ok) << "the shrunk case no longer fails: " << outcome.Summary();
    EXPECT_EQ(FailureSignature(replayed), outcome.signature) << replayed.detail;

    // And through the file, which is the artifact anyone else gets. The
    // run-level gates (`skip_recovery` here) have to travel with it: a case
    // file that says it fails and replays green is worse than no file.
    const std::string path = std::string(KDS_BINARY_DIR) + "/minimized.sim";
    ASSERT_TRUE(WriteCase(path, config, outcome.plan, outcome.signature).ok());
    auto loaded = ReadCase(path);
    ASSERT_TRUE(loaded.ok()) << loaded.status().message();
    const SimVerdict from_file = RunPlan(loaded.value().config, loaded.value().plan);
    EXPECT_FALSE(from_file.ok) << "the case file replays green: " << path;
    EXPECT_EQ(FailureSignature(from_file), outcome.signature) << from_file.detail;
}

// Every enum that crosses a command line or a case file goes both ways in
// one place. The direction that was open-coded twice is the one that
// drifts, so it is the one pinned.
TEST(SimCase, EveryNamedEnumRoundTrips) {
    for (const SimMode mode : {SimMode::kClean, SimMode::kSyncCrash, SimMode::kCrash}) {
        EXPECT_EQ(ParseSimMode(SimModeName(mode)), mode);
    }
    for (const Profile profile : {Profile::kUniform, Profile::kZipfian, Profile::kColliding}) {
        EXPECT_EQ(ParseProfile(ProfileName(profile)), profile);
    }
    for (const FaultProfile faults : {FaultProfile::kNone, FaultProfile::kIo}) {
        EXPECT_EQ(ParseFaultProfile(FaultProfileName(faults)), faults);
    }
    for (const FaultKind kind :
         {FaultKind::kPageFailRead, FaultKind::kPageFailWrite, FaultKind::kPageFailSync,
          FaultKind::kPageFailGrow, FaultKind::kLogFailWrite, FaultKind::kLogFailSync}) {
        EXPECT_EQ(ParseFaultKind(FaultKindName(kind)), kind);
    }
    EXPECT_FALSE(ParseSimMode("no-such-mode").has_value());
    EXPECT_FALSE(ParseProfile("").has_value());
    EXPECT_FALSE(ParseFaultProfile("torn").has_value());
}

// The count assertion is the sharpest one the harness makes, and an
// unknown must cost it as little as it truthfully has to: a predicate on
// the pk names one row, so it stays checkable while *that* row is known.
TEST(SimOracleTest, AnUnknownRowCostsOnlyTheCountsItCouldChange) {
    Oracle oracle;
    oracle.CreateTable("t");
    oracle.ApplyInsert("t", 1, OracleRow{10, "a"});
    oracle.ApplyInsert("t", 2, OracleRow{10, "b"});

    const Oracle::Predicate by_pk_known{true, 1, 0};
    const Oracle::Predicate by_pk_unknown{true, 2, 0};
    const Oracle::Predicate by_value{false, 0, 10};
    EXPECT_TRUE(oracle.CountCheckable("t", by_pk_known));
    EXPECT_TRUE(oracle.CountCheckable("t", by_value));

    // An errored UPDATE on id 2: its own count is gone, id 1's is not, and
    // a value predicate that might match id 2 is.
    oracle.NoteUnchecked("t", 2);
    EXPECT_TRUE(oracle.CountCheckable("t", by_pk_known));
    EXPECT_FALSE(oracle.CountCheckable("t", by_pk_unknown));
    EXPECT_FALSE(oracle.CountCheckable("t", by_value));

    // An errored INSERT: the relation may hold rows it never named, so a
    // value predicate is gone — but a pk the engine *did* name is still
    // the oracle's to assert on.
    Oracle second;
    second.CreateTable("t");
    second.ApplyInsert("t", 1, OracleRow{10, "a"});
    second.NoteIndeterminate("t");
    EXPECT_TRUE(second.CountCheckable("t", Oracle::Predicate{true, 1, 0}));
    EXPECT_FALSE(second.CountCheckable("t", Oracle::Predicate{true, 99, 0}));
    EXPECT_FALSE(second.CountCheckable("t", by_value));
}



// ---- What this harness found: the Cabin's rolled-back set ----------------
//
// **[GATED: cabin rollback]**, `docs/inflight/known-gaps.md`. Found by SIM06's first
// fault-free sweep (seed 2, profile colliding, mode clean) and shrunk to
// these six statements by SIM07's minimizer, 1200 ops -> 9 in 933 replays.
//
// `cabin_store.hpp` states the authority rule: "Observed => complete,
// superset form, per snapshot ... Missing a qualifying pk violates
// authority." The header then argues the build-by-observation hazard is
// structurally dead because statements run to completion on the owning
// core - which answers a *concurrent* writer and not this:
//
//   the recording walk runs under **the recording transaction's snapshot**,
//   so a row that transaction has delete-marked is invisible to it and
//   never enters the set; the set is memory-resident, cross-transaction and
//   authoritative; and a ROLLBACK restores the row but not the entry.
//
// From then on every probe of that value answers from a set that is missing
// a live row - a wrong answer, not a slower one. The same shape reaches
// across sessions: any in-flight transaction's uncommitted delete can be
// baked into another session's banked set.
//
// **Fixed** by declining to record from a view that can still be
// contradicted (`docs/spec/cabin.md` §6a): a recording walk banks nothing
// while any transaction is in flight, its own included. Written gated, it
// ran SKIPPED for as long as the gap stood and is an ordinary regression
// test now.
TEST(SimFindings, ACabinSetBankedInsideARolledBackTransactionServesEveryLiveRow) {
    SimInstance::Options options;
    options.cabins = true;
    auto made = SimInstance::Create(options);
    ASSERT_TRUE(made.ok());
    SimInstance& db = *made.value();

    ASSERT_EQ(db.Execute("CREATE TABLE t (id int64, v int64, name varchar) HEAP").substr(0, 7),
              "CREATED");
    ASSERT_EQ(db.Execute("CREATE CABIN ON t(v)").substr(0, 7), "CREATED");
    ASSERT_EQ(db.Execute("INSERT INTO t VALUES (0, 'a')").substr(0, 8), "INSERTED");

    ASSERT_EQ(db.Execute("BEGIN").rfind("BEGIN ", 0), 0u);
    ASSERT_EQ(db.Execute("DELETE FROM t WHERE v = 0"), "DELETED 1");
    // The probe that banks the value's entry set — from inside the
    // transaction, where the deleted row is invisible.
    ASSERT_EQ(SplitEscapedLines(db.Execute("SELECT * FROM t WHERE v = 0")).size(), 1u);
    ASSERT_EQ(db.Execute("ROLLBACK").rfind("ROLLBACK ", 0), 0u);

    ASSERT_EQ(db.Execute("INSERT INTO t VALUES (0, 'b')").substr(0, 8), "INSERTED");
    const std::string after = db.Execute("SELECT * FROM t WHERE v = 0");
    EXPECT_EQ(SplitEscapedLines(after).size(), 3u)
        << "the rolled-back row is missing from an authoritative set: " << after;
}

// The control, and it passes today: the same shape with the DELETE
// committed banks a set that is right, so what the case above isolates is
// the *rollback*, not deletion or recording.
TEST(SimFindings, ACabinSetBankedAfterACommittedDeleteIsCorrect) {
    SimInstance::Options options;
    options.cabins = true;
    auto made = SimInstance::Create(options);
    ASSERT_TRUE(made.ok());
    SimInstance& db = *made.value();

    ASSERT_EQ(db.Execute("CREATE TABLE t (id int64, v int64, name varchar) HEAP").substr(0, 7),
              "CREATED");
    ASSERT_EQ(db.Execute("CREATE CABIN ON t(v)").substr(0, 7), "CREATED");
    ASSERT_EQ(db.Execute("INSERT INTO t VALUES (0, 'a')").substr(0, 8), "INSERTED");
    ASSERT_EQ(db.Execute("DELETE FROM t WHERE v = 0"), "DELETED 1");
    db.Execute("SELECT * FROM t WHERE v = 0");
    ASSERT_EQ(db.Execute("INSERT INTO t VALUES (0, 'b')").substr(0, 8), "INSERTED");
    const std::string after = db.Execute("SELECT * FROM t WHERE v = 0");
    EXPECT_EQ(SplitEscapedLines(after).size(), 2u) << after;  // header + the live row
}


// The **other half of the same bug**, and the reason declining to record
// is the fix rather than un-observing on rollback: a set banked while
// another session's transaction is in flight misses the rows that
// transaction is about to *commit*. Nothing rolls back, so there is no
// event an unobserve-on-rollback rule could hang from — the set is simply
// wrong from the moment the other transaction commits.
//
// Two sessions over one dispatcher, which is what `Dispatch(sql, &session)`
// is for and what SIM08's driver will want wholesale.
TEST(SimFindings, ACabinSetIsNotBankedWhileAnotherTransactionIsInFlight) {
    SimInstance::Options options;
    options.cabins = true;
    auto made = SimInstance::Create(options);
    ASSERT_TRUE(made.ok());
    SimInstance& db = *made.value();
    server::Session other;
    const auto on_other = [&](const std::string& sql) {
        return db.dispatcher().Dispatch(sql, &other).response;
    };

    ASSERT_EQ(db.Execute("CREATE TABLE t (id int64, v int64, name varchar) HEAP").substr(0, 7),
              "CREATED");
    ASSERT_EQ(db.Execute("CREATE CABIN ON t(v)").substr(0, 7), "CREATED");
    ASSERT_EQ(db.Execute("INSERT INTO t VALUES (0, 'a')").substr(0, 8), "INSERTED");

    // The other session writes a second row for the value and holds it.
    ASSERT_EQ(on_other("BEGIN").rfind("BEGIN ", 0), 0u);
    ASSERT_EQ(on_other("INSERT INTO t VALUES (0, 'b')").substr(0, 8), "INSERTED");

    // This session probes and banks the set — from a view the uncommitted
    // row is not in.
    ASSERT_EQ(SplitEscapedLines(db.Execute("SELECT * FROM t WHERE v = 0")).size(), 2u);

    ASSERT_EQ(on_other("COMMIT").rfind("COMMIT ", 0), 0u);

    const std::string after = db.Execute("SELECT * FROM t WHERE v = 0");
    EXPECT_EQ(SplitEscapedLines(after).size(), 3u) << after;  // header + both rows

    // And for the stated reason: the guard fired rather than the answer
    // happening to be right for some other cause.
    const std::string cabins = db.Execute("SHOW CABINS");
    EXPECT_EQ(cabins.find("unbankable_views=0"), std::string::npos) << cabins;
}


}  // namespace
}  // namespace kds::sim
