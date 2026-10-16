#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "reclaim_census.hpp"

#include "kds/base/current_core.hpp"
#include "kds/server/expeditor.hpp"
#include "kds/server/page_reclaim.hpp"
#include "kds/storage/anchor_page.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/wal/record.hpp"

// BY-S2 (`instructions/v3.2.0/workorder-by-reindex-drop-index-reclaim.md`):
// a dropped index's tree, and a rolled-back create's, go back to the
// allocator once nothing can replay or read them - BF's reclaim aimed at one
// tree (BY-R2, BY-R4) - and the slot that owed it is removed (BY-R5). The
// cells run on a mounted volume with no cadence checkpoint, so the gate
// (BF-R4) opens only where a cell checkpoints.

namespace kds::server {
namespace {

namespace fs = std::filesystem;
using reclaim_census::Fill;
using reclaim_census::PagesOf;

struct TempDir {
    fs::path path;
    TempDir() {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("kds_by_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

StatusOr<std::unique_ptr<Expeditor>> Mount(const fs::path& dir) {
    Expeditor::Config config;
    config.buffer_pool_frames = 4096;  // required (BE-R3)
    config.data_file = (dir / "kds.db").string();
    config.wal_dir = (dir / "wal").string();
    config.log_file = {};
    config.cores = 1;
    config.debug_text_port = 0;
    config.checkpoint_interval_ns = 0;
    return Expeditor::Open(config, /*now_unix_seconds=*/1000);
}

void Ok(Expeditor& db, const std::string& sql, Session* session = nullptr) {
    const std::string reply = db.dispatcher().Dispatch(sql, session).response;
    ASSERT_NE(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
}

// A checkpoint whose redo start is itself (drop_table_reclaim_test.cpp's
// shape): pages first, so the dirty table is empty.
void CleanCheckpoint(Expeditor& db) {
    ASSERT_TRUE(db.store().Sync().ok());
    ASSERT_TRUE(db.Checkpoint().ok());
}

// Core 0's reclaim tick, driven by hand until nothing is owed or a job is
// refused.
void Settle(Expeditor& db) {
    for (int step = 0; step < 100000; ++step) {
        if (db.reclaim_counters().pending.load() == 0 ||
            db.reclaim_counters().refused.load() != 0) {
            break;
        }
        ASSERT_TRUE(db.ReclaimStep().ok());
    }
}

catalog::Oid IndexOid(Expeditor& db, const std::string& name) {
    auto row = db.catalog().FindIndexByName(name);
    EXPECT_TRUE(row.ok()) << row.status().message();
    return row.ok() ? row.value().index_oid : 0;
}

// The slots `table`'s anchor holds.
std::vector<catalog::Oid> Slots(Expeditor& db, const std::string& table) {
    auto oid = db.catalog().FindTableOidByName(catalog::kNamespacePublic, table);
    EXPECT_TRUE(oid.ok());
    auto row = db.catalog().GetSysTableRow(oid.value());
    EXPECT_TRUE(row.ok());
    auto page = db.store().GetForRead(row.value().anchor_page_id);
    EXPECT_TRUE(page.ok());
    auto slots = storage::AnchorIndexSlots(page.value().bytes());
    EXPECT_TRUE(slots.ok());
    std::vector<catalog::Oid> out;
    for (const auto& slot : slots.value()) out.push_back(slot.index_oid);
    return out;
}

bool Holds(const std::vector<catalog::Oid>& slots, catalog::Oid oid) {
    return std::find(slots.begin(), slots.end(), oid) != slots.end();
}

// A relation of 3,000 rows and an index on its string column: a tree of
// several levels.
void Seed(Expeditor& db) {
    Ok(db, "CREATE TABLE t (id int64, v varchar)");
    Fill(db.dispatcher(), "t", 3000);
    Ok(db, "CREATE INDEX by_v ON t (v)");
}

TEST(IndexReclaimJobTest, ADroppedIndexsTreeReturnsOnlyOnceTheRedoStartPassesTheDrop) {
    TempDir dir;
    auto opened = Mount(dir.path);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    Seed(db);
    const catalog::Oid ix = IndexOid(db, "by_v");
    const std::set<PageId> tree = PagesOf(db.store(), ix);
    ASSERT_GE(tree.size(), 4u) << "a tree of several levels";

    Ok(db, "DROP INDEX by_v");
    Settle(db);
    EXPECT_EQ(PagesOf(db.store(), ix), tree) << "freed before the redo start passed the drop";

    CleanCheckpoint(db);
    Settle(db);
    EXPECT_EQ(db.reclaim_counters().refused.load(), 0u);
    EXPECT_EQ(db.reclaim_counters().pending.load(), 0u);
    for (const PageId id : tree) EXPECT_FALSE(db.store().IsAllocated(id)) << "page " << id;
    EXPECT_FALSE(Holds(Slots(db, "t"), ix)) << "the slot outlived its tree";
    Ok(db, "SELECT id FROM t WHERE v = 'row7'");
}

TEST(IndexReclaimJobTest, ARolledBackDropOwesNothing) {
    TempDir dir;
    auto opened = Mount(dir.path);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    Seed(db);
    const catalog::Oid ix = IndexOid(db, "by_v");
    const std::set<PageId> tree = PagesOf(db.store(), ix);
    Session s;
    Ok(db, "BEGIN", &s);
    Ok(db, "DROP INDEX by_v", &s);
    Ok(db, "ROLLBACK", &s);
    CleanCheckpoint(db);
    Settle(db);
    EXPECT_EQ(PagesOf(db.store(), ix), tree);
    EXPECT_TRUE(Holds(Slots(db, "t"), ix));
    EXPECT_EQ(db.dispatcher().Dispatch("SELECT id FROM t WHERE v = 'row7'").response, "id\\n8");
}

TEST(IndexReclaimJobTest, ARolledBackCreatesLoggedTreeIsReclaimed) {
    TempDir dir;
    auto opened = Mount(dir.path);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    Ok(db, "CREATE TABLE t (id int64, v varchar)");
    Fill(db.dispatcher(), "t", 3000);
    Session s;
    Ok(db, "BEGIN", &s);
    Ok(db, "CREATE INDEX by_v ON t (v)", &s);
    const catalog::Oid ix = IndexOid(db, "by_v");
    const std::set<PageId> tree = PagesOf(db.store(), ix);
    ASSERT_GE(tree.size(), 4u);
    Ok(db, "ROLLBACK", &s);
    // One checkpoint opens the gate: a rollback's gate is the log's end at
    // the decide, where the checkpoint's first record lands.
    CleanCheckpoint(db);
    Settle(db);
    EXPECT_EQ(db.reclaim_counters().refused.load(), 0u);
    EXPECT_EQ(db.reclaim_counters().pending.load(), 0u);
    EXPECT_TRUE(PagesOf(db.store(), ix).empty());
    EXPECT_FALSE(Holds(Slots(db, "t"), ix));
}

TEST(IndexReclaimJobTest, TheDroppersOwnInsertsGrowTheTreeAndTheWholeTreeIsReclaimed) {
    // BY-R2: the root is read from the slot when the job walks. The
    // dropper's own writes still maintain the index it delete-marked, so a
    // root taken at the drop would reach only part of the tree.
    TempDir dir;
    auto opened = Mount(dir.path);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    Ok(db, "CREATE TABLE t (id int64, v varchar)");
    Fill(db.dispatcher(), "t", 100);
    Ok(db, "CREATE INDEX by_v ON t (v)");
    const catalog::Oid ix = IndexOid(db, "by_v");
    const std::size_t before = PagesOf(db.store(), ix).size();
    Session s;
    Ok(db, "BEGIN", &s);
    Ok(db, "DROP INDEX by_v", &s);
    for (int i = 0; i < 30; ++i) {
        std::string sql = "INSERT INTO t VALUES ";
        for (int k = 0; k < 100; ++k) {
            if (k > 0) sql += ", ";
            sql += "('own" + std::to_string(i * 100 + k) + "')";
        }
        Ok(db, sql, &s);
    }
    ASSERT_GT(PagesOf(db.store(), ix).size(), before + 2)
        << "the dropper's inserts did not grow the dropped index";
    Ok(db, "COMMIT", &s);
    CleanCheckpoint(db);
    Settle(db);
    EXPECT_EQ(db.reclaim_counters().refused.load(), 0u);
    EXPECT_TRUE(PagesOf(db.store(), ix).empty()) << "a page the grown tree took was left";
}

TEST(IndexReclaimJobTest, ARelationsJobAheadWithTheLaterGateOwnsTheIndexTree) {
    // BY-R3, forced (BY-S2's review): across cores a `DROP TABLE` can queue
    // its job before an earlier `DROP INDEX` queues its own, so the
    // relation's job sits ahead with the later gate. Driven on a reclaimer of
    // the cell's own: a step with the redo start between the gates lets only
    // the index job move - partway, its tree being larger than a step - then
    // the relation's job walks every slot and frees what is left, another
    // relation reuses the freed ids, and only then could the index job's
    // stale plan run on. Without BY-R3 it frees those reused, live pages.
    TempDir dir;
    auto opened = Mount(dir.path);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    Ok(db, "CREATE TABLE t (id int64, v varchar)");
    Fill(db.dispatcher(), "t", 8000);
    Ok(db, "CREATE INDEX by_v ON t (v)");
    Ok(db, "CREATE TABLE u (id int64, v varchar)");
    const catalog::Oid ix = IndexOid(db, "by_v");
    const catalog::Oid rel =
        db.catalog().FindTableOidByName(catalog::kNamespacePublic, "t").value();
    const catalog::Oid other =
        db.catalog().FindTableOidByName(catalog::kNamespacePublic, "u").value();
    const auto row = db.catalog().GetSysTableRow(rel).value();
    const PageId anchor = row.anchor_page_id;
    // The roots the drop's tombstone records (BF-R2).
    const catalog::PendingRoots roots{anchor, row.varheap_page_id};
    ASSERT_GT(PagesOf(db.store(), ix).size(), kReclaimBatchPages / 2)
        << "the index job must still hold a plan after one step";
    Ok(db, "DROP INDEX by_v");
    Ok(db, "DROP TABLE t");

    ReclaimCounters counters;
    PageReclaimer reclaimer(db.catalog(), db.store(), counters, /*log=*/nullptr);
    constexpr wal::Lsn kLate = 1'000'000'000'000;
    reclaimer.EnqueueCommittedDrop(rel, roots, /*commit_lsn=*/kLate, /*word=*/0);
    reclaimer.EnqueueOrphanedIndex(rel, anchor, ix, /*gate=*/1, /*word=*/0);
    ASSERT_TRUE(reclaimer.Step(/*durable_redo_start=*/10, nullptr).ok());
    Fill(db.dispatcher(), "u", 300);
    for (int step = 0; step < 10000 && counters.pending.load() == 2; ++step) {
        ASSERT_TRUE(reclaimer.Step(kLate + 1, nullptr).ok());
    }
    Fill(db.dispatcher(), "u", 3000);  // takes the ids the relation's job freed
    const std::set<PageId> live = PagesOf(db.store(), other);
    for (int step = 0; step < 10000 && counters.pending.load() != 0 &&
                       counters.refused.load() == 0;
         ++step) {
        ASSERT_TRUE(reclaimer.Step(kLate + 1, nullptr).ok());
    }
    EXPECT_EQ(counters.refused.load(), 0u);
    for (const PageId id : live) {
        EXPECT_TRUE(db.store().IsAllocated(id)) << "page " << id << " of the live relation u";
    }
    EXPECT_EQ(db.dispatcher().Dispatch("SELECT COUNT(*) FROM u").response, "count(*)\\n3300");
}

TEST(IndexReclaimJobTest, ADropTableAfterADropIndexFreesEveryPageOnceAndOwesNothing) {
    // BY-R3: the relation's job owns every slot, so the index's job is
    // dropped before it walks.
    TempDir dir;
    auto opened = Mount(dir.path);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    Seed(db);
    const catalog::Oid ix = IndexOid(db, "by_v");
    const auto rel = db.catalog().FindTableOidByName(catalog::kNamespacePublic, "t").value();
    const std::size_t pages = PagesOf(db.store(), ix).size() + PagesOf(db.store(), rel).size();
    const std::uint64_t freed_before = db.store().pages_freed();
    Ok(db, "DROP INDEX by_v");
    Ok(db, "DROP TABLE t");
    CleanCheckpoint(db);
    Settle(db);
    EXPECT_EQ(db.reclaim_counters().refused.load(), 0u);
    EXPECT_EQ(db.reclaim_counters().pending.load(), 0u);
    EXPECT_TRUE(PagesOf(db.store(), ix).empty());
    EXPECT_TRUE(PagesOf(db.store(), rel).empty());
    EXPECT_EQ(db.store().pages_freed() - freed_before, pages) << "a page was freed twice";
}

TEST(IndexReclaimJobTest, CreateDropReclaimCyclesNeverReachTheSlotCap) {
    // Before BY about 679 create-then-drop cycles filled the anchor and the
    // next `CREATE INDEX` was refused. A reclaimed slot is removed (BY-R5).
    TempDir dir;
    auto opened = Mount(dir.path);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    Ok(db, "CREATE TABLE t (id int64, v varchar)");
    for (std::size_t i = 0; i < storage::kAnchorMaxIndexEntries + 20; ++i) {
        Ok(db, "CREATE INDEX ix" + std::to_string(i) + " ON t (v)");
        Ok(db, "DROP INDEX ix" + std::to_string(i));
        if (i % 100 == 99) {
            CleanCheckpoint(db);
            Settle(db);
        }
    }
    Ok(db, "CREATE INDEX last ON t (v)");
    EXPECT_EQ(db.reclaim_counters().refused.load(), 0u);
}

TEST(IndexReclaimJobTest, AReindexsOldTreeIsReclaimedAndTheNewOneServes) {
    // BY-R6: the old tree is queued at the REINDEX's commit, as a drop's.
    TempDir dir;
    auto opened = Mount(dir.path);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    Seed(db);
    const catalog::Oid old_ix = IndexOid(db, "by_v");
    const std::set<PageId> old_tree = PagesOf(db.store(), old_ix);
    Ok(db, "REINDEX INDEX by_v");
    const catalog::Oid new_ix = IndexOid(db, "by_v");
    ASSERT_NE(new_ix, old_ix);
    CleanCheckpoint(db);
    Settle(db);
    EXPECT_EQ(db.reclaim_counters().refused.load(), 0u);
    for (const PageId id : old_tree) EXPECT_FALSE(db.store().IsAllocated(id)) << "page " << id;
    EXPECT_FALSE(Holds(Slots(db, "t"), old_ix));
    EXPECT_TRUE(Holds(Slots(db, "t"), new_ix));
    EXPECT_FALSE(PagesOf(db.store(), new_ix).empty());
    EXPECT_EQ(db.dispatcher().Dispatch("SELECT id FROM t WHERE v = 'row7'").response, "id\\n8");
}

void CrashImage(const fs::path& from, const fs::path& to);

TEST(IndexReclaimJobTest, ACrashAfterAReindexMountsOneIndexAndOwesTheOldTree) {
    TempDir dir;
    catalog::Oid old_ix = 0;
    TempDir image;
    {
        auto opened = Mount(dir.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        Seed(db);
        old_ix = IndexOid(db, "by_v");
        Ok(db, "REINDEX INDEX by_v");
        ASSERT_TRUE(db.wal().Flush().ok());
        CrashImage(dir.path, image.path);
    }
    auto mounted = Mount(image.path);
    ASSERT_TRUE(mounted.ok()) << mounted.status().message();
    Expeditor& db = *mounted.value();
    CurrentCoreGuard as(0);
    auto rows = db.catalog().ListIndexes();
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows.value().size(), 1u) << "one index by the name after the crash";
    EXPECT_NE(rows.value()[0].index_oid, old_ix);
    Settle(db);
    EXPECT_EQ(db.reclaim_counters().refused.load(), 0u);
    EXPECT_TRUE(PagesOf(db.store(), old_ix).empty()) << "the old tree was owed at mount";
    EXPECT_EQ(db.dispatcher().Dispatch("SELECT id FROM t WHERE v = 'row7'").response, "id\\n8");
}

// ---- A crash anywhere in the job ------------------------------------------

void CrashImage(const fs::path& from, const fs::path& to) {
    fs::create_directories(to);
    fs::copy_file(from / "kds.db", to / "kds.db", fs::copy_options::overwrite_existing);
    fs::copy(from / "wal", to / "wal",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
}

fs::path SegmentOf(const fs::path& image, std::uint64_t segment_size, wal::Lsn lsn) {
    return image / "wal" / ("wal-0-" + std::to_string(lsn / segment_size) + ".log");
}

// Zeroes `image`'s log from `lsn` to the end of its segment: the log a crash
// at `lsn` leaves. A segment is pre-written zero (`FileLogDevice`'s
// `Prewrite`), so this is a tail never written (an LSN is a byte offset,
// `wal.md` §1).
void CutLogAt(const fs::path& image, std::uint64_t segment_size, wal::Lsn lsn) {
    const fs::path segment = SegmentOf(image, segment_size, lsn);
    std::fstream file(segment, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file.is_open()) << segment;
    file.seekp(static_cast<std::streamoff>(lsn % segment_size));
    const std::vector<char> zeros(segment_size - lsn % segment_size, 0);
    file.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    ASSERT_TRUE(file.good()) << segment;
}

// Every record `image`'s log holds in [from, to): its start and its type.
std::vector<std::pair<wal::Lsn, wal::RecordType>> RecordsBetween(const fs::path& image,
                                                                 std::uint64_t segment_size,
                                                                 wal::Lsn from, wal::Lsn to) {
    std::ifstream file(SegmentOf(image, segment_size, from), std::ios::binary);
    EXPECT_TRUE(file.is_open());
    std::vector<std::byte> bytes(to - from);
    file.seekg(static_cast<std::streamoff>(from % segment_size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    EXPECT_TRUE(file.good());
    std::vector<std::pair<wal::Lsn, wal::RecordType>> out;
    wal::RecordReader reader(bytes, from);
    for (wal::Lsn at = from; auto record = reader.Next(); at = reader.end_lsn()) {
        out.emplace_back(at, record->type());
    }
    EXPECT_EQ(reader.end_lsn(), to) << "the log between the flushes did not decode whole";
    return out;
}

TEST(IndexReclaimJobTest, ACrashAtEveryRecordOfAReindexMountsTheOldIndexOrTheNewNeverNeither) {
    // BY-S3's crash cell: the log cut at the start of every record the
    // `REINDEX` wrote - the old row's mark, the new tree's images, the new
    // slot and row, the commit - and at its end, each over the data file as
    // it was before the statement, so no page is ahead of the cut log. A cut
    // before the commit rolls the swap back whole and the old index serves;
    // a cut past it, the new one; never neither, never both.
    TempDir dir;
    TempDir images;
    catalog::Oid old_ix = 0;
    wal::Lsn from = 0;
    wal::Lsn to = 0;
    std::uint64_t segment_size = 0;
    {
        auto opened = Mount(dir.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        Ok(db, "CREATE TABLE t (id int64, v varchar)");
        Fill(db.dispatcher(), "t", 200);
        Ok(db, "CREATE INDEX by_v ON t (v)");
        CleanCheckpoint(db);
        ASSERT_TRUE(db.wal().Flush().ok());
        from = db.wal().flushed_lsn();
        CrashImage(dir.path, images.path / "before");
        old_ix = IndexOid(db, "by_v");
        Ok(db, "REINDEX INDEX by_v");
        ASSERT_TRUE(db.wal().Flush().ok());
        to = db.wal().flushed_lsn();
        segment_size = db.wal().segment_size();
        CrashImage(dir.path, images.path / "after");
    }
    ASSERT_GT(to, from);
    ASSERT_EQ(from / segment_size, to / segment_size) << "the cell cuts one segment";
    const auto records = RecordsBetween(images.path / "after", segment_size, from, to);
    const auto commit = std::find_if(records.begin(), records.end(), [](const auto& r) {
        return r.second == wal::RecordType::kTxnCommit;
    });
    ASSERT_NE(commit, records.end()) << "the REINDEX logged no commit";
    ASSERT_GE(commit - records.begin(), 3) << "a mark, a tree and a row come before it";

    std::vector<wal::Lsn> cuts;
    for (const auto& record : records) cuts.push_back(record.first);
    cuts.push_back(to);
    for (std::size_t i = 0; i < cuts.size(); ++i) {
        const wal::Lsn cut = cuts[i];
        const bool committed = cut > commit->first;
        const std::string where = "cut " + std::to_string(i) + " at +" +
                                  std::to_string(cut - from) + " of " +
                                  std::to_string(to - from);
        const fs::path image = images.path / std::to_string(i);
        fs::create_directories(image);
        fs::copy_file(images.path / "before" / "kds.db", image / "kds.db");
        fs::copy(images.path / "after" / "wal", image / "wal", fs::copy_options::recursive);
        CutLogAt(image, segment_size, cut);
        auto mounted = Mount(image);
        ASSERT_TRUE(mounted.ok()) << where << ": " << mounted.status().message();
        Expeditor& db = *mounted.value();
        CurrentCoreGuard as(0);
        auto rows = db.catalog().ListIndexes();
        ASSERT_TRUE(rows.ok()) << where;
        ASSERT_EQ(rows.value().size(), 1u) << where;
        EXPECT_EQ(rows.value()[0].index_oid == old_ix, !committed) << where;
        Settle(db);
        EXPECT_EQ(db.reclaim_counters().refused.load(), 0u) << where;
        EXPECT_EQ(db.dispatcher().Dispatch("SELECT id FROM t WHERE v = 'row7'").response,
                  "id\\n8")
            << where;
    }
}


TEST(IndexReclaimJobTest, AJobCutAnywhereIsFinishedByTheNextMount) {
    // Every cut a crash can make - after each free, each map sync, and on
    // each side of the slot's removal - taken as an image and mounted: the
    // next mount's owed-slot scan (BY-R1) drives the job again, and it ends
    // with the tree free, the slot gone and nothing refused.
    TempDir dir;
    std::set<PageId> tree;
    catalog::Oid ix = 0;
    {
        auto opened = Mount(dir.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        Seed(db);
        ix = IndexOid(db, "by_v");
        tree = PagesOf(db.store(), ix);
        Ok(db, "DROP INDEX by_v");
        CleanCheckpoint(db);
    }
    TempDir images;
    std::vector<std::pair<fs::path, std::string>> cuts;
    {
        auto opened = Mount(dir.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        db.SetReclaimCutForTest([&](const char* what) {
            ASSERT_TRUE(db.wal().Flush().ok());
            const fs::path image = images.path / std::to_string(cuts.size());
            CrashImage(dir.path, image);
            cuts.emplace_back(image, "cut " + std::to_string(cuts.size()) + " (" + what + ")");
        });
        Settle(db);
        db.SetReclaimCutForTest(nullptr);
        for (const PageId id : tree) EXPECT_FALSE(db.store().IsAllocated(id));
    }
    ASSERT_GE(cuts.size(), 3u);
    for (const auto& [image, where] : cuts) {
        auto mounted = Mount(image);
        ASSERT_TRUE(mounted.ok()) << where << ": " << mounted.status().message();
        Expeditor& db = *mounted.value();
        CurrentCoreGuard as(0);
        Settle(db);
        EXPECT_EQ(db.reclaim_counters().refused.load(), 0u) << where;
        EXPECT_TRUE(PagesOf(db.store(), ix).empty()) << where;
        EXPECT_FALSE(Holds(Slots(db, "t"), ix)) << where;
        EXPECT_EQ(db.dispatcher().Dispatch("SELECT COUNT(*) FROM t").response,
                  "count(*)\\n3000")
            << where;
    }
}

}  // namespace
}  // namespace kds::server
