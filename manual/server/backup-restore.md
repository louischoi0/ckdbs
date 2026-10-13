# Backup and Restore

How to take a backup of a running `kds_server`, keep a WAL archive, verify,
list and expire backups, and restore - to a backup's stop or to a later LSN.
Verified against `src/server/backup.cpp`, `src/server/restore.cpp`,
`src/server/wal_archive.cpp`, `src/server/main.cpp` and
`tools/kds_backup.py` (BR, `instructions/v3.0.0/workorder-br-backup-restore.md`;
the design is `docs/spec/backup.md`).

---

## 1. What a backup is

A backup is a **directory with a manifest**. A directory without a
`backup_manifest` is not a backup.

```
<dir>/backup_manifest       written last, synced; its last line checksums the rest
<dir>/kds.db                the data file (path B's copy, or a snapshot's)
<dir>/wal/wal-0-<n>.log     the log segments from the backup's start to its stop
```

Every backup has a **start** and a **stop** (both LSNs). Restoring it is
mount recovery from the start, and **the restored instance does not open
until its recovery has passed the stop**. A backup carries its own log for
that window, so it restores with the WAL archive off.

The users file and the TLS key and certificate are configuration, not data:
they are never in a backup. Keep them with your configuration.

Two ways to take one:

| Path | The data file comes from | Needs |
|---|---|---|
| **B - base backup** | the server, which copies it while serving | nothing |
| **D - snapshot** | an atomic snapshot you take between START and STOP | a snapshot that freezes the filesystem (LVM, ZFS, btrfs), or `scripts/backup/snapshot-freeze.sh` |

One backup runs at a time.

## 2. Path B: a base backup

```sh
tools/kds_backup.py base --target /backups/2026-10-12
```

This runs, on the server:

| Statement | What it does |
|---|---|
| `BACKUP START '<label>'` | pins the log from the durable redo start (the start LSN) and logs `BACKUP_BEGIN` |
| `BACKUP COPY TO '<dir>'` | the server copies its data file into `<dir>/kds.db` on a thread, under a write fence so no page is copied half-written; returns at once |
| `BACKUP STATUS` | `copying=1` until the copy is done (`pages_copied=`, `data_pages=`) |
| `BACKUP STOP TO '<dir>'` | copies the pages the file grew, logs `BACKUP_END` (the stop), copies the maps, then on a thread copies the log and writes the manifest; the pin is released after |

`BACKUP STATUS` says `state=idle last_outcome=done` when the backup is
whole. The directories are paths **on the server's host**. The statements
need the admin role and run only outside an explicit transaction.

The copier runs at up to 256 MiB/s (`[provisional]`). While it copies, a
write to the data file that meets the page range being read waits for that
read (`SHOW META`: `backup_fence_writes_waited`).

## 3. Path D: a snapshot backup

```sh
tools/kds_backup.py snapshot --target /backups/snap-1 \
    --command 'scripts/backup/snapshot-lvm.sh vg0 data /srv/kds/kds.db'
```

`kds_backup` runs `BACKUP START`, then your command (with `KDS_BACKUP_DIR`
and `KDS_BACKUP_ID` set), then `BACKUP STOP TO '<dir>'`. Your command takes
an atomic snapshot of the volume holding the data file and either copies the
data file into `$KDS_BACKUP_DIR/kds.db` or keeps the snapshot elsewhere (you
then name its data file at restore).

**The snapshot must be atomic**: every write the kernel accepted in it, none
half in it. An LVM, ZFS or btrfs snapshot is; so is
`scripts/backup/snapshot-freeze.sh <server-pid> <data_file>`, which stops the
server process (`SIGSTOP`), copies the data file and resumes it - for hosts
with no snapshot-capable volume (the server stalls for the copy). A cloud
disk snapshot that does not freeze the filesystem is a crash image and may
hold a torn page, which the restore refuses (§8).

**The log does not come from the snapshot.** `BACKUP STOP` copies the
window's segments into the backup after the snapshot, so a separate WAL
volume needs no second snapshot and no ordering.

## 4. The WAL archive

Set one of these in `kds.conf`:

```
archive_dir = /archive/kds          # copy, fsync the file, fsync the directory
archive_command = cp %p /archive/kds/%f   # exit 0 means durable; %p path, %f name
```

Archiving is on when either is set; setting both is refused at startup.
Every sealed segment is archived, in order, as
`kds-<lineage, 16 hex>-<segment>.log`. **No segment is removed from the log
before it is archived.** `SHOW META`: `wal_archive_next_segment`,
`wal_archive_lag_segments`, `wal_archived`, `wal_archive_failures`,
`wal_archive_blocking`, and while failing `wal_archive_last_error`.

A failure is retried with backoff (1 s doubling to 30 s). If the archive
keeps failing **and** the WAL filesystem has less than 4 segments (256 MiB)
free, every statement that writes the log - `INSERT`, `UPDATE`, `DELETE`,
`CREATE`, `DROP`, `ALTER`, `PURGE` - is refused with `ERR` naming the
archive's failure, until archiving recovers. Reads go on. Fix the archive
destination; nothing else is needed.

`archive_dir` never overwrites: a name already present with the same bytes
is skipped (a restart), and with different bytes stops archiving with
`wal_archive_last_error`. An `archive_command` must be idempotent.

**Lineage.** A volume's history has a lineage id (`SHOW META`:
`wal_lineage`). Every restore that opens takes a new one, so a restored
instance archiving into the same place never writes over the original's
segments.

## 5. Verify, list, expire

```sh
tools/kds_backup.py verify /backups/2026-10-12          # = kds_server --verify-backup
tools/kds_backup.py list /backups
tools/kds_backup.py expire /backups --keep 7 --archive /archive/kds
tools/kds_backup.py expire /backups --keep-since 1760000000 --archive /archive/kds
```

`verify` checks the manifest and every file's size and CRC32C, that every
segment from the start to the stop is present and that the log decodes -
every record's CRC32C - from the start to the stop, and **every data page by
its own CRC32C** (allocated pages against the file's own maps; headerless
pages carry none and never-written pages are zeros). For a snapshot kept
outside the backup, name its data file: `kds_backup verify DIR
--snapshot-data FILE`. On success it stamps `last_verified`, which `list`
shows.

The same page check runs when a backup stops - a data file in the backup
with a page that fails it fails the backup (`BACKUP STATUS`:
`last_outcome=failed`, the pages named) and no manifest is written - and
again when a restore lays a backup out, before it changes anything. `expire` removes the backups outside the
retention (it refuses a retention that keeps none) and the archived segments
of a retained backup's lineage below the oldest retained start of that
lineage. **It never removes a segment a retained backup needs**, and never
touches a lineage no retained backup names (that may be a live history).

## 6. Restore

```sh
tools/kds_backup.py restore /backups/2026-10-12 --config /etc/kds/new.conf
# or directly:
kds_server --restore-from /backups/2026-10-12 --config /etc/kds/new.conf
```

The configured `data_file` and `wal_dir` **must hold nothing**. The restore
checks the manifest, verifies every file, copies the data file and marks the
restore owed in it at once (a restore interrupted after that never opens
short of the stop), gives back the pages the copy holds as never written (a
second read of the data file), copies the segments, and mounts it: recovery
replays from the backup's start, and the instance opens only once it has
passed the stop. It then serves, or exits with `--restore-only`. Path D: name
the snapshot's data file with `--restore-data <file>` when the backup does
not hold one.

`SHOW META` on the restored instance prints `recovery_restore_backup=`,
`recovery_restore_start=` and `recovery_restore_stop=` for the mount that
completed it.

## 7. Point-in-time recovery

```sh
kds_server --restore-from /backups/b --restore-archive /archive/kds \
           --restore-target 812345678 --config new.conf
kds_server --restore-from /backups/b --restore-archive /archive/kds \
           --restore-target latest --config new.conf
```

A target is an **LSN** (a commit time is not recorded in the log, so there is
no time target). The restore reads the archived segments after the backup's
stop under the backup's lineage, cuts the log at the first record at or past
the target, and recovers: everything committed before that record is there,
nothing after it is, and a transaction open at the cut is rolled back.
`latest` replays every archived segment. A target below the stop is refused.

To find an LSN: `SHOW META` prints `wal_durable_lsn`, the log's durable end;
note it before an operation you may want to undo, and restore to it.

## 8. Refusals and what to do

| Message | Cause | What to do |
|---|---|---|
| `BACKUP START: backup N (...) is open` | one backup at a time | `BACKUP STOP` or `BACKUP ABORT` it |
| `BACKUP ... inside a transaction is not implemented` | inside `BEGIN` | run it in autocommit |
| `BACKUP STOP: ...'s data copy has X of Y pages` | the copy is still running | wait for `copying=0` |
| `BACKUP STOP: ...'s data copy failed` | the copier hit an I/O error | `BACKUP ABORT`, fix the target, take a new backup |
| `BACKUP STOP: <dir> holds <name>` | the directory is not new | a new directory |
| `backup: <dir> has no backup_manifest` | not a backup, or STOP never finished | take a new backup |
| `backup: manifest format N is not this build's` | another build's backup | restore with the build that wrote it |
| `backup N: wal/wal-0-K.log is ... the manifest says ...` | a file was changed or damaged | use another backup; the restore wrote nothing |
| `backup N: segment K (lsn ...) is not in the backup` | a segment is missing | use another backup |
| `data file: N of M written page(s) fail their checksum (page ...)` | a torn page (a snapshot that did not freeze the filesystem) or damage | take the backup again; for path D use a freezing snapshot |
| `backup N: its log decodes only to lsn ...` | a damaged segment the manifest's checksum did not catch | use another backup |
| `restore: ... already holds data` | the target is not empty | an empty target |
| `restore: target lsn T is below ... stop lsn` | the target precedes the backup's stop | an older backup |
| `restore: the archive's history ... ends at lsn E ... segment K is missing` | the archive does not reach the target | a lower target, or find segment K |
| `restore of backup N (start ..., stop ...) did not complete; the instance does not open` | the log does not reach the stop, or recovery failed | the message's tail says which; a restore that stops short never opens |
| `ERR ... the WAL archive is failing (...)` on a write | §4's fail-closed | fix the archive destination |

An open backup idle for an hour with nothing running is aborted
(`[provisional]`); a server restart ends any open backup. Neither leaves a
`BACKUP_END`, so neither can be mistaken for a backup.

## 9. What a backup does not do

No incremental backup, compression, encryption, object-storage target,
parallel copy, concurrent backups, logical dump or standby. A torn page from
the device itself (a crash-consistent snapshot) is refused, not healed: the
engine logs no per-checkpoint page images (`docs/inflight/known-gaps.md`).
`kds_backup` reaches the admin role only on an `auth = off` server (its KWP
client has no SCRAM).
