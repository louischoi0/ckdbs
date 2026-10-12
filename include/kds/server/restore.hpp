#pragma once

#include <string>

#include "kds/base/status.hpp"
#include "kds/server/superblock.hpp"

namespace kds::storage {
class PageDevice;
}  // namespace kds::storage

// Restore, the offline half (BR, `instructions/v3.0.0/workorder-br-backup-restore.md`,
// `br-s0-census.md` §2). Everything here runs against a volume no instance
// has open: it lays a backup out and marks the restore owed, and the mount
// does the rest - restore is mount recovery (BR-R1), so there is no second
// redo here.
namespace kds::server {

// **What a restored volume cannot trust, made unreachable** (BR-S8). The
// copy holds every page as of some instant of the window; redo brings the
// logged ones to the stop, and nothing brings the unlogged ones anywhere -
// Waystone pages and `sys.access_stats` (invariant 8 and CR6 make both
// advisory). Two halves:
//
// `ForgetUnwrittenPages` - offline, on the laid-out data file: every user
// page the copied maps call allocated whose bytes are all zero (or past the
// file's end) is given back. A logged one is recreated by its record, which
// re-sets the bit (`CreateAt`); an unlogged one was never written and would
// stay allocated and unreadable for good. Headerless pages are left: their
// zeros are an empty directory.
//
// `DiscardAdvisoryState` - at the restore's mount, after recovery: every
// pattern's Waystone root is cleared and `sys.access_stats` emptied, so no
// unlogged structure can reach a page the first half gave back.
Status ForgetUnwrittenPages(storage::PageDevice& device, std::uint64_t* forgotten = nullptr);

// A copied data file made a restore's volume: the restore marked owed
// **first** (`MarkRestoreOwedOn`), so a crash at any later point of the
// layout leaves a volume that refuses to open short of the stop rather than
// one that mounts from the copied anchor (BR-R4); then
// `ForgetUnwrittenPages`. `LayOutRestore`'s and the simulation's one path.
struct RestoreWindow;
Status PrepareRestoredVolume(storage::PageDevice& device, const RestoreWindow& window,
                             std::uint64_t* lineage = nullptr);

}  // namespace kds::server

namespace kds::catalog {
class Catalog;
}  // namespace kds::catalog

namespace kds::server {

Status DiscardAdvisoryState(catalog::Catalog& catalog);

// A backup's window: where its replay starts and the stop it must reach.
struct RestoreWindow {
    std::uint64_t start_lsn = 0;
    std::uint64_t stop_lsn = 0;
    std::uint64_t backup_id = 0;
};

// Marks the restore of `window` owed in page 0 of the data file at
// `data_file`, with a fresh lineage id (BR-R10), and syncs it: anchor slot 0
// becomes `{start, start, stop, segment(start)}`, so the next mount starts at
// the backup's start, never at the copied anchor (BR-R4), and does not open
// short of the stop (BR-R2). Refuses a page 0 that is not a whole superblock
// of this build's version, a window whose start is not below its stop, and
// backup id 0. Returns the new lineage id through `lineage` when non-null.
Status MarkRestoreOwed(const std::string& data_file, const RestoreWindow& window,
                       std::uint64_t* lineage = nullptr);
// The same on a device no store has open (the simulation harness's restore).
Status MarkRestoreOwedOn(storage::PageDevice& device, const RestoreWindow& window,
                         std::uint64_t* lineage = nullptr);

// What to restore, and onto what.
struct RestoreRequest {
    std::string backup_dir;      // a directory with a manifest
    std::string data_file;       // the new volume's data file (must not exist)
    std::string wal_dir;         // its log directory (absent or empty)
    // Path D: the snapshot's data file, since the backup carries none. Empty
    // for path B, whose data file is the backup's own.
    std::string data_file_from;
    // PITR (BR-S7, BR-Q5 (b)): replay past the stop through the WAL archive
    // to the first record at or past `target_lsn` (refused below the stop),
    // or through every archived segment (`target_latest`). Neither: the
    // backup's own stop.
    std::string archive_dir;
    std::uint64_t target_lsn = 0;
    bool target_latest = false;
};

struct RestoreLayout {
    std::uint64_t backup_id = 0;
    std::uint64_t start_lsn = 0;
    std::uint64_t stop_lsn = 0;
    std::uint64_t cut_lsn = 0;       // 0 when the log was not cut
    std::uint64_t last_segment = 0;  // the last segment laid out
    std::uint64_t lineage_id = 0;    // the restored volume's fresh lineage
};

// Lays a backup out as a new volume and marks its restore owed (BR-S6): the
// manifest is checked (an unknown format or another build's superblock is
// refused, BR-R7) and every file verified against it (a missing, short or
// corrupt one is refused by name, BR-R8); the target must hold nothing; the
// data file and the segments `[start, stop]` are copied, durably; for a
// target past the stop the archive's segments under the backup's lineage
// follow, and the log is cut at the first record at or past the target;
// then `MarkRestoreOwed`. The next mount recovers, refuses to open short of
// the stop, and takes the fresh lineage (BR-R10). Nothing here replays.
Status LayOutRestore(const RestoreRequest& request, RestoreLayout* out = nullptr);

}  // namespace kds::server
