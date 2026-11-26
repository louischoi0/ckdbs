# Work order DT - a write that gives its core back: the write path's page miss off the reactor, and a write walk that yields

Status: **WRITTEN 2026-11-26, not opened.** Written on worktree
`known-gaps-milestone-orders` (branch
`worktree-known-gaps-milestone-orders`) from `8274c951`
(`v3.4.17-148-g8274c951e`) on the operator's word of 2026-11-26
(`raft-marks-2026-11-26.md` §3): *"write a milestone work order for each
item, one by one"*, over the twelve candidates CLA drew from
`known-gaps.md`. No stage is started; DT-S0 waits for the operator's word
and for the marks on §4's items. A `[source-read]` claim with a
`path:line` was read at `8274c951`; a claim taken from `known-gaps.md`
carries that entry's own verification commit and is **not re-verified
here** - DT-S0 re-verifies each.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured; `[quiet-wrong]` where a wrong choice turns a
correct answer into a wrong one with no error.

Related: `docs/spec/sched.md` §3; `docs/spec/page.md` §12;
`docs/spec/protocol.md` §7, §10; `docs/inflight/known-gaps.md`,
"Statement scheduling", "Key sets (CU)", "Column slots and `ADD`/`DROP
COLUMN` (BW)", "Catalog chains past the old ceiling", "Multi-core state,
continued"; `instructions/v3.2.0/workorder-de-page-read-off-the-reactor.md`
(DE, closed narrow; DE-Q7); `instructions/v3.2.0/workorder-cr-read-committed-rerun.md`
(a second user of the "wrote nothing, run again" test).

## 0. What DT is

`[design]` Two things a write cannot do today, and one consequence:

1. **A write's page read that must wait for the device leaves the
   reactor**, as a plain `SELECT`'s does since DE.
2. **A write walk yields its core** every slice of leaves, as a `SELECT`'s
   outermost walk does.
3. **A cancel is taken where a write yields**, so a cancelled or
   disconnected client's long write stops instead of running to its end.

The engine's answers do not change. What changes is who else runs on the
core while a write is cold or long.

## 1. The tree as read

`[source-read]` from `docs/inflight/known-gaps.md` at `8274c951`:

- **What yields** (`:1148`, verified at `0064d8bd`, re-read at
  `bf289a82`): a `SELECT`'s outermost walk, a relation-`X` build or an
  online `CREATE INDEX`, `GATHER STATISTICS`, `PURGE INDEX`. Not a write,
  a tuple key descent, a key set's descents, an index walk, a join's
  inner walk under one outer page.
- **What still blocks on a miss** (`:1164`, verified at `1942cb9a`): a
  write and its read phase - an `UPDATE`'s or `DELETE`'s locate, an
  `INSERT`'s descent, a foreign-key or unique check; a locking read; a
  `SELECT` run inside another statement; a walk's own first descent and
  every fetch under a walk's leaf; builds, `GATHER STATISTICS`, recovery
  and background tasks. **DE-Q7 (a)** is why the write was left: an issued
  key's first page write comes before its descent, and a write path's
  callers recover from a failed fetch in ways a refusal cannot pass
  through.
- **A cancel** (`:1269`) is observed only where a walk or a build yields,
  or at the next frame; a write asked to stop runs to its end.
- **A write by list holds its core for all its descents** (`:3014`,
  `:3044`); **`PURGE DROPPED COLUMNS` does not yield** (`:1816`); **the
  catalog's delete-mark purge walks with no yield** (`:2150`); **the Cabin
  controller's seeded build stops core 0 for a whole-relation walk**
  (`:739`).
- **A core holds at most 256 suspended walks** (`:1272`):
  `txn::kMaxRegisteredReaders` (`include/kds/txn/manager.hpp:202`).

`[source-read]` in the code at `8274c951`:

- DE's refusal is a thread-local arm: `ArmMissRefusal` /
  `NoteMissRefusal` (`include/kds/storage/miss_refusal.hpp:42-58`); a
  refused head is run again at most `kMaxMissReruns = 8` times
  (`include/kds/server/command_dispatcher.hpp:2276`); the reader has
  `kPageReadThreads = 4` (`include/kds/storage/page_reader.hpp:59`).
- A walk's slice is `pages_per_slice = 64`
  (`include/kds/exec/step_vm.hpp:347`).
- The write's row walk is `CommandDispatcher::WriteWalk`
  (`src/server/command_dispatcher.cpp:12879`), with a collection pass
  `CollectTargets` (`:13076`) and a `resumed_targets` argument
  (`:12903`): a write that met an undecided holder after writing rows
  parks mid-walk and resumes under its first view
  (`workorder-cr-read-committed-rerun.md` §1.2 reads the same at
  `ce5347e6`).

`[design]` So two mechanisms exist that DT composes rather than invents:
the run-again of a statement that wrote nothing (BA-S14's, DE's), and the
write walk's mid-walk park and resume.

## 2. The mechanism `[design]`

1. **Before the first write: run again.** A write statement whose trail
   is at its mark and that meets a page that would wait is treated as
   DE's descent is: the fetch is refused under the arm, the statement
   parks under `io.page_read`, and it is run again from its head. The
   arm is dropped at the statement's first page write, so nothing after
   that write is ever refused. DE-Q7 (a)'s case - an issued key's first
   write ahead of its descent - is met either by ordering the descent
   first or by excluding issued-key inserts (DT-Q3).
2. **After the first write: park at a leaf boundary.** A write walk that
   has written rows cannot be run again. Where its next leaf would wait
   for the device, it releases that leaf's latch and pin, parks as the
   mid-walk park does, and fetches the leaf again on its wake - the
   `SELECT` walk's rule, on the write walk's existing resume.
3. **The yield.** The same boundary, taken every slice of leaves without
   a miss: the walk gives its core back holding no latch and no pin, its
   lock units, view, borrow and statement epoch held as a suspended
   `SELECT`'s are.
4. **The cancel.** At a yield or a park, a pending cancel fails the
   statement: in autocommit its transaction is aborted, inside an
   explicit transaction the transaction is failed, as a walking
   `SELECT`'s cancel does today.
5. **Fetches below the walk** - an undo chain, a spilled value, a
   foreign-key parent, a unique check - stay blocking in DT's first
   stages and are counted; DT-S0 sizes them.

## 3. Rules

- **DT-R1. No latch, no pin and no no-refuse window is held across a
  yield or a park** `[quiet-wrong]` for the window: a refusal or a
  suspension after a page write leaves a mutation half done
  (`eviction.md` §3.3). A debug build aborts a yield taken inside one.
- **DT-R2. A statement that wrote anything is never run again**
  `[quiet-wrong]`, CR-R1's rule; it parks.
- **DT-R3. A resumed walk writes each qualifying row once** `[quiet-wrong]`:
  a row the statement itself moved or re-versioned is not written twice,
  whatever ran on the core in between. A test interleaves a second
  writer between two slices.
- **DT-R4. A write that never misses and is shorter than one slice
  executes no new instruction beyond the arm's test.** Measured at the
  close on a resident point `UPDATE` and `INSERT`.
- **DT-R5. Every new suspension is inside a `WaitScope`** under an
  existing wait event, and the wait-for graph sees every lock wait it
  saw before.
- **DT-R6. No persisted format, record kind or fingerprint moves.**
- The standing rules: each stage reviewed, the full suite per stage, the
  overhead measured once at the close in `build-release`, a stage's
  measurement inside 20 minutes under a 60 s watchdog, nothing pushed or
  merged without the operator's word.

## 4. Items for the operator (DT-Q), CLA's proposals

None is assumed; each waits for a mark.

| | item | options | CLA proposes |
|---|---|---|---|
| DT-Q0 | Is DT opened, and with what scope? | (a) §2's 1-4 for `UPDATE`, `DELETE` and `INSERT`; (b) the page miss only, no yield; (c) the yield only | (a) |
| DT-Q1 | Does DT-S0 gate the build? | (a) S0 is a census with a bar: if any write path holds a pin or a window across the proposed boundary and cannot be reordered, that path is cut and named; if the cold-write stall S0 measures is under 1 ms at p99, DT stops. (b) built whatever S0 shows | (a) |
| DT-Q2 | The write walk's slice | (a) `pages_per_slice`, one value with the read walk; (b) its own constant | (a): no second name for a quantity a setting expresses |
| DT-Q3 | An issued-key `INSERT` | (a) the descent is ordered before the id's first write where S0 shows it can be; else excluded and stated. (b) excluded | (a) |
| DT-Q4 | A cancel taken mid-write | (a) fails the statement as a walking `SELECT`'s does; (b) a write is never cancelled once it wrote | (a) |
| DT-Q5 | The 256 suspended walks | (a) a suspended write takes no reader-lease slot beyond what its transaction already holds, S0 to confirm; (b) it shares the 256 | (a) |
| DT-Q6 | The background walks (`PURGE DROPPED COLUMNS`, the catalog purge, the controller's seeded build) | (a) a last stage, each made a `BuildRun` as `PURGE INDEX` is; (b) outside DT | (a) |
| DT-Q7 | Locking reads and query-layer walks | (a) outside DT, stated; (b) inside | (a) |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DT-S0** | The census: every write path's fetches in order, what each holds (latch, pin, window, unit) at each, where the trail first moves, what a refused fetch's callers do today. The premise measured: a cold point `UPDATE`'s and a long walked `UPDATE`'s effect on a co-located point read's p99, in `build-release`. No behaviour change | a results file under `bench/`, the census in §7, DT-Q1's bar read |
| **DT-S1** | §2.1: the run-again of a write that wrote nothing, for a keyed `UPDATE`/`DELETE` and a named-key `INSERT` | cells: the run-again commits once; a statement that wrote is not re-run; the eight-run bound; a rig cell beside a reader |
| **DT-S2** | §2.2 and §2.3: the walk's park at a cold leaf and its yield | cells: DT-R1's abort, DT-R3's interleaving, a sim seed class that yields writes |
| **DT-S3** | §2.4, and DT-Q6's background walks as marked | cells: a cancelled walked `UPDATE` leaves no row written; `C_CANCEL` and a closed connection |
| **DT-S4** | The close: overhead A/B over the whole change, `sched.md` §3, `page.md` §12, `known-gaps.md`, the report | the operator's word on landing |

## 6. What DT does not do

- No yield inside a tuple key descent, an index walk or a join's inner
  walk on the read side; those are `sched.md` §3's and stay.
- No savepoint, and no re-run of a statement that wrote.
- No `O_DIRECT`, no change to the page reader's thread count.
- No change to recovery's or a checkpoint's reads (DZ owns the
  checkpoint's reactor time).

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-26

On `known-gaps-milestone-orders` from `8274c951`, on the operator's word.
No stage is opened and no code is written. **Not reviewed**: the
`critics-developer` review was started and stopped before it reported,
so no line reference or reading in this order has a second reader behind
it.
