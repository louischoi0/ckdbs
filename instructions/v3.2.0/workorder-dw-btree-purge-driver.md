# Work order DW - what a btree relation's deletes leave, purged unasked: index entries first, the row's own space on the operator's word

Status: **CLOSED 2026-11-27** at `f85aa3042` (`v3.4.17-185-gf85aa3042`),
the driver built behind `index_purge_driver`, off by default, for a
btree's deleted rows and a moved key on either kind; the row itself is
left to `PURGE` (DW-Q4 (a), DW-S3 with no subject). **Closed on CLA's
reading of the close measurement** (§7, "DW-S4"), adopted on the standing
go-ahead and not the operator's mark, **and not landed**: the merge to
`main` and the push wait for the operator's word. Opened 2026-11-27 on
worktree `dw-btree-purge-driver` (branch
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
| DW-Q7 | `updates` counts every `UPDATE`, and only one that moves a key or a covered value leaves an entry. Raised at DW-S2 | (a) the existing count, weighted by what the relation's passes found (halved to an eighth by a pass whose updates left nothing, judged only where no older view stood below its writes); (b) a new count on the write path of the updates that appended an index entry, which DW-Q2 (a) excludes | (a): DW-Q2 (a) stands, and (b) is proposed only if a close measures the learned weight's cost - up to three fruitless passes a relation, then one in eight - as a regression |

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
| DW-Q2 | (a), **amended by DW-Q7 (b) at the close** | existing counters only, and one new one: `deletes` as it stood, and `key_updates` |
| DW-Q3 | (a) | one boot key, off by default |
| DW-Q4 | (a) | the row is left to `PURGE`; DW-S3 has no subject. (c) needs the operator's own word and the standing word is not that |
| DW-Q5 | (a) | IX2c's first entry and spilled covered churn stay outside |
| DW-Q6 | (a) | `PURGE`'s polling wait stays outside |
| DW-Q7 | **(b), revised by CLA at the close from its own (a)** | raised by CLA at DW-S2 and adopted the same way: the item is not in the order as the operator read it. (a)'s proposal named its own trigger - "(b) is proposed only if a close measures the learned weight's cost as a regression" - and the first close measured it (§7, "DW-S4"): updates that move no key bought a fruitless pass every 2 s, 4.8 % of server CPU a statement for one index. **(b) adds one count, and so amends DW-Q2 (a)**, "existing counters only": `key_updates` is new, counted on the write path, and - DW-R3 - only where the driver is on |

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

### DW-S1: the driver for btree indexes (2026-11-27)

On `dw-btree-purge-driver`, over `05c32f378`. *Later: the signal below is
`deletes` alone; DW-S2 added `updates` and DW-S4's remedy replaced them
with `key_updates` (§7, "DW-S4"). The threshold, the switch, the step
and `RelationIndexPurge` stand as built here.*

**Built.**

- **`exec::RelationIndexPurge`** (`exec/index_purge.hpp`): every index
  of one relation walked once, one `PurgeIndexLeaves` attempt a step, its
  place an index's oid and a resume entry. It is the heap release's purge
  phase, factored out so the release and the driver run one walk
  (`HeapSlotRelease::Purge` is now a call of it). `IndexPurgeAttempt`
  gained `read`, the entries the walked leaves held. **Nothing the judge
  decides moved** (DW-R1): the one line added to `PurgeIndexLeaves` is
  that count.
- **`exec::IndexPurgeDriver`** (`exec/index_purge_driver.hpp`): the
  choice of what a pass runs on. The signal is `sys.relation_stats`'
  store's `deletes` (DW-Q2 (a)); a relation is queued once its deletes
  since it was last queued reach `max(4,096, kept / 5)`, `kept` being the
  entries an index held after the last pass, the indexes averaged.
  **DW-S1's signal is `deletes` alone**: this section's S0 log named
  `deletes + updates`, and `updates` is DW-S2's, with the moved key.
- **`CommandDispatcher::QueueIndexPurges` / `StepIndexPurge`**: the step
  is `StepHeapRelease`'s shape - the relation's `IX` by a read holder's
  id, try-only, a refusal skipping the step, the catalog memo made
  current under it, everything given back at the return.
- **The switch** (DW-Q3 (a)): `index_purge_driver`, a boot key, off by
  default. `Expeditor::Open` builds the driver on core 0's dispatcher
  only where it is on, and `Start` submits its two ticks (the offer once
  a second, a slice of 16 leaves every 50 ms) only where a driver exists.
  Off, there is no driver, no tick and no count read (DW-R3).
- **`sys.meta`'s `index_purge` section**, on the dispatcher that runs
  the driver: `index_purge_passes`, `_queued`, `_entries_read`,
  `_entries_removed`, `_rows_skipped`.
- **The simulator** runs the driver, floor 1, on seeds with
  `seed % 8 == 7` - half of those with a lock table - offered and stepped
  beside its reclaim step; every boot empties the relation statistics, as
  a server's restart does.

**DW-R2 as built: the driver paces, it does not register.** One slice a
50 ms tick, nothing held between two, so a relation `X` asked on any
core is granted in the gap; a slice that finds the relation held is
skipped. No `HolderProgress` registration is made: the driver never
holds across a wait, so no waiter's fault net has anything to ask it.

**Cells** (`tests/index_purge_driver_test.cpp`, 14; one each in
`config_file_test.cpp` and `expeditor_test.cpp`): a deleted row's
entries leave unasked and no probe moves; off, nothing is offered,
stepped or shown; the threshold, and that it follows one index's kept
entries however many indexes; DW-R5 on one core and across two (a view
keeps its rows' entries, and the next pass takes them); an undecided
deleter's rows skipped and counted; DW-R2 (an `X` granted between every
two slices, a held relation skipping the slice; a DDL on another core
between two slices, its index walked by the pass); an index dropped or
created mid-pass; an index still being built passed over (DD-R8); a
dropped relation and a heap; no lock table; a pass crashed durable and
not; and a serving instance whose tick purges only where the key is on.

**The review** (`critics-developer`, on the uncommitted tree): no
correctness defect; the heap release's refactor read as
behaviour-preserving, with two timing differences recorded (moving to
the next index, or passing one over, no longer costs an empty step; the
last index's walk ends the phase in its own step). Four mutations each
failed a named cell. **Applied**: the threshold mixed units - entries
summed over every index against deleted rows - and now reads one index's
kept entries; **the back-off CLA first built was removed** - under a
deletes-only signal a fruitless pass means an older view, not a useless
relation, and doubling the threshold on it left the dead entries growing
after the reader ended; the simulator emptied the statistics at `Create`
only, so a reboot ran a pass a server's restart would not, and the crash
cell pinned that artifact; one release guard for the three read-holder
scopes; the pacing explained in two places, not four; why no statement
epoch is owed, stated in the driver's header; and the cells the review
named - DW-R5 across cores, the skipped-rows count, the key's default
and parse, the serving instance. **Rejected**: a "never" flag for a
relation that is not a btree (DW-S2 walks heaps, and a relation with no
index can gain one; an empty pass is one memo lookup); the building
index as a two-core cell with a parked online build (it would step core
0's dispatcher from the cell's thread while its reactor runs - pinned on
the pass itself instead); folding the two tick steps' prologues into one
helper (it touches DD's statement run for about ten lines).

**Suite**: 5,315 of 5,315 passed on this tree, at `-j4`
(`ctest -LE bs-nightly`, 178 s). **Six earlier runs were not green and no
failure is the change's**: this host's `/tmp` is a `tmpfs` under a user
quota of 12.6 GB, another session's scratch held 5.0 GB of it, and at
`-j8` or `-j12` the crash rigs' copies ran three cells into `Disk quota
exceeded` (each passes alone); the other failures were the two-core
cells `known-gaps.md` ("Testing") lists -
`DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter`, 12 of 42 runs
alone on this tree against 5 of 30 at `60dc593af`, the two rates not
separated and no code the cell runs changed, and
`SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement`. A run
with the tests' directory moved to disk took 504 s and failed a cell
that times a sync; it was stopped and is no evidence either way.

**The simulator**: 108 of 108 runs passed on six seeds that draw the
driver (7, 15, 23, 31, 39, 47; `clean`, `sync-crash` and `crash`; faults
`none`, `io` and `grow`; `uniform` and `colliding`; 1,500 ops, two
iterations), in the Debug binary, before the count below was added and
again after it. The verdict line now carries
`unasked_purged_entries`: 130 to 380 a `clean` run on those seeds, so the
driver's passes took entries under the harness and did not merely run.
No committed seed of `tests/testdata/sim_seeds.txt` draws the driver
(none is 7 mod 8); the sweep above is by hand and no seed was added.

**Overhead not measured; measured at the milestone's close.**

### DW-S2: a moved key's entry, with no retired slot (2026-11-27)

On `dw-btree-purge-driver`, over `39b19ea77`. *Later: DW-S4's remedy
(`f85aa3042`) deleted the update weight, the horizon mark and
`TransactionManager::CommitCeiling` described below and counts
`key_updates` in place of `updates` (§7, "DW-S4"); a heap's walk, its
deletes buying no pass, and the release and a pass by turns stand.*

**Built.**

- **The driver counts `updates`.** `IndexPurgeDriver::Offer` takes a
  relation's `deletes` and `updates`, and the writes that buy a pass are
  `deletes + (updates >> update_shift)`. That is the whole of the
  "moved-key purge with no retired slot" (`known-gaps.md`, DR-Q18): the
  judge already called a moved key's old entry dead, and nothing ran it
  where no row was deleted.
- **A heap is walked.** The driver's pass runs on a heap as on a btree;
  only a dropped relation and a catalog chain end a pass unwalked. **A
  heap's deletes buy no pass** - its slot release purges for them - which
  the driver reads at the relation's first pass, a kind being fixed for
  the life of an oid; a first pass a heap's deletes bought is a pass the
  release would have run.
- **An update's weight is learned** (DW-Q7 (a)). `updates` is every
  `UPDATE`, and only one that moves a key or a covered value leaves an
  entry. A whole pass whose removals, beyond its deletes' entries, are
  under an eighth of the updates that helped buy it halves the weight of
  the relation's updates, to an eighth; a pass that did better makes it
  whole.
- **A pass an older view could have made fruitless judges nothing.** A
  relation is queued with the commit ceiling of that moment as its mark
  (`TransactionManager::CommitCeiling`, an accessor added), and its pass
  judges the updates only if the read horizon had reached the mark at
  its first step.

**The heap release and the driver on one relation** `[source-read]`,
confirmed by the review: both only remove entries, by the one judge,
each seeking its leaf again at every step, a step of each by turns on
core 0's tick and never at one moment; the release's verify reads what
either left and its free asks each slot again under its page's hold. The
judge keeps a declared Keystone's tombstone entry
(`src/exec/index_ddl.cpp:1117`), so a pass frees no key (K1, DW-R4).

**Cells** (`tests/index_purge_driver_test.cpp`, 23 in all): a btree and
a heap whose keys only move return to a rebuild's size but for a leaf's
first entry; a moved primary key's `$pkey_<oid>` entries go, and the
value it moved off is free to place; a heap's deletes buy no pass once
its kind is read; updates that move no key buy passes at a falling rate,
to the ceiling, until updates that do restore it; a delete's entries are
not counted as the updates' findings; a pass under an older view leaves
the weight, with a delete in it and with none; a heap's release and a
pass stepped by turns free no slot an entry names (every index walked
after every step); a declared Keystone's tombstone keeps its key through
a pass.

**The review** (`critics-developer`, on the uncommitted tree): DN-R2, K1
and every result hold; four mutations of the driver were run, three
killed by the cells as written and the fourth - the deletes' entries not
subtracted - by a cell the review added. **Applied by the review**: a
pass a refusal or a drop ended no longer moves the weight; three cells
(the fourth mutation's, the release and a pass by turns, the declared
Keystone's tombstone); the manual's `PURGE INDEX` text, which said a
heap purges its own moved keys. **Applied by CLA on its findings**: the
guard CLA first wrote - "the deletes were at least half answered" -
misread an older view that kept about half of a pass's writes, and any
older view where the pass held no delete, as updates that move nothing;
**it is replaced by the horizon mark above**, which reads the older view
itself, and the cell with no delete in the pass pins it. A pass that did
not walk every index leaves the threshold as well as the weight; the
writes that bought a pass are kept once, on the relation. **Raised and
adopted**: DW-Q7 (§4) - the review's other remedy, a count on the write
path of the updates that appended an entry, is option (b), outside
DW-Q2 (a). **Left as they are**: an index created or still building
mid-pass is counted among the indexes a delete's entries are spread
over, and `deletes` counts a statement a later `ROLLBACK` undoes - both
make a pass's updates look more useful, never less, so neither lowers a
weight wrongly.

**Suite**: 5,324 of 5,324 passed on this tree at `-j4`, on the second
run; the first failed `DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter`
alone (`known-gaps.md`, "Testing"). **The simulator**: 108 of 108 on the
six seeds of DW-S1's sweep, `unasked_purged_entries` 447 to 611 on three
`clean` runs.

**Overhead not measured; measured at the milestone's close.**

### DW-S3: no subject

DW-Q4 was adopted at (a): the row and its key are left to `PURGE`, as
built. Nothing was designed and nothing was built. (b) - a delete-marked
row's spills released unasked, the tombstone kept - stays CLA's next
proposal, and (c), the keyed tombstone, needs the operator's own word.

### DW-S4: the close (2026-11-27)

**The first close**, `[measured]` at `0c2092df8`
(`v3.4.17-183-g0c2092df8`) against the opening commit `630ecc359`,
Release, pinned, interleaved
(`bench/v3.5.0/results-dw-close-v3.4.17-183-g0c2092df8.md`, committed at
`95fd6a9e7`):

- **Off, the milestone costs nothing**: server CPU a statement at 0.984
  of the opening commit's over 39 cells (btree and heap, six shapes,
  three sizes), the A-against-A control at 1.0006. The 1.6 % on the
  cheaper side is on every shape and is not explained.
- **On, an index is held at its rows**: 57,515 to 99,800 entries for
  50,000 live rows where off it reached 2.6 to 5.3 million, a point read
  at 1.06 to 1.08 of a clean index's cost at one core (1.82 and 1.83 at
  two) against 26.0 to 29.5 and 4.9 times (48 and 54); at a cost of 9 % to
  13 % of server CPU a statement on the one-core churn cells, 5 % to 6 %
  at two cores.
- **And it wasted a pass every 2 s on updates that move no key**: 61
  passes in 120 s, 3.05 million entries read, none removed, 4.8 % of
  server CPU a statement for one index. DW-S2's learned weight halves a
  relation's updates to an eighth and no further, which at 46,000
  updates a second over 50,000 rows is a pass every 80,000 updates.

**The remedy** (`f85aa3042`): DW-Q7 revised from (a) to (b) (§4,
"Marks"). The driver's second signal is `key_updates`, the rows an
`UPDATE` wrote with a new entry appended to some index: counted on the
write path at the row (`MaintainIndexes`' `appended`), on every core's
dispatcher, and only where the key is on. The learned weight, its
horizon mark and `TransactionManager::CommitCeiling` are deleted. **The
review** (`critics-developer`, on the uncommitted tree) found the count
complete - every `UPDATE` path writes its row through one function - and
the record within the statistics' protocol under the leaf's hold;
**applied**: CLA's first form counted every row that moved what an index
carries, so a row flipping between two values was counted at every flip
for one dead entry, and the count now is the append itself; a row is
counted before a later index's refusal, not after; the clamp to the
floor is made once. **Left**: a spilled covered value changed to another
of the same length is counted and never purged (IX2a) - narrow, and in
`known-gaps.md`. **Cells** (`tests/index_purge_driver_test.cpp`, 24 in
all; one added in `expeditor_test.cpp`): the learned weight's three
cells are replaced by four - an update that moves nothing an index
carries buying no pass and a covered value's buying one, a flipping row
counted once an entry it leaves (with no pass between its flips), a
rolled-back update's entries counted and taken, an older view finding a
moved row through its old key - and every core counting only where the
driver is on. **Suite**, as the commit message of `f85aa3042` records
it: 5,326 of 5,326 at `-j4` on the sixth run, the five before failing
only the two-core cells `known-gaps.md` ("Testing") lists; the
simulator 108 of 108.

**The second close**, `[measured]` at `f85aa3042`
(`v3.4.17-185-gf85aa3042`)
(`bench/v3.5.0/results-dw-close-v3.4.17-185-gf85aa3042.md`):

- **Off against the opening commit**: 0.981 (control 1.0007), 39 cells,
  eight rounds. No regression.
- **On against off, a statement at a time, no pass running**: 0.999 over
  36 cells, the indexed-column `UPDATE` at 0.994 to 1.004 against a
  control of 0.996 to 1.004. The count on the write path is below the
  harness's 1 % to 2 %.
- **Updates that move no key: 0 passes** (61), server CPU on over off
  1.006 (1.048).
- **The churn cells as before**: on over off 1.102 (delete-heavy), 1.117
  (key-moving), 1.045 (delete-heavy, two cores); entries held at 59,001,
  64,366 and 88,856 against 2.8, 5.3 and 4.9 million; the point read at
  1.05, 1.21 and 1.50 of a clean index's against 28, 5.2 and 52 times
  (the probe's own floor in this run is about 1.4: an index that never
  held a dead entry read 1.33 and 1.40).
- **A row flipping between two values, a cell added here**: off, the
  index stands at 100,000 entries and a probe costs 1.06 of clean; on,
  the driver removed 2.47 million entries in 60 passes that the next
  flips appended again, for 7.2 % of server CPU a statement and nothing
  a read gains.
- **A slice costs its core**: statements over 1 ms rose 15 to 21 a pass
  with the key on, p99 not at all. The p99.9 rose by 12 us to 50 us, and
  neither close attributes it: the slices' statements are under 0.1 % of
  the total.

**CLA's reading, adopted on the standing go-ahead and not the operator's
mark.** DW closes: the default engine is not slower (off against the
opening commit, twice), the key costs a statement nothing measurable
until a pass runs, and where dead entries grow without bound the driver
holds them at the rows for about a tenth of a statement's server CPU.
**Two costs are read as the mechanism's price and not as a regression,
and an operator may read either the other way**: the flipping row's
7.2 % for no gain - the count cannot know an entry will be wanted back,
and with the key off that relation needs no purge - and a slice's 1 ms
to 5 ms on core 0, which is DW-R2's own bound and the heap release's
slice. Both are in `known-gaps.md` ("The unasked index purge (DW)"),
with what was not explained (the pass rate halving at two cores; stalls
of 0.29 s to 1.03 s in both arms). **Whether the key should be on by
default is not read here**: DW-Q3 (a) leaves it off.

**The host was shared** through both closes and the suites: another
user's `llama-server`, a SQL Server and an Oracle instance. Every
comparison is interleaved and carries its control; the absolute figures
are that host's.

**Budget**: the first close 29 min 53 s of measurement, the second
28 min 37 s, together 58 min 30 s against the milestone's hour, the two
Release builds' time in neither results file; no quiet-gate hold in
either. **Cut**, both: 120 s a mix,
one size (50,000 rows) in the mixes, one run a cell and arm in the
second, `relaxed` only, a client that bound every cell (the server 29 %
to 38 % busy), no heap in the mixes, no reader holding an old view.

**The durable content, moved**: `docs/spec/index.md` §2, §2.3 and §13;
`docs/spec/sys-views.md` §7; `docs/spec/heap-and-tuple.md` §3.1c;
`docs/inflight/known-gaps.md` ("The unasked index purge (DW)", and three
entries above it corrected); `CLAUDE.md`'s "Secondary indexes" row;
`kds.conf.sample`; `manual/server/server.md`, `manual/sql/sql.md`.

**The close's review** (`critics-developer`, on `6bad7d706`, the
milestone read as one change): no correctness defect in
`630ecc359..f85aa3042`, and nothing dead in the code but one include.
**Applied by the review, in documents only**: the p99.9 rise was blamed
on the slice against both results files, which leave its source
unidentified; the first close's figures were given for one core as if
for all; `known-gaps.md` lacked that a restart puts every threshold back
at the floor, that a relation is not offered while its own pass runs,
and what the second close could not resolve of throughput; an older
view's next pass is bought "by the same rule", not at the same price; a
sentence comparing the shape with PostgreSQL's autovacuum had nothing
behind it and is gone; `sys-views.md` says what `index_purge_queued`
does not count; the logs of DW-S1 and DW-S2 each got one line pointing
at what this stage replaced. **Applied by CLA on its findings**: three
comments that still described the count's first form, the dead include,
a cell for the mechanism the second close could only infer (a flip back
after a pass appends, and is counted, again), the candidate cause of the
two-core pass rate in `known-gaps.md`, and a correction note in the
second close's results file on its reading of `index_purge_queued`.
**Rejected**: letting a relation be offered while its own pass runs - a
change of behaviour that would want its own cell and measurement, after
the close measured the other.

**Suite at the close**: 5,327 of 5,327 at `-j4`, first run, on the tree
this log is committed with. **That tree is not byte for byte the one
measured**: after `f85aa3042` came comments, one removed include and one
test cell, and no statement of engine code.

**The merge of `origin/main`**: nothing to merge. `origin/main` stood at
`630ecc359`, the commit DW opened from, at every fetch through the
close.
