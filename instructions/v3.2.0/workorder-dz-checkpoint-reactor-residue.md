# Work order DZ - what a checkpoint still does on its reactor: the dirty-table walk, the page writes, the map barrier

Status: **WRITTEN 2026-11-26, not opened.** Written on worktree
`known-gaps-milestone-orders` (branch
`worktree-known-gaps-milestone-orders`) from `8274c951`
(`v3.4.17-148-g8274c951e`) on the operator's word of 2026-11-26
(`raft-marks-2026-11-26.md` §3). No stage is started; DZ-S0 waits for the
operator's word and for the marks on §4's items. A `[source-read]` claim
with a `path:line` was read at `8274c951`; a claim taken from
`known-gaps.md` carries that entry's own verification commit and is not
re-verified here.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `docs/spec/page.md` §5, §8; `docs/spec/wal.md` §11, §11a;
`docs/spec/sched.md` §4, §9; `docs/inflight/known-gaps.md`, "Carve and
checkpoint stalls", "The frame table's structure latch (DO)";
`instructions/v3.0.0/workorder-bi-checkpoint-off-the-reactor.md` (BI,
closed 2026-10-11); `instructions/v3.2.0/workorder-do-frame-table-long-holds.md`
(DO, closed 2026-11-22; its §5a is the fuzzy table's order);
`instructions/v3.2.0/workorder-cw-optimistic-page-read.md` (CW,
abandoned).

## 0. What DZ is

`[source-read]` Since BI a checkpoint is a `system` task that parks for
every sync, and since DO no other core waits more than about 0.3 ms for
its dirty-table walk. What is left is on the **walking core's own
reactor** (`CLAUDE.md`, "WAL", "Buffer-pool eviction").
`[design]` DZ bounds each piece of that to one bar, so a session on the
core that checkpoints is not stalled by the checkpoint.

**This order replaces candidate 7 of CLA's list as first stated.** The
list named the per-fetch holds of the structure latch; CW-S0 priced them
at 2.8 % of a point read's server CPU and CW was abandoned on that
number, so they are §6's, not DZ's.

## 1. The tree as read

`[source-read]` from `docs/inflight/known-gaps.md` at `8274c951`:

- **The dirty-table walk** (`:1618`, verified at DO-S3): the walking
  core is stalled about 49 ms for H1 at 1 Mi dirty frames under three
  readers (26 ms before E2), and the publish's walk again - the sort, a
  table lookup a slot and a handover a chunk, all off the latch but on
  the walker's reactor. "A walk budget, or a handover every few chunks,
  is not built."
- **The fuzzy table's premise is stated and not enforced** (`:1630`):
  no core yields between an append and its stamp.
- **The writes** (`:1578`, restated at `ec827a19`): a step's and a
  publish batch's `pwrite`s stay on the reactor, single writes up to
  about 300 ms in the kernel's writeback throttle, and the inline log
  gate after 16 deferrals at `relaxed`.
- **The map barrier** (`:1590`, `[source-read]`, not measured): the run
  task's `WriteHeaderlessMaps` and `WriteFreeMaps` take
  `map_flush_latch_`, a plain mutex, on the reactor, and a blocking
  flush - `SYNC`, a reclaim's `PersistMaps`, the synchronous anchor
  publish - holds it across an `fsync`.
- **A paced publish's map write can carry the bit of a page another
  core created unlogged after the collection** (`:1603`), allocated and
  never written after a crash.
- **A `relaxed` carve's longest stall is 226-258 ms per server life**
  (`:1571`, `[measured]` at `e5a7e1b7`), read there as a data-file
  `fsync` stall; BA-S13's exit, a carve under 10 ms, is not met.
- **Every core checkpoints in its turn** (`:1660`).

`[source-read]` in the code at `8274c951`: `DevicePageStore::PersistMaps`
(`src/storage/device_page_store.cpp:745`). The run's own functions are
not located here; DZ-S0 lists them.

## 2. The mechanism `[design]`

1. **The walk yields.** The collect and the publish walk give the
   reactor back every K chunks, holding nothing. What the walk has
   already collected stays valid by DO's §5a order - a frame dirtied
   after its slot was passed is the next run's - and DZ-S0 re-reads that
   order with a yield placed between chunks, which it was not written
   for.
2. **The premise is enforced.** A debug build aborts a yield taken
   between a dirty-table append and its stamp (`:1630`).
3. **The page writes leave the reactor.** A step copies each page under
   its latch on the reactor, as writeback does, and hands the copies'
   `pwrite`s to the instance's data-sync thread or a writer beside it;
   the run parks until they return. The copy stays where the latch is.
4. **The map barrier is tried, not waited for.** The run's map halves
   try `map_flush_latch_` and park on a busy answer, as the cadence
   tries the stream latch since CZ.
5. **The carve** is measured again first (DZ-S0). If it is still a
   data-file `fsync` on a statement's path, it is DZ's only where the
   statement can park (DZ-Q4).

## 3. Rules

- **DZ-R1. A run that yields mid-walk still covers every page dirtied
  before its begin record's redo point** `[quiet-wrong]`: a page the
  fold drops is a page redo does not replay. The anchor's fold never
  decreases, as today; a crash cell cuts between two yields of one walk
  on every core of a two-core instance.
- **DZ-R2. A page write off the reactor writes the bytes copied under
  the latch**, and only what the copy covered is cleaned - writeback's
  rule, unchanged.
- **DZ-R3. One checkpoint runs at a time for the instance**
  (`wal::CheckpointGate`), and a yielded run still holds the gate.
- **DZ-R4. A failed write or sync is sticky** as today: a failed
  data-file sync stops the instance.
- **DZ-R5. The bar**: no single hold of a reactor by a checkpoint above
  10 ms at 1 Mi dirty frames, at `group`; a run's whole length may grow
  by at most 25 %, since a longer run lengthens the replay range.
  Missed, DZ reports which piece missed.
- **DZ-R6. No thread is started after startup** (`sched.md` §9
  invariant 1); a writer thread, if one is added, starts with the
  instance and is the operator's item (DZ-Q2).
- The standing rules, as DT's order lists them.

## 4. Items for the operator (DZ-Q), CLA's proposals

| | item | options | CLA proposes |
|---|---|---|---|
| DZ-Q0 | Is DZ opened, and with what scope? | (a) §2's 1-4; (b) the walk only; (c) the writes only | (a) |
| DZ-Q1 | Does DZ-S0 gate? | (a) S0 measures each piece's reactor hold at 64 Ki, 256 Ki and 1 Mi dirty frames on the current tree; a piece already under DZ-R5's bar is left. (b) all built | (a) |
| DZ-Q2 | Who makes the page writes | (a) the existing data-sync thread, its queue ordered so a sync never waits behind a batch longer than S0 measures acceptable; (b) one more thread started with the instance | (a), with (b) if S0 shows syncs queueing |
| DZ-Q3 | The walk's yield interval | (a) derived from DZ-R5's bar and S0's per-chunk cost; (b) a boot key | (a): a key is a second name for the bar |
| DZ-Q4 | The carve at `relaxed` | (a) measured, and built only if it is the `fsync` the diagnosis read; (b) outside DZ | (a) |
| DZ-Q5 | The unlogged-page bit (`:1603`) | (a) outside DZ: it leaks a page and DV counts it; (b) inside | (a) |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DZ-S0** | The census of a run: every step on the reactor with its hold, at three dirty-set sizes and `cores` 1 and 4; DO's §5a re-read with a yield between chunks; the carve re-measured | a results file; DZ-Q1's bar read |
| **DZ-S1** | The walk's yield and the enforced premise | DZ-R1's crash cell; the rotated gate's cadence cells unchanged |
| **DZ-S2** | The page writes off the reactor; the map barrier tried | cells: a write failing off the reactor is sticky; a blocking flush beside a run |
| **DZ-S3** | The carve, as DZ-Q4 marks | its own cell or its recorded cut |
| **DZ-S4** | The close: DZ-R5 measured, a foreground A/B on the checkpointing core, `page.md` §8, `wal.md` §11, `known-gaps.md` | the operator's word on landing |

## 6. What DZ does not do

- Nothing for the structure latch's per-fetch holds or the mutex's
  unfairness (`known-gaps.md:1624`, `:1634`): CW's subject, abandoned.
- Nothing for a WAL segment roll (DY).
- No change to the cadence, the rotation or what a checkpoint records.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-26

On `known-gaps-milestone-orders` from `8274c951`, on the operator's word.
No stage is opened and no code is written. **Not reviewed**: the
`critics-developer` review was started and stopped before it reported,
so no line reference or reading in this order has a second reader behind
it.
