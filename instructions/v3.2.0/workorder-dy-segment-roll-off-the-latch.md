# Work order DY - a WAL segment roll that does not hold the append latch across its I/O: an exploratory order, a proof before any code

Status: **WRITTEN 2026-11-26, not opened.** Written on worktree
`known-gaps-milestone-orders` (branch
`worktree-known-gaps-milestone-orders`) from `8274c951`
(`v3.4.17-148-g8274c951e`) on the operator's word of 2026-11-26
(`raft-marks-2026-11-26.md` §3). No stage is started; DY-S0 waits for the
operator's word and for the marks on §4's items - **and DY-Q0 needs the
operator's own word, not a standing go-ahead**, because BQ-Q5 (b) says
so. A `[source-read]` claim with a `path:line` was read at `8274c951`; a
claim taken from `known-gaps.md` carries that entry's own verification
commit and is not re-verified here.

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
