# Work order ED - backup, a second pass: recovery to a time, an incremental backup, a backup that is compressed and encrypted, a snapshot's torn page healed

Status: **WRITTEN 2026-11-26, not opened.** Written on worktree
`known-gaps-milestone-orders` (branch
`worktree-known-gaps-milestone-orders`) from `8274c951`
(`v3.4.17-148-g8274c951e`) on the operator's word of 2026-11-26
(`raft-marks-2026-11-26.md` §3). No stage is started; ED-S0 waits for the
operator's word and for the marks on §4's items. A `[source-read]` claim
with a `path:line` was read at `8274c951`; a claim taken from
`known-gaps.md` carries that entry's own verification commit and is not
re-verified here.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `docs/spec/backup.md`; `manual/server/backup-restore.md`;
`docs/spec/wal.md` §10; `docs/inflight/known-gaps.md`, "Backup and
restore (BR)", "Power cuts (BS-S4)";
`instructions/v3.0.0/workorder-br-backup-restore.md` (BR §0.2, BR-Q5);
`instructions/v3.2.0/workorder-db-torn-page-healing.md` (DB-Q7 (a)).
**Two entries of the same section are DS's, open on its own branch
today, and not ED's**: the backup tool's authentication and a hung
`archive_command`.

## 0. What ED is

`[source-read]` BR built two backup paths over one restore, a WAL
archive and PITR to an LSN (`CLAUDE.md`, "Backup and restore"). It left
unbuilt, by its §0.2: incremental backup, compression and encryption, an
object-storage target, a parallel copy, concurrent backups, a logical
dump, a streaming standby (`known-gaps.md:1694`). `[design]` ED takes
the four an operator of a financial system meets first, each a stage
that lands whole, and leaves the rest named.

## 1. The tree as read

`[source-read]` from `docs/inflight/known-gaps.md` at `8274c951`:

- **PITR targets an LSN, not a time** (`:1711`): the commit record
  carries no time (BR-Q5 (b)); an operator notes `SHOW META`'s
  `wal_durable_lsn`.
- **A torn page in a path D copy is refused, not healed** (`:1697`,
  verified at `c802fac9`, DB-Q7 (a)): every data page's CRC32C is
  checked when the backup stops, at `verify` and at the restore's
  layout; since DB the restore's mount would heal one whose image lies
  in `[start, stop]` - "no cell shows it, and relaxing the three checks
  is a successor's".
- **A restore leaks** (`:1706`) - DV's.
- **The copier's rate, chunk, fail-closed reserve and idle hour stay
  `[provisional]`** (`:1739`), each measured at one value.
- **A backup starts a thread after startup, which `sched.md` §9's
  invariant 1 forbids and no document declares** (`:1721`).
- **Restore and PITR over a log of packed images are covered only
  implicitly** (`:167`).

`[source-read]` in the code at `8274c951`: `TXN_COMMIT` has no payload -
"the envelope's" fields are all it carries
(`include/kds/wal/payload.hpp:40`); the manifest is
`include/kds/server/backup_manifest.hpp`; the backup's finish runs on a
thread (`include/kds/server/backup.hpp:87`, `:124`).

## 2. The mechanism `[design]`

1. **Recovery to a time (S1).** Two forms, ED-Q1's: a wall-clock time in
   a record the log already writes at a cadence - the checkpoint's begin
   record - so a time resolves to the last such mark at or before it,
   with the cadence as its stated resolution; or a time in every commit
   record, exact, at a record-format change every commit pays. Either
   way the restore still stops at an LSN, and the time is a way to name
   one.
2. **A snapshot's torn page (S2).** The three checks pass a page whose
   CRC fails **only if** an image or a `PAGE_INIT` of that page lies in
   the backup's own log `[start, stop]`; the manifest records each page
   so passed, and the restore's mount must heal every one or refuse.
3. **Incremental backup (S3).** A backup that names a parent copies the
   pages whose `page_lsn` is above the parent's start, every page of a
   class that carries no LSN or is unlogged, and its own log. A restore
   lays the chain down oldest first and then recovers as today, from the
   last member's start. The free map is not trusted across members: it
   is unlogged, and the restore's recovery repairs it.
4. **Compression and encryption (S4).** Applied by the copier to the
   stream it writes and by the archiver to a segment, recorded in the
   manifest; the checksums the restore verifies are over the plaintext.
   Which algorithm, which library and where a key comes from are the
   operator's (ED-Q4, ED-Q5): CLA proposes a toolchain and never decides
   one.

## 3. Rules

- **ED-R1. A restore opens only on a consistent image at its stop**
  `[quiet-wrong]`: restore stays mount recovery from the backup's start,
  never the copied anchor, and does not open short of the stop. An
  incremental chain with a missing, reordered or foreign member is
  refused by the manifest, never laid down.
- **ED-R2. A page passed with a failed CRC is healed or the restore
  refuses** `[quiet-wrong]`; no torn page is served.
- **ED-R3. An incremental backup misses no changed page** `[quiet-wrong]`:
  ED-S0 lists every page class and, for each, why `page_lsn` above the
  parent's start is a complete test or why the class is copied whole. A
  class it cannot argue is copied whole.
- **ED-R4. A recovery to a time never opens past the time asked**: the
  LSN it resolves to is at or before the last commit known to precede
  it, and the reply names the LSN and the resolution.
- **ED-R5. An encrypted backup's key never appears in an argv, a log
  line, an error or the manifest.**
- **ED-R6. A server that takes no backup pays nothing**; S1's record
  change, if ED-Q1 marks the exact form, is measured on a one-row
  commit.
- **ED-R7. A backup BR's build wrote restores under ED's build.**
- The standing rules, as DT's order lists them.

## 4. Items for the operator (ED-Q), CLA's proposals

| | item | options | CLA proposes |
|---|---|---|---|
| ED-Q0 | Is ED opened, and with what scope? | (a) S1-S4; (b) S1 and S2 only; (c) one of them | (a), in the order S2, S1, S3, S4 if a cut is wanted: the cheapest first |
| ED-Q1 | A time's form (BR-Q5 was marked (b), LSN only) | (a) a time in the checkpoint's begin record, resolution the cadence; (b) a time in every commit record; (c) stays LSN only | (a): no record every commit pays for, and a wall clock is not commit order, so an exact time is a weaker promise than it reads |
| ED-Q2 | Does ED-S0 gate S3? | (a) S0 is ED-R3's census; a class with no argument is copied whole, and if more than half a typical volume is such classes S3 is cut. (b) built regardless | (a) |
| ED-Q3 | An incremental chain's depth | (a) bounded, a stated maximum, each member naming its parent's identity; (b) unbounded | (a) |
| ED-Q4 | Compression | (a) a library the operator names, behind one interface; (b) none in ED - a pipe through the operator's own tool, documented; (c) CLA proposes zstd | (b) first, (c) if the operator wants it in the server |
| ED-Q5 | Encryption and its key | (a) AES-256-GCM through the OpenSSL the server already links for TLS, the key read from a file a boot key names; (b) a pipe through the operator's tool, as ED-Q4 (b); (c) none in ED | (b) with ED-Q4 (b), else (a) |
| ED-Q6 | The backup's thread started after startup (`:1721`) | (a) `sched.md` §9 declares it as an exception; (b) the thread is started with the instance and parked | (b) |
| ED-Q7 | The `[provisional]` constants | (a) each measured at three values in the close; (b) left | (a) |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **ED-S0** | ED-R3's census of page classes; the three CRC checks located; what a checkpoint's begin record can carry without a version move; a restore's time measured against the log length it replays | the census in §7; a results file; ED-Q2 read |
| **ED-S1** | Recovery to a time, as ED-Q1 marks | cells: a time between two marks; before the backup's stop (refused); the reply's LSN; the sim's backup cell with a time target |
| **ED-S2** | A snapshot's torn page healed | cells: a torn page with an image in range heals; one without is refused at the stop, at `verify` and at the layout; `io_sweep.py`'s tears over a path D copy |
| **ED-S3** | Incremental backup | cells: a chain of three restores to the same bytes as a full backup at the same stop; a missing member; a page of each class changed between members; PITR over a chain |
| **ED-S4** | Compression and encryption, as marked | cells: a flipped byte refused; a wrong key refused before any page is laid down; ED-R5 |
| **ED-S5** | The close: ED-R6's A/B, the constants (ED-Q7), `backup.md`, the manual, `known-gaps.md` | the operator's word on landing |

## 6. What ED does not do

- No streaming standby (CM is on a branch with no order on `main`), no
  logical dump, no object-storage target, no parallel copy, no
  concurrent backups.
- No change to path B's fence or to what a backup pins.
- Nothing of DS's two entries.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-26

On `known-gaps-milestone-orders` from `8274c951`, on the operator's word.
No stage is opened and no code is written. **Not reviewed**: the
`critics-developer` review was started and stopped before it reported,
so no line reference or reading in this order has a second reader behind
it.
