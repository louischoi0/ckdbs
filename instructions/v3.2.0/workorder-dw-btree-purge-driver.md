# Work order DW - what a btree relation's deletes leave, purged unasked: index entries first, the row's own space on the operator's word

Status: **OPENED 2026-11-27** on worktree `dw-btree-purge-driver` (branch
`worktree-dw-btree-purge-driver`) from `630ecc359`
(`v3.4.17-178-g630ecc359`), under the operator's standing go-ahead of
2026-11-27 (`raft-marks-2026-11-27.md` §1): every DW-Q item is **adopted
on that go-ahead, never marked by the operator item by item** (§4,
"Marks"). Written 2026-11-26 on worktree `known-gaps-milestone-orders`
(branch `worktree-known-gaps-milestone-orders`) from `8274c951`
(`v3.4.17-148-g8274c951e`) on the operator's word of 2026-11-26
(`raft-marks-2026-11-26.md` §3). A `[source-read]` claim
with a `path:line` was read at `8274c951`; a claim taken from
`known-gaps.md` carries that entry's own verification commit and is not
re-verified here.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `docs/rules/keystoneid-invariant.md` K1;
`docs/spec/heap-and-tuple.md` §3.1b, §4.1, §4.1c; `docs/spec/index.md`
§2, §2.2, IX2c, IX4b; `docs/inflight/known-gaps.md`, "Keystone ids",
"Index entry purge (DD)", "A primary key over the Keystone (DR)";
`instructions/v3.2.0/workorder-dd-index-entry-purge.md` (DD-Q4 (a));
`instructions/v3.0.0/workorder-bh-purge-key.md`.

## 0. What DW is

`[source-read]` A heap's slot release purges its indexes unasked
(`include/kds/exec/heap_release.hpp:82`, `CLAUDE.md`, "Heap relations").
On a btree relation nothing does: `PURGE INDEX` runs when an
administrator runs it, and a delete-marked row stays in its leaf until an
operator's `PURGE` (`known-gaps.md:3549`, `:804`). `[design]` DW gives a
btree relation an unasked driver for the part that touches no identity -
its index entries - and puts the part that does touch one, the row and
its key, before the operator as a question it does not answer itself.

**K1 is not moved by DW's default scope**: a Keystone id stays bound to
its delete-marked row until `PURGE` (Hard Invariant 11).

## 1. The tree as read

`[source-read]` from `docs/inflight/known-gaps.md` at `8274c951`:

- **Nothing drives `PURGE INDEX` on a btree** (`:3549`, DD-Q4 (a));
  **a btree's indexed key keeps its dead entries until a statement
  purges them** (`:3725`).
- **A moved key's old entry is purged only beside a retired slot**
  (`:3647`, DR-Q18): a relation whose keys move and whose rows are never
  deleted keeps every old entry. The remedy named and not built: the
  release's index purge run with no retired slot.
- **An engine-internal purge of a user relation owes a keyed tombstone,
  and none is built** (`:804`, verified at `26766699`): a keyed,
  never-visible tombstone in the row's key position, which the duplicate
  check and the placement order read and every walk treats as absent -
  or it reclaims nothing.
- **What bounds `PURGE`** (`:818`): one older snapshot blocks it; its
  wait polls; the leaf space comes back only when the leaf fills; it
  removes no secondary-index entry.
- **A leaf's first entry is never removed** (`:3559`, IX2c); **spilled
  covered churn under one key is never purged** (`:3563`); **a DDL on
  another core is not queued for by the purge** (`:3565`); **a purge of
  a row `PURGE`'s window is about eight times dearer an entry**
  (`:3554`, `[measured]`
  `bench/v3.4.17/dd-close-v3.4.17-3-gb8880f9b.md`).
- **`PURGE`'s horizon premise is `[quiet-wrong]`** (`:3536`): every view
  that can see a superseded row must stay registered with the horizon.

`[source-read]` in the code at `8274c951`: `PURGE INDEX` is a sliced run
of 16 leaves (`src/server/command_dispatcher.cpp:3825`) that asks each
row's tuple `U` try-only (`src/exec/index_purge.cpp:65`); the heap's
release takes its relation unit at
`src/server/command_dispatcher.cpp:9475`; the catalog's own purge is
`Catalog::RetireDeleteMarks` (`src/catalog/catalog.cpp:1163`), catalog
chains only.

**Not measured**: how fast a btree's indexes grow dead entries under an
OLTP mix, and what a read pays for them.

## 2. The mechanism `[design]`

1. **The index driver.** Core 0's tick, which drives a heap's slot
   release, also schedules `PURGE INDEX`'s own run over a btree
   relation's indexes: the same judge, the same slices, the same `IX`,
   nothing held across a yield. What it adds is the choice of what to
   run and when.
2. **What it runs on.** A relation is a candidate by a signal that
   already exists - `sys.relation_stats`' deletes and key-moving updates
   since its last run, `est_delete_marked` where the steward has it -
   never by a new scan (DW-Q2).
3. **A moved key's entry.** The same driver runs a heap's index purge
   with no retired slot (`:3647`'s remedy), so one mechanism covers both
   kinds.
4. **The row.** Three forms are before the operator (DW-Q4): leave it to
   `PURGE`; release a delete-marked row's spilled values once every
   reader is past its deleter, the tombstone and its key staying; or the
   keyed tombstone of `:804`. Only the first is in DW unless marked.

## 3. Rules

- **DW-R1. An entry is removed only if a rebuild would not append it**,
  by `PURGE INDEX`'s own judge, unchanged `[quiet-wrong]`. DW changes
  who calls it, never what it decides.
- **DW-R2. The driver holds nothing across a yield and never makes a
  statement wait longer than one slice**; a relation `X` waiter is seen
  between slices, and the driver backs off (it registers with
  `HolderProgress` or paces, DW-Q3).
- **DW-R3. A switched-off driver does nothing and collects nothing**,
  and with it off the engine is the engine at `8274c951`.
- **DW-R4. K1 holds**: no Keystone id is freed by anything but `PURGE`,
  unless DW-Q4 is marked otherwise by the operator's own word.
- **DW-R5. The horizon premise is restated and tested for the driver**:
  it reads `ResolvedForEveryReader` as the statement does, and a cell
  holds an old view across a run.
- The standing rules, as DT's order lists them.

## 4. Items for the operator (DW-Q), CLA's proposals

| | item | options | CLA proposes |
|---|---|---|---|
| DW-Q0 | Is DW opened, and with what scope? | (a) the index driver for btree relations and the moved-key purge for both kinds; (b) and the row, as DW-Q4 marks | (a) |
| DW-Q1 | Does DW-S0 gate? | (a) S0 measures dead-entry growth and its read cost on a delete-heavy and a key-moving mix; if a point read by index pays under 5 % at steady state, DW stops and says so. (b) built regardless | (a) |
| DW-Q2 | The driver's signal | (a) existing counters only; (b) a per-index dead-entry estimate, a new sampled word | (a) |
| DW-Q3 | The switch | (a) one boot key, off by default until a close measures it on; (b) on by default; (c) per relation | (a) |
| DW-Q4 | The row and its key | (a) left to `PURGE`, as built; (b) a delete-marked row's spills released unasked, the tombstone kept - no key freed, a var-heap release at `kNoTxnId` as `PURGE`'s is; (c) the keyed tombstone, K1's second exception or a new slot state | (a); (b) is CLA's next proposal and (c) needs the operator's own word |
| DW-Q5 | IX2c's first entry and spilled covered churn | (a) outside DW, stated; (b) inside | (a): DM, which would have lifted IX2c, was abandoned |
| DW-Q6 | `PURGE`'s polling wait | (a) outside DW (EE's decide slot would give it a wake); (b) inside | (a) |

### Marks

**Every row is CLA's proposal adopted on the operator's standing go-ahead
of 2026-11-27 (go-ahead-achieving-milestone)** - *"go ahead until
achieving milestone, I will follow CLA proposal if decision needed"*
(`raft-marks-2026-11-27.md` §1) - and none is the operator's own mark on
the item. No item is tagged `[quiet-wrong]`.

| | adopted | note |
|---|---|---|
| DW-Q0 | (a) | the index driver for btree relations and the moved-key purge for both kinds |
| DW-Q1 | (a) | DW-S0 gates; the bar is read in §7 |
| DW-Q2 | (a) | existing counters only |
| DW-Q3 | (a) | one boot key, off by default |
| DW-Q4 | (a) | the row is left to `PURGE`; DW-S3 has no subject. (c) needs the operator's own word and the standing word is not that |
| DW-Q5 | (a) | IX2c's first entry and spilled covered churn stay outside |
| DW-Q6 | (a) | `PURGE`'s polling wait stays outside |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DW-S0** | The premise: dead entries per index over a delete-heavy and a key-moving mix, 10 minutes each, and the read cost at the end against a rebuilt index. The census of the heap release's scheduling, for reuse | a results file; DW-Q1's bar read |
| **DW-S1** | The driver for btree indexes, behind its switch | cells: entries leave unasked; DW-R2 beside a DDL on another core; DW-R5; the sim drawing it |
| **DW-S2** | The moved-key purge with no retired slot, btree and heap | cells: a relation whose keys only move returns to its rebuilt size |
| **DW-S3** | DW-Q4 as marked, if marked beyond (a) | its own design reviewed before code |
| **DW-S4** | The close: the driver's cost on a foreground mix, on against off, interleaved; `index.md` §2.2, `known-gaps.md` | the operator's word on landing |

## 6. What DW does not do

- No reissue of a Keystone id, no row moved, no leaf merged.
- No change to `PURGE`'s or `PURGE INDEX`'s statement, judge or record
  kinds.
- No vacuum of a version chain: undo is the transaction manager's.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-26

On `known-gaps-milestone-orders` from `8274c951`, on the operator's word.
No stage is opened and no code is written. **Not reviewed**: the
`critics-developer` review was started and stopped before it reported,
so no line reference or reading in this order has a second reader behind
it.

### DW-S0: the premise (2026-11-27)

On `dw-btree-purge-driver` at `630ecc359` (`v3.4.17-178-g630ecc359`), a
Release binary of that commit, `[measured]`
`bench/v3.5.0/results-dw-s0-premise-v3.4.17-178-g630ecc359.md`. Four
mixes of 480 s each over three twin btree relations of 50,000 live rows
with a plain index on `k`, nothing purging: a delete-heavy and a
key-moving mix, each with key values that recur and with key values
never used again.

**What it measured.**

- **Dead entries grow one per `DELETE` or key-moving `UPDATE`, and there
  is no steady state**: 3.1 to 6.1 million entries for 50,000 live rows
  after 480 s, in every cell.
- **A point read by the index pays where its key's run holds the dead
  entries, and nowhere else.** Recurring keys, **at 480 s and still
  rising**: 32.5 times the purged twin's server CPU in the delete-heavy
  cell (470 us against 14.5 us, 617 dead entries in the run) and 4.9
  times the rebuilt twin's in the key-moving one (39.9 us against 8.1 us,
  111 in the run); 739 ns and 286 ns a dead entry. The figures are a
  point on a line, set by how long the mix ran and how many rows share a
  key value, not a property of the shape. Fresh keys: 1.002 and 0.967, and 1.045 against a
  purged twin in the key-moving cell, which the data do not settle
  against 5 % (the twins differ from each other by more).
- **`REINDEX INDEX` takes out no deleted row's entry** (the count stayed
  at 3,134,200); `PURGE INDEX` took out 98.0 % to 98.8 % of each
  purged index's entries in 1.6 s to 4.4 s of one core, 0.5 us to 1.1 us a
  removed entry, no row skipped.
- `sys.relation_stats`' `deletes` and `updates` were exact;
  `est_delete_marked` was NULL on every twin, the steward never having
  sampled a leaf of a relation no planned join named.

**DW-Q1's bar, read by CLA** (adopted (a), §4 "Marks"): *"if a point
read by index pays under 5 % at steady state, DW stops and says so."*
There is no steady state to read it at - the dead entries grow for as
long as the mix runs - so the bar is read at the end of each mix, and by
shape. **Where key values recur a point read was over the bar at the
first sample, 30 s into the mix** (+96 % delete-heavy, +24 % key-moving,
observed) **and 32.5 and 4.9 times at 480 s; where they do not it pays
nothing.** A derived figure, not observed, puts the crossing at about
one dead entry a key run (0.75 s and 6.1 s into these mixes). The bar is
not met: DW does not stop, and DW-S1 opens. **The reading is CLA's
`[design]`, on the standing go-ahead, and it holds for two of the four
shapes**: what it rests on is the premise that a key whose values recur -
a status, an owner, a foreign key - is an ordinary OLTP index. An
operator who does not hold that premise reads the same data as "stop".

**What S0 hands the driver's design.**

- The signal DW-Q2 (a) leaves is `deletes + updates`: `est_delete_marked`
  is absent wherever no join asked for a prior, and `sys.indexes`'
  `entries` is a walk. `updates` counts every row an `UPDATE` writes, one
  that moves no key included (`src/stats/relation_stats.cpp:16`, by
  statement kind; M-rec's count holds its 101 same-value moves), so the signal is a superset and the driver must not keep
  walking a relation whose passes remove nothing.
- An idle-instance purge at the measured growth is under 1 % of a core;
  what a slice costs a statement on core 0 is DW-S4's to measure.

**The census of the heap release's scheduling**, `[source-read]` at
`630ecc359`, for reuse:

- **The queue** is `exec::HeapSpace`'s dead list (`NoteDead` /
  `TakeDead`, `src/exec/heap_store.cpp:49`), one for the instance, a
  relation in it once however often it is noted: a heap `DELETE` queues
  its relation at every row it marks
  (`src/server/command_dispatcher.cpp:18356`), as do an insert whose
  unlogged placement is taken back by retiring its slot (`:9367`), a
  rolled-back heap insert (the manager's observer,
  `include/kds/server/command_dispatcher.hpp:2247`), a row `PURGE`
  (`src/server/command_dispatcher.cpp:17906`), a job that ended owing a
  slot (`src/exec/heap_release.cpp:84`), a failed step
  (`src/server/command_dispatcher.cpp:9806`) and a mount, every heap
  once (`src/server/expeditor.cpp:1418`). Nothing queues a btree, and
  nothing queues a heap for an `UPDATE`.
- **The tick** is core 0's `system` group, every 50 ms
  (`src/server/expeditor.cpp:2303`): one `StepHeapRelease` of
  `PagesPerSlice(Priority::kAnalytic)` (16) pages or leaves, a predicate
  and a return when nothing is queued. The simulator steps it beside its
  reclaim (`sim/instance.cpp:213`).
- **A step** (`src/server/command_dispatcher.cpp:9779`) takes the
  relation's `IX` under a read holder's id, try-only - a refusal skips
  the step - makes the catalog memo current under it
  (`src/exec/heap_release.cpp:59`), runs one phase's bounded work under
  `DrainOnPressure`, and gives everything back. Nothing is held between
  two steps, so a relation `X` asked on any core is granted in the gap,
  which is 50 ms less one slice.
- **Its purge phase** (`src/exec/heap_release.cpp:176`) is the part DW
  reuses: every index of the relation in oid order, each walked once
  left to right by `PurgeIndexLeaves` with its resume entry, a building
  or dropped index passed over.

**Not explained**: `sys.meta`'s `pages_allocated` read 2,753 lower after
the delete-heavy cell's `PURGE INDEX` (the results file, section 6).
Outside DW; not followed.

**Cut**, to the stage's 20 minutes: 480 s a mix, not 10 minutes; one run
a cell; one size; `cores = 1`, `relaxed`; no reader holding an old view
(DW-R5 is a test's, DW-S1). The quiet gate held the run about 1.5 min
for this session's own Debug build, outside the budget. **Suite not
executed: no engine code changed.**

**The review of DW-S0** (`critics-developer`, on `acdf8b6a6`): no defect
that makes a number wrong - the twins' history, the liveness of the read
keys, the schedstat windows and the 5 % arithmetic were each re-derived,
and the report script reproduces `report.txt` byte for byte from the
archive. **Applied**: six corrections in the results file (the key-moving
cell's 6,341 moves that made no entry are 101 same-value moves and moves
back to a value the row held; the fresh cells' probe series is not flat
at one sample; the control column mixed two definitions; the recheck's
median ratio is 1.01; `updates` does count an `UPDATE` that moves no
value; the census is in this section) and three in this log (a heap
`DELETE` queues at each row it marks, `src/server/command_dispatcher.cpp:18356`,
not at `:9367`, which is an insert taken back; a failed step and a mount
queue too; the purge figures are of the purged twin). The bar's reading
was reworded on the review's finding: it rests on the observed 30 s
samples, the multipliers are quoted at 480 s, and the premise it depends
on is named. **Rejected**: asserting a row count on the mix's writes and
the five cuts proposed in the two tools - a driver changed after its run
is a driver that measured nothing, and the as-run copy is the archive's;
the results file now says what the writes' check does not cover.
