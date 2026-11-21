# Work order DG - a WAL segment's body: what the zero fill costs the sessions beside it, and what its absence costs a commit, measured before anything is built

Status: **OPENED 2026-11-21 on the operator's standing go-ahead
(go-ahead-achieving-milestone), on worktree `dg-wal-segment-body` from
`46ee8a6e` (`raft-marks-2026-11-21.md` §3). DG-Q0..Q10 are adopted (a) as CLA
proposed, and none is the operator's own mark. Under DG-Q1 (a) and DG-Q2
(a), the order stops at V0.** Written 2026-11-18. Written on worktree
`cz-checkpoint-roll-wait` at `860d36f9` (`v3.4.12-72-g860d36f9`) on the operator's word
of 2026-11-18 (`raft-marks-2026-11-18.md` §42). The worktree is CZ's: the
order rests on results files that are on that branch and not yet on
`main`. The letter DG is CLA's proposal. **Exploratory by CLA's proposal,
not by the operator's word** (DG-Q0).

**This is a wider order than that word covers.** The word asked for an
order for C2 - no zero prewrite - if CZ-S1b's numbers supported it. They
do not support C2 as it stands (§1.1); they do show that the fill has a
cost of its own. CLA wrote the order on the fill and put "no order" to
the operator as DG-Q0 (c).

No stage is started. DG waits for the operator's word and for the marks on
§4's items. Every `[source-read]` claim below was read at `860d36f9`,
whose source is `origin/main`'s at `57d7c67f` for every file cited. No
suite was run: the order changes no build input.

Tags: `[source-read]` read in the tree, with `path:line`; `[measured]`
with the results file; `[design]` CLA's, unbuilt and unmeasured;
`[quiet-wrong]` where a wrong choice turns a correct answer into a wrong
one with no error.

Related:
- `docs/spec/wal.md` §4.1 (a segment's body, built ahead), §4.2 (the torn
  tail), §6 (the roll under the latch);
- `instructions/v3.0.0/workorder-bq-wal-segment-roll.md` (H4, its three
  options; BQ-S2's verdict on option 3);
- `instructions/v3.2.0/workorder-cz-checkpoint-roll-wait.md` (C2; CZ-S1,
  CZ-S1b);
- `bench/v3.4.12/results-cz-s1-ceiling-v3.4.12-66-g20d89234.md`,
  `bench/v3.4.12/results-cz-s1b-c2-priced-v3.4.12-66-g20d89234.md`;
- `docs/inflight/known-gaps.md` (the WAL roll entry, which names H4 option
  3 as an open lead).

## 0. What DG is

`[design]` DG asks what a new segment's 64 MiB body should be made of,
for the engine as a whole: the zero fill as it is, the fill paced, no
fill, or a body whose extents were written some other way. BQ asked part
of it once, as H4 option 3, against a bar on the reader's p99, and left
its guard unmeasured. CZ-S1 and CZ-S1b measured the fill and its absence
for every session, and the guard with them.

## 1. The tree as read

### 1.1 What is measured `[measured]`

All on one host (8 vCPU, ext4 on NVMe), 10 s windows, medians of 5 runs,
on CZ-S1's measurement-only spike (`KDS_CZ_SPIKE=c2`: no zero prewrite
where `posix_fallocate` reserved; the body's `fsync` kept). Builds were
on the host for 6 of CZ-S1's 25 cells and 12 of CZ-S1b's 55.

**With the fill** (`results-cz-s1b-...md` §5). The half mark is the
half-segment point, where the next body is asked for. A long statement is
a bulk statement of 30 ms or more.

| cell | half mark to the roll | segments with a long statement after the mark | the first begins after the mark | the last ends after the mark | roll hold median | sync gate's share of the holds |
|---|---|---|---|---|---|---|
| bulk writer closed-loop at `group`, 56 MB/s | 774 ms | 38 of 41 | 45 ms | 417 ms | 3.1 ms | 0 % |
| C1; inserter held to 120 statements/s, `relaxed`, 57 MB/s | 577 ms | 32 of 40 | 342 ms | 413 ms | 4.1 ms | 60 % |
| C1; inserter held to 150 statements/s, `relaxed`, 71 MB/s | 512 ms | 55 of 55 | 378 ms | 527 ms | 39.6 ms | 93 % |
| C1; bulk writer closed-loop at `relaxed`, 74 MB/s (CZ-S1's `c1`) | 304 ms | 55 of 56 | 300 ms | 553 ms | 251.4 ms | 99 % |
| the first and third without the fill | 482 ms, 462 ms | 0 of 50, 0 of 55 | - | - | 4.7 ms, 5.3 ms | 0 %, 1 % |

- With the fill, nearly every segment has long statements in the 0.4 to
  0.55 s after the half mark; without it, none has. At `group` they fill
  about 370 ms of it. That the fill is the cause is inferred from the
  place: the preparer is not timed.
- A roll that arrives inside that time waits at the sync gate for what is
  left of it (553 ms less 304 ms, against a hold of 251 ms). The body was
  ready at every roll but 17 of CZ-S1's 56.

**Without the fill** (`results-cz-s1-...md` §4, `results-cz-s1b-...md`
§1-§4):

| cell | reading | with the fill | without it |
|---|---|---|---|
| `group`, 4 one-row writers at 800/s, a sync a commit | commit p50 from its schedule | 799 µs | 809 µs (+1.3 %) |
| the same | commit service p95 | 985 µs | 1.6 ms (+65 %) |
| `strict`, the same | commit p50 from its schedule | 792 µs | 813 µs (+2.6 %) |
| the same | commit service p95 | 945 µs | 1.6 ms (+71 %) |
| `group`, the bulk writer closed-loop | rows a second | 121,469 rows/s | 146,073 rows/s (+20 %) |
| the same | statement p50 / p99 / longest | 5.6 ms / 66.8 ms / 289 ms | 6.7 ms / 11.1 ms / 21 ms |
| the same | walker statements a window | 11,404 statements | 9,543 statements (-16 %) |
| `relaxed`, the bulk writer closed-loop | rows a second | 158,412 rows/s | 242,968 rows/s (+53 %) |
| the same | the inserter's longest statement | 639 ms | 17 ms |
| the same | walker statements a window / its p99 | 1,051 / 234 ms | 1,581 / 16 ms |
| the same | reader samples at or above 10 ms, 5 runs | 174 samples | 46 samples (45 in one cell a build reached) |
| `relaxed`, C1 in both arms, the inserter held to 150 statements/s | reader p99 | 1.7 ms | 1.3 ms |

- **The commit's price is at its p95**: up 0.6-0.7 ms, no run of one arm
  inside the other's range in any of the 20 commit cells, and 5-8 % more
  commits 1.5 ms late. The service p99 over the runs no build reached is
  2.3 ms against 1.5 ms at `group` and 2.0 ms against 1.4 ms at `strict`.
  - Read as about 1 ms on a sync that meets a never-written block, which
    one commit in sixteen of this size (248 bytes of log) is (inferred).
    It agrees with the device header's own figure (§1.2). A commit that
    logs a block or more would meet it every time: the bulk writer's
    statement pays 1.1 ms at its median.
- **The roll pays it too, under the stream latch**: its header's sync
  takes 1.25 ms against 0.45 ms, and the short roll's hold 4.7 ms against
  3.1 ms.
- **The commit cells priced the absence alone.** They never reached half
  a segment, so the preparer did not run; the base arm's one segment was
  filled at the server's start (`src/wal/stream.cpp:76-78`). `group` and
  `strict` were one regime there, a sync a commit.
- **Long stalls without the fill, too few to read.** Three commit runs
  have a writer stall of 185 ms or more, two of them without the fill;
  each ran beside or just behind another session's build.
- **A tail of the roll stays without the fill**: 6 holds of 80 ms or more
  in about 310 rolls (619, 609, 531, 447, 119 and 84 ms), five of them in
  cells a build reached. Five are 94 % or more at the sync gate; the
  447 ms one is 310 ms at the gate and 134 ms in the roll's own sync.
  CZ-S1b's results file counts 5 and omits the 119 ms one.

**CLA's judgement, which §42 left to it.** The numbers do not support C2
as it stands: it moves a cost of about a millisecond onto one small
commit in sixteen and onto every large one, in an engine whose purpose is
OLTP. They do support asking the question. The fill costs a bulk writer
about a third of a second a segment at `group`, and at a high log rate it
stops every appender at the roll. Whether a body can be had that owes
neither cost is not measured (§2).

**What BQ measured at `4cf30270`**, on another host
(`workorder-bq-wal-segment-roll.md` §7, "BQ-S2"): without the fill the
hold's median from about 325 ms to 16 ms, its bulk cell up 43 %, the
pooled hold p99 not down (809 ms to 1.14 s, inside the spread), 13 % of
rolls carrying 80 % of the hold. Its guard, a `group` commit's p99, "was
not evaluated". BQ bounded out asking for the body earlier and keeping
two ahead (its H4 options 1 and 2).

**Not measured, and it is the premise:**

- a body with written extents and no fill in the window;
- the fill paced;
- the commit's price for a commit of a block or more, under group commit
  proper, with the log rolling, and as a mean;
- what the disturbance is: the preparer's write and `fsync` are not
  timed;
- what the roll's remaining tail is. One hypothesis: a sync that converts
  an extent needs a filesystem journal commit, which ties the log's sync
  to everything else writing to that filesystem;
- another filesystem, windows longer than 10 s, a host no other session
  is building on.

### 1.2 What the tree does `[source-read]` at `860d36f9`

- **A body is reserved, zero-filled and synced.** `BuildBody`
  (`src/wal/file_log_device.cpp:368-385`) calls `Reserve` (`:97-107`:
  `posix_fallocate`, or `ftruncate` where the filesystem refuses it) and
  then `Prewrite` (`:135-163`): 64 writes of 1 MiB of zeroes and an
  `fsync`.
  - The reason is `Prewrite`'s comment (`:129-134`): `posix_fallocate`
    hands back unwritten extents, and the first write into each costs an
    extent-conversion journal transaction inside the `fsync` a commit is
    waiting on.
  - The `fsync` is also what makes the file's size and extents durable;
    `WriteHeaderAndSync`'s `fdatasync` relies on it (`:109-127`).
  - On the `ftruncate` path the file is sparse, and the fill is what keeps
    an accepted append from failing for space (`:94-96`;
    `include/kds/wal/file_log_device.hpp:27-29`).
- **It is usually built ahead.** Past half a segment an append asks the
  device for the next body (`src/wal/stream.cpp:454-463`), and the
  device's own thread builds it under `prepare_mutex_`
  (`file_log_device.cpp:387-403`). The roll's creation takes the body if
  it is ready, waits on that mutex if it is being built, and builds it
  itself otherwise (`:411-463`); the roll line names which (`body=hit`,
  `waited`, `inline`).
- **A roll waits out a sync in flight, under the stream latch.**
  `StartSegment` syncs the segment it leaves through `SyncDevice`
  (`src/wal/stream.cpp:154-166`, `:113-146`), which first takes the sync
  gate the writer thread's sync holds. In CZ-S1's `levels` 98.6 % of the
  roll's hold was that wait.
- **The segment's tail is read as zeroes.** A mount scans the last
  segment and, where it is unsealed, zeroes what lies more than a record
  header past its end: tested a 4 KiB block at a time, the blocks from
  the first that holds a non-zero byte to the last are written, and a
  clean tail is no write (`ScrubTail`, `src/wal/stream.cpp:219-269`;
  `wal.md` §4.2). A seal that leaves a tail shorter than a record header
  writes no marker and leaves the zeroes the segment was created with
  (`stream.cpp:378-381`). A scan tells that seal from a tear by the
  tail's length where a later segment exists
  (`src/wal/log_scanner.cpp:175-177`), and by the reader's rule where
  none does: a header's worth of zeroes is a clean end, and anything else
  that does not decode is a torn tail (`src/wal/record.cpp:175-207`),
  which a mount reports and a restore refuses
  (`src/server/mount_recovery.cpp:40`, `src/server/restore.cpp:516-529`).
- **A recycled segment is removed.** Segments below the durable redo
  start are detached and then unlinked by the writer thread
  (`ReclaimDetached`, `file_log_device.cpp:656-705`), unless an archive
  still holds them. Nothing reuses one.
- **The segment size is a property of the volume** and no setting governs
  a segment's creation (`wal.md` §4.1).
- **The device's own header prices the conversion**: "measured ~950us
  against ~2,100us per flush" (`include/kds/wal/file_log_device.hpp:31-32`),
  and names a rename as a way to recycle (`:25`).
- **The device keeps one body ahead.** `BuildAhead` builds only the
  segment at the device's end (`file_log_device.cpp:397`) and holds
  `prepare_mutex_` across the whole build.

## 2. The candidates `[design]`

Priced by DG-S0; none is chosen here.

- **P0 - nothing.** The fill stays as it is. C1 mends the reader in CZ; a
  bulk writer keeps its long statements behind every half mark, and every
  appender stops at a roll that arrives among them.
- **P1 - no zero fill where `posix_fallocate` reserved** (BQ's H4 option
  3, CZ's C2). The reservation and the body's `fsync` stay; the fill
  stays on the `ftruncate` path. It is the candidate that is measured.
  - **Its costs**: the conversion on a commit's sync and on the roll's
    header sync under the latch (§1.1), and, if the hypothesis above
    holds, the log's sync tied to the filesystem's journal.
  - **It is sound against the code as read**: the kept `fsync` makes the
    size and the reserved extents durable; an unwritten extent reads as
    zeroes to the scan, the scrub and the short-tail seal; a crash
    between the reservation and the first write leaves a temporary file;
    a backup, an archive and a restore copy by reading, so their copies
    are dense. DG-S0's census reads each again.
  - `posix_fallocate` succeeding is not a promise of space on a
    copy-on-write filesystem, with the fill or without (DG-R1).
- **P2 - the fill paced** `[quiet-wrong]`: the 64 MiB written a piece at
  a time across the segment's life, so that no third of a second belongs
  to it. It puts no conversion on a commit's sync. Not measured anywhere.
  - A zero write that lands after the body is adopted erases appended
    records. Today the preparer holds `prepare_mutex_` across the whole
    build, and a roll that comes early waits for it under the latch.
  - **The fill moved** - asked for earlier, or two bodies ahead - is not a
    candidate. BQ bounded it out; the long statements stay wherever the
    fill runs; and the device keeps one body ahead, so the second is not
    a switch.
- **P3 - a body that was already written**: a recycled segment renamed
  into place `[quiet-wrong]`. Its extents are written, so it needs
  neither the fill nor a conversion.
  - **What holds already**: an old record cannot validate at its position
    in the new segment. A record's LSN must equal its position and the
    CRC covers the LSN (`src/wal/record.cpp:209-215`), and segment
    numbers never restart. The invariant P3 owes is that a reused body
    comes only from a lower segment of the same lineage.
  - **What breaks**: the tail is no longer zeroes. Every clean end reads
    as a torn tail, so a mount's `recovery_torn_tail` is wrong, and a
    restore refuses a sound archive whose last segment was sealed with a
    short tail (`src/server/restore.cpp:522`). A mount's scrub goes from
    a test in memory to a write of up to 64 MiB. That is a format event
    by `wal.md` §4.1's own rule unless the census shows otherwise
    (DG-R4).
  - **What it costs**: an append is an unaligned buffered write, so a
    partial block written into a recycled page that has left the cache is
    a read first, under the latch.
  - **Supply**: a segment detaches only when the redo start, a backup's
    pin and the archive's floor have passed it. Under a bulk load the log
    recycles late (BQ saw 3 of 15 base runs recycle), so the fallback may
    carry the load exactly where the fill hurts.
- **P4 - bodies filled when the log is quiet**: a few kept ahead, built
  by today's fill at times no commit is waiting. Written extents and none
  of P3's hazards. It needs a rule for how many and for what "quiet" is,
  and a bulk load long enough outruns it.
- **P5 - a reservation that writes zeroes without data I/O**
  (`FALLOC_FL_WRITE_ZEROES`). Written extents from the filesystem, where
  the kernel and the device offer it. Not available on this host's
  kernel (6.12); which kernels and devices offer it is for DG-S0 to
  establish, not this order.

## 3. Rules

Binding from DG-S1 on, if the order reaches it.

- **DG-R1 - an accepted append never fails for space.** The full-size
  reservation stays in every candidate, and the fill stays wherever the
  reservation is not a promise.
- **DG-R2 `[quiet-wrong]` - nothing past a segment's end is ever accepted
  as a record, and a clean end is told from a torn one.** Today both rest
  on the tail reading as zeroes (`wal.md` §4.2, `record.cpp:175-207`). A
  candidate that keeps zeroes says who guarantees them; one that does not
  (P3) owes another mechanism for both halves, and every reader of a tail
  is read against it: the scan, the scrub, the short-tail seal, a
  backup's copy, an archive, a restore.
- **DG-R3 - the body's metadata is durable before its header.** The
  `fsync` that follows the reservation stays.
- **DG-R4 - nothing on disk moves without the operator's word.** A
  candidate that changes what a build without it reads from a volume or
  an archive is a format event, and DG-S0 says of each whether it is.
- **DG-R5 - the roll's order is untouched**: BQ-R1's three clauses. Its
  cost is not: P1 lengthens the header's sync under the latch.

## 4. Items for the operator (DG-Q), CLA's proposals

(a) is CLA's proposal in each. **Each one is adopted (a) on the operator's
standing go-ahead of 2026-11-21 (go-ahead-achieving-milestone)**
(`raft-marks-2026-11-21.md` §3). None is the operator's own per-item mark.
DG-Q5 (a) is revised against the tree: C1 is on `main` (CZ-S3), so both arms
carry it as it was built, with no switch.

| | item | (a) | other |
|---|---|---|---|
| DG-Q0 | Is there an order, and is it exploratory? §42's condition - that the numbers support C2 - is not met as it was put. | yes, on the fill, and DG-S0 may close it with nothing built | (b) build P1 on CZ-S1b's numbers; (c) no order: the finding goes into CZ's order and `known-gaps.md`'s roll entry, and this file is deleted |
| DG-Q1 | The bar. The trade is between kinds of session - a small commit's p95 against a bulk writer's rate and tail - and CLA has no ground to weigh one against the other. | none before DG-S0. DG-S0 ends in one table - each candidate's commit p50 and p95 at two commit sizes, the bulk writer's rate and longest run of long statements, the roll's hold - and **V0 is the operator's reading of it**. V1 is set then | (b) a commit clause now: p50 within 3 % and p95 within 10 % of written extents with no fill (M2), which P1 misses on CZ-S1b's numbers; (c) a bulk clause now |
| DG-Q2 | Which candidate is built? | the one the operator names at V0, chosen on paper in DG-S1. P2 and P3 are `[quiet-wrong]` and each needs its own mark | (b) P1 now, the commit's p95 accepted; (c) P4 now, as the one with neither P1's price nor P3's hazards, unmeasured |
| DG-Q3 | A setting? PostgreSQL has two (`wal_init_zero`, `wal_recycle`). | none: one behaviour, chosen by what the filesystem promises | (b) a boot key, for a volume whose commits are small and many against one that loads in bulk |
| DG-Q4 | The filesystems DG-S0 measures. | ext4 alone, stated. Another needs a block device of its own: a loop file over ext4 does not measure it | (b) the operator provides a device and names the filesystem |
| DG-Q5 | DG and CZ. | independent orders. DG-S0's measurement build carries CZ-S1's C1 switch, on in both arms, so that a reader's p99 is not CZ's stalls; DG's close measures with C1 if CZ-S3 has landed | (b) DG waits for CZ's close |
| DG-Q6 | The roll's tail that remains without the fill (6 holds of 80 ms or more in about 310 rolls). | in DG-S0 as a reading, from the roll line's own fields in every cell, against the hypothesis of §1.1; six events may not settle it, and the results file says so | (b) out of DG |
| DG-Q7 | The measurement budget. DG-S0's cells as listed are about 100, some 34 minutes of running. | the standing 20 minutes, cut in this order and the cut stated: M4 on every arm, M2, M1, M3 | (b) lifted for DG-S0 to an hour, and windows longer than 10 s with it |
| DG-Q8 | The driver. M1 needs an inserter held to a rate, which is on CZ's spike branch only (`--insert-rate`). | it lands in `tools/bl_mixed.py` in DG-S0, with its own review; a commit of a block or more is that inserter with a small batch, no third change | (b) it stays a spike's |
| DG-Q9 | The host. CZ-S1 and CZ-S1b ran beside other sessions' builds, and the long stalls cannot be read for it. | the shared host and the quiet gate, as now, with the count of cells a build reached stated | (b) the operator gives DG-S0 a quiet host or a quiet hour |
| DG-Q10 | The letter. | DG | (b) another |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DG-S0 - premise and census** | No engine code to land but the driver's flag (DG-Q8). **Measurement** on a measurement-only build (CZ-S1's switches re-cut from `main`, with one each for M2 and M3): M1-M4 below. **Census**: every reader of a segment's tail against DG-R2; what a backup, an archive and a restore do with a segment that was never filled; P3's and P2's hazards and P5's availability, by reading; for each candidate, whether it is a format event (DG-R4). | **V0**: the operator's reading (DG-Q1) |
| **DG-S1 - the choice, on paper** | The named candidate against the code: where the fill stays, the durable order of a creation, the tests owed, and every text that becomes false (`wal.md` §4.1; `file_log_device.hpp:27-33`; `file_log_device.cpp:109-111`, `:153-155`, `:447-451`, `:614-618`). | items back to the operator before any code |
| **DG-S2 - the build** | With its review and the full suite. | a branch at the push gate |
| **DG-S3 - the close** | The interleaved A/B over the whole change; V1; `known-gaps.md`'s roll entry and BQ's record amended; the row flipped. | closed, or closed narrow |

**DG-S0's cells.** Each interleaved with the fill as it is, 5 runs; each
read for the commit's p50 and p95, the bulk writer's rate and long
statements, the roll's hold and the reader's p99.

- **M1 - the commit under a log that rolls**, with and without the fill:
  one-row writers at `group` and at `strict` beside an inserter held to a
  rate; the same with a commit of a block or more; and writers enough
  that `group` groups.
- **M2 - written extents, no fill in the window.** Bodies filled before
  the window opens and none built in it, warm, and again with the bodies
  dropped from the page cache. It is the ceiling for P3, P4 and P5 at the
  device, not P3's price: it has a full supply, no scrub and no reuse.
  The base arm's segments removed a roll are reported as P3's supply.
- **M3 - the fill paced** (P2), in the bulk cells at `relaxed` and at
  `group`.
- **M4 - the fill, timed.** The preparer's write and `fsync` stamped in
  the log, on every arm: whether the long statements and the roll's wait
  are the fill's.

## 6. What DG does not do

- No change to the segment size, the roll's order or the stream latch.
- Nothing for the reader's wait on a roll: that is CZ's C1.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-18

A document only; nothing was measured for it and no suite was run.

**Reviewed by `critics-developer`** against the two results files and
the source, before the commit.
- **Corrected by the review:** the count of long holds without the fill
  (6, not 5, and not all at the gate); four citations; what the mount's
  scrub and the short-tail seal do, with the reader's rule and the
  restore's refusal the first draft did not name; that the commit cells'
  base segment was filled at the server's start.
- **Applied from its findings:**
  - P3's hazards were the wrong ones. The one named is already closed by
    the LSN-equals-position rule; the real ones are the clean end read as
    torn, a restore refusing a sound archive, the scrub's write, a read
    before a partial write, and supply;
  - DG-R2 stated the mechanism and so forbade P3 as written; it now
    states the invariant;
  - P1's cost was understated: the roll's header sync under the latch,
    and the journal hypothesis for the tail;
  - "P2 costs a commit nothing" was false of a bulk statement at `group`;
    the fill moved is no longer a candidate; the fill paced is
    `[quiet-wrong]`;
  - two candidates were missing (P4, P5), and M2 is named a ceiling;
  - the first draft's bar was drawn after its numbers and passed only
    candidates with no evidence; DG-Q1 now proposes none before DG-S0;
  - the order is wider than §42's word, and DG-Q0 (c) is "no order";
  - the driver, the host and the budget's cut are items; the title, §0
    and §6 were cut.
- **Rejected:** none.

### Opened 2026-11-21

DG was opened on the operator's standing go-ahead
(go-ahead-achieving-milestone, `raft-marks-2026-11-21.md` §3), on worktree
`dg-wal-segment-body` from `46ee8a6e`. DG-Q0..Q10 are adopted (a) and none
is the operator's own mark.

DG-Q5 (a) was revised against the tree. CZ's C1 is on `main` (CZ-S3), so
DG-S0's build carries it as built and has no switch for it.

Under DG-Q1 (a) and DG-Q2 (a), the order stops at V0.

### DG-S0 - premise and census (2026-11-21)

**The driver (DG-Q8).** `--insert-rate` landed in `tools/bl_mixed.py` at
`e9952d64`.
- Reviewed by `critics-developer`: no bug.
- Applied: `insert_rate` is recorded only where the arm has an inserter,
  and the interval is computed once.
- Rejected: putting the rate in the run tag. `--batch` already leaves the
  tag, and DG-S0 writes each cell to a directory of its own.

**The measurement.**
`bench/v3.5.0/results-dg-s0-premise-v3.5.0-36-ge9952d64.md`, on a spike
that is not landed (`archive/dg-s0-premise-v3.5.0-36-ge9952d64/dg-s0-spike.patch`; the source
was reverted after the build). There are 54 cells, 18 shapes × 3 runs, on
ext4.

**Over budget.** The run took 23.9 minutes against the standing 20, and
28.2 from the build's start to the last cell's end. The cut was DG-Q7 (a)'s, made
before the run, and is stated in the results file's §0.

Its §1 is V0's table. In short:
- **P0 (the fill as it is).**
  - Under a log that rolls at 24 MB/s, a one-row commit's p95 is 171 ms
    at `group` and 174 ms at `strict`, against a p50 of 976 µs and
    936 µs.
  - 79 % of the writers' samples of 10 ms or more at `group` (76 % at
    `strict`, 68 % with 16 writers) are due inside a fill's `fsync`, which M4 times at 287-503 ms. The write before it takes
    35-50 ms.
  - At `relaxed` every roll is held, at 263 ms p50.
  - CZ-S1b's commit cells never reached a fill, and so never saw this.
- **P1 (no fill).**
  - A commit's p50 rises to 1.9 ms at `group` (+94 %) and 1.6 ms at
    `strict` (+76 %), and a 128-row commit's to 2.6 ms (+66 %). The p95
    is 2.6-2.9 ms.
  - The held inserter fell behind its rate (3,584 of 4,000 statements;
    P0's 3,886, M2's 4,000).
  - The bulk writer gains 18 % at `group` and 49 % at `relaxed`, and the
    roll's hold falls to 5.5 ms.
- **M2 (written extents, no fill in the window; the ceiling of P3, P4
  and P5)** is the best arm at the commit (p25 to p99.9) and the bulk
  rate, not at every tail: P1 beats it at the bulk writer's p99.9 and
  max, at the roll's longest hold at `relaxed` (6.5 ms against 390 ms)
  and at the commit's max at `strict`.
  - A commit: 872 µs p50, 1.2 ms p95.
  - The bulk writer: +36 % at `group` and +53 % at `relaxed`. Cold, it is
    7-9 % below warm.
- **P2 (the fill paced, 1 MiB per 8 ms).**
  - At `group` the bulk writer gains 27 %.
  - At `relaxed` every roll waited for its body under the latch: 302 ms
    p50, 691 ms at its longest.
  - P2 was not run in M1 (the cut).
- **The tail without the fill (DG-Q6).** Every roll of 80 ms or more and
  every inserter statement of 100 ms or more that no fill overlaps lies
  inside a cadence checkpoint run, on core 0, 1 or 2: 8 rolls and 39
  statements in all, against runs covering 5-31 % of a window. The
  coincidence is measured; reading it as the log's sync tied to the
  filesystem's other syncs is inferred, and not settled.
- **P3's supply.** No segment was removed inside any window.
- **P5.** It is not in this host's headers (6.12), nor in the tree.

**The census** was read on `2c2ed8dd`, source `46ee8a6e`, delegated to a
reading agent and spot-checked at `restore.cpp:507` and `falloc.h`.
- **The tail's readers.**
  - The reader's rule: a header of zeroes is a clean end; an undecodable
    header, or an LSN not its position, is torn (`record.cpp:189-216`;
    the CRC covers the LSN, `:126-127`, `:145-146`).
  - The scanner: a PAD ends the segment, and a short tail is a seal only
    where a later segment exists (`log_scanner.cpp:147-152`, `:175-185`,
    `:196-198`).
  - The mount's scrub (`stream.cpp:220-270`) and the short-tail seal
    (`:379`, `:395-398`).
  - The torn-tail report, which is a report only (`analysis.cpp:189`,
    `mount_recovery.cpp:40`, `:82`).
  - A backup's end check, which ignores `stopped_early`
    (`restore.cpp:270-287`).
  - A restore's refusal of an undecodable archived tail, as Corruption
    (`restore.cpp:500-514`).
  - A backup's copy, `archive_dir` and a restore's copy all go through
    `CopyFileDurably`, a plain read, write and fsync
    (`backup_manifest.cpp:155-192`). Nothing in the tree uses
    `copy_file_range`, reflink or hole seeking.
  - An `archive_command` may make a sparse copy. That costs the archive's
    own space only: its check reads the bytes, and a restore copies them
    back densely.
- **Per candidate.**
  - **P1 and P5** keep every reader's bytes.
  - **P2 and P4** keep them only while no zero write lands after a body
    is adopted.
    - Today `prepare_mutex_` makes that so: the build ahead and the
      creation both hold it, and a build declines any number that is not
      the device's end (`file_log_device.cpp:388-403`, `:422-427`,
      `:453-456`).
    - A paced fill that lets go between pieces needs an exclusion of its
      own. On the `ftruncate` path, a body adopted part-filled has holes
      (DG-R1).
  - **P3 breaks.** Every mount's live tail reads as torn and is scrubbed,
    up to 64 MiB. A last archived segment sealed with a short tail is
    refused by a restore with a target.
- **Corrections to §1.2 and §2.**
  - The torn refusal is `restore.cpp:500-514`, not `:516-529`. It
    touches a short-tail or unsealed last segment, not every clean end: a
    PAD seal, and a short tail in a non-last segment, are unaffected.
  - The short-tail seal is at `stream.cpp:379`/`:395-398`, and the ask at
    `:494-503`.
  - **P3's invariant** is a *strictly lower-numbered* body. A
    point-in-time restore removes the segments past its cut
    (`restore.cpp:108-113`), and their numbers are created again, so
    "same lineage" is neither needed nor enough.
  - The read before a partial write is not P3's alone: a filled body that
    has left the page cache has it too.
  - `wal.md` §4.1's text makes a format event of a change of payload or
    size only. P3 is one by DG-R4 and §4.2.
- **Texts that become false under P1**, besides the five §5 names (at the
  same lines at `46ee8a6e`): `file_log_device.hpp:129-132`, `:203-205`;
  `file_log_device.cpp:429-437`; `wal.md`'s "reserves, zeroes and fsyncs";
  `known-gaps.md`'s roll entry.

**Reviews.**
- The spike, by `critics-developer` before the tables were read: no arm
  invalid. Its three reading checks are applied in the results file.
- **The results file, by `critics-developer` against the raw data.** All
  three scripts were re-run and reproduce byte for byte. The corrections
  it made in the documents:
  - "M2 beats both in every cell" was an overclaim. M2 is best at the
    commit and at the bulk rate, but P1 beats it at the bulk writer's
    p99.9 and max at `group`, at the roll's longest hold at `relaxed`,
    and at the walker's p99 at `relaxed`.
  - **The checkpoint runs.**
    - They are on cores 0-2, not core 0 alone.
    - Their parks are mostly on the log (`log_park_us` 426-657 ms,
      `data_park_us` 30-88 ms), so which way the stall runs is not
      established.
    - They cover 5-31 % of a window, so the residual tail's link to them
      is a measured coincidence with an inferred cause.
  - The writer attribution is 79 % at `group`, 76 % at `strict` and 68 %
    with 16 writers, summed over 3 runs. Six samples at `strict` are due
    inside a checkpoint run. In P1's 16-writer cell, the commit tail is
    the checkpoint's (327 of 344 samples).
  - P2's explanation at `relaxed`. A segment fills in 0.5-0.6 s after the
    previous roll, so every roll waits 153-630 ms for its body.
  - P1's held inserter. P0 fell short too, and P1's commits were measured
    beside 8-23 % less inserter load.
  - Inferences now labelled as such, and several ranges corrected.
  - The budget: 28.2 minutes from the build's start.
  - V0's table now carries the 128-row commit's p95: P0 2.0 ms, P1
    3.7 ms, M2 1.9 ms. At that size P0 beats P1.
- **Flagged and applied by CLA:**
  - the archive renamed with its describe suffix;
  - `__pycache__` removed;
  - the thesis's "stalls commits" narrowed to one-row commits, since the
    128-row p95 shows no stall.
- **Flagged and left:**
  - The "longest run of long statements" DG-Q1 (a) names is not
    computed; long ms/s stands in for it, and the file says so.
  - The §2-§4 tables carry their units in the heading only.
  - `bg-pw`'s 285 ms roll, with 9 ms at the gate, is unexplained, and so
    is the cold arm's low walker count.

**Ends at V0, the operator's reading of the results file's §1 (DG-Q1 (a)).**
DG-S1 waits for the candidate the operator names (DG-Q2 (a)).
