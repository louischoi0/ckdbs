#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kds/base/status.hpp"

// A backup's manifest and the file I/O it rests on (BR-R7, BR-R8,
// `instructions/v3.0.0/workorder-br-backup-restore.md`).
//
// **A backup is a directory with a manifest; a directory without one is not
// a backup.** The layout is fixed:
//
//   <dir>/backup_manifest       this file, written last and synced
//   <dir>/kds.db                the data file (path B's copy, or a snapshot's)
//   <dir>/wal/wal-0-<n>.log     the segments [start, stop] (BR-Q6)
//
// The manifest is text, one `key value` per line, its last line `end
// <crc32c>` over every byte before it, so a torn or edited manifest is
// refused rather than read. Every file it names carries its size and
// CRC32C. The users file and the TLS key and certificate are configuration,
// not data, and are never in a backup (`br-s0-census.md` §3 item 6).
namespace kds::server {

inline constexpr std::uint32_t kBackupManifestVersion = 1;
inline constexpr const char* kBackupManifestName = "backup_manifest";
inline constexpr const char* kBackupDataFileName = "kds.db";
inline constexpr const char* kBackupWalDirName = "wal";

struct BackupFile {
    std::string name;  // relative to the backup directory
    std::uint64_t size = 0;
    std::uint32_t crc32c = 0;
};

struct BackupManifest {
    std::uint32_t format_version = kBackupManifestVersion;
    std::uint64_t backup_id = 0;
    std::string label;
    char path = 'B';            // 'B' (a data file in the backup) or 'D' (none)
    std::string engine;         // `git describe --tags` of the writing build
    std::uint32_t superblock_version = 0;
    std::uint32_t page_size = 0;
    std::uint64_t segment_size = 0;
    std::uint64_t lineage_id = 0;
    std::uint64_t start_lsn = 0;
    std::uint64_t stop_lsn = 0;
    std::uint64_t start_segment = 0;
    std::uint64_t stop_segment = 0;
    std::uint64_t created_unix = 0;
    std::vector<BackupFile> files;
};

// The segment file name a single-stream log uses (`FileLogDevice`).
std::string SegmentFileName(std::uint64_t segment_no);

// Copies `from` to `to` (refusing an existing `to`), through a temporary
// name, syncing the file and then the directory, so `to` exists only whole.
// Reports the copy's size and CRC32C.
Status CopyFileDurably(const std::string& from, const std::string& to, std::uint64_t* size,
                       std::uint32_t* crc32c);

// Writes all of `[data, data + len)` at `offset`, through EINTR and short
// writes.
Status PwriteAll(int fd, const std::byte* data, std::size_t len, std::uint64_t offset,
                 const std::string& what);

// Size and CRC32C of a file, read whole.
Status ChecksumFile(const std::string& path, std::uint64_t* size, std::uint32_t* crc32c);

// `fsync` on a directory, so an entry created or renamed in it is durable.
Status SyncDirectory(const std::string& dir);

// Writes `manifest` to `<dir>/backup_manifest` through a temporary name,
// synced, then the directory synced. Every file `manifest.files` names must
// already be in `dir`; their sizes and checksums are the caller's.
Status WriteManifest(const std::string& dir, const BackupManifest& manifest);

// Reads and checks `<dir>/backup_manifest`: its closing checksum, its format
// version (an unknown one is refused, BR-R7), and the fields every backup
// carries. Does not read the files it names - `VerifyBackupFiles` does.
StatusOr<BackupManifest> ReadManifest(const std::string& dir);

// Checks every file the manifest names against its size and CRC32C, and
// that the segments `[start_segment, stop_segment]` are all named - a
// missing, short or corrupt one is refused by its number and the LSN it was
// needed for (BR-R8). Nothing is skipped.
Status VerifyBackupFiles(const std::string& dir, const BackupManifest& manifest);

}  // namespace kds::server
