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

#include <unistd.h>

#include <gtest/gtest.h>

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

// `rows` rows into `t (id int64, v varchar)`, the pk issued, 100 a statement.
void Fill(Expeditor& db, const std::string& table, int rows) {
    for (int done = 0; done < rows;) {
        std::string sql = "INSERT INTO " + table + " VALUES ";
        for (int k = 0; k < 100 && done < rows; ++k, ++done) {
            if (k > 0) sql += ", ";
            sql += "('row" + std::to_string(done) + "')";
        }
        Ok(db, sql);
    }
}

// The relation's `oid=` and `root_page_id=`, from `DESCRIBE`.
std::uint64_t DescribeField(Expeditor& db, const std::string& table, const std::string& key) {
    const std::string shape = db.dispatcher().Dispatch("DESCRIBE " + table).response;
    const std::string field = key + "=";
    std::size_t at = StartsWith(shape, field) ? 0 : shape.find(" " + field);
    EXPECT_NE(at, std::string::npos) << shape;
    if (at == std::string::npos) return 0;
    if (at != 0) ++at;
    return std::strtoull(shape.c_str() + at + field.size(), nullptr, 10);
}

// Every allocated id at or above the first user page, with its header's
// `owner_oid` (0 for a headerless page).
std::vector<std::pair<PageId, std::uint64_t>> AllocatedPages(Expeditor& db) {
    storage::DevicePageStore& store = db.store();
    std::vector<std::pair<PageId, std::uint64_t>> out;
    const PageId end = kFirstUserPageId + 4 * store.allocated_pages() + 1024;
    for (PageId id = kFirstUserPageId; id < end; ++id) {
        if (!store.IsAllocated(id) || storage::IsMapPageId(id)) continue;
        if (store.IsHeaderless(id)) {
            out.emplace_back(id, 0);
            continue;
        }
        auto page = store.GetForRead(id);
        EXPECT_TRUE(page.ok()) << id << ": " << page.status().message();
        out.emplace_back(id, page.ok() ? storage::GetOwnerOid(page.value().bytes()) : 0);
    }
    return out;
}

// The ids of the pages stamped with `oid`.
std::set<PageId> PagesOf(Expeditor& db, std::uint64_t oid) {
    std::set<PageId> out;
    for (const auto& [id, owner] : AllocatedPages(db)) {
        if (owner == oid) out.insert(id);
    }
    return out;
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

// **What a mount does with a pending reclaim before a cell looks.** Nothing
// at `df741e3c`: no reclaim exists. BF-S3 drives core 0's reclaim tick here
// until no tombstone is pending.
void Settle(Expeditor& db) { (void)db; }

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
    page[storage::kPageBodyOffset + (index >> 3)] &=
        static_cast<std::byte>(~(1u << (index & 7)));
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
    Fill(db, "t", 3);
    leaf = static_cast<PageId>(DescribeField(db, "t", "root_page_id"));
    CleanShutdownCheckpoint(db);
    Fill(db, "t", 3);
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
Dropped CreateFillDrop(const fs::path& dir, const std::string& table, int rows) {
    Dropped out;
    auto opened = Mount(dir, /*cores=*/1);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    if (!opened.ok()) return out;
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    Settle(db);
    Ok(db, "CREATE TABLE " + table + " (id int64, v varchar)");
    Fill(db, table, rows);
    out.oid = DescribeField(db, table, "oid");
    out.pages = PagesOf(db, out.oid);
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
    EXPECT_TRUE(PagesOf(db, dropped.oid).empty());
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
    Fill(db, "u", 1000);
    const std::set<PageId> reused = PagesOf(db, DescribeField(db, "u", "oid"));
    std::vector<PageId> common;
    std::set_intersection(dropped.pages.begin(), dropped.pages.end(), reused.begin(), reused.end(),
                          std::back_inserter(common));
    EXPECT_FALSE(common.empty()) << "no page of u is on an id t held";
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
                              [[maybe_unused]] std::uint64_t owner) {
    return exec::VerifyTupleAt(store, page, /*slot=*/0, pk, /*recorded_epoch=*/0);
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
        Fill(db, "t", 1000);
        oid = DescribeField(db, "t", "oid");
        pages = PagesOf(db, oid);
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
    EXPECT_EQ(PagesOf(db, oid), pages);
    EXPECT_EQ(db.dispatcher().Dispatch("SELECT COUNT(*) FROM t").response, "count(*)\\n1000");
}

}  // namespace
}  // namespace kds::server
