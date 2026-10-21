// BR - backup and restore (`instructions/v3.0.0/workorder-br-backup-restore.md`,
// `br-s0-census.md`). Every cell runs a real instance on real files: a
// backup is a fact about the data file and the log on disk, and an
// in-memory device would test a copy of a copy.
//
// BR-S1, the recovery foundation: a restore owed starts at the backup's
// start, never the copied anchor (BR-R4), and does not open short of its
// stop (BR-R2). BR-S2, the control points: START pins `D`, STOP logs the
// stop and copies the window's log under a manifest.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/catalog/rows.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/server/backup_manifest.hpp"
#include "kds/server/expeditor.hpp"
#include "kds/server/restore.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/wal/file_log_device.hpp"
#include "kds/wal/log_scanner.hpp"
#include "view_text.hpp"

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
               ("kds_br_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// No cadence checkpoint: the only anchors are the mount's and the ones a
// cell asks for, so a cell knows where every start lies.
StatusOr<std::unique_ptr<Expeditor>> Mount(const fs::path& dir, std::uint32_t cores = 1) {
    Expeditor::Config config;
    config.buffer_pool_frames = 4096;
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

// The ids a `SELECT id` answers, in reply order (`id\n1\n2`, header first).
std::vector<std::uint64_t> Ids(Expeditor& db, const std::string& sql) {
    std::vector<std::uint64_t> out;
    const std::string reply = db.dispatcher().Dispatch(sql).response;
    EXPECT_FALSE(StartsWith(reply, "ERR")) << sql << " -> " << reply;
    std::size_t at = reply.find("\\n");
    while (at != std::string::npos) {
        const std::size_t next = reply.find("\\n", at + 2);
        out.push_back(std::stoull(reply.substr(
            at + 2, next == std::string::npos ? std::string::npos : next - at - 2)));
        at = next;
    }
    return out;
}

std::vector<std::uint64_t> Range(std::uint64_t first, std::uint64_t last) {
    std::vector<std::uint64_t> out;
    for (std::uint64_t id = first; id <= last; ++id) out.push_back(id);
    return out;
}

void Insert(Expeditor& db, std::uint64_t first, std::uint64_t last) {
    for (std::uint64_t id = first; id <= last; ++id) {
        Ok(db, "INSERT INTO t VALUES (" + std::to_string(id) + ", " + std::to_string(id) + ")");
    }
}

// What a clean stop does: pages first, so the anchor's redo start is the
// checkpoint itself and every page below it is on the device.
void CleanCheckpoint(Expeditor& db) {
    ASSERT_TRUE(db.store().Sync().ok());
    ASSERT_TRUE(db.Checkpoint().ok());
}

using PageBytes = std::array<std::byte, kPageSize>;

PageBytes ReadPage(const fs::path& file, PageId id) {
    PageBytes page{};
    std::ifstream f(file, std::ios::binary);
    f.seekg(static_cast<std::streamoff>(id) * kPageSize);
    f.read(reinterpret_cast<char*>(page.data()), kPageSize);
    EXPECT_TRUE(f.good());
    return page;
}

void WritePage(const fs::path& file, PageId id, const PageBytes& page) {
    std::fstream f(file, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(static_cast<std::streamoff>(id) * kPageSize);
    f.write(reinterpret_cast<const char*>(page.data()), kPageSize);
    ASSERT_TRUE(f.good());
}

// **The fuzzy copy BR-R4 is about**, built by hand: the data file as it
// stood at anchor `start` (rows 1..10 on the device), page 0 as a later
// checkpoint wrote it (its anchor past rows 11..20's records), and the whole
// log. Returns {start, stop}: the start a backup would have taken and the
// durable end of the log.
struct FuzzyImage {
    wal::Lsn start = 0;
    wal::Lsn copied_anchor = 0;
    wal::Lsn stop = 0;
};

FuzzyImage BuildFuzzyImage(const fs::path& live, const fs::path& image) {
    FuzzyImage out;
    auto opened = Mount(live);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    if (!opened.ok()) return out;
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    Ok(db, "CREATE TABLE t (id int64, v int64)");
    Insert(db, 1, 10);
    CleanCheckpoint(db);
    out.start = db.superblock().wal_anchor(0).redo_start_lsn;
    fs::create_directories(image);
    fs::copy_file(live / "kds.db", image / "kds.db");

    Insert(db, 11, 20);
    CleanCheckpoint(db);
    out.copied_anchor = db.superblock().wal_anchor(0).redo_start_lsn;
    Insert(db, 21, 30);
    EXPECT_TRUE(db.Sync().ok());
    out.stop = db.wal().durable_lsn();

    WritePage(image / "kds.db", kSuperBlockPageId, ReadPage(live / "kds.db", kSuperBlockPageId));
    fs::copy(live / "wal", image / "wal", fs::copy_options::recursive);
    opened.value().reset();
    return out;
}

void CopyVolume(const fs::path& from, const fs::path& to) {
    fs::create_directories(to);
    fs::copy_file(from / "kds.db", to / "kds.db", fs::copy_options::overwrite_existing);
    fs::copy(from / "wal", to / "wal",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
}

// ---- BR-S1 -------------------------------------------------------------------

// The hazard, pinned: mounted from the copied anchor, the fuzzy image cannot
// recover rows 11..20 - their leaf is the device's older copy and redo
// starts past their records. Mounted as a restore from the backup's
// start, every row is there, the mount says which backup it completed, and
// the next mount is an ordinary one.
void RestoreStartsAtTheBackupsStart(std::uint32_t cores) {
    TempDir live, image, restored;
    const FuzzyImage fuzzy = BuildFuzzyImage(live.path, image.path);
    ASSERT_NE(fuzzy.start, 0u);
    ASSERT_LT(fuzzy.start, fuzzy.copied_anchor);
    ASSERT_LT(fuzzy.copied_anchor, fuzzy.stop);
    CopyVolume(image.path, restored.path);

    {
        // Refused here by the sorted-leaf check (BD-R1), whose slot a row
        // 11..20 should have taken; a leaf with room past them would take
        // rows 21..30 and lose 11..20 in silence. Either way, not an open
        // with every row.
        auto plain = Mount(image.path);
        CurrentCoreGuard as(0);
        EXPECT_TRUE(!plain.ok() || Ids(*plain.value(), "SELECT id FROM t") != Range(1, 30))
            << "the copied anchor alone recovered every row - the cell no longer shows the "
               "hazard BR-R4 guards";
    }

    std::uint64_t lineage = 0;
    ASSERT_TRUE(MarkRestoreOwed((restored.path / "kds.db").string(),
                                {fuzzy.start, fuzzy.stop, /*backup_id=*/77}, &lineage)
                    .ok());
    ASSERT_NE(lineage, 0u);
    {
        auto opened = Mount(restored.path, cores);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        EXPECT_EQ(Ids(db, "SELECT id FROM t"), Range(1, 30));
        EXPECT_EQ(db.recovery().restore_backup_id, 77u);
        EXPECT_EQ(db.recovery().restore_stop_lsn, fuzzy.stop);
        EXPECT_FALSE(db.superblock().restore_owed());
        EXPECT_EQ(db.superblock().lineage_id(), lineage);
        const std::string meta = db.dispatcher().Dispatch("SHOW META").response;
        EXPECT_EQ(testing_view::MetaField(meta, "recovery_restore_backup"), "77") << meta;
        EXPECT_EQ(testing_view::MetaField(meta, "recovery_restore_stop"),
                  std::to_string(fuzzy.stop))
            << meta;
        EXPECT_EQ(testing_view::MetaField(meta, "wal_lineage"), std::to_string(lineage)) << meta;
        Insert(db, 31, 31);
    }
    {
        auto again = Mount(restored.path, cores);
        ASSERT_TRUE(again.ok()) << again.status().message();
        CurrentCoreGuard as(0);
        EXPECT_EQ(Ids(*again.value(), "SELECT id FROM t"), Range(1, 31));
        EXPECT_EQ(again.value()->recovery().restore_backup_id, 0u);
        EXPECT_EQ(again.value()->superblock().lineage_id(), lineage);
    }
}

TEST(BackupRestoreTest, ARestoreStartsAtTheBackupsStartNotTheCopiedAnchor) {
    RestoreStartsAtTheBackupsStart(/*cores=*/1);
}

// Above one core the checkpoint anchor's warm-up republishes the mount's
// anchor until every core has published (`FoldedAnchor`): the restore's
// slot 0, never the copied one.
TEST(BackupRestoreTest, ARestoreAtTwoCoresStartsAtTheBackupsStartToo) {
    RestoreStartsAtTheBackupsStart(/*cores=*/2);
}

// BR-R2: a log that ends below the stop does not open, says which backup
// and which stop, and goes on refusing - the fields stay owed.
TEST(BackupRestoreTest, ARestoreWhoseLogEndsShortOfItsStopDoesNotOpen) {
    TempDir live, image;
    const FuzzyImage fuzzy = BuildFuzzyImage(live.path, image.path);
    const wal::Lsn beyond = fuzzy.stop + 4096;
    ASSERT_TRUE(MarkRestoreOwed((image.path / "kds.db").string(), {fuzzy.start, beyond, 5}).ok());
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto opened = Mount(image.path);
        ASSERT_FALSE(opened.ok());
        const std::string& message = opened.status().message();
        EXPECT_NE(message.find("restore of backup 5"), std::string::npos) << message;
        EXPECT_NE(message.find("stop lsn " + std::to_string(beyond)), std::string::npos)
            << message;
        EXPECT_NE(message.find("does not open"), std::string::npos) << message;
    }
}

// Refused before any file is opened.
TEST(BackupRestoreTest, ARestoreWindowNamesABackupAndAStartBelowItsStop) {
    const std::string nowhere = "/nonexistent/kds.db";
    EXPECT_EQ(MarkRestoreOwed(nowhere, {900, 900, 1}).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(MarkRestoreOwed(nowhere, {100, 0, 1}).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(MarkRestoreOwed(nowhere, {0, 900, 1}).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(MarkRestoreOwed(nowhere, {100, 900, 0}).code(), StatusCode::kInvalidArgument);
}

// ---- BR-S2 -------------------------------------------------------------------

std::string Field(const std::string& reply, const std::string& key) {
    const std::string tag = key + "=";
    std::size_t at = reply.find(" " + tag);
    if (at == std::string::npos) return {};
    at += 1 + tag.size();
    return reply.substr(at, reply.find(' ', at) - at);
}

std::uint64_t U64(const std::string& reply, const std::string& key) {
    return std::strtoull(Field(reply, key).c_str(), nullptr, 10);
}

std::string Reply(Expeditor& db, const std::string& sql) {
    return db.dispatcher().Dispatch(sql).response;
}

// BR-R3, BR-Q4: START answers the durable redo start and pins it; a second
// START is refused naming the open one; ABORT ends it and releases the pin;
// a transaction cannot run one; every malformed spelling names its byte.
TEST(BackupControlTest, StartPinsTheDurableRedoStartAndOneRunsAtATime) {
    TempDir live;
    auto opened = Mount(live.path);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    Ok(db, "CREATE TABLE t (id int64, v int64)");
    Insert(db, 1, 5);
    CleanCheckpoint(db);
    const wal::Lsn d = db.superblock().wal_anchor(0).redo_start_lsn;

    const std::string started = Reply(db, "BACKUP START 'nightly'");
    ASSERT_TRUE(StartsWith(started, "OK ")) << started;
    const std::uint64_t id = U64(started, "backup_id");
    EXPECT_NE(id, 0u);
    EXPECT_EQ(U64(started, "start_lsn"), d);
    EXPECT_EQ(db.backup().ReclaimBound(~wal::Lsn{0}), d);
    wal::Lsn recycled = 0;
    db.backup().Recycle(d + 1'000'000, [&](wal::Lsn bound) { recycled = bound; });
    EXPECT_EQ(recycled, d);

    const std::string second = Reply(db, "BACKUP START 'again'");
    EXPECT_TRUE(StartsWith(second, "ERR")) << second;
    EXPECT_NE(second.find(std::to_string(id)), std::string::npos) << second;
    EXPECT_NE(Reply(db, "BACKUP STATUS").find("state=open"), std::string::npos);
    EXPECT_EQ(testing_view::MetaField(Reply(db, "SHOW META"), "backup_state"), "open");

    const std::string aborted = Reply(db, "BACKUP ABORT");
    ASSERT_TRUE(StartsWith(aborted, "OK ")) << aborted;
    EXPECT_EQ(db.backup().ReclaimBound(12345), 12345u);
    EXPECT_NE(Reply(db, "BACKUP STATUS").find("last_outcome=aborted"), std::string::npos);

    Session session;
    ASSERT_FALSE(StartsWith(db.dispatcher().Dispatch("BEGIN", &session).response, "ERR"));
    const std::string in_txn = db.dispatcher().Dispatch("BACKUP START 'x'", &session).response;
    EXPECT_TRUE(StartsWith(in_txn, "ERR")) << in_txn;
    EXPECT_NE(in_txn.find("autocommit"), std::string::npos) << in_txn;
    (void)db.dispatcher().Dispatch("ROLLBACK", &session);

    for (const char* bad : {"BACKUP", "BACKUP START", "BACKUP START nightly", "BACKUP STOP",
                            "BACKUP STOP 'somewhere'", "BACKUP START 'a' 'b'",
                            "BACKUP START 'open"}) {
        const std::string reply = Reply(db, bad);
        EXPECT_TRUE(StartsWith(reply, "ERR")) << bad << " -> " << reply;
        EXPECT_NE(reply.find("(byte "), std::string::npos) << bad << " -> " << reply;
    }
}

// BR-S2's exit, end to end at one core: a backup whose data file is taken
// inside its window (as a snapshot would) and whose log STOP copies is whole
// by its manifest, carries its BACKUP_END, and restores to every row
// committed before its stop - including those written after the data file
// was taken - and to none written after it.
TEST(BackupControlTest, AStoppedBackupCarriesItsWindowAndRestoresToItsStop) {
    TempDir live, backup, target;
    const fs::path dir = backup.path / "b1";
    std::uint64_t id = 0;
    wal::Lsn start = 0, stop = 0;
    {
        auto opened = Mount(live.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        Ok(db, "CREATE TABLE t (id int64, v int64)");
        Insert(db, 1, 10);
        CleanCheckpoint(db);
        const std::string started = Reply(db, "BACKUP START 'b1'");
        ASSERT_TRUE(StartsWith(started, "OK ")) << started;
        id = U64(started, "backup_id");
        start = U64(started, "start_lsn");
        Insert(db, 11, 20);
        // The snapshot: one thread, nothing writing, so the file is whole.
        ASSERT_TRUE(db.Sync().ok());
        fs::create_directories(dir);
        fs::copy_file(live.path / "kds.db", dir / kBackupDataFileName);
        Insert(db, 21, 30);
        const std::string stopped = Reply(db, "BACKUP STOP TO '" + dir.string() + "'");
        ASSERT_TRUE(StartsWith(stopped, "OK ")) << stopped;
        stop = U64(stopped, "stop_lsn");
        EXPECT_EQ(Field(stopped, "path"), "D");  // a snapshot's data file, not the copier's
        db.backup().WaitIdle();
        const std::string status = Reply(db, "BACKUP STATUS");
        EXPECT_NE(status.find("last_outcome=done"), std::string::npos) << status;
        Insert(db, 31, 40);  // past the stop: never in the restore
    }

    auto manifest = ReadManifest(dir.string());
    ASSERT_TRUE(manifest.ok()) << manifest.status().message();
    EXPECT_EQ(manifest.value().backup_id, id);
    EXPECT_EQ(manifest.value().label, "b1");
    EXPECT_EQ(manifest.value().start_lsn, start);
    EXPECT_EQ(manifest.value().stop_lsn, stop);
    ASSERT_TRUE(VerifyBackupFiles(dir.string(), manifest.value()).ok());

    int ends = 0;
    {
        auto log = wal::FileLogDevice::Open((dir / kBackupWalDirName).string(), 0,
                                            wal::kDefaultSegmentSize,
                                            manifest.value().start_segment);
        ASSERT_TRUE(log.ok()) << log.status().message();
        ASSERT_TRUE(wal::ScanLog(*log.value(), 0, start, [&](const wal::DecodedRecord& r) {
                        ends += r.type() == wal::RecordType::kBackupEnd;
                        return Status::OK();
                    }).ok());
    }
    EXPECT_EQ(ends, 1);

    fs::create_directories(target.path);
    fs::copy_file(dir / kBackupDataFileName, target.path / "kds.db");
    fs::copy(dir / kBackupWalDirName, target.path / "wal", fs::copy_options::recursive);
    ASSERT_TRUE(MarkRestoreOwed((target.path / "kds.db").string(), {start, stop, id}).ok());
    auto restored = Mount(target.path);
    ASSERT_TRUE(restored.ok()) << restored.status().message();
    CurrentCoreGuard as(0);
    EXPECT_EQ(Ids(*restored.value(), "SELECT id FROM t"), Range(1, 30));
    EXPECT_EQ(restored.value()->recovery().restore_backup_id, id);
}

// **CF-S1b**: a Waystone opt-in issued inside a backup's window is in the
// restore - its row is logged - and the trail recorded under it is not: a
// restore discards unlogged state, and the opted row gets a fresh, empty
// directory at the restored volume's mount.
TEST(BackupControlTest, ARestoreKeepsAWaystoneOptInAndNotItsTrail) {
    TempDir live, backup, target;
    const fs::path dir = backup.path / "w1";
    const std::string sql = "SELECT * FROM t WHERE id = 3";
    std::uint64_t id = 0;
    wal::Lsn start = 0, stop = 0;
    PageId recorded = kInvalidPageId;
    {
        auto opened = Mount(live.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        Ok(db, "CREATE TABLE t (id int64, v int64) BTREE");
        Insert(db, 1, 5);
        CleanCheckpoint(db);
        const std::string started = Reply(db, "BACKUP START 'w1'");
        ASSERT_TRUE(StartsWith(started, "OK ")) << started;
        id = U64(started, "backup_id");
        start = U64(started, "start_lsn");
        ASSERT_TRUE(StartsWith(Reply(db, "WAYSTONE ON " + sql), "OK WAYSTONE ON"));
        Ok(db, sql);
        Ok(db, sql);
        ASSERT_NE(Reply(db, "ANALYZE " + sql).find("replays=1"), std::string::npos)
            << "nothing was recorded; the cell proves nothing";
        auto fp = parser::FingerprintOf(sql);
        ASSERT_TRUE(fp.has_value());
        auto row = db.catalog().GetSysPatternRow(fp->fetch_id);
        ASSERT_TRUE(row.ok());
        recorded = row.value().waystone_root;
        ASSERT_TRUE(db.Sync().ok());
        fs::create_directories(dir);
        fs::copy_file(live.path / "kds.db", dir / kBackupDataFileName);
        const std::string stopped = Reply(db, "BACKUP STOP TO '" + dir.string() + "'");
        ASSERT_TRUE(StartsWith(stopped, "OK ")) << stopped;
        stop = U64(stopped, "stop_lsn");
        db.backup().WaitIdle();
    }

    fs::create_directories(target.path);
    fs::copy_file(dir / kBackupDataFileName, target.path / "kds.db");
    fs::copy(dir / kBackupWalDirName, target.path / "wal", fs::copy_options::recursive);
    ASSERT_TRUE(MarkRestoreOwed((target.path / "kds.db").string(), {start, stop, id}).ok());
    auto restored = Mount(target.path);
    ASSERT_TRUE(restored.ok()) << restored.status().message();
    CurrentCoreGuard as(0);
    Expeditor& db = *restored.value();
    auto fp = parser::FingerprintOf(sql);
    ASSERT_TRUE(fp.has_value());
    auto row = db.catalog().GetSysPatternRow(fp->fetch_id);
    ASSERT_TRUE(row.ok()) << "the opt-in was not restored: " << row.status().message();
    EXPECT_NE(row.value().flags & catalog::kPatternWaystone, 0);
    EXPECT_TRUE(catalog::HasWaystoneDirectory(row.value()));
    EXPECT_NE(row.value().waystone_root, recorded) << "the backup's directory was kept";
    EXPECT_EQ(Reply(db, "ANALYZE " + sql).find("replays="), std::string::npos)
        << "a trail survived the restore";
}

// CF-S1b's bump: a backup a version-32 build wrote is refused before
// anything is laid out, naming both versions.
TEST(BackupControlTest, AVersion32BackupIsRefused) {
    TempDir live, backup, target;
    const fs::path dir = backup.path / "v32";
    {
        auto opened = Mount(live.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        Ok(db, "CREATE TABLE t (id int64, v int64)");
        Insert(db, 1, 3);
        CleanCheckpoint(db);
        ASSERT_TRUE(StartsWith(Reply(db, "BACKUP START 'v32'"), "OK "));
        ASSERT_TRUE(db.Sync().ok());
        fs::create_directories(dir);
        fs::copy_file(live.path / "kds.db", dir / kBackupDataFileName);
        ASSERT_TRUE(StartsWith(Reply(db, "BACKUP STOP TO '" + dir.string() + "'"), "OK "));
        db.backup().WaitIdle();
    }
    auto manifest = ReadManifest(dir.string());
    ASSERT_TRUE(manifest.ok()) << manifest.status().message();
    ASSERT_EQ(manifest.value().superblock_version, kSuperBlockVersion);
    BackupManifest older = manifest.value();
    older.superblock_version = 32;
    ASSERT_TRUE(WriteManifest(dir.string(), older).ok());

    RestoreRequest request;
    request.backup_dir = dir.string();
    request.data_file = (target.path / "kds.db").string();
    request.wal_dir = (target.path / "wal").string();
    const Status laid = LayOutRestore(request);
    ASSERT_FALSE(laid.ok());
    EXPECT_EQ(laid.code(), StatusCode::kUnsupported) << laid.message();
    for (const std::uint32_t named : {32u, kSuperBlockVersion}) {
        EXPECT_NE(laid.message().find("superblock " + std::to_string(named)), std::string::npos)
            << laid.message();
    }
    EXPECT_FALSE(fs::exists(target.path / "kds.db")) << "a refused restore laid out a file";
}

// A backup's directory is new: STOP refuses one holding another backup's
// files, and the backup stays open to be stopped elsewhere.
TEST(BackupControlTest, StopRefusesADirectoryThatIsNotNew) {
    TempDir live, backup;
    auto opened = Mount(live.path);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    CurrentCoreGuard as(0);
    ASSERT_TRUE(StartsWith(Reply(db, "BACKUP START 'x'"), "OK "));
    const fs::path used = backup.path / "used";
    fs::create_directories(used / kBackupWalDirName);
    std::ofstream(used / kBackupWalDirName / SegmentFileName(0)) << "another backup's";
    const std::string refused = Reply(db, "BACKUP STOP TO '" + used.string() + "'");
    EXPECT_TRUE(StartsWith(refused, "ERR")) << refused;
    EXPECT_TRUE(db.backup().active());
    const std::string stopped =
        Reply(db, "BACKUP STOP TO '" + (backup.path / "fresh").string() + "'");
    EXPECT_TRUE(StartsWith(stopped, "OK ")) << stopped;
    EXPECT_EQ(Field(stopped, "path"), "D");
    db.backup().WaitIdle();
    EXPECT_FALSE(db.backup().active());
}

// ---- A recovery defect BR found (BR-R1) -----------------------------------

// A rolled-back DROP TABLE, replayed over data pages written before it: the
// drop's catalog retype is logged, and so - since BR - is its rollback's
// compensation. Before, the compensation was unlogged on catalog pages
// (`TransactionManager::Compensate`), so redo replayed the drop and nothing
// put it back, and the table was gone after a crash or a restore whose replay
// began before the drop - in silence. BR-S8's simulation found it: a backup's
// start lies further back than a crash's anchor usually does.
void RolledBackDropOverPagesFromBeforeIt(bool as_restore) {
    TempDir live, image;
    wal::Lsn start = 0, stop = 0;
    {
        auto opened = Mount(live.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        start = db.superblock().wal_anchor(0).redo_start_lsn;
        Ok(db, "CREATE TABLE t (id int64, v int64)");
        Insert(db, 1, 5);
        ASSERT_TRUE(db.store().Sync().ok());
        fs::create_directories(image.path);
        fs::copy_file(live.path / "kds.db", image.path / "kds.db");  // the pages before the drop
        Session session;
        ASSERT_FALSE(StartsWith(db.dispatcher().Dispatch("BEGIN", &session).response, "ERR"));
        std::string reply = db.dispatcher().Dispatch("DROP TABLE t", &session).response;
        ASSERT_FALSE(StartsWith(reply, "ERR")) << reply;
        reply = db.dispatcher().Dispatch("ROLLBACK", &session).response;
        ASSERT_FALSE(StartsWith(reply, "ERR")) << reply;
        Insert(db, 6, 7);
        ASSERT_TRUE(db.Sync().ok());
        stop = db.wal().durable_lsn();
        fs::copy(live.path / "wal", image.path / "wal", fs::copy_options::recursive);
    }
    if (as_restore) {
        ASSERT_TRUE(MarkRestoreOwed((image.path / "kds.db").string(), {start, stop, 9}).ok());
    }
    auto again = Mount(image.path);
    ASSERT_TRUE(again.ok()) << again.status().message();
    CurrentCoreGuard as(0);
    EXPECT_EQ(Ids(*again.value(), "SELECT id FROM t"), Range(1, 7));
}

TEST(BackupRestoreTest, ARolledBackDropSurvivesAReplayOverPagesFromBeforeIt) {
    RolledBackDropOverPagesFromBeforeIt(/*as_restore=*/false);  // a crash
    RolledBackDropOverPagesFromBeforeIt(/*as_restore=*/true);
}

}  // namespace
}  // namespace kds::server
