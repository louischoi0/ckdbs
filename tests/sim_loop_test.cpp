#include "sim/loop.hpp"

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "sim/instance.hpp"
#include "sim/integrity.hpp"
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

}  // namespace
}  // namespace kds::sim
