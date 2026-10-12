# Work order BR — backup and restore: a volume-snapshot path and a base-backup-plus-archive path, carried to semi-production

**Written 2026-10-12** in the main checkout on `main` at `dbff867b`
(`v2.7.0-907-gdbff867b`), on the operator's words W0-W2
(`raft-marks-2026-10-12.md` §14), and pushed to `main` as a document-only
change. **Opened 2026-10-12** on the operator's word (BR-Q0 (a), §10) on
`br-backup-restore`; BR-Q1..Q6 adopted on the operator's standing go-ahead
of 2026-10-12 (go-ahead-achieving-milestone), §10. BR-S0's census is
`br-s0-census.md`; where it restates a rule of §2, the census governs.

The operator's words, translated:

- **W0** (a question): *"ckdbs has no backup feature yet. What approach
  should we take?"* - answered in the session with five candidates: (A)
  offline file copy, (B) physical fuzzy base backup plus WAL archive, (C)
  MVCC-snapshot logical dump, (D) filesystem/volume snapshot, (E) WAL
  streaming standby.
- **W1**: *"First implement B and D as the minimum, and write the work order
  in the direction of developing them from there. When the milestone ends it
  must have the completeness of a semi-production feature."*
- **W2**: *"Push it, and check the milestone id for duplicates."* - BR was
  checked against every file name under `instructions/v3.0.0/`, every `BR`
  token in `*.md`, `*.cpp`, `*.hpp`, `*.h`, `*.py`, `*.sh` and `*.txt`, and
  every remote head, at `dbff867b`: no use found. The last letter assigned is
  BQ.

## 0. What BR is

### 0.1 Subject

ckdbs has no way to recover from losing its volume, from an operator's
mistaken write, or from corruption the log cannot heal. The only durability
path is the log plus mount recovery over the original files.

BR builds two backup paths and one restore path:

- **D - volume snapshot.** The engine marks a start and a stop; an external
  atomic snapshot (LVM, ZFS, btrfs, a cloud or VM disk snapshot) is the
  copy. Restore is mount recovery from the snapshot, bounded below by the
  start and above by a mandatory stop.
- **B - base backup plus WAL archive.** Sealed segments are archived
  continuously; the engine copies the data file while serving traffic.
  Restore is the copied data file plus archived segments replayed from the
  start to at least the stop, and optionally beyond it to a target (PITR).

**Both paths stand on one foundation: restore is mount recovery started
from a point the backup supplies, with an end it may not open short of.**
The stages build that foundation first, so B and D are thin layers over it.

### 0.2 Out of scope (successors, §7)

Logical dump (C), streaming standby (E), page-level incremental backup,
compression, encryption, native object-storage clients, parallel copy,
concurrent backups. Heap relations are under SUS-1 and get no work of their
own here; a heap page in the data file is copied and replayed like any page.

## 1. What the tree already gives - `[source-read]` at `dbff867b`

| # | Fact | Source |
|---|---|---|
| T1 | **One WAL stream per instance**; the LSN is one instance-wide monotonic byte offset, comparable everywhere. Per-core streams are refused at mount | `docs/spec/wal.md` §3 (lines 42-48); `CLAUDE.md` WAL row |
| T2 | Redo is idempotent: a record replays iff `record.lsn > page_lsn`; mount counts `redo_skipped_by_lsn` | `docs/spec/wal.md` §9 (line 137); `include/kds/server/mount_recovery.hpp:106` |
| T3 | A page carries a CRC32C that detects a torn page at load; an FPI in the log heals it; a checksum-failed page with no FPI by the end of the scan is a failure, not an answer | `docs/spec/wal.md` §10 (line 141); `src/wal/redo.cpp:24-27` |
| T4 | **The FPI cadence - the first modification of a page after each checkpoint logging an image - is not built.** Images are logged only by index builds, bulk fills and a Bound Cabin chain's old tail | `docs/spec/wal.md` §10 (line 141) |
| T5 | Recovery starts at the superblock anchor's redo start (slot 0 of the fold), runs once on core 0 before any peer exists, and ends with a completion checkpoint | `docs/spec/wal.md` §3, §12 |
| T6 | The log is fixed-size segment files, numbers never restart, the live run is contiguous `[first, end)`; a gap at or above the anchor's segment is `Corruption` | `docs/spec/wal.md` §4.1 (line 64) |
| T7 | Segments wholly below `D`, the durable anchor's redo start, are recycled; **archiving does not gate it (BC-Q1 (a)), and when archiving is built its floor joins the bound at the one place it is computed, `min(D, archived)`** | `docs/spec/wal.md` §11-4; `workorder-bc-wal-recycling.md:286`, `:380` |
| T8 | The archive seam is designed, not built: *"sealing a segment fires an archive hook"*; one file per segment so that archiving is a file copy | `docs/spec/wal.md` §13; `include/kds/wal/file_log_device.hpp:22-24`; `include/kds/wal/log_device.hpp:14` |
| T9 | Checkpoints are per core, one run at a time for the instance (`wal::CheckpointGate`), a `system` task since BI; the anchor reaches the device only after a sync of every page it stands for, and the fold never decreases | `docs/spec/wal.md` §11 |
| T10 | Durable state is **one data file** (`data_file`, the superblock is its page 0) and **one WAL directory** (`wal_dir`, default `<data_file>.wal`, separately configurable) | `kds.conf.sample:18-23` |
| T11 | The free map is not logged; recovery repairs it by raising the allocation floor | `docs/spec/page.md` §5 (line 130) |
| T12 | A dropped relation's pages are freed with no record, once the durable redo start has passed the drop (BF-R4) | `docs/spec/drop-table.md:33`; `CLAUDE.md` DROP TABLE row |
| T13 | A failed log write or sync fail-stops the log; a failed data-file sync stops the instance | `docs/spec/wal.md` §11-3; `CLAUDE.md` WAL row |
| T14 | The segment roll's latched hold is BQ's subject, measured there, not opened | `workorder-bq-wal-segment-roll.md` §0 |

What T1 settles: the consistency point is **one LSN**, not a per-stream
vector; Guideline 3 is satisfied by construction. What T4 settles: a fuzzy
copy today has **no heal for a page torn by the copy** - T3 makes that loud
(a refused restore), never quiet, but a backup that cannot restore is not a
backup. What T7 and T8 settle: BR does not invent the archive floor or the
seam; it builds what BC and `wal.md` §13 left a place for.

## 2. Rules BR proposes (`BR-Rn`)

**BR-R1 - Restore is mount recovery; there is no second redo.** A backup is
valid iff recovery started at its start LSN reaches its stop LSN. A
difference between restore and crash-recovery semantics is a defect in
recovery, fixed there.

**BR-R2 - A restore that has not reached the stop LSN does not open.**
`[quiet-wrong]` - this is the milestone's principal risk. The stop LSN is
persisted in the restored volume (superblock field or a sidecar the mount
reads before anything else; BR-S1 decides by a census) and checked before
the first session is admitted. No override in BR.

**BR-R3 - The start LSN is the durable anchor's redo start `D` at
`backup_start`, pinned against recycling until `backup_stop`.** `D` is
already a valid redo start (T7, T9: the anchor is published after a sync of
every page it stands for). A forced checkpoint is not needed for
correctness; it only shortens replay, and BR-Q3 decides whether to take one.
The pin joins the recycler's bound at the same one place as the archive
floor (T7): `min(D, archived, backup_pin)`.

**BR-R4 - The start comes from the backup, never from the copied
superblock.** In B the data file's page 0 is copied at some instant during
the copy, and its anchor can lie *above* the start, while pages copied
earlier are older than that anchor; recovering from the copied anchor would
skip records those pages need. `[quiet-wrong]`. Restore writes the backup's
start into the anchor before mount, or mount takes it from the label in
preference to slot 0.

**BR-R5 - Torn-page protection covers the backup window.** `[quiet-wrong]`
(corrected 2026-10-12, defect 1 of the review in §9). From `backup_start`
until `backup_stop`, the first modification of each headered page whose
`page_lsn` is below the **arm LSN** `A` logs a `FULL_PAGE_IMAGE` (T4's
cadence, armed only for the window). `A` is the log's end read after the
window is visible to every mutation, **not** the start LSN `D`: a page
modified between `D` and the arming already carries a `page_lsn` above `D`,
so a threshold of `D` would log no image for its next modification and a
copy torn by that modification's writeback would have nothing to heal it.
Two further conditions the threshold alone does not give, each verified by
BR-S0 (H4, H5) and closed in BR-S2: (i) no mutation that read the window
clear may take an LSN at or above `A` - the decision is re-checked where the
LSN is assigned, or the arming waits past every mutation that read it clear;
(ii) a page dirtied before `A` and written back during the copy with no later
modification gets no image, so either the copy begins only after every page
dirty at `A` is written (a checkpoint begun after the arming), or the copier
proves each page whole by its CRC32C and re-reads a torn one. Built so that
the general per-checkpoint cadence, if later ordered, is the same mechanism
with the threshold moved to each checkpoint's begin. BR-Q1.

**BR-R6 - Zero cost on the hot path when no backup runs and archiving is
off.** The permitted cost is one predictable branch on an instance word at
the FPI decision site. `cores = 1` stays byte-identical in what it writes
when no backup runs. Measured in BR-S10.

**BR-R7 - Every backup is self-describing and versioned.** A manifest
carries: format version; `git describe --tags` of the writing engine;
superblock version; page size; start LSN, stop LSN; the start and stop
segment numbers; each file with size and CRC32C; the backup id. The id is
also in the label, so a label and files from two backups are refused
(BR-S8 case 5). Restore refuses a manifest version it does not know.

**BR-R8 - A missing, short or corrupt segment stops restore by name.** The
error names the segment number and the LSN it was needed for. Nothing is
skipped.

**BR-R9 - The copier and the archiver are `system` tasks** (as the
checkpoint is since BI), one bounded step per poll, no latch across a park
(AR2), throttled by a `[provisional]` rate until BR-S10 measures it. The
archiver never runs under the stream latch: a seal only marks the segment
ready, so BR adds nothing to the hold BQ is measuring (T14).

**BR-R10 - A restored instance never writes into the original's archive
lineage.** `[quiet-wrong]` (corrected 2026-10-12, defect 2 of §9). **Every
restore that opens** starts a new lineage - one that opens at the stop LSN
as much as a PITR open: from the stop onward the restored instance writes
log the original also wrote past the same point, under the same segment
numbers. The lineage id is written to the superblock and carried in every
archived segment's name, so the restored instance's segments cannot
overwrite the original's.

## 3. Hypotheses BR-S0 verifies before code

Each is a prediction with the check that decides it; a false one changes the
stage it names.

| ID | Prediction | Check | If false |
|---|---|---|---|
| H1 | Mount can take its redo start from an external value without touching analysis, redo or undo | Read `mount_recovery.cpp`'s entry and `superblock_checkpoint_anchor.cpp` | BR-S1 adds the entry; BR-R4 becomes a mount change, not a pre-mount write |
| H2 | No durable state lives outside `data_file` and `wal_dir` (T10) | Census of every `open`/`creat` path in `src/` | Each extra file is enumerated in the manifest and its consistency rule written |
| H3 | BF's reclaim (T12) and the free-map repair (T11) are safe under redo from a start below the current `D` | Read BF-R4's gate; build a cell: drop, reclaim, reuse, backup started before the drop | **`[quiet-wrong]` candidate.** The reclaim's gate takes `min(D, backup_pin)` |
| H4 | A copied page torn mid-copy fails its CRC32C at restore and is healed by a BR-R5 image | Cell: copy a page while a writer holds it, restore | BR-R5's arming point moves earlier, or the copier reads under the page latch shared |
| H5 | The FPI decision site is one place per mutation path | Census of page-mutation log sites | BR-R5 is built per site, each with a cell |
| H6 | The seal point is observable on core 0's writer thread without extending the stream latch | Read `file_log_device.cpp`'s roll | The archiver is woken from the next poll instead |
| H7 | Recovery's completion checkpoint and recycling do not remove segments a still-running restore needs | Read BC's removal order at mount | Restore suppresses recycling until it reaches the stop LSN |
| H8 | Allocation, freeing and reuse of page ids inside the window are reproduced by redo from the start alone: a page created past the data file's length read at the copy's start is recreated by its logged creation record, and neither the unlogged free map (T11) nor BF's unrecorded free (T12) lets a restored instance hand out an id a replayed record placed (added 2026-10-12, defect 3 of §9) | Read the creation records' redo, the free map's repair at mount, and the in-memory free list's rebuild | **`[quiet-wrong]` candidate.** Restore rebuilds the allocation state from redo before the open |
| H9 | The development VM can take an atomic snapshot of the volumes holding `data_file` and `wal_dir` (added 2026-10-12, defect 5 of §9) | `sudo -n`, `lvcreate`, `btrfs`, `zfs`, the root and `/tmp` filesystems | BR-S3's drill takes its atomic copy by freezing the server process (`SIGSTOP`), copying both paths, and resuming it (`SIGCONT`): every write the kernel accepted is in the copy and none is half in it, which is the property a volume snapshot gives; the LVM script stays the reference, run where root exists |

## 4. Stages

A stage opens only when the one before it has closed (∥ marks stages that may
run side by side once their gate is met). A stage that lands carries
"overhead not measured; measured at the milestone's close" as a stated fact.

### BR-S0 - The census (no code)
`br-s0-census.md`: H1-H7 each verified or refuted with path:line and commit;
the persistence site for the stop LSN (BR-R2); the FPI decision sites (H5).
Every rule in §2 that changes is restated. **Exit:** census pushed; BR-Q1,
BR-Q2 answerable.

### BR-S1 - Recovery foundation
- Mount takes a start LSN and a stop LSN from a restore label (BR-R1, BR-R4).
- The stop LSN is persisted and the open refused short of it (BR-R2);
  `SHOW META` prints both and whether the stop was reached.
- Restore suppresses recycling until the stop LSN (H7).
- **Cells:** a volume whose stop is unreached refuses sessions; a start
  below the copied anchor replays what the copied anchor would have skipped;
  the existing recovery suite unchanged.

### BR-S2 - Backup control points
- `BACKUP START '<label>'` / `BACKUP STOP` (admin role, autocommit only;
  spelling decided at the stage against `parser-v2.md`, the debug port and
  KWP), and `kds_backup start|stop` - a new client tool: no admin command-line tool exists at `dbff867b` (`tools/` holds benchmarks, `scripts/` build and run scripts), and its name is fixed at this stage.
- Start: take `D` (BR-R3), set the pin and the FPI window (BR-R5), log a
  `BACKUP_BEGIN` record carrying the backup id and start LSN, return the
  label.
- Stop: log `BACKUP_END` with the id, make it durable, force a segment seal
  so the stop lies in a sealed segment, clear the window, release the pin
  only once the end segment is archived (when archiving is on), return the
  stop info.
- One backup at a time (BR-Q4); a second start is refused naming the open
  one. An abandoned backup (connection gone) is ended by
  `backup_abandon_ms` `[provisional]` or the next start; it has no
  `BACKUP_END`, so it is invalid by construction.
- New record kinds are numbered and the superblock version moves if the
  stop field lands there (BR-S0 decides).
- **Exit:** start/stop under load; the records present; R6's branch
  measured at `cores = 1`.

### BR-S3 - Path D: volume snapshot ∥ (after S2)
- Procedure: start → snapshot every volume holding `data_file` or `wal_dir`
  → stop → keep the label and stop info beside the snapshot (outside it).
- One volume holding both: the snapshot is crash-consistent on its own; the
  start/stop still bound the restore (BR-R2) and fill the manifest.
- Two volumes (CN-9's dedicated WAL device): valid only with start/stop, and
  only if the WAL volume's snapshot is taken after the data volume's or WAL
  covering the stop is archived. The order is the documented contract.
- `scripts/backup/snapshot-lvm.sh` as the reference; the engine carries no
  vendor logic.
- **Exit:** D's drills (BR-S8) pass on an atomic snapshot in the development
  VM. H9 decides the substrate: LVM where the VM grants root, else the
  process-freeze copy (`scripts/backup/snapshot-freeze.sh`), whose atomicity
  is the drill's premise and is stated as such in its report (corrected
  2026-10-12, defect 5 of §9: the VM's `sudo` needs a password, so LVM is not
  reachable from a session).

### BR-S4 - WAL archive ∥ (after S2)
- `archive_mode`, and one of `archive_dir` (built in: copy, `fsync` the file,
  `fsync` the directory) or `archive_command` (a shell hook whose exit 0
  means durable). One quantity, one name: no second spelling of either.
- A seal marks the segment ready; the archiver task (BR-R9) archives in
  segment order and records `done` durably, so a restart resumes and never
  re-archives a different body under one name.
- The archive floor joins the recycler's bound (T7, BR-R3).
- Archived names carry the lineage id (BR-R10).
- Failure: retry with backoff `[provisional]`; `wal_archive_failures`,
  `wal_archive_last_error`, `wal_archive_lag_segments`; the behaviour at the
  WAL volume's limit is BR-Q2.
- **Exit:** kill during archive loses nothing and duplicates nothing that
  differs.

### BR-S5 - Path B: base backup (after S2; S4 only for PITR past the stop)
- `kds_backup base --target DIR`: start → the copier streams `data_file`
  in chunks (BR-R9) → stop → copy the segments from the start segment to the
  stop segment into the backup, held by the pin until copied → write and
  `fsync` the manifest (BR-R7). A directory without a manifest is not a
  backup.
- **A base backup carries its own log** (BR-Q6, corrected 2026-10-12, defect
  4 of §9): the segments `[start, stop]` are in the backup directory, so a
  base backup restores to its stop with archiving off. The archive is what a
  restore past the stop (PITR, BR-S7) reads; it is not a precondition of B.
- The file grows during the copy: the copier copies to the length it read
  **at start** (corrected 2026-10-12, defect 3 of §9: the earlier text said
  "at stop", which contradicted its own next clause); a page past that
  length is created inside the window, and H8 decides whether its creation
  record recreates it with no help from the copy.
- **Exit:** a base backup under load completes and its manifest verifies.

### BR-S6 - Restore (after S1, and S3 or S5)
- `kds_backup restore --from BACKUP --to DIR [--archive SRC] [--target ...]`:
  check the manifest (BR-R7), refuse a non-empty target, lay out and verify
  the files, write the restore label, read segments from the backup's own
  log and then, past the stop, from the archive on
  demand, recover to the stop (and the target, BR-S7), open.
- A gap stops by name (BR-R8).
- **Exit:** B and D restore drills pass.

### BR-S7 - Point-in-time recovery (after S6; scope BR-Q5)
- Target by LSN, and by commit time (`NOW()`'s clock as recorded in the
  commit record, if it is recorded - BR-S0 checks).
- Recovery stops at the last commit at or below the target; losers past it
  are undone by the ordinary undo pass; the open starts a new lineage
  (BR-R10). A target below the stop LSN is refused.
- **Exit:** drop a table at T, restore to just before T: the table is there
  and nothing committed after T is.

### BR-S8 - Drills and fault injection (runs against S3, S5, S6, S7 as each lands)
- **Equivalence drill:** a workload runs; at a recorded LSN every btree
  relation is read under one snapshot and hashed; the workload continues; a
  backup is taken; restore to the recorded LSN; the hashes match. B and D.
- **Each fault ends in a correct instance or a named refusal, never an open
  instance with the wrong rows:**
  1. kill during the base copy;
  2. kill between `BACKUP_END` and the manifest;
  3. kill during archive, during restore's replay, during PITR;
  4. a missing, short or bit-flipped segment; a bit-flipped data-file page;
  5. a label from one backup with the files of another;
  6. a D snapshot taken with no stop;
  7. restore onto a non-empty directory;
  8. H3's drop-reclaim-reuse across the window.
- The simulation harness (`sim/`) gains a backup-and-restore op so the
  seeded crash corpus covers the window.
- `kds_backup verify BACKUP`: manifest, checksums, and the presence of
  every segment from start to stop in the archive, without restoring.
- The non-destructive subset runs with the suite; the full drill runs per
  tag.

### BR-S9 - Operation
- `kds_backup list`: id, path (B or D), start, stop, size, the segment
  range it needs, last verified.
- `kds_backup expire --keep N | --keep-since T`: removes backups and the
  archived segments no retained backup needs; never one a retained backup
  needs (cell).
- `SHOW META`: backup in progress, bytes copied, throttle, the archive
  counters of BR-S4, restore position.
- `manual/server/backup-restore.md`: both procedures, restore, PITR, and every
  BR-R8 / BR-S8 refusal with what the operator does about it.
- **Exit:** the runbook followed by itself, end to end, on a fresh VM.

### BR-S10 - Measurement (`build-release`, AS-E epoch, named by `git describe --tags`)
- M1: throughput and p99 with backup off; archive on; base backup running
  (FPI volume and copier interference); interleaved A/B, `cores = 1` and
  above.
- M2: archive throughput against peak WAL generation; the margin recorded.
- M3: restore time as copy time plus replay time per GiB of log.
- Every `[provisional]` constant (throttle, chunk size, retry backoff,
  `backup_abandon_ms`) gets a measured value or stays provisional with its
  reason.
- The milestone's overhead A/B, from the commit BR opened at to the one that
  closes it.

### BR-S11 - Close
`docs/spec/wal.md` §13 restated from `[PROPOSED]` to what is built; a
`docs/spec/backup.md` owning the subject; `CLAUDE.md` gains a Backup row;
`known-gaps.md` gains what §0.2 leaves out; the README's invariant list gains
a line only if every word of it is true at the tag.

## 5. Risks

| Risk | Kind | Guard |
|---|---|---|
| Open before the stop LSN | `[quiet-wrong]` | BR-R2, BR-S1 cell |
| Recovery from the copied anchor instead of the backup's start | `[quiet-wrong]` | BR-R4, BR-S1 cell |
| BF reclaim or free-map repair below the pin | `[quiet-wrong]` candidate | H3, BR-S8 case 8 |
| Label and files from different backups | `[quiet-wrong]` | backup id in both (BR-R7) |
| A restored instance archives over the original | `[quiet-wrong]` | lineage id (BR-R10) |
| Retention removes a needed segment | `[quiet-wrong]` | `expire` computes the union of needed ranges; cell |
| A torn copied page with no image | loud (T3) | BR-R5 |
| Archive falls behind | loud | M2; counters; BR-Q2 |
| WAL volume fills under archive failure | loud | BR-Q2 |
| Copier latency on the reactor | loud | BR-R9 throttle; M1 |

## 6. The semi-production bar - BR closes only when every line holds at one commit

1. B and D each back up under live load and each restores to an instance the
   equivalence drill passes.
2. Every BR-S8 case ends correct or refused by name; zero wrong opens.
3. PITR as BR-Q5 marks it.
4. `verify`, `list` and `expire` built, each with cells; `expire` never
   strands a retained backup.
5. Archive failure visible in `SHOW META`, and its end state the one BR-Q2
   marks.
6. The manifest is versioned and an unknown version is refused.
7. BR-R6 measured; the cost with a backup running measured and published.
8. The runbook followed by itself on a fresh VM.
9. Every constant measured or `[provisional]` with its reason.
10. The milestone's overhead A/B taken, or its absence stated.

Not required: compression, encryption, incremental, object storage, parallel
copy, concurrent backups, logical dump, standby.

## 7. Successors (opened by none of this)

Incremental base backup (page tracking or a log summary); compression and
encryption of backups and the archive; a native object-storage target; the
general FPI cadence of T4 (the same mechanism as BR-R5, armed per
checkpoint); logical dump (C); streaming standby (E) over BR-S4's sealed
segments, which `wal.md` §13 already names as the log-shipping source.

## 8. Questions for the operator (`BR-Qn`)

| ID | Question | Options | CLA proposes |
|---|---|---|---|
| BR-Q0 | Open BR? | (a) open now, all stages; (b) open BR-S0 alone now, the rest once the current open work closes; (c) not now | **(b)**: S0 writes no code and answers H3, the one candidate quiet-wrong; BL is paused and BQ is not opened, but adding a code stream beside them repeats the half-landed pattern |
| BR-Q1 | Torn-page protection for the window (T4) | (a) BR-R5's window-only FPI; (b) build the general per-checkpoint cadence now; (c) the copier reads each page under its latch shared and needs no image | **(a)**: (b) costs log volume always, against BR-R6; (c) covers B and not D, and leaves a torn page from the device itself unhealed |
| BR-Q2 | The archive keeps failing and the WAL volume nears its limit | (a) fail-closed: writes refused by name until archiving recovers; (b) fail-open: recycle past the archive floor, mark every backup since as broken, count it | **(a)**: (b) turns a loud failure into a backup that cannot restore, found when it is needed |
| BR-Q3 | Force a checkpoint at `backup_start`? | (a) no, start at `D` (BR-R3); (b) yes, for a shorter replay | **(a)**, with (b) as an option of the command once M3 shows replay time matters |
| BR-Q4 | Concurrent backups | (a) one at a time; (b) many | **(a)** in BR |
| BR-Q5 | PITR scope | (a) LSN and commit time; (b) LSN only; (c) none in BR | **(a)** if BR-S0 finds commit time in the commit record, else **(b)** |
| BR-Q6 | Does a base backup carry its own log `[start, stop]`? (added 2026-10-12, defect 4 of §9) | (a) yes, always: the segments are copied into the backup under the pin, and the archive is needed only past the stop; (b) only when archiving is off; (c) no: B is refused by name with archiving off | **(a)**: a base backup that restores only while a second system (the archive) is intact is two backups that must both survive; (b) makes the backup's contents depend on a setting; (c) leaves the archive-off instance with no path B |

## 9. Review of the order as written (2026-10-12)

Five defects found in the order as pushed at `fb953f07`, read in the main
checkout at `dc117a5d`, and corrected on `br-backup-restore` from
`fb953f07` before BR-S0 began, on the operator's word *"fix the five
defects in the order"*:

1. **BR-R5's image threshold** named the start LSN `D`. A page modified
   between `D` and the arming carries a `page_lsn` above `D`, so its next
   modification logs no image and a copy torn by that modification's
   writeback is unhealable. The threshold is the arm LSN `A`; the two
   conditions the threshold alone does not give (a mutation that read the
   window clear taking an LSN above `A`, and a page dirty before `A` written
   back during the copy) are stated in BR-R5 and decided by BR-S0.
   `[quiet-wrong]`.
2. **BR-R10** gave a new lineage only after a PITR open or a restore past the
   stop. A restore that opens at the stop writes log under the segment
   numbers the original also wrote past the same point. Every restore that
   opens starts a lineage. `[quiet-wrong]`.
3. **BR-S5** said the copier copies "to the length it read at stop" and then
   that pages past it are created in the window - which only reads with the
   length taken at start. Corrected, and H8 added: whether allocation, free
   and reuse inside the window are reproduced by redo alone, given the
   unlogged free map (T11) and BF's unrecorded free (T12).
4. **Path B depended on the archive** with nothing saying so: with archiving
   off a base backup could not restore. BR-Q6 added (a base backup carries
   its own `[start, stop]` log), and S5 no longer waits for S4.
5. **BR-S3's exit** required LVM in a VM whose `sudo` needs a password. H9
   added, and the exit names a process-freeze copy as the substrate where
   LVM cannot run.

## 10. Marks

BR-Q0 was answered by the operator's own word: *"fix the five defects in the
order and /go-ahead-achieving-milestone"* (2026-10-12), which opens BR whole
- option (a). Every other item below is **adopted on the operator's standing
go-ahead of 2026-10-12 (go-ahead-achieving-milestone)**, not marked by the
operator item by item; each is CLA's proposal, revised against the tree
where BR-S0 found its premise false (`br-s0-census.md`).

| ID | Mark | How |
|---|---|---|
| BR-Q0 | (a) open now, all stages | the operator's word |
| BR-Q1 | **(c), refined and revised from (a)**: the copier reads under a device-level write fence; no window images | adopted, revised: (a)'s premise - an image decided at one site and re-checkable where the LSN is assigned - is false (census H5), and redo's heal is refused for an allocated page (H4, `docs/inflight/bugs/a-full-page-image-cannot-heal-a-checksum-failed-allocated-page.md`) |
| BR-Q2 | (a) fail-closed | adopted as proposed |
| BR-Q3 | (a) no forced checkpoint; start at `D` | adopted as proposed; with BR-R5 replaced, nothing else needs one |
| BR-Q4 | (a) one backup at a time | adopted as proposed |
| BR-Q5 | **(b) LSN only** | adopted as proposed: the commit record carries no time (census) |
| BR-Q6 | (a) a base backup carries its own log | adopted as proposed, and extended to D (census §2: D's log comes from the backup) |
