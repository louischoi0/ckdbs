#include "kds/server/restore.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "kds/catalog/catalog.hpp"
#include "kds/server/backup_manifest.hpp"
#include "kds/server/wal_archive.hpp"
#include "kds/storage/file_page_device.hpp"
#include "kds/storage/free_map.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/wal/file_log_device.hpp"
#include "kds/wal/log_device.hpp"
#include "kds/wal/log_scanner.hpp"

namespace kds::server {

namespace {

// Refused before any file is opened.
Status CheckWindow(const RestoreWindow& window) {
    if (window.start_lsn == 0 || window.start_lsn >= window.stop_lsn) {
        return Status::InvalidArgument("restore: start lsn " + std::to_string(window.start_lsn) +
                                       " is not a start below stop lsn " +
                                       std::to_string(window.stop_lsn));
    }
    if (window.backup_id == 0) {
        return Status::InvalidArgument("restore: backup id 0 names no backup");
    }
    return Status::OK();
}

}  // namespace

Status MarkRestoreOwed(const std::string& data_file, const RestoreWindow& window,
                       std::uint64_t* lineage) {
    if (Status s = CheckWindow(window); !s.ok()) return s;
    auto device = storage::FilePageDevice::Open(data_file);
    if (!device.ok()) return device.status();
    if (Status s = MarkRestoreOwedOn(*device.value(), window, lineage); !s.ok()) {
        return s.WithMessage("restore: " + data_file + ": " + s.message());
    }
    return Status::OK();
}

Status MarkRestoreOwedOn(storage::PageDevice& device, const RestoreWindow& window,
                         std::uint64_t* lineage) {
    if (Status s = CheckWindow(window); !s.ok()) return s;
    alignas(8) std::array<std::byte, kPageSize> page{};
    const std::span<std::byte, kPageSize> bytes(page);
    if (Status s = device.ReadPage(kSuperBlockPageId, bytes); !s.ok()) return s;
    if (Status s = storage::VerifyPageChecksum(bytes); !s.ok()) {
        return Status::Corruption("page 0 is not a whole superblock: " + s.message());
    }
    auto decoded = SuperBlock::Decode(bytes);
    if (!decoded.ok()) return decoded.status();
    SuperBlock superblock = decoded.value();
    const std::uint64_t fresh = DrawLineageId();
    superblock.SetRestoreBackupId(window.backup_id);
    superblock.SetLineageId(fresh);
    // **Slot 0 too** (census §3 item 3): every reader of the image's anchor -
    // the mount's, the checkpoint anchor's warm-up, a peer's copy - sees the
    // backup's start, never the copied one, and the slot's durable point
    // carries the stop into analysis's check whoever reads it.
    if (Status s = superblock.SetWalAnchor(
            0, {/*checkpoint_lsn=*/window.start_lsn, window.start_lsn, window.stop_lsn,
                window.start_lsn / wal::kDefaultSegmentSize});
        !s.ok()) {
        return s;
    }
    superblock.Encode(bytes);
    storage::StampPageChecksum(bytes);
    if (Status s = device.WritePage(kSuperBlockPageId, bytes); !s.ok()) return s;
    if (Status s = device.Sync(); !s.ok()) return s;
    if (lineage != nullptr) *lineage = fresh;
    return Status::OK();
}

namespace {

namespace fs = std::filesystem;

bool EmptyOrAbsent(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return true;
    return fs::is_directory(p, ec) && fs::is_empty(p, ec);
}

// Cuts the log in `wal_dir` at `cut`: the segment holding it keeps its bytes
// below `cut` and zeros from it on, synced, and every later segment is
// removed. A scan of the result ends exactly at `cut` (a zeroed tail is the
// unsealed end of a stream, `log_scanner.hpp`).
Status CutLogAt(const std::string& wal_dir, wal::Lsn cut, std::uint64_t segment_size,
                std::uint64_t last_segment) {
    const std::uint64_t segment = cut / segment_size;
    const std::string path = (fs::path(wal_dir) / SegmentFileName(segment)).string();
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return Status::IoError("restore: open " + path + ": " + std::strerror(errno));
    std::vector<std::byte> zeros(1 << 20);
    Status status;
    for (std::uint64_t at = cut % segment_size; at < segment_size && status.ok();) {
        const std::size_t n = static_cast<std::size_t>(
            std::min<std::uint64_t>(zeros.size(), segment_size - at));
        status = PwriteAll(fd, zeros.data(), n, at, "restore: zero " + path);
        at += n;
    }
    if (status.ok() && ::fsync(fd) != 0) {
        status = Status::IoError("restore: fsync " + path + ": " + std::strerror(errno));
    }
    ::close(fd);
    if (!status.ok()) return status;
    for (std::uint64_t later = segment + 1; later <= last_segment; ++later) {
        std::error_code ec;
        fs::remove(fs::path(wal_dir) / SegmentFileName(later), ec);
        if (ec) return Status::IoError("restore: remove segment " + std::to_string(later) + ": " +
                                       ec.message());
    }
    return SyncDirectory(wal_dir);
}

}  // namespace

namespace {

using PageBytes = std::array<std::byte, kPageSize>;

bool AllZero(const PageBytes& p) {
    return std::all_of(p.begin(), p.end(), [](std::byte b) { return b == std::byte{0}; });
}

// One read of a copied data file, page by page, against its own maps (BR,
// `restore.hpp`): every page those maps call allocated - and every system
// page below the first user id - must carry the checksum it was written
// with, unless it is headerless (no checksum to carry) or all zeros (never
// written). With `forget`, every all-zero allocated user page is also given
// back; its map pages are rewritten only once the whole file has passed.
Status WalkDataFile(storage::PageDevice& device, bool forget, DataFileCheck* out) {
    const std::uint32_t capacity = device.page_capacity();
    DataFileCheck check;
    std::vector<std::pair<PageId, PageBytes>> rewritten_maps;
    std::vector<PageId> failed;
    PageBytes map{}, headerless{}, page{};
    const auto verify = [&](PageId id) -> Status {
        if (Status s = device.ReadPage(id, page); !s.ok()) return s;
        if (AllZero(page)) return Status::OK();
        ++check.pages_checked;
        if (!storage::VerifyPageChecksum(page).ok()) failed.push_back(id);
        return Status::OK();
    };
    for (std::uint32_t region = 0; storage::FreeMapRegionBase(region) < capacity; ++region) {
        const PageId base = storage::FreeMapRegionBase(region);
        const PageId map_id = storage::FreeMapPageIdFor(base);
        if (map_id >= capacity) break;
        if (Status s = device.ReadPage(map_id, map); !s.ok()) return s;
        if (AllZero(map)) continue;  // a region whose map was never written: nothing allocated
        ++check.pages_checked;
        if (Status s = storage::ValidateFreeMapPage(map); !s.ok()) {
            failed.push_back(map_id);
            continue;  // its bits cannot be trusted; the page is named below
        }
        const PageId headerless_id = storage::HeaderlessMapPageIdFor(base);
        headerless.fill(std::byte{0});
        if (headerless_id < capacity) {
            if (Status s = device.ReadPage(headerless_id, headerless); !s.ok()) return s;
        }
        const bool has_headerless = !AllZero(headerless);
        if (has_headerless) {
            ++check.pages_checked;
            if (!storage::ValidateFreeMapPage(headerless, PageType::kHeaderlessMap).ok()) {
                failed.push_back(headerless_id);
                continue;
            }
        }
        bool changed = false;
        for (std::uint32_t index = 0; index < storage::kFreeMapBitsPerPage; ++index) {
            const PageId id = base + index;
            if (storage::IsMapPageId(id)) continue;
            const bool system = id < server::kFirstUserPageId;
            if (!system && !storage::FreeMapIsAllocated(map, index)) continue;
            if (has_headerless && storage::FreeMapIsAllocated(headerless, index)) continue;
            // Past the copy's end, or zeros: never written into the copy.
            bool unwritten = id >= capacity;
            if (!unwritten) {
                if (Status s = verify(id); !s.ok()) return s;
                unwritten = AllZero(page);
            }
            if (!system && forget && unwritten) {
                storage::FreeMapRelease(map, index);
                changed = true;
                ++check.pages_forgotten;
            }
        }
        if (changed) {
            storage::StampPageChecksum(map);
            rewritten_maps.emplace_back(map_id, map);
        }
    }
    if (!failed.empty()) {
        std::string ids;
        for (std::size_t i = 0; i < failed.size() && i < 16; ++i) {
            ids += (i == 0 ? "" : ", ") + std::to_string(failed[i]);
        }
        if (failed.size() > 16) ids += ", ...";
        return Status::Corruption(
            "data file: " + std::to_string(failed.size()) + " of " +
            std::to_string(check.pages_checked) + " written page(s) fail their checksum (page " +
            ids + "); the copy is torn or damaged and is no backup");
    }
    for (const auto& [map_id, bytes] : rewritten_maps) {
        if (Status s = device.WritePage(map_id, bytes); !s.ok()) return s;
    }
    if (!rewritten_maps.empty()) {
        if (Status s = device.Sync(); !s.ok()) return s;
    }
    if (out != nullptr) *out = check;
    return Status::OK();
}

}  // namespace

Status ForgetUnwrittenPages(storage::PageDevice& device, DataFileCheck* out) {
    return WalkDataFile(device, /*forget=*/true, out);
}

namespace {

// A data file read and nothing else: `O_RDONLY`, so a backup on read-only
// media can be checked, and every write refused.
class ReadOnlyPageFile final : public storage::PageDevice {
public:
    static StatusOr<std::unique_ptr<ReadOnlyPageFile>> Open(const std::string& path) {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return Status::IoError("open " + path + ": " + std::strerror(errno));
        const ::off_t size = ::lseek(fd, 0, SEEK_END);
        if (size < 0) {
            ::close(fd);
            return Status::IoError("size of " + path + ": " + std::strerror(errno));
        }
        return std::unique_ptr<ReadOnlyPageFile>(
            new ReadOnlyPageFile(fd, static_cast<std::uint32_t>(size / kPageSize)));
    }
    ~ReadOnlyPageFile() override { ::close(fd_); }
    std::uint32_t page_capacity() const noexcept override { return pages_; }
    Status ReadPage(PageId id, std::span<std::byte, kPageSize> out) override {
        if (id >= pages_) return Status::OutOfRange("page " + std::to_string(id));
        std::size_t done = 0;
        while (done < kPageSize) {
            const ::ssize_t n = ::pread(fd_, out.data() + done, kPageSize - done,
                                        static_cast<::off_t>(id) * kPageSize + done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return Status::IoError("read page " + std::to_string(id));
            done += static_cast<std::size_t>(n);
        }
        return Status::OK();
    }
    Status WritePage(PageId, std::span<const std::byte, kPageSize>) override { return Refused(); }
    Status EnsureCapacity(std::uint32_t) override { return Refused(); }
    Status Sync() override { return Refused(); }

private:
    ReadOnlyPageFile(int fd, std::uint32_t pages) : fd_(fd), pages_(pages) {}
    static Status Refused() { return Status::Unsupported("a data file opened to read (byte 0)"); }
    int fd_;
    std::uint32_t pages_;
};

}  // namespace

Status VerifyDataFilePages(const std::string& data_file, DataFileCheck* out) {
    auto device = ReadOnlyPageFile::Open(data_file);
    if (!device.ok()) return device.status();
    if (Status s = WalkDataFile(*device.value(), /*forget=*/false, out); !s.ok()) {
        return s.WithMessage(data_file + ": " + s.message());
    }
    return Status::OK();
}

Status VerifyBackupLog(const std::string& backup_dir, const BackupManifest& manifest) {
    // Every record of the window decodes - its CRC32C included - from the
    // start to the stop: a scan ends at the first record that does not.
    // Read-only: a backup on read-only media is scanned, never written.
    auto device = wal::FileLogDevice::OpenReadOnly(
        (std::filesystem::path(backup_dir) / kBackupWalDirName).string(), 0,
        manifest.segment_size, manifest.start_segment);
    if (!device.ok()) return device.status();
    auto scanned = wal::ScanLogToEnd(*device.value(), 0, manifest.start_lsn);
    if (!scanned.ok()) return scanned.status();
    if (scanned.value().end_lsn < manifest.stop_lsn) {
        const wal::Lsn end = scanned.value().end_lsn;
        return Status::Corruption(
            "backup " + std::to_string(manifest.backup_id) + ": its log decodes only to lsn " +
            std::to_string(end) + " (segment " + std::to_string(end / manifest.segment_size) +
            "), short of its stop lsn " + std::to_string(manifest.stop_lsn));
    }
    return Status::OK();
}

Status PrepareRestoredVolume(storage::PageDevice& device, const RestoreWindow& window,
                             std::uint64_t* lineage) {
    if (Status s = MarkRestoreOwedOn(device, window, lineage); !s.ok()) return s;
    return ForgetUnwrittenPages(device);
}

Status DiscardAdvisoryState(catalog::Catalog& catalog, bool waystone_recording) {
    // Every previous Waystone directory is unreachable from here on: its
    // pages are unlogged, so the copy holds them as of some instant of the
    // window and their children may be pages `ForgetUnwrittenPages` gave
    // back. An opted pattern gets a fresh, empty one, which names no stale
    // child (CF-S1b). Two walks of `sys.patterns`, whatever its length; the
    // second sets the instance's opted count (CF-S1c).
    auto opted = catalog.RenewPatternDirectories(waystone_recording);
    if (!opted.ok()) return opted.status();
    return catalog.DiscardAccessStats();
}

Status LayOutRestore(const RestoreRequest& request, RestoreLayout* out) {
    auto read = ReadManifest(request.backup_dir);
    if (!read.ok()) return read.status();
    const BackupManifest& manifest = read.value();
    // A backup another build wrote is refused, not read (BR-R7).
    if (manifest.superblock_version != kSuperBlockVersion || manifest.page_size != kPageSize ||
        manifest.segment_size != wal::kDefaultSegmentSize) {
        return Status::Unsupported(
            "restore: backup " + std::to_string(manifest.backup_id) + " was written by " +
            manifest.engine + " (superblock " + std::to_string(manifest.superblock_version) +
            ", page " + std::to_string(manifest.page_size) + ", segment " +
            std::to_string(manifest.segment_size) + "); this build reads superblock " +
            std::to_string(kSuperBlockVersion) + " only");
    }
    if (Status s = VerifyBackupFiles(request.backup_dir, manifest); !s.ok()) return s;
    if (Status s = VerifyBackupLog(request.backup_dir, manifest); !s.ok()) return s;

    // The data file: the backup's own (B), or the snapshot's (D).
    const bool has_data = std::any_of(manifest.files.begin(), manifest.files.end(),
                                      [](const BackupFile& f) {
                                          return f.name == kBackupDataFileName;
                                      });
    std::string data_from = request.data_file_from;
    if (data_from.empty()) {
        if (!has_data) {
            return Status::InvalidArgument(
                "restore: backup " + std::to_string(manifest.backup_id) +
                " is a snapshot backup (path D) with no data file of its own; name the "
                "snapshot's data file");
        }
        data_from = (fs::path(request.backup_dir) / kBackupDataFileName).string();
    }

    // A restore lays out onto nothing (BR-S8 case 7).
    std::error_code ec;
    if (fs::exists(request.data_file, ec) || !EmptyOrAbsent(request.wal_dir)) {
        return Status::AlreadyExists("restore: " + request.data_file + " or " + request.wal_dir +
                                     " already holds data; a restore never writes over a volume");
    }

    // PITR (BR-S7, BR-Q5 (b)): an LSN at or past the stop, never below it.
    const bool to_target = request.target_lsn != 0 || request.target_latest;
    if (request.target_lsn != 0 && request.target_lsn < manifest.stop_lsn) {
        return Status::InvalidArgument("restore: target lsn " +
                                       std::to_string(request.target_lsn) +
                                       " is below backup " + std::to_string(manifest.backup_id) +
                                       "'s stop lsn " + std::to_string(manifest.stop_lsn) +
                                       "; a backup restores to its stop or past it");
    }
    if (to_target && request.archive_dir.empty()) {
        return Status::InvalidArgument("restore: a target past the stop reads the WAL archive; "
                                       "name it");
    }

    // From here on the layout writes, and a failure takes back what it laid
    // out - the target held nothing - so a retry meets an empty target again.
    struct TakeBack {
        const RestoreRequest& request;
        bool keep = false;
        ~TakeBack() {
            if (keep) return;
            std::error_code ignored;
            fs::remove(request.data_file, ignored);
            fs::remove_all(request.wal_dir, ignored);
        }
    } take_back{request};

    for (const fs::path& dir : {fs::path(request.data_file).parent_path(), fs::path(request.wal_dir)}) {
        if (dir.empty()) continue;
        fs::create_directories(dir, ec);
        if (ec) return Status::IoError("restore: cannot create " + dir.string() + ": " + ec.message());
    }
    // Each copy is checked against the manifest as it is made: a file that
    // changed after `VerifyBackupFiles` read it is refused, not laid out.
    const auto copy_checked = [&](const std::string& from, const std::string& to,
                                  const std::string& name) -> Status {
        std::uint64_t size = 0;
        std::uint32_t crc = 0;
        if (Status s = CopyFileDurably(from, to, &size, &crc); !s.ok()) return s;
        for (const BackupFile& f : manifest.files) {
            if (f.name == name && (f.size != size || f.crc32c != crc)) {
                return Status::Corruption("restore: " + name + " changed while it was copied");
            }
        }
        return Status::OK();
    };
    // **Under a temporary name until the restore is marked owed in it**: a
    // crash between the copy and the mark must not leave a data file whose
    // copied anchor a mount would start from (BR-R4).
    const std::string preparing = request.data_file + ".restoring";
    fs::remove(preparing, ec);
    if (Status s = copy_checked(data_from, preparing,
                                request.data_file_from.empty() ? kBackupDataFileName : "");
        !s.ok()) {
        fs::remove(preparing, ec);
        return s;
    }
    std::uint64_t lineage = 0;
    {
        auto device = storage::FilePageDevice::Open(preparing);
        if (!device.ok()) return device.status();
        if (Status s = PrepareRestoredVolume(
                *device.value(), {manifest.start_lsn, manifest.stop_lsn, manifest.backup_id},
                &lineage);
            !s.ok()) {
            fs::remove(preparing, ec);
            // Named for the file the operator gave, not the temporary one.
            return s.WithMessage("restore: " + data_from + ": " + s.message());
        }
    }
    if (::rename(preparing.c_str(), request.data_file.c_str()) != 0) {
        return Status::IoError("restore: name " + request.data_file + ": " + std::strerror(errno));
    }
    if (Status s = SyncDirectory(fs::path(request.data_file).parent_path().string()); !s.ok()) {
        return s;
    }
    for (std::uint64_t seg = manifest.start_segment; seg <= manifest.stop_segment; ++seg) {
        const std::string name = std::string(kBackupWalDirName) + "/" + SegmentFileName(seg);
        if (Status s = copy_checked((fs::path(request.backup_dir) / name).string(),
                                    (fs::path(request.wal_dir) / SegmentFileName(seg)).string(),
                                    name);
            !s.ok()) {
            return s;
        }
    }

    std::uint64_t last_segment = manifest.stop_segment;
    wal::Lsn cut = 0;
    if (to_target) {
        // The archive's segments past the stop, in order, under the backup's
        // lineage; the first one missing ends what can be read (BR-R8).
        for (std::uint64_t seg = manifest.stop_segment + 1;; ++seg) {
            const fs::path archived =
                fs::path(request.archive_dir) / ArchiveName(manifest.lineage_id, seg);
            if (!fs::exists(archived, ec)) break;
            if (request.target_lsn != 0 && seg * manifest.segment_size >= request.target_lsn) {
                break;  // starts at or past the target: nothing in it is kept
            }
            const std::string to = (fs::path(request.wal_dir) / SegmentFileName(seg)).string();
            std::uint64_t size = 0;
            std::uint32_t crc = 0;
            if (Status s = CopyFileDurably(archived.string(), to, &size, &crc); !s.ok()) return s;
            last_segment = seg;
        }
        // The first record at or past the target is where the log is cut.
        auto device = wal::FileLogDevice::Open(request.wal_dir, 0, manifest.segment_size,
                                               manifest.start_segment);
        if (!device.ok()) return device.status();
        wal::Lsn first_past = 0;
        auto scanned = wal::ScanLog(*device.value(), 0, manifest.start_lsn,
                                    [&](const wal::DecodedRecord& r) -> Status {
                                        if (request.target_lsn != 0 &&
                                            r.header.lsn >= request.target_lsn) {
                                            first_past = r.header.lsn;
                                            return Status::Cancelled("cut");
                                        }
                                        return Status::OK();
                                    });
        device.value().reset();
        if (!scanned.ok() && first_past == 0) return scanned.status();
        // **A damaged archived segment is refused, never a short history.**
        // A scan stops at a record that does not decode as at a log's end
        // (`log_scanner.hpp`), so without this a corrupt segment in the middle
        // would end the replay there and leave the segments after it in
        // place, read by no mount. Every archived segment is sealed, so an
        // archive read whole ends cleanly.
        if (first_past == 0 && scanned.value().stopped_early) {
            const wal::Lsn end = scanned.value().end_lsn;
            return Status::Corruption(
                "restore: the log laid out from backup " + std::to_string(manifest.backup_id) +
                " and the archive does not decode past lsn " + std::to_string(end) +
                " (segment " + std::to_string(end / manifest.segment_size) +
                "); an archived segment is damaged");
        }
        if (request.target_lsn != 0 && first_past == 0) {
            const wal::Lsn end = scanned.value().end_lsn;
            if (end < request.target_lsn) {
                return Status::NotFound(
                    "restore: the archive's history for lineage " +
                    std::to_string(manifest.lineage_id) + " ends at lsn " + std::to_string(end) +
                    " (segment " + std::to_string(last_segment) + "), below target lsn " +
                    std::to_string(request.target_lsn) + "; segment " +
                    std::to_string(last_segment + 1) + " is missing from " + request.archive_dir);
            }
        }
        if (first_past != 0) {
            cut = first_past;
            if (Status s = CutLogAt(request.wal_dir, cut, manifest.segment_size, last_segment);
                !s.ok()) {
                return s;
            }
        }
    }

    take_back.keep = true;
    if (out != nullptr) {
        out->backup_id = manifest.backup_id;
        out->start_lsn = manifest.start_lsn;
        out->stop_lsn = manifest.stop_lsn;
        out->cut_lsn = cut;
        out->last_segment = last_segment;
        out->lineage_id = lineage;
    }
    return Status::OK();
}

}  // namespace kds::server
