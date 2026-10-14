# Backup and restore

What is built and enforcing. Built by BR (`instructions/v3.0.0/workorder-br-backup-restore.md`,
`instructions/v3.0.0/br-s0-census.md`); the operator's procedures are
`manual/server/backup-restore.md`.

## 1. The one foundation

**B1 - Restore is mount recovery; there is no second redo.** A backup is a
start LSN, a stop LSN, a data file and the log between them. Restoring it
lays them out and marks the restore owed; the mount recovers from the start
like any crash mount. A difference between restore and crash recovery is a
defect in recovery.

**B2 - A restore that has not reached its stop does not open.**
`[quiet-wrong]` guard. `MarkRestoreOwed` (`server/restore.hpp`) writes, in
page 0 of the laid-out volume, anchor slot 0 = `{start, start, stop,
segment(start)}` and `restore_backup_id` (superblock v23). Slot 0's durable
point is the stop, and analysis refuses a log that ends below it
(`wal/analysis.cpp`), so every mount until the restore completes - whoever
reads slot 0 - starts at the start and refuses short of the stop, named
"restore of backup N ... did not complete; the instance does not open". The
first anchor published after a completed recovery clears
`restore_backup_id` in the same page image (`SuperBlockCheckpointAnchor`).

**B3 - The start is the durable redo start `D` at `BACKUP START`, pinned.**
`D` is what a crash mount starts from, so it is a valid start; no other LSN
is (analysis seeds losers only from `CHECKPOINT_BEGIN`s in range). `D` is
read and the pin set in one hold of `BackupControl`'s mutex, and the
recycler's bound and the BF reclaim gate are computed as `min(D, pin)` under
the same hold. Without the pin a page freed with no record during the window
would be named by a replayed record with its bit clear in the copy, and a
segment the restore needs could be removed.

**B4 - The start comes from the backup, never from the copied superblock.**
`[quiet-wrong]` guard. A fuzzy copy's page 0 can carry an anchor above pages
copied before it; B2's slot-0 rewrite replaces it.

## 2. Taking a backup

**B5 - The data copy is fenced** (`storage/page_write_fence.hpp`). Path B's
copier reads the data file through the device's write fence: armed, a write
and the copier's read of an overlapping page range never run at once, so no
copied page is torn by the engine's own write - headered, headerless, free
map or superblock. Disarmed, a data-file write pays three atomic operations
(BR-R6). No full-page images are logged (`br-s0-census.md` H5: there is no
single decision site).

**B6 - The copy is bounded by the stop on both sides.** The copier copies to
the capacity read at START; STOP copies the tail the file grew, **then**
appends BACKUP_END, so every copied page's `page_lsn` is below the stop
(WAL-before-data). After the stop it writes every resident map region's
free and headerless map, **as it stands in memory**, into the copy
(`DevicePageStore::MapImages`) - never to the source's device, where a bit
ahead of an unlogged page's bytes is BI-Q12's hazard - so the copy's maps
carry the bit of every page a record up to the stop names, Waystone pages
included, which are created with no record (a crash's form of that hole is
closed by every mount discarding Waystone, `waystone-concpets.md`).

**B6a - A restore makes unlogged state unreachable** (`restore.hpp`).
Marking the restore owed is the layout's first write after the data copy,
so an interrupted layout never mounts from the copied anchor. Then every
user page the copied maps call allocated whose bytes are all zero is given
back (`ForgetUnwrittenPages`): a logged one is recreated by its record, an
unlogged one was never written. At the restore's mount - as at every mount
(`waystone-concpets.md`) - every Waystone root is cleared and
`sys.access_stats` emptied in place (`DiscardAdvisoryState`), so nothing
unlogged can reach a page given back (invariant 8, CR6).

**B7 - The stop is where the backup's log ends.** BACKUP_END is appended and
its segment sealed in one hold of the stream latch (`WalStream::AppendAndSeal`),
then synced; the stop is the record's end. A record another core appends
lands in the next segment.

**B8 - A backup carries its own log.** STOP's worker copies the segments
`[segment(start), segment(stop)]` into the backup, under the pin, and writes
the manifest last; the pin is released after it. Path D's log comes from the
backup too, not the snapshot, so a separate WAL volume needs no ordered
second snapshot.

**B9 - A backup is a directory with a manifest** (`server/backup_manifest.hpp`):
format version, backup id, label, path (B or D), `git describe` of the
writing build, superblock version, page and segment size, lineage, start
and stop LSN and segments, and every file with its size and CRC32C, closed
by a CRC32C of the text. An unknown format, another build's superblock or a
mismatching file is refused by name.

**B9a - Every page and every record a backup carries is checked by its own
CRC32C** (`restore.hpp`). The data file - path B's copy, or a snapshot's,
in the backup or named beside it - is walked against its own maps: every
page they call allocated, and every system page, must pass its checksum
unless it is headerless or all zeros (never written). STOP's worker runs it
before the manifest, so a backup holding a torn or damaged page fails and is
no backup; `--verify-backup` and the restore's layout run it again, the
layout before it marks anything. The log is scanned from the start: every
record's CRC32C must decode to at least the stop. The manifest's per-file
CRC32C says the files are the ones written; these say what was written is
whole.

**B10 - One backup at a time.** An open backup is ended by `BACKUP ABORT`,
by an hour idle with nothing running (`[provisional]`), or by a restart;
none writes BACKUP_END, so none is a backup.

## 3. The WAL archive

**B11 - Every sealed segment is archived before it can be removed**
(`server/wal_archive.hpp`). On when `archive_dir` or `archive_command` is
set (one, never both). A thread archives in order every segment below the
durable LSN's; its floor joins the recycler's bound. `archive_dir` never
writes a different body under one name.

**B12 - Every archived name carries the lineage**:
`kds-<lineage>-<segment>.log`. A fresh lineage is drawn at creation and by
every restore, so a restored instance never writes over the original's
history (BR-R10).

**B13 - Fail-closed** (BR-Q2 (a)): while archiving fails and the WAL
filesystem has fewer than 4 segments free, `INSERT`, `UPDATE`, `DELETE`,
`CREATE`, `DROP`, `ALTER` and `PURGE` are refused by name.

## 4. Point-in-time recovery

**B14 - A target is an LSN at or past the stop** (BR-Q5 (b); the commit
record carries no time). The restore reads the archive's segments after the
stop under the backup's lineage, cuts the log at the first record at or past
the target (`LayOutRestore`), and recovers; a transaction open at the cut is
a loser. `latest` reads every archived segment.

## 5. What this does not do

Incremental backup, compression, encryption, an object-storage target,
parallel copy, concurrent backups, a logical dump and a streaming standby
are not built. A torn page from the device itself (a snapshot that does not
freeze the filesystem, a power cut) is refused, not healed - the image
cadence of `wal.md` §10 is not built. A page the original freed after its map
region was copied stays allocated in the restore: a leak, as after a crash.
