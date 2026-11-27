# Work order DY - a WAL segment roll that does not hold the append latch across its I/O: an exploratory order, a proof before any code

Status: **OPEN for DY-S0 only, since 2026-11-27; DY-S0 is the proof and
builds nothing, and DY stops behind it at DY-Q0.** Opened on worktree
`dy-segment-roll-off-the-latch` (branch
`worktree-dy-segment-roll-off-the-latch`) from `630ecc359`
(`v3.4.17-178-g630ecc359`), on the operator's word of 2026-11-27
(`raft-marks-2026-11-27.md` §1): the standing go-ahead
(`go-ahead-achieving-milestone`), given with DY named as its milestone.
**DY-Q0 is not marked.** BQ-Q5 (b) reserves rung 4's license to the
operator's own word and this order says a standing go-ahead is not that
word; a go-ahead that names DY is still a standing go-ahead. **CLA's
reading, and it is CLA's**: the word asks for DY's work, the reservation is
on building H6, and DY-S0 builds nothing - so DY-S0 runs, and no stage
after it starts until the operator says DY-Q0. DY-Q1 (a), DY-Q3 (a) and
DY-Q4 (a), the items DY-S0 itself turns on, are adopted on that standing
go-ahead (§4, "Marks").

Written on worktree `known-gaps-milestone-orders` (branch
`worktree-known-gaps-milestone-orders`) from `8274c951`
(`v3.4.17-148-g8274c951e`) on the operator's word of 2026-11-26
(`raft-marks-2026-11-26.md` §3). A `[source-read]` claim with a
`path:line` in §0-§2 was read at `8274c951`, and one in §7 at
`630ecc359`; a claim taken from `known-gaps.md` carries that entry's own
verification commit and is not re-verified here.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `docs/spec/wal.md` §3, §6, §11;
`docs/inflight/known-gaps.md`, "WAL", "Placement per transaction (BL)",
"Statement scheduling";
`instructions/v3.0.0/workorder-bq-wal-segment-roll.md` (BQ, closed with
the latch kept: BQ-Q5 (b), BQ-Q6 (a), H3, H6, §7);
`instructions/v3.2.0/workorder-cz-checkpoint-roll-wait.md` (CZ);
`instructions/v3.2.0/workorder-dg-wal-segment-body.md` (DG, abandoned
2026-11-21 with nothing built).

## 0. What DY is

`[source-read]` A roll syncs the segment it leaves under the stream
latch, deliberately (`wal.md` §6 step 1). At `relaxed` under one bulk
writer the hold is hundreds of milliseconds, and every taker of the
latch but the cadence checkpoint waits it out
(`known-gaps.md:493`). Two orders have looked at it: BQ kept the latch,
and DG was abandoned. **No order owns the stall.**

`[design]` DY is what BQ named rung 4 (H6): the latch released for the
roll's I/O. BQ marked it `[quiet-wrong]`, its proof unwritten. So DY is
**exploratory, and its first stage writes that proof and nothing else**.
If the proof does not close, DY stops with nothing built, as DG did, and
says what would be needed.

## 1. The tree as read

`[source-read]` from `docs/inflight/known-gaps.md` at `8274c951`:

- **The hold** (`:493`, `[measured]` by BQ at `4cf30270`): p50 about
  330 ms, p99 about 790 ms, max 1.2 s at `relaxed` under one bulk
  writer; about 6 ms p50 at `group`. CZ's close cell read a median of
  about 250 ms (`bench/v3.4.12/results-cz-close-v3.4.12-113-g74861601.md`).
- **Who waits**: a commit, a drain, `EnsureDurable` and a foreground
  statement on a page latch a roll's appender holds. Core 0's
  stream-latch wait was 3.0-3.9 s of every 10 s in BL's cell (`:1308`).
  A yielding build is stalled 529 and 540 ms at its two rolls
  (`:1242`).
- **Every sync covers every live segment** (`:456`): about 50 us a live
  segment, and with every non-tail sync removed the roll's hold p99
  stays 652-780 ms at `relaxed`, so narrowing the sync cannot meet BQ's
  bar (BQ-Q6 (a)). "Syncing only the segments written since the last
  sync would be enough ... Nothing records which those are."
- **The zero prewrite** (`:513`, `[measured]`
  `bench/v3.5.0/results-dg-s0-premise-v3.5.0-36-ge9952d64.md`, ext4): a
  body built ahead is 64 MiB of zero writes (35-50 ms) and an `fsync`
  of 287-503 ms; beside a 24 MB/s log a one-row commit's p95 is about
  171 ms at `group`; without the fill the commit's median rises
  76-94 %.

`[source-read]` in the code at `8274c951`: `WalStream::Roll`
(`src/wal/stream.cpp:272`) reaches `WalStream::StartSegment` (`:155`),
which syncs the segment it leaves; the device's sync is
`FileLogDevice::Sync` (`src/wal/file_log_device.cpp:576`), whose comment
forbids narrowing it to the tail because a roll can land between the
stream capturing its watermark and the device sync.

## 2. The mechanism `[design]`

A sketch for DY-S0 to prove or refute, not a design to build from:

1. **Under the latch, the roll only seals and switches.** It writes the
   roll record, fixes the left segment's end LSN, and points the append
   at a next segment that already exists and was made durable ahead of
   need. No device sync is made under the latch.
2. **The left segment's sync is the WAL writer's**, made off the latch.
3. **The durable point is per segment.** The durable LSN cannot pass
   the end of a sealed segment until that segment's sync has returned:
   it is the minimum over unsynced segments, in LSN order. This is the
   record `:456` says is missing, and it would also let a sync cover
   only the segments written since the last one.
4. **The next segment's body** is built ahead by the writer thread, as
   today. Its `fsync` is what DG measured stalling commits; DY-S0 reads
   whether that stall survives once no latch is held across it, and does
   not take up DG's question otherwise (DY-Q3).

## 3. Rules

- **DY-R1. No LSN is reported durable whose segment's sync has not
  returned** `[quiet-wrong]`: a commit acknowledged on an unsynced
  sealed segment is a lost commit after a power cut. DY-S0's proof is of
  this rule, over every reader of the durable point - a `group` drain, a
  `strict` commit's publish, the checkpoint's log park, the recycler's
  fold, a backup's stop.
- **DY-R2. A failed sync of a sealed segment fail-stops the log**, as a
  failed sync does today: every later write is refused and no page is
  written back.
- **DY-R3. Recovery reads the same log**: no record kind, envelope or
  segment header moves, and a log DY's build wrote is one the build at
  `8274c951` replays.
- **DY-R4. The mount's open, which zeroes an unsealed tail and syncs
  what it scanned, is unchanged** (`wal.md` §4.2).
- **DY-R5. The simulator gains a crash point between a seal and its
  sync**, torn and reordered, before any behaviour lands.
- **DY-R6. The bar**: the append latch's hold at a roll under BQ-S1's
  `relaxed` bulk cell is under 10 ms at p99, and a co-located point
  read's p99 through a roll is inside 2x its p99 without one. Missed,
  DY reports the miss and does not land.
- The standing rules, as DT's order lists them.

## 4. Items for the operator (DY-Q), CLA's proposals

| | item | options | CLA proposes |
|---|---|---|---|
| DY-Q0 | Is rung 4 licensed? (BQ-Q5 (b): the operator's own word) | (a) yes, as an exploratory order whose first stage is the proof; (b) no - the latch stays and DY is not opened | (a) |
| DY-Q1 | Does DY-S0 gate? | (a) the proof is reviewed before any code; a reader of the durable point the proof cannot cover ends DY. (b) n/a - there is no (b): no code precedes the proof | (a) |
| DY-Q2 | The narrowed sync (`:456`, BQ's rung 3) | (a) taken with DY, since §2.3's per-segment record is what it lacked; (b) left | (a) |
| DY-Q3 | The zero prewrite's stall | (a) measured in DY-S0 and left as DG left it unless the latch's release removes it; (b) reopened | (a) |
| DY-Q4 | `relaxed` | (a) the bar is read at `relaxed`, `group` and `strict`; (b) at `group` and `strict` only, `relaxed` being a rung a financial system does not run | (a) |
| DY-Q5 | An off-switch | (a) none: one roll path; (b) a boot key for one release | (a) |
| DY-Q6 | **What a process kill loses at `relaxed`** (added by DY-S0, §7 P3), `[quiet-wrong]`: under §7's P0 a record flushed into a segment not yet named is discarded by the mount after a kill, where today the mount keeps every flushed record. A `relaxed` commit acknowledged in that span - until the first sync after the switch returns, 0.7 s at the median under a bulk writer - is lost; no `group` or `strict` commit is | (a) accepted and stated: `wal.md` §1's `relaxed` row says its window, after a roll, is the sync's and not the flush interval's, at a kill as at a power cut. (b) not accepted: DY is not built. (c) the mount adopts a temporary segment that stands behind a durably sealed tail and removes one that stands behind an unsealed tail, which amends DY-R4 and is a `[quiet-wrong]` proof of its own (a stale temporary file's records must never be read as the stream) | (a): `relaxed` is the rung for reconstructible data, its window at a power cut is already the sync's length (the measurement), and (c) reopens the open, which DY-R4 exists to leave alone. **Not adopted: no stage that would build it has started, and it changes what a rung means, against §6** |
| DY-Q7 | **How many segments may wait for a name** (added by DY-S0, §7 "The measurement"): at `relaxed` under a bulk writer the syncs took a median 0.69 s and a p90 of 1.95 s against a segment filled every 1.03 s, with the appenders stalled by the holds DY removes | (a) a bound of two unnamed segments; a roll that would make a third is today's latched roll, so the log's distance from its durable point is bounded and the hold returns as the backpressure. (b) no bound | (a). With (b) nothing bounds the temporary files or the span DY-Q6 loses. **Its price is DY-R6's**: every roll that meets the bound holds the latch as today, and the measurement does not show how many would. Not adopted |

### Marks

No row above carries the operator's own mark. What stands, and how:

| | mark | whose |
|---|---|---|
| DY-Q0 | **unmarked.** Reserved to the operator's own word (BQ-Q5 (b)); the standing go-ahead of 2026-11-27 is not that word and CLA does not adopt its own proposal here. DY-S0 ran on CLA's reading that a proof builds nothing (the header) | - |
| DY-Q1 | **(a)**: the proof is reviewed before any code | CLA's proposal, adopted on the operator's standing go-ahead of 2026-11-27 (go-ahead-achieving-milestone) |
| DY-Q2 | **unmarked, and CLA's proposal is revised to (b) by DY-S0** (§7, "What the proof changes in this order"): §2.3's per-segment record is not what rung 4 needs while every sync covers the live run, so the narrowed sync is a second `[quiet-wrong]` proof bought for about 50 us a live segment a sync (`known-gaps.md:510` at `630ecc359`) | - |
| DY-Q3 | **(a)**: measured in DY-S0 as far as the tree's instruments reach, and left as DG left it | CLA's proposal, adopted on the same go-ahead |
| DY-Q4 | **(a)**: the bar is read at `relaxed`, `group` and `strict` | CLA's proposal, adopted on the same go-ahead |
| DY-Q5 | unmarked; no stage that would turn on it has started | - |
| DY-Q6 | unmarked. Added by DY-S0 with CLA's proposal (a); it changes what `relaxed` means at a process kill, and no stage that would build it has started | - |
| DY-Q7 | unmarked. Added by DY-S0 with CLA's proposal (a) | - |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DY-S0** | The proof of DY-R1: every reader and writer of the durable point, the watermark's capture against a concurrent roll (`FileLogDevice::Sync`'s comment), the roll record's premise at `stream.cpp`, AL-R1's annotation. The hold re-measured at the current tree in BQ-S1's cell. No engine code | the proof in §7, reviewed; a results file; DY-Q1 read |
| **DY-S1** | The per-segment durable record and the narrowed sync, the latch still held - no behaviour moves but the sync's breadth | cells per reader of the durable point; DY-R5's crash points |
| **DY-S2** | The latch released for the roll's I/O | every cell and mutant of H6; `scripts/sim.sh` with the new points; the `tsan` build over the roll |
| **DY-S3** | The close: DY-R6's bar, `wal.md` §3 and §6 rewritten, `known-gaps.md` | the operator's word on landing |

## 6. What DY does not do

- No second log stream, no per-core log (D14: one stream for the
  instance).
- No change to segment size, the group-commit cadence or the durability
  rungs' meaning.
- No reuse of a recycled segment's body in place of the zero fill.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-26

On `known-gaps-milestone-orders` from `8274c951`, on the operator's word.
No stage is opened and no code is written. **Not reviewed**: the
`critics-developer` review was started and stopped before it reported,
so no line reference or reading in this order has a second reader behind
it.

### DY-S0 (2026-11-27): the proof of DY-R1, on `dy-segment-roll-off-the-latch` from `630ecc359`

No engine code. Every `path:line` below was read at `630ecc359`
(`v3.4.17-178-g630ecc359`). The proof is of a **mechanism this stage
restates** (P0): §2's sketch does not survive as written in two places,
and "What the proof changes in this order" says which.

#### P0. The mechanism proved `[design]`

1. **The next segment is entered under its temporary name.** The body is
   built ahead as today (`FileLogDevice::BuildAhead`,
   `src/wal/file_log_device.cpp:387`), and its header is written and
   synced with it, by the preparer and not by the roll: the header's
   fields are the segment's number and nothing the roll learns later
   (`src/wal/stream.cpp:169-177`).
2. **Under the latch a roll seals and switches.** `SealLocked` (the `PAD`
   marker and its `pwrite`, `stream.cpp:369-405`), then the device
   **adopts** the prepared descriptor into its segment table - no I/O -
   and the stream moves `append_lsn_` and `flushed_lsn_` to the next
   segment's first record, **in that order: adopt, then move, the move
   of `flushed_lsn_` a release store** (it is relaxed today,
   `stream.cpp:193`). The adoption holds `prepare_mutex_`, as a creation
   does (`file_log_device.cpp:422`), so the preparer's test of the run's
   end cannot go stale beside it and no build removes an adopted file's
   name. `Roll`'s own `FlushLocked` (`stream.cpp:274`) stays, for a
   segment sealed before the roll reached it. No sync, no rename and no
   directory sync is made under the latch.
3. **The name is the sync's.** A device sync copies, in the one hold of
   `segments_mutex_` it already takes (`file_log_device.cpp:608-612`),
   the descriptors **and the list of adopted segments not yet named**. It
   `fdatasync`s every descriptor as today, and only then, for each
   unnamed segment of its copy in ascending order, renames it to its own
   name **and syncs the directory before the next rename**: two renames
   under one directory sync are ordered by nothing but the filesystem's
   journal, and a cut that kept the later name alone would leave a gap
   the mount refuses (`file_log_device.cpp:296`). It returns OK only
   after all of that. A failure of any step is the sync's failure, so
   `WalStream::SyncDevice` stops the log (`stream.cpp:145`). A sync
   whose copy lists no unnamed segment makes no directory sync - a
   commit's sync costs what it costs today. A sync marks named only what
   it renamed, since a switch can land after its copy; and it changes
   nothing in the segment table, which a flush's `WriteAt` reads
   unlocked (`file_log_device.cpp:517`).
4. **Every sync still covers the whole live run**
   (`file_log_device.cpp:581-632`). No per-segment durable record is kept
   and the durable point stays the two watermarks it is.
5. **A roll that finds no body ready, an unshared stream and a stream
   with no writer keep today's roll**, whole and under the latch
   (`StartSegment`, `stream.cpp:155-197`): sync, then create. That sync
   names whatever is still unnamed first, by step 3.

#### P1. Who reads the durable point, and who writes it `[source-read]`

**There are two watermarks and every reader reads one of two things**:
`WalManager::durable_lsn()` (`include/kds/wal/manager.hpp:264`), the
larger of the stream's and the writer's, almost always through
`WalDurability::IsDurable` (`include/kds/wal/durability.hpp:71`); or the
writer's alone, which an attached manager's `Sync` waits on
(`src/wal/writer.cpp:36-44`, `manager.cpp:201`). Both are written under
the one rule below, so a reader of either is a reader of the claim. By
what they do with it:

| reader | where | what a wrong answer is |
|---|---|---|
| a `group` or `strict` commit's publish, a deferred commit | `src/server/command_dispatcher.cpp:1257`, `:1562`, `:7359`, `:15400`, `:16419`, `:16893`; `EnsureDurable` at `:1275`, `:1685`, `:1750` | a commit acknowledged and lost |
| the page writeback gate (WAL before data) | `src/storage/device_page_store.cpp:1983`, `:2268`; `src/storage/page_mgr/page_mgr.cpp:185` | a page on disk ahead of its record |
| the checkpoint: its end record's wait, its parks, the anchor's `durable_lsn` | `src/wal/checkpointer.cpp:382`, `:391`, `:540`, `:613`, `:739-812` | an anchor naming log that is not there (`src/wal/analysis.cpp:195` refuses that mount) |
| the recycler | its bound is a published anchor's redo start (`wal.md` §11), so it reads the durable point through the checkpoint | a segment removed that redo needs |
| the archiver | `src/server/wal_archive.cpp:138` (and its report, `:345`): every segment below `durable_lsn() / segment_size` is copied **by its file name** (`:179`) | a segment archived short, or not found |
| a backup's stop | `src/server/backup.cpp:300` (`SyncAll`), then segments `[start, stop]` copied **by name** (`:386-389`) | a backup whose log ends short of its stop |
| the drain's free-tick test | `src/wal/manager.cpp:567` | a tick that skips work it owes |
| `sys.meta` | `command_dispatcher.cpp:2620` | a wrong report |

DY-R1 named five of these. **The page writeback gate, the archiver and
the anchor's field were not in its list** and are in this proof. Three
callers take their target from `appended_lsn()` and then wait on
`IsDurable` like any other (`src/catalog/catalog.cpp:3412`,
`command_dispatcher.cpp:10229`, `src/server/expeditor.cpp:1563`); none
infers durability from the append point, from `sealed()` or from a roll
having returned.

**One reader's premise goes, and its liveness does not rest on it.** The
cadence checkpoint's log park says that under a roll it "ends durable,
the roll's seal having flushed the ring and its sync published it, with
no second try" (`checkpointer.cpp:758-765`, `wal.md` §11). Under P0 a
roll publishes nothing. The park still ends when the roll's flag clears
and the loop finds the LSN not durable and asks again (`:797`), so the
checkpoint waits one more round; the sentence is DY-S3's to rewrite.

**The writers are three sites**, and nothing else stores either
watermark (a `grep` over `src/` and `include/` for `PublishDurable`,
`durable_lsn_` and the writer's `durable_`):

- `WalStream::Sync`: reads `flushed_lsn()`, then `SyncDevice`, then
  publishes what it read (`stream.cpp:615-622`);
- the roll: `SyncDevice`, then publishes `flushed_lsn()`
  (`stream.cpp:165-166`), sound only because the latch is held and no
  flush can land between the two - **under P0 this site is P0.5's
  latched roll alone**, where that still holds;
- `WalWriter::Run`: reads `requested_` (`src/wal/writer.cpp:90`), calls
  the stream's `SyncDevice` (`:117`, wired at `manager.cpp:144`), then
  publishes what it read (`:127`). Every request is a `flushed_lsn()`
  read by its asker before the ask (`manager.cpp:200-201`, `:291`).

`WalStream::Open` stores the scanned end after a sync of its own, alone,
before any other thread exists (`stream.cpp:94-99`).

**So today's rule is one sentence (I0, the snapshot rule): a published
value is a `flushed_lsn()` read before a `SyncDevice` call began, and is
published only if that call returned OK.** Every sync of the segments'
data on a running instance goes through `SyncDevice`, one at a time
(`stream.cpp:114-147`); the open's is its own, and the simulator's
offline one is on a log nothing appends to (`sim/instance.cpp:485`). The
directory is synced by three more sites - the open, a creation and the
reclaim (`file_log_device.cpp:331`, `:493`, `:695`) - serialized with
each other and sticky on a failure (`:712-729`). **One effect of P0.3 is
new there**: a reclaim whose directory sync failed stops the log at the
next sync that names a segment, where today it stops it at the next
roll.

#### P2. The lemmas

- **A (below the flushed point is written).** Every byte below a
  `flushed_lsn()` value was handed to the device before that value was
  stored: an unlatched flush stores after its `pwrite` returns
  (`stream.cpp:563-570`), a latched one the same (`:592-601`), and a
  switch stores the next segment's first record after adopting a
  descriptor whose header was written and synced by the preparer (P0.1,
  P0.2). No flush is in flight across a switch: the seal's `FlushLocked`,
  or `Roll`'s where the segment was sealed earlier (`:370-372`, `:274`),
  waits one out, and none can be claimed while the latch is held
  (`:547`, `:582`), so no later store pulls the value back into the
  segment left.
- **B (a sync covers what was flushed before it began).** Let `F` be read
  before a `SyncDevice` call begins. A byte below `F` lies in a segment
  that was in the table when `F` was read (P0.2's order: adopt, then
  move). The sync's copy is taken later, and a table only loses a
  segment to a detach, which takes only segments wholly below a
  **published anchor's** redo start - below a durable point published
  earlier, so durable already, by this lemma applied to that earlier
  publish (publishes are ordered in time, so the induction is
  well-founded). So the copy holds every segment with a byte below `F`
  that is not durable yet, and the `fdatasync` of each returns before the
  call does. This is today's argument, unchanged.
- **C (a name follows its predecessor's seal).** A segment is renamed
  only inside a sync whose copy lists it unnamed (P0.3). It is listed
  only after its adoption, which is after the seal's `pwrite` into the
  segment before it returned (P0.2). The rename comes after that sync's
  `fdatasync` of every descriptor in the copy, the predecessor's among
  them. So **on the device, a segment has its own name only after its
  predecessor's records and seal are durable** - `wal.md` §4.1's
  sentence, "a scan never stops at an unsealed segment with the stream
  resuming beyond it", and BQ-R1's first clause, read as it must be: the
  next segment does not exist *to a mount* before the one left is
  synced. Two segments unnamed at once are named in ascending order
  inside one sync, each one's predecessor in the same copy and each
  name's directory sync returned before the next rename (P0.3), so no
  cut leaves a higher name without the lower.
- **D (durable inside a segment means the segment is named).** Let a
  published value `F` lie in segment `K`, at or past its first record.
  `F` was read after the switch into `K`, so after `K`'s adoption; the
  `SyncDevice` call it was published from began after that read; its
  copy therefore lists `K`, named or not; and the call returned OK, which
  P0.3 allows only after `K` - and every lower unnamed segment - has its
  name and its directory sync has returned. **"After" must be an order
  the copy can rely on**: the adoption and the copy both take
  `segments_mutex_`, and the reader of `F` is tied to the adoption by
  P0.2's release store, which `flushed_lsn()` already loads with acquire
  (`include/kds/wal/stream.hpp:294`); a writer-thread publish is tied
  through the asker's read and `requested_` (`writer.cpp:22`, `:90`).
  The exposure without the release store is the one value at `K`'s first
  record, which no commit or `page_lsn` waits on (`IsDurable` is strict)
  but the anchor's `durable_lsn` field can carry.

#### P3. DY-R1, reader by reader

**The claim: no value is published that a power cut can take back.**
Let `F` be published. By I0 and B every byte below `F` is durable. By D
the segment holding `F` and every one below it is named durably, and by
C each is sealed durably before its successor has a name. So a mount
after any cut adopts a run whose scan reaches at least `F`
(`src/wal/log_scanner.cpp:117-200`: a sealed segment is walked through;
`:176` and `:196` are unchanged), and:

- **a commit's publish** waits for a value past its record: covered;
- **the page writeback gate** waits for a value past the page's
  `page_lsn`: covered, and it is the reader that makes an early publish
  unrecoverable rather than merely lossy;
- **the checkpoint and the recycler**: an anchor is published only past
  its end record's durable point (`checkpointer.cpp:382`, `:613`), which
  is at or above its redo start, so the segment its `segment_no` names
  is named durably (D) and a mount finds it (the refusal at
  `file_log_device.cpp:284` is not reached); a segment the recycler
  detaches is wholly below that redo start, so synced (B) - never a
  sealed segment still waiting for its sync;
- **the archiver** copies segment `s` by name only when the durable point
  is in a segment above it: `s` is named and wholly durable;
- **a backup's stop** seals and then `SyncAll`s; `SyncAll` returns only
  past a publish at or above the stop (an owner's `Sync`, or the
  writer's watermark through `EnsureDurable`), so every segment up to the
  stop's is named when the copy by name begins;
- **the drain's free tick** holds only when the durable point has reached
  the append point - which is then named - so no tick is skipped while a
  segment waits for its name (liveness, below).

**The fail-stop (DY-R2).** A failed `fdatasync`, rename or directory
sync fails the device sync, `SyncDevice` stops the stream before it
releases `sync_mutex_` (`stream.cpp:145`), and nothing is published from
that call or from any later one (`:130`). Appends accepted into the
entered segment before the stop were never published past; a `relaxed`
commit among them was acknowledged on the append, as it is today inside
its loss window.

**Recovery reads the same log (DY-R3, DY-R4).** After a power cut, the
states the device can be in are today's but for a temporary file that
can now hold records, which no open reads:

| the cut | on the device | the mount |
|---|---|---|
| after the switch, before a sync has returned its `fdatasync`s | `N` sealed or torn; `N+1` a temporary file holding a header and whatever records reached it | adopts through `N`, as after today's cut before a roll's sync; the temporary file is never adopted (`wal.md` §4.1) and the next creation of that number removes it (`file_log_device.cpp:369`). Nothing in it was ever published |
| after a rename, before its directory sync returns | the row above, or the row below | either |
| between two segments' names (two unnamed at once) | `N+1` named and durable, `N+2` a temporary file | the first row, one segment on: P0.3 syncs the directory before the second rename |
| after the directory sync | `N` sealed and durable; `N+1` named, with a durable header and a tail that may be torn | today's state after a roll and some appends; the open scrubs the tail (`stream.cpp:220-270`) |

No record kind, envelope or header moves, the open is not changed, and
a temporary file with records in it is to the build at `8274c951` what
an empty one is: a leftover it removes (the name's suffix is refused at
the open and the next build of that number unlinks it,
`file_log_device.cpp:369`).

**What a process kill loses is new, and it is `relaxed`'s** (DY-Q6,
below). Today a flushed record survives a killed process: it is in the
page cache under a name the mount adopts, and the open syncs it before
claiming it (`stream.cpp:85-96`, `wal.md` §4.2). Under P0 a record
flushed into a segment that has no name yet is in a temporary file, and
the mount after a kill discards it as the mount after a power cut does.
No published value is lost, so no `group` or `strict` commit and no page
is affected; **a `relaxed` commit acknowledged in that span is lost at a
process kill, where today only a power cut loses it** - and the span
lasts until the first sync after the switch returns, which "The
measurement" reads at 0.7 s median at `relaxed` under a bulk writer,
against a loss window `wal.md` §1 states as the flush interval.

**Liveness.** While a segment is unnamed the durable point is below its
first record (D, read backwards), so `appended_lsn() != durable_lsn()`
and the existing askers still ask: a parked committer's drain
(`manager.cpp:586`), core 0's `relaxed` tick (`:605`), `SyncAll` at a
clean shutdown. The writer's own early-out (`writer.cpp:100`) cannot
swallow the ask, for the same reason. No new trigger is needed.

#### P4. BQ's H6 hazards, at this mechanism

H6 (`workorder-bq-wal-segment-roll.md` §3) kept the appends in the ring
while the next segment was made. P0 does not: the next segment is
writable at the switch. So:

1. *the ring fills during the roll* - does not arise outside P0.5's
   latched roll, where it is today's; a flush after the switch writes
   the entered segment;
2. *a second roll during the first* - there is no claim to wait out; a
   second switch needs a second body, and without one it is P0.5's
   latched roll, whose sync names the first;
3. *a reader of the moved `append_lsn_`, an anchor naming a missing
   segment* - P3's checkpoint row;
4. *the numbering* - the prepared number is the device's
   `end_segment()` when it is built (`file_log_device.cpp:397`) and the
   adoption is what moves it, under the latch, as a creation does today.

#### P5. What the proof rests on and cannot show

- **The filesystem**: data written through a descriptor is made durable
  by `fdatasync` of that descriptor whatever name the file has then or
  later; a rename issued after an `fdatasync` returned cannot be durable
  before it. The first is what today's body-ahead already rests on for
  the zero fill. Nothing is asked of the order of two renames: P0.3
  puts a directory sync between them.
- **The release store at the switch** (P0.2, lemma D).
- **Every sync goes through `SyncDevice`** and **every sync covers the
  live run**. A second sync path, or a sync narrowed to the tail, breaks
  D and B without failing any cell that exists today. Both are owed a
  mutant if DY is built.
- **The simulated device cannot yet express an unnamed segment**:
  `MemoryLogDevice::CreateSegment` makes name and header durable at once
  (`include/kds/wal/memory_log_device.hpp:66-71`). DY-R5's points - a
  cut after the switch, after the `fdatasync`s, after the rename - need
  the adoption and the naming modelled first. Until then C and D are
  arguments, not cells.
- **The bar is not this proof's.** What stays under the latch is the
  seal's `pwrite` and, when the body is not ready, P0.5's whole roll. At
  the bulk writer's rate BQ measured, a segment fills in under a second
  and a body takes about half a second to build, so how often a roll
  finds its body is a measurement ("The measurement", below), and so is
  whether one writer thread keeps up with the syncs.
- **A `group` or `strict` commit gains nothing through a roll, and its
  first batch after one pays more**: it still waits for the sync of the
  segment left, as it must, and that sync now also makes the rename and
  the directory sync the roll made. What P0 frees is everything that
  does not need the durable point - an append, a flush, a read that
  meets an appender's page latch.

#### What the proof changes in this order

- **§2.1, "a next segment that already exists and was made durable ahead
  of need", is refuted if "exists" means under its own name.** A segment
  named before its predecessor is sealed durably is the state
  `StartSegment`'s comment forbids (`stream.cpp:156-163`): a cut keeps
  the new name and drops the seal, and the stream resumes beyond a
  segment every scan stops in. Telling that state from a tail would
  change the open (against DY-R4), and the build at `8274c951` would
  misread it quietly (against DY-R3). P0.1 is the reading that holds.
- **§2.1's "roll record" is no log record.** A roll writes a `PAD`
  marker, or nothing where the tail cannot hold a header
  (`stream.cpp:379-398`), and DY-R3 already says no record kind moves.
  If the phrase means `RollRecord`, BQ-S1's timing of a roll
  (`stream.hpp:186`), that is an instrument and carries no premise. The
  premise at `stream.cpp` the proof had to answer is `StartSegment`'s
  comment, and lemma C answers it.
- **§2.3's per-segment durable point is not what rung 4 needs.** With
  every sync covering the live run, I0 and B carry DY-R1 as they do
  today; the one new obligation is the name (C, D), and it is met inside
  the sync, not by a watermark. So DY-Q2's premise - "§2.3's per-segment
  record is what it lacked" - does not hold, and CLA's proposal is
  revised to **(b), the narrowed sync left**: it would add a
  `[quiet-wrong]` proof of its own to save about 50 us a live segment a
  sync (`known-gaps.md:510` at `630ecc359`). The item stays the operator's.
- **DY-S1 as §5 writes it has no content left** if DY-Q2 is (b). What
  must come before any behaviour is DY-R5: the simulated device's
  adoption and naming, and the cut points over them.
- **DY-R1's reader list** gains the page writeback gate, the archiver
  and the anchor's field (P1).
- **§6's "no change to ... the durability rungs' meaning" does not hold
  of `relaxed` at a process kill** (P3). DY-Q6 is written for it.
- **DY-S3's rewrite list is longer than `wal.md` §3 and §6**: §4.1's "A
  roll's order is durable", §11's log park, `stream.hpp`'s protocol
  header (`:39-60`) and `FileLogDevice::SyncTimed`'s comment
  (`file_log_device.cpp:582-601`) each state the roll P0 replaces.

**AL-R1's annotation.** AL-R1 put the size checks, the roll, the encode
and the cursor bump under one latch and the device sync outside it
(`workorder-al-m0-single-wal.md`, AL-4). Its note of 2026-10-12 records
that a roll's sync has been under the latch since `a1679272`, and that
releasing the latch for the roll's I/O "crosses this ruling". **P0
releases nothing the ruling holds**: the roll - its seal and its switch
- stays under the latch with the append, and the LSN is still fixed
there. What leaves is the sync, the name and the directory sync, which
is the ruling's own sentence restored. If DY is built, the annotation
is one more dated note saying so; the ruling is not rewritten, being the
operator's.

**DY-Q1 read.** Under P0, and on P5's premises - the release store and
the directory sync between renames among them, both added by the review
- the proof finds no reader of the durable point it cannot cover. **It
closes on paper, and that is all it is**: no cell stands behind C or D,
and it does not show the bar ("The measurement").

#### The measurement `[measured]`

`bench/v3.5.0/results-dy-s0-premise-v3.4.17-178-g630ecc359.md`, in
BQ-S1's cell C1 (a 1,024-row bulk insert, six open-loop point readers,
four cores, 30 s windows), nine runs, 349 s; the build was 28 s. Cut for
the stage's 20 minutes: one placement, C2 and C3 not run, `strict` one
run. The host and the filesystem are not BQ's (a 24-CPU workstation on
xfs against an 8-vCPU VM on ext4), so no number here is a delta against
BQ's.

| rung | rolls | hold p50 | hold max | what would stay under the latch, max | the reader's p99 against its p99 alone |
|---|---|---|---|---|---|
| `relaxed` | 60 | 574 ms | 3,001 ms | 74 us | 1,308x and 1,676x |
| `group` | 43 | 1.65 ms | 15.3 ms | 27 us | 0.8x |
| `strict` | 19 | 1.39 ms | 15.8 ms | 26 us | 0.8x |

- **The premise holds at `relaxed` and is larger than BQ read it.** 95.2 %
  of the hold is the wait for the writer's sync in flight, and about
  99 % of the reader's slow samples fall inside a roll's window.
- **DY-R6's hold bar is reachable in principle and not shown.** With the
  sync, the header, the rename and the directory sync gone, a seal and
  a flush are left, at most 74 us in 122 rolls. **The body was ready in
  122 of 122**, so the case that would break the bar by itself - a roll
  that waits for its body, DG's 35-50 ms fill and 287-503 ms `fsync` -
  was not met, at 40-63 MiB/s of log with the appenders stalled by the
  holds. Without the holds the log is faster and the body has less
  time.
- **Whether one writer thread keeps up at `relaxed` is not
  established.** The syncs took a median 0.69 s and a p90 of 1.95 s
  against a nominal 1.03 s between rolls; 14 of 58 rolls had a gate wait
  and sync longer than that. DY-Q7 is written for it.
- **At `group` and `strict` there is little to remove.** The hold is the
  roll's own sync and creation, 1.4-1.7 ms at the median, over 10 ms in
  one or two rolls a run, and the reader is not slowed. **DY's gain is
  `relaxed`'s**, the rung DY-Q4 (b) calls one a financial system does
  not run - and DY-Q6 is that rung's cost.
- **The log is not recycled at `relaxed`** (0 segments removed, up to 36
  live at a sync), as BQ found; each non-tail segment costs a sync about
  60 us, so a whole-run sync's breadth is about 2 ms of a 0.7 s sync
  and DY-Q2 (b) gives up nothing the measurement can see.
- **Not measured**: DG's stall (DY-Q3 (a)) - no instrument in the tree
  separates a body's build from a roll, so it stays as DG left it;
  whether the long syncs are the device or the checkpoint's writeback
  beside them; anything of P0 itself, which is not built. The isolated
  reader was slower at the median than the loaded one (about 680 us
  against 260 us), read as this host's idle cores and not measured, so
  the ratios above have a soft denominator; against `group`'s loaded
  p99 the `relaxed` reader is still over 1,600x.

Overhead not measured; measured at the milestone's close. There is no
engine change to measure.

#### The review

`critics-developer`, on the proof as first written, with the code and
not the conclusion: it enumerated the watermark's writers, readers and
sync sites itself, checked every citation and tried to break each lemma.
**It found no interleaving or cut that publishes a value a power cut
takes back.** Applied:

- two renames under one directory sync can reach the disk out of order
  and leave a mount refused on a gap - P0.3 now syncs the directory
  between renames, and P3's table has the row;
- a process kill loses what was flushed into an unnamed segment - P3
  states it and DY-Q6 is written;
- the checkpoint's log park rests on the roll publishing - P1;
- what P0.3 left unsaid (no directory sync without a rename, named
  only what it renamed, the table untouched, `prepare_mutex_` at the
  adoption), lemma A for a segment sealed earlier, the writer's own
  watermark as a second thing read, three more callers, the sync sites
  outside `SyncDevice` and the reclaim's new stop, the fallback keeping
  the roll's publish - P0, P1, P2;
- AL-R1's annotation and this measurement were owed by the stage's row
  and missing - both written;
- three citations (`known-gaps.md`'s line at `630ecc359`, two ranges)
  and five overclaims, among them "the states are today's" and "a
  `group` commit gains nothing".

**Rejected in part**: that lemma D was wrong in the C++ model without a
release store. A load cannot read a store that happens after it, and the
reader's copy is sequenced after its load, so the mutex's order does
follow even from a relaxed store. The requirement is adopted all the
same: it costs nothing and makes the lemma a sentence instead of an
argument about the model.

**The suite was not executed**: the stage changes no build input.
