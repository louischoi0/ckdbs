# BR-S0 — the census

**Read 2026-10-12** on `br-backup-restore` at `3b9d5550` (the order's
corrections over `fb953f07`), `[source-read]`, no code changed. It answers
H1-H9 of `workorder-br-backup-restore.md` §3, names the persistence site of
the stop LSN (BR-R2) and the torn-page decision (BR-R5, BR-Q1), and restates
every rule of §2 that changes. Paths are relative to the repository root.

## 1. The hypotheses

| ID | Verdict | Evidence | What it changes |
|---|---|---|---|
| H1 | **True.** Mount takes its start from a `WalAnchorFields` its caller builds | `src/server/expeditor.cpp:846-849` (the log's `first_needed`), `:967-968` (`RecoverCoreAtMount(…, superblock.wal_anchor(0), …)`), `:1151-1153` (`ResumeAssertionsAfterRecovery`'s start); `src/server/mount_recovery.cpp:24-26` copies it into `wal::AnalysisStart{redo_start_lsn, anchor_durable_lsn}` | BR-R4 is a mount change of three call sites reading one value, not a change to analysis, redo or undo |
| H1a | **Found: the stop refusal already exists.** Analysis refuses a log whose end lies below `anchor_durable_lsn` | `src/wal/analysis.cpp:262-268` | BR-R2's refusal is that check with the stop LSN as its bound; BR-S1 names it |
| H1b | **Found: a start must be a published redo start.** Analysis seeds its loser table only from `CHECKPOINT_BEGIN`s inside the scan | `src/wal/analysis.cpp:223-243`, `:90`, `:185-192` | BR-R3's `D` qualifies (it is what a crash mount starts from); no other LSN may be a start |
| H2 | **True.** Durable state is `data_file` and `wal_dir` | `src/storage/file_page_device.cpp:59` (the one data-file `open`), `src/wal/file_log_device.cpp:190-192` (segment names `wal-<core>-<n>.log`); the superblock is page 0 of the data file | The manifest lists the data file and the segments, nothing else |
| H3 | **True only with the pin.** BF's free writes no record (`src/storage/device_page_store.cpp:1562-1620`, the one caller `src/server/page_reclaim.cpp:342`), and a replayed record naming a page whose copied bit is clear fails the mount (`src/wal/redo.cpp:437-450`, the bit read before the `page_lsn` test). The gate reads `D` in one place | `src/server/expeditor.cpp:1314-1317` → `src/server/page_reclaim.cpp:72` | **`[quiet-wrong]` closed by construction, loud otherwise**: the gate takes `min(D, backup_pin)`, so no page of a drop committed at or above the start is freed during the window; a drop committed below it has no record in `[start, stop]` |
| H4 | **False as worded, and the question moves.** A file copy can tear a page against the store's own `pwrite` (no file lock: `src/storage/file_page_device.cpp:102,135`), and redo's heal is not reliable: an image of a checksum-failed page calls `CreateAt` (`src/wal/redo.cpp:425`), which `ClaimNamedIdLocked` refuses for an allocated id whose device bytes are not zeros (`src/storage/device_page_store.cpp:1312`). No cell covers the heal (`pages_healed` appears in no test) | as cited | BR-R5 is replaced (§2): the copier reads each page under a device-level write fence, so no copied page is ever torn by the engine's own write, and no image is needed. The heal defect is recorded in `docs/inflight/bugs/` |
| H5 | **False.** There is no single decision site: every mutation appends, then stamps (`DevicePageStore::StampPageLsn`, `src/storage/device_page_store.cpp:1679-1731`), and the decision cannot be re-checked where the LSN is assigned - the stream latch is inner to the page latch and the stream cannot see a page (`include/kds/storage/device_page_store.hpp:165-172`, `src/wal/stream.cpp:261,281`) | as cited | Confirms the replacement of BR-R5: an armed image would need the race of BR-R5 (i) closed by a mechanism the tree does not have |
| H6 | **True, with the seam named.** The seal-and-durable point is inside `WalStream::StartSegment` after its `SyncDevice` (`src/wal/stream.cpp:96-98`), under the stream latch on the appending thread; nothing outside tests forces a seal (`WalStream::Seal`, `stream.cpp:185-189`, one test caller) | as cited | The archiver does not hook the roll: it reads `end_segment()` - every segment below `end_segment() - 1` is sealed and durable - from its own thread, so BR adds nothing to the hold BQ measures. `BACKUP STOP` adds a `SealAndRoll` under the latch, one call per backup |
| H7 | **True.** No recycling can run during recovery: the recycler is wired after `RecoverCoreAtMount` (`src/server/expeditor.cpp:1175-1182`), and the first removal follows the completion checkpoint's publish (`src/server/superblock_checkpoint_anchor.cpp:166-183`), which runs only after the scan passed the stop | as cited | BR-S1 needs no suppression of its own; it clears the restore fields in the completion checkpoint's own page-0 image |
| H8 | **True, given H3's pin.** A page created in the window is recreated by its `PAGE_INIT` or image through `CreateAt` (`src/wal/redo.cpp:432-441`), which sets its bit (`device_page_store.cpp:1318-1320`); the allocation floor rises past every page the scan names (`src/wal/high_water.cpp:35-36`). A page created before the start had its bit on the device before `D` was (a publish writes the maps before page 0, `docs/spec/wal.md` §11 step 3), and the only free is the gated reclaim. The free list is memory only (`device_page_store.hpp:1210`) | as cited | The copier copies to the capacity read at `BACKUP START`. A page the original freed after its map region was copied stays allocated in the restore: a leak, the same one a crash leaves (`known-gaps.md`) |
| H9 | **False.** `sudo` needs a password; `lvcreate` and `btrfs` exist but cannot run; `/` and `/tmp` are ext4 | `sudo -n true`, `findmnt` | BR-S3's drill takes its snapshot by freezing the server process (`SIGSTOP`, copy, `SIGCONT`); `scripts/backup/snapshot-lvm.sh` is the reference and is not run here |

Two further facts the census found:

- **The commit record carries no time** (`include/kds/wal/record.hpp:51`,
  `src/wal/manager.cpp:406,438`: an empty payload). PITR by commit time
  needs a new payload, a format event. BR-Q5's proposal therefore falls to
  **(b), LSN only**.
- **The superblock has room**: 2,132 of 8,192 bytes used
  (`include/kds/server/superblock.hpp:459`); new fields go after
  `core_count`, 8-aligned, and the version moves 22 → 23 under the standing
  order of 2026-10-07 (older volumes refused, no migration).

## 2. Rules restated

**BR-R2 (restated) - the stop LSN lives in the superblock.** Version 23 adds
`lineage_id`, `restore_redo_start`, `restore_stop_lsn` and
`restore_backup_id` after `core_count`. A restore writes the three restore
fields and a fresh lineage into page 0 and syncs it before the mount reads
anything else; a mount that finds `restore_stop_lsn != 0` starts at
`restore_redo_start` and refuses to open short of `restore_stop_lsn`
(H1a's check, named); the completion checkpoint's page-0 image clears them,
so the fields and the anchor change in one page write. A crash anywhere
before that write mounts the restore again from the same start.

**BR-R5 (replaced) - the copy is fenced, not imaged.** The data file's
device carries a write fence (`storage::PageWriteFence`): disarmed, a write
pays one branch on an atomic flag plus an in-flight count; armed by the
copier, a write and the copier's read of an overlapping page range never
run at once. Every data-file write goes through the device
(`WriteBack`'s runs, `FlushMaps`, the headerless zero write, `PersistPage`
via `WriteBack`), so no copied page - headered, headerless, free map or
superblock - is torn by the engine. The copier also verifies every headered
page's CRC32C and refuses the backup naming a page that fails it: that page
is corrupt on the source. The torn page a *device* leaves at a power cut
stays T4's gap, outside BR.

**BR-R6 (restated)** - with BR-R5 replaced, no mutation path changes. The
hot-path cost is the fence's disarmed branch on each data-file write and
the archiver's absence.

**BR-R9 (restated) - the copier and the archiver are threads, not
reactor tasks.** The census's model for off-reactor I/O is
`storage::DataSyncer` and the log device's preparer, both threads; a
reactor task issuing a `pread` per page would put the copy's I/O on the
reactor, which BI spent a milestone taking the checkpoint's off. The
copier and the archiver touch no latch-guarded structure: the copier reads
the device through the fence, and the archiver reads sealed segment files.
Throttled by a `[provisional]` rate.

**BR-Q1** - the proposal is revised from (a) to **(c), refined**: the
copier reads under the device's write fence, which covers B entirely; D's
snapshot is atomic when it freezes the filesystem (LVM, ZFS, btrfs, the
freeze script), and a crash-consistent snapshot that does not freeze is a
crash image, whose torn page is T4's. The order's objection to (c) - "covers
B and not D" - was about a page latch, which a fence at the device does not
need.

**D's log comes from the backup, not the snapshot.** `BACKUP STOP` copies
the segments `[start, stop]` into the backup directory for path D as for
path B (BR-Q6), so a D restore is the snapshot's data file plus the
backup's own log, and the two-volume ordering of BR-S3 is met by
construction: the log is copied after the data volume's snapshot.

## 3. Review (critics-developer, on `br-backup-restore` at `27f8a0ae`)

Eight findings; six change the design above, two confirm it. Where this
section and §1-§2 disagree, this section governs.

1. **H8 was wrong: Waystone pages are created with no record**
   (`src/stats/waystone_dir.cpp:47`, `:104`, `src/stats/trail_store.cpp:126`),
   so redo never recreates them and the allocation floor does not count
   them. A free map copied before such a page's bit reached the device
   leaves the id free in the restore; the cursor then hands it to a logged
   page, and the next Waystone write lands on it unlogged - lost rows, a
   breach of invariant 8. `[quiet-wrong]`. **Applied**: `BACKUP STOP` copies,
   under the fence, every page between the capacity the copier reached and
   the capacity at that moment; appends `BACKUP_END` (the stop); then writes
   the maps (`DevicePageStore::WriteMaps`) and copies every map page of
   every region. The maps are copied **after** the stop, so they carry the
   bit of every page any record up to the stop can name - including a
   Waystone root a `sys.patterns` row replayed below the stop points at. A
   bit for a page created after the stop only leaks that page in the
   restore, as a crash's floor does. A crash has a narrower
   form of the same hole (a Waystone parent written back ahead of the map
   write); it is the engine's, not BR's, and is recorded in
   `docs/inflight/bugs/an-unlogged-waystone-page-can-lose-its-allocation-bit.md`.
2. **The copier cannot verify a page's CRC32C from the device alone**: a
   headerless page created after its map was read fails it, and an
   allocated, never-written page is zeros. **Applied**: §2's copy-time
   verification is dropped; a backup is verified by `kds_backup verify`
   against its manifest and by the restore's mount. The copier therefore
   touches no latch-guarded structure, as BR-R9 says.
3. **H1 named three readers of the mount anchor; there are more**
   (`SuperBlockCheckpointAnchor::MountAnchorOf`, whose warm-up republishes
   the mount anchor above one core, and the fixture path's
   `core_config.anchor`). **Applied**: `MarkRestoreOwed` also rewrites
   slot 0 to `{0, start, stop, segment(start)}`, so no reader of the image
   can see the copied anchor at all, and slot 0's durable point carries the
   stop into analysis's check whatever reads it.
4. **The pin and the read of `D` must be one act** - a publish between them
   would let the recycler and the reclaim pass the start. **Applied**:
   `BACKUP START` reads `D` and installs the pin under the backup control's
   mutex, and both the recycler's bound and the reclaim step's gate are
   computed under the same mutex.
5. **The stop must follow the copier's last read**, so WAL-before-data keeps
   every copied `page_lsn` at or below it; a page copied after the stop
   could hold a loser's change undo never sees. `[quiet-wrong]`.
   **Applied**: `BACKUP STOP` refuses while the copier runs and copies the
   data tail (finding 1) before it appends `BACKUP_END`. The map pages it
   copies after the stop carry no `page_lsn` redo or undo reads.
6. **H2 overclaimed**: the users file and the TLS key and certificate are
   durable state outside `data_file` and `wal_dir`. **Applied**: they are
   configuration, not data, and are excluded from a backup by name in the
   manifest's documentation (`manual/server/backup-restore.md`). The fence
   covers every data-file write (`device_page_store.cpp:442,552,563,2107`);
   `MemoryPageDevice` needs it too for the simulation harness.
7. **H3's free path confirmed** - one caller of `FreePage`; `ReleaseClaim`
   only before any record.
8. **Copy order across pages confirmed** - redo gates each page on its own
   LSN, `next_trx_id` is the maximum of the superblock's and the log's, and
   every core's latest `CHECKPOINT_BEGIN` lies at or above `D`.

**Amended at BR-S8** (the simulation's backup op): finding 1's map write
went to the source's device and so itself put bits ahead of unlogged pages'
bytes - BI-Q12's hazard, which a crash after a STOP turned into a page
allocated and never written on the *source*. STOP now copies the maps from
memory into the backup (`DevicePageStore::MapImages`) and writes nothing to
the device. The restore side gives back pages the copy holds as never
written and makes Waystone and `sys.access_stats` unreachable
(`docs/spec/backup.md` B6a).
