// BR-S4..S7 - the WAL archive, the fenced base copy, the restore's layout and
// point-in-time recovery (`instructions/v3.0.0/workorder-br-backup-restore.md`,
// `br-s0-census.md`). Real files throughout: an archive, a backup and a
// restore are facts about directories.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/server/backup_manifest.hpp"
#include "kds/server/expeditor.hpp"
#include "kds/server/restore.hpp"
#include "kds/server/wal_archive.hpp"
#include "kds/storage/file_page_device.hpp"

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
               ("kds_bra_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

StatusOr<std::unique_ptr<Expeditor>> Mount(const fs::path& dir, const fs::path& archive = {},
                                           std::uint32_t cores = 1) {
    Expeditor::Config config;
    config.buffer_pool_frames = 4096;
    config.data_file = (dir / "kds.db").string();
    config.wal_dir = (dir / "wal").string();
    config.log_file = {};
    config.cores = cores;
    config.debug_text_port = 0;
    config.checkpoint_interval_ns = 0;
    config.archive.archive_dir = archive.string();
    return Expeditor::Open(config, /*now_unix_seconds=*/1000);
}

std::string Reply(Expeditor& db, const std::string& sql) {
    return db.dispatcher().Dispatch(sql).response;
}

void Ok(Expeditor& db, const std::string& sql) {
    const std::string reply = Reply(db, sql);
    ASSERT_FALSE(StartsWith(reply, "ERR")) << sql << " -> " << reply;
}

std::vector<std::uint64_t> Ids(Expeditor& db, const std::string& sql) {
    std::vector<std::uint64_t> out;
    const std::string reply = Reply(db, sql);
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

void Insert(Expeditor& db, const std::string& table, std::uint64_t first, std::uint64_t last) {
    for (std::uint64_t id = first; id <= last; ++id) {
        Ok(db, "INSERT INTO " + table + " VALUES (" + std::to_string(id) + ", " +
                   std::to_string(id) + ")");
    }
}

void CleanCheckpoint(Expeditor& db) {
    ASSERT_TRUE(db.store().Sync().ok());
    ASSERT_TRUE(db.Checkpoint().ok());
}

std::string Field(const std::string& reply, const std::string& key) {
    const std::string tag = " " + key + "=";
    std::size_t at = reply.find(tag);
    if (at == std::string::npos) return {};
    at += tag.size();
    return reply.substr(at, reply.find(' ', at) - at);
}

std::uint64_t U64(const std::string& reply, const std::string& key) {
    return std::strtoull(Field(reply, key).c_str(), nullptr, 10);
}

// Polls `done` for up to 30 s.
template <typename F>
bool WaitFor(F done) {
    for (int i = 0; i < 3000; ++i) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

// ---- The fence (BR-R5 replaced) ----------------------------------------------

// Three writers rewrite one page with a uniform byte each, as fast as they
// can, while a reader reads it through the armed fence: every read is one
// writer's whole page, never two halves. The cell the fence is for.
TEST(PageWriteFenceTest, AFencedReadNeverSeesATornPage) {
    TempDir dir;
    auto device = storage::FilePageDevice::Open((dir.path / "kds.db").string());
    ASSERT_TRUE(device.ok()) << device.status().message();
    ASSERT_TRUE(device.value()->EnsureCapacity(8).ok());
    storage::FilePageDevice& d = *device.value();
    std::atomic<bool> stop{false};
    std::vector<std::thread> writers;
    for (int w = 0; w < 3; ++w) {
        writers.emplace_back([&, w] {
            std::array<std::byte, kPageSize> page{};
            page.fill(std::byte{static_cast<unsigned char>(0x11 * (w + 1))});
            while (!stop.load(std::memory_order_relaxed)) {
                ASSERT_TRUE(d.WritePage(3, page).ok());
            }
        });
    }
    d.write_fence().Arm();
    int torn = 0;
    std::array<std::byte, kPageSize> read{};
    for (int i = 0; i < 20000; ++i) {
        ASSERT_TRUE(d.ReadPageRunFenced(3, 1, read).ok());
        for (const std::byte b : read) {
            if (b != read[0]) {
                ++torn;
                break;
            }
        }
    }
    d.write_fence().Disarm();
    stop.store(true);
    for (std::thread& t : writers) t.join();
    EXPECT_EQ(torn, 0);
}

// ---- The archive (BR-S4) ----------------------------------------------------

// A fake log: `count` segment files of `size` bytes, each filled with its
// own number, as `FileLogDevice` names them.
void FakeSegments(const fs::path& wal, std::uint64_t count, std::uint64_t size) {
    fs::create_directories(wal);
    for (std::uint64_t s = 0; s < count; ++s) {
        std::ofstream f(wal / SegmentFileName(s), std::ios::binary);
        const std::string body(size, static_cast<char>('a' + s));
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
}

// Every sealed segment - every one below the durable LSN's - lands under its
// lineage, in order, and the floor follows; the segment holding the durable
// point waits. A restart re-finds what it archived and skips it; a different
// body under one name stops the archive, counted and named, never written
// over.
TEST(WalArchiverTest, ArchivesEverySealedSegmentUnderItsLineageAndNeverOverwrites) {
    TempDir dir;
    const fs::path wal = dir.path / "wal", archive = dir.path / "archive";
    fs::create_directories(archive);
    constexpr std::uint64_t kSize = 4096;
    FakeSegments(wal, 3, kSize);
    std::atomic<wal::Lsn> durable{2 * kSize + 100};  // segment 2 holds it
    const auto wiring = [&](std::uint64_t first) {
        return WalArchiver::Wiring{{archive.string(), {}}, wal.string(), 0xabcULL, kSize, first,
                                   [&] { return durable.load(); }, {}, nullptr};
    };
    {
        WalArchiver archiver(wiring(0));
        EXPECT_EQ(archiver.floor(), 0u);
        archiver.Start();
        ASSERT_TRUE(WaitFor([&] { return archiver.next_segment() == 2; }));
        EXPECT_EQ(archiver.floor(), 2 * kSize);
        EXPECT_TRUE(fs::exists(archive / ArchiveName(0xabc, 0)));
        EXPECT_TRUE(fs::exists(archive / ArchiveName(0xabc, 1)));
        EXPECT_FALSE(fs::exists(archive / ArchiveName(0xabc, 2)));
        EXPECT_EQ(ArchiveName(0xabc, 1), "kds-0000000000000abc-1.log");
    }
    {
        // A restart from segment 0 again: the same bodies are skipped.
        WalArchiver archiver(wiring(0));
        archiver.Start();
        ASSERT_TRUE(WaitFor([&] { return archiver.next_segment() == 2; }));
        std::ostringstream meta;
        archiver.AppendMeta(meta);
        EXPECT_NE(meta.str().find("wal_archive_failures=0"), std::string::npos) << meta.str();
    }
    {
        // Segment 1's archived body differs from the log's: refused, kept.
        std::ofstream(archive / ArchiveName(0xabc, 1), std::ios::binary | std::ios::trunc)
            << "not this segment";
        WalArchiver archiver(wiring(0));
        archiver.Start();
        ASSERT_TRUE(WaitFor([&] {
            std::ostringstream meta;
            archiver.AppendMeta(meta);
            return meta.str().find("wal_archive_failures=0") == std::string::npos;
        }));
        EXPECT_EQ(archiver.next_segment(), 1u);
        EXPECT_NE(archiver.last_error().find("different body"), std::string::npos)
            << archiver.last_error();
        std::ifstream kept(archive / ArchiveName(0xabc, 1));
        std::string body((std::istreambuf_iterator<char>(kept)), std::istreambuf_iterator<char>());
        EXPECT_EQ(body, "not this segment");
    }
}

TEST(WalArchiverTest, AnArchiveCommandIsHandedThePathAndTheName) {
    TempDir dir;
    const fs::path wal = dir.path / "wal", out = dir.path / "out";
    fs::create_directories(out);
    constexpr std::uint64_t kSize = 4096;
    FakeSegments(wal, 2, kSize);
    WalArchiver archiver(WalArchiver::Wiring{{{}, "cp %p " + out.string() + "/%f"},
                                             wal.string(), 7, kSize, 0,
                                             [] { return wal::Lsn{kSize + 1}; }, {}, nullptr});
    archiver.Start();
    ASSERT_TRUE(WaitFor([&] { return archiver.next_segment() == 1; }));
    EXPECT_TRUE(fs::exists(out / ArchiveName(7, 0)));

    WalArchiver failing(WalArchiver::Wiring{{{}, "false"}, wal.string(), 7, kSize, 0,
                                            [] { return wal::Lsn{kSize + 1}; }, {}, nullptr});
    failing.Start();
    ASSERT_TRUE(WaitFor([&] { return !failing.last_error().empty(); }));
    EXPECT_EQ(failing.next_segment(), 0u);
    EXPECT_EQ(failing.floor(), 0u);
}

// ---- Path B, restore, PITR (BR-S5..S7) ---------------------------------------

struct BaseBackup {
    std::uint64_t id = 0;
    wal::Lsn start = 0;
    wal::Lsn stop = 0;
};

// START, COPY, wait for the copy, `between()`, STOP, wait for the finish.
template <typename F>
BaseBackup TakeBaseBackup(Expeditor& db, const fs::path& dir, F between) {
    BaseBackup b;
    const std::string started = Reply(db, "BACKUP START 'base'");
    EXPECT_TRUE(StartsWith(started, "OK ")) << started;
    b.id = U64(started, "backup_id");
    b.start = U64(started, "start_lsn");
    const std::string copying = Reply(db, "BACKUP COPY TO '" + dir.string() + "'");
    EXPECT_TRUE(StartsWith(copying, "OK ")) << copying;
    // Writes and writebacks while the copier reads: the fence's work.
    between();
    EXPECT_TRUE(WaitFor([&] { return Field(Reply(db, "BACKUP STATUS"), "copying") == "0"; }));
    const std::string stopped = Reply(db, "BACKUP STOP TO '" + dir.string() + "'");
    EXPECT_TRUE(StartsWith(stopped, "OK ")) << stopped;
    EXPECT_EQ(Field(stopped, "path"), "B");
    b.stop = U64(stopped, "stop_lsn");
    db.backup().WaitIdle();
    const std::string status = Reply(db, "BACKUP STATUS");
    EXPECT_NE(status.find("last_outcome=done"), std::string::npos) << status;
    return b;
}

// BR-S5's exit and BR-S6's: a base backup taken while rows land and pages
// are written back restores - laid out by `LayOutRestore`, recovered by the
// mount - to exactly the rows committed before its stop, verified by its
// manifest, under a fresh lineage.
TEST(BaseBackupTest, ABaseBackupTakenUnderWritesRestoresToItsStop) {
    TempDir live, backups, target;
    const fs::path dir = backups.path / "b";
    BaseBackup b;
    std::uint64_t source_lineage = 0;
    {
        auto opened = Mount(live.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        source_lineage = db.superblock().lineage_id();
        Ok(db, "CREATE TABLE t (id int64, v int64)");
        Insert(db, "t", 1, 200);
        CleanCheckpoint(db);
        b = TakeBaseBackup(db, dir, [&] {
            for (std::uint64_t id = 201; id <= 400; id += 50) {
                Insert(db, "t", id, id + 49);
                ASSERT_TRUE(db.store().Sync().ok());  // writebacks during the copy
            }
        });
        Insert(db, "t", 401, 450);  // past the stop
    }
    auto manifest = ReadManifest(dir.string());
    ASSERT_TRUE(manifest.ok()) << manifest.status().message();
    EXPECT_EQ(manifest.value().path, 'B');
    EXPECT_EQ(manifest.value().lineage_id, source_lineage);

    RestoreLayout layout;
    ASSERT_TRUE(LayOutRestore({dir.string(), (target.path / "kds.db").string(),
                               (target.path / "wal").string()},
                              &layout)
                    .ok());
    EXPECT_EQ(layout.backup_id, b.id);
    EXPECT_NE(layout.lineage_id, source_lineage);
    auto restored = Mount(target.path);
    ASSERT_TRUE(restored.ok()) << restored.status().message();
    CurrentCoreGuard as(0);
    EXPECT_EQ(Ids(*restored.value(), "SELECT id FROM t"), Range(1, 400));
    EXPECT_EQ(restored.value()->recovery().restore_backup_id, b.id);
}

// BR-R8 and BR-S8 cases 4, 5, 7: a restore lays out onto nothing, and a
// backup whose files do not match its manifest - a flipped bit, a segment
// another backup holds, a missing one - is refused by name before anything
// is written.
TEST(BaseBackupTest, ARestoreRefusesATargetThatHoldsDataAndFilesTheManifestDoesNotName) {
    TempDir live, backups, target;
    const fs::path dir = backups.path / "b";
    {
        auto opened = Mount(live.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        Ok(db, "CREATE TABLE t (id int64, v int64)");
        Insert(db, "t", 1, 20);
        CleanCheckpoint(db);
        (void)TakeBaseBackup(db, dir, [] {});
    }
    const std::string data = (target.path / "kds.db").string();
    const std::string wal = (target.path / "wal").string();

    {
        std::ofstream(data) << "occupied";
        const Status s = LayOutRestore({dir.string(), data, wal});
        EXPECT_EQ(s.code(), StatusCode::kAlreadyExists) << s.message();
        fs::remove(data);
    }
    const fs::path segment = dir / kBackupWalDirName / SegmentFileName(0);
    {
        std::fstream f(segment, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(4096 + 64);
        f.put('\x7f');
    }
    {
        const Status s = LayOutRestore({dir.string(), data, wal});
        EXPECT_EQ(s.code(), StatusCode::kCorruption) << s.message();
        EXPECT_NE(s.message().find(SegmentFileName(0)), std::string::npos) << s.message();
        EXPECT_FALSE(fs::exists(data));
    }
    fs::remove(segment);
    {
        const Status s = LayOutRestore({dir.string(), data, wal});
        EXPECT_FALSE(s.ok());
        EXPECT_NE(s.message().find(SegmentFileName(0)), std::string::npos) << s.message();
    }
    {
        // An unknown manifest format is refused, never guessed at (BR-R7).
        std::string text;
        {
            std::ifstream in(dir / kBackupManifestName);
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        text.replace(0, text.find('\n'), "kds-backup-manifest 99");
        std::ofstream(dir / kBackupManifestName, std::ios::trunc) << text;
        EXPECT_FALSE(ReadManifest(dir.string()).ok());
    }
}

// BR-S7's exit, BR-R10 and BR-Q5 (b): restore to an LSN past the stop through
// the archive. The table dropped after the target is there with every row
// committed before it, nothing committed after it is, a target below the
// stop or past the archive is refused by name, and the restored instance's
// archive lands under its own lineage beside the original's, untouched.
TEST(PointInTimeTest, ARestoreToAnLsnPastTheStopEndsJustBeforeIt) {
    TempDir live, archive, backups, target, early, late;
    const fs::path dir = backups.path / "b";
    BaseBackup b;
    wal::Lsn target_lsn = 0;
    std::uint64_t source_lineage = 0;
    {
        auto opened = Mount(live.path, archive.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        source_lineage = db.superblock().lineage_id();
        Ok(db, "CREATE TABLE t (id int64, v int64)");
        Ok(db, "CREATE TABLE u (id int64, v int64)");
        Insert(db, "t", 1, 20);
        CleanCheckpoint(db);
        b = TakeBaseBackup(db, dir, [] {});
        Insert(db, "t", 21, 30);  // past the stop, in the next segment
        ASSERT_TRUE(db.Sync().ok());
        target_lsn = db.wal().durable_lsn();
        Ok(db, "DROP TABLE t");
        Insert(db, "u", 1, 5);
        // A second backup's STOP seals this segment, and one more write past
        // it lets the archiver take it.
        ASSERT_TRUE(StartsWith(Reply(db, "BACKUP START 'seal'"), "OK "));
        ASSERT_TRUE(StartsWith(
            Reply(db, "BACKUP STOP TO '" + (backups.path / "seal").string() + "'"), "OK "));
        db.backup().WaitIdle();
        Insert(db, "u", 6, 6);
        ASSERT_TRUE(db.Sync().ok());
        const std::uint64_t sealed_end = db.wal().durable_lsn() / wal::kDefaultSegmentSize;
        db.archiver()->Kick();
        ASSERT_TRUE(WaitFor([&] { return db.archiver()->next_segment() >= sealed_end; }));
        EXPECT_GT(target_lsn, b.stop);
    }

    const std::string data = (target.path / "kds.db").string();
    const std::string wal = (target.path / "wal").string();
    RestoreRequest below{dir.string(), data, wal, {}, archive.path.string(), b.stop - 1, false};
    EXPECT_EQ(LayOutRestore(below).code(), StatusCode::kInvalidArgument);
    RestoreRequest beyond{dir.string(),
                          (late.path / "kds.db").string(),
                          (late.path / "wal").string(),
                          {},
                          archive.path.string(),
                          100 * wal::kDefaultSegmentSize,
                          false};
    const Status past = LayOutRestore(beyond);
    EXPECT_EQ(past.code(), StatusCode::kNotFound) << past.message();
    EXPECT_NE(past.message().find("missing"), std::string::npos) << past.message();

    RestoreLayout layout;
    RestoreRequest to_t{dir.string(), data, wal, {}, archive.path.string(), target_lsn, false};
    ASSERT_TRUE(LayOutRestore(to_t, &layout).ok());
    EXPECT_GE(layout.cut_lsn, target_lsn);
    std::uint64_t restored_lineage = 0;
    {
        auto restored = Mount(target.path, archive.path);
        ASSERT_TRUE(restored.ok()) << restored.status().message();
        Expeditor& db = *restored.value();
        CurrentCoreGuard as(0);
        restored_lineage = db.superblock().lineage_id();
        EXPECT_NE(restored_lineage, source_lineage);
        EXPECT_EQ(Ids(db, "SELECT id FROM t"), Range(1, 30));
        EXPECT_TRUE(Ids(db, "SELECT id FROM u").empty());
        // The restored history archives under its own name.
        Insert(db, "t", 31, 31);
        ASSERT_TRUE(StartsWith(Reply(db, "BACKUP START 'x'"), "OK "));
        ASSERT_TRUE(StartsWith(
            Reply(db, "BACKUP STOP TO '" + (backups.path / "x").string() + "'"), "OK "));
        db.backup().WaitIdle();
        Insert(db, "t", 32, 32);
        ASSERT_TRUE(db.Sync().ok());
        const std::uint64_t sealed_end = db.wal().durable_lsn() / wal::kDefaultSegmentSize;
        db.archiver()->Kick();
        ASSERT_TRUE(WaitFor([&] { return db.archiver()->next_segment() >= sealed_end; }));
    }
    int source_files = 0, restored_files = 0;
    for (const auto& entry : fs::directory_iterator(archive.path)) {
        const std::string name = entry.path().filename().string();
        source_files += name.rfind(ArchiveName(source_lineage, 0).substr(0, 21), 0) == 0;
        restored_files += name.rfind(ArchiveName(restored_lineage, 0).substr(0, 21), 0) == 0;
    }
    EXPECT_GE(source_files, 2);
    EXPECT_GE(restored_files, 1);

    // `latest`: every archived segment of the backup's lineage.
    RestoreRequest latest{dir.string(), (early.path / "kds.db").string(),
                          (early.path / "wal").string(), {}, archive.path.string(), 0, true};
    ASSERT_TRUE(LayOutRestore(latest).ok());
    auto all = Mount(early.path);
    ASSERT_TRUE(all.ok()) << all.status().message();
    CurrentCoreGuard as(0);
    EXPECT_EQ(Ids(*all.value(), "SELECT id FROM u"), Range(1, 5));
}

}  // namespace
}  // namespace kds::server
