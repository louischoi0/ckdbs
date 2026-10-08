// **BF: DROP TABLE page reclamation**
// (`instructions/v3.0.0/workorder-bf-drop-table-page-reclaim.md`).
//
// BF-S1's cells, written before anything is built:
//
// - **The warm-up premise** (§1.4, §1.9 item 1, BF-R4's `cores > 1` arm). A
//   mount's completion checkpoint moves the durable redo start `D` past the
//   replayed log at `cores = 1`, and leaves the mount's anchor where it was
//   at `cores = 2` until every core has published. The cell shows it the
//   only way a free could be hurt by it: a dropped relation's bit is
//   cleared in the image a crash right after that checkpoint leaves, and
//   the next mount is refused at `cores = 2` and mounts at `cores = 1`. No
//   store code is touched - the bit is cleared by editing the map page's
//   bytes in the copied file.
// - **Red at `df741e3c`**, each for the reason it names: a dropped
//   relation's pages stay allocated across a restart; five create-fill-drop
//   rounds grow the volume by four rounds' pages; a new relation never
//   lands on a dropped one's ids; the verifier accepts another owner's
//   page; `ReadTuple` reads a slot that leaves the page.
// - **Guards, green and kept green:** a rolled-back drop leaves the
//   relation whole across a restart.
//
// Every cell drives an `Expeditor` without `Start()`: the dispatcher runs on
// the calling thread, and a mount is `Expeditor::Open`, production's own.

#include <algorithm>
#include <thread>
#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "reclaim_census.hpp"

#include "kds/base/current_core.hpp"
#include "kds/exec/tuple_verify.hpp"
#include "kds/server/expeditor.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/free_map.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/page_header.hpp"

namespace kds::server {
namespace {

namespace fs = std::filesystem;
using reclaim_census::AllocatedPages;
using reclaim_census::DescribeField;
using reclaim_census::Fill;
using reclaim_census::PagesOf;

bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

struct TempDir {
    fs::path path;
    TempDir() {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("kds_bf_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// A mount of the volume in `dir`, with no cadence checkpoint: the only
// checkpoints are the mount's completion checkpoint and the ones a cell
// asks for, so a cut lands where the cell put it.
StatusOr<std::unique_ptr<Expeditor>> Mount(const fs::path& dir, std::uint32_t cores) {
    Expeditor::Config config;
    config.buffer_pool_frames = 4096;  // required (BE-R3)
    config.data_file = (dir / "kds.db").string();
    config.wal_dir = (dir / "wal").string();
    config.log_file = {};
    config.cores = cores;
    config.debug_text_port = 0;
    config.checkpoint_interval_ns = 0;
    return Expeditor::Open(config, /*now_unix_seconds=*/1000);
}

void Ok(Expeditor& db, const std::string& sql) {
    const std::string reply = db.dispatcher().Dispatch(sql).response;
    ASSERT_FALSE(StartsWith(reply, "ERR")) << sql << " -> " << reply;
}

// What a clean stop does (`CoreRuntime::ShutdownCheckpoint`): pages first,
// so the checkpoint's dirty table is empty and the anchor's redo start is
// the checkpoint itself. `Checkpoint()` alone starts redo at the oldest
// dirty page's recLSN, inside the run, so the next mount would replay the
// run and raise the allocation floor past every page it names.
void CleanShutdownCheckpoint(Expeditor& db) {
    EXPECT_TRUE(db.store().Sync().ok());
    EXPECT_TRUE(db.Checkpoint().ok());
}

// **What a mount does with a pending reclaim before a cell looks**: core
// 0's reclaim tick (BF-R8), driven by hand until no tombstone is owed or one
// is refused. BF-S1 wrote this as a no-op and the cells red against it.
void Settle(Expeditor& db) {
    for (int step = 0; step < 100000; ++step) {
        if (db.reclaim_counters().pending.load() == 0 ||
            db.reclaim_counters().refused.load() != 0) {
            break;
        }
        ASSERT_TRUE(db.ReclaimStep().ok());
    }
}

// A crash at this instant: the data file and the log copied as they are.
// Nothing is synced first, so only what the store and the log have already
// written is in the copy.
void CrashImage(const fs::path& from, const fs::path& to) {
    fs::create_directories(to);
    fs::copy_file(from / "kds.db", to / "kds.db", fs::copy_options::overwrite_existing);
    fs::copy(from / "wal", to / "wal",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
}

// Clears `id`'s allocation bit in the data file at `file`: the region's map
// page is read, its bit cleared, its checksum restamped, and written back.
void ClearMapBit(const fs::path& file, PageId id) {
    const PageId map_page = storage::FreeMapPageIdFor(id);
    alignas(8) std::array<std::byte, kPageSize> page{};
    std::fstream f(file, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(f.good());
    f.seekg(static_cast<std::streamoff>(map_page) * kPageSize);
    f.read(reinterpret_cast<char*>(page.data()), kPageSize);
    ASSERT_TRUE(f.good());
    std::span<std::byte, kPageSize> bytes(page);
    ASSERT_TRUE(storage::ValidateFreeMapPage(bytes).ok());
    const std::uint32_t index = storage::FreeMapBitIndexOf(id);
    ASSERT_TRUE(storage::FreeMapIsAllocated(bytes, index));
    storage::FreeMapRelease(bytes, index);
    ASSERT_FALSE(storage::FreeMapIsAllocated(bytes, index));
    storage::StampPageChecksum(bytes);
    f.seekp(static_cast<std::streamoff>(map_page) * kPageSize);
    f.write(reinterpret_cast<const char*>(page.data()), kPageSize);
    ASSERT_TRUE(f.good());
}

// ---- The warm-up premise (BF-R4's `cores > 1` arm) -------------------------

// A volume whose log, from its anchor on, still names a dropped relation's
// leaf by a record that is not its creation: the relation is created and
// filled, a checkpoint moves the anchor past its `PAGE_INIT`s, more rows
// land on the same leaf, and the relation is dropped. Then the crash.
// Returns the leaf.
PageId VolumeWithADroppedLeafInItsReplayRange(const fs::path& dir) {
    PageId leaf = kInvalidPageId;
    auto opened = Mount(dir, /*cores=*/1);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    if (!opened.ok()) return leaf;
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    Ok(db, "CREATE TABLE t (id int64, v varchar)");
    Fill(db.dispatcher(), "t", 3);
    leaf = static_cast<PageId>(DescribeField(db.dispatcher(), "t", "root_page_id"));
    CleanShutdownCheckpoint(db);
    Fill(db.dispatcher(), "t", 3);
    Ok(db, "DROP TABLE t");
    EXPECT_TRUE(db.wal().Flush().ok());
    TempDir crash;
    CrashImage(dir, crash.path);
    opened.value().reset();
    // The volume is the crash image, not the closed instance's files.
    fs::remove_all(dir / "wal");
    CrashImage(crash.path, dir);
    return leaf;
}

// Mounts the volume at `cores`, takes the crash image its completion
// checkpoint leaves, clears `leaf`'s bit in it, and mounts that.
Status MountAfterTheBitIsClearedAtTheCompletionCheckpoint(const fs::path& volume, PageId leaf,
                                                          std::uint32_t cores) {
    TempDir cut;
    {
        auto first = Mount(volume, cores);
        if (!first.ok()) return first.status();
        EXPECT_TRUE(first.value()->store().IsAllocated(leaf))
            << "the first mount must still hold the dropped leaf";
        CrashImage(volume, cut.path);
    }
    ClearMapBit(cut.path / "kds.db", leaf);
    auto second = Mount(cut.path, cores);
    return second.ok() ? Status::OK() : second.status();
}

TEST(DropTableReclaimPremiseTest, AClearedBitRefusesTheNextMountUntilEveryCoreHasPublished) {
    // Repeated: the cut is deterministic - no cadence, no peer reactor - so
    // a run that disagreed with another would be a finding in itself.
    constexpr int kRuns = 5;
    for (int run = 0; run < kRuns; ++run) {
        SCOPED_TRACE("run " + std::to_string(run));
        TempDir two;
        const PageId leaf2 = VolumeWithADroppedLeafInItsReplayRange(two.path);
        ASSERT_NE(leaf2, kInvalidPageId);
        const Status at_two = MountAfterTheBitIsClearedAtTheCompletionCheckpoint(two.path, leaf2, 2);
        EXPECT_FALSE(at_two.ok())
            << "at cores = 2 the completion checkpoint left the anchor where it was, so the "
               "next mount replays a record that names the cleared leaf";
        EXPECT_NE(at_two.message().find("names page " + std::to_string(leaf2) + ","),
                  std::string::npos)
            << "refused for another reason: " << at_two.message();

        TempDir one;
        const PageId leaf1 = VolumeWithADroppedLeafInItsReplayRange(one.path);
        ASSERT_NE(leaf1, kInvalidPageId);
        const Status at_one = MountAfterTheBitIsClearedAtTheCompletionCheckpoint(one.path, leaf1, 1);
        EXPECT_TRUE(at_one.ok())
            << "at cores = 1 the completion checkpoint moves D past the replayed log: "
            << at_one.message();
    }
}

// ---- Red first: the leak ---------------------------------------------------

struct Dropped {
    std::uint64_t oid = 0;
    std::set<PageId> pages;
};

// One round: create, fill, drop, and a clean shutdown. Returns what the
// drop left behind.
Dropped CreateFillDrop(const fs::path& dir, const std::string& table, int rows,
                       const std::string& storage = "") {
    Dropped out;
    auto opened = Mount(dir, /*cores=*/1);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    if (!opened.ok()) return out;
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    Settle(db);
    Ok(db, "CREATE TABLE " + table + " (id int64, v varchar)" + storage);
    Fill(db.dispatcher(), table, rows);
    out.oid = DescribeField(db.dispatcher(), table, "oid");
    out.pages = PagesOf(db.store(), out.oid);
    Ok(db, "DROP TABLE " + table);
    CleanShutdownCheckpoint(db);
    return out;
}

TEST(DropTableReclaimTest, ADroppedRelationsPagesAreFreeAfterTheNextMount) {
    // Red at `df741e3c`: `DROP TABLE` writes catalog rows only, and nothing
    // clears a free-map bit (DT1).
    TempDir dir;
    const Dropped dropped = CreateFillDrop(dir.path, "t", 1000);
    ASSERT_GE(dropped.pages.size(), 4u) << "the anchor, a var-heap root and two leaves at least";

    auto opened = Mount(dir.path, /*cores=*/1);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    Settle(db);
    for (const PageId id : dropped.pages) {
        EXPECT_FALSE(db.store().IsAllocated(id)) << "page " << id << " of the dropped relation";
    }
    EXPECT_TRUE(PagesOf(db.store(), dropped.oid).empty());
}

TEST(DropTableReclaimTest, FiveCreateFillDropRoundsGrowTheVolumeByLessThanOneRound) {
    // Red at `df741e3c`: every round's pages stay allocated, so five rounds
    // hold five relations' worth. The allowance is the undo pages a run
    // leaves behind (UP4, `txn.md` §4.1), which BF does not reclaim; it is
    // far below one round's relation pages at this fill.
    TempDir dir;
    constexpr int kRounds = 5;
    std::vector<std::uint32_t> allocated;
    std::size_t round_pages = 0;
    for (int round = 0; round < kRounds; ++round) {
        const Dropped dropped = CreateFillDrop(dir.path, "t", 3000);
        if (round == 0) round_pages = dropped.pages.size();
        auto opened = Mount(dir.path, /*cores=*/1);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        CurrentCoreGuard as(0);
        Settle(*opened.value());
        allocated.push_back(opened.value()->store().allocated_pages());
    }
    ASSERT_GE(round_pages, 10u);
    const std::uint32_t growth = allocated.back() - allocated.front();
    EXPECT_LT(growth, round_pages)
        << "after round 1: " << allocated.front() << " pages; after round " << kRounds << ": "
        << allocated.back() << "; one round's relation holds " << round_pages;
}

TEST(DropTableReclaimTest, ADroppedHeapRelationsChainIsFreed) {
    // BF-Q8 (a)'s one heap cell, and no crash matrix: only a test seam
    // creates a heap relation since SUS-1 (`heap_suspension_env.cpp`). The
    // anchor names the chain's head; the walk takes the chain by its links
    // and frees it tail-first.
    TempDir dir;
    const Dropped dropped = CreateFillDrop(dir.path, "h", 2000, " HEAP");
    ASSERT_GE(dropped.pages.size(), 4u);
    auto opened = Mount(dir.path, /*cores=*/1);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    Settle(db);
    EXPECT_EQ(db.reclaim_counters().refused.load(), 0u);
    EXPECT_EQ(db.reclaim_counters().skipped.load(), 0u);
    for (const PageId id : dropped.pages) {
        EXPECT_FALSE(db.store().IsAllocated(id)) << "page " << id << " of the dropped heap";
    }
}

TEST(DropTableReclaimTest, ANewRelationLandsOnIdsADroppedRelationHeld) {
    // Red at `df741e3c`: the cursor starts at 128 after a clean restart and
    // finds no cleared bit, so a new relation goes past every dropped page.
    TempDir dir;
    const Dropped dropped = CreateFillDrop(dir.path, "t", 1000);
    ASSERT_FALSE(dropped.pages.empty());

    auto opened = Mount(dir.path, /*cores=*/1);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    Settle(db);
    Ok(db, "CREATE TABLE u (id int64, v varchar)");
    Fill(db.dispatcher(), "u", 1000);
    const std::set<PageId> reused = PagesOf(db.store(), DescribeField(db.dispatcher(), "u", "oid"));
    std::vector<PageId> common;
    std::set_intersection(dropped.pages.begin(), dropped.pages.end(), reused.begin(), reused.end(),
                          std::back_inserter(common));
    EXPECT_FALSE(common.empty()) << "no page of u is on an id t held";
}

// ---- BF-S3: the mount-time reclaim, its gate and its crash cuts -----------

// A relation of two tree levels with a var-heap chain - values past the
// inline width spill - beside a live relation the reclaim must not touch.
// Dropped, and the instance shut down cleanly. Returns the ledger.
struct Ledger {
    Dropped dropped;
    std::uint64_t keep_rows = 0;
};
Ledger TwoRelationsOneDropped(const fs::path& dir) {
    Ledger out;
    auto opened = Mount(dir, /*cores=*/1);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    if (!opened.ok()) return out;
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    Ok(db, "CREATE TABLE keep (id int64, v varchar)");
    Fill(db.dispatcher(), "keep", 500);
    out.keep_rows = 500;
    Ok(db, "CREATE TABLE t (id int64, n int64, v varchar)");
    for (int done = 0; done < 1500;) {
        std::string sql = "INSERT INTO t VALUES ";
        for (int k = 0; k < 100 && done < 1500; ++k, ++done) {
            if (k > 0) sql += ", ";
            sql += "(" + std::to_string(done) + ", 'row" + std::to_string(done) + "')";
        }
        Ok(db, sql);
    }
    // Past the inline width, so the values spill and the var-heap chain grows.
    const std::string spill(300, 's');
    for (int k = 0; k < 80; ++k) Ok(db, "INSERT INTO t VALUES (" + std::to_string(k) + ", '" + spill + "')");
    Ok(db, "CREATE INDEX t_n ON t (n)");
    out.dropped.oid = DescribeField(db.dispatcher(), "t", "oid");
    out.dropped.pages = PagesOf(db.store(), out.dropped.oid);
    Ok(db, "DROP TABLE t");
    CleanShutdownCheckpoint(db);
    return out;
}

// What a reclaim must leave, however a crash cut it: every page of the
// dropped relation's ledger free once a mount has driven it to the end, the
// live relation whole, and nothing refused.
void ExpectReclaimedAndWhole(Expeditor& db, const Ledger& ledger, const std::string& where) {
    Settle(db);
    EXPECT_EQ(db.reclaim_counters().pending.load(), 0u) << where;
    EXPECT_EQ(db.reclaim_counters().refused.load(), 0u) << where;
    for (const PageId id : ledger.dropped.pages) {
        // An index page carries the index's oid, so the ledger holds the
        // relation-owned pages; each must be free.
        EXPECT_FALSE(db.store().IsAllocated(id)) << where << ": page " << id << " still allocated";
    }
    EXPECT_EQ(db.dispatcher().Dispatch("SELECT COUNT(*) FROM keep").response,
              "count(*)\\n" + std::to_string(ledger.keep_rows))
        << where;
}

TEST(DropTableReclaimGateTest, AMountAtTwoCoresFreesNothingBeforeEveryCoreHasPublished) {
    // BF-R4's `cores > 1` arm, and the premise cell's other half: the
    // completion checkpoint left `D` at the mount's anchor, so the reclaim
    // must wait - and a crash in the wait must find a volume that mounts.
    TempDir dir;
    const PageId leaf = VolumeWithADroppedLeafInItsReplayRange(dir.path);
    ASSERT_NE(leaf, kInvalidPageId);
    TempDir cut;
    {
        auto opened = Mount(dir.path, /*cores=*/2);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        ASSERT_EQ(db.reclaim_counters().pending.load(), 1u);
        for (int step = 0; step < 32; ++step) ASSERT_TRUE(db.ReclaimStep().ok());
        EXPECT_TRUE(db.store().IsAllocated(leaf)) << "freed before every core published";
        EXPECT_EQ(db.reclaim_counters().pending.load(), 1u);
        EXPECT_TRUE(db.store().PersistMaps().ok());
        CrashImage(dir.path, cut.path);
    }
    auto again = Mount(cut.path, /*cores=*/2);
    EXPECT_TRUE(again.ok()) << "the crash in the wait refused the mount: "
                            << again.status().message();
}

TEST(DropTableReclaimCrashTest, AReclaimCutAnywhereIsFinishedByTheNextMount) {
    // Every cut a crash can make in a reclaim - after each free, after each
    // map sync, after the tombstone's clear - taken as a crash image and
    // mounted: the next mount drives the reclaim again and it ends with every
    // page free, the live relation whole and nothing refused (BF-R7). The
    // var-heap chain is freed tail-first, a sync a page, so a cut inside it
    // leaves a prefix still linked from the root the tombstone names.
    TempDir dir;
    const Ledger ledger = TwoRelationsOneDropped(dir.path);
    ASSERT_GE(ledger.dropped.pages.size(), 20u);

    // Two passes over the same volume. The first takes each image as the
    // reclaim left the device. The second also flushes the map at every free
    // first - another core's checkpoint landing the map mid-plan - which is
    // what shows a chain freed head-first with one sync at its end: its head
    // reaches the device before its tail (the review's mutation).
    TempDir images;
    std::vector<std::pair<fs::path, std::string>> cuts;
    TempDir again;
    fs::copy(dir.path, again.path, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    for (const bool flush_maps : {false, true}) {
        const fs::path volume = flush_maps ? again.path : dir.path;
        auto opened = Mount(volume, /*cores=*/1);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        db.SetReclaimCutForTest([&, flush_maps, volume](const char* what) {
            if (flush_maps && std::string(what) == "after a free") {
                ASSERT_TRUE(db.store().PersistMaps().ok());
            }
            ASSERT_TRUE(db.wal().Flush().ok());
            const fs::path image = images.path / std::to_string(cuts.size());
            CrashImage(volume, image);
            cuts.emplace_back(image, std::string(flush_maps ? "map-flushed " : "") + "cut " +
                                         std::to_string(cuts.size()) + " (" + what + ")");
        });
        ExpectReclaimedAndWhole(db, ledger, "the run that cut");
        db.SetReclaimCutForTest(nullptr);
        EXPECT_GT(db.store().pages_freed(), ledger.dropped.pages.size())
            << "the index's pages, which carry the index's oid, were not freed";
    }  // both passes
    ASSERT_GE(cuts.size(), ledger.dropped.pages.size());
    for (const auto& [image, where] : cuts) {
        auto mounted = Mount(image, /*cores=*/1);
        ASSERT_TRUE(mounted.ok()) << where << ": the mount refused: " << mounted.status().message();
        CurrentCoreGuard as(0);
        ExpectReclaimedAndWhole(*mounted.value(), ledger, where);
    }
}

TEST(DropTableReclaimCrashTest, APageReusedBeforeTheClearIsNotFreedByTheRedrive) {
    // A crash after the frees and before the tombstone's clear, with the
    // freed ids already handed to a new relation: the re-drive walks from the
    // same roots, meets pages another owner holds, and frees none of them
    // (BF-R3's owner check).
    TempDir dir;
    const Ledger ledger = TwoRelationsOneDropped(dir.path);
    TempDir image;
    std::uint64_t reuse_rows = 0;
    {
        auto opened = Mount(dir.path, /*cores=*/1);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        bool taken = false;
        db.SetReclaimCutForTest([&](const char* what) {
            if (taken || std::string(what) != "before the clear") return;
            taken = true;
            Ok(db, "CREATE TABLE u (id int64, v varchar)");
            Fill(db.dispatcher(), "u", 1500);
            reuse_rows = 1500;
            ASSERT_TRUE(db.Sync().ok());
            CrashImage(dir.path, image.path);
        });
        Settle(db);
        db.SetReclaimCutForTest(nullptr);
        ASSERT_TRUE(taken);
        ASSERT_GT(db.store().pages_reused(), 0u) << "u reused no freed id; the cell proves nothing";
    }
    auto mounted = Mount(image.path, /*cores=*/1);
    ASSERT_TRUE(mounted.ok()) << mounted.status().message();
    Expeditor& db = *mounted.value();
    CurrentCoreGuard as(0);
    ASSERT_EQ(db.reclaim_counters().pending.load(), 1u) << "the clear was not cut";
    Settle(db);
    EXPECT_GT(db.reclaim_counters().skipped.load(), 0u)
        << "the re-drive met none of u's pages, so the owner check was not exercised";
    EXPECT_EQ(db.reclaim_counters().pending.load(), 0u);
    EXPECT_EQ(db.dispatcher().Dispatch("SELECT COUNT(*) FROM u").response,
              "count(*)\\n" + std::to_string(reuse_rows));
    for (const PageId id : PagesOf(db.store(), DescribeField(db.dispatcher(), "u", "oid"))) {
        EXPECT_TRUE(db.store().IsAllocated(id)) << "u's page " << id << " was freed by the re-drive";
    }
}

TEST(DropTableReclaimCrashTest, ARelaxedDropCrashedBeforeItsCommitIsDurableFreesNothing) {
    // BF-R1: under relaxed durability the drop's commit is acknowledged before
    // it is on the device. A crash there makes it a loser: the retype's undo
    // restores the tombstone word to 0 with the row, and nothing is owed.
    TempDir dir;
    std::set<PageId> pages;
    TempDir image;
    {
        Expeditor::Config config;
        config.buffer_pool_frames = 4096;  // required (BE-R3)
        config.data_file = (dir.path / "kds.db").string();
        config.wal_dir = (dir.path / "wal").string();
        config.log_file = {};
        config.cores = 1;
        config.debug_text_port = 0;
        config.checkpoint_interval_ns = 0;
        config.wal_drain_interval_ns = 0;
        config.durability = wal::DurabilityClass::kRelaxed;
        auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        Ok(db, "CREATE TABLE t (id int64, v varchar)");
        Fill(db.dispatcher(), "t", 1000);
        pages = PagesOf(db.store(), DescribeField(db.dispatcher(), "t", "oid"));
        ASSERT_TRUE(db.Sync().ok());
        // The drop's records reach the log file, and its commit - relaxed,
        // so acknowledged before it is written - does not.
        Ok(db, "BEGIN");
        Ok(db, "DROP TABLE t");
        ASSERT_TRUE(db.wal().Flush().ok());
        Ok(db, "COMMIT");
        // The crash: what the device holds, the drop's commit not among it.
        CrashImage(dir.path, image.path);
    }
    auto mounted = Mount(image.path, /*cores=*/1);
    ASSERT_TRUE(mounted.ok()) << mounted.status().message();
    Expeditor& db = *mounted.value();
    CurrentCoreGuard as(0);
    EXPECT_GE(db.recovery().losers, 1u) << "the drop never reached the log; the cell tests nothing";
    EXPECT_EQ(db.reclaim_counters().pending.load(), 0u);
    Settle(db);
    EXPECT_EQ(db.dispatcher().Dispatch("SELECT COUNT(*) FROM t").response, "count(*)\\n1000");
    for (const PageId id : pages) EXPECT_TRUE(db.store().IsAllocated(id)) << "page " << id;
}

// ---- BF-S4: a mount-found reclaim at two cores, while serving --------------

// Two loopback ports nobody holds: the KWP port and the debug text port
// `STOP` is reachable on (`protocol.md` §12).
std::pair<std::uint16_t, std::uint16_t> TwoFreePorts() {
    std::uint16_t out[2] = {0, 0};
    int fds[2] = {-1, -1};
    for (int i = 0; i < 2; ++i) {
        fds[i] = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fds[i] < 0) break;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(fds[i], reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) break;
        socklen_t len = sizeof(addr);
        if (::getsockname(fds[i], reinterpret_cast<sockaddr*>(&addr), &len) != 0) break;
        out[i] = ntohs(addr.sin_port);
    }
    for (const int fd : fds) {
        if (fd >= 0) ::close(fd);
    }
    return {out[0], out[1]};
}

int ConnectLoopback(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) return fd;
    ::close(fd);
    return -1;
}

std::string SendLine(int fd, const std::string& line) {
    const std::string out = line + "\n";
    if (::write(fd, out.data(), out.size()) < 0) return "ERR write failed";
    std::string response;
    char buf[512];
    while (response.find('\n') == std::string::npos) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        response.append(buf, static_cast<std::size_t>(n));
    }
    if (!response.empty() && response.back() == '\n') response.pop_back();
    return response;
}

TEST(DropTableReclaimServingTest, ATwoCoreMountReclaimsOnceEveryCoreHasCheckpointedWhileServing) {
    // BF-R12's rig cell: the tombstone a two-core mount found is freed on
    // core 0's tick once every core's cadence checkpoint has moved `D` past
    // the mount (BF-R4's `cores > 1` arm), while a client writes another
    // relation through the text port the whole time.
    TempDir dir;
    const Dropped dropped = CreateFillDrop(dir.path, "t", 1000);
    ASSERT_FALSE(dropped.pages.empty());
    {
        auto opened = Mount(dir.path, /*cores=*/1);  // a live relation to serve
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        CurrentCoreGuard as(0);
        Ok(*opened.value(), "CREATE TABLE live (id int64, v varchar)");
        CleanShutdownCheckpoint(*opened.value());
    }

    Expeditor::Config config;
    config.buffer_pool_frames = 4096;  // required (BE-R3)
    config.data_file = (dir.path / "kds.db").string();
    config.wal_dir = (dir.path / "wal").string();
    config.log_file = {};
    config.cores = 2;
    config.checkpoint_interval_ns = 100'000'000;  // 100 ms: the warm-up ends quickly
    const auto [port, text_port] = TwoFreePorts();
    ASSERT_NE(port, 0);
    ASSERT_NE(text_port, 0);
    config.port = port;
    config.debug_text_port = text_port;
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    EXPECT_EQ(db.reclaim_counters().pending.load(), 1u);
    ASSERT_TRUE(db.Start().ok());
    const int client = ConnectLoopback(text_port);
    ASSERT_GE(client, 0);
    Status ran = Status::OK();
    std::thread reactor([&] { ran = db.RunUntilStopped(); });

    int written = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (db.reclaim_counters().pending.load() != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        const std::string reply = SendLine(client, "INSERT INTO live VALUES ('w')");
        EXPECT_FALSE(StartsWith(reply, "ERR")) << reply;
        ++written;
    }
    EXPECT_EQ(db.reclaim_counters().pending.load(), 0u) << "the reclaim never ran";
    EXPECT_EQ(db.reclaim_counters().refused.load(), 0u);
    const std::string counted = SendLine(client, "SELECT COUNT(*) FROM live");
    EXPECT_EQ(counted, "count(*)\\n" + std::to_string(written));
    (void)SendLine(client, "STOP");
    ::close(client);
    reactor.join();
    EXPECT_TRUE(ran.ok()) << ran.message();
    opened.value().reset();

    auto again = Mount(dir.path, /*cores=*/1);
    ASSERT_TRUE(again.ok()) << again.status().message();
    CurrentCoreGuard as(0);
    for (const PageId id : dropped.pages) {
        EXPECT_FALSE(again.value()->store().IsAllocated(id)) << "page " << id;
    }
    EXPECT_EQ(again.value()->dispatcher().Dispatch("SELECT COUNT(*) FROM live").response,
              "count(*)\\n" + std::to_string(written));
}

// ---- Red first: the defence checks (BF-R10) --------------------------------

// A leaf of relation `owner` holding one row whose Keystone id is `pk`.
PageId LeafHolding(storage::InMemoryPageStore& store, std::uint64_t owner, std::uint64_t pk) {
    auto created = store.CreateNew();
    EXPECT_TRUE(created.ok());
    auto view = heap::PageView::CreateEmptyAs(created.value().second.bytes(), /*min_key=*/1,
                                              PageType::kBtreeLeaf, owner);
    EXPECT_TRUE(view.ok());
    std::array<std::byte, 16> payload{};
    const std::uint64_t word = Keystone::Encode(pk, 0, 0).value();
    std::memcpy(payload.data(), &word, sizeof(word));
    EXPECT_TRUE(view.value().InsertTuple(payload, /*trx_id=*/1).ok());
    return created.value().first;
}

// `VerifyTupleAt` for a location remembered for relation `owner`. The
// verifier takes no owner at `df741e3c`; BF-R10 adds it, and passing
// `owner` through is the one line BF-S3 changes here.
exec::VerifiedTuple VerifyFor(storage::PageStore& store, PageId page, std::uint64_t pk,
                              std::uint64_t owner) {
    return exec::VerifyTupleAt(store, page, /*slot=*/0, pk, /*recorded_epoch=*/0, owner);
}

TEST(DropTableReclaimDefenceTest, TheVerifierMissesOnAnotherOwnersPage) {
    // Red at `df741e3c`: `VerifyTupleAt` checks the epoch and the Keystone
    // id, never the owner (`tuple_verify.hpp`). Keystone ids collide across
    // relations by design, so a location remembered for relation 88 that
    // names a page now owned by relation 77 - a reused id - validates
    // whenever 77's row there carries the pk 88's entry expects.
    storage::InMemoryPageStore store{kFirstUserPageId};
    const PageId page = LeafHolding(store, /*owner=*/77, /*pk=*/5);
    EXPECT_TRUE(VerifyFor(store, page, /*pk=*/5, /*owner=*/77).ok())
        << "the owner's own location must still verify";
    EXPECT_FALSE(VerifyFor(store, page, /*pk=*/5, /*owner=*/88).ok())
        << "a location remembered for relation 88 validated on 77's page";
}

TEST(DropTableReclaimDefenceTest, ReadTupleRefusesASlotThatLeavesThePage) {
    // Red at `df741e3c`: `ReadTuple` bounds the slot index and nothing else,
    // so a slot whose offset and length leave the page is read past it. On a
    // reused id a stale location can name such a slot. The page sits at the
    // front of a buffer twice its size, so today's read past it stays inside
    // memory the cell owns.
    alignas(8) std::array<std::byte, 2 * kPageSize> buffer{};
    std::span<std::byte, kPageSize> bytes(buffer.data(), kPageSize);
    auto view = heap::PageView::CreateEmptyAs(bytes, /*min_key=*/1, PageType::kBtreeLeaf, 77);
    ASSERT_TRUE(view.ok());
    std::array<std::byte, 16> payload{};
    ASSERT_TRUE(view.value().InsertTuple(payload, /*trx_id=*/1).ok());
    // The page's one directory entry ends at `lower`; its offset word is the
    // entry's first two bytes and names the one tuple, at `upper`.
    const std::size_t entry = view.value().lower() - heap::kSlotOnDiskSize;
    std::uint16_t was = 0;
    std::memcpy(&was, bytes.data() + entry + heap::kSlotOffsetOffset, sizeof(was));
    ASSERT_EQ(was, view.value().upper()) << "not slot 0's offset word";
    const std::uint16_t past = static_cast<std::uint16_t>(kPageSize - 8);
    std::memcpy(bytes.data() + entry + heap::kSlotOffsetOffset, &past, sizeof(past));

    auto tuple = view.value().ReadTuple(0);
    EXPECT_FALSE(tuple.ok()) << "a slot at offset " << past << " was read past the page";
    if (!tuple.ok()) EXPECT_EQ(tuple.status().code(), StatusCode::kCorruption);
}

// ---- Guards: green at `df741e3c`, kept green -------------------------------

TEST(DropTableReclaimGuardTest, ARolledBackDropLeavesTheRelationWholeAcrossARestart) {
    TempDir dir;
    std::set<PageId> pages;
    std::uint64_t oid = 0;
    {
        auto opened = Mount(dir.path, /*cores=*/1);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        Ok(db, "CREATE TABLE t (id int64, v varchar)");
        Fill(db.dispatcher(), "t", 1000);
        oid = DescribeField(db.dispatcher(), "t", "oid");
        pages = PagesOf(db.store(), oid);
        Ok(db, "BEGIN");
        Ok(db, "DROP TABLE t");
        Ok(db, "ROLLBACK");
        CleanShutdownCheckpoint(db);
    }
    auto opened = Mount(dir.path, /*cores=*/1);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    Settle(db);
    EXPECT_EQ(PagesOf(db.store(), oid), pages);
    EXPECT_EQ(db.dispatcher().Dispatch("SELECT COUNT(*) FROM t").response, "count(*)\\n1000");
}

}  // namespace
}  // namespace kds::server
